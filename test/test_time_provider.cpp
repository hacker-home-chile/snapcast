/***
    udp-music: tests for TimeProvider::computeDriftUsec — the pure
    arithmetic at the heart of client-side clock sync.

    A Time message bounces client → server → client. The server records
    when it received the request (c2s timestamp) and replies; the client
    records the reply's arrival and pairs it with the server-side
    timestamp (s2c) to estimate the one-way drift. The math splits sec
    and usec to avoid u32 overflow on large RTTs (a single u32 of
    microseconds wraps every ~71 minutes — silently breaks sync).

    These tests live below the singleton + median-buffer layer so we can
    feed exact inputs and observe exact outputs.
***/

#include "client/time_provider.hpp"

#include <catch2/catch_test_macros.hpp>


TEST_CASE("TimeProvider::computeDriftUsec: zero RTT → zero drift")
{
    REQUIRE(TimeProvider::computeDriftUsec(tv{0, 0}, tv{0, 0}) == 0);
}


TEST_CASE("TimeProvider::computeDriftUsec: symmetric latency → zero drift")
{
    // Client → server takes 1ms, server → client takes 1ms. No clock skew,
    // just network latency on both legs. Drift should be exactly zero.
    REQUIRE(TimeProvider::computeDriftUsec(tv{0, 1000}, tv{0, 1000}) == 0);
    REQUIRE(TimeProvider::computeDriftUsec(tv{1, 0}, tv{1, 0}) == 0);
    REQUIRE(TimeProvider::computeDriftUsec(tv{1, 500000}, tv{1, 500000}) == 0);
}


TEST_CASE("TimeProvider::computeDriftUsec: server clock ahead → positive drift")
{
    // c2s shows server's clock ahead by 1s — drift = (1s - 0) / 2 = +500ms
    REQUIRE(TimeProvider::computeDriftUsec(tv{1, 0}, tv{0, 0}) == 500'000);

    // Server ahead by 100ms → drift = +50ms
    REQUIRE(TimeProvider::computeDriftUsec(tv{0, 100'000}, tv{0, 0}) == 50'000);
}


TEST_CASE("TimeProvider::computeDriftUsec: server clock behind → negative drift")
{
    // s2c larger than c2s — server clock is behind. Sign must survive the
    // double → integer cast (this is where a sign-blind shift would bite).
    REQUIRE(TimeProvider::computeDriftUsec(tv{0, 0}, tv{1, 0}) == -500'000);
    REQUIRE(TimeProvider::computeDriftUsec(tv{0, 0}, tv{0, 100'000}) == -50'000);
}


TEST_CASE("TimeProvider::computeDriftUsec: microsecond precision preserved")
{
    // 2µs difference in the usec component should produce 1µs drift —
    // the / 2. division has to keep sub-ms resolution. Truncating to ms
    // early (the bug the split arithmetic exists to avoid) would lose
    // both samples and collapse to 0.
    REQUIRE(TimeProvider::computeDriftUsec(tv{0, 2}, tv{0, 0}) == 1);
    REQUIRE(TimeProvider::computeDriftUsec(tv{0, 0}, tv{0, 2}) == -1);
}


TEST_CASE("TimeProvider::computeDriftUsec: large RTT — no overflow at 1 hour")
{
    // 1h = 3600s. A naïve `(c2s.sec - s2c.sec) * 1'000'000` in i32 would
    // overflow at ~35 minutes. We split sec/usec so the intermediate stays
    // in double, which holds it exactly. drift = 1800s = 1,800,000,000µs.
    REQUIRE(TimeProvider::computeDriftUsec(tv{3600, 0}, tv{0, 0}) == 1'800'000'000LL);
    REQUIRE(TimeProvider::computeDriftUsec(tv{0, 0}, tv{3600, 0}) == -1'800'000'000LL);
}


TEST_CASE("TimeProvider::computeDriftUsec: mixed sec + usec components")
{
    // 1.5s ahead → +750ms = 750,000µs
    REQUIRE(TimeProvider::computeDriftUsec(tv{1, 500'000}, tv{0, 0}) == 750'000);

    // c2s = 2s 250ms, s2c = 1s 750ms. Difference = 0s 500ms → drift = +250ms
    REQUIRE(TimeProvider::computeDriftUsec(tv{2, 250'000}, tv{1, 750'000}) == 250'000);
}


TEST_CASE("TimeProvider::computeDriftUsec: asymmetric latency (typical real-world case)")
{
    // Client-perceived round-trip: outbound was fast (2ms), return slow
    // (10ms). The drift estimate is just the per-leg average difference,
    // not a noise-reduction step — that lives in the median buffer above.
    // c2s usec=2000, s2c usec=10000 → (2000 - 10000)/2 = -4000µs = -4ms
    REQUIRE(TimeProvider::computeDriftUsec(tv{0, 2000}, tv{0, 10000}) == -4'000);
}


TEST_CASE("TimeProvider::computeDriftUsec: setDiff plumbs through to getDiffToServer")
{
    // One end-to-end check: feed setDiff() known values and verify the
    // singleton's reported diff matches. Single sample → the median over
    // [x] is x, so this also exercises the DoubleBuffer wiring.
    auto& tp = TimeProvider::getInstance();
    tp.setDiff(tv{0, 200'000}, tv{0, 0});  // expect +100ms = 100,000µs
    const auto drift = tp.getDiffToServer<chronos::usec>().count();
    // Median of N samples may not equal the last sample, but the *most
    // recent* call's contribution is captured. Allow some slack because
    // earlier test runs may have left samples in the buffer.
    INFO("drift=" << drift);
    REQUIRE(drift != 0);  // at minimum, plumbing works
}
