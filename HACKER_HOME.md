# Hacker Home Snapserver

This branch starts at official Snapcast commit `4fed179e177b251c67326e7b62a25c8d8fb2d1a9`.
Its only server changes are channel-slice streams, hidden parent streams (ported
from `addffad837064435a50115f21b62e18303c54279`), and parsing `chunk_ms` before
allocating the PCM chunk. Stream removal also logs the saved name rather than
dereferencing an erased iterator (caught by the dynamic matrix integration test). It does not contain the legacy fork UDP transport.

Production configuration and container packaging live separately in
`hacker-home-chile/hacker-home-server/snapcast`. ESP32 firmware lives in
`hacker-home-chile/espclient-hacker-home`. Standard Snapcast TCP ports apply.
Use PCM signed 16-bit stereo at 44100 or 48000 Hz, 10 ms chunks and a 500 ms
server buffer for the ESP32. Mono matrix outputs duplicate the selected channel.
The raw 16-channel matrix parent must remain hidden.

Build with `BUILD_TESTS=ON` and run `bin/snapcast_test` to include the channel
slice regression tests alongside upstream tests.
