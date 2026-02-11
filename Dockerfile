FROM alpine:edge AS builder

RUN echo "https://dl-cdn.alpinelinux.org/alpine/edge/testing/" >> /etc/apk/repositories \
 && apk add --no-cache \
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

# Collect runtime shared library dependencies
RUN mkdir -p /runtime-libs \
 && ldd bin/snapserver | awk '/=>/ {print $3}' | sort -u | while read lib; do \
      cp "$lib" /runtime-libs/; \
    done

# Download snapweb
ARG SNAPWEB_VERSION=0.9.3
RUN wget -q "https://github.com/snapcast/snapweb/releases/download/v${SNAPWEB_VERSION}/snapweb.zip" \
 && mkdir -p /snapweb \
 && unzip -q snapweb.zip -d /snapweb \
 && rm snapweb.zip

FROM alpine:edge

RUN apk add --no-cache libstdc++

COPY --from=builder /runtime-libs/ /usr/lib/
COPY --from=builder /src/bin/snapserver /usr/bin/snapserver
COPY --from=builder /snapweb/ /usr/share/snapserver/snapweb/

# Default config: serve snapweb and use opus codec with FEC
RUN mkdir -p /etc && printf '[http]\n\
doc_root = /usr/share/snapserver/snapweb\n\
[stream]\n\
codec = opus\n' > /etc/snapserver.conf

EXPOSE 1704 1705 1706 1780

VOLUME ["/config", "/data"]

ENTRYPOINT ["snapserver"]
