#!/usr/bin/env python3
"""Print a GitHub Actions matrix of AERONET CMake option combinations.

Output (stdout): {"include": [{"id": N, "kind": "wave|random", "cmake_args": "-DAERONET_...=ON ..."}, ...]}

Two kinds of combinations are generated:

  * "wave": a slice of a deterministic, coverage-guided sequence. Each combination of the
    sequence is the best of POOL_SIZE distinct valid candidates, i.e. the one that covers the
    most not-yet-covered (or least covered) ON/OFF assignments of any STRENGTH options. This is
    an empirical result, not a guarantee of the algorithm: on the current constrained 15-option
    space, simulations need roughly half as many combinations as uniform sampling of the valid
    combinations to reach 95-99 % of the feasible 5-option interactions (88621 of them; the 96096
    theoretical ones include impossible patterns).
    The sequence only depends on the options and constraints below (never on test results), so
    consecutive runs simply take consecutive slices of it: wave N is combinations
    [N * wave_count, (N + 1) * wave_count) of the epoch. Each epoch independently builds its own
    sequence: interaction counts are reset between epochs, which diversifies the combinations and
    means the process never ends. An epoch (EPOCH_MIN_SIZE combinations) has covered all the
    feasible interactions in every simulated case (full coverage after 310-373 combinations).
  * "random": --random-count additional random combinations. --seed identifies the campaign; the
    wave is mixed in, so every wave gets different (but reproducible) random combinations.

Maintenance:
  * To cover a new boolean option, append its full CMake name to FLAGS.
  * --wave-count must stay constant during a campaign (it defines the slicing of the sequence).
  * Adding/removing an option or changing is_valid() changes the sequence but does NOT reset the
    wave number: set FIRST_RUN_NUMBER in .github/workflows/flag-combinations.yml to the next run
    number to restart from wave 0. Bumping SALT also changes every sequence (same remark).

Requires numpy (only when --wave-count > 0).
"""
import argparse
import hashlib
import itertools
import json
import math
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

STRENGTH = 5           # size of the option subsets whose ON/OFF assignments are covered
POOL_SIZE = 500        # candidates examined to select each combination
EPOCH_MIN_SIZE = 420   # length of one guided sequence (rounded up to a whole number of waves)
# Changing the salt changes every sequence (see the maintenance notes above about the wave number).
SALT = "aeronet-flag-waves-v2"


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


def to_args(bits):
    return " ".join(f"-D{flag}={'ON' if on else 'OFF'}" for flag, on in zip(FLAGS, bits))


def candidates(epoch, step):
    """Endless stream of valid combinations derived from a hash: reproducible on any machine."""
    counter = 0
    while True:
        digest = hashlib.sha256(f"{SALT}:{epoch}:{step}:{counter}".encode()).digest()
        counter += 1
        value = int.from_bytes(digest, "big")  # 256 bits: enough for up to 256 options
        bits = tuple(bool((value >> i) & 1) for i in range(len(FLAGS)))
        if is_valid(dict(zip(FLAGS, bits))):
            yield bits


def guided_sequence(epoch, length):
    """First `length` combinations of the coverage-guided sequence of `epoch`.

    Greedy selection: at each step, among POOL_SIZE candidates pick the one maximising
    sum(1 / (1 + times the interaction was already selected)) over all its interactions.
    Integer arithmetic keeps the result identical across numpy versions and platforms.
    """
    import numpy as np

    n = len(FLAGS)
    k = min(STRENGTH, n)
    groups = np.array(list(itertools.combinations(range(n), k)))  # (G, k) option indices
    offsets = np.arange(len(groups), dtype=np.int64) << k         # first slot of each group
    counts = np.zeros(len(groups) << k, dtype=np.int64)           # one slot per (group, ON/OFF pattern)
    chosen, chosen_set = [], set()

    for step in range(length):
        pool, pool_set = [], set()
        for bits in candidates(epoch, step):
            if bits in chosen_set or bits in pool_set:
                continue
            pool.append(bits)
            pool_set.add(bits)
            if len(pool) == POOL_SIZE:
                break
        matrix = np.array(pool, dtype=np.uint8)                   # (M, n)
        codes = np.zeros((len(pool), len(groups)), dtype=np.int64)
        for j in range(k):
            codes += matrix[:, groups[:, j]].astype(np.int64) << j
        ids = codes + offsets                                     # (M, G) interaction ids
        weights = (1 << 20) // (1 + counts)
        best = int(np.argmax(weights[ids].sum(axis=1)))           # first best on ties
        counts[ids[best]] += 1
        chosen.append(pool[best])
        chosen_set.add(pool[best])
    return chosen


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--wave-count", type=int, default=20,
                    help="combinations per wave (0 = no waves); keep constant during a campaign")
    ap.add_argument("--wave", type=int, default=0, help="wave number")
    ap.add_argument("--random-count", type=int, default=0, help="additional random combinations")
    ap.add_argument("--seed", type=int, default=0, help="seed of the random combinations")
    ap.add_argument("--info-file", help="append a one-line description of the selection to this file")
    args = ap.parse_args()
    if args.wave < 0:
        ap.error("--wave must be >= 0 (is FIRST_RUN_NUMBER in the workflow greater than the run number?)")

    include = []
    seen = set()

    def add(bits, kind):
        if bits in seen:
            return
        seen.add(bits)
        include.append({"id": len(include), "kind": kind, "cmake_args": to_args(bits)})

    info = f"Random seed: {args.seed} ({args.random_count} random combinations)."

    if args.wave_count > 0:
        waves_per_epoch = math.ceil(EPOCH_MIN_SIZE / args.wave_count)
        epoch, slot = divmod(args.wave, waves_per_epoch)
        start = slot * args.wave_count
        for bits in guided_sequence(epoch, start + args.wave_count)[start:]:
            add(bits, "wave")
        info = (
            f"Coverage-guided wave {slot + 1}/{waves_per_epoch} of epoch #{epoch + 1} "
            f"({args.wave_count} combinations per wave). " + info
        )

    # --seed identifies the campaign; mixing in the wave gives different randoms in every wave.
    digest = hashlib.sha256(f"{SALT}:random:{args.seed}:{args.wave}".encode()).digest()
    rng = random.Random(int.from_bytes(digest[:8], "big"))
    wanted = len(include) + args.random_count
    attempts = 0
    while len(include) < wanted and attempts < args.random_count * 200:
        attempts += 1
        bits = tuple(rng.random() < 0.5 for _ in FLAGS)
        if is_valid(dict(zip(FLAGS, bits))):
            add(bits, "random")

    if len(include) > 256:
        raise SystemExit(f"matrix too large ({len(include)} > 256)")

    if args.info_file:
        with open(args.info_file, "a", encoding="utf-8") as f:
            f.write(info + "\n")
    print(json.dumps({"include": include}))


if __name__ == "__main__":
    main()