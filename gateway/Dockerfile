FROM ubuntu:24.04 AS builder

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        cmake \
        libcurl4-openssl-dev \
        libpaho-mqtt-dev \
        libpaho-mqttpp-dev \
        libssl-dev \
        nlohmann-json3-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /source
COPY . .

RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
    && cmake --build build --parallel \
    && ctest --test-dir build --output-on-failure

FROM ubuntu:24.04 AS runtime

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates \
        libcurl4t64 \
        libpaho-mqtt1.3 \
        libpaho-mqttpp3-1 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /source/build/agro-gateway /usr/local/bin/agro-gateway

RUN useradd --system --uid 10001 --no-create-home gateway
USER gateway

ENTRYPOINT ["/usr/local/bin/agro-gateway"]
