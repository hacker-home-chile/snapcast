/***
    udp-music: small helpers for building PCM test fixtures (sine waves,
    silence, raw-byte chunks) so encoder/decoder tests don't each carry
    their own copy of the same buffer arithmetic.
***/

#pragma once

#include "common/message/pcm_chunk.hpp"
#include "common/sample_format.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>


namespace test_fixtures
{

/// Build a stereo (or mono) PCM chunk containing a sine wave at @p freq_hz
/// for @p duration_ms. Both channels carry the same tone. Assumes
/// @p format.bits() == 16 (signed).
inline std::shared_ptr<msg::PcmChunk> sineWaveChunk(const SampleFormat& format,
                                                     double freq_hz,
                                                     uint32_t duration_ms,
                                                     double amplitude = 0.5)
{
    auto chunk = std::make_shared<msg::PcmChunk>(format, duration_ms);
    const uint32_t frames = chunk->getFrameCount();
    const auto channels = format.channels();
    const double phase_step = 2.0 * M_PI * freq_hz / format.rate();
    auto* samples = reinterpret_cast<int16_t*>(chunk->payload);
    const double scale = amplitude * 32767.0;
    for (uint32_t i = 0; i < frames; ++i)
    {
        const double v = std::sin(phase_step * i);
        const int16_t s = static_cast<int16_t>(v * scale);
        for (uint16_t c = 0; c < channels; ++c)
            samples[i * channels + c] = s;
    }
    return chunk;
}

/// Build a chunk of N ms of silence in @p format.
inline std::shared_ptr<msg::PcmChunk> silenceChunk(const SampleFormat& format, uint32_t duration_ms)
{
    auto chunk = std::make_shared<msg::PcmChunk>(format, duration_ms);
    std::memset(chunk->payload, 0, chunk->payloadSize);
    return chunk;
}

/// RMS amplitude of an interleaved int16 buffer.
inline double rms(const int16_t* samples, size_t count)
{
    if (count == 0) return 0.0;
    long double acc = 0;
    for (size_t i = 0; i < count; ++i)
        acc += static_cast<long double>(samples[i]) * samples[i];
    return std::sqrt(static_cast<double>(acc / count));
}

} // namespace test_fixtures
