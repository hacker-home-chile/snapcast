/***
    udp-music: tests for SessionDirectory — the session registry extracted
    from StreamServer. Covers the stale-session-kill behavior added in
    commit b3e0ce99 (server: kill stale sessions on Hello so volume RPCs
    reach live client) plus basic add / remove / lookup invariants.
***/

#include "server/session_directory.hpp"
#include "test/fakes/fake_stream_session.hpp"

#include <boost/asio/io_context.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>


namespace
{

/// Test fixture: io_context for the strand any session needs, plus a
/// minimal ServerSettings (defaults are fine — we never exercise the
/// settings-driven branches).
struct DirFixture
{
    boost::asio::io_context io;
    ServerSettings settings{};

    std::shared_ptr<FakeStreamSession> make(const std::string& clientId,
                                            StreamMessageReceiver* receiver = nullptr)
    {
        return std::make_shared<FakeStreamSession>(io.get_executor(), settings, receiver, clientId);
    }
};

} // namespace


TEST_CASE("SessionDirectory: add then find by clientId")
{
    DirFixture fx;
    SessionDirectory dir;

    auto a = fx.make("alpha");
    auto b = fx.make("bravo");
    dir.add(a);
    dir.add(b);

    REQUIRE(dir.size() == 2);
    REQUIRE(dir.findByClientId("alpha").get() == a.get());
    REQUIRE(dir.findByClientId("bravo").get() == b.get());
    REQUIRE(dir.findByClientId("charlie") == nullptr);
    REQUIRE(dir.findByRawPointer(a.get()).get() == a.get());
    REQUIRE(dir.findByRawPointer(nullptr) == nullptr);
}


TEST_CASE("SessionDirectory: cleanup drops expired weak refs")
{
    DirFixture fx;
    SessionDirectory dir;

    auto a = fx.make("alpha");
    dir.add(a);
    {
        auto tmp = fx.make("temp");
        dir.add(tmp);
        REQUIRE(dir.size() == 2);
    } // tmp expires here

    REQUIRE(dir.cleanup() == 1);
    REQUIRE(dir.size() == 1);
    REQUIRE(dir.findByClientId("temp") == nullptr);
    REQUIRE(dir.findByClientId("alpha").get() == a.get());
}


TEST_CASE("SessionDirectory: remove() drops only the matching session")
{
    DirFixture fx;
    SessionDirectory dir;

    auto a = fx.make("alpha");
    auto b = fx.make("bravo");
    dir.add(a);
    dir.add(b);

    dir.remove(a.get());
    REQUIRE(dir.size() == 1);
    REQUIRE(dir.findByClientId("alpha") == nullptr);
    REQUIRE(dir.findByClientId("bravo").get() == b.get());

    // Removing again is a no-op.
    dir.remove(a.get());
    REQUIRE(dir.size() == 1);
}


// ---------------------------------------------------------------------------
// The crux: stopOthers() is the behavior added in commit b3e0ce99. A new
// Hello from a rebooted client must wipe any prior session bearing the same
// clientId, otherwise volume / ServerSettings RPCs route to the corpse
// (getStreamSession returns the first match in the vector) and the user-
// visible volume change silently disappears.
// ---------------------------------------------------------------------------

TEST_CASE("SessionDirectory: stopOthers stops stale sessions for same clientId, preserves keep")
{
    DirFixture fx;
    SessionDirectory dir;

    auto stale = fx.make("client-A");
    auto fresh = fx.make("client-A");
    auto other = fx.make("client-B");

    dir.add(stale);
    dir.add(fresh);
    dir.add(other);

    const size_t stopped = dir.stopOthers("client-A", fresh.get());

    REQUIRE(stopped == 1);
    REQUIRE(stale->stopped() == 1);
    REQUIRE(fresh->stopped() == 0);
    REQUIRE(other->stopped() == 0);
}


TEST_CASE("SessionDirectory: stopOthers ignores unrelated clientIds")
{
    DirFixture fx;
    SessionDirectory dir;

    auto fresh = fx.make("client-A");
    auto unrelated1 = fx.make("client-B");
    auto unrelated2 = fx.make("client-C");

    dir.add(fresh);
    dir.add(unrelated1);
    dir.add(unrelated2);

    REQUIRE(dir.stopOthers("client-A", fresh.get()) == 0);
    REQUIRE(unrelated1->stopped() == 0);
    REQUIRE(unrelated2->stopped() == 0);
    REQUIRE(fresh->stopped() == 0);
}


TEST_CASE("SessionDirectory: stopOthers handles multiple stale dupes")
{
    DirFixture fx;
    SessionDirectory dir;

    // Pathological: three stale sessions for the same client (e.g. flaky
    // network leaving stale TCP half-connections behind). All must die.
    auto stale1 = fx.make("client-A");
    auto stale2 = fx.make("client-A");
    auto stale3 = fx.make("client-A");
    auto fresh = fx.make("client-A");

    dir.add(stale1);
    dir.add(stale2);
    dir.add(stale3);
    dir.add(fresh);

    REQUIRE(dir.stopOthers("client-A", fresh.get()) == 3);
    REQUIRE(stale1->stopped() == 1);
    REQUIRE(stale2->stopped() == 1);
    REQUIRE(stale3->stopped() == 1);
    REQUIRE(fresh->stopped() == 0);
}


// In production, the session's stop() asynchronously drives onDisconnect on
// the transport layer, which ends up calling SessionDirectory::remove(). We
// can simulate that ordering and confirm lookups end up on the fresh
// session (not the stale corpse) afterwards — the exact bug class the
// commit message describes.
TEST_CASE("SessionDirectory: lookup after stop+disconnect prefers live session")
{
    struct DropReceiver : StreamMessageReceiver
    {
        SessionDirectory* dir;
        void onMessageReceived(const std::shared_ptr<StreamSession>&, const msg::BaseMessage&, char*) override {}
        void onDisconnect(StreamSession* s) override
        {
            dir->remove(s);
        }
    };

    DirFixture fx;
    SessionDirectory dir;
    DropReceiver receiver{};
    receiver.dir = &dir;

    auto stale = fx.make("client-A", &receiver);
    auto fresh = fx.make("client-A", &receiver);
    dir.add(stale);
    dir.add(fresh);

    // Before cleanup: first-match wins — could be either, but documents
    // why the cleanup matters.
    auto before = dir.findByClientId("client-A");
    REQUIRE(before != nullptr);

    dir.stopOthers("client-A", fresh.get());
    stale->simulateDisconnect(); // production: socket close handler runs

    REQUIRE(dir.size() == 1);
    REQUIRE(dir.findByClientId("client-A").get() == fresh.get());
}
