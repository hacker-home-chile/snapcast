/***
    udp-music: UDP audio transport server implementation.
***/

#include "udp_audio_server.hpp"

#include "common/aixlog.hpp"

#include <boost/asio/bind_executor.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>

static constexpr auto LOG_TAG = "UdpAudio";

using boost::asio::ip::udp;

UdpAudioServer::UdpAudioServer(boost::asio::io_context& io_context,
                               const std::string& bind_address,
                               uint16_t port,
                               uint8_t fec_group_size)
    : bind_address_(bind_address),
      port_(port),
      fec_n_(fec_group_size ? fec_group_size : 4),
      socket_(io_context),
      strand_(boost::asio::make_strand(io_context.get_executor()))
{
}

UdpAudioServer::~UdpAudioServer()
{
    try { stop(); } catch (...) {}
}

void UdpAudioServer::start()
{
    boost::system::error_code ec;
    auto addr = boost::asio::ip::make_address(bind_address_, ec);
    if (ec)
    {
        LOG(ERROR, LOG_TAG) << "invalid bind address '" << bind_address_ << "': " << ec.message() << "\n";
        return;
    }
    udp::endpoint bind_ep(addr, port_);
    socket_.open(bind_ep.protocol(), ec);
    if (ec) { LOG(ERROR, LOG_TAG) << "open: " << ec.message() << "\n"; return; }
    socket_.set_option(boost::asio::socket_base::reuse_address(true));
    socket_.bind(bind_ep, ec);
    if (ec)
    {
        LOG(ERROR, LOG_TAG) << "bind " << bind_address_ << ":" << port_ << " failed: " << ec.message() << "\n";
        return;
    }
    // Large send buffer — tolerate bursts on busy hosts.
    boost::system::error_code ignored;
    socket_.set_option(boost::asio::socket_base::send_buffer_size(256 * 1024), ignored);

    LOG(INFO, LOG_TAG) << "listening on " << bind_address_ << ":" << port_
                       << " fec_group_size=" << static_cast<int>(fec_n_) << "\n";
    running_ = true;
    startReceive();
}

void UdpAudioServer::stop()
{
    running_ = false;
    boost::system::error_code ec;
    socket_.close(ec);
}

size_t UdpAudioServer::registeredClients() const
{
    std::lock_guard<std::mutex> lk(clients_mutex_);
    return clients_.size();
}

bool UdpAudioServer::hasClient(const std::string& client_id) const
{
    std::lock_guard<std::mutex> lk(clients_mutex_);
    return clients_.find(client_id) != clients_.end();
}

void UdpAudioServer::startReceive()
{
    if (!running_) return;
    socket_.async_receive_from(
        boost::asio::buffer(rx_buf_), rx_endpoint_,
        boost::asio::bind_executor(strand_,
            [this](const boost::system::error_code& ec, size_t bytes)
            {
                if (!running_) return;
                if (ec)
                {
                    if (ec != boost::asio::error::operation_aborted)
                        LOG(WARNING, LOG_TAG) << "recv error: " << ec.message() << "\n";
                    if (running_) startReceive();
                    return;
                }
                handleRegistration(rx_endpoint_, bytes);
                startReceive();
            }));
}

std::optional<std::string> UdpAudioServer::parseRegistration(const uint8_t* data, size_t bytes)
{
    // Registration frame: 4-byte magic "UDPR" + client_id (up to 255 bytes,
    // no terminator required).
    if (data == nullptr) return std::nullopt;
    if (bytes < sizeof(kRegistrationMagic) + 1) return std::nullopt;
    if (std::memcmp(data, kRegistrationMagic, sizeof(kRegistrationMagic)) != 0)
        return std::nullopt;

    size_t id_len = bytes - sizeof(kRegistrationMagic);
    if (id_len > 255) id_len = 255;
    std::string client_id(reinterpret_cast<const char*>(data + sizeof(kRegistrationMagic)), id_len);
    // Trim trailing NULs / whitespace.
    while (!client_id.empty() && (client_id.back() == '\0' || client_id.back() == ' '))
        client_id.pop_back();
    if (client_id.empty()) return std::nullopt;
    return client_id;
}

UdpAudioServer::RegistrationOutcome UdpAudioServer::applyRegistration(const std::string& client_id, const endpoint& from)
{
    std::lock_guard<std::mutex> lk(clients_mutex_);
    auto it = clients_.find(client_id);
    if (it == clients_.end())
    {
        auto cs = std::make_shared<ClientState>();
        cs->ep = from;
        clients_.emplace(client_id, cs);
        LOG(INFO, LOG_TAG) << "registered client '" << client_id << "' at "
                           << from.address().to_string() << ":" << from.port() << "\n";
        return RegistrationOutcome::Inserted;
    }
    if (it->second->ep == from)
        return RegistrationOutcome::Unchanged;

    // Endpoint change = client rebooted / roamed. Reset transport state
    // so the ESP, which anchors g_next_expected_seq to the first seq it
    // sees, doesn't end up chasing a stale high counter from before.
    LOG(INFO, LOG_TAG) << "client '" << client_id << "' endpoint updated to "
                       << from.address().to_string() << ":" << from.port()
                       << ", resetting transport state\n";
    auto& cs = *it->second;
    cs.ep          = from;
    cs.seq         = 0;
    cs.fec_group   = 0;
    cs.fec_idx     = 0;
    cs.length_xor  = 0;
    cs.xor_max_len = 0;
    std::fill(cs.xor_accum.begin(), cs.xor_accum.end(), 0);
    return RegistrationOutcome::EndpointChanged;
}

std::optional<UdpAudioServer::ClientSnapshot> UdpAudioServer::snapshotClient(const std::string& client_id) const
{
    std::lock_guard<std::mutex> lk(clients_mutex_);
    auto it = clients_.find(client_id);
    if (it == clients_.end()) return std::nullopt;
    const auto& cs = *it->second;
    ClientSnapshot snap{};
    snap.endpoint     = cs.ep;
    snap.seq          = cs.seq;
    snap.fec_group    = cs.fec_group;
    snap.fec_idx      = cs.fec_idx;
    snap.length_xor   = cs.length_xor;
    snap.xor_max_len  = cs.xor_max_len;
    snap.sent_data    = cs.sent_data.load(std::memory_order_relaxed);
    snap.sent_parity  = cs.sent_parity.load(std::memory_order_relaxed);
    return snap;
}

void UdpAudioServer::handleRegistration(const endpoint& from, size_t bytes)
{
    auto client_id = parseRegistration(rx_buf_.data(), bytes);
    if (!client_id) return;
    applyRegistration(*client_id, from);
}

void UdpAudioServer::setClientStreamResolver(ClientStreamResolver resolver)
{
    std::lock_guard<std::mutex> lk(resolver_mutex_);
    client_stream_resolver_ = std::move(resolver);
}

void UdpAudioServer::broadcast(const std::string& stream_id, const msg::PcmChunk& chunk)
{
    if (chunk.payloadSize == 0 || chunk.payload == nullptr) return;

    // Carry the chunk's full (sec, usec) timestamp — sec goes in `aux` for
    // data packets, usec in the 32-bit `timestamp_us` field (0..999999). A
    // single u32 of μs would wrap every ~71 minutes and wreck the client's
    // sync arithmetic.
    const uint32_t ts_sec = static_cast<uint32_t>(chunk.timestamp.sec);
    const uint32_t ts_usec = static_cast<uint32_t>(chunk.timestamp.usec);

    // Snapshot (clientId, state) under the clients mutex so the strand
    // task doesn't race a registration/removal.
    std::vector<std::pair<std::string, std::shared_ptr<ClientState>>> targets;
    {
        std::lock_guard<std::mutex> lk(clients_mutex_);
        targets.reserve(clients_.size());
        for (auto& kv : clients_) targets.emplace_back(kv.first, kv.second);
    }
    if (targets.empty()) return;

    ClientStreamResolver resolver;
    {
        std::lock_guard<std::mutex> lk(resolver_mutex_);
        resolver = client_stream_resolver_;
    }

    const auto* payload = reinterpret_cast<const uint8_t*>(chunk.payload);
    const size_t len = static_cast<size_t>(chunk.payloadSize);

    // Boost asio UDP isn't thread-safe — serialize sends on our strand.
    boost::asio::post(strand_,
        [this, targets = std::move(targets), resolver = std::move(resolver),
         stream_id, data = std::vector<uint8_t>(payload, payload + len), ts_sec, ts_usec]() mutable
        {
            for (auto& kv : targets)
            {
                if (resolver)
                {
                    const auto cs_stream = resolver(kv.first);
                    // Empty resolved id == unknown client (no group yet).
                    // Skip unconditionally: blasting audio to a client
                    // that hasn't picked a stream just wastes airtime,
                    // even if the broadcast stream_id is itself empty.
                    if (cs_stream.empty() || cs_stream != stream_id)
                        continue;
                }
                sendOneClient(*kv.second, data.data(), data.size(), ts_sec, ts_usec);
            }
        });
}

std::optional<UdpAudioServer::FecGroupClose> UdpAudioServer::appendFecData(
    ClientState& cs, const uint8_t* payload, size_t len, uint8_t fec_n)
{
    // Grow the XOR buffer to fit the longest payload seen in this group.
    // Shorter payloads contribute their bytes plus implicit zero padding,
    // which is exactly what receivers reconstruct on recovery.
    if (len > cs.xor_max_len) cs.xor_max_len = len;
    if (cs.xor_accum.size() < cs.xor_max_len)
        cs.xor_accum.resize(cs.xor_max_len, 0);
    for (size_t i = 0; i < len; ++i)
        cs.xor_accum[i] ^= payload[i];
    cs.length_xor ^= static_cast<uint32_t>(static_cast<uint16_t>(len));
    cs.fec_idx++;

    if (cs.fec_idx < fec_n)
        return std::nullopt;

    FecGroupClose out;
    out.parity.assign(cs.xor_accum.begin(), cs.xor_accum.begin() + cs.xor_max_len);
    out.length_xor = cs.length_xor;
    out.fec_group  = cs.fec_group;

    // Reset group ready for the next batch.
    cs.fec_idx = 0;
    cs.length_xor = 0;
    cs.xor_max_len = 0;
    std::fill(cs.xor_accum.begin(), cs.xor_accum.end(), 0);
    cs.fec_group++;
    return out;
}

void UdpAudioServer::sendOneClient(ClientState& cs, const uint8_t* payload, size_t len,
                                   uint32_t timestamp_sec, uint32_t timestamp_usec)
{
    if (!running_) return;

    // Build data packet.
    auto buf = std::make_shared<std::vector<uint8_t>>();
    buf->resize(msg::kUdpAudioHeaderSize + len);

    msg::UdpAudioHeader h{};
    h.magic          = msg::kUdpAudioMagic;
    h.version        = msg::kUdpAudioVersion;
    h.flags          = 0;
    h.fec_group_size = fec_n_;
    h.seq            = cs.seq++;
    h.fec_group      = cs.fec_group;
    h.timestamp_us   = timestamp_usec;
    h.payload_len    = static_cast<uint16_t>(len);
    h.reserved0      = 0;
    h.aux            = timestamp_sec;
    msg::encodeUdpAudioHeader(buf->data(), h);
    std::memcpy(buf->data() + msg::kUdpAudioHeaderSize, payload, len);

    auto ep = cs.ep;
    socket_.async_send_to(boost::asio::buffer(*buf), ep,
        boost::asio::bind_executor(strand_,
            [buf](const boost::system::error_code& ec, size_t)
            {
                if (ec && ec != boost::asio::error::operation_aborted)
                    LOG(TRACE, LOG_TAG) << "send err: " << ec.message() << "\n";
            }));
    cs.sent_data.fetch_add(1, std::memory_order_relaxed);

    if (auto close = appendFecData(cs, payload, len, fec_n_))
        sendParityPacket(cs, *close, timestamp_usec);
}

void UdpAudioServer::sendParityPacket(ClientState& cs, const FecGroupClose& close,
                                      uint32_t timestamp_usec)
{
    const size_t plen = close.parity.size();
    auto buf = std::make_shared<std::vector<uint8_t>>();
    buf->resize(msg::kUdpAudioHeaderSize + plen);

    msg::UdpAudioHeader h{};
    h.magic          = msg::kUdpAudioMagic;
    h.version        = msg::kUdpAudioVersion;
    h.flags          = msg::kUdpFlagIsParity;
    h.fec_group_size = fec_n_;
    h.seq            = cs.seq++;
    h.fec_group      = close.fec_group;
    h.timestamp_us   = timestamp_usec;
    h.payload_len    = static_cast<uint16_t>(plen);
    h.reserved0      = 0;
    h.aux            = close.length_xor;  // parity packets use aux for length_xor
    msg::encodeUdpAudioHeader(buf->data(), h);
    if (plen > 0)
        std::memcpy(buf->data() + msg::kUdpAudioHeaderSize, close.parity.data(), plen);

    auto ep = cs.ep;
    socket_.async_send_to(boost::asio::buffer(*buf), ep,
        boost::asio::bind_executor(strand_,
            [buf](const boost::system::error_code& ec, size_t)
            {
                if (ec && ec != boost::asio::error::operation_aborted)
                    LOG(TRACE, LOG_TAG) << "parity err: " << ec.message() << "\n";
            }));
    cs.sent_parity.fetch_add(1, std::memory_order_relaxed);
}
