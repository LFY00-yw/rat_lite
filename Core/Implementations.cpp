#include "stdafx.h"
#include <deque>
#include <thread>
#include "Packet.h"
#include "Socket.h"
#include "Listener.h"
#include <boost/asio.hpp>
#include <mutex>
#include "CommonNetwork.h"
#include <atomic>
#include <iostream>

struct SL::Remote_Access_Library::Network::SocketImpl {
	SocketImpl(SocketImpl&& impl) : io_service(std::move(impl.io_service)), socket(std::move(impl.socket)), NetworkEvents_(std::move(impl.NetworkEvents_)) {}
	SocketImpl(boost::asio::io_service& io, boost::asio::ip::tcp::socket& s, NetworkEvents& netevents) : io_service(io), socket(std::move(s)), NetworkEvents_(netevents), PendingBytestobesent(0) {}
	SocketImpl(boost::asio::io_service& io, NetworkEvents& netevents) : io_service(io), socket(io), NetworkEvents_(netevents), PendingBytestobesent(0) {}

	boost::asio::io_service& io_service;
	boost::asio::ip::tcp::socket socket;

	NetworkEvents NetworkEvents_;
	PacketHeader PacketHeader;
	std::deque<std::shared_ptr<Packet>> OutgoingPackets;
	std::shared_ptr<Packet> IncomingPacket;
	std::atomic<size_t> PendingBytestobesent;

};
class io_runner {
public:
	boost::asio::io_service io_service;

	io_runner():work(io_service) {
		io_servicethread = std::thread([this]() {
			std::cout << "Starting io_service" << std::endl;
			io_service.run();
			std::cout << "stopping io_service" << std::endl;
		});
	}
	~io_runner() {
		io_service.stop();
		io_servicethread.detach();//it will stop eventually, but we dont need to wait for that...
	}
private:
	std::thread io_servicethread;
	boost::asio::io_service::work work;
};
void do_connect(std::shared_ptr<SL::Remote_Access_Library::Network::Socket> ptr, SL::Remote_Access_Library::Network::SocketImpl* impl, boost::asio::ip::tcp::resolver::iterator endpoint_iterator);
void do_read_header(std::shared_ptr<SL::Remote_Access_Library::Network::Socket> ptr, SL::Remote_Access_Library::Network::SocketImpl* impl);
void do_write(std::shared_ptr<SL::Remote_Access_Library::Network::Socket> ptr, SL::Remote_Access_Library::Network::SocketImpl* impl);
void do_read_body(std::shared_ptr<SL::Remote_Access_Library::Network::Socket> ptr, SL::Remote_Access_Library::Network::SocketImpl* impl);


SL::Remote_Access_Library::Network::Socket::Socket(SocketImpl* impl) : _SocketImpl(impl) {
	std::cout << "Socket()" << std::endl;
}
SL::Remote_Access_Library::Network::Socket::~Socket() {
	std::cout << "~Socket()" << std::endl;
	delete _SocketImpl;
}

std::shared_ptr<SL::Remote_Access_Library::Network::Socket> SL::Remote_Access_Library::Network::Socket::ConnectTo(const char* host, const char* port, NetworkEvents& netevents, long timeout_in_s) {
	//first thread in will initialize this and start the io_service
	static io_runner _io_runner;

	boost::asio::ip::tcp::resolver resolver(_io_runner.io_service);
	auto socketimpl = new SocketImpl(_io_runner.io_service, netevents);
	auto socket = std::make_shared<SL::Remote_Access_Library::Network::Socket>(socketimpl);
	auto addrs = resolver.resolve({ host,port });

	do_connect(socket, socketimpl, addrs);
	return socket;
}

void SL::Remote_Access_Library::Network::Socket::send(std::shared_ptr<Packet> pack)
{
	auto self(shared_from_this());
	_SocketImpl->io_service.post([this, pack, self]()
	{
		_SocketImpl->PendingBytestobesent += getSize(pack->get_Blk());
		bool write_in_progress = !_SocketImpl->OutgoingPackets.empty();
		_SocketImpl->OutgoingPackets.push_back(pack);
		if (!write_in_progress)
		{
			do_write(self, _SocketImpl);
		}
	});
}

void SL::Remote_Access_Library::Network::Socket::close()
{
	auto self(shared_from_this());
	_SocketImpl->NetworkEvents_.OnClose(self);
	_SocketImpl->io_service.post([self, this]() {
		_SocketImpl->socket.close();
	});
}

size_t SL::Remote_Access_Library::Network::Socket::get_PendingOutgoingBytes() const
{
	return size_t();
}


void do_connect(std::shared_ptr<SL::Remote_Access_Library::Network::Socket> ptr, SL::Remote_Access_Library::Network::SocketImpl* impl, boost::asio::ip::tcp::resolver::iterator endpoint_iterator) {
	boost::asio::async_connect(impl->socket, endpoint_iterator, [ptr, impl](boost::system::error_code ec, boost::asio::ip::tcp::resolver::iterator)
	{
		if (!impl->socket.is_open()) return;
		
		if (!ec) {
			impl->NetworkEvents_.OnConnect(ptr);
			do_read_header(ptr, impl);
		}
		else ptr->close();
	});
}


void do_read_header(std::shared_ptr<SL::Remote_Access_Library::Network::Socket> ptr, SL::Remote_Access_Library::Network::SocketImpl* impl) {

	boost::asio::async_read(impl->socket, boost::asio::buffer(&impl->PacketHeader, sizeof(impl->PacketHeader)), [ptr, impl](boost::system::error_code ec, std::size_t /*length*/)
	{
		if (!ec) do_read_body(ptr, impl);
		else ptr->close();
	});
}
void do_read_body(std::shared_ptr<SL::Remote_Access_Library::Network::Socket> ptr, SL::Remote_Access_Library::Network::SocketImpl* impl) {
	impl->IncomingPacket = SL::Remote_Access_Library::Network::Packet::CreatePacket(impl->PacketHeader.Packet_Type, impl->PacketHeader.PayloadLen);
	auto p(impl->IncomingPacket->get_Payload());
	auto size(impl->IncomingPacket->get_Payloadsize());
	boost::asio::async_read(impl->socket, boost::asio::buffer(p, size), [ptr, impl](boost::system::error_code ec, std::size_t /*length*/)
	{
		if (!ec) {
			impl->NetworkEvents_.OnReceive(ptr, impl->IncomingPacket);
			do_read_header(ptr, impl);
		}
		else ptr->close();
	});
}

void do_write(std::shared_ptr<SL::Remote_Access_Library::Network::Socket> ptr, SL::Remote_Access_Library::Network::SocketImpl* impl) {
	auto blk(impl->OutgoingPackets.front()->get_Blk());
	boost::asio::async_write(impl->socket, boost::asio::buffer(getData(blk), getSize(blk)), [ptr, impl](boost::system::error_code ec, std::size_t byteswritten)
	{
		if (!ec)
		{
			impl->PendingBytestobesent -= byteswritten;
			impl->OutgoingPackets.pop_front();
			if (!impl->OutgoingPackets.empty())
			{
				do_write(ptr, impl);
			}
		}
		else ptr->close();
	});
}


////////////////////////SERVER HERE

struct SL::Remote_Access_Library::Network::ListinerImpl {
	ListinerImpl(const boost::asio::ip::tcp::endpoint& endpoint, NetworkEvents& netevents) :
		NetworkEvents_(netevents),
		acceptor_(io_service, endpoint),
		socket_(io_service) {
		io_servicethread = std::thread([this]() {
			std::cout << "Starting io_service for server" << std::endl;
			io_service.run();
			std::cout << "stopping io_service for server" << std::endl;
		});
	}

	boost::asio::io_service io_service;
	boost::asio::ip::tcp::acceptor acceptor_;
	boost::asio::ip::tcp::socket socket_;
	NetworkEvents NetworkEvents_;
	std::thread io_servicethread;
};

void do_accept(std::shared_ptr<SL::Remote_Access_Library::Network::Listener> ptr, SL::Remote_Access_Library::Network::ListinerImpl* impl) {
	impl->acceptor_.async_accept(impl->socket_, [ptr, impl](boost::system::error_code ec)
	{
		if (!ec)
		{
			auto socketimpl = new SL::Remote_Access_Library::Network::SocketImpl(impl->io_service, std::move(impl->socket_), impl->NetworkEvents_);
			auto sock = std::make_shared<SL::Remote_Access_Library::Network::Socket>(socketimpl);
			impl->NetworkEvents_.OnConnect(sock);
			do_read_header(sock, socketimpl);
		}
		do_accept(ptr, impl);
	});
}

SL::Remote_Access_Library::Network::Listener::Listener(unsigned short port, NetworkEvents& netevents)
{
	_ListinerImpl = new SL::Remote_Access_Library::Network::ListinerImpl(boost::asio::ip::tcp::endpoint(boost::asio::ip::tcp::v4(), port), netevents);
}
SL::Remote_Access_Library::Network::Listener::~Listener() {
	delete _ListinerImpl;
}
void SL::Remote_Access_Library::Network::Listener::Start() {
	do_accept(shared_from_this(), _ListinerImpl);
}
void SL::Remote_Access_Library::Network::Listener::Stop() {
	_ListinerImpl->io_service.stop();
}