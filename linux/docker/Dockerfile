# Parametrized scenario image: toolchain for the given base distro.
# Build: docker build --build-arg BASE_IMAGE=ubuntu:22.04 -f docker/Dockerfile -t interpose:ubuntu-22.04 .
ARG BASE_IMAGE
FROM ${BASE_IMAGE}

RUN if command -v apt-get >/dev/null 2>&1; then \
        apt-get update && \
        DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
            build-essential cmake ninja-build meson autoconf automake libtool \
            python3 python3-pip && \
        rm -rf /var/lib/apt/lists/*; \
    elif command -v apk >/dev/null 2>&1; then \
        apk add --no-cache \
            build-base cmake ninja meson autoconf automake libtool python3; \
    else \
        echo "unsupported base image" >&2; exit 1; \
    fi

WORKDIR /src
