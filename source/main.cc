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
void echo_do_receive(Udp::socket& udp_socket, Udp::endpoint& endpoint, char* data);
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

struct EndPointFormatter : fmt::formatter<std::string>
{
    template <typename FormatContext>
    auto format(Udp::endpoint const& input, FormatContext& ctx) -> decltype(ctx.out());
};

template<>
struct fmt::formatter<Connection> : Connection::Formatter {};

template<>
struct fmt::formatter<Udp::endpoint> : EndPointFormatter {};

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
        runner_1.get();
        runner_2.get();
        task_1.get();
        task_2.get();
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
    processed_count = 0;
}

Connection::~Connection()
{
    l::info("{} is being destroyed", *this);
    --s_count;

    auto avg_time_before_send = total_time_before_send / processed_count;
    auto avg_time_after_send = total_time_after_send / processed_count;

    std::scoped_lock lock{ s_status_report_mutex };

    s_status_report.push_back(ConnectionStatusReport
    {
        .processed_count = processed_count,
        .avg_time_before_send = avg_time_before_send,
        .avg_time_after_send = avg_time_after_send,
        .max_time_before_send = max_time_before_send,
        .max_time_after_send = max_time_after_send
    });

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
            std::array<std::byte, 2048> temp_buffer{};
            Udp::endpoint from;
            auto bytes_read = co_await receiver.async_receive_from(a::buffer(temp_buffer), from);
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
            /*co_await*/ sender.async_send_to(a::buffer(temp_buffer.data(), bytes_read), to);
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