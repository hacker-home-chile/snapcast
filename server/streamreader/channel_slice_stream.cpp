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

// prototype/interface header file
#include "channel_slice_stream.hpp"

// local headers
#include "common/aixlog.hpp"
#include "common/snap_exception.hpp"

// standard headers
#include <utility>


using namespace std;

namespace streamreader
{

static constexpr auto LOG_TAG = "SliceStream";


ChannelSliceStream::ChannelSliceStream(PcmStream::Listener* pcmListener, std::shared_ptr<PcmStream> parent, std::vector<std::uint8_t> channel_indices,
                                       boost::asio::io_context& ioc, const ServerSettings& server_settings, const StreamUri& uri, PcmStream::Source source)
    : PcmStream(pcmListener, ioc, server_settings, uri, source), parent_(std::move(parent)), channel_indices_(std::move(channel_indices))
{
    if (!parent_)
        throw SnapException("ChannelSliceStream: parent is null");

    if (channel_indices_.empty())
        throw SnapException("ChannelSliceStream '" + getName() + "': channels list must not be empty");

    const auto parent_channels = parent_->getSampleFormat().channels();
    for (auto idx : channel_indices_)
    {
        if (idx >= parent_channels)
            throw SnapException("ChannelSliceStream '" + getName() + "': channel index " + std::to_string(static_cast<int>(idx)) +
                                " out of range (parent has " + std::to_string(parent_channels) + " channels)");
    }

    if (sampleFormat_.rate() != parent_->getSampleFormat().rate())
        throw SnapException("ChannelSliceStream '" + getName() + "': sample rate must match parent");
    if (sampleFormat_.bits() != parent_->getSampleFormat().bits())
        throw SnapException("ChannelSliceStream '" + getName() + "': sample bits must match parent");
    if (sampleFormat_.channels() != channel_indices_.size())
        throw SnapException("ChannelSliceStream '" + getName() + "': declared channel count (" + std::to_string(sampleFormat_.channels()) +
                            ") doesn't match channels list length (" + std::to_string(channel_indices_.size()) + ")");

    slice_chunk_ = std::make_unique<msg::PcmChunk>(sampleFormat_, 0);

    LOG(INFO, LOG_TAG) << "ChannelSliceStream '" << getName() << "' parent='" << parent_->getName() << "' channels="
                       << static_cast<int>(channel_indices_.size()) << " rate=" << sampleFormat_.rate() << "\n";
}


ChannelSliceStream::~ChannelSliceStream()
{
    if (parent_)
        parent_->removeListener(this);
}


void ChannelSliceStream::start()
{
    LOG(DEBUG, LOG_TAG) << "Start '" << getName() << "'\n";
    PcmStream::start();
    parent_->addListener(this);
    first_chunk_ = true;
}


void ChannelSliceStream::stop()
{
    LOG(DEBUG, LOG_TAG) << "Stop '" << getName() << "'\n";
    if (parent_)
        parent_->removeListener(this);
    PcmStream::stop();
}


void ChannelSliceStream::onChunkRead(const PcmStream* pcmStream, const msg::PcmChunk& chunk)
{
    if (pcmStream != parent_.get() || !active_)
        return;

    const auto parent_channels = parent_->getSampleFormat().channels();
    const auto sample_size = parent_->getSampleFormat().sampleSize();
    const std::size_t parent_frame_size = static_cast<std::size_t>(parent_channels) * sample_size;
    if (parent_frame_size == 0)
        return;
    const std::size_t frame_count = chunk.payloadSize / parent_frame_size;

    slice_chunk_->setFrameCount(static_cast<int>(frame_count));

    deinterleave(chunk.payload, frame_count, parent_channels, sample_size, channel_indices_, slice_chunk_->payload);

    // Forward the parent's next-encoded-chunk timestamp so the slice's
    // encoder stamps the same absolute time. On the very first chunk we
    // additionally seed from the parent (which may have been running for
    // a while) so the slice's encoder doesn't stamp an epoch-zero time.
    if (first_chunk_)
    {
        first_chunk_ = false;
        tvEncodedChunk_ = parent_->getTvEncodedChunk();
    }
    else
    {
        // Keep slice aligned to parent even if encoder buffering causes
        // tiny drift between the two independent encoders.
        tvEncodedChunk_ = parent_->getTvEncodedChunk();
    }

    chunkRead(*slice_chunk_);
}


void ChannelSliceStream::onStateChanged(const PcmStream* pcmStream, ReaderState state)
{
    if (pcmStream != parent_.get())
        return;
    setState(state);
    if (state != ReaderState::kPlaying)
        first_chunk_ = true;
}


void ChannelSliceStream::onResync(const PcmStream* pcmStream, double ms)
{
    if (pcmStream != parent_.get())
        return;
    resync(std::chrono::nanoseconds(static_cast<std::int64_t>(ms * 1000000)));
    first_chunk_ = true;
}


void ChannelSliceStream::onPropertiesChanged(const PcmStream* /*pcmStream*/, const Properties& /*properties*/)
{
    // Slices don't propagate parent metadata — they're independent assignable
    // streams. Add forwarding later if a UX need emerges.
}


void ChannelSliceStream::onChunkEncoded(const PcmStream* /*pcmStream*/, std::shared_ptr<msg::PcmChunk> /*chunk*/, double /*duration*/)
{
    // We only consume parent's RAW chunks via onChunkRead. The parent's
    // encoded chunk path is irrelevant to slicing.
}


} // namespace streamreader
