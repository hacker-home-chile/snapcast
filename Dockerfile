FROM alpine:3.21 AS snapweb

# Fetch the pre-built snapweb UI bundle matching upstream snapserver releases.
RUN apk add --no-cache curl unzip \
 && mkdir -p /snapweb \
 && curl -fsSL https://github.com/snapcast/snapweb/releases/download/v0.9.3/snapweb.zip \
      -o /tmp/snapweb.zip \
 && unzip -d /snapweb /tmp/snapweb.zip


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
    libvorbis openssl opus soxr libstdc++ \
    socat

COPY --from=builder /src/bin/snapserver /usr/bin/snapserver
COPY --from=snapweb  /snapweb           /usr/share/snapweb

EXPOSE 1704 1705 1780 4100/udp

VOLUME ["/config", "/data"]

ENTRYPOINT ["snapserver"]
CMD ["-c", "/config/snapserver.conf"]
