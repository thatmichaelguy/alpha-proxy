#pragma once

#include <cstdint>
#include <boost/asio.hpp>
#include <boost/asio/steady_timer.hpp>
#include <memory>
#include <string>
#include <list>
#include <thread>
#include <functional>
#include <map>
#include <chrono>
#include <chrono>
#include <vector>

#if BOOST_VERSION <= 106200
#define io_context io_service
#endif

using Packet = std::vector<uint8_t>;
using PacketPtr = std::shared_ptr<Packet>;

class Session;
using SessionPtr = std::shared_ptr<Session>;

class Proxy;
using ProxyPtr = std::shared_ptr<Proxy>;

class Server;

constexpr int CHECK_INTERVAL = 1000;
constexpr int TIMEOUT = 10000;
constexpr int BUFFER_SIZE = 65535;

class Session : public std::enable_shared_from_this<Session>
{

public:
    Session(boost::asio::io_context &io, boost::asio::io_service::strand &strand, uint32_t id, uint32_t ip) : m_io(io), m_strand(strand), m_socket(m_io), m_id(id), m_ip(ip) {}
    ~Session();

    void start(uint16_t destPort);
    void terminate();
    void addProxy(const ProxyPtr &proxy);
    void removeProxy(const ProxyPtr &proxy);
    void onProxyPacket(uint32_t packetId, uint32_t lastRecivedPacketId, const PacketPtr &packet);

private:
    void readHeader();
    void onHeader(const boost::system::error_code &ec, std::size_t bytes_transferred);
    void onPacket(const boost::system::error_code &ec, std::size_t bytes_transferred);
    void onStatusPacket(std::size_t bytes_transferred);

    void onSent(const boost::system::error_code &ec, std::size_t bytes_transferred);

    boost::asio::io_context &m_io;
    boost::asio::io_service::strand &m_strand;
    boost::asio::ip::tcp::socket m_socket;
    uint32_t m_id;
    uint32_t m_ip = 0;

    bool m_connected = false;
    uint32_t m_inputPacketId = 1;
    uint32_t m_outputPacketId = 1;

    std::list<std::weak_ptr<Proxy>> m_proxies;

    uint8_t m_buffer[BUFFER_SIZE];
    std::map<uint32_t, PacketPtr> m_sendQueue;
    std::map<uint32_t, PacketPtr> m_proxySendQueue;
};

class Proxy : public std::enable_shared_from_this<Proxy>
{

public:
    Proxy(boost::asio::io_context &io, boost::asio::io_service::strand &strand, boost::asio::ip::tcp::socket socket, Server *server) : m_io(io), m_strand(strand), m_timer(io), m_socket(std::move(socket)), m_server(server)
    {
    }
    ~Proxy();

    void start();
    void terminate();
    void send(const PacketPtr &packet, bool front = false);
    void sendSessionEnd(uint32_t sessionId);

private:
    void check(const boost::system::error_code &ec);

    void readHeader();
    void onHeader(const boost::system::error_code &ec, std::size_t bytes_transferred);
    void onProxyHeader(const boost::system::error_code &ec, std::size_t bytes_transferred);
    void onPacket(const boost::system::error_code &ec, std::size_t bytes_transferred);

    void sendPing();
    void onSent(PacketPtr p, const boost::system::error_code &ec, std::size_t bytes_transferred);

    boost::asio::io_context &m_io;
    boost::asio::io_service::strand &m_strand;
    boost::asio::steady_timer m_timer;
    boost::asio::ip::tcp::socket m_socket;

    Server *m_server;

    uint16_t m_destPort = 0;
    uint32_t m_uid = 0;
    uint32_t m_ip = 0;
#ifdef DEBUG
    uint32_t m_proxy_ip = 0;
#endif
    bool m_intialPacket = true;
    bool m_firstPacket = true;
    bool m_realIP = false;

    uint8_t m_buffer[BUFFER_SIZE];
    std::list<PacketPtr> m_sendQueue;

    std::chrono::time_point<std::chrono::high_resolution_clock> m_lastPacket;
};

class Server
{
public:
    Server(uint16_t port, uint16_t maxConnectionsPerIP);
    void run(size_t threads_num = 1);
    void open();
    uint16_t getMaxConnectionsPerIP() const;

private:
    void accept();

    uint16_t m_port;
    uint16_t m_maxConnectionsPerIP;
    boost::asio::io_context m_io;
    boost::asio::io_service::strand m_strand;
    boost::asio::ip::tcp::acceptor m_acceptor;
    boost::asio::steady_timer m_timer;
};
