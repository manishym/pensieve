FROM ubuntu:24.04 AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake pkg-config liburing-dev ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt ./
COPY src/ src/
COPY tests/ tests/

RUN cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXE_LINKER_FLAGS="-static-libgcc -static-libstdc++" \
    && cmake --build build -j$(nproc) --target pensieve_server

# ─── Runtime ──────────────────────────────────────────────────────────────────
FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
        liburing2 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /src/build/pensieve_server /usr/local/bin/pensieve_server

ENV PENSIEVE_HOST=0.0.0.0 \
    PENSIEVE_DATA_PORT=11211 \
    PENSIEVE_GOSSIP_PORT=7946 \
    PENSIEVE_MEMORY_MB=64 \
    PENSIEVE_SEED="" \
    PENSIEVE_SEED_DATA_PORT=11211

EXPOSE 11211/tcp 7946/udp

ENTRYPOINT ["pensieve_server"]
