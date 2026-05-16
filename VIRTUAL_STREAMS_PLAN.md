# Virtual Streams — Implementation Plan

Add channel-slice "virtual streams" to snapserver so one multi-channel parent
PCM source (e.g. a 6-ch 5.1 mix arriving over TCP) can be split into multiple
narrower streams (mono center, stereo front, stereo surround, …) that share the
parent's chunk timestamps. Clients bind to virtual streams via the existing
group/stream mechanism. **Zero client-side or wire-protocol changes.**

## Goals

- One new `PcmStream` subclass: `ChannelSliceStream`.
- One new config block: `[virtual_stream]`.
- One optional flag on existing streams: `hidden = true`.
- Cross-stream sync **by construction** (all slices forward the parent's
  timestamp; one ingest event → one timestamp shared by all derived chunks).
- Full backwards compatibility: existing configs and existing snapclient
  builds (desktop, mobile, ESP firmware, third-party) keep working unchanged.

## Non-goals (explicitly out of scope)

- The upstream matrix mixer / live-tuning daemon. Snapserver accepts an
  N-channel PCM source at a documented channel order; how it's produced
  (ffmpeg, a daemon, a file) is not snapserver's problem.
- Any change to the wire protocol (`UDPR`, `Hello`, `ServerSettings`,
  `CodecHeader`). No new bytes anywhere.
- Per-client channel routing. Routing happens by binding a client's group to
  a virtual stream — the existing mechanism.
- In-server matrix / mixing / gain / EQ / crossover. Pure channel selection.
- Time sync, FEC, JSON-RPC, web UI, encoders, transport — all untouched.

## Architectural summary

```
TCP source (e.g. 48000:16:6 raw PCM)
       │  one ingest event per chunk  →  timestamp T (server clock)
       ▼
   PcmStream "mix"  (hidden = true)
       │
       ├── ChannelSliceStream "front"     channels=[0,1]  → Opus 2ch encoder → group "Front"
       ├── ChannelSliceStream "center"    channels=[2]    → Opus 1ch encoder → group "Center"
       └── ChannelSliceStream "surround"  channels=[4,5]  → Opus 2ch encoder → group "Surround"
```

All slices forward timestamp `T` verbatim. Each client computes
`T + bufferMs` against the shared server clock → simultaneous playout across
all speakers, regardless of which group/stream they're on.

## Code map (snapserver)

Relevant existing files (read these first):

- `server/streamreader/pcm_stream.hpp` / `.cpp` — base class. Note how it
  produces chunks, owns its encoder, and how `SampleFormat` flows.
- `server/streamreader/meta_stream.hpp` / `.cpp` — **closest precedent**:
  a `PcmStream` that wraps other `PcmStream`s. Mirror its lifecycle handling
  (parent-attached, parent-detached, restart propagation).
- `server/streamreader/tcp_stream.cpp` — likely parent source in practice;
  shows how chunks are timestamped at ingest.
- `server/streamreader/stream_manager.hpp` / `.cpp` — loads streams from
  config; the new `[virtual_stream]` block plugs in here.
- `server/encoder/` — per-stream encoders; no changes needed but verify
  Opus mono/stereo paths both work (they already do).
- `server/server.cpp` / config layer — wire up `[virtual_stream]` parsing
  and `hidden` flag.

## Deliverable 1 — `ChannelSliceStream`

New files: `server/streamreader/channel_slice_stream.hpp` / `.cpp`.

### Interface (sketch — adapt to actual `PcmStream` base)

```cpp
class ChannelSliceStream : public PcmStream
{
public:
    // parent is a non-owning pointer; lifetime managed by StreamManager.
    // channel_indices: zero-based indices into parent's channel layout.
    // Resulting SampleFormat = parent rate, parent bits, channel_indices.size().
    ChannelSliceStream(PcmListener* listener,
                       boost::asio::io_context& ioc,
                       const ServerSettings& settings,
                       const StreamUri& uri,
                       std::shared_ptr<PcmStream> parent,
                       std::vector<uint8_t> channel_indices);

    void start() override;   // attach to parent
    void stop() override;    // detach from parent

private:
    // Called when parent emits a chunk. De-interleaves the configured channels
    // into a narrower chunk and forwards downstream WITH PARENT'S TIMESTAMP.
    void onParentChunk(const msg::PcmChunk& parent_chunk);

    std::shared_ptr<PcmStream> parent_;
    std::vector<uint8_t> channel_indices_;
};
```

### Behavioral requirements

1. **Timestamp forwarding.** The slice chunk's timestamp MUST equal the parent
   chunk's timestamp. Do not re-stamp at slice time. This is what makes
   cross-slice sync exact.
2. **No buffering beyond what's required to de-interleave one chunk.** One
   parent chunk in → one slice chunk out, same frame count.
3. **De-interleave loop** is the only DSP. For each frame, copy the listed
   channels into the output buffer in the order given by `channel_indices_`.
   Support `int16_t` first; templatize for other sample widths only if the
   existing codebase already does (mirror what `PcmStream` does).
4. **Encoder.** Inherited from `PcmStream` — instantiate Opus (or whatever
   the URI specifies) with the slice's channel count. No new encoder path.
5. **Lifecycle.**
   - `start()`: register a chunk-listener callback on the parent.
   - `stop()`: unregister.
   - Parent end-of-stream / disconnect → propagate (all slices go idle).
   - Parent reconnect → slices resume automatically via the same callback.
6. **Validation.** On construction, assert each index < parent's channel
   count. Error clearly on misconfig.
7. **Thread safety.** Follow whatever pattern `MetaStream` uses for
   cross-stream callbacks (likely `io_context::post` to serialize).

### Parent-side hook

`PcmStream` likely already broadcasts encoded chunks to its own listeners. The
slice needs the **decoded** (pre-encoder) chunk. Two options, pick whichever
fits the existing code:

- Add a `addRawChunkListener(callback)` API on `PcmStream` that fires with the
  raw PCM chunk before encoding. Slices subscribe. Cheapest change.
- Or: have `ChannelSliceStream` reach into the parent the same way
  `MetaStream` reaches into its children — mirror that pattern exactly.

**Read `MetaStream` first and copy its idiom.** Don't invent a new pattern.

## Deliverable 2 — Config support

### New block: `[virtual_stream]`

```ini
[stream]
source = tcp://127.0.0.1:4000?name=mix&sampleformat=48000:16:6&codec=raw
hidden = true                       # new optional flag, default false

[virtual_stream]
name     = front
parent   = mix
channels = 0,1
codec    = opus
# Inherits rate/bits from parent. Channel count derived from `channels` length.
# Optional encoder params (bitrate etc.) accepted same as [stream].

[virtual_stream]
name     = center
parent   = mix
channels = 2
codec    = opus

[virtual_stream]
name     = surround
parent   = mix
channels = 4,5
codec    = opus
```

### Loader changes (`stream_manager.cpp`)

1. Parse all `[stream]` blocks first and instantiate as today.
2. Then parse `[virtual_stream]` blocks. For each:
   - Look up `parent` by name; error if missing or itself a virtual stream
     (no nested slicing in v1 — keep it simple).
   - Parse `channels` as comma-separated indices, validate against parent's
     channel count.
   - Construct a `ChannelSliceStream` and register it in the stream map under
     `name`, like any other stream.
3. Honor `hidden = true` on any stream by tagging it so the web UI / JSON-RPC
   `Server.GetStatus` either omits it or marks it non-assignable. Check
   what's least invasive: a `meta.hidden` flag in the stream's JSON status
   the UI can respect is probably enough; the UI doesn't need to be patched
   if we just omit hidden streams from the list response.

### Backwards compat

- No `[virtual_stream]` blocks → identical behavior to today.
- `hidden` absent → defaults to `false` → identical behavior to today.
- Existing snapclients connecting to a virtual stream receive a normal
  `CodecHeader` with 1 or 2 channels — already supported.

## Deliverable 3 — Tests

### Unit tests (new)

- `ChannelSliceStream` de-interleaves correctly for: `[0]`, `[1]`, `[0,1]`,
  `[2]`, `[4,5]`, `[5,4]` (order matters), against a synthetic 6-ch chunk
  of known per-channel values.
- Slice chunk timestamp == parent chunk timestamp.
- Slice chunk frame count == parent chunk frame count.
- Invalid index (>= parent channels) → construction error.

### Integration test (manual, document in plan)

```bash
# Feed snapserver a synthetic 6-channel stream with one distinct tone per channel.
ffmpeg -f lavfi -i "sine=f=200:c=1, sine=f=400:c=1, sine=f=800:c=1, \
                    sine=f=1200:c=1, sine=f=1600:c=1, sine=f=2000:c=1" \
  -filter_complex "[0:a][1:a][2:a][3:a][4:a][5:a]amerge=inputs=6[a]" \
  -map "[a]" -f s16le -ar 48000 tcp://127.0.0.1:4000
```

Connect three snapclients, bind to `front` / `center` / `surround`. Verify
audibly (or with a spectrum analyzer): `front` plays 200+400 Hz, `center`
plays 800 Hz, `surround` plays 1600+2000 Hz. All three start simultaneously
on play.

### Sync verification

Two snapclients on different virtual streams, both playing a click track on
their respective channels. Loopback-record both outputs into a stereo
recorder. Click alignment within Snapcast's normal sync tolerance
(sub-millisecond). Document the measurement procedure in the test plan.

## Build order (recommended for the implementing agent)

1. Read `pcm_stream.{hpp,cpp}` and `meta_stream.{hpp,cpp}` end-to-end.
   Understand how chunks flow, how `SampleFormat` propagates, how lifecycles
   nest. Do not write code until this is clear.
2. Decide on the parent→slice hook (raw chunk listener vs. mirror MetaStream
   idiom). Document the choice in code comments.
3. Implement `ChannelSliceStream` against a hand-built mock parent in unit
   tests. Get de-interleaving + timestamp forwarding green before touching
   `StreamManager`.
4. Add `[virtual_stream]` parsing in `stream_manager.cpp`. Wire up
   instantiation. Add `hidden` flag plumbing.
5. End-to-end test with the ffmpeg tone generator above and three local
   snapclients on different ALSA devices.
6. Verify backwards compat: start snapserver with an existing config that
   has no `[virtual_stream]` blocks → must behave identically to before.

## Out-of-scope follow-ups (do not implement now, just note)

- Web UI hint that a stream is "5.1 group — keep matched bufferMs."
- Nested virtual streams (slice of a slice).
- Server-side gain/EQ/crossover per slice — keep that in the daemon.
- Dynamic add/remove of virtual streams over JSON-RPC at runtime.
- Per-output `output_idx` UDP routing (only needed if we ever revive the
  in-server matrix variant — currently rejected in favor of the daemon).

## Acceptance checklist

- [ ] `ChannelSliceStream` exists and passes unit tests.
- [ ] `[virtual_stream]` blocks parse and instantiate.
- [ ] `hidden = true` hides a stream from default JSON-RPC enumeration.
- [ ] Three vanilla snapclients on `front` / `center` / `surround` play
      distinct tones, start in sync, stay in sync.
- [ ] Existing configs without virtual streams behave identically to before.
- [ ] No changes to wire protocol, client binaries, or web UI source.
