/***
    Tests for ChannelSliceStream — the "virtual stream" that subscribes to a
    parent PcmStream and forwards a de-interleaved subset of its channels.

    Focuses on the pure de-interleave path (static helper) and the
    higher-level invariants the plan calls out: order preservation, frame
    count preservation, sample-width independence. The full PcmStream
    runtime is heavyweight (encoder factory + control script + properties
    timer + io_context), so we deliberately exercise only what's reachable
    without instantiating the whole chain.
***/

#include "server/streamreader/channel_slice_stream.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>


using streamreader::ChannelSliceStream;


namespace
{

// Build a synthetic interleaved PCM buffer where each frame carries a
// per-channel tag: frame f, channel c => sample value = (f * 100) + c.
// 16-bit samples (s16le) — the common case.
std::vector<std::int16_t> makeInterleavedS16(std::uint16_t channels, std::size_t frames)
{
    std::vector<std::int16_t> out(channels * frames);
    for (std::size_t f = 0; f < frames; ++f)
        for (std::uint16_t c = 0; c < channels; ++c)
            out[f * channels + c] = static_cast<std::int16_t>(f * 100 + c);
    return out;
}

} // namespace


TEST_CASE("ChannelSliceStream::deinterleave: picks single channel from stereo (s16)")
{
    constexpr std::uint16_t kChannels = 2;
    constexpr std::size_t kFrames = 8;
    auto in = makeInterleavedS16(kChannels, kFrames);

    std::vector<std::uint8_t> indices{1}; // right channel only
    std::vector<std::int16_t> out(kFrames);

    ChannelSliceStream::deinterleave(reinterpret_cast<const char*>(in.data()), kFrames, kChannels, sizeof(std::int16_t), indices,
                                     reinterpret_cast<char*>(out.data()));

    for (std::size_t f = 0; f < kFrames; ++f)
        REQUIRE(out[f] == static_cast<std::int16_t>(f * 100 + 1));
}


TEST_CASE("ChannelSliceStream::deinterleave: picks center channel from 6-channel layout")
{
    constexpr std::uint16_t kChannels = 6;
    constexpr std::size_t kFrames = 4;
    auto in = makeInterleavedS16(kChannels, kFrames);

    std::vector<std::uint8_t> indices{2}; // center
    std::vector<std::int16_t> out(kFrames * indices.size());

    ChannelSliceStream::deinterleave(reinterpret_cast<const char*>(in.data()), kFrames, kChannels, sizeof(std::int16_t), indices,
                                     reinterpret_cast<char*>(out.data()));

    for (std::size_t f = 0; f < kFrames; ++f)
        REQUIRE(out[f] == static_cast<std::int16_t>(f * 100 + 2));
}


TEST_CASE("ChannelSliceStream::deinterleave: picks front pair [0,1] from 6-channel layout")
{
    constexpr std::uint16_t kChannels = 6;
    constexpr std::size_t kFrames = 4;
    auto in = makeInterleavedS16(kChannels, kFrames);

    std::vector<std::uint8_t> indices{0, 1};
    std::vector<std::int16_t> out(kFrames * indices.size());

    ChannelSliceStream::deinterleave(reinterpret_cast<const char*>(in.data()), kFrames, kChannels, sizeof(std::int16_t), indices,
                                     reinterpret_cast<char*>(out.data()));

    for (std::size_t f = 0; f < kFrames; ++f)
    {
        REQUIRE(out[f * 2 + 0] == static_cast<std::int16_t>(f * 100 + 0));
        REQUIRE(out[f * 2 + 1] == static_cast<std::int16_t>(f * 100 + 1));
    }
}


TEST_CASE("ChannelSliceStream::deinterleave: picks surround pair [4,5]")
{
    constexpr std::uint16_t kChannels = 6;
    constexpr std::size_t kFrames = 3;
    auto in = makeInterleavedS16(kChannels, kFrames);

    std::vector<std::uint8_t> indices{4, 5};
    std::vector<std::int16_t> out(kFrames * indices.size());

    ChannelSliceStream::deinterleave(reinterpret_cast<const char*>(in.data()), kFrames, kChannels, sizeof(std::int16_t), indices,
                                     reinterpret_cast<char*>(out.data()));

    for (std::size_t f = 0; f < kFrames; ++f)
    {
        REQUIRE(out[f * 2 + 0] == static_cast<std::int16_t>(f * 100 + 4));
        REQUIRE(out[f * 2 + 1] == static_cast<std::int16_t>(f * 100 + 5));
    }
}


TEST_CASE("ChannelSliceStream::deinterleave: preserves index order [5,4] reverses channels")
{
    // The plan explicitly calls out that index order matters — channels=5,4
    // produces L=parent[5], R=parent[4]. Verifies the per-frame copy loop
    // walks `indices` in order rather than sorting them.
    constexpr std::uint16_t kChannels = 6;
    constexpr std::size_t kFrames = 2;
    auto in = makeInterleavedS16(kChannels, kFrames);

    std::vector<std::uint8_t> indices{5, 4};
    std::vector<std::int16_t> out(kFrames * indices.size());

    ChannelSliceStream::deinterleave(reinterpret_cast<const char*>(in.data()), kFrames, kChannels, sizeof(std::int16_t), indices,
                                     reinterpret_cast<char*>(out.data()));

    for (std::size_t f = 0; f < kFrames; ++f)
    {
        REQUIRE(out[f * 2 + 0] == static_cast<std::int16_t>(f * 100 + 5));
        REQUIRE(out[f * 2 + 1] == static_cast<std::int16_t>(f * 100 + 4));
    }
}


TEST_CASE("ChannelSliceStream::deinterleave: 24-bit (3-byte) sample width")
{
    // Sample-width independence: the helper takes sample_size in bytes.
    // Pack three distinct byte values per sample so we can verify each
    // channel's full sample is copied as a unit.
    constexpr std::uint16_t kChannels = 4;
    constexpr std::size_t kFrames = 2;
    constexpr std::uint16_t kSampleSize = 3;

    std::vector<std::uint8_t> in(kChannels * kFrames * kSampleSize);
    for (std::size_t f = 0; f < kFrames; ++f)
    {
        for (std::uint16_t c = 0; c < kChannels; ++c)
        {
            auto* p = in.data() + (f * kChannels + c) * kSampleSize;
            p[0] = static_cast<std::uint8_t>(0xA0 | c);
            p[1] = static_cast<std::uint8_t>(0xB0 | f);
            p[2] = static_cast<std::uint8_t>(0xC0 | (c + f));
        }
    }

    std::vector<std::uint8_t> indices{2, 0};
    std::vector<std::uint8_t> out(kFrames * indices.size() * kSampleSize);

    ChannelSliceStream::deinterleave(reinterpret_cast<const char*>(in.data()), kFrames, kChannels, kSampleSize, indices,
                                     reinterpret_cast<char*>(out.data()));

    for (std::size_t f = 0; f < kFrames; ++f)
    {
        for (std::size_t outc = 0; outc < indices.size(); ++outc)
        {
            const auto srcc = indices[outc];
            auto* expected = in.data() + (f * kChannels + srcc) * kSampleSize;
            auto* got = out.data() + (f * indices.size() + outc) * kSampleSize;
            REQUIRE(std::memcmp(got, expected, kSampleSize) == 0);
        }
    }
}


TEST_CASE("ChannelSliceStream::deinterleave: zero frames is a no-op")
{
    // Defensive: an empty parent chunk (e.g. flushed encoder) must not
    // touch the output buffer or read past the input.
    std::array<char, 0> in{};
    std::array<char, 0> out{};
    std::vector<std::uint8_t> indices{0, 1};
    ChannelSliceStream::deinterleave(in.data(), 0, 2, 2, indices, out.data());
    // No crash, nothing to assert beyond reaching here.
    SUCCEED();
}


TEST_CASE("ChannelSliceStream::deinterleave: output size = frames * indices.size() * sample_size")
{
    // Sanity for callers sizing the destination buffer. Verifies by
    // writing into a sentinel-padded buffer that exactly the expected
    // byte range is touched.
    constexpr std::uint16_t kChannels = 3;
    constexpr std::size_t kFrames = 5;
    auto in = makeInterleavedS16(kChannels, kFrames);

    std::vector<std::uint8_t> indices{0, 2};
    constexpr std::uint8_t kSentinel = 0xEE;
    std::vector<std::uint8_t> out(kFrames * indices.size() * sizeof(std::int16_t) + 16, kSentinel);

    ChannelSliceStream::deinterleave(reinterpret_cast<const char*>(in.data()), kFrames, kChannels, sizeof(std::int16_t), indices,
                                     reinterpret_cast<char*>(out.data()));

    // Trailing bytes after the expected payload must still be the sentinel.
    const std::size_t written = kFrames * indices.size() * sizeof(std::int16_t);
    for (std::size_t i = written; i < out.size(); ++i)
        REQUIRE(out[i] == kSentinel);
}
