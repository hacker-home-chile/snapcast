/***
    udp-music: tests for ActiveSelector — the snapshot-then-apply pattern
    that MetaStream's per-property RPCs (setVolume, setMute, setRate, etc.)
    should use to route to the currently-active child stream without
    racing the active-stream switch.

    Current MetaStream uses TWO different mutexes for the read (setVolume:
    PcmStream::mutex_) and the write (switch_stream: active_mutex_), so a
    concurrent volume RPC during a stream switch can land on the wrong
    target or crash the worker thread. These tests pin down the safe
    behavior so adopting this helper would close that hole.
***/

#include "server/streamreader/active_selector.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <memory>
#include <thread>
#include <vector>


namespace
{

/// Tiny stand-in for a PcmStream — records every setVolume call so the
/// test can verify routing.
struct FakeStream
{
    std::atomic<int> volume_calls{0};
    int last_volume = -1;

    void setVolume(int v)
    {
        ++volume_calls;
        last_volume = v;
    }
};

} // namespace


TEST_CASE("ActiveSelector: get on empty returns nullptr")
{
    ActiveSelector<FakeStream> sel;
    REQUIRE(sel.get() == nullptr);
}


TEST_CASE("ActiveSelector: set then get returns the same pointer")
{
    ActiveSelector<FakeStream> sel;
    auto s = std::make_shared<FakeStream>();
    sel.set(s);
    REQUIRE(sel.get().get() == s.get());
}


TEST_CASE("ActiveSelector: applyToActive routes to currently-active element")
{
    ActiveSelector<FakeStream> sel;
    auto a = std::make_shared<FakeStream>();
    auto b = std::make_shared<FakeStream>();
    sel.set(a);

    REQUIRE(sel.applyToActive([](FakeStream& s) { s.setVolume(50); }));
    REQUIRE(a->volume_calls == 1);
    REQUIRE(a->last_volume == 50);
    REQUIRE(b->volume_calls == 0);

    // Switch active and confirm subsequent calls re-route.
    sel.set(b);
    REQUIRE(sel.applyToActive([](FakeStream& s) { s.setVolume(75); }));
    REQUIRE(b->volume_calls == 1);
    REQUIRE(b->last_volume == 75);
    REQUIRE(a->volume_calls == 1);  // unchanged
}


TEST_CASE("ActiveSelector: applyToActive on nullptr active returns false (no crash)")
{
    // Mirrors the `if (session != nullptr)` style guard. A volume RPC for
    // a meta-stream whose children are all stopped should be a no-op, not
    // a nullptr deref.
    ActiveSelector<FakeStream> sel;
    bool fn_ran = false;
    REQUIRE_FALSE(sel.applyToActive([&](FakeStream&) { fn_ran = true; }));
    REQUIRE_FALSE(fn_ran);

    // After explicitly setting to nullptr (the "stop everything" path),
    // same behavior.
    sel.set(std::make_shared<FakeStream>());
    sel.set(nullptr);
    REQUIRE_FALSE(sel.applyToActive([](FakeStream&) {}));
}


TEST_CASE("ActiveSelector: get() snapshot survives a concurrent set()")
{
    // The crux of the snapshot-then-apply pattern: even if active_stream_
    // is swapped between get() returning and the caller dereferencing it,
    // the shared_ptr keeps the original alive. Without this, MetaStream's
    // setVolume could read active_stream_, get preempted by switch_stream,
    // then call setVolume on a destroyed object.
    ActiveSelector<FakeStream> sel;
    auto first = std::make_shared<FakeStream>();
    sel.set(first);

    auto snapshot = sel.get();
    sel.set(std::make_shared<FakeStream>());  // swap underway
    first.reset();                            // release our owner
    sel.set(nullptr);                         // selector drops its own ref to "first"

    // snapshot is the only thing keeping the original alive — but it IS
    // keeping it alive, and calling on it must not crash.
    snapshot->setVolume(42);
    REQUIRE(snapshot->volume_calls == 1);
    REQUIRE(snapshot->last_volume == 42);
}


TEST_CASE("ActiveSelector: applyToActive does not deadlock on re-entry")
{
    // The internal lock must NOT be held while @p fn runs, otherwise an fn
    // that calls back into the selector (e.g. to read get() for a sanity
    // check, or to setActive(nullptr) as a side-effect of stop) would
    // deadlock the worker thread.
    ActiveSelector<FakeStream> sel;
    sel.set(std::make_shared<FakeStream>());

    bool reentry_ok = false;
    sel.applyToActive([&](FakeStream&)
    {
        // Re-enter — would deadlock if applyToActive held mutex_.
        auto inner = sel.get();
        reentry_ok = (inner != nullptr);
        sel.set(std::make_shared<FakeStream>());  // also a write, also must not deadlock
    });
    REQUIRE(reentry_ok);
}


TEST_CASE("ActiveSelector: concurrent set + applyToActive — no torn reads, no crash")
{
    // Stress-style smoke test under TSan/ASan: a writer thread swaps the
    // active element 1000× while a reader thread applies a no-op closure
    // 1000×. We don't assert on which element the reader saw — only that
    // every call landed on SOME valid element (no nullptr deref, no
    // torn shared_ptr read).
    ActiveSelector<FakeStream> sel;
    sel.set(std::make_shared<FakeStream>());

    std::atomic<bool> stop{false};
    std::atomic<int> reads_with_valid_target{0};

    std::thread writer([&]
    {
        for (int i = 0; i < 1000 && !stop.load(); ++i)
            sel.set(std::make_shared<FakeStream>());
    });
    std::thread reader([&]
    {
        for (int i = 0; i < 1000 && !stop.load(); ++i)
        {
            if (sel.applyToActive([](FakeStream& s) { s.setVolume(7); }))
                ++reads_with_valid_target;
        }
    });

    writer.join();
    reader.join();
    stop.store(true);

    // Every read landed on a valid stream — would fail (or ASan would
    // fire) if the shared_ptr read were torn under the swap.
    REQUIRE(reads_with_valid_target == 1000);
}
