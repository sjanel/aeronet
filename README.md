# Aeronet

Experimental C++23 minimal HTTP server library (Linux / epoll) – work in progress.

## Build

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

## Run example

```
./build/examples/aeronet-minimal 8080
```

Then visit http://localhost:8080/

## Roadmap (initial)

- [x] Project scaffold
- [ ] Socket utilities abstraction
- [ ] Epoll event loop class
- [ ] Minimal HTTP parser (request line + headers)
- [ ] Basic response building
- [ ] Graceful shutdown
- [ ] Tests hitting example server
- [ ] TLS (OpenSSL) support

## License
TBD (will add an open-source license later).
