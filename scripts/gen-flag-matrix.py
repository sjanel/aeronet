#!/usr/bin/env python3
"""Print a GitHub Actions matrix of random AERONET CMake option combinations.

Output (stdout): {"include": [{"id": N, "cmake_args": "-DAERONET_...=ON ..."}, ...]}

To cover a new boolean option, just append its full CMake name to FLAGS.
The same --seed always yields the same combinations, so failures are reproducible.
"""
import argparse
import json
import random

FLAGS = [
    "AERONET_ENABLE_ASYNC_HANDLERS",
    "AERONET_ENABLE_GLAZE",
    "AERONET_ENABLE_HTTP2",
    "AERONET_ENABLE_BROTLI",
    "AERONET_ENABLE_ZLIB",
    "AERONET_ENABLE_ZLIBNG",
    "AERONET_ENABLE_ZSTD",
    "AERONET_ENABLE_OPENSSL",
    "AERONET_ENABLE_JWT",
    "AERONET_ENABLE_OPENTELEMETRY",
    "AERONET_ENABLE_SPDLOG",
    "AERONET_ENABLE_WEBSOCKET",
    "AERONET_ENABLE_HTTP_CLIENT",
    "AERONET_ENABLE_HTTP_SERVER",
    "AERONET_BUILD_BENCHMARKS",
]


def is_valid(cfg):
    """Mirror the constraints of the top-level CMakeLists.txt."""
    # FATAL_ERROR in CMakeLists.txt: client and server cannot both be disabled.
    if not cfg["AERONET_ENABLE_HTTP_CLIENT"] and not cfg["AERONET_ENABLE_HTTP_SERVER"]:
        return False
    # AERONET_ENABLE_JWT is a cmake_dependent_option: silently forced OFF unless OPENSSL and
    # GLAZE are ON. Only generate JWT=ON when it can really be ON.
    if cfg["AERONET_ENABLE_JWT"] and not (
        cfg["AERONET_ENABLE_OPENSSL"] and cfg["AERONET_ENABLE_GLAZE"]
    ):
        return False
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--count", type=int, default=30, help="number of combinations")
    ap.add_argument("--seed", type=int, default=0)
    args = ap.parse_args()

    rng = random.Random(args.seed)
    seen = set()
    include = []
    attempts = 0
    while len(include) < args.count and attempts < args.count * 200:
        attempts += 1
        cfg = {flag: rng.random() < 0.5 for flag in FLAGS}
        key = tuple(cfg.values())
        if key in seen or not is_valid(cfg):
            continue
        seen.add(key)
        cmake_args = " ".join(f"-D{flag}={'ON' if on else 'OFF'}" for flag, on in cfg.items())
        include.append({"id": len(include), "cmake_args": cmake_args})

    print(json.dumps({"include": include}))


if __name__ == "__main__":
    main()