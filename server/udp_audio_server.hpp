/***
    udp-music: UDP audio transport server.

    Holds a UDP socket on server.udp_port (default 4100) and streams
    encoded Opus audio chunks to each client that has registered via a
    small "UDPR" registration datagram. Per-client FEC state maintains
    an XOR parity packet every N data packets (group size).

    Control traffic (Hello, Time, CodecHeader, etc.) stays on TCP and
    is untouched by this server.
***/

#pragma once

#include "common/message/pcm_chunk.hpp"
#include "common/message/udp_audio.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/strand.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

class UdpAudioServer
{
public:
    /// 4-byte magic for the client registration datagram
    static constexpr char kRegistrationMagic[4] = {'U', 'D', 'P', 'R'};

    UdpAudioServer(boost::asio::io_context& io_context,
                   const std::string& bind_address,
                   uint16_t port,
                   uint8_t fec_group_size = 4);
    ~UdpAudioServer();

    /// Start receiving client registrations and sending audio.
    void start();
    /// Stop the receive loop.
    void stop();

    /// Fan an encoded audio chunk out to all registered clients.
    /// Called by StreamServer::onChunkEncoded. The payload must be a
    /// single self-contained frame (currently Opus).
    void broadcast(const msg::PcmChunk& chunk);

    /// Number of clients currently registered.
    size_t registeredClients() const;

private:
    using endpoint = boost::asio::ip::udp::endpoint;

    struct ClientState
    {
        endpoint ep;
        uint32_t seq          = 0;
        uint32_t fec_group    = 0;
        uint8_t  fec_idx      = 0;
        uint32_t length_xor   = 0;   // XOR of data payload_lens in this group
        std::vector<uint8_t> xor_accum; // padded-to-max XOR buffer
        size_t   xor_max_len  = 0;
        // stats
        std::atomic<uint32_t> sent_data{0};
        std::atomic<uint32_t> sent_parity{0};
    };

    void startReceive();
    void handleRegistration(const endpoint& from, size_t bytes);
    void sendOneClient(ClientState& cs, const uint8_t* payload, size_t len,
                       uint32_t timestamp_sec, uint32_t timestamp_usec);
    void closeGroup(ClientState& cs, uint32_t timestamp_sec, uint32_t timestamp_usec);

    std::string bind_address_;
    uint16_t port_;
    uint8_t fec_n_;

    boost::asio::ip::udp::socket socket_;
    boost::asio::strand<boost::asio::io_context::executor_type> strand_;

    std::array<uint8_t, 1600> rx_buf_{};
    endpoint rx_endpoint_;

    mutable std::mutex clients_mutex_;
    std::unordered_map<std::string, std::shared_ptr<ClientState>> clients_;

    bool running_ = false;
};
