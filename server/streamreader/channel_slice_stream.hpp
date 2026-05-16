/***
    This file is part of snapcast
    Copyright (C) 2014-2026  Johannes Pohl

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
***/

#pragma once


// local headers
#include "pcm_stream.hpp"

// standard headers
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>


namespace streamreader
{

/// "Virtual" stream that subscribes to a parent PcmStream's raw (pre-encoder)
/// chunks, de-interleaves a configurable subset of its channels, and forwards
/// the narrowed chunk through its own encoder.
///
/// One parent ingest event → one timestamp shared by all derived slices, so
/// clients bound to different slices stay frame-synchronous by construction
/// (see VIRTUAL_STREAMS_PLAN.md).
///
/// Mirrors MetaStream's idiom of being both a PcmStream and a
/// PcmStream::Listener on another PcmStream.
class ChannelSliceStream : public PcmStream, public PcmStream::Listener
{
public:
    /// c'tor.
    /// @param parent shared owner of the parent stream; must already be
    ///        instantiated. Its sample format is read for validation.
    /// @param channel_indices zero-based indices into the parent's channel
    ///        layout. Order is preserved in the slice output.
    ChannelSliceStream(PcmStream::Listener* pcmListener, std::shared_ptr<PcmStream> parent, std::vector<uint8_t> channel_indices,
                       boost::asio::io_context& ioc, const ServerSettings& server_settings, const StreamUri& uri, PcmStream::Source source);

    ~ChannelSliceStream() override;

    void start() override;
    void stop() override;

    /// De-interleave @p in_frame_count frames of interleaved PCM at @p sample_size
    /// bytes/sample and @p parent_channels channels into @p out, picking the
    /// channels listed in @p indices in the given order. Pure function, no
    /// PcmStream state required — defined inline so unit tests can call it
    /// without linking the full streamreader translation unit (which would
    /// drag in the encoder factory, control scripts, and io_context).
    static inline void deinterleave(const char* in, std::size_t in_frame_count, std::uint16_t parent_channels, std::uint16_t sample_size,
                                    const std::vector<std::uint8_t>& indices, char* out)
    {
        const std::size_t parent_frame_size = static_cast<std::size_t>(parent_channels) * sample_size;
        const std::size_t out_frame_size = indices.size() * sample_size;
        for (std::size_t f = 0; f < in_frame_count; ++f)
        {
            const char* src_frame = in + f * parent_frame_size;
            char* dst_frame = out + f * out_frame_size;
            for (std::size_t c = 0; c < indices.size(); ++c)
            {
                std::memcpy(dst_frame + c * sample_size, src_frame + static_cast<std::size_t>(indices[c]) * sample_size, sample_size);
            }
        }
    }

protected:
    void onPropertiesChanged(const PcmStream* pcmStream, const Properties& properties) override;
    void onStateChanged(const PcmStream* pcmStream, ReaderState state) override;
    void onChunkRead(const PcmStream* pcmStream, const msg::PcmChunk& chunk) override;
    void onChunkEncoded(const PcmStream* pcmStream, std::shared_ptr<msg::PcmChunk> chunk, double duration) override;
    void onResync(const PcmStream* pcmStream, double ms) override;

private:
    std::shared_ptr<PcmStream> parent_;
    std::vector<std::uint8_t> channel_indices_;
    std::unique_ptr<msg::PcmChunk> slice_chunk_;
    bool first_chunk_ = true;
};

} // namespace streamreader
