ARG BASE_IMAGE=ubuntu:22.04@sha256:8a2321a92b438238f93b4346fff61bc4259c99fdd4d9b52ee86f153fee0c344f
FROM ${BASE_IMAGE}

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates git build-essential gcc-11 g++-11 cmake ninja-build \
    autoconf automake libtool pkg-config nasm curl python3 python3-dev \
    python3-setuptools python3-pip cython3 python3-yaml python3-prettytable \
    python3-sphinx libssl-dev libcurl4-openssl-dev libsnappy-dev zlib1g-dev \
    liblz4-dev libzstd-dev libaio-dev libudev-dev libblkid-dev libkeyutils-dev \
    xfslibs-dev libgoogle-perftools-dev libexpat1-dev libcap-ng-dev \
    libatomic-ops-dev libsqlite3-dev uuid-dev libxml2-dev valgrind \
    libhiredis-dev redis-server uuid-runtime iproute2 procps jq \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /ceph
CMD ["bash"]
