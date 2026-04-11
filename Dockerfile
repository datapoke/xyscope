FROM debian:bookworm-slim

RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        build-essential                        \
        libpipewire-0.3-dev                    \
        libfftw3-dev                           \
        libsdl2-dev                            \
        libsdl2-ttf-dev                        \
        pipewire                               \
        wayland-protocols                      \
        libwayland-dev                         \
        libegl-dev                             \
    && rm -rf /var/lib/apt/lists/*

COPY ./                     /usr/src/xyscope
COPY ./docker-entrypoint.sh /usr/local/bin/

RUN    cd /usr/src/xyscope     \
    && rm -rf release/         \
    && mkdir -p release/linux/ \
    && make

ENTRYPOINT ["/usr/local/bin/docker-entrypoint.sh"]
