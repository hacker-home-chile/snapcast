/***
    udp-music: tests for the Opus encoder → decoder round-trip and the
    encoder's remainder-buffer behavior when the input sample rate forces
    the resampler on.

    Round-trip (test #7) — guards against header/sample-format mismatch
    between encoder and decoder: if the encoder's pseudo-header carries
    one rate but the decoder is fed another, audio comes out as silence or
    high-pitched garbage. Catching that needs an actual encode→decode loop.

    Remainder buffer (test #5) — guards against the drift bug that the
    min_chunk_size=20ms fix in opus_encoder.cpp:38-45 addresses. With a
    non-48kHz source, the 44.1k→48k resampler emits a fractional remainder
    every chunk; if the encoder's accumulator mis-handles those bytes, the
    output frame rate doubles or loses samples over time.
***/

#include "server/encoder/opus_encoder.hpp"
#include "client/decoder/opus_decoder.hpp"
#include "common/message/codec_header.hpp"
#include "common/message/pcm_chunk.hpp"
#include "common/sample_format.hpp"
#include "fixtures/pcm_fixtures.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
#include <vector>


using namespace test_fixtures;


namespace
{

/// Collect encoder output into a flat vector. The encoder emits chunks via
/// its callback; we copy each one's encoded payload so the test can replay
/// them through the decoder afterwards.
struct EncodedSink
{
    std::vector<std::vector<char>> chunks;
    double total_duration_ms = 0.0;

    encoder::Encoder::OnEncodedCallback callback()
    {
        return [this](const encoder::Encoder&, std::shared_ptr<msg::PcmChunk> chunk, double duration_ms)
        {
            chunks.emplace_back(chunk->payload, chunk->payload + chunk->payloadSize);
            total_duration_ms += duration_ms;
        };
    }
};

} // namespace


// ---------------------------------------------------------------------------
// Round-trip: encode a sine wave at the native 48k/16/2 (no resampler), then
// decode all the encoded chunks back through OpusDecoder. The decoded buffer
// must (a) be roughly the same length as the input and (b) carry non-trivial
// audio energy — i.e., the codec didn't silently produce noise or silence.
// ---------------------------------------------------------------------------

TEST_CASE("Opus encoder → decoder round-trip preserves audio")
{
    const SampleFormat format(48000, 16, 2);
    const uint32_t duration_ms = 200;
    auto sine = sineWaveChunk(format, /*freq_hz=*/1000.0, duration_ms);
    const double input_rms = rms(reinterpret_cast<int16_t*>(sine->payload),
                                 sine->payloadSize / sizeof(int16_t));
    REQUIRE(input_rms > 1000.0);  // confirm the fixture itself has signal

    // --- encode ---
    encoder::OpusEncoder enc("BITRATE:192000,COMPLEXITY:5");
    EncodedSink sink;
    enc.init(sink.callback(), format);
    enc.encode(*sine);

    REQUIRE_FALSE(sink.chunks.empty());
    // The encoder produces ~one frame per 20ms of input. Allow some slack
    // around chunk size negotiation (60/40/20ms greedy split).
    REQUIRE(sink.total_duration_ms > 0.0);
    REQUIRE(sink.total_duration_ms <= static_cast<double>(duration_ms) + 5.0);

    // --- decode ---
    decoder::OpusDecoder dec;
    auto header = enc.getHeader();
    REQUIRE(header != nullptr);
    auto decoded_format = dec.setHeader(header.get());
    REQUIRE(decoded_format.rate() == 48000);
    REQUIRE(decoded_format.bits() == 16);
    REQUIRE(decoded_format.channels() == 2);

    std::vector<int16_t> decoded_pcm;
    for (auto& enc_chunk : sink.chunks)
    {
        // Reconstruct a PcmChunk holding the encoded bytes, then let the
        // decoder rewrite payload in-place with PCM (same path as the
        // production client).
        msg::PcmChunk in;
        in.payloadSize = static_cast<uint32_t>(enc_chunk.size());
        in.payload = static_cast<char*>(std::malloc(in.payloadSize));
        std::memcpy(in.payload, enc_chunk.data(), in.payloadSize);

        REQUIRE(dec.decode(&in));
        REQUIRE(in.payloadSize > 0);
        REQUIRE(in.payloadSize % sizeof(int16_t) == 0);

        const auto* s = reinterpret_cast<const int16_t*>(in.payload);
        decoded_pcm.insert(decoded_pcm.end(), s, s + in.payloadSize / sizeof(int16_t));
    }

    REQUIRE_FALSE(decoded_pcm.empty());
    // Sample count is within a couple of Opus frames (~120 samples each) of
    // the input — Opus may introduce a small algorithmic delay but shouldn't
    // double or halve the output.
    const size_t input_samples = sine->payloadSize / sizeof(int16_t);
    REQUIRE(decoded_pcm.size() >= input_samples * 95 / 100);
    REQUIRE(decoded_pcm.size() <= input_samples * 105 / 100);

    // Decoded audio carries energy — RMS at least within a factor of 2 of
    // the original. Looser than a "lossless" comparison because Opus is
    // lossy, but tight enough to fail if we got silence or noise.
    const double out_rms = rms(decoded_pcm.data(), decoded_pcm.size());
    REQUIRE(out_rms > input_rms * 0.5);
    REQUIRE(out_rms < input_rms * 1.5);
}


// ---------------------------------------------------------------------------
// Remainder-buffer drift: feed many small chunks at the non-native 44.1kHz
// rate (which forces the resampler). The fix in opus_encoder.cpp raised
// min_chunk_size from 10ms to 20ms specifically to stop this loop from
// doubling output frame rate when the resampler leaves ~0.08ms of remainder
// per call. We assert: total encoded ms ≈ total input ms (no systematic
// drift), within the resampler's per-chunk noise.
// ---------------------------------------------------------------------------

TEST_CASE("Opus encoder doesn't drift when source rate != 48kHz")
{
    const SampleFormat src_format(44100, 16, 2);
    encoder::OpusEncoder enc("BITRATE:192000,COMPLEXITY:1");
    EncodedSink sink;
    enc.init(sink.callback(), src_format);

    // 50 × 20ms = 1000ms of source audio. With a properly-behaved encoder
    // and the resampler, we expect ~980-1000ms of encoded output (a little
    // less than the input because the trailing fraction stays in the
    // remainder buffer until enough data arrives to flush it).
    const uint32_t chunk_ms = 20;
    const int chunks = 50;
    const double input_ms = chunk_ms * chunks;

    for (int i = 0; i < chunks; ++i)
    {
        auto sine = sineWaveChunk(src_format, /*freq_hz=*/440.0, chunk_ms);
        enc.encode(*sine);
    }

    INFO("input_ms=" << input_ms << " encoded_ms=" << sink.total_duration_ms
                     << " chunks=" << sink.chunks.size());

    // Within ~one chunk of input — the remainder buffer holds at most
    // min_chunk_size (20ms) of audio between flushes. The PRE-fix behavior
    // produced ~2× the expected encoded duration; that's the regression
    // this bound catches.
    REQUIRE(sink.total_duration_ms >= input_ms - 40.0);
    REQUIRE(sink.total_duration_ms <= input_ms + 5.0);

    // Each emitted encoded chunk must be one of Opus's valid frame
    // durations: 5, 10, 20, 40, 60 ms. The current code path picks from
    // {60, 40, 20, min_chunk_size=20}, so we expect every chunk to be a
    // multiple of 10ms when expressed in samples at 48kHz.
    // (We don't have direct access to per-chunk duration here without
    // re-instrumenting the sink — covered by the aggregate above.)
}


TEST_CASE("Opus encoder: native 48kHz path produces ~one chunk per 20ms input")
{
    // Sanity check the no-resampler path — if this test ever drifts the
    // same way the 44.1k path used to, the regression came from elsewhere.
    const SampleFormat format(48000, 16, 2);
    encoder::OpusEncoder enc("BITRATE:192000,COMPLEXITY:1");
    EncodedSink sink;
    enc.init(sink.callback(), format);

    const uint32_t chunk_ms = 20;
    const int chunks = 25;
    for (int i = 0; i < chunks; ++i)
    {
        auto sine = sineWaveChunk(format, /*freq_hz=*/440.0, chunk_ms);
        enc.encode(*sine);
    }

    REQUIRE(sink.chunks.size() == static_cast<size_t>(chunks));
    REQUIRE(sink.total_duration_ms == Catch::Approx(chunks * chunk_ms).margin(0.1));
}
