/***
    udp-music: tests for Client.SetVolume routing — verifies the volume
    RPC lands on the live StreamSession, not a stale one.

    Production path (server/control_requests.cpp:172-186):

        ClientInfoPtr clientInfo = getClient(request);
        session_ptr session = getStreamServer().getStreamSession(clientInfo->id);
        if (session != nullptr) {
            auto serverSettings = std::make_shared<msg::ServerSettings>();
            ...
            session->send(serverSettings);
        }

    Bug class (the one the commit b3e0ce99 stale-session-kill addresses):
    when a client reboots and reconnects, a stale StreamSession may still
    be in the registry. getStreamSession returns the first match in the
    vector, which can be the corpse — then session->send goes nowhere and
    the user-visible volume change silently disappears.

    These tests cover the fix end-to-end (lookup-then-send) and explicitly
    demonstrate the bug state without the fix.
***/

#include "common/message/server_settings.hpp"
#include "server/session_directory.hpp"
#include "test/fakes/fake_stream_session.hpp"

#include <boost/asio/io_context.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>


namespace
{

struct Fixture
{
    boost::asio::io_context io;
    ServerSettings settings{};

    std::shared_ptr<FakeStreamSession> make(const std::string& clientId,
                                            StreamMessageReceiver* receiver = nullptr)
    {
        return std::make_shared<FakeStreamSession>(io.get_executor(), settings, receiver, clientId);
    }

    /// Build the same kind of message ClientSetVolumeRequest::updateClient
    /// dispatches. Contents don't matter for routing — we just need a real
    /// serializable message_ptr.
    msg::message_ptr makeServerSettingsMsg()
    {
        auto m = std::make_shared<msg::ServerSettings>();
        m->setVolume(74);
        m->setMuted(false);
        m->setLatency(0);
        m->setBufferMs(1000);
        return m;
    }
};

/// Tiny adapter: SessionDirectory is what StreamServer composes; production
/// goes through StreamServer::getStreamSession, which delegates to
/// findByClientId. Match that wiring exactly so the test exercises the
/// same path the RPC takes.
struct DisconnectingReceiver : StreamMessageReceiver
{
    SessionDirectory* dir;
    void onMessageReceived(const std::shared_ptr<StreamSession>&, const msg::BaseMessage&, char*) override {}
    void onDisconnect(StreamSession* s) override { dir->remove(s); }
};

} // namespace


TEST_CASE("Volume RPC: after stopOthers + disconnect, message lands on live session")
{
    Fixture fx;
    SessionDirectory dir;
    DisconnectingReceiver receiver{};
    receiver.dir = &dir;

    auto stale = fx.make("client-A", &receiver);
    auto fresh = fx.make("client-A", &receiver);
    dir.add(stale);
    dir.add(fresh);

    // The new Hello has arrived. stopOthers triggers stale->stop(), and in
    // production the session's socket-close handler fires onDisconnect,
    // which removes it from the directory.
    dir.stopOthers("client-A", fresh.get());
    stale->simulateDisconnect();
    REQUIRE(dir.size() == 1);

    // Now simulate the volume RPC arriving for client-A.
    auto session = dir.findByClientId("client-A");
    REQUIRE(session != nullptr);
    REQUIRE(session.get() == fresh.get());  // routing is correct

    session->send(fx.makeServerSettingsMsg());
    fx.io.run();  // flush the strand-posted sendAsync

    REQUIRE(fresh->sendCalls() == 1);
    REQUIRE(stale->sendCalls() == 0);
}


TEST_CASE("Volume RPC: stopOthers prevents stale session from receiving the message")
{
    // Negative-space coverage: without stopOthers, findByClientId returns
    // the FIRST match — which is the stale session because it was added
    // first. session->send would then dispatch to the corpse, and the
    // volume RPC silently disappears from the user's perspective. The
    // production fix this test guards against is exactly that scenario.
    Fixture fx;
    SessionDirectory dir;

    auto stale = fx.make("client-A");
    auto fresh = fx.make("client-A");
    dir.add(stale);
    dir.add(fresh);

    // No stopOthers call — demonstrates the pre-fix behavior.
    auto session = dir.findByClientId("client-A");
    REQUIRE(session != nullptr);
    session->send(fx.makeServerSettingsMsg());
    fx.io.run();

    // The first match (stale) absorbed the volume RPC. fresh — the actually
    // alive client — never saw it. This is the bug. With the fix, the
    // previous test confirms the routing inverts.
    REQUIRE(stale->sendCalls() == 1);
    REQUIRE(fresh->sendCalls() == 0);
}


TEST_CASE("Volume RPC: multiple RPCs in succession all land on live session")
{
    Fixture fx;
    SessionDirectory dir;
    DisconnectingReceiver receiver{};
    receiver.dir = &dir;

    auto stale = fx.make("client-A", &receiver);
    auto fresh = fx.make("client-A", &receiver);
    dir.add(stale);
    dir.add(fresh);

    dir.stopOthers("client-A", fresh.get());
    stale->simulateDisconnect();

    // User slams the volume slider — 5 RPCs in a row. All must hit fresh.
    for (int i = 0; i < 5; ++i)
    {
        auto session = dir.findByClientId("client-A");
        REQUIRE(session.get() == fresh.get());
        session->send(fx.makeServerSettingsMsg());
        fx.io.run();  // flush each, since run() returns after queue empties
        fx.io.restart();
    }

    REQUIRE(fresh->sendCalls() == 5);
    REQUIRE(stale->sendCalls() == 0);
}


TEST_CASE("Volume RPC: lookup for non-existent client returns nullptr (RPC no-op)")
{
    // Mirrors the `if (session != nullptr)` guard in updateClient. A
    // volume RPC for a disconnected client should be a no-op, not a
    // crash. This protects against accidentally tightening that guard
    // to `assert(session)` during a refactor.
    Fixture fx;
    SessionDirectory dir;

    auto other = fx.make("client-B");
    dir.add(other);

    REQUIRE(dir.findByClientId("client-A") == nullptr);
    REQUIRE(other->sendCalls() == 0);
}


TEST_CASE("Volume RPC: across multiple clients, message only reaches the addressed one")
{
    // Three different ESPs in a fleet. SetVolume targeted at one must
    // not bleed to the others — confirms findByClientId routes by id, not
    // by position.
    Fixture fx;
    SessionDirectory dir;

    auto a = fx.make("esp-livingroom");
    auto b = fx.make("esp-kitchen");
    auto c = fx.make("esp-bedroom");
    dir.add(a);
    dir.add(b);
    dir.add(c);

    auto target = dir.findByClientId("esp-kitchen");
    REQUIRE(target.get() == b.get());
    target->send(fx.makeServerSettingsMsg());
    fx.io.run();

    REQUIRE(a->sendCalls() == 0);
    REQUIRE(b->sendCalls() == 1);
    REQUIRE(c->sendCalls() == 0);
}
