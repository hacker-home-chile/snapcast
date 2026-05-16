/***
    udp-music: tests for the FEC XOR accumulator in UdpAudioServer.

    The contract this code must uphold for the ESP/lwIP receiver:
      1. After N data packets in an FEC group, the server emits a parity
         packet such that XOR(data_1..data_N, parity) == 0, with shorter
         data packets implicitly zero-padded to the longest length.
      2. The parity packet's `length_xor` field is the XOR of all data
         payload_lens in the group (truncated to u16), so a receiver can
         recover the *length* of a missing packet, not just its bytes.
      3. After a group closes, the per-client transport state is reset
         (fec_idx=0, length_xor=0, xor accum cleared, xor_max_len=0) and
         the group counter is bumped.

    All exercised through the static appendFecData() helper so we don't
    need a real UDP socket.
***/

#include "server/udp_audio_server.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <vector>


using ClientState = UdpAudioServer::ClientState;
using FecGroupClose = UdpAudioServer::FecGroupClose;


namespace
{

std::vector<uint8_t> bytes(std::initializer_list<uint8_t> b)
{
    return std::vector<uint8_t>(b);
}

/// XOR @p a into @p out (zero-extending @p out as needed).
void xorInto(std::vector<uint8_t>& out, const std::vector<uint8_t>& a)
{
    if (out.size() < a.size())
        out.resize(a.size(), 0);
    for (size_t i = 0; i < a.size(); ++i)
        out[i] ^= a[i];
}

} // namespace


TEST_CASE("FEC: parity == XOR of data packets (basic invariant)")
{
    ClientState cs;
    const uint8_t fec_n = 4;

    const auto a = bytes({0x01, 0x02, 0x03, 0x04});
    const auto b = bytes({0xff, 0xee, 0xdd, 0xcc});
    const auto c = bytes({0x10, 0x20, 0x30, 0x40});
    const auto d = bytes({0xa5, 0x5a, 0xa5, 0x5a});

    REQUIRE_FALSE(UdpAudioServer::appendFecData(cs, a.data(), a.size(), fec_n).has_value());
    REQUIRE_FALSE(UdpAudioServer::appendFecData(cs, b.data(), b.size(), fec_n).has_value());
    REQUIRE_FALSE(UdpAudioServer::appendFecData(cs, c.data(), c.size(), fec_n).has_value());
    auto close = UdpAudioServer::appendFecData(cs, d.data(), d.size(), fec_n);

    REQUIRE(close.has_value());
    REQUIRE(close->parity.size() == 4);

    // parity ^ all data must be zero (the recovery invariant)
    for (size_t i = 0; i < 4; ++i)
    {
        const uint8_t x = a[i] ^ b[i] ^ c[i] ^ d[i] ^ close->parity[i];
        INFO("byte " << i);
        REQUIRE(x == 0);
    }
}


TEST_CASE("FEC: recovery — XOR of survivors + parity == lost packet")
{
    // The whole point of an XOR-parity FEC group: a receiver that sees N-1
    // data packets plus the parity can recover the missing one.
    ClientState cs;
    const uint8_t fec_n = 4;

    const auto a = bytes({0x01, 0x02, 0x03, 0x04});
    const auto b = bytes({0xff, 0xee, 0xdd, 0xcc});  // pretend this one was dropped on the wire
    const auto c = bytes({0x10, 0x20, 0x30, 0x40});
    const auto d = bytes({0xa5, 0x5a, 0xa5, 0x5a});

    UdpAudioServer::appendFecData(cs, a.data(), a.size(), fec_n);
    UdpAudioServer::appendFecData(cs, b.data(), b.size(), fec_n);
    UdpAudioServer::appendFecData(cs, c.data(), c.size(), fec_n);
    auto close = UdpAudioServer::appendFecData(cs, d.data(), d.size(), fec_n);
    REQUIRE(close.has_value());

    // Receiver-side recovery: XOR of (a, c, d, parity) must reproduce b.
    std::vector<uint8_t> recovered(4, 0);
    xorInto(recovered, a);
    xorInto(recovered, c);
    xorInto(recovered, d);
    xorInto(recovered, close->parity);

    REQUIRE(recovered == b);
}


TEST_CASE("FEC: variable-length payloads pad to the longest")
{
    ClientState cs;
    const uint8_t fec_n = 4;

    const auto a = bytes({0xaa, 0xbb});                              // 2 bytes
    const auto b = bytes({0x01, 0x02, 0x03, 0x04, 0x05, 0x06});      // 6 bytes — sets xor_max_len
    const auto c = bytes({0xff});                                    // 1 byte
    const auto d = bytes({0x10, 0x20, 0x30, 0x40});                  // 4 bytes

    UdpAudioServer::appendFecData(cs, a.data(), a.size(), fec_n);
    UdpAudioServer::appendFecData(cs, b.data(), b.size(), fec_n);
    UdpAudioServer::appendFecData(cs, c.data(), c.size(), fec_n);
    auto close = UdpAudioServer::appendFecData(cs, d.data(), d.size(), fec_n);

    REQUIRE(close.has_value());
    REQUIRE(close->parity.size() == 6);  // longest payload determines parity length

    // Recovery invariant still holds, with shorter payloads zero-padded.
    std::vector<uint8_t> reconstructed(6, 0);
    xorInto(reconstructed, a);
    xorInto(reconstructed, b);
    xorInto(reconstructed, c);
    xorInto(reconstructed, d);
    xorInto(reconstructed, close->parity);
    REQUIRE(reconstructed == std::vector<uint8_t>(6, 0));
}


TEST_CASE("FEC: length_xor encodes payload lengths (u16-truncated)")
{
    ClientState cs;
    const uint8_t fec_n = 4;
    const std::vector<size_t> lens = {137, 200, 65535, 1};

    const std::vector<uint8_t> filler(70000, 0xab);
    for (size_t i = 0; i < lens.size() - 1; ++i)
        UdpAudioServer::appendFecData(cs, filler.data(), lens[i], fec_n);
    auto close = UdpAudioServer::appendFecData(cs, filler.data(), lens.back(), fec_n);

    REQUIRE(close.has_value());

    uint16_t expected = 0;
    for (auto l : lens)
        expected ^= static_cast<uint16_t>(l);
    REQUIRE(close->length_xor == static_cast<uint32_t>(expected));
}


TEST_CASE("FEC: state reset and fec_group increment after close")
{
    ClientState cs;
    const uint8_t fec_n = 2;

    const auto p = bytes({1, 2, 3, 4, 5});

    REQUIRE(cs.fec_group == 0);
    REQUIRE_FALSE(UdpAudioServer::appendFecData(cs, p.data(), p.size(), fec_n).has_value());
    REQUIRE(cs.fec_idx == 1);
    REQUIRE(cs.xor_max_len == 5);

    auto close = UdpAudioServer::appendFecData(cs, p.data(), p.size(), fec_n);
    REQUIRE(close.has_value());
    REQUIRE(close->fec_group == 0);

    // Post-close: state reset, group counter advanced.
    REQUIRE(cs.fec_idx == 0);
    REQUIRE(cs.length_xor == 0);
    REQUIRE(cs.xor_max_len == 0);
    REQUIRE(cs.fec_group == 1);
    REQUIRE(std::all_of(cs.xor_accum.begin(), cs.xor_accum.end(),
                        [](uint8_t b) { return b == 0; }));
}


TEST_CASE("FEC: groups are independent across multiple closes")
{
    ClientState cs;
    const uint8_t fec_n = 2;

    const auto g1a = bytes({0x11, 0x22});
    const auto g1b = bytes({0xaa, 0xbb});
    const auto g2a = bytes({0x33, 0x44, 0x55});
    const auto g2b = bytes({0x66, 0x77, 0x88});

    UdpAudioServer::appendFecData(cs, g1a.data(), g1a.size(), fec_n);
    auto close1 = UdpAudioServer::appendFecData(cs, g1b.data(), g1b.size(), fec_n);
    REQUIRE(close1.has_value());
    REQUIRE(close1->fec_group == 0);
    REQUIRE(close1->parity == std::vector<uint8_t>{0x11 ^ 0xaa, 0x22 ^ 0xbb});

    UdpAudioServer::appendFecData(cs, g2a.data(), g2a.size(), fec_n);
    auto close2 = UdpAudioServer::appendFecData(cs, g2b.data(), g2b.size(), fec_n);
    REQUIRE(close2.has_value());
    REQUIRE(close2->fec_group == 1);
    // Second group is wholly independent of first — no carryover state.
    REQUIRE(close2->parity == std::vector<uint8_t>{0x33 ^ 0x66, 0x44 ^ 0x77, 0x55 ^ 0x88});
}


TEST_CASE("FEC: fec_n=1 closes group on every data packet")
{
    // Pathological / minimum configuration: every data packet immediately
    // emits a parity (which equals the data itself).
    ClientState cs;
    const uint8_t fec_n = 1;
    const auto p = bytes({0xde, 0xad, 0xbe, 0xef});

    auto close = UdpAudioServer::appendFecData(cs, p.data(), p.size(), fec_n);
    REQUIRE(close.has_value());
    REQUIRE(close->parity == p);
    REQUIRE(close->fec_group == 0);
    REQUIRE(cs.fec_group == 1);

    auto close2 = UdpAudioServer::appendFecData(cs, p.data(), p.size(), fec_n);
    REQUIRE(close2.has_value());
    REQUIRE(close2->fec_group == 1);
    REQUIRE(cs.fec_group == 2);
}


TEST_CASE("FEC: clients are independent (no shared accumulator)")
{
    // Two clients running in parallel — feeding one must not affect the
    // other. Trivially true because ClientState is per-client, but a
    // refactor that accidentally hoisted any of these fields to shared
    // state would break receivers silently.
    ClientState cs_a, cs_b;
    const uint8_t fec_n = 2;

    const auto a = bytes({0x10, 0x20});
    const auto b = bytes({0xff, 0xee});

    UdpAudioServer::appendFecData(cs_a, a.data(), a.size(), fec_n);
    auto close_b = UdpAudioServer::appendFecData(cs_b, b.data(), b.size(), fec_n);
    REQUIRE_FALSE(close_b.has_value());
    REQUIRE(cs_a.fec_idx == 1);
    REQUIRE(cs_b.fec_idx == 1);

    auto close_a = UdpAudioServer::appendFecData(cs_a, a.data(), a.size(), fec_n);
    REQUIRE(close_a.has_value());
    REQUIRE(close_a->parity == std::vector<uint8_t>{0x00, 0x00});  // a^a = 0
    // cs_b is untouched by cs_a's close.
    REQUIRE(cs_b.fec_idx == 1);
    REQUIRE(cs_b.fec_group == 0);
}
