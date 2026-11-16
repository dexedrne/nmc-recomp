#!/bin/bash
# compile-only check of the hand-written runtime for arm64 on the Mac mini (windowless, nice, one file at a time).  Output: pass/fail per file.
# usage: tools/arm64check.sh   (rsyncs runtime/ + lifter-rt/ to ~/khnmc-work/rt-arm64check on the Mac, compiles with clang -arch arm64 in bias mode KH_XLATE_MODE=2)
set -u
cd "$(dirname "$0")/.." || exit 1
MAC=user@mac-mini
ssh -o BatchMode=yes $MAC 'mkdir -p ~/khnmc-work/rt-arm64check' || exit 1
rsync -a --delete -e "ssh -o BatchMode=yes" runtime lifter-rt $MAC:~/khnmc-work/rt-arm64check/ || exit 1
ssh -o BatchMode=yes $MAC 'cd ~/khnmc-work/rt-arm64check && mkdir -p obj && for f in runtime/*.c lifter-rt/khrt_cpu.c lifter-rt/rt_libm.c; do b=$(basename $f .c); if nice -n 10 clang -arch arm64 -std=gnu11 -O1 -w -fno-strict-aliasing -fwrapv -fno-math-errno -ffp-contract=off -DKH_XLATE_MODE=2 -Ilifter-rt -Iruntime -c $f -o obj/$b.o 2> obj/$b.err; then echo "PASS $f"; else echo "FAIL $f"; head -12 obj/$b.err; fi; done; file obj/*.o | head -3; sw_vers -productVersion; clang --version | head -1'
