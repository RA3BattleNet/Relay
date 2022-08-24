#include <boost/asio.hpp>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <array>
#include <chrono>
#include <mutex>
#include <stop_token>

namespace a = boost::asio;
namespace h = httplib;
namespace t = std::chrono;
using namespace std::literals;

a::awaitable<void> run_control_server();
a::awaitable<void> process_control_server(a::ip::tcp::iostream connection);
nlohmann::json get_status();
a::awaitable<nlohmann::json> spawn_relay_server(nlohmann::json request);

// 把服务器的内网地址转换为外网地址
class RelayServerNat
{
private:
    std::mutex m_mutex;
    a::ip::address_v4 m_public_ip;

private:
    RelayServerNat() = default;
    void update_ip();

public:
    static RelayServerNat& instance();
    a::ip::address_v4 public_ip();
    void translate(a::ip::udp::endpoint& local_endpoint);
};

class Connection
{
private:
    inline static long s_count = 0; // 统计存活的连接数量
private:
    std::mutex m_mutex;
    t::steady_clock::time_point m_start_time = t::steady_clock::now(); // 记录连接的开始时间
    t::steady_clock::time_point m_last_update; // 记录上次更新的时间（用来判断连接什么时候超时）
    std::array<std::array<std::byte, 2048>, 2> m_buffer = {}; // 用于收发数据的缓冲区
    std::array<a::ip::udp::endpoint, 2> m_players; // 记录连接的两个玩家的地址
    std::array<a::ip::udp::socket, 2> m_sockets; // 与玩家连接的套接字
    std::future<std::array<a::ip::udp::endpoint, 2>> m_server_local_endpoints; // 获取用于联通两个玩家的端口号

public:
    static a::awaitable<std::array<a::ip::udp::endpoint, 2>> start_relay
    (
        std::array<a::ip::udp::endpoint, 2> players
    );
    static void run_relay
    (
        a::any_io_executor context,
        std::array<a::ip::udp::endpoint, 2> players,
        std::promise<std::array<a::ip::udp::endpoint, 2>> result
    );
    Connection(a::any_io_executor const& executor);
    template<std::size_t index>
    a::ip::udp::endpoint get_our_target(a::ip::udp::endpoint&& their_target);
    template<std::size_t index>
    a::awaitable<void> do_relay();
};

int main()
{
    try
    {
        a::io_context context;
        auto task = a::co_spawn(context, run_control_server, a::use_future);
        auto runner_1 = std::async(std::launch::async, [&context] { context.run(); });
        auto runner_2 = std::async(std::launch::async, [&context] { context.run(); });
    }
    catch (std::exception const& e)
    {
        spdlog::critical(e.what());
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
    std::string line;
    std::getline(connection, line);
    if (line == "status")
    {
        nlohmann::json response;
        response["Succeeded"] = false;
        response["Message"] = "Not implemented";
        connection << response << std::endl;
    }
    connection << co_await spawn_relay_server(nlohmann::json::parse(line)) << std::endl;
}

a::awaitable<nlohmann::json> spawn_relay_server(nlohmann::json request)
{
    auto string_1 = request["Player1"].get<std::string>();
    auto s_1 = string_1.find(":");
    auto string_2 = request["Player2"].get<std::string>();
    auto s_2 = string_1.find(":");

    auto ip_1 = a::ip::address_v4::from_string(string_1.substr(0, s_1));
    auto ip_2 = a::ip::address_v4::from_string(string_2.substr(0, s_2));
    auto port_1 = static_cast<std::uint16_t>(std::stoi(string_1.substr(s_1 + 1)));
    auto port_2 = static_cast<std::uint16_t>(std::stoi(string_2.substr(s_2 + 1)));
    auto player_1 = a::ip::udp::endpoint{ ip_1, port_1 };
    auto player_2 = a::ip::udp::endpoint{ ip_2, port_2 };

    auto [result_1, result_2] = co_await Connection::start_relay({ player_1, player_2 });
    auto& nat = RelayServerNat::instance();
    nat.translate(result_1);
    nat.translate(result_2);
    auto result = nlohmann::json();
    result["Succeeded"] = true;
    result["EndPoint1"] = result_1.address().to_string() + ":" + std::to_string(result_1.port());
    result["EndPoint1"] = result_2.address().to_string() + ":" + std::to_string(result_2.port());
    co_return result;
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
                    spdlog::error("error when updating ip: {}", e.what());
                }
                std::this_thread::sleep_for(30min);
            }
        };
        std::thread{ runner }.detach();
        return &self;
    })();
    return *pointer;
}

a::ip::address_v4 RelayServerNat::public_ip()
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
    std::promise<std::array<a::ip::udp::endpoint, 2>> promise;
    auto result = promise.get_future();
    std::thread{ run_relay, co_await a::this_coro::executor, players, std::move(promise) }.detach();
    co_return result.get();
}

void Connection::run_relay
(
    a::any_io_executor executor,
    std::array<a::ip::udp::endpoint, 2> players,
    std::promise<std::array<a::ip::udp::endpoint, 2>> result
)
{
    auto strand = a::make_strand(executor);
    auto connection = Connection{ strand };
    auto& [s0, s1] = connection.m_sockets;
    s0.bind(a::ip::udp::endpoint{ a::ip::udp::v4(), 0 });
    s1.bind(a::ip::udp::endpoint{ a::ip::udp::v4(), 0 });
    result.set_value(std::array{ s0.local_endpoint(), s1.local_endpoint() });
    connection.m_last_update = t::steady_clock::now();
    connection.m_players = { players[1], players[0] };
    auto watchdog = a::co_spawn(strand, [&connection]() -> a::awaitable<void>
    {
        a::steady_timer timer{ co_await a::this_coro::executor };
        while (true) // TODO: Optimize this
        {
            if ((connection.m_last_update - connection.m_start_time > 1min)
                or not connection.m_sockets[0].is_open()
                or not connection.m_sockets[1].is_open())
            {
                connection.m_sockets[0].close();
                connection.m_sockets[1].close();
                break;
            }
            timer.expires_after(30s);
            co_await timer.async_wait(a::use_awaitable);
        };
    }, a::use_future);
    auto task1 = a::co_spawn(strand, [&connection]() -> a::awaitable<void>
    {
        co_await connection.do_relay<0>();
    }, a::use_future);
    auto task2 = a::co_spawn(strand, [&connection]() -> a::awaitable<void>
    {
        co_await connection.do_relay<1>();
    }, a::use_future);

}

Connection::Connection(a::any_io_executor const& executor) :
    m_sockets{ a::ip::udp::socket{executor, a::ip::udp::v4()}, a::ip::udp::socket{executor, a::ip::udp::v4()} }
{}


template<std::size_t index>
a::ip::udp::endpoint Connection::get_our_target(a::ip::udp::endpoint&& their_target)
{
    auto now = t::steady_clock::now();
    std::scoped_lock lock{ m_mutex };
    m_last_update = now;
    m_players[std::size_t{ 1 } - index] = std::move(their_target);
    return m_players[index];
}

template<std::size_t index>
a::awaitable<void> Connection::do_relay()
{
    auto& receiver = m_sockets[index];
    auto& sender = m_sockets[std::size_t{ 1 } - index];
    auto& buffer = m_buffer[index];
    a::ip::udp::endpoint from;
    while (receiver.is_open())
    {
        auto bytes_read = co_await receiver.async_receive_from(a::buffer(buffer), from, a::use_awaitable);
        auto to = get_our_target<index>(std::move(from));
        co_await sender.async_send_to(a::buffer(buffer.data(), bytes_read), to, a::use_awaitable);
    }
}

