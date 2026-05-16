/***
    udp-music: tests for UdpAudioServer registration parsing and the
    endpoint-roaming reset. The receive callback is exercised indirectly by
    going through parseRegistration() + applyRegistration(), which lets us
    cover the parsing and state-transition logic without binding a real
    socket.
***/

#include "server/udp_audio_server.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/udp.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>


using endpoint_t = boost::asio::ip::udp::endpoint;

namespace
{

/// Build a registration datagram payload from a clientId.
std::vector<uint8_t> makeDatagram(const std::string& clientId, bool with_magic = true)
{
    std::vector<uint8_t> out;
    if (with_magic)
    {
        out.insert(out.end(),
                   std::begin(UdpAudioServer::kRegistrationMagic),
                   std::end(UdpAudioServer::kRegistrationMagic));
    }
    out.insert(out.end(), clientId.begin(), clientId.end());
    return out;
}

endpoint_t ep(const std::string& addr, uint16_t port)
{
    return endpoint_t(boost::asio::ip::make_address(addr), port);
}

} // namespace


// ---------------------------------------------------------------------------
// parseRegistration — pure function, no socket, no state.
// ---------------------------------------------------------------------------

TEST_CASE("parseRegistration: accepts valid frame and trims trailing NUL/space")
{
    auto buf = makeDatagram("esp-001");
    auto id = UdpAudioServer::parseRegistration(buf.data(), buf.size());
    REQUIRE(id.has_value());
    REQUIRE(*id == "esp-001");

    // Trailing nulls (some lwIP clients pad to fixed-width buffers).
    auto padded = makeDatagram(std::string("esp-002") + std::string(8, '\0'));
    auto id2 = UdpAudioServer::parseRegistration(padded.data(), padded.size());
    REQUIRE(id2.has_value());
    REQUIRE(*id2 == "esp-002");

    // Trailing spaces.
    auto spaced = makeDatagram("esp-003   ");
    auto id3 = UdpAudioServer::parseRegistration(spaced.data(), spaced.size());
    REQUIRE(id3.has_value());
    REQUIRE(*id3 == "esp-003");
}


TEST_CASE("parseRegistration: rejects malformed input")
{
    SECTION("nullptr")
    {
        REQUIRE_FALSE(UdpAudioServer::parseRegistration(nullptr, 10).has_value());
    }
    SECTION("too short — magic only, no clientId")
    {
        const uint8_t magic_only[] = {'U', 'D', 'P', 'R'};
        REQUIRE_FALSE(UdpAudioServer::parseRegistration(magic_only, sizeof(magic_only)).has_value());
    }
    SECTION("too short — less than magic")
    {
        const uint8_t partial[] = {'U', 'D'};
        REQUIRE_FALSE(UdpAudioServer::parseRegistration(partial, sizeof(partial)).has_value());
    }
    SECTION("wrong magic")
    {
        auto buf = makeDatagram("esp-001", /*with_magic=*/false);
        // prepend wrong magic
        std::vector<uint8_t> wrong = {'X', 'X', 'X', 'X'};
        wrong.insert(wrong.end(), buf.begin(), buf.end());
        REQUIRE_FALSE(UdpAudioServer::parseRegistration(wrong.data(), wrong.size()).has_value());
    }
    SECTION("magic + only padding (no real clientId)")
    {
        auto buf = makeDatagram(std::string(16, '\0'));
        REQUIRE_FALSE(UdpAudioServer::parseRegistration(buf.data(), buf.size()).has_value());
    }
}


TEST_CASE("parseRegistration: clamps oversized clientId at 255 chars")
{
    // Don't trust the network — even though clients are well-behaved today,
    // a 64KB UDP datagram of all-A's mustn't allocate a 64KB std::string
    // here. The parse caps clientId length at 255.
    const std::string huge(8192, 'A');
    auto buf = makeDatagram(huge);
    auto id = UdpAudioServer::parseRegistration(buf.data(), buf.size());
    REQUIRE(id.has_value());
    REQUIRE(id->size() == 255);
    REQUIRE(id->find_first_not_of('A') == std::string::npos);
}


// ---------------------------------------------------------------------------
// applyRegistration — touches clients_ map; requires a UdpAudioServer
// instance but does NOT need start() (we never bind the socket).
// ---------------------------------------------------------------------------

TEST_CASE("applyRegistration: inserts new client; second same-endpoint is a no-op")
{
    boost::asio::io_context io;
    UdpAudioServer server(io, "0.0.0.0", 0);

    const auto ep_a = ep("10.0.0.5", 4100);
    REQUIRE(server.applyRegistration("esp-001", ep_a) == UdpAudioServer::RegistrationOutcome::Inserted);
    REQUIRE(server.hasClient("esp-001"));
    REQUIRE(server.registeredClients() == 1);

    REQUIRE(server.applyRegistration("esp-001", ep_a) == UdpAudioServer::RegistrationOutcome::Unchanged);
    REQUIRE(server.registeredClients() == 1);
}


TEST_CASE("applyRegistration: endpoint change resets transport state (roaming)")
{
    boost::asio::io_context io;
    UdpAudioServer server(io, "0.0.0.0", 0);

    server.applyRegistration("esp-001", ep("10.0.0.5", 4100));

    // Simulate the client having been broadcasting for a while: poke the
    // ClientState via a fresh registration from the same endpoint, then
    // verify the snapshot starts from zero. (We can't mutate seq directly
    // from outside, so we just check the post-reset invariants below.)
    auto before = server.snapshotClient("esp-001");
    REQUIRE(before.has_value());
    REQUIRE(before->endpoint == ep("10.0.0.5", 4100));

    // Now the client reboots and shows up from a new IP — the lwIP/ESP
    // sequence-anchoring bug described in udp_audio_server.cpp:132-148 is
    // exactly what this reset prevents.
    const auto ep_b = ep("10.0.0.6", 4100);
    REQUIRE(server.applyRegistration("esp-001", ep_b) == UdpAudioServer::RegistrationOutcome::EndpointChanged);

    auto after = server.snapshotClient("esp-001");
    REQUIRE(after.has_value());
    REQUIRE(after->endpoint == ep_b);
    REQUIRE(after->seq == 0);
    REQUIRE(after->fec_group == 0);
    REQUIRE(after->fec_idx == 0);
    REQUIRE(after->length_xor == 0);
    REQUIRE(after->xor_max_len == 0);

    // Client count is unchanged — still one entry, just at a new endpoint.
    REQUIRE(server.registeredClients() == 1);
}


TEST_CASE("applyRegistration: port-only change also counts as a roam")
{
    // Same IP, different source port (NAT rebinding scenario). Still a
    // distinct endpoint and the client may have reset its counters, so we
    // must reset ours.
    boost::asio::io_context io;
    UdpAudioServer server(io, "0.0.0.0", 0);

    REQUIRE(server.applyRegistration("esp-001", ep("10.0.0.5", 4100)) == UdpAudioServer::RegistrationOutcome::Inserted);
    REQUIRE(server.applyRegistration("esp-001", ep("10.0.0.5", 4101)) == UdpAudioServer::RegistrationOutcome::EndpointChanged);
    REQUIRE(server.snapshotClient("esp-001")->endpoint.port() == 4101);
}


TEST_CASE("applyRegistration: multiple clients are independent")
{
    boost::asio::io_context io;
    UdpAudioServer server(io, "0.0.0.0", 0);

    server.applyRegistration("esp-001", ep("10.0.0.5", 4100));
    server.applyRegistration("esp-002", ep("10.0.0.6", 4100));
    server.applyRegistration("esp-003", ep("10.0.0.7", 4100));

    REQUIRE(server.registeredClients() == 3);
    REQUIRE(server.hasClient("esp-001"));
    REQUIRE(server.hasClient("esp-002"));
    REQUIRE(server.hasClient("esp-003"));

    // Roaming one client doesn't disturb the others.
    server.applyRegistration("esp-002", ep("10.0.0.66", 4100));
    REQUIRE(server.snapshotClient("esp-001")->endpoint == ep("10.0.0.5", 4100));
    REQUIRE(server.snapshotClient("esp-002")->endpoint == ep("10.0.0.66", 4100));
    REQUIRE(server.snapshotClient("esp-003")->endpoint == ep("10.0.0.7", 4100));
    REQUIRE(server.registeredClients() == 3);
}


TEST_CASE("snapshotClient: returns nullopt for unknown client")
{
    boost::asio::io_context io;
    UdpAudioServer server(io, "0.0.0.0", 0);
    REQUIRE_FALSE(server.snapshotClient("never-registered").has_value());
}
