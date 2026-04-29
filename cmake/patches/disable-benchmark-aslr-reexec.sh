#!/bin/sh
# google-benchmark v1.9.5 MaybeReenterWithoutASLR() calls execv() in a loop
# when personality(ADDR_NO_RANDOMIZE) appears to succeed but the kernel/AppArmor
# prevents it from taking effect. This neutralizes the execv call.
sed -i 's/execv(argv\[0\], argv);/\/\/ execv disabled (AppArmor compat)/' src/benchmark.cc
