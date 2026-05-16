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
#include "udp_client_presence.hpp"

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
    auto removed = sessions_.cleanup();
    if (removed > 0)
        LOG(INFO, LOG_TAG) << "Removed " << removed << " inactive session(s), active sessions: " << sessions_.size() << "\n";
}


void StreamServer::addSession(const std::shared_ptr<StreamSession>& session)
{
    session->setMessageReceiver(this);
    session->setBufferMs(settings_.stream.bufferMs);
    session->start();
    sessions_.add(session);
}


void StreamServer::onChunkEncoded(const PcmStream* pcmStream, bool isDefaultStream, const std::shared_ptr<msg::PcmChunk>& chunk, double /*duration*/)
{
    // udp-music: BOTH TCP and UDP transports now carry audio. The UDP
    // broadcast happens in Server::onChunkEncoded; the TCP fan-out below
    // stays live so stock snapclients (e.g., the Alpine pkg that feeds
    // LedFx over a FIFO) keep working unchanged. The tradeoff is that
    // every chunk goes out twice for UDP-registered clients, but since
    // those clients simply don't consume WireChunks they only pay the
    // upload bandwidth — still cheap on LAN.
    // LOG(TRACE, LOG_TAG) << "onChunkRead (" << pcmStream->getName() << "): " << duration << "ms\n";
    shared_const_buffer buffer(*chunk);

    // snapshot keeps the iteration safe even if a session disconnects mid-fan-out
    auto sessions = sessions_.snapshot();

    for (const auto& session : sessions)
    {
        // udp-music: don't TCP-send audio to clients that are receiving it
        // over UDP. Doubling up wastes WiFi airtime, adds per-chunk jitter
        // on the ESP's LwIP thread, and forces the ESP to drain + discard
        // every WireChunk. Skipping keeps TCP quiet for UDP clients while
        // still letting stock TCP clients (e.g., the ledfx-feeder
        // sidecar) receive their audio normally.
        if (isUdpRegistered(session->clientId))
            continue;

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

        if (!session->pcmStream() && isDefaultStream) //->getName() == "default")
            session->send(buffer);
        else if (session->pcmStream().get() == pcmStream)
            session->send(buffer);
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
    session_ptr session = sessions_.findByRawPointer(streamSession);
    if (session == nullptr)
        return;

    LOG(INFO, LOG_TAG) << "onDisconnect: " << session->clientId << "\n";
    sessions_.remove(streamSession);
    if (messageReceiver_ != nullptr)
        messageReceiver_->onDisconnect(streamSession);
}


session_ptr StreamServer::getStreamSession(StreamSession* streamSession) const
{
    return sessions_.findByRawPointer(streamSession);
}


void StreamServer::stopOtherSessions(const std::string& clientId, StreamSession* keep)
{
    sessions_.stopOthers(clientId, keep);
}


bool StreamServer::isUdpRegistered(const std::string& clientId) const
{
    return ::isUdpRegistered(udp_audio_server_, clientId);
}


session_ptr StreamServer::getStreamSession(const std::string& clientId) const
{
    return sessions_.findByClientId(clientId);
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

    startAccept();
}


void StreamServer::stop()
{
    for (auto& acceptor : acceptor_)
        acceptor->cancel();
    acceptor_.clear();

    sessions_.cleanup();
    sessions_.stopAll();
}
