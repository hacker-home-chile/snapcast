/***
    This file is part of snapcast
    Copyright (C) 2014-2025  Johannes Pohl

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
***/

// prototype/interface header file
#include "stream_server.hpp"

// local headers
#include "common/aixlog.hpp"
#include "config.hpp"
#include "stream_session_tcp.hpp"

// 3rd party headers

// standard headers
#include <iostream>

using namespace std;
using namespace streamreader;

using json = nlohmann::json;

static constexpr auto LOG_TAG = "StreamServer";

StreamServer::StreamServer(boost::asio::io_context& io_context, ServerSettings serverSettings, StreamMessageReceiver* messageReceiver)
    : io_context_(io_context), config_timer_(io_context), settings_(std::move(serverSettings)), messageReceiver_(messageReceiver)
{
}


StreamServer::~StreamServer() = default;


void StreamServer::cleanup()
{
    auto new_end = std::remove_if(sessions_.begin(), sessions_.end(), [](const std::weak_ptr<StreamSession>& session) { return session.expired(); });
    auto count = distance(new_end, sessions_.end());
    if (count > 0)
    {
        LOG(INFO, LOG_TAG) << "Removing " << count << " inactive session(s), active sessions: " << sessions_.size() - count << "\n";
        sessions_.erase(new_end, sessions_.end());
    }
}


void StreamServer::addSession(const std::shared_ptr<StreamSession>& session)
{
    session->setMessageReceiver(this);
    session->setBufferMs(settings_.stream.bufferMs);
    session->start();

    std::lock_guard<std::recursive_mutex> mlock(sessionsMutex_);
    sessions_.emplace_back(session);
    cleanup();
}


void StreamServer::onChunkEncoded(const PcmStream* pcmStream, bool isDefaultStream, const std::shared_ptr<msg::PcmChunk>& chunk, double /*duration*/)
{
    // LOG(TRACE, LOG_TAG) << "onChunkRead (" << pcmStream->getName() << "): " << duration << "ms\n";
    shared_const_buffer buffer(*chunk);

    // make a copy of the sessions to avoid that a session get's deleted
    std::vector<std::shared_ptr<StreamSession>> sessions;
    {
        std::lock_guard<std::recursive_mutex> mlock(sessionsMutex_);
        for (const auto& session : sessions_)
            if (auto s = session.lock())
                sessions.push_back(s);
    }

    for (const auto& session : sessions)
    {
        if (!settings_.stream.sendAudioToMutedClients)
        {
            std::lock_guard<std::mutex> lock(Config::instance().getMutex());
            GroupPtr group = Config::instance().getGroupFromClient(session->clientId);
            if (group)
            {
                if (group->muted)
                {
                    continue;
                }
                else
                {
                    ClientInfoPtr client = group->getClient(session->clientId);
                    if (client && client->config.volume.muted)
                        continue;
                }
            }
        }

        bool shouldSend = (!session->pcmStream() && isDefaultStream) || (session->pcmStream().get() == pcmStream);
        if (shouldSend)
        {
            if (session->udp_endpoint_.has_value() && udp_socket_)
            {
                sendUdp(session, buffer);
            }
            else
            {
                session->send(buffer);
            }
        }
    }
}


void StreamServer::sendUdp(const std::shared_ptr<StreamSession>& session, const shared_const_buffer& buffer)
{
    try
    {
        const auto& data = buffer.message().data;
        if (data.size() < 4)
            return;

        // The base message header begins with: type (bytes 0-1), id (bytes 2-3).
        // We need to patch the id with a per-session UDP sequence number, but
        // the source buffer is shared across sessions and must not be mutated.
        // Instead of copying the entire payload, stage a 4-byte scratch header
        // in the session and send it alongside the rest of the payload using
        // scatter-gather. This keeps the hot path allocation-free.
        uint16_t seq = session->udp_sequence_++;
        session->udp_header_scratch_[0] = data[0];
        session->udp_header_scratch_[1] = data[1];
        session->udp_header_scratch_[2] = static_cast<char>(seq & 0xFF);
        session->udp_header_scratch_[3] = static_cast<char>((seq >> 8) & 0xFF);

        std::array<boost::asio::const_buffer, 2> bufs = {
            boost::asio::buffer(session->udp_header_scratch_.data(), 4),
            boost::asio::buffer(data.data() + 4, data.size() - 4)};

        boost::system::error_code ec;
        udp_socket_->send_to(bufs, *session->udp_endpoint_, 0, ec);
        if (ec)
        {
            LOG(WARNING, LOG_TAG) << "UDP send error to " << session->clientId << ": " << ec.message() << "\n";
            // Clear UDP endpoint so we fall back to TCP
            session->udp_endpoint_.reset();
        }
    }
    catch (const std::exception& e)
    {
        LOG(ERROR, LOG_TAG) << "Exception in sendUdp: " << e.what() << "\n";
        session->udp_endpoint_.reset();
    }
}


void StreamServer::onMessageReceived(const std::shared_ptr<StreamSession>& streamSession, const msg::BaseMessage& baseMessage, char* buffer)
{
    try
    {
        if (messageReceiver_ != nullptr)
            messageReceiver_->onMessageReceived(streamSession, baseMessage, buffer);
    }
    catch (const std::exception& e)
    {
        LOG(ERROR, LOG_TAG) << "Server::onMessageReceived exception: " << e.what() << ", message type: " << baseMessage.type << "\n";
        auto session = getStreamSession(streamSession.get());
        session->stop();
    }
}


void StreamServer::onDisconnect(StreamSession* streamSession)
{
    std::lock_guard<std::recursive_mutex> mlock(sessionsMutex_);
    session_ptr session = getStreamSession(streamSession);

    if (session == nullptr)
        return;

    LOG(INFO, LOG_TAG) << "onDisconnect: " << session->clientId << "\n";
    LOG(DEBUG, LOG_TAG) << "sessions: " << sessions_.size() << "\n";
    sessions_.erase(std::remove_if(sessions_.begin(), sessions_.end(),
                                   [streamSession](const std::weak_ptr<StreamSession>& session)
    {
        auto s = session.lock();
        return s.get() == streamSession;
    }),
                    sessions_.end());
    LOG(DEBUG, LOG_TAG) << "sessions: " << sessions_.size() << "\n";
    if (messageReceiver_ != nullptr)
        messageReceiver_->onDisconnect(streamSession);
    cleanup();
}


session_ptr StreamServer::getStreamSession(StreamSession* streamSession) const
{
    std::lock_guard<std::recursive_mutex> mlock(sessionsMutex_);

    for (const auto& session : sessions_)
    {
        if (auto s = session.lock())
            if (s.get() == streamSession)
                return s;
    }
    return nullptr;
}


session_ptr StreamServer::getStreamSession(const std::string& clientId) const
{
    //	LOG(INFO, LOG_TAG) << "getStreamSession: " << mac << "\n";
    std::lock_guard<std::recursive_mutex> mlock(sessionsMutex_);
    for (const auto& session : sessions_)
    {
        if (auto s = session.lock())
            if (s->clientId == clientId)
                return s;
    }
    return nullptr;
}


void StreamServer::startAccept()
{
    auto accept_handler = [this](error_code ec, tcp::socket socket)
    {
        if (!ec)
            handleAccept(std::move(socket));
        else
            LOG(ERROR, LOG_TAG) << "Error while accepting socket connection: " << ec.message() << "\n";
    };

    for (auto& acceptor : acceptor_)
        acceptor->async_accept(accept_handler);
}


void StreamServer::handleAccept(tcp::socket socket)
{
    try
    {
        struct timeval tv;
        tv.tv_sec = 5;
        tv.tv_usec = 0;
        setsockopt(socket.native_handle(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(socket.native_handle(), SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        /// experimental: turn on tcp::no_delay
        socket.set_option(tcp::no_delay(true));

        LOG(NOTICE, LOG_TAG) << "StreamServer::NewConnection: " << socket.remote_endpoint().address().to_string() << "\n";
        shared_ptr<StreamSession> session = make_shared<StreamSessionTcp>(this, settings_, std::move(socket));
        addSession(session);
    }
    catch (const std::exception& e)
    {
        LOG(ERROR, LOG_TAG) << "Exception in StreamServer::handleAccept: " << e.what() << "\n";
    }
    startAccept();
}


void StreamServer::start()
{
    if (settings_.tcp_stream.enabled)
    {
        for (const auto& address : settings_.tcp_stream.bind_to_address)
        {
            try
            {
                LOG(INFO, LOG_TAG) << "Creating TCP stream acceptor for address: " << address << ", port: " << settings_.tcp_stream.port << "\n";
                acceptor_.emplace_back(make_unique<tcp::acceptor>(boost::asio::make_strand(io_context_.get_executor()),
                                                                  tcp::endpoint(boost::asio::ip::make_address(address), settings_.tcp_stream.port)));
            }
            catch (const boost::system::system_error& e)
            {
                LOG(ERROR, LOG_TAG) << "error creating TCP stream acceptor: " << e.what() << ", code: " << e.code() << "\n";
            }
        }
    }

    // Set up UDP socket for audio streaming
    if (settings_.udp_stream.enabled)
    {
        try
        {
            const auto& address = settings_.udp_stream.bind_to_address.front();
            auto endpoint = boost::asio::ip::udp::endpoint(boost::asio::ip::make_address(address), settings_.udp_stream.port);
            udp_socket_ = std::make_unique<boost::asio::ip::udp::socket>(io_context_, endpoint);
            LOG(INFO, LOG_TAG) << "UDP streaming socket opened on " << address << ":" << settings_.udp_stream.port << "\n";
            startUdpReceive();
        }
        catch (const boost::system::system_error& e)
        {
            LOG(ERROR, LOG_TAG) << "Error creating UDP streaming socket: " << e.what() << ", code: " << e.code() << "\n";
        }
    }

    startAccept();
}


void StreamServer::startUdpReceive()
{
    if (!udp_socket_)
        return;

    udp_socket_->async_receive_from(
        boost::asio::buffer(udp_recv_buffer_), udp_remote_endpoint_,
        [this](const boost::system::error_code& ec, std::size_t bytes_recvd) { handleUdpRegistration(ec, bytes_recvd); });
}


void StreamServer::handleUdpRegistration(const boost::system::error_code& ec, std::size_t bytes_recvd)
{
    if (!ec && bytes_recvd >= 2)
    {
        // Parse registration packet: 2-byte length prefix (little-endian) + clientId string
        uint16_t id_len = static_cast<uint8_t>(udp_recv_buffer_[0]) | (static_cast<uint8_t>(udp_recv_buffer_[1]) << 8);
        if (id_len > 0 && static_cast<size_t>(2 + id_len) <= bytes_recvd)
        {
            std::string clientId(udp_recv_buffer_.data() + 2, id_len);
            LOG(INFO, LOG_TAG) << "UDP registration from clientId: " << clientId << " at " << udp_remote_endpoint_ << "\n";

            std::lock_guard<std::recursive_mutex> mlock(sessionsMutex_);
            for (const auto& weak_session : sessions_)
            {
                if (auto session = weak_session.lock())
                {
                    if (session->clientId == clientId)
                    {
                        session->udp_endpoint_ = udp_remote_endpoint_;
                        // NOTE: do NOT reset udp_sequence_ here. Clients
                        // re-register periodically (every ~5s) to refresh
                        // the NAT mapping; resetting the sequence on every
                        // re-registration desynchronises the client's
                        // reorder buffer and causes it to reject packets
                        // as late. The struct default already initialises
                        // udp_sequence_ to 0 on fresh sessions.
                        LOG(INFO, LOG_TAG) << "UDP endpoint registered for session: " << clientId << "\n";
                        break;
                    }
                }
            }
        }
        else
        {
            LOG(WARNING, LOG_TAG) << "Invalid UDP registration packet, id_len: " << id_len << ", bytes: " << bytes_recvd << "\n";
        }
    }
    else if (ec)
    {
        LOG(ERROR, LOG_TAG) << "UDP receive error: " << ec.message() << "\n";
    }

    // Continue receiving
    startUdpReceive();
}


void StreamServer::stop()
{
    for (auto& acceptor : acceptor_)
        acceptor->cancel();
    acceptor_.clear();

    if (udp_socket_)
    {
        boost::system::error_code ec;
        udp_socket_->close(ec);
        udp_socket_.reset();
    }

    std::lock_guard<std::recursive_mutex> mlock(sessionsMutex_);
    cleanup();
    for (const auto& s : sessions_)
    {
        if (auto session = s.lock())
            session->stop();
    }
}
