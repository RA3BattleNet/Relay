#include <boost/asio.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <spdlog/cfg/env.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/spdlog.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <random>
#include <ranges>
#include <source_location>

namespace a = boost::asio;
namespace h = httplib;
namespace j = nlohmann;
namespace l = spdlog;
using namespace std::literals;
using namespace a::experimental::awaitable_operators;
using Tcp = a::ip::tcp;
using Udp = a::ip::udp;
using SteadyTimer = a::use_awaitable_t<>::as_default_on_t<a::steady_timer>;
using UdpSocket = a::use_awaitable_t<>::as_default_on_t<Udp::socket>;

// 把服务器的内网地址转换为外网地址
class RelayServerNat
{
private:
    mutable std::mutex m_mutex;
    a::ip::address_v4 m_public_ip;

private:
    RelayServerNat() = default;
    void update_ip();

public:
    static RelayServerNat& instance();
    a::ip::address_v4 public_ip() const;
    void translate(Udp::endpoint& local_endpoint);
};

class NatnegPlusConnection
{
public:
    struct NatnegRequest
    {
        std::map<Udp::endpoint, std::uint16_t> players_and_local_ports;
        std::map<Udp::endpoint, std::string> cached_responses;
        bool alive = true;
    };
    struct Route
    {
        inline static constexpr std::uint8_t debug_limit = 8;

        Udp::endpoint endpoint;
        std::uint16_t linked = 0;
        std::uint8_t debug_counter = debug_limit + 1;
        bool watchdog_alive_flag = false;
        bool valid = false;
    };
private:
    inline static constexpr std::size_t router_map_size = std::numeric_limits<std::uint16_t>::max() + 1;
    std::array<Route, router_map_size> m_router_map; // 标记每个token应该转发到哪里，以及watchdog_alive_flag flag
    std::map<std::uint32_t, NatnegRequest> m_natneg_map;  // 每个natneg session的缓存
    std::string m_control_log_id;

private:
    bool store_natneg_map(std::uint32_t id, Udp::endpoint endpoint, std::uint16_t local_port);
public:
    a::awaitable<void> start_control();
    a::awaitable<void> process_control(Udp::socket& control_socket);
    a::awaitable<void> start_relay();
    a::awaitable<void> natneg_request_watchdog();
    a::awaitable<void> watchdog();
};

struct Request
{
    std::size_t sequence_number;
    std::uint32_t session_id;
    std::uint16_t local_port;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE
    (
        Request,
        sequence_number,
        session_id,
        local_port
    );
};

struct Response
{
    inline static std::string_view constexpr header =
        "MaribelHearnUsamiRenko"sv;
    std::size_t sequence_number;
    std::string peer_ip;
    std::uint16_t peer_port;
    std::uint16_t peer_local_port;
    std::uint16_t relay_token;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE
    (
        Response,
        sequence_number,
        peer_ip,
        peer_port,
        peer_local_port,
        relay_token
    );
};

struct EndPointFormatter : fmt::formatter<std::string>
{
    template <typename FormatContext>
    auto format(Udp::endpoint const& input, FormatContext& ctx) const -> decltype(ctx.out());
};

template<>
struct fmt::formatter<Udp::endpoint> : EndPointFormatter {};

std::minstd_rand rng{ std::random_device{}() };
std::uniform_int_distribution<std::uint16_t> distribute;

int main()
{
    try
    {
        // l::cfg::load_env_levels();
        l::sink_ptr default_sink = std::make_shared<l::sinks::rotating_file_sink_mt>("logs/relay.txt", 1048576 * 5, 3);
        l::sink_ptr error_sink = std::make_shared<l::sinks::rotating_file_sink_mt>("logs/relay_errors.txt", 1048576 * 5, 3);
        default_sink->set_level(l::level::trace);
        error_sink->set_level(l::level::err);
        l::set_default_logger(std::make_shared<l::logger>("main", std::initializer_list{ default_sink, error_sink }));
        l::flush_every(60s);
        l::info("public ip is {}", RelayServerNat::instance().public_ip().to_string());
        a::io_context context;
        l::info("starting relay server...");

        a::io_context natneg_plus_context;
        NatnegPlusConnection natneg_plus_connection;
        auto natneg_plus_control_task = a::co_spawn
        (
            natneg_plus_context,
            [&c = natneg_plus_connection] { return c.start_control(); },
            a::use_future
        );
        auto natneg_plus_relay_task = a::co_spawn
        (
            natneg_plus_context,
            [&c = natneg_plus_connection] { return c.start_relay(); },
            a::use_future
        );
        auto natneg_plus_request_watchdog = a::co_spawn
        (
            natneg_plus_context,
            [&c = natneg_plus_connection] { return c.natneg_request_watchdog(); },
            a::use_future
        );
        auto natneg_plus_relay_watchdog = a::co_spawn
        (
            natneg_plus_context,
            [&c = natneg_plus_connection] { return c.watchdog(); },
            a::use_future
        );
        try
        {
            natneg_plus_context.run();
        }
        catch (std::exception const& e)
        {
            l::critical("natneg_plus_context is terminating because: {}", e.what());
        }
        l::critical("natneg_plus_context finished");
        natneg_plus_control_task.get();
        natneg_plus_relay_task.get();
        natneg_plus_request_watchdog.get();
        natneg_plus_relay_watchdog.get();
    }
    catch (std::exception const& e)
    {
        l::critical("Application is terminating because: {}", e.what());
        return 1;
    }
    return 0;
}

void RelayServerNat::update_ip()
{
    h::Client client{ "http://api.ipify.org" };
    if (h::Result result = client.Get("/"); result)
    {
        l::info("obtaining public ip, response = {}", result.value().body);
        auto new_ip = a::ip::address_v4::from_string(result.value().body);
        l::info("parsed public ip: {}", new_ip.to_string());
        std::scoped_lock lock{ m_mutex };
        m_public_ip = new_ip;
    }
    else
    {
        l::error("failed to obtain public ip: {}", h::to_string(result.error()));
    }
}

RelayServerNat& RelayServerNat::instance()
{
    static auto pointer = ([]
    {
        static auto self = RelayServerNat{};
    self.update_ip();
    auto runner = []
    {
        while (true)
        {
            std::this_thread::sleep_for(30min);
            try
            {
                self.update_ip();
            }
            catch (std::exception const& e)
            {
                l::error("error when updating public ip: {}", e.what());
            }
        }
    };
    std::thread{ runner }.detach();
    return &self;
    })();
    return *pointer;
}

a::ip::address_v4 RelayServerNat::public_ip() const
{
    std::scoped_lock lock{ m_mutex };
    return m_public_ip;
}

void RelayServerNat::translate(Udp::endpoint& local_endpoint)
{
    std::scoped_lock lock{ m_mutex };
    local_endpoint.address(m_public_ip);
}

template <typename FormatContext>
auto EndPointFormatter::format(Udp::endpoint const& input, FormatContext& ctx) const -> decltype(ctx.out())
{
    return fmt::format_to(ctx.out(), "{}:{}", input.address().to_string(), input.port());
}

bool NatnegPlusConnection::store_natneg_map(std::uint32_t id, Udp::endpoint endpoint, std::uint16_t local_port)
{
    auto found = m_natneg_map.find(id);
    if (found == m_natneg_map.end())
    {
        // insert new endpoint into new vector
        m_natneg_map.try_emplace
        (
            id,
            std::map<Udp::endpoint, std::uint16_t>{ { endpoint, local_port } }
        );
        return false;
    }
    auto& [key, natneg_request] = *found;
    if (natneg_request.players_and_local_ports.contains(endpoint))
    {
        // current endpoint already exists, skip it
        return natneg_request.players_and_local_ports.size() == 2;
    }
    if (natneg_request.players_and_local_ports.size() >= 2)
    {
        // Received more than two INIT, impossibe, clean it.
        auto get_key = &decltype(natneg_request.players_and_local_ports)::value_type::first;
        l::warn
        (
            "NATNEG+ session {:08X} detected more than two INIT: existing from {}, new from {}",
            id, fmt::join(natneg_request.players_and_local_ports | std::views::transform(get_key), ", "), endpoint
        );
        natneg_request.players_and_local_ports.clear();
        natneg_request.players_and_local_ports[endpoint] = local_port;
        return false;
    }
    // insert new endpoint into existing vector
    natneg_request.players_and_local_ports[endpoint] = local_port;
    return true; // implies that we have two endpoints now
}

a::awaitable<void> NatnegPlusConnection::start_control()
{
    //std::scoped_lock lock{ NatnegPlusConnection::m_natneg_map };
    Udp::socket control_socket = { co_await a::this_coro::executor, { a::ip::address_v4::any(), 10189 } };
    m_control_log_id = fmt::format("NATNEG+ control @{}", control_socket.local_endpoint().port());
    while (true)
    {
        try
        {
            co_await process_control(control_socket);
        }
        catch (std::exception const& e)
        {
            l::error("{} encountered error because: {}", m_control_log_id, e.what());
        }
    }
}

a::awaitable<void> NatnegPlusConnection::process_control(Udp::socket& control_socket)
{
    std::array<char, 128> control_data = {};
    Udp::endpoint endpoint;
    auto received_length = co_await control_socket.async_receive_from
    (
        boost::asio::buffer(control_data),
        endpoint,
        a::use_awaitable
    );
    std::string_view received{ control_data.data(), received_length };
    j::json json = j::json::parse(received, nullptr, false);
    if (not json.is_object() or json.is_discarded())
    {
        l::error("{} received invalid length {} bytes from {}", m_control_log_id, received_length, endpoint);
        co_return;
    }
    Request request{};
    try
    {
        json.get_to(request);
    }
    catch (std::exception const& e)
    {
        l::error("{} received invalid JSON from {}: {}: {}", m_control_log_id, endpoint, e.what(), received);
        co_return;
    }
    // Parse input
    auto [sequence_number, session_id, local_port] = request;
    l::info("{} processing request of {:08X} from {}...", m_control_log_id, session_id, endpoint);
    if (not store_natneg_map(session_id, endpoint, local_port))
    {
        // not ready yet
        l::info("{} session {:08X} not ready yet", m_control_log_id, session_id);
        co_return;
    }
    auto& [players_and_local_ports, cached_responses, alive] = m_natneg_map[session_id];
    if (cached_responses.size() != 2)
    {
        l::info
        (
            "{} see request of {:08X}/{} is ready, creating connection...",
            m_control_log_id,
            session_id,
            sequence_number
        );

        // Create relay here.
        auto& [player_1, local_port_1] = *players_and_local_ports.begin();
        auto& [player_2, local_port_2] = *players_and_local_ports.rbegin();
        std::uint16_t token_1 = distribute(rng);
        while (m_router_map[token_1].valid)
        {
            ++token_1;
        }
        std::uint16_t token_2 = distribute(rng);
        while (m_router_map[token_2].valid or token_2 == token_1)
        {
            ++token_2;
        }
        m_router_map[token_1] =
        {
            .endpoint = player_2,
            .linked = token_2,
            .watchdog_alive_flag = true,
            .valid = true,
        };
        m_router_map[token_2] =
        {
            .endpoint = player_1,
            .linked = token_1,
            .watchdog_alive_flag = true,
            .valid = true
        };
        if (spdlog::get_level() <= spdlog::level::debug)
        {
            m_router_map[token_1].debug_counter = 0;
            m_router_map[token_2].debug_counter = 0;
        }

        l::info
        (
            "{} create following info: player_1 [endpoint {}, token {}, linked {}], player_2 [endpoint {}, token {}, linked {}]",
            m_control_log_id,
            player_1,
            token_1,
            m_router_map[token_1].linked,
            player_2,
            token_2,
            m_router_map[token_2].linked
        );

        Response response_1
        {
            .sequence_number = sequence_number,
            .peer_ip = player_2.address().to_string(),
            .peer_port = player_2.port(),
            .peer_local_port = local_port_2,
            .relay_token = token_1,
        };
        Response response_2
        {
            .sequence_number = sequence_number,
            .peer_ip = player_1.address().to_string(),
            .peer_port = player_1.port(),
            .peer_local_port = local_port_1,
            .relay_token = token_2,
        };
        cached_responses =
        {
            { player_1, std::string{ Response::header } + j::json(response_1).dump() },
            { player_2, std::string{ Response::header } + j::json(response_2).dump() },
        };
    }
    else
    {
        l::info
        (
            "{} see request of {:08X}/{} was already ready, sending cached response...",
            m_control_log_id,
            session_id,
            sequence_number
        );
    }

    auto& [player_1, response_1] = *cached_responses.begin();
    auto& [player_2, response_2] = *cached_responses.rbegin();
    auto send_to_player_1 = control_socket.async_send_to
    (
        boost::asio::buffer(response_1),
        player_1,
        a::use_awaitable
    );
    auto send_to_player_2 = control_socket.async_send_to
    (
        boost::asio::buffer(response_2),
        player_2,
        a::use_awaitable
    );
    co_await(std::move(send_to_player_1) and std::move(send_to_player_2));
    l::info
    (
        "{} successfully sent response for {:08X}/{}",
        m_control_log_id,
        session_id,
        sequence_number
    );
}

a::awaitable<void> NatnegPlusConnection::start_relay()
{
    Udp::endpoint bind_address = { a::ip::address_v4::any(), 10190 };
    Udp::socket relay_socket = { co_await a::this_coro::executor, bind_address };
    l::info("NATNEG+ relay starting...");
    std::byte relay_data[2048] = {};
    while (true)
    {
        try
        {
            Udp::endpoint endpoint;
            auto received_length = co_await relay_socket.async_receive_from
            (
                boost::asio::buffer(relay_data),
                endpoint,
                a::use_awaitable
            );
            if (received_length < 2)
            {
                l::error("NATNEG+ relay received a tiny packet (< 2 bytes), ignore");
                continue;
            }
            // Parse to get token
            std::uint16_t token;
            std::memcpy(&token, relay_data, 2);
            // Obtain endpoint, set watchdog_alive_flag flag, then send.
            auto& target = m_router_map[token];
            auto& linked = m_router_map[target.linked];
            if (not (target.valid and linked.valid))
            {
                l::error("NATNEG+ relay: invalid token={}, linked={}", token, target.linked);
                continue;
            }
            target.watchdog_alive_flag = true;
            linked.watchdog_alive_flag = true;
            linked.endpoint = endpoint;
            if (target.debug_counter < 8)
            {
                ++target.debug_counter;
                l::debug
                (
                    "NATNEG+ relay: token {} (linked {}) received from {}, sending to {}",
                    token,
                    target.linked,
                    endpoint,
                    target.endpoint
                );
            }
            co_await relay_socket.async_send_to
            (
                a::buffer(relay_data + 2, received_length - 2),
                target.endpoint,
                a::use_awaitable
            );
        }
        catch (std::exception const& e)
        {
            l::error("NATNEG+ relay: catched exception: {}", e.what());
        }
    }
}

a::awaitable<void> NatnegPlusConnection::natneg_request_watchdog()
{
    // Clean dead requests.
    SteadyTimer timer{ co_await a::this_coro::executor };
    while (true)
    {
        std::erase_if(m_natneg_map, [](std::pair<std::uint32_t, NatnegRequest> const& element)
        {
            auto& [key, request] = element;
        l::info("NATNEG+ watchdog clearing dead request {:08X}", key);
        return not request.alive;
        });
        // marking current request as dead so they will be clean after 30 seconds
        for (auto& [key, request] : m_natneg_map)
        {
            l::info("NATNEG+ watchdog marking request {:08X}", key);
            request.alive = false;
        }

        timer.expires_after(30s);
        co_await timer.async_wait();
    };
}

a::awaitable<void> NatnegPlusConnection::watchdog()
{
    // Clean dead connection.
    SteadyTimer timer{ co_await a::this_coro::executor };
    while (true)
    {
        for (Route& route : m_router_map)
        {
            if (not route.valid)
            {
                continue;
            }

            if (route.watchdog_alive_flag)
            {
                route.watchdog_alive_flag = false;
                continue;
            }

            auto token = &route - m_router_map.data();
            l::info("NATNEG+ watchdog clearing dead connection token={}, linked={}", token, route.linked);
            route = { .valid = false };
        }
        timer.expires_after(300s);
        co_await timer.async_wait();
    };
    co_return;
}
