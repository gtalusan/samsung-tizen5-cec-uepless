# Build cecdump: static armv7 musl binary for the Q90T (Tizen 5.5, kernel 4.1.10).
# The toolchain is the official musl.cc arm cross (soft-float, matches the
# proven dropbear-tv configuration). Build and test:
#
#   podman build -t cecdump-builder .
#   podman run --rm -v "$PWD:/src" -w /src cecdump-builder ./build.sh
#   podman run --rm -v "$PWD:/src" -w /src cecdump-builder ./test/test.sh
FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
    curl xz-utils ca-certificates file \
  && rm -rf /var/lib/apt/lists/*

RUN curl -fsSL -o /opt/cross.tgz https://musl.cc/arm-linux-musleabi-cross.tgz \
  && echo "d70c607101fee5330083463feacf5992892a85b826d1626094500e0c37ec7d25  /opt/cross.tgz" | sha256sum -c - \
  && tar -C /opt -xzf /opt/cross.tgz \
  && rm /opt/cross.tgz

ENV PATH="/opt/arm-linux-musleabi-cross/bin:${PATH}"
WORKDIR /src
CMD ["./build.sh"]
