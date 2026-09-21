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
#include "stream_manager.hpp"

// local headers
#include "airplay_stream.hpp"
#include "common/aixlog.hpp"
#include <algorithm>
#ifdef HAS_ALSA
#include "alsa_stream.hpp"
#endif
#ifdef HAS_JACK
#include "jack_stream.hpp"
#endif
#ifdef HAS_PIPEWIRE
#include "pipewire_stream.hpp"
#endif
#include "channel_slice_stream.hpp"
#include "common/snap_exception.hpp"
#include "common/str_compat.hpp"
#include "common/utils/string_utils.hpp"
#include "file_stream.hpp"
#include "librespot_stream.hpp"
#include "meta_stream.hpp"
#include "pipe_stream.hpp"
#include "process_stream.hpp"
#include "tcp_stream.hpp"

// 3rd party headers

// standard headers


static constexpr auto LOG_TAG = "StreamManager";


using namespace std;

namespace streamreader
{

StreamManager::StreamManager(PcmStream::Listener* pcmListener, boost::asio::io_context& ioc, ServerSettings settings)
    // const std::string& defaultSampleFormat, const std::string& defaultCodec, size_t defaultChunkBufferMs)
    : pcmListener_(pcmListener), settings_(std::move(settings)), io_context_(ioc)
{
}


PcmStreamPtr StreamManager::addStream(const std::string& uri, PcmStream::Source source)
{
    StreamUri streamUri(uri);
    return addStream(streamUri, source);
}


PcmStreamPtr StreamManager::addStream(StreamUri& streamUri, PcmStream::Source source)
{
    // Slice streams inherit sampleformat from the parent — patch it into the
    // URI before the PcmStream c'tor runs (which requires sampleformat).
    PcmStreamPtr slice_parent;
    std::vector<std::uint8_t> slice_channels;
    if (streamUri.scheme == "slice")
    {
        std::string parent_name = streamUri.path;
        if (!parent_name.empty() && parent_name.front() == '/')
            parent_name.erase(0, 1);
        if (parent_name.empty())
            throw SnapException("slice stream: parent name must be the URI path (slice:///<parent>?...)");

        auto parent_iter = std::find_if(streams_.begin(), streams_.end(),
                                        [&parent_name](const PcmStreamPtr& s) { return s->getName() == parent_name; });
        if (parent_iter == streams_.end())
            throw SnapException("slice stream: parent '" + parent_name + "' not found (must be declared before the slice)");
        slice_parent = *parent_iter;
        if (slice_parent->getUri().scheme == "slice")
            throw SnapException("slice stream: nested slicing is not supported in v1");

        auto channels_str = streamUri.getQuery("channels");
        if (channels_str.empty())
            throw SnapException("slice stream: 'channels' query param required (e.g. channels=0,1)");
        for (const auto& tok : utils::string::split(channels_str, ','))
        {
            if (tok.empty())
                continue;
            int v = cpt::stoi(tok, -1);
            if (v < 0 || v > 255)
                throw SnapException("slice stream: invalid channel index '" + tok + "'");
            slice_channels.push_back(static_cast<std::uint8_t>(v));
        }
        if (slice_channels.empty())
            throw SnapException("slice stream: channels list must not be empty");

        // Synthesize a sampleformat string from parent rate/bits and slice channel count.
        const auto& pf = slice_parent->getSampleFormat();
        streamUri.query[kUriSampleFormat] =
            cpt::to_string(pf.rate()) + ":" + cpt::to_string(pf.bits()) + ":" + cpt::to_string(slice_channels.size());
    }

    if (streamUri.query.find(kUriSampleFormat) == streamUri.query.end())
        streamUri.query[kUriSampleFormat] = settings_.stream.sampleFormat;

    if (streamUri.query.find(kUriCodec) == streamUri.query.end())
        streamUri.query[kUriCodec] = settings_.stream.codec;

    if (streamUri.query.find(kUriChunkMs) == streamUri.query.end())
        streamUri.query[kUriChunkMs] = cpt::to_string(settings_.stream.streamChunkMs);

    auto name = streamUri.query[kUriName];
    if (name.empty())
        throw SnapException("Stream name must not be empty");

    auto iter = find_if(streams_.begin(), streams_.end(), [&name](const PcmStreamPtr& stream) { return stream->getName() == name; });
    if (iter != streams_.end())
        throw SnapException("Stream with name '" + name + "' already exists");
    //	LOG(DEBUG) << "\nURI: " << streamUri.uri << "\nscheme: " << streamUri.scheme << "\nhost: "
    //		<< streamUri.host << "\npath: " << streamUri.path << "\nfragment: " << streamUri.fragment << "\n";

    //	for (auto kv: streamUri.query)
    //		LOG(DEBUG) << "key: '" << kv.first << "' value: '" << kv.second << "'\n";
    PcmStreamPtr stream(nullptr);

    PcmStream::Listener* listener = pcmListener_;
    if ((streamUri.query[kUriCodec] == "null") && (streamUri.scheme != "meta"))
    {
        // Streams with null codec are "invisible" and will not report any updates to the listener.
        // If the stream is used as input for a Meta stream, then the meta stream will add himself
        // as another listener to the stream, so that updates are indirect reported through it.
        listener = nullptr;
    }

    if (streamUri.scheme == "pipe")
    {
        stream = make_shared<PipeStream>(listener, io_context_, settings_, streamUri, source);
    }
#ifdef HAS_ALSA
    else if (streamUri.scheme == "alsa")
    {
        stream = make_shared<AlsaStream>(listener, io_context_, settings_, streamUri, source);
    }
#endif
    else if ((streamUri.scheme == "spotify") || (streamUri.scheme == "librespot"))
    {
        // Overwrite sample format here instead of inside the constructor, to make sure
        // that all constructors of all parent classes also use the overwritten sample
        // format.
        streamUri.query[kUriSampleFormat] = "44100:16:2";
        stream = make_shared<LibrespotStream>(listener, io_context_, settings_, streamUri, source);
    }
    else if (streamUri.scheme == "airplay")
    {
        // Overwrite sample format here instead of inside the constructor, to make sure
        // that all constructors of all parent classes also use the overwritten sample
        // format.
        streamUri.query[kUriSampleFormat] = "44100:16:2";
        stream = make_shared<AirplayStream>(listener, io_context_, settings_, streamUri, source);
    }
    else if (streamUri.scheme == "file")
    {
        stream = make_shared<FileStream>(listener, io_context_, settings_, streamUri, source);
    }
    else if (streamUri.scheme == "process")
    {
        stream = make_shared<ProcessStream>(listener, io_context_, settings_, streamUri, source);
    }
    else if (streamUri.scheme == "tcp")
    {
        stream = make_shared<TcpStream>(listener, io_context_, settings_, streamUri, source);
    }
#ifdef HAS_PIPEWIRE
    else if (streamUri.scheme == "pipewire")
    {
        stream = make_shared<PipeWireStream>(listener, io_context_, settings_, streamUri, source);
    }
#endif
#ifdef HAS_JACK
    else if (streamUri.scheme == "jack")
    {
        stream = make_shared<JackStream>(listener, io_context_, settings_, streamUri, source);
    }
#endif
    else if (streamUri.scheme == "meta")
    {
        stream = make_shared<MetaStream>(listener, streams_, io_context_, settings_, streamUri, source);
    }
    else if (streamUri.scheme == "slice")
    {
        stream = make_shared<ChannelSliceStream>(listener, slice_parent, std::move(slice_channels), io_context_, settings_, streamUri, source);
    }
    else
    {
        throw SnapException("Unknown stream type: " + streamUri.scheme);
    }

    if (stream)
        streams_.push_back(stream);

    return stream;
}


bool StreamManager::removeStream(const std::string& name)
{
    LOG(INFO, LOG_TAG) << "Removing stream '" << name << "'\n";
    auto iter = std::find_if(streams_.begin(), streams_.end(), [&name](const PcmStreamPtr& stream) { return stream->getName() == name; });
    if (iter != streams_.end())
    {
        (*iter)->stop();
        streams_.erase(iter);
        LOG(DEBUG, LOG_TAG) << "Found and removed stream '" << (*iter)->getName() << "'\n";
        return true;
    }
    else
    {
        LOG(WARNING, LOG_TAG) << "Stream '" << name << "' not found\n";
        return false;
    }
}


const std::vector<PcmStreamPtr>& StreamManager::getStreams() const
{
    return streams_;
}


const PcmStreamPtr StreamManager::getDefaultStream() const
{
    if (streams_.empty())
        return nullptr;

    auto& default_source = settings_.stream.default_source;
    PcmStreamPtr firstValidStream = nullptr;
    for (const auto& stream : streams_)
    {
        if (stream->getCodec() != "null")
        {
            if (firstValidStream == nullptr)
                firstValidStream = stream;

            if (!default_source.has_value() || (default_source.value() == stream->getName()))
                return stream;
        }
    }
    return firstValidStream;
}


const PcmStreamPtr StreamManager::getStream(const std::string& id) const
{
    for (auto stream : streams_)
    {
        if (stream->getId() == id)
            return stream;
    }
    return nullptr;
}


void StreamManager::start()
{
    // Start meta and slice streams first so they're listening before parents emit.
    for (const auto& stream : streams_)
        if (stream->getUri().scheme == "meta" || stream->getUri().scheme == "slice")
            stream->start();
    // Start normal streams second
    for (const auto& stream : streams_)
        if (stream->getUri().scheme != "meta" && stream->getUri().scheme != "slice")
            stream->start();
}


void StreamManager::stop()
{
    // Stop normal streams first
    for (const auto& stream : streams_)
        if (stream && (stream->getUri().scheme != "meta") && (stream->getUri().scheme != "slice"))
            stream->stop();
    // Stop meta/slice streams second
    for (const auto& stream : streams_)
        if (stream && ((stream->getUri().scheme == "meta") || (stream->getUri().scheme == "slice")))
            stream->stop();
}


json StreamManager::toJson() const
{
    json result = json::array();
    for (const auto& stream : streams_)
    {
        // A stream with "null" codec will only serve as input for a meta stream, i.e. is not a "stand alone" stream.
        // Streams marked hidden=true (e.g. raw multi-channel parents of virtual slices) are likewise
        // excluded from default enumeration so clients only see the assignable slices.
        if (stream->getCodec() != "null" && !stream->isHidden())
            result.push_back(stream->toJson());
    }
    return result;
}

} // namespace streamreader
