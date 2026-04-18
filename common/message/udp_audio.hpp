/***
    This file is part of udp-music — a loss-tolerant snapcast fork.

    24-byte packet header for the UDP audio transport.  Every UDP audio
    datagram from the server carries this header followed by either an
    encoded Opus frame (data packet) or the XOR of the N data payloads
    in the same FEC group (parity packet).

    Control traffic (Hello, ServerSettings, Time, CodecHeader, …) keeps
    going over TCP; only the audio chunk stream moves to UDP.
***/

#pragma once

#include <cstdint>
#include <cstring>

namespace msg
{

static constexpr uint8_t  kUdpAudioMagic   = 0xA0;
static constexpr uint8_t  kUdpAudioVersion = 1;
static constexpr size_t   kUdpAudioHeaderSize = 24;

enum UdpAudioFlags : uint8_t
{
    kUdpFlagIsParity = 1u << 0,
};

// Wire layout — little-endian, packed, no trailing padding.
#pragma pack(push, 1)
struct UdpAudioHeader
{
    uint8_t  magic;            // kUdpAudioMagic
    uint8_t  version;          // kUdpAudioVersion
    uint8_t  flags;            // UdpAudioFlags bitset
    uint8_t  fec_group_size;   // N (number of data pkts per FEC group)
    uint32_t seq;              // global packet sequence number
    uint32_t fec_group;        // FEC group id
    uint32_t timestamp_us;     // playout timestamp (μs, server clock)
    uint16_t payload_len;      // bytes of payload following the header
    uint16_t reserved0;        // must be 0
    // Meaning depends on packet type:
    //   data   → timestamp_sec: the seconds portion of the chunk's playout
    //            time. Paired with `timestamp_us` (µs of current second,
    //            0..999999) that replaces the old 32-bit wrapping us field.
    //   parity → XOR of all data packets' payload_len values in this group,
    //            so a receiver can recover the missing packet's true length.
    uint32_t aux;
};
#pragma pack(pop)

static_assert(sizeof(UdpAudioHeader) == kUdpAudioHeaderSize,
              "UdpAudioHeader must be exactly 24 bytes on the wire");

// Little-endian serialization helpers — avoid UB from punning.
inline void writeLE16(uint8_t* p, uint16_t v)
{
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}
inline void writeLE32(uint8_t* p, uint32_t v)
{
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}

inline void encodeUdpAudioHeader(uint8_t* buf, const UdpAudioHeader& h)
{
    buf[0] = h.magic;
    buf[1] = h.version;
    buf[2] = h.flags;
    buf[3] = h.fec_group_size;
    writeLE32(buf + 4,  h.seq);
    writeLE32(buf + 8,  h.fec_group);
    writeLE32(buf + 12, h.timestamp_us);
    writeLE16(buf + 16, h.payload_len);
    writeLE16(buf + 18, h.reserved0);
    writeLE32(buf + 20, h.aux);
}

} // namespace msg
