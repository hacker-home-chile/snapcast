/***
    udp-music: tests for the TCP↔UDP fan-out dedup decision used by
    StreamServer::onChunkEncoded. Guards against the regression class fixed
    in commit 4b8156fe — an intermediate version on this branch short-
    circuited the TCP fan-out entirely when UDP was enabled, breaking
    stock TCP-only snapclients (e.g. the ledfx-feeder sidecar).
***/

#include "server/udp_client_presence.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <unordered_set>


namespace
{

/// Hand-rolled fake — keeps the test free of UdpAudioServer (which would
/// drag in a socket + strand).
class FakeUdpPresence : public UdpClientPresence
{
public:
    void add(const std::string& clientId) { registered_.insert(clientId); }
    bool hasClient(const std::string& clientId) const override
    {
        return registered_.count(clientId) != 0;
    }
private:
    std::unordered_set<std::string> registered_;
};

} // namespace


TEST_CASE("isUdpRegistered: nullptr presence returns false (TCP-only build)")
{
    // Stock snapserver build (no UDP transport configured) must never claim
    // a client is UDP-registered. This is the path stock TCP-only clients
    // take and the path the regression broke.
    REQUIRE_FALSE(isUdpRegistered(nullptr, "any-client"));
    REQUIRE_FALSE(isUdpRegistered(nullptr, ""));
}


TEST_CASE("isUdpRegistered: routes through the presence interface")
{
    FakeUdpPresence p;
    p.add("esp-001");
    p.add("esp-002");

    REQUIRE(isUdpRegistered(&p, "esp-001"));
    REQUIRE(isUdpRegistered(&p, "esp-002"));
    REQUIRE_FALSE(isUdpRegistered(&p, "esp-003"));      // not registered
    REQUIRE_FALSE(isUdpRegistered(&p, ""));             // empty id
    REQUIRE_FALSE(isUdpRegistered(&p, "ESP-001"));      // case-sensitive
}


TEST_CASE("isUdpRegistered: mixed fleet — only registered clients are skipped")
{
    // The actual fan-out invariant: in a fleet that mixes UDP-registered ESP
    // clients and stock TCP-only clients (ledfx-feeder, an old snapclient),
    // ONLY the UDP-registered ones are skipped. Everything else must
    // continue to receive TCP WireChunks.
    FakeUdpPresence p;
    p.add("esp-livingroom");
    p.add("esp-kitchen");

    // What the fan-out loop in onChunkEncoded would see:
    struct Client { std::string id; bool expect_skipped; };
    const std::vector<Client> fleet = {
        {"esp-livingroom",   true},   // UDP — skip TCP
        {"esp-kitchen",      true},   // UDP — skip TCP
        {"ledfx-feeder",     false},  // stock TCP, must keep receiving
        {"laptop-snapclient",false},  // upstream snapclient, must keep receiving
        {"",                 false},  // empty / unrelated
    };

    for (const auto& c : fleet)
    {
        const bool skipped = isUdpRegistered(&p, c.id);
        INFO("clientId=" << c.id);
        REQUIRE(skipped == c.expect_skipped);
    }
}
