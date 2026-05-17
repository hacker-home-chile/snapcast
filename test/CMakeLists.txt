find_package(Catch2 3 QUIET)

if(NOT Catch2_FOUND)
  message(STATUS "Catch2 not found, fetching it")
  include(FetchContent)

  FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.6.0 # or a later release
  )

  FetchContent_MakeAvailable(Catch2)
endif()

# Make test executable
set(TEST_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/test_main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_session_directory.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_udp_dedup.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_udp_registration.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_udp_fec.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_udp_broadcast_filter.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_message_framing.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_time_provider.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_volume_rpc_routing.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_active_selector.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_channel_slice.cpp
    ${CMAKE_SOURCE_DIR}/client/time_provider.cpp
    ${CMAKE_SOURCE_DIR}/common/stream_uri.cpp
    ${CMAKE_SOURCE_DIR}/common/base64.cpp
    ${CMAKE_SOURCE_DIR}/common/sample_format.cpp
    ${CMAKE_SOURCE_DIR}/common/utils/string_utils.cpp
    ${CMAKE_SOURCE_DIR}/common/utils/file_utils.cpp
    ${CMAKE_SOURCE_DIR}/server/authinfo.cpp
    # ${CMAKE_SOURCE_DIR}/server/jwt.cpp
    ${CMAKE_SOURCE_DIR}/server/session_directory.cpp
    ${CMAKE_SOURCE_DIR}/server/stream_session.cpp
    ${CMAKE_SOURCE_DIR}/server/udp_audio_server.cpp
    ${CMAKE_SOURCE_DIR}/server/streamreader/control_error.cpp
    ${CMAKE_SOURCE_DIR}/server/streamreader/properties.cpp
    ${CMAKE_SOURCE_DIR}/server/streamreader/metadata.cpp)

find_package(OpenSSL REQUIRED)

include_directories(SYSTEM ${Boost_INCLUDE_DIR})
# server/ headers do `#include "encoder/encoder.hpp"` etc. assuming the server
# dir is on the include path (set in server/CMakeLists.txt). The test target
# needs the same when it links server sources, plus client/ for the
# decoder round-trip tests.
include_directories(${CMAKE_SOURCE_DIR}/server)
include_directories(${CMAKE_SOURCE_DIR}/client)

# Opus codec tests: only build if libopus is available (OPUS_FOUND is set
# by the root CMakeLists.txt via pkg-config). Resampler.cpp is needed
# because OpusEncoder::initEncoder constructs one whenever the source rate
# isn't 48 kHz, and the destructor still touches it on the native path.
if(OPUS_FOUND)
  list(APPEND TEST_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/test_codec_roundtrip.cpp
    ${CMAKE_SOURCE_DIR}/common/resampler.cpp
    ${CMAKE_SOURCE_DIR}/server/encoder/opus_encoder.cpp
    ${CMAKE_SOURCE_DIR}/client/decoder/opus_decoder.cpp)
  include_directories(${OPUS_INCLUDE_DIRS})
  link_directories(${OPUS_LIBRARY_DIRS})
  if(SOXR_FOUND)
    include_directories(${SOXR_INCLUDE_DIRS})
    link_directories(${SOXR_LIBRARY_DIRS})
  endif()
endif()

add_executable(snapcast_test ${TEST_SOURCES})

if(ANDROID)
  target_link_libraries(snapcast_test log)
endif(ANDROID)

target_link_libraries(snapcast_test OpenSSL::Crypto OpenSSL::SSL)
target_link_libraries(snapcast_test Catch2::Catch2WithMain Catch2::Catch2)

if(OPUS_FOUND)
  target_link_libraries(snapcast_test ${OPUS_LIBRARIES})
  if(SOXR_FOUND)
    target_link_libraries(snapcast_test ${SOXR_LIBRARIES})
  endif()
endif()
