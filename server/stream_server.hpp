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

#pragma once


// local headers
#include "common/message/message.hpp"
#include "common/queue.hpp"
#include "control_server.hpp"
#include "server_settings.hpp"
#include "session_directory.hpp"
#include "stream_session.hpp"

#include "udp_client_presence.hpp"

// 3rd party headers
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

// standard headers
#include <memory>
#include <mutex>
#include <vector>


using namespace streamreader;

using boost::asio::ip::tcp;
using acceptor_ptr = std::unique_ptr<tcp::acceptor>;
using session_ptr = std::shared_ptr<StreamSession>;


/// Forwars PCM data to the connected clients
/**
 * Reads PCM data from several StreamSessions
 * Accepts and holds client connections (StreamSession)
 * Receives (via the StreamMessageReceiver interface) and answers messages from the clients
 * Forwards PCM data to the clients
 */
class StreamServer : public StreamMessageReceiver
{
public:
    /// c'tor
    StreamServer(boost::asio::io_context& io_context, ServerSettings serverSettings, StreamMessageReceiver* messageReceiver = nullptr);
    /// d'tor
    virtual ~StreamServer();

    /// Start accepting connections
    void start();
    /// Stop accepting connections and active sessions
    void stop();

    /// Send a message to all connceted clients
    //	void send(const msg::BaseMessage* message);

    /// Add a new stream session
    void addSession(const std::shared_ptr<StreamSession>& session);
    /// Callback for chunks that are ready to be sent
    void onChunkEncoded(const PcmStream* pcmStream, bool isDefaultStream, const std::shared_ptr<msg::PcmChunk>& chunk, double duration);

    /// udp-music: register a UdpClientPresence (UdpAudioServer in production)
    /// so the TCP fan-out can skip WireChunk sends to clients receiving
    /// audio over UDP. Optional — if unset, all TCP sessions receive audio
    /// (stock TCP-only behavior).
    void setUdpAudioServer(const UdpClientPresence* udp) { udp_audio_server_ = udp; }

    /// True when @p clientId is currently receiving audio over UDP and
    /// therefore should be skipped in the TCP WireChunk fan-out. Nullsafe
    /// against the "no UDP transport configured" case.
    bool isUdpRegistered(const std::string& clientId) const;

    /// @return stream session for @p clientId
    session_ptr getStreamSession(const std::string& clientId) const;
    /// @return stream session for @p session
    session_ptr getStreamSession(StreamSession* session) const;

    /// udp-music: stop every session that claims @p clientId except @p keep.
    /// Called when a fresh Hello arrives so Client.SetVolume / ServerSettings
    /// pushes can't land in a half-dead pre-reboot session that lwIP hasn't
    /// fully torn down yet (race between client RST and getStreamSession's
    /// first-match lookup).
    void stopOtherSessions(const std::string& clientId, StreamSession* keep);

private:
    void startAccept();
    void handleAccept(tcp::socket socket);
    void cleanup();

    /// Implementation of StreamMessageReceiver
    void onMessageReceived(const std::shared_ptr<StreamSession>& streamSession, const msg::BaseMessage& baseMessage, char* buffer) override;
    void onDisconnect(StreamSession* streamSession) override;

    SessionDirectory sessions_;
    boost::asio::io_context& io_context_;
    std::vector<acceptor_ptr> acceptor_;
    boost::asio::steady_timer config_timer_;

    ServerSettings settings_;
    Queue<std::shared_ptr<msg::BaseMessage>> messages_;
    StreamMessageReceiver* messageReceiver_;
    const UdpClientPresence* udp_audio_server_ = nullptr;
};
