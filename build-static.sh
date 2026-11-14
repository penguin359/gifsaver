#!/bin/sh
# build-static.sh - build a fully static, musl-linked gifsaver binary
# inside an Alpine Linux container. The result has no dependency on
# glibc or any shared library and can be copied between machines.
#
# Audio is disabled in static builds: ALSA's plugin system relies on
# dlopen(), which does not work in static executables. Everything else
# (including an embedded GIF via GIFSAVER_EMBED_GIF) works normally.
#
# Usage:  ./build-static.sh            -> writes ./gifsaver-static
#         OUT=name ./build-static.sh   -> custom output name
#         IMAGE=alpine:x.y ./build-static.sh

set -eu
cd "$(dirname "$0")"

IMAGE="${IMAGE:-alpine:3.22}"
OUT="${OUT:-gifsaver-static}"

# libXau ships no static library in Alpine; built from pinned source.
LIBXAU_VERSION=1.0.12
LIBXAU_SHA256=74d0e4dfa3d39ad8939e99bda37f5967aba528211076828464d2777d477fc0fb

echo ">>> building static gifsaver in $IMAGE (audio disabled)"

docker run --rm -i \
    -v "$PWD":/src:ro \
    -v "$PWD":/out \
    -e HOST_UID="$(id -u)" \
    -e HOST_GID="$(id -g)" \
    -e OUT="$OUT" \
    -e LIBXAU_VERSION="$LIBXAU_VERSION" \
    -e LIBXAU_SHA256="$LIBXAU_SHA256" \
    "$IMAGE" /bin/sh -eu <<'INNER'
apk add --no-cache --quiet build-base curl xz \
    libx11-dev libx11-static libxcb-static libxdmcp-dev \
    giflib-dev giflib-static

# Build a static libXau (not packaged as a .a by Alpine).
cd /tmp
curl -fsSL -o libXau.tar.xz \
    "https://www.x.org/releases/individual/lib/libXau-$LIBXAU_VERSION.tar.xz"
echo "$LIBXAU_SHA256  libXau.tar.xz" | sha256sum -c -
tar xJf libXau.tar.xz
cd "libXau-$LIBXAU_VERSION"
./configure --prefix=/usr --enable-static --disable-shared --quiet
make --quiet
make --quiet install

# Build gifsaver statically; -D overrides force the ALSA code out.
cp -r /src /build
cd /build
make clean
make CFLAGS="-O2 -Wall -Wextra -static \
             -DGIFSAVER_AUDIO=0 -DGIFSAVER_EMBED_WAV=0" \
     LDLIBS="-lX11 -lxcb -lXau -lXdmcp -lgif"

# Verify the result really is static.
file gifsaver
if ldd gifsaver >/dev/null 2>&1; then
    echo "error: binary is still dynamically linked" >&2
    exit 1
fi
strip gifsaver

cp gifsaver "/out/$OUT"
chown "$HOST_UID:$HOST_GID" "/out/$OUT"
echo ">>> wrote $OUT"
INNER

echo ">>> done: $(file "$OUT")"
