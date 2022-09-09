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

a::awaitable<void> run_echo_server();
a::awaitable<void> run_control_server();
a::awaitable<void> process_control_server(Tcp::iostream connection);
j::json get_status();
a::awaitable<j::json> spawn_relay_server(j::json request);

struct Response
{
    bool succeeded = false;
    std::string message;
    Udp::endpoint endpoint_1;
    Udp::endpoint endpoint_2;

    j::json to_json() const;
};

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

struct ConnectionStatusReport
{
    std::uintmax_t processed_count;
    std::chrono::nanoseconds avg_time_before_send;
    std::chrono::nanoseconds avg_time_after_send;
    std::chrono::nanoseconds max_time_before_send;
    std::chrono::nanoseconds max_time_after_send;
};

void to_json(j::json& j, const ConnectionStatusReport& report)
{
    j = j::json
    {
        { "ProcessedCount", report.processed_count},
        { "AverageTimeBeforeSend", report.avg_time_before_send.count()},
        { "AverageTimeAfterSend", report.avg_time_after_send.count()},
        { "MaxTimeBeforeSend", report.max_time_before_send.count()},
        { "MaxTimeAfterSend", report.max_time_after_send.count()}
    };
}

class Connection
{
public:
    struct Formatter : fmt::formatter<std::string>
    {
        template <typename FormatContext>
        auto format(Connection const& input, FormatContext& ctx) -> decltype(ctx.out());
    };
public:
    inline static std::atomic<std::uintmax_t> s_count = 0; // 统计存活的连接数量
    inline static std::atomic<std::uintmax_t> s_counter = 0;
    inline static std::mutex s_status_report_mutex;
    inline static std::vector<ConnectionStatusReport> s_status_report;
private:
    std::uintmax_t m_id = ++s_counter;
    std::array<int, 2> m_debug_counters = { 3, 3 };
    bool m_cancelled = false;
    bool m_watchdog_alive_flag = true;
    std::array<std::array<std::byte, 2048>, 2> m_buffer = {}; // 用于收发数据的缓冲区
    std::array<Udp::endpoint, 2> m_players; // 记录连接的两个玩家的地址
    std::array<UdpSocket, 2> m_sockets; // 与玩家连接的套接字
    std::future<std::array<Udp::endpoint, 2>> m_server_local_endpoints; // 获取用于联通两个玩家的端口号
    std::chrono::nanoseconds total_time_before_send;
    std::chrono::nanoseconds max_time_before_send;
    std::chrono::nanoseconds total_time_after_send;
    std::chrono::nanoseconds max_time_after_send;
    std::uintmax_t processed_count;

public:
    static a::awaitable<std::array<Udp::endpoint, 2>> start_relay
    (
        std::array<Udp::endpoint, 2> players
    );
    static a::awaitable<void> run_relay
    (
        std::array<UdpSocket, 2> sockets,
        std::array<Udp::endpoint, 2> players
    );
    Connection(std::array<UdpSocket, 2>&& sockets);
    ~Connection();
    a::awaitable<void> watchdog();
    Udp::endpoint get_our_target(std::size_t index, Udp::endpoint their_target);
    a::awaitable<void> do_relay(std::size_t index);
};

class NetnegPlusConnection
{
public:
    inline static std::mutex s_router_map_mutex;
    inline static std::array<std::pair<Udp::endpoint, bool>, std::numeric_limits<std::uint16_t>::max() + 1>s_router_map; // 标记每个token应该转发到哪里，以及alive flag
    inline static std::map<std::uint32_t, std::vector<Udp::endpoint>> s_natneg_map;  // 每个natneg session的缓存

private:
    static bool store_natneg_map(std::uint32_t id, Udp::endpoint endpoint);

public:
    static a::awaitable<void> start_control();
    static a::awaitable<void> start_relay();
    static a::awaitable<void> watchdog();
};

struct EndPointFormatter : fmt::formatter<std::string>
{
    template <typename FormatContext>
    auto format(Udp::endpoint const& input, FormatContext& ctx) -> decltype(ctx.out());
};

template<>
struct fmt::formatter<Connection> : Connection::Formatter {};

template<>
struct fmt::formatter<Udp::endpoint> : EndPointFormatter {};

std::minstd_rand rng{ std::random_device{}() };
std::uniform_int_distribution<std::uint16_t> distribute;

int main()
{
    try
    {
        l::cfg::load_env_levels();
        l::set_default_logger(l::rotating_logger_mt("main", "logs/relay.txt", 1048576 * 5, 3));
        l::flush_every(60s);
        l::info("public ip is {}", RelayServerNat::instance().public_ip().to_string());
        a::io_context context;
        l::info("starting relay server...");
        auto task_1 = a::co_spawn(context, run_control_server, a::use_future);
        auto task_2 = a::co_spawn(context, run_echo_server, a::use_future);
        auto runner_1 = std::async(std::launch::async, [&context] { context.run(); });
        auto runner_2 = std::async(std::launch::async, [&context] { context.run(); });

        a::io_context natneg_plus_context;
        auto netnag_plus_relay_server = a::co_spawn
        (
            natneg_plus_context,
            NetnegPlusConnection::start_relay,
            a::use_future
        );
        auto netnag_plus_control_server = a::co_spawn
        (
            natneg_plus_context,
            NetnegPlusConnection::start_control,
            a::use_future
        );
        auto netnag_plus_runner = std::async(std::launch::async, [&natneg_plus_context] { natneg_plus_context.run(); });

        runner_1.get();
        runner_2.get();
        netnag_plus_runner.get();
        task_1.get();
        task_2.get();
        netnag_plus_relay_server.get();
        netnag_plus_control_server.get();
    }
    catch (std::exception const& e)
    {
        l::critical("Application is terminating because: {}", e.what());
        return 1;
    }
    return 0;
}

a::awaitable<void> run_echo_server()
{
    Udp::socket udp_socket = { co_await a::this_coro::executor, { a::ip::address_v4::any(), 10010 } };
    char data[1024];

    while (true)
    {
        Udp::endpoint endpoint;
        auto recevied_length = co_await udp_socket.async_receive_from(
            boost::asio::buffer(data, 1024),
            endpoint,
            a::use_awaitable);
        l::info("UDP Echo received {} bytes", recevied_length);
        udp_socket.async_send_to(
            boost::asio::buffer(data, recevied_length),
            endpoint,
            [](boost::system::error_code ec, std::size_t bytes_sent)
            {
                // Do nothing.
                if (ec.value() != 0)
                {
                    l::error("UDP Echo send failed with error code {}", ec.value());
                    l::error(ec.message());
                }
                else
                {
                    l::info("UDP Echo sent {} bytes", bytes_sent);
                }
            });
    }
}

a::awaitable<void> run_natneg_plus_server()
{
    co_await(NetnegPlusConnection::start_control() and NetnegPlusConnection::start_relay() and NetnegPlusConnection::watchdog());
}

a::awaitable<void> run_control_server()
{
    Tcp::endpoint port = { a::ip::address_v4::any(), 10086 };
    Tcp::acceptor acceptor{ co_await a::this_coro::executor, port };
    while (true)
    {
        auto socket = co_await acceptor.async_accept(a::use_awaitable);
        a::co_spawn(co_await a::this_coro::executor, [&]
        {
            return process_control_server(Tcp::iostream{ std::move(socket) });
        }, a::detached);
    }
}

a::awaitable<void> process_control_server(Tcp::iostream connection)
{
    j::json response;
    try
    {
        std::string line;
        std::getline(connection, line);
        l::info("Control server: processing input {}", line);
        if (line == "status")
        {
            response = Response{ .succeeded = true, .message = get_status().dump() }.to_json();
        }
        else
        {
            response = co_await spawn_relay_server(j::json::parse(line));
        }
    }
    catch (std::exception const& e)
    {
        l::error("Error in control server: {}", e.what());
        response = Response{ .succeeded = false, .message = e.what() }.to_json();
    }
    try
    {
        connection << response.dump();
        connection.flush();
    }
    catch (std::exception const& e)
    {
        l::error("Error in control server when writing output: {}", e.what());;
    }
}

j::json get_status()
{
    std::scoped_lock lock{ Connection::s_status_report_mutex };

    j::json status = j::json::object();
    status["CurrentConnections"] = Connection::s_count.load();
    status["AllConnections"] = Connection::s_counter.load();
    status["PublicIp"] = RelayServerNat::instance().public_ip().to_string();
    status["ConnectionStatusReport"] = Connection::s_status_report;
    return status;
}

a::awaitable<j::json> spawn_relay_server(j::json request)
{
    auto parse_endpoint = [](std::string text)
    {
        try
        {
            std::size_t separator_position = text.find(':');
            unsigned long port = std::stoul(text.substr(separator_position + 1));
            if (port > std::numeric_limits<std::uint16_t>::max())
            {
                throw std::invalid_argument{ "port value too large" };
            }
            text.resize(separator_position);
            auto ip = a::ip::address_v4::from_string(text);
            return Udp::endpoint{ ip, static_cast<std::uint16_t>(port) };
        }
        catch (std::exception const& e)
        {
            throw std::invalid_argument{ fmt::format("failed to parse {} into endpoint: {}", text, e.what()) };
        }
    };
    auto player_1 = parse_endpoint(request["Player1"].get<std::string>());
    auto player_2 = parse_endpoint(request["Player2"].get<std::string>());

    auto [result_1, result_2] = co_await Connection::start_relay({ player_1, player_2 });
    auto& nat = RelayServerNat::instance();
    nat.translate(result_1);
    nat.translate(result_2);
    l::info("Starting Relay Server for {} and {} with {} and {}", player_1, player_2, result_1, result_2);
    co_return Response{ .succeeded = true, .endpoint_1 = result_1, .endpoint_2 = result_2 }.to_json();
}

j::json Response::to_json() const
{
    j::json result = j::json::object();
    result["Succeeded"] = succeeded;
    result["Message"] = message;
    result["EndPoint1"] = fmt::format("{}", endpoint_1);
    result["EndPoint2"] = fmt::format("{}", endpoint_2);
    return result;
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

a::awaitable<std::array<Udp::endpoint, 2>> Connection::start_relay
(
    std::array<Udp::endpoint, 2> players
)
{
    auto strand = a::make_strand(co_await a::this_coro::executor);
    Udp::endpoint any_port = { a::ip::address_v4::any(), 0 };
    std::array sockets = { UdpSocket{ strand, any_port }, UdpSocket{ strand, any_port } };
    std::array local_endpoints = { sockets[0].local_endpoint(), sockets[1].local_endpoint() };
    a::co_spawn(strand, [sockets = std::move(sockets), players]() mutable
    {
        return run_relay(std::move(sockets), players);
    }, a::detached);
    co_return local_endpoints;
}

a::awaitable<void> Connection::run_relay
(
    std::array<UdpSocket, 2> sockets,
    std::array<Udp::endpoint, 2> players
)
{
    l::info("Creating relay for {} and {}...", players[0], players[1]);
    try
    {
        auto connection = Connection{ std::move(sockets) };
        connection.m_players = { players[1], players[0] };
        l::info("{} serving for players {} and {}", connection, players[0], players[1]);
        co_await(connection.watchdog() and connection.do_relay(0) and connection.do_relay(1));
    }
    catch (std::exception const& e)
    {
        l::error("Caught exception when running relay for {} and {}: {}", players[0], players[1], e.what());
    }
}

Connection::Connection(std::array<UdpSocket, 2>&& sockets) :
    m_sockets{ std::move(sockets) }
{
    ++s_count;
    l::info("{} is being created", *this);

    total_time_before_send = std::chrono::nanoseconds::zero();
    max_time_before_send = std::chrono::nanoseconds::zero();
    total_time_after_send = std::chrono::nanoseconds::zero();
    max_time_after_send = std::chrono::nanoseconds::zero();
    processed_count = 1; // prevent math error
}

Connection::~Connection()
{
    l::info("{} is being destroyed", *this);
    --s_count;

    auto avg_time_before_send = total_time_before_send / processed_count;
    auto avg_time_after_send = total_time_after_send / processed_count;

    std::scoped_lock lock{ s_status_report_mutex };

    //s_status_report.push_back(ConnectionStatusReport
    //{
    //    .processed_count = processed_count,
    //    .avg_time_before_send = avg_time_before_send,
    //    .avg_time_after_send = avg_time_after_send,
    //    .max_time_before_send = max_time_before_send,
    //    .max_time_after_send = max_time_after_send
    //});

    l::debug
    (
        "Processed {} packages, avg time before send is {} ns, avg time after send is {} ns, max time before send is {} ns, max time after send is {} ns",
        processed_count,
        avg_time_before_send.count(),
        avg_time_after_send.count(),
        max_time_before_send.count(),
        max_time_after_send.count()
    );
}

a::awaitable<void> Connection::watchdog()
{
    auto& [s0, s1] = m_sockets;
    SteadyTimer timer{ co_await a::this_coro::executor };
    while (true)
    {
        if (not m_watchdog_alive_flag or not s0.is_open() or not s1.is_open())
        {
            m_cancelled = true;
            l::info("{} is being cancelled by watchdog", *this);
            for (auto& socket : m_sockets)
            {
                if (socket.is_open())
                {
                    socket.close();
                }
            }
            break;
        }
        m_watchdog_alive_flag = false;
        timer.expires_after(45s);
        co_await timer.async_wait();
    };
}

Udp::endpoint Connection::get_our_target(std::size_t index, Udp::endpoint their_target)
{
    m_players[std::size_t{ 1 } - index] = their_target;
    return m_players[index];
}

a::awaitable<void> Connection::do_relay(std::size_t index)
{
    std::string name = fmt::format("{} {}<{}>", *this, std::source_location::current().function_name(), index);
    try
    {
        UdpSocket& receiver = m_sockets[std::size_t{ 1 } - index];
        UdpSocket& sender = m_sockets[index];
        //std::array<std::byte, 2048>& buffer = m_buffer[index];
        //Udp::endpoint from;
        while (not m_cancelled)
        {
            char* data = new char[2048];
            Udp::endpoint from;
            auto bytes_read = co_await receiver.async_receive_from(a::buffer(data, 2048), from);
            auto start = std::chrono::steady_clock::now();
            auto to = get_our_target(index, from);
            m_watchdog_alive_flag = true;
            if (m_debug_counters[index] > 0)
            {
                --m_debug_counters[index];
                l::debug
                (
                    "{} received {} on {}, sending it from {} to {}",
                    name,
                    from,
                    receiver.local_endpoint(),
                    sender.local_endpoint(),
                    to
                );
            }
            auto before_send = std::chrono::steady_clock::now();
            sender.async_send_to(a::buffer(data, bytes_read), to, 
                [data, name](boost::system::error_code ec, std::size_t bytes_recvd)
                {
                    if (ec.value() != 0)
                    {
                        l::error("{} async send to error: {}", name, ec.message());
                    }
                    delete[] data;
                });
            auto after_send = std::chrono::steady_clock::now();
            auto time_before_send = before_send - start;
            auto time_after_send = after_send - start;
            if (time_before_send > max_time_before_send)
            {
                max_time_before_send = time_before_send;
            }
            if (time_after_send > max_time_after_send)
            {
                max_time_after_send = time_after_send;
            }
            total_time_before_send += time_before_send;
            total_time_after_send += time_after_send;
            processed_count++;
        }
    }
    catch (std::exception const& e)
    {
        if (m_cancelled)
        {
            l::info("{} cancelled because: {}", name, e.what());
        }
        else
        {
            l::error("{} terminated because: {}", name, e.what());
        }
        co_return;
    }
    l::info("{} stopped", name);
}

template <typename FormatContext>
auto Connection::Formatter::format(Connection const& input, FormatContext& ctx) -> decltype(ctx.out())
{
    return fmt::format_to(ctx.out(), "Connection #{}", input.m_id);
}

template <typename FormatContext>
auto EndPointFormatter::format(Udp::endpoint const& input, FormatContext& ctx) -> decltype(ctx.out())
{
    return fmt::format_to(ctx.out(), "{}:{}", input.address().to_string(), input.port());
}

bool NetnegPlusConnection::store_natneg_map(std::uint32_t id, Udp::endpoint endpoint)
{
    auto itr = s_natneg_map.find(id);
    if (itr != s_natneg_map.end())
    {
        if (itr->second.size() < 2)
        {
            itr->second.push_back(endpoint);
            return true;
        }
        // Received more than two INIT, impossibe, clean it.
        else
        {
            itr->second.clear();
            itr->second.push_back(endpoint);
            return false;
        }
    }
    else
    {
        s_natneg_map.insert(std::make_pair(id, std::vector<Udp::endpoint>(1, endpoint)));
        return false;
    }
}

a::awaitable<void> NetnegPlusConnection::start_control()
{
    //std::scoped_lock lock{ NetnegPlusConnection::s_natneg_map };
    Udp::socket control_socket = { co_await a::this_coro::executor, { a::ip::address_v4::any(), 10087 } };
    while (true)
    {
        char control_data[64];
        Udp::endpoint endpoint;
        auto recevied_length = co_await control_socket.async_receive_from
        (
            boost::asio::buffer(control_data, 64),
            endpoint,
            a::use_awaitable
        );
        if (recevied_length != 4)
        {
            l::error("NATNEG+ control received invalid length {} bytes", recevied_length);
            continue;
        }
        // Parse input
        std::uint32_t session_id;
        memcpy(&session_id, control_data, 4);
        if (store_natneg_map(session_id, endpoint))
        {
            // Create relay here.
            auto player_1 = s_natneg_map[session_id][0];
            auto player_2 = s_natneg_map[session_id][1];
            std::uint16_t token_1 = distribute(rng);
            while (s_router_map[token_1] != std::pair<Udp::endpoint, bool>())
            {
                token_1 = distribute(rng);
            }
            std::uint16_t token_2 = distribute(rng);
            while (s_router_map[token_2] != std::pair<Udp::endpoint, bool>())
            {
                token_2 = distribute(rng);
            }
            s_router_map[token_1] = std::make_pair(player_2, true);
            s_router_map[token_2] = std::make_pair(player_1, true);

            // Send message here.
            std::uint32_t ip_1 = player_1.address().to_v4().to_ulong();
            ip_1 = htonl(ip_1);
            std::uint16_t port_1 = player_1.port();
            port_1 = htons(port_1);
            std::uint32_t ip_2 = player_2.address().to_v4().to_ulong();
            ip_2 = htonl(ip_2);
            std::uint16_t port_2 = player_1.port();
            port_2 = htons(port_2);

            l::info
            (
                "NATNEG+ control create following info: player_1 endpoint {}, player_1 token {}, player_2 endpoint {}, player_2 token {}",
                player_1,
                token_1,
                player_2,
                token_2
            );
            char response_1[16];
            memcpy(response_1, "CONNECT", 7);
            memcpy(response_1 + 7, &token_1, 2);
            memcpy(response_1 + 9, &ip_2, 4);
            memcpy(response_1 + 13, &port_2, 2);
            char response_2[16];
            memcpy(response_2, "CONNECT", 7);
            memcpy(response_2 + 7, &token_2, 2);
            memcpy(response_2 + 9, &ip_1, 4);
            memcpy(response_2 + 13, &port_1, 2);
            co_await
            (
                control_socket.async_send_to
                (
                    a::buffer(response_1, 15),
                    player_1,
                    a::use_awaitable
                )
                and
                control_socket.async_send_to
                (
                    a::buffer(response_2, 15),
                    player_2,
                    a::use_awaitable
                )
            );
            // Remove map
            s_natneg_map.erase(session_id);
        }
    }
}

a::awaitable<void> NetnegPlusConnection::start_relay()
{
    watchdog();
    Udp::socket relay_socket = { co_await a::this_coro::executor, { a::ip::address_v4::any(), 10088 } };
    while (true)
    {
        char relay_data[2048];
        Udp::endpoint endpoint;
        auto recevied_length = co_await relay_socket.async_receive_from
        (
            boost::asio::buffer(relay_data, 2048),
            endpoint,
            a::use_awaitable
        );
        if (recevied_length < 2)
        {
            l::error("NATNEG+ relay received a tiny packet (< 2 bytes), ignore");
            continue;
        }
        // Parse to get token
        std::uint16_t token;
        memcpy(&token, relay_data, 2);
        // Obtain endpoint, set alive flag, then send.
        auto& target = s_router_map[token];
        if (target == std::pair<Udp::endpoint, bool>())
        {
            l::error("Invalid token {}", token);
            continue;
        }
        target.second = true;
        co_await relay_socket.async_send_to
        (
            a::buffer(relay_data + 2, recevied_length - 2),
            target.first,
            a::use_awaitable
        );
    }
}

a::awaitable<void> NetnegPlusConnection::watchdog()
{
    // Clean dead connection.
    boost::asio::use_awaitable_t<>::as_default_on_t<boost::asio::steady_timer> timer{ co_await boost::asio::this_coro::executor };
    while (true)
    {
        for (auto& route : s_router_map)
        {
            if (not route.second)
            {
                route = std::pair<Udp::endpoint, bool>();
            }
            else
            {
                route.second = false;
            }
        }
        timer.expires_after(45s);
        co_await timer.async_wait();
    };
    co_return;
}