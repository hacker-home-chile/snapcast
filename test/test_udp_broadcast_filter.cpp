/***
    udp-music: regression test for per-stream UDP fan-out filtering.

    Background: UdpAudioServer::broadcast() is called once per encoded
    chunk per PcmStream. Before the resolver-based filter, broadcast()
    sent every chunk to every registered UDP client, so a server with
    N parallel streams (e.g. the 10-stream channel-slice setup) would
    fan out N × per-stream-rate packets per client, all decoded as
    interleaved garbage by the receiver. This test pins the contract
    that broadcast(stream_id, chunk) only reaches clients whose
    resolver-reported stream id matches stream_id.
***/

#include "server/udp_audio_server.hpp"
#include "common/message/pcm_chunk.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/udp.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>


using endpoint_t = boost::asio::ip::udp::endpoint;

namespace
{

endpoint_t ep(const std::string& addr, uint16_t port)
{
    return endpoint_t(boost::asio::ip::make_address(addr), port);
}

msg::PcmChunk makeChunk()
{
    msg::PcmChunk c;
    // Any non-empty payload triggers the send path; we don't decode it.
    const char* sample = "audio";
    c.payloadSize = 5;
    c.payload = static_cast<char*>(std::malloc(c.payloadSize));
    std::memcpy(c.payload, sample, c.payloadSize);
    c.timestamp.sec = 1;
    c.timestamp.usec = 0;
    return c;
}

// Drain the io_context. broadcast() posts to a strand, so we need to
// run() the context to actually let the send path execute and bump
// sent_data. run_for is bounded so a regression doesn't hang the suite.
void drain(boost::asio::io_context& io)
{
    io.run_for(std::chrono::milliseconds(100));
    io.restart();
}

} // namespace


TEST_CASE("broadcast: routes chunks only to clients on the matching stream")
{
    boost::asio::io_context io;
    // Bind to ephemeral loopback port. We don't read from this socket;
    // we only need it to accept async_send_to.
    UdpAudioServer srv(io, "127.0.0.1", 0, /*fec_group_size=*/4);
    // start() opens & binds the socket and flips running_ true — without
    // it sendOneClient short-circuits and sent_data stays zero.
    srv.start();

    // Three clients in three different groups. Endpoints are arbitrary
    // loopback addresses — the kernel happily accepts the sends; we only
    // assert against the per-client sent_data counter.
    srv.applyRegistration("front",   ep("127.0.0.1", 40001));
    srv.applyRegistration("kitchen", ep("127.0.0.1", 40002));
    srv.applyRegistration("orphan",  ep("127.0.0.1", 40003));

    srv.setClientStreamResolver(
        [](const std::string& clientId) -> std::string
        {
            if (clientId == "front")   return "matrix";
            if (clientId == "kitchen") return "spotify";
            return std::string{}; // "orphan" has no group / no stream
        });

    SECTION("only clients on the broadcast stream receive the chunk")
    {
        auto chunk = makeChunk();
        srv.broadcast("matrix", chunk);
        drain(io);

        REQUIRE(srv.snapshotClient("front")->sent_data   == 1);
        REQUIRE(srv.snapshotClient("kitchen")->sent_data == 0);
        REQUIRE(srv.snapshotClient("orphan")->sent_data  == 0);

        // PcmChunk's destructor frees payload on scope exit.
    }

    SECTION("a different stream id routes to a different client")
    {
        auto chunk = makeChunk();
        srv.broadcast("spotify", chunk);
        drain(io);

        REQUIRE(srv.snapshotClient("front")->sent_data   == 0);
        REQUIRE(srv.snapshotClient("kitchen")->sent_data == 1);
        REQUIRE(srv.snapshotClient("orphan")->sent_data  == 0);

        // PcmChunk's destructor frees payload on scope exit.
    }

    SECTION("clients with no resolvable stream never receive audio")
    {
        // Broadcasting to whatever stream the orphan would be on must
        // still not reach it — an unconfigured client is the canonical
        // 'silent rather than blast garbage' case.
        auto chunk = makeChunk();
        srv.broadcast("", chunk);
        drain(io);

        REQUIRE(srv.snapshotClient("front")->sent_data   == 0);
        REQUIRE(srv.snapshotClient("kitchen")->sent_data == 0);
        REQUIRE(srv.snapshotClient("orphan")->sent_data  == 0);

        // PcmChunk's destructor frees payload on scope exit.
    }
}


TEST_CASE("broadcast: with no resolver installed, fans out to every client")
{
    // Defensive: tests / harnesses that construct a bare UdpAudioServer
    // without a resolver should still see the old broadcast-to-all
    // semantics. Production wires a resolver in Server::start().
    boost::asio::io_context io;
    UdpAudioServer srv(io, "127.0.0.1", 0, /*fec_group_size=*/4);
    // start() opens & binds the socket and flips running_ true — without
    // it sendOneClient short-circuits and sent_data stays zero.
    srv.start();

    srv.applyRegistration("a", ep("127.0.0.1", 41001));
    srv.applyRegistration("b", ep("127.0.0.1", 41002));

    auto chunk = makeChunk();
    srv.broadcast("anything", chunk);
    drain(io);

    REQUIRE(srv.snapshotClient("a")->sent_data == 1);
    REQUIRE(srv.snapshotClient("b")->sent_data == 1);
}
