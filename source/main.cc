#include <boost/asio.hpp>
#if not __has_include(<coroutine>)
#include <experimental/coroutine>
#endif
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/spdlog.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <source_location>

namespace a = boost::asio;
namespace h = httplib;
namespace j = nlohmann;
namespace l = spdlog;
namespace t = std::chrono;
using namespace std::literals;
using namespace a::experimental::awaitable_operators;

a::awaitable<void> run_control_server();
a::awaitable<void> process_control_server(a::ip::tcp::iostream connection);
j::json get_status();
a::awaitable<j::json> spawn_relay_server(j::json request);

struct Response
{
    bool succeeded = false;
    std::string message;
    a::ip::udp::endpoint endpoint_1;
    a::ip::udp::endpoint endpoint_2;

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
    void translate(a::ip::udp::endpoint& local_endpoint);
};

class Connection
{
public:
    using SteadyTimer = a::use_awaitable_t<>::as_default_on_t<a::steady_timer>;
    using UdpSocket = a::use_awaitable_t<>::as_default_on_t<a::ip::udp::socket>;
    struct Formatter : fmt::formatter<std::string>
    {
        template <typename FormatContext>
        auto format(Connection const& input, FormatContext& ctx) -> decltype(ctx.out());
    };
public:
    inline static std::atomic<std::uintmax_t> s_count = 0; // 统计存活的连接数量
    inline static std::atomic<std::uintmax_t> s_counter = 0;
private:
    std::uintmax_t m_id = ++s_counter;
    std::array<int, 2> m_debug_counters = { 3, 3 };
    bool m_cancelled = false;
    t::steady_clock::time_point m_start_time = t::steady_clock::now(); // 记录连接的开始时间
    t::steady_clock::time_point m_last_update; // 记录上次更新的时间（用来判断连接什么时候超时）
    std::array<std::array<std::byte, 2048>, 2> m_buffer = {}; // 用于收发数据的缓冲区
    std::array<a::ip::udp::endpoint, 2> m_players; // 记录连接的两个玩家的地址
    std::array<UdpSocket, 2> m_sockets; // 与玩家连接的套接字
    std::future<std::array<a::ip::udp::endpoint, 2>> m_server_local_endpoints; // 获取用于联通两个玩家的端口号

public:
    static a::awaitable<std::array<a::ip::udp::endpoint, 2>> start_relay
    (
        std::array<a::ip::udp::endpoint, 2> players
    );
    static a::awaitable<void> run_relay
    (
        std::array<UdpSocket, 2> sockets,
        std::array<a::ip::udp::endpoint, 2> players
    );
    Connection(std::array<UdpSocket, 2>&& sockets);
    ~Connection();
    a::awaitable<void> watchdog();
    template<std::size_t index>
    a::ip::udp::endpoint get_our_target(a::ip::udp::endpoint&& their_target);
    template<std::size_t index>
    a::awaitable<void> do_relay();
};

struct EndPointFormatter : fmt::formatter<std::string>
{
    template <typename FormatContext>
    auto format(a::ip::udp::endpoint const& input, FormatContext& ctx) -> decltype(ctx.out());
};

template<>
struct fmt::formatter<Connection> : Connection::Formatter {};

template<>
struct fmt::formatter<a::ip::udp::endpoint> : EndPointFormatter {};

int main()
{
    try
    {
        l::set_default_logger(l::rotating_logger_mt("file_logger", "logs/relay.txt", 1048576 * 5, 3));
        a::io_context context;
        auto task = a::co_spawn(context, run_control_server, a::use_future);
        auto runner_1 = std::async(std::launch::async, [&context] { context.run(); });
        auto runner_2 = std::async(std::launch::async, [&context] { context.run(); });
        runner_1.get();
        runner_2.get();
    }
    catch (std::exception const& e)
    {
        l::critical("Application is terminating because: {}", e.what());
    }
}

a::awaitable<void> run_control_server()
{
    a::ip::tcp::endpoint port = { a::ip::address_v4::any(), 10086 };
    a::ip::tcp::acceptor acceptor{ co_await a::this_coro::executor, port };
    while (true)
    {
        auto socket = co_await acceptor.async_accept(a::use_awaitable);
        a::co_spawn(co_await a::this_coro::executor, [&]
        {
            return process_control_server(a::ip::tcp::iostream{ std::move(socket) });
        }, a::detached);
    }
}

a::awaitable<void> process_control_server(a::ip::tcp::iostream connection)
{
    try
    {
        std::string line;
        std::getline(connection, line);
        l::info("Control server: processing input {}", line);
        if (line == "status")
        {
            connection << Response{ .succeeded = true, .message = get_status().dump() }.to_json() << std::endl;
            co_return;
        }
        connection << co_await spawn_relay_server(j::json::parse(line)) << std::endl;
    }
    catch (std::exception const& e)
    {
        l::error("Error in control server: {}", e.what());
        connection << Response{ .succeeded = false, .message = e.what() }.to_json() << std::endl;
    }
}

j::json get_status()
{
    j::json status = j::json::object();
    status["CurrentConnections"] = Connection::s_count.load();
    status["AllConnections"] = Connection::s_counter.load();
    status["PublicIp"] = RelayServerNat::instance().public_ip().to_string();
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
            return a::ip::udp::endpoint{ ip, static_cast<std::uint16_t>(port) };
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
    h::Client client{ "https://api.ipify.org", 80 };
    if (h::Result result = client.Get("/"); result)
    {
        auto new_ip = a::ip::address_v4::from_string(result.value().body);;
        std::scoped_lock lock{ m_mutex };
        m_public_ip = new_ip;
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
                try
                {
                    self.update_ip();
                }
                catch (std::exception const& e)
                {
                    l::error("error when updating ip: {}", e.what());
                }
                std::this_thread::sleep_for(30min);
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

void RelayServerNat::translate(a::ip::udp::endpoint& local_endpoint)
{
    std::scoped_lock lock{ m_mutex };
    local_endpoint.address(m_public_ip);
}

a::awaitable<std::array<a::ip::udp::endpoint, 2>> Connection::start_relay
(
    std::array<a::ip::udp::endpoint, 2> players
)
{
    auto strand = a::make_strand(co_await a::this_coro::executor);
    a::ip::udp::endpoint any_port = { a::ip::address_v4::any(), 0 };
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
    std::array<a::ip::udp::endpoint, 2> players
)
{
    l::info("Creating relay for {} and {}...", players[0], players[1]);
    try
    {
        auto connection = Connection{ std::move(sockets) };
        connection.m_last_update = t::steady_clock::now();
        connection.m_players = { players[1], players[0] };
        l::info("{} serving for players {} and {}", connection, players[0], players[1]);
        co_await(connection.watchdog() and connection.do_relay<0>() and connection.do_relay<1>());
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
}

Connection::~Connection()
{
    l::info("{} is being destroyed", *this);
    --s_count;
}

a::awaitable<void> Connection::watchdog()
{
    auto& [s0, s1] = m_sockets;
    SteadyTimer timer{ co_await a::this_coro::executor };
    while (true)
    {
        if ((m_last_update - m_start_time > 1min)
            or not s0.is_open() or not s1.is_open())
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
        timer.expires_after(30s);
        co_await timer.async_wait();
    };
}

template<std::size_t index>
a::ip::udp::endpoint Connection::get_our_target(a::ip::udp::endpoint&& their_target)
{
    auto now = t::steady_clock::now();
    m_last_update = now;
    m_players[std::size_t{ 1 } - index] = std::move(their_target);
    return m_players[index];
}

template<std::size_t index>
a::awaitable<void> Connection::do_relay()
{
    std::string name = fmt::format("{} {}<{}>", *this, std::source_location::current().function_name(), index);
    try
    {
        auto& receiver = m_sockets[std::size_t{ 1 } - index];
        auto& sender = m_sockets[index];
        auto& buffer = m_buffer[index];
        while (not m_cancelled)
        {
            a::ip::udp::endpoint from;
            auto bytes_read = co_await receiver.async_receive_from(a::buffer(buffer), from);
            auto to = get_our_target<index>(std::move(from));
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
            co_await sender.async_send_to(a::buffer(buffer.data(), bytes_read), to);
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
auto EndPointFormatter::format(a::ip::udp::endpoint const& input, FormatContext& ctx) -> decltype(ctx.out())
{
    return fmt::format_to(ctx.out(), "{}:{}", input.address().to_string(), input.port());
}