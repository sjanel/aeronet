# TechEmpower Aeronet Benchmark - distroless style build
# Build from the repository root so the COPY paths resolve:
#   docker build -f benchmarks/techempower/aeronet/docker/Dockerfile .
FROM ubuntu:24.04 AS build

ARG BUILD_MODE=Release

RUN apt-get update && \
    apt-get upgrade -y && \
    apt-get install -y --no-install-recommends \
        build-essential \
        ninja-build \
        libssl-dev \
        zlib1g-dev \
        libcurl4-openssl-dev \
        cmake \
        git \
        ca-certificates && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY CMakeLists.txt /app/
COPY VERSION /app/
COPY cmake /app/cmake
COPY aeronet /app/aeronet
COPY benchmarks/techempower/aeronet /app/benchmarks/techempower/aeronet

WORKDIR /app/build
RUN cmake -GNinja \
      -DCMAKE_BUILD_TYPE=${BUILD_MODE} \
      -DAERONET_ENABLE_GLAZE=ON \
      -DAERONET_ENABLE_HTTP2=OFF \
      -DAERONET_ENABLE_ASYNC_HANDLERS=OFF \
      -DAERONET_ENABLE_BROTLI=OFF \
      -DAERONET_ENABLE_ZLIB=OFF \
      -DAERONET_ENABLE_ZSTD=OFF \
      -DAERONET_ENABLE_OPENSSL=OFF \
      -DAERONET_ENABLE_OPENTELEMETRY=OFF \
      -DAERONET_ENABLE_WEBSOCKET=OFF \
      -DAERONET_BENCH_ENABLE_OATPP=OFF \
      -DAERONET_BENCH_ENABLE_DROGON=OFF \
      -DAERONET_BENCH_ENABLE_HTTPLIB=OFF \
      -DAERONET_BENCH_ENABLE_PISTACHE=OFF \
      -DAERONET_BENCH_ENABLE_CROW=OFF \
      -DAERONET_BUILD_TESTS=OFF \
      -DAERONET_BUILD_EXAMPLES=OFF \
      -DAERONET_BUILD_BENCHMARKS=ON \
      ..

RUN cmake --build .

# Collect runtime dependencies for the benchmark binary
RUN ldd /app/build/benchmarks/techempower/aeronet/benchmark_techempower \
    | tr -s '[:blank:]' '\n' \
    | grep '^/' \
    | xargs -I % sh -c 'mkdir -p /deps$(dirname %); cp % /deps%;'

FROM scratch

COPY --from=build /etc/ssl/certs /etc/ssl/certs
COPY --from=build /deps /
COPY --from=build /app/build/benchmarks/techempower/aeronet/benchmark_techempower /app/benchmark_techempower

EXPOSE 8080

ENTRYPOINT ["/app/benchmark_techempower"]
