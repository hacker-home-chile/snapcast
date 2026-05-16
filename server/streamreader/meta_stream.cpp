/***
    This file is part of snapcast
    Copyright (C) 2014-2025  Johannes Pohl

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
#include "meta_stream.hpp"

// local headers
#include "common/aixlog.hpp"
#include "common/snap_exception.hpp"
#include "common/utils/string_utils.hpp"


using namespace std;

namespace streamreader
{

static constexpr auto LOG_TAG = "MetaStream";
// static constexpr auto kResyncTolerance = 50ms;


MetaStream::MetaStream(PcmStream::Listener* pcmListener, const std::vector<std::shared_ptr<PcmStream>>& streams, boost::asio::io_context& ioc,
                       const ServerSettings& server_settings, const StreamUri& uri, PcmStream::Source source)
    : PcmStream(pcmListener, ioc, server_settings, uri, source), first_read_(true)
{
    auto path_components = utils::string::split(uri.path, '/');
    for (const auto& component : path_components)
    {
        if (component.empty())
            continue;

        bool found = false;
        for (const auto& stream : streams)
        {
            if (stream->getName() == component)
            {
                streams_.push_back(stream);
                stream->addListener(this);
                found = true;
                break;
            }
        }
        if (!found)
            throw SnapException("Unknown stream: \"" + component + "\"");
    }

    if (streams_.empty())
        throw SnapException("Meta stream '" + getName() + "' must contain at least one stream");

    auto first = streams_.front();
    active_selector_.set(first);
    resampler_ = make_unique<Resampler>(first->getSampleFormat(), sampleFormat_);
}


MetaStream::~MetaStream()
{
    stop(); // NOLINT
}


void MetaStream::start()
{
    LOG(DEBUG, LOG_TAG) << "Start, sampleformat: " << sampleFormat_.toString() << "\n";
    PcmStream::start();
}


void MetaStream::stop()
{
    active_ = false;
}


void MetaStream::onPropertiesChanged(const PcmStream* pcmStream, const Properties& properties)
{
    LOG(DEBUG, LOG_TAG) << "onPropertiesChanged: " << pcmStream->getName() << "\n";
    auto active = active_selector_.get();
    if (!active || pcmStream != active.get())
        return;
    setProperties(properties);
}


void MetaStream::onStateChanged(const PcmStream* pcmStream, ReaderState state)
{
    LOG(DEBUG, LOG_TAG) << "onStateChanged: " << pcmStream->getName() << ", state: " << state << "\n";

    // Should a pause keep the stream active? E.g. Spotify can only pause, so it would never get inactive
    // if (active_stream_->getProperties().playback_status == PlaybackStatus::kPaused)
    //     return;

    auto switch_stream = [this](const std::shared_ptr<PcmStream>& new_stream)
    {
        auto current = active_selector_.get();
        if (new_stream == current)
            return;
        LOG(INFO, LOG_TAG) << "Stream: " << name_ << ", switching active stream: " << (current ? current->getName() : "<null>") << " => "
                           << new_stream->getName() << "\n";
        active_selector_.set(new_stream);
        setProperties(new_stream->getProperties());
        resampler_ = make_unique<Resampler>(new_stream->getSampleFormat(), sampleFormat_);
    };

    for (const auto& stream : streams_)
    {
        if (stream->getState() == ReaderState::kPlaying)
        {
            if (state_ != ReaderState::kPlaying)
                first_read_ = true;

            auto current = active_selector_.get();
            if (current != stream)
                switch_stream(stream);

            setState(ReaderState::kPlaying);
            return;
        }
    }

    switch_stream(streams_.front());
    setState(ReaderState::kIdle);
}


void MetaStream::onChunkRead(const PcmStream* pcmStream, const msg::PcmChunk& chunk)
{
    // LOG(TRACE, LOG_TAG) << "onChunkRead: " << pcmStream->getName() << ", duration: " << chunk.durationMs() << "\n";
    auto active = active_selector_.get();
    if (!active || pcmStream != active.get())
        return;
    // active_stream_->sampleFormat_
    // sampleFormat_

    if (first_read_)
    {
        first_read_ = false;
        LOG(INFO, LOG_TAG) << "first read, updating timestamp\n";
        tvEncodedChunk_ = std::chrono::steady_clock::now() - chunk.duration<std::chrono::nanoseconds>();
        next_tick_ = std::chrono::steady_clock::now();
    }


    next_tick_ += chunk.duration<std::chrono::nanoseconds>();
    auto currentTick = std::chrono::steady_clock::now();
    auto next_read = next_tick_ - currentTick;

    // Read took longer, wait for the buffer to fill up
    if (next_read < 0ms)
    {
        // if (next_read >= -kResyncTolerance)
        // {
        //     LOG(INFO, LOG_TAG) << "next read < 0 (" << getName() << "): " << std::chrono::duration_cast<std::chrono::microseconds>(next_read).count() / 1000.
        //                        << " ms\n";
        // }
        // else
        // {
        resync(-next_read);
        first_read_ = true;
        // }
    }

    if (resampler_ && resampler_->resamplingNeeded())
    {
        auto resampled_chunk = resampler_->resample(chunk);
        if (resampled_chunk)
            chunkRead(*resampled_chunk);
    }
    else
        chunkRead(chunk);
}


void MetaStream::onChunkEncoded(const PcmStream* pcmStream, std::shared_ptr<msg::PcmChunk> chunk, double duration)
{
    std::ignore = pcmStream;
    std::ignore = chunk;
    std::ignore = duration;
    // LOG(TRACE, LOG_TAG) << "onChunkEncoded: " << pcmStream->getName() << ", duration: " << duration << "\n";
    // chunkEncoded(*encoder_, chunk, duration);
}


void MetaStream::onResync(const PcmStream* pcmStream, double ms)
{
    LOG(DEBUG, LOG_TAG) << "onResync: " << pcmStream->getName() << ", duration: " << ms << " ms\n";
    auto active = active_selector_.get();
    if (!active || pcmStream != active.get())
        return;
    resync(std::chrono::nanoseconds(static_cast<int64_t>(ms * 1000000)));
}


// Property/command delegation: each method snapshots the active child via
// applyToActive, then invokes the underlying call OUTSIDE the selector's
// lock. The shared_ptr snapshot keeps the child alive even if onStateChanged
// switches active_selector_ to a different stream mid-call — closing the
// data race the prior split-mutex code had.

void MetaStream::setShuffle(bool shuffle, ResultHandler&& handler)
{
    active_selector_.applyToActive([shuffle, h = std::move(handler)](PcmStream& s) mutable
    {
        s.setShuffle(shuffle, std::move(h));
    });
}

void MetaStream::setLoopStatus(LoopStatus status, ResultHandler&& handler)
{
    active_selector_.applyToActive([status, h = std::move(handler)](PcmStream& s) mutable
    {
        s.setLoopStatus(status, std::move(h));
    });
}

void MetaStream::setVolume(uint16_t volume, ResultHandler&& handler)
{
    active_selector_.applyToActive([volume, h = std::move(handler)](PcmStream& s) mutable
    {
        s.setVolume(volume, std::move(h));
    });
}

void MetaStream::setMute(bool mute, ResultHandler&& handler)
{
    active_selector_.applyToActive([mute, h = std::move(handler)](PcmStream& s) mutable
    {
        s.setMute(mute, std::move(h));
    });
}

void MetaStream::setRate(float rate, ResultHandler&& handler)
{
    active_selector_.applyToActive([rate, h = std::move(handler)](PcmStream& s) mutable
    {
        s.setRate(rate, std::move(h));
    });
}


// Control commands
void MetaStream::setPosition(std::chrono::milliseconds position, ResultHandler&& handler)
{
    active_selector_.applyToActive([position, h = std::move(handler)](PcmStream& s) mutable
    {
        s.setPosition(position, std::move(h));
    });
}

void MetaStream::seek(std::chrono::milliseconds offset, ResultHandler&& handler)
{
    active_selector_.applyToActive([offset, h = std::move(handler)](PcmStream& s) mutable
    {
        s.seek(offset, std::move(h));
    });
}

void MetaStream::next(ResultHandler&& handler)
{
    active_selector_.applyToActive([h = std::move(handler)](PcmStream& s) mutable
    {
        s.next(std::move(h));
    });
}

void MetaStream::previous(ResultHandler&& handler)
{
    active_selector_.applyToActive([h = std::move(handler)](PcmStream& s) mutable
    {
        s.previous(std::move(h));
    });
}

void MetaStream::pause(ResultHandler&& handler)
{
    active_selector_.applyToActive([h = std::move(handler)](PcmStream& s) mutable
    {
        s.pause(std::move(h));
    });
}

void MetaStream::playPause(ResultHandler&& handler)
{
    LOG(DEBUG, LOG_TAG) << "PlayPause\n";
    auto active = active_selector_.get();
    if (!active)
        return;
    if (active->getState() == ReaderState::kIdle)
        play(std::move(handler));
    else
        active->playPause(std::move(handler));
}

void MetaStream::stop(ResultHandler&& handler)
{
    active_selector_.applyToActive([h = std::move(handler)](PcmStream& s) mutable
    {
        s.stop(std::move(h));
    });
}

void MetaStream::play(ResultHandler&& handler)
{
    LOG(DEBUG, LOG_TAG) << "Play\n";
    auto active = active_selector_.get();
    if (active && active->getProperties().can_play &&
        active->getProperties().playback_status != PlaybackStatus::kPlaying)
    {
        active->play(std::move(handler));
        return;
    }

    for (const auto& stream : streams_)
    {
        if ((stream->getState() == ReaderState::kIdle) && (stream->getProperties().can_play))
        {
            stream->play(std::move(handler));
            return;
        }
    }

    // call play on the active stream to get the handler called
    if (active)
        active->play(std::move(handler));
}


} // namespace streamreader
