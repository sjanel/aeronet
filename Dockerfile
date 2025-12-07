FROM alpine
RUN sed -i 's/https/http/' /etc/apk/repositories
RUN apk update
RUN apk add --no-cache build-base cmake ninja git bash python3 linux-headers \
            openssl-dev zlib-dev zstd-dev brotli-dev curl-dev protobuf-dev musl-dev

WORKDIR /app

COPY aeronet ./aeronet/
COPY tests ./tests/
COPY CMakeLists.txt .
COPY cmake ./cmake/
COPY examples ./examples/
RUN pwd && ls -ltr

RUN cmake -S . -B build-alpine -G Ninja \
            -DCMAKE_BUILD_TYPE=Debug \
            -DAERONET_BUILD_TESTS=ON \
            -DAERONET_BUILD_EXAMPLES=ON \
            -DAERONET_BUILD_BENCHMARKS=OFF \
            -DAERONET_ENABLE_ASAN=OFF \
            -DAERONET_ENABLE_SPDLOG=ON \
            -DAERONET_ENABLE_OPENSSL=ON \
            -DAERONET_ENABLE_KTLS=ON \
            -DAERONET_ENABLE_BROTLI=ON \
            -DAERONET_ENABLE_ZLIB=ON \
            -DAERONET_ENABLE_ZSTD=ON \
            -DAERONET_ENABLE_OPENTELEMETRY=ON

RUN cmake --build build-alpine --parallel

RUN ctest --test-dir build-alpine --output-on-failure --repeat until-fail:3 --timeout 100
