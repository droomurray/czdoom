# Builds czdoom for the CardputerZero (Debian trixie, arm64) on any Docker host.
#
#   docker build --output type=local,dest=dist .                  -> dist/czdoom_<ver>_arm64.deb
#   docker build --target smoke-out --output type=local,dest=out . -> test screenshots
#
# On an arm64 host the build is native; on x86 it cross-compiles, so no QEMU
# emulation is needed either way.

FROM debian:trixie AS toolchain
ENV DEBIAN_FRONTEND=noninteractive
RUN set -eux; \
    apt-get update; \
    if [ "$(dpkg --print-architecture)" = arm64 ]; then \
        apt-get install -y --no-install-recommends build-essential pkg-config git ca-certificates \
            libsdl2-dev libsdl2-mixer-dev; \
        printf 'CROSS_COMPILE=\nPKG_CONFIG=pkg-config\n' >/etc/czdoom-toolchain; \
    else \
        dpkg --add-architecture arm64; \
        apt-get update; \
        apt-get install -y --no-install-recommends build-essential crossbuild-essential-arm64 \
            pkg-config git ca-certificates libsdl2-dev:arm64 libsdl2-mixer-dev:arm64; \
        printf 'CROSS_COMPILE=aarch64-linux-gnu-\nPKG_CONFIG=aarch64-linux-gnu-pkg-config\n' >/etc/czdoom-toolchain; \
    fi; \
    rm -rf /var/lib/apt/lists/*

FROM toolchain AS build
WORKDIR /src
COPY Makefile LICENSE ./
COPY src ./src
COPY packaging ./packaging
RUN set -eux; . /etc/czdoom-toolchain; \
    make -j"$(nproc)" deb CROSS_COMPILE="$CROSS_COMPILE" PKG_CONFIG="$PKG_CONFIG"; \
    dpkg-deb --info dist/*.deb; dpkg-deb --contents dist/*.deb

# Runs the game for a few seconds against a fake 320x170 RGB565 framebuffer
# and scripted key events, using the build host's own architecture.
FROM debian:trixie AS smoke
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends build-essential pkg-config git \
        ca-certificates libsdl2-dev libsdl2-mixer-dev freedoom python3 \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY Makefile LICENSE ./
COPY src ./src
COPY packaging ./packaging
COPY tests ./tests
RUN make -j"$(nproc)" && tests/smoke.sh build/czdoom /out

FROM scratch AS smoke-out
COPY --from=smoke /out/ /

FROM scratch
COPY --from=build /src/dist/ /
