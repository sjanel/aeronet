# Aeronet

Experimental C++23 minimal HTTP server library (Linux / epoll) – work in progress.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

## Run example

```bash
./build/examples/aeronet-minimal 8080
```

Then visit <http://localhost:8080/>

### Multi-reactor (SO_REUSEPORT) example

You can start several independent event loops on the same port (kernel load balances incoming connections) by using
`enablePortReuse(true)` and running one `HttpServer` per thread:

```bash
./build/examples/aeronet-multi 8080 4   # port 8080, 4 threads
```

Each thread owns its own listening socket (SO_REUSEPORT) and epoll instance – no shared locks in the accept path.
This is the simplest horizontal scaling strategy before introducing a worker pool.

## Roadmap (initial)

- [x] Project scaffold
- [x] Epoll event loop class
- [x] Minimal HTTP parser (request line + headers)
- [x] Basic response building
- [x] Graceful shutdown / runUntil
- [x] Tests (gtest) including move semantics & SO_REUSEPORT
- [x] Multi-reactor scaling via SO_REUSEPORT
- [ ] TLS (OpenSSL) support

## License
TBD (will add an open-source license later).
