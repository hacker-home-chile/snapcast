FROM alpine:3.21 AS builder

RUN apk add --no-cache \
    build-base cmake ninja ccache \
    alsa-lib-dev avahi-dev boost-dev expat-dev flac-dev \
    libvorbis-dev openssl-dev opus-dev soxr-dev

WORKDIR /src
COPY . .

RUN cmake -S . -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_CLIENT=OFF \
    -DBUILD_SERVER=ON \
    -DBUILD_WITH_PULSE=OFF \
    -DBUILD_WITH_JACK=OFF \
    -DBUILD_WITH_PIPEWIRE=OFF \
 && cmake --build build --parallel

FROM alpine:3.21

RUN apk add --no-cache \
    alsa-lib avahi-libs boost1.84-program_options expat flac-libs \
    libvorbis openssl opus soxr libstdc++

COPY --from=builder /src/build/bin/snapserver /usr/bin/snapserver

EXPOSE 1704 1705 1706 1780 4100/udp

VOLUME ["/config", "/data"]

ENTRYPOINT ["snapserver"]
