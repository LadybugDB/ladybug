FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates \
        curl \
        gnupg \
        software-properties-common \
    && add-apt-repository ppa:ubuntu-toolchain-r/test -y \
    && apt-get update \
    && apt-get install -y --no-install-recommends \
        bash \
        build-essential \
        ccache \
        cmake \
        g++-13 \
        gcc-13 \
        gh \
        git \
        make \
        ninja-build \
        libssl-dev \
        openjdk-17-jdk \
        pkg-config \
        python3 \
        python3-pip \
        python3-venv \
        sudo \
        tar \
        unzip \
        xz-utils \
        zip \
        zstd \
    && update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-13 130 \
    && update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-13 130 \
    && JAVA_HOME_PATH="$(find /usr/lib/jvm -maxdepth 1 -type d -name 'java-17-openjdk-*' | head -n 1)" \
    && ln -s "$JAVA_HOME_PATH" /usr/lib/jvm/java-17-openjdk \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/*

ENV JAVA_HOME=/usr/lib/jvm/java-17-openjdk
ENV PATH="${JAVA_HOME}/bin:${PATH}"
