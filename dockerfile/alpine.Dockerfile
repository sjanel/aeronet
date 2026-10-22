# simple alpine build with gcc (default) or clang to mimick ci build
# alpine does not support asan with gcc, if you need asan, you can switch to clang
FROM alpine:latest

RUN set -e; \
    for i in 1 2 3; do \
      apk update && \
      apk add --no-cache build-base cmake ninja git bash python3 linux-headers \
        openssl-dev protobuf-dev musl-dev curl-dev \
      compiler-rt && \
      break || { [ $i -lt 3 ] && sleep $((2**i)) || false; }; \
    done

ENV CC=gcc
ENV CXX=g++

COPY aeronet /aeronet/aeronet/
COPY cmake /aeronet/cmake/
COPY CMakeLists.txt /aeronet/CMakeLists.txt
COPY tests /aeronet/tests/
COPY examples /aeronet/examples/

WORKDIR /aeronet

RUN cmake -S . -B build-alpine -G Ninja \
            -D CMAKE_BUILD_TYPE=Debug \
            -D AERONET_BUILD_TESTS=ON \
            -D AERONET_BUILD_EXAMPLES=ON \
            -D AERONET_BUILD_BENCHMARKS=OFF \
            -D AERONET_ENABLE_ASAN=OFF \
            -D AERONET_ENABLE_ASYNC_HANDLERS=ON \
            -D AERONET_ENABLE_GLAZE=ON \
            -D AERONET_ENABLE_ADDITIONAL_MEMORY_CHECKS=ON \
            -D AERONET_ENABLE_HTTP2=ON \
            -D AERONET_ENABLE_BROTLI=ON \
            -D AERONET_ENABLE_ZLIB=ON \
            -D AERONET_ENABLE_ZSTD=ON \
            -D AERONET_ENABLE_OPENTELEMETRY=ON \
            -D AERONET_ENABLE_OPENSSL=ON \
            -D AERONET_ENABLE_SPDLOG=ON \
            -D AERONET_ENABLE_WEBSOCKET=ON

WORKDIR /aeronet/build-alpine

RUN ninja

RUN ctest --output-on-failure --repeat until-fail:3 --timeout 60 -j
