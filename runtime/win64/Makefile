# runtime-v0 build (run on build-host: make -j8).  Layout:  src/runtime/*.c (this tree)  lifter-rt/ (snapshot of tools/rt)  lifted/ (generated: shards lib, gen/*.c)
# usage: make            -> ./khnmc  (links libkh2lifted.a built by the lifter's ninja)
#        make arm64check -> compile-only check of the runtime sources for arm64 (clang, on the Mac mini)
ROOT ?= .
LIFTED ?= $(ROOT)/lifted
CC ?= gcc
OPT ?= -O2 -g
CFLAGS = -std=gnu11 $(OPT) -w -fno-strict-aliasing -fwrapv -fno-delete-null-pointer-checks -fno-math-errno -ffp-contract=off -pthread \
         -I$(ROOT)/lifter-rt -I$(ROOT)/runtime -I$(LIFTED)/gen $(EXTRA)
RTSRC = $(wildcard $(ROOT)/runtime/*.c)
RTOBJ = $(patsubst $(ROOT)/runtime/%.c,obj/%.o,$(RTSRC))
LIFTOBJ = obj/khrt_cpu.o obj/khrt_rcp_tab.o obj/rt_libm.o obj/kh_table.o obj/rt_imports_default.o obj/nep_stubs.o
WRAP = -Wl,--wrap=f_11d070 -Wl,--wrap=f_11d0a0 -Wl,--wrap=f_124060 -Wl,--wrap=f_10e860 -Wl,--wrap=f_11c150 -Wl,--wrap=f_127ed0 -Wl,--wrap=f_129110
FILE_HOOKS = 106000 1063d0 106700 106790 106870
all: khnmc
# Weak definitions also allow same-shard relocations to bind to the runtime hooks.
obj/libkh2loose.a: $(LIFTED)/lib/x86/libkh2lifted.a Makefile tools/alias_file_hooks.py
	@mkdir -p obj
	python3 tools/alias_file_hooks.py $< $@ $(FILE_HOOKS)
obj/%.o: $(ROOT)/runtime/%.c $(wildcard $(ROOT)/runtime/*.h)
	@mkdir -p obj && $(CC) $(CFLAGS) -c $< -o $@
obj/khrt_cpu.o: $(ROOT)/lifter-rt/khrt_cpu.c
	@mkdir -p obj && $(CC) $(CFLAGS) -c $< -o $@
obj/khrt_rcp_tab.o: $(ROOT)/lifter-rt/khrt_rcp_tab.c
	@mkdir -p obj && $(CC) $(CFLAGS) -c $< -o $@
obj/rt_libm.o: $(ROOT)/lifter-rt/rt_libm.c
	@mkdir -p obj && $(CC) $(CFLAGS) -c $< -o $@
obj/kh_table.o: $(LIFTED)/gen/kh_table.c
	@mkdir -p obj && $(CC) $(CFLAGS) -c $< -o $@
obj/rt_imports_default.o: $(LIFTED)/gen/rt_imports_default.c
	@mkdir -p obj && $(CC) $(CFLAGS) -c $< -o $@
obj/nep_stubs.c: $(LIFTED)/gen/nep_stubs.c $(ROOT)/tools/gen_nep.sh
	@mkdir -p obj && sh $(ROOT)/tools/gen_nep.sh $< $@
obj/nep_stubs.o: obj/nep_stubs.c
	$(CC) $(CFLAGS) -c $< -o $@
khnmc: $(RTOBJ) $(LIFTOBJ) obj/libkh2loose.a
	$(CC) -o $@ $(RTOBJ) $(LIFTOBJ) obj/libkh2loose.a -lm -lpthread -rdynamic $(WRAP)
# arm64 compile check of the hand-written runtime only (no lifted shards): clang -arch arm64 on macOS
arm64check:
	@for f in $(RTSRC) $(ROOT)/lifter-rt/khrt_cpu.c $(ROOT)/lifter-rt/rt_libm.c; do echo CC $$f; clang -arch arm64 -std=gnu11 -O1 -w -fno-strict-aliasing -fwrapv -fno-math-errno -ffp-contract=off -I$(ROOT)/lifter-rt -I$(ROOT)/runtime -I$(LIFTED)/gen -DKH_XLATE_MODE=2 -c $$f -o /tmp/khnmc_arm_chk.o || exit 1; done
clean:
	rm -rf obj khnmc
.PHONY: all clean arm64check
