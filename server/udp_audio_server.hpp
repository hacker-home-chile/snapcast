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
#include "udp_client_presence.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/strand.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

class UdpAudioServer : public UdpClientPresence
{
public:
    /// 4-byte magic for the client registration datagram
    static constexpr char kRegistrationMagic[4] = {'U', 'D', 'P', 'R'};

    /// Outcome of applyRegistration().
    enum class RegistrationOutcome
    {
        Inserted,        ///< brand-new client
        EndpointChanged, ///< existing client moved IP/port — transport state was reset
        Unchanged,       ///< existing client re-registered from the same endpoint (no-op)
    };

    /// Snapshot of a registered client's transport state. Returned by
    /// snapshotClient() for diagnostics and tests; values are a copy and
    /// will not change as the live state evolves.
    struct ClientSnapshot
    {
        boost::asio::ip::udp::endpoint endpoint;
        uint32_t seq;
        uint32_t fec_group;
        uint8_t  fec_idx;
        uint32_t length_xor;
        size_t   xor_max_len;
        uint32_t sent_data;
        uint32_t sent_parity;
    };

    UdpAudioServer(boost::asio::io_context& io_context,
                   const std::string& bind_address,
                   uint16_t port,
                   uint8_t fec_group_size = 4);
    ~UdpAudioServer();

    /// Start receiving client registrations and sending audio.
    void start();
    /// Stop the receive loop.
    void stop();

    /// Resolves a clientId → the stream id the client is currently bound
    /// to (via its group), or "" if the client is unknown. Used by
    /// broadcast() to send each chunk only to clients on the originating
    /// stream — without this filter, every PcmStream's chunks fan out to
    /// every UDP client and interleave on the wire (10 streams × 50 pps
    /// = 500 pps per client, all decoded as garbage).
    using ClientStreamResolver = std::function<std::string(const std::string& clientId)>;

    /// Install the resolver used by broadcast() for per-client stream
    /// filtering. If no resolver is set (e.g. unit tests), broadcast()
    /// falls back to sending every chunk to every client.
    void setClientStreamResolver(ClientStreamResolver resolver);

    /// Fan an encoded audio chunk for @p stream_id out to every UDP
    /// client whose resolver-reported stream matches. Called by
    /// Server::onChunkEncoded. The payload must be a single
    /// self-contained frame (currently Opus).
    void broadcast(const std::string& stream_id, const msg::PcmChunk& chunk);

    /// Number of clients currently registered.
    size_t registeredClients() const;

    /// True if @p client_id has registered for UDP audio. Used by
    /// StreamServer to skip redundant TCP WireChunk sends to UDP clients —
    /// doubling up the audio wastes WiFi airtime and adds jitter on the
    /// ESP without any functional benefit (our ESP drops TCP WireChunks).
    bool hasClient(const std::string& client_id) const override;

    using endpoint = boost::asio::ip::udp::endpoint;

    /// Parse a raw registration datagram (4-byte "UDPR" magic + clientId
    /// payload, trailing NULs/spaces trimmed). Pure function, public so it
    /// can be exercised in unit tests without a socket. Returns nullopt for
    /// any malformed input (too short, wrong magic, empty clientId).
    static std::optional<std::string> parseRegistration(const uint8_t* data, size_t bytes);

    /// Register @p client_id at @p ep, or update an existing entry if the
    /// endpoint changed (the lwIP/ESP client anchors to the first seq it
    /// sees, so we must zero the transport state on roaming or it chases
    /// a stale high counter). Idempotent: same-endpoint re-registration
    /// returns Unchanged and touches nothing. Public for testing — the
    /// receive callback uses it under the hood.
    RegistrationOutcome applyRegistration(const std::string& client_id, const endpoint& ep);

    /// Snapshot of @p client_id's current transport state, or nullopt if
    /// not registered. Useful for diagnostics and tests that need to assert
    /// the roaming reset zeroed the right fields.
    std::optional<ClientSnapshot> snapshotClient(const std::string& client_id) const;

    /// Per-client transport / FEC state. Public so the pure FEC helpers
    /// below can be exercised in unit tests; production code touches it
    /// only via the methods on this class.
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

    /// Description of an FEC group that has just been filled by an
    /// appendFecData() call. The parity payload is exactly `parity.size()`
    /// bytes (= max payload length seen in the group, shorter payloads
    /// zero-padded into the XOR), and would be sent on the wire with a
    /// header where `aux = length_xor` and `fec_group = group`.
    struct FecGroupClose
    {
        std::vector<uint8_t> parity;
        uint32_t length_xor;
        uint32_t fec_group;
    };

    /// Append one data packet's payload to @p cs's FEC accumulator. Pure
    /// (no socket, no logging). When @p fec_n packets have accumulated this
    /// returns the parity bytes and resets the group; otherwise nullopt.
    ///
    /// Crucially: the per-group invariant for receivers is that XOR(all
    /// data payloads, parity) == 0, with shorter payloads zero-padded out
    /// to the longest. `length_xor` is the XOR of all the truncated-to-u16
    /// payload lengths in the group, used by receivers to recover the
    /// true length of a missing packet.
    static std::optional<FecGroupClose> appendFecData(
        ClientState& cs, const uint8_t* payload, size_t len, uint8_t fec_n);

private:

    void startReceive();
    void handleRegistration(const endpoint& from, size_t bytes);
    void sendOneClient(ClientState& cs, const uint8_t* payload, size_t len,
                       uint32_t timestamp_sec, uint32_t timestamp_usec);
    void sendParityPacket(ClientState& cs, const FecGroupClose& close,
                          uint32_t timestamp_usec);

    std::string bind_address_;
    uint16_t port_;
    uint8_t fec_n_;

    boost::asio::ip::udp::socket socket_;
    boost::asio::strand<boost::asio::io_context::executor_type> strand_;

    std::array<uint8_t, 1600> rx_buf_{};
    endpoint rx_endpoint_;

    mutable std::mutex clients_mutex_;
    std::unordered_map<std::string, std::shared_ptr<ClientState>> clients_;

    mutable std::mutex resolver_mutex_;
    ClientStreamResolver client_stream_resolver_;

    bool running_ = false;
};
