//#define DEBUG
//#define WITH_IP_EXTENSION
//#define ALLOW_ONLY_HAPROXY
//#define FREE_VERSION

#include "server.h"
#include <iostream>
#include <set>

// SERVER

std::map <uint32_t, std::weak_ptr<Session> > g_sessions;
std::map <uint32_t, uint32_t> g_connectionsPerIp;
int g_activeConnections = 0;

Session::~Session()
{
    g_sessions.erase(m_id);
}

void Session::start(uint16_t destPort)
{
#ifdef DEBUG
    std::clog << "[Session " << m_id << "] start, port: " << destPort << std::endl;
#endif
    auto self(shared_from_this());
    auto endpoint = boost::asio::ip::tcp::endpoint(boost::asio::ip::address_v4::from_string("127.0.0.1"), destPort);
    self->m_socket.async_connect(endpoint, m_strand.wrap([self, endpoint, destPort](const boost::system::error_code& ec) {
            if (ec) {
                std::cerr << "Can't establish connection with local server (port " << destPort << ")" << std::endl;
                return;
            }
#ifdef DEBUG
            std::clog << "[Session " << self->m_id << "] connected to remote server" << std::endl;
#endif
            boost::system::error_code ecc;
            self->m_socket.set_option(boost::asio::ip::tcp::no_delay(true), ecc);
            self->m_socket.set_option(boost::asio::socket_base::send_buffer_size(65536), ecc);
            self->m_socket.set_option(boost::asio::socket_base::receive_buffer_size(65536), ecc);
            if (ecc) {
#ifdef DEBUG
                std::clog << "[Session " << self->m_id << "] start error: " << ecc.message() << std::endl;
#endif
            }
            
#ifdef WITH_IP_EXTENSION
            auto packet = std::make_shared<Packet>(6);
            *(uint16_t*)(&(packet->data())[0]) = 0xFFFE;
            *(uint32_t*)(&(packet->data())[2]) = self->m_ip;
            boost::asio::async_write(self->m_socket, boost::asio::buffer(packet->data(), packet->size()),
                                self->m_strand.wrap([self, packet, destPort](const boost::system::error_code& ecc, size_t) {
                                    if(ecc) {
                                        std::cerr << "Can't send real IP to local server (port " << destPort << ")" << std::endl;
                                        return;                                        
                                    }
                                    self->m_connected = true;
                                    g_activeConnections += 1;
                                    self->readHeader();
                                    if (!self->m_sendQueue.empty() && self->m_sendQueue.begin()->first == self->m_inputPacketId) {
                                        boost::asio::async_write(self->m_socket, boost::asio::buffer(self->m_sendQueue.begin()->second->data(), self->m_sendQueue.begin()->second->size()),
                                            self->m_strand.wrap(std::bind(&Session::onSent, self, std::placeholders::_1, std::placeholders::_2)));
                                    }                                    
                                }));
#else
            self->m_connected = true;
            self->readHeader();
            if (!self->m_sendQueue.empty() && self->m_sendQueue.begin()->first == self->m_inputPacketId) {
                boost::asio::async_write(self->m_socket, boost::asio::buffer(self->m_sendQueue.begin()->second->data(), self->m_sendQueue.begin()->second->size()),
                    self->m_strand.wrap(std::bind(&Session::onSent, self, std::placeholders::_1, std::placeholders::_2)));
            }    
#endif
        }));
}

void Session::terminate()
{
#ifdef DEBUG
    std::clog << "[Session " << m_id << "] terminate" << std::endl;
#endif
    for (auto& it : m_proxies) {
        if (auto proxy = it.lock()) {
            proxy->sendSessionEnd(m_id);
        }
    }
    g_sessions.erase(m_id);
    boost::system::error_code ec;
    m_socket.close(ec);
    g_activeConnections -= 1;
#ifdef FREE_VERSION
    std::cout << "Connections: " << g_activeConnections << " (limit: 200)" << std::endl;
    if(g_activeConnections > 300) // will crash if someones try to cheat
        terminate();
#endif    
}

void Session::readHeader()
{
    boost::asio::async_read(m_socket, boost::asio::buffer(m_buffer + 14, 2),
        m_strand.wrap(std::bind(&Session::onHeader, shared_from_this(), std::placeholders::_1, std::placeholders::_2)));
}

void Session::onHeader(const boost::system::error_code& ec, std::size_t bytes_transferred)
{
    if (ec || bytes_transferred != 2) {
#ifdef DEBUG
        std::clog << "[Session " << m_id << "] onHeader error: " << ec.message() << std::endl;
#endif
        return terminate();
    }
    
    uint16_t packetSize = *(uint16_t*)(&m_buffer[14]);
    if (packetSize == 0 || packetSize + 16 > BUFFER_SIZE) {
#ifdef DEBUG
        std::clog << "[Session " << m_id << "] wrong packet size: " << packetSize << std::endl;
#endif
        return terminate();
    }

    boost::asio::async_read(m_socket, boost::asio::buffer(m_buffer + 16, packetSize),
        m_strand.wrap(std::bind(&Session::onPacket, shared_from_this(), std::placeholders::_1, std::placeholders::_2)));
}

void Session::onPacket(const boost::system::error_code& ec, std::size_t bytes_transferred)
{
    if (ec) {
#ifdef DEBUG
        std::clog << "[Session " << m_id << "] onPacket error: " << ec.message() << std::endl;
#endif
        return terminate();
    }
    
    if(m_outputPacketId == 1) {
#ifdef FREE_VERSION
        std::cout << "Connections: " << g_activeConnections << " (limit: 200)" << std::endl;
#endif        
    }

    uint32_t packetId = m_outputPacketId++;
    uint16_t packetSize = *(uint16_t*)(&m_buffer[14]);
    *(uint16_t*)(&m_buffer[0]) = packetSize + 14;
    *(uint32_t*)(&m_buffer[2]) = m_id;
    *(uint32_t*)(&m_buffer[6]) = packetId;
    *(uint32_t*)(&m_buffer[10]) = m_inputPacketId - 1;

#ifdef DEBUG
    //std::clog << "[Session " << m_id << "] onPacket size: " << packetSize << std::endl;
#endif

    auto packet = std::make_shared<Packet>(m_buffer, m_buffer + 16 + packetSize);
    m_proxySendQueue.emplace(packetId, packet);

    readHeader();

    for (auto it = m_proxies.begin(); it != m_proxies.end(); ) {
        if (auto proxy = it->lock()) {
            proxy->send(packet);
            ++it;
            continue;
        }
        it = m_proxies.erase(it);
    }
}

void Session::addProxy(const ProxyPtr& proxy)
{
#ifdef DEBUG
    std::clog << "[Session " << m_id << "] addProxy" << std::endl;
#endif
    for (auto it = m_proxies.begin(); it != m_proxies.end(); ) {
        if (auto p = it->lock()) {
            if (p == proxy)
                return;
            ++it;
            continue;
        }
        it = m_proxies.erase(it);
    }
    m_proxies.push_back(proxy);
    // send pending packets
    for (auto& packet : m_proxySendQueue) {
        proxy->send(packet.second);
    }
}

void Session::removeProxy(const ProxyPtr& proxy)
{
#ifdef DEBUG
    std::clog << "[Session " << m_id << "] removeProxy" << std::endl;
#endif
    for (auto it = m_proxies.begin(); it != m_proxies.end(); ) {
        if (auto p = it->lock()) {
            if (p == proxy) 
            {
                it = m_proxies.erase(it);
                continue;
            }
            ++it;
            continue;
        }
        it = m_proxies.erase(it);
    }
}

void Session::onProxyPacket(uint32_t packetId, uint32_t lastRecivedPacketId, const PacketPtr& packet)
{
#ifdef DEBUG
    std::clog << "[Session " << m_id << "] onProxyPacket, id: " << packetId << " ("<< m_inputPacketId << ") last: " << lastRecivedPacketId <<
        " (" << m_outputPacketId << ") size: " << packet->size() << std::endl;
#endif
    if (packetId < m_inputPacketId) {
        return;
    }

    auto it = m_proxySendQueue.begin();
    while (it != m_proxySendQueue.end() && it->first <= lastRecivedPacketId) {
        it = m_proxySendQueue.erase(it);
    }

    bool sendNow = m_connected && (m_sendQueue.empty() || m_sendQueue.begin()->first != m_inputPacketId);
    m_sendQueue.emplace(packetId, packet);
    if (packetId == m_inputPacketId && sendNow) {
        boost::asio::async_write(m_socket, boost::asio::buffer(packet->data(), packet->size()),
            m_strand.wrap(std::bind(&Session::onSent, shared_from_this(), std::placeholders::_1, std::placeholders::_2)));
    }
}

void Session::onSent(const boost::system::error_code& ec, std::size_t bytes_transferred)
{
    if (ec) {
#ifdef DEBUG
        std::clog << "[Session " << m_id << "] onSent error: " << ec.message() << std::endl;
#endif
        return terminate();
    }
#ifdef DEBUG
    //std::clog << "[Session " << m_id << "] onSent, bytes: " << bytes_transferred << std::endl;
#endif
    if (m_sendQueue.begin()->first != 0) {
        m_inputPacketId += 1;
    }
    m_sendQueue.erase(m_sendQueue.begin());
    if (!m_sendQueue.empty() && (m_sendQueue.begin()->first == m_inputPacketId || m_sendQueue.begin()->first == 0)) {
        boost::asio::async_write(m_socket, boost::asio::buffer(m_sendQueue.begin()->second->data(), m_sendQueue.begin()->second->size()),
            m_strand.wrap(std::bind(&Session::onSent, shared_from_this(), std::placeholders::_1, std::placeholders::_2)));
    }
}



Proxy::~Proxy() {
    if(m_realIP) {
        g_connectionsPerIp[m_ip] -= 1;
    }
}


void Proxy::start()
{
    m_lastPacket = std::chrono::high_resolution_clock::now();
    check(boost::system::error_code());
    readHeader();
}

void Proxy::terminate()
{
#ifdef DEBUG
    std::clog << "[Proxy " << m_destPort << "] terminate" << std::endl;
#endif
    boost::system::error_code ec;
    m_timer.cancel(ec);
    m_socket.close(ec);
}

void Proxy::check(const boost::system::error_code& ec)
{
    if (ec) {
        return;
    }

    uint32_t lastPacket = (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now() - m_lastPacket).count();
    if (lastPacket > TIMEOUT) {
#ifdef DEBUG
        std::clog << "[Proxy " << m_destPort << "] timeout" << std::endl;
#endif
        return terminate();
    }

    m_timer.expires_from_now(std::chrono::milliseconds(CHECK_INTERVAL));
    m_timer.async_wait(m_strand.wrap(std::bind(&Proxy::check, shared_from_this(), std::placeholders::_1)));
}

void Proxy::readHeader()
{
    boost::asio::async_read(m_socket, boost::asio::buffer(m_buffer, 2), 
        m_strand.wrap(std::bind(&Proxy::onHeader, shared_from_this(), std::placeholders::_1, std::placeholders::_2)));
}

void Proxy::onHeader(const boost::system::error_code& ec, std::size_t bytes_transferred)
{
    if (ec || bytes_transferred != 2) {
#ifdef DEBUG
        std::clog << "[Proxy " << m_destPort << "] onHeader error: " << ec.message() << std::endl;
#endif
        return terminate();
    }

    uint16_t packetSize = *(uint16_t*)m_buffer;
    if(m_firstPacket) {
        if(m_buffer[0] == 0x0D && m_buffer[1] == 0x0A) { // PROXY HEADER
            m_firstPacket = false;
            boost::asio::async_read(m_socket, boost::asio::buffer(m_buffer, 26), 
                m_strand.wrap(std::bind(&Proxy::onProxyHeader, shared_from_this(), std::placeholders::_1, std::placeholders::_2)));
            return;
        } else {
            boost::system::error_code ec2;
            m_ip = m_socket.remote_endpoint(ec2).address().to_v4().to_ulong();
        }
    }
    
#ifdef ALLOW_ONLY_HAPROXY
    if(m_firstPacket) {
#ifdef DEBUG
        std::clog << "[Proxy " << m_destPort << "] onHeader, connection not from haproxy" << std::endl;
#endif
        return terminate();        
    }
#endif
    
    if (packetSize < 12 || packetSize > BUFFER_SIZE) {
#ifdef DEBUG
        std::clog << "[Proxy " << m_destPort << "] onHeader invalid size: " << packetSize << std::endl;
#endif
        return terminate();
    }

    m_firstPacket = false;
    boost::asio::async_read(m_socket, boost::asio::buffer(m_buffer, packetSize), 
        m_strand.wrap(std::bind(&Proxy::onPacket, shared_from_this(), std::placeholders::_1, std::placeholders::_2)));
}

void Proxy::onProxyHeader(const boost::system::error_code& ec, std::size_t bytes_transferred)
{
   if (ec || bytes_transferred != 26) {
#ifdef DEBUG
        std::clog << "[Proxy " << m_destPort << "] onProxyHeader error: " << ec.message() << std::endl;
#endif
        return terminate();
    }
    
    // header has 14 bytes
    uint32_t src_addr = *(uint32_t*)&m_buffer[14];
    //uint32_t dst_addr = *(uint32_t*)&m_buffer[18];
    //uint16_t src_port = *(uint32_t*)&m_buffer[22];
    //uint16_t dst_port = *(uint32_t*)&m_buffer[24];
    m_ip = src_addr;
    m_realIP = true;

    auto it = g_connectionsPerIp.emplace(m_ip, 0).first;
    if(it->second > 10) {
        return terminate();
    }
    it->second += 1;
    
    readHeader();
}

void Proxy::onPacket(const boost::system::error_code& ec, std::size_t bytes_transferred)
{
    if (ec || bytes_transferred < 12) {
#ifdef DEBUG
        std::clog << "[Proxy " << m_destPort << "] onPacket error: " << ec.message() << std::endl;
#endif
        return terminate();
    }

    m_lastPacket = std::chrono::high_resolution_clock::now();
    uint32_t sessionId = *(uint32_t*)(&m_buffer[0]);
    uint32_t packetId = *(uint32_t*)(&m_buffer[4]);
    uint32_t lastRecivedPacketId = *(uint32_t*)(&m_buffer[8]);

    if (sessionId == 0) {
        readHeader();
        return sendPing();
    }

    SessionPtr session = nullptr;
    auto it = g_sessions.find(sessionId);
    if (it != g_sessions.end()) { // create session
        session = it->second.lock();
    } else if (packetId == 0) {
#ifdef DEBUG
        std::clog << "[Proxy " << m_destPort << "] creating new session: " << sessionId << std::endl;
#endif
        m_destPort = lastRecivedPacketId;
#ifdef FREE_VERSION
        if(g_activeConnections < 200 && g_activeConnections < 201) {
#endif
            session = std::make_shared<Session>(m_io, m_strand, sessionId, m_ip);
            session->start(m_destPort);
            g_sessions[sessionId] = session;
#ifdef FREE_VERSION
        }
#endif
    }   

    if (!session) {
        readHeader();
        return sendSessionEnd(sessionId);
    }

    if (packetId == 0) {
        readHeader();
        return session->addProxy(shared_from_this());
    }
    else if (packetId == 0xFFFFFFFFu)
    {
        readHeader();
        return session->removeProxy(shared_from_this());
    }

    uint16_t packetSize = *(uint16_t*)(&m_buffer[12]);
    auto packet = std::make_shared<Packet>(m_buffer + 12, m_buffer + 14 + packetSize);
    session->onProxyPacket(packetId, lastRecivedPacketId, packet);
    readHeader();
}

void Proxy::sendPing()
{
    auto packet = std::make_shared<Packet>(14, 0);
    packet->at(0) = 12; // size = 12
    send(packet, true);
}

void Proxy::sendSessionEnd(uint32_t sessionId)
{
#ifdef DEBUG
    std::clog << "[Proxy " << m_destPort << "] sendSessionEnd: " << sessionId << std::endl;
#endif
    auto packet = std::make_shared<Packet>(14, 0);
    packet->at(0) = 12; // size = 12
    *(uint32_t*)(&(packet->data()[2])) = sessionId;
    *(uint32_t*)(&(packet->data()[6])) = 0xFFFFFFFFu;
    send(packet);
}

void Proxy::send(const PacketPtr& packet, bool front)
{
    bool sendNow = m_sendQueue.empty();
    if(front) {
        m_sendQueue.push_front(packet);
    } else {
        m_sendQueue.push_back(packet);
    }
    if (sendNow) {
        boost::asio::async_write(m_socket, boost::asio::buffer(packet->data(), packet->size()), 
            m_strand.wrap(std::bind(&Proxy::onSent, shared_from_this(), packet, std::placeholders::_1, std::placeholders::_2)));
    }
}

void Proxy::onSent(PacketPtr p, const boost::system::error_code& ec, std::size_t bytes_transferred)
{
    if (ec) {
#ifdef DEBUG
        std::clog << "[Proxy " << m_destPort << "] onSent error: " << ec.message() << std::endl;
#endif
        return terminate();
    }

    for(auto it = m_sendQueue.begin(); it != m_sendQueue.end(); ++it) {
        if(*it == p) {
            m_sendQueue.erase(it);
            break;
        }
    }

#ifdef DEBUG
    //std::clog << "[Proxy " << m_destPort << "] onSent bytes: " << bytes_transferred << std::endl;
#endif
    if (!m_sendQueue.empty()) {
		auto packet = m_sendQueue.front();
        boost::asio::async_write(m_socket, boost::asio::buffer(packet->data(), packet->size()),
            m_strand.wrap(std::bind(&Proxy::onSent, shared_from_this(), packet, std::placeholders::_1, std::placeholders::_2)));
    }
}

void Server::run(size_t threads_num)
{
    open();
    std::vector<std::thread> threads;
    for(size_t i = 0; i < threads_num; ++i) {
        threads.emplace_back([&] {
            m_io.run();
        });
    }
    for(auto& thread : threads) {
        thread.join();
    }
}

void Server::open()
{
#ifdef DEBUG
    std::clog << "[Server] open : " << m_port << std::endl;
#endif
    boost::system::error_code ec;
    boost::asio::ip::tcp::endpoint endpoint(boost::asio::ip::address_v4::any(), m_port);
    m_acceptor.close(ec);
    m_acceptor.open(endpoint.protocol(), ec);
    if (!ec) {
        m_acceptor.set_option(boost::asio::ip::tcp::acceptor::reuse_address(true), ec);
        if (!ec) {
            m_acceptor.bind(endpoint, ec);
            if (!ec) {
                m_acceptor.listen(2147483647, ec);
            }
        }
    }
    if (ec) {
        std::cerr << "Server can't bind port " << m_port << ", error: " << ec.message() << std::endl;
        std::cerr << "Retry in 1 second" << std::endl;
        m_timer.expires_from_now(std::chrono::seconds(1));
        m_timer.async_wait(m_strand.wrap(std::bind(&Server::open, this)));
        return;
    }
    m_acceptor.set_option(boost::asio::ip::tcp::no_delay(true));
    accept();
}

void Server::accept()
{
    auto socket = new boost::asio::ip::tcp::socket{ m_io };
    m_acceptor.async_accept(*socket,
        m_strand.wrap([&, socket](boost::system::error_code ec)
        {
            if (ec)
            {
                std::cerr << "Server::accept error: " << ec.message() << std::endl;
                std::cerr << "Did you set system connection limit correctly?" << std::endl;
                std::cerr << "Usually it is 1023, you can change it by using ulimit -n" << std::endl;
                delete socket;
                // try to accept again in 1s
                m_timer.expires_from_now(std::chrono::seconds(1));
                m_timer.async_wait(m_strand.wrap(std::bind(&Server::open, this)));
                return;
            }
#ifdef DEBUG
            std::clog << "[Server] new connection" << std::endl; 
#endif
            boost::system::error_code ecc;
            socket->set_option(boost::asio::socket_base::send_buffer_size(65536), ecc);
            socket->set_option(boost::asio::socket_base::receive_buffer_size(65536), ecc);
#ifdef DEBUG
            if (ecc) {
                std::clog << "[Server] sock error " << ecc.message() << std::endl;
            }
#endif
            auto proxy = std::make_shared<Proxy>(m_io, m_strand, std::move(*socket));
            proxy->start();
            delete socket;
            accept();
        }));
}