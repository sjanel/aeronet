# simple alpine build with gcc (default) or clang to mimick ci build
# alpine does not support asan with gcc, if you need asan, you can switch to clang
FROM alpine:latest

RUN apk update && \
    apk add --no-cache build-base cmake ninja git bash python3 linux-headers \
      openssl-dev protobuf-dev musl-dev curl-dev \
    compiler-rt

ENV CC=gcc
ENV CXX=g++

COPY aeronet /aeronet/aeronet/
COPY cmake /aeronet/cmake/
COPY CMakeLists.txt /aeronet/CMakeLists.txt
COPY tests /aeronet/tests/
COPY examples /aeronet/examples/

WORKDIR /aeronet

RUN cmake -S . -B build-alpine -G Ninja \
            -DCMAKE_BUILD_TYPE=Debug \
            -DAERONET_BUILD_TESTS=ON \
            -DAERONET_BUILD_EXAMPLES=ON \
            -DAERONET_BUILD_BENCHMARKS=OFF \
            -DAERONET_ENABLE_ASAN=OFF \
            -DAERONET_ENABLE_ASYNC_HANDLERS=ON \
            -DAERONET_ENABLE_GLAZE=ON \
            -DAERONET_ENABLE_ADDITIONAL_MEMORY_CHECKS=ON \
            -DAERONET_ENABLE_HTTP2=ON \
            -DAERONET_ENABLE_BROTLI=ON \
            -DAERONET_ENABLE_ZLIB=ON \
            -DAERONET_ENABLE_ZSTD=ON \
            -DAERONET_ENABLE_OPENTELEMETRY=ON \
            -DAERONET_ENABLE_OPENSSL=ON \
            -DAERONET_ENABLE_SPDLOG=ON \
            -DAERONET_ENABLE_WEBSOCKET=ON

WORKDIR /aeronet/build-alpine

RUN ninja

RUN ctest --output-on-failure --repeat until-fail:3 --timeout 100
