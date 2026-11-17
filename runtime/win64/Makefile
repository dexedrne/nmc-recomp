# runtime-v0 build (run on build-host: make -j8).  Layout:  src/runtime/*.c (this tree)  lifter-rt/ (snapshot of tools/rt)  lifted/ (generated: shards lib, gen/*.c)
# usage: make            -> ./khnmc  (links libkh2lifted.a built by the lifter's ninja)
#        make arm64check -> compile-only check of the runtime sources for arm64 (clang, on the Mac mini)
#
# Variants (task 20).  Plain `make` on Linux is the build above with the same commands as before (prebuilt identity-mode archive,
# objcopy hooks, obj/, ./khnmc).  Two switches select the others; each variant has its own object directory and binary:
#   XLATE=2          bias mode: host = guest + g->bias (lifter-rt/khrt.h), guest space reserved wherever the host has room.  The
#                    lifted shards are compiled HERE with -DKH_XLATE_MODE=2 (117 files, the lifter's own flags) instead of using
#                    the prebuilt archive.  The address model of every host that cannot map 0x140000000 (macOS / iOS arm64).
#   HOOKMODE=rename  hooks without objcopy / --wrap: tools/hook_shards.py writes a copy of the shards with the hooked definitions
#                    renamed (orig_f_<rva>, __real_f_<rva>) from the SAME lists below (FILE_HOOKS, WRAP_FNS).  Mach-O needs it.
#   make XLATE=2                  -> obj-x2-objcopy/, ./khnmc-x2-objcopy   (Linux: isolates bias bugs from Darwin bugs)
#   make XLATE=2 HOOKMODE=rename  -> obj-x2-rename/,  ./khnmc-x2-rename    (Linux: the Darwin hook mechanism under the GNU linker)
#   make  (on Darwin)             -> XLATE=2 HOOKMODE=rename by default, obj/, ./khnmc, clang, no vkd3d backend (tools/darwin_build.sh)
# SHARDS_DIR = where the lifted shard_*.c are read from (default $(LIFTED)/shards; never written).
ROOT ?= .
LIFTED ?= $(ROOT)/lifted
CC ?= gcc
OPT ?= -O2 -g
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
XLATE ?= 2
HOOKMODE ?= rename
O ?= obj
BIN ?= khnmc
GFX_VKD3D = 0
LDEXPORT =
else
LDEXPORT = -rdynamic
endif
XLATE ?= 0
HOOKMODE ?= objcopy
ifeq ($(XLATE)-$(HOOKMODE),0-objcopy)
O ?= obj
BIN ?= khnmc
LIFTLIB ?= $(LIFTED)/lib/x86/libkh2lifted.a
else
O ?= obj-x$(XLATE)-$(HOOKMODE)
BIN ?= khnmc-x$(XLATE)-$(HOOKMODE)
LIFTLIB = $(O)/libkh2lifted.a
XFLAGS = -DKH_XLATE_MODE=$(XLATE)
endif
ifneq ($(XLATE),0)
# the vkd3d backend hands guest pointers to vkd3d as host pointers: identity mode on x86-64 only (KHNMC_GFX=vkd3d is a named fatal here)
GFX_VKD3D = 0
endif
ifeq ($(filter $(HOOKMODE),objcopy rename),)
$(error HOOKMODE must be objcopy or rename)
endif
CFLAGS = -std=gnu11 $(OPT) -w -fno-strict-aliasing -fwrapv -fno-delete-null-pointer-checks -fno-math-errno -ffp-contract=off -pthread \
         -I$(ROOT)/lifter-rt -I$(ROOT)/runtime -I$(LIFTED)/gen $(XFLAGS) $(EXTRA)
RTSRC = $(wildcard $(ROOT)/runtime/*.c)
RTOBJ = $(patsubst $(ROOT)/runtime/%.c,$(O)/%.o,$(RTSRC))
LIFTOBJ = $(O)/khrt_cpu.o $(O)/khrt_rcp_tab.o $(O)/rt_libm.o $(O)/kh_table.o $(O)/rt_imports_default.o $(O)/nep_stubs.o
# link-time entry probes (runtime/gxwrap.c): ld --wrap on Linux, renamed definitions + generated forwarders with HOOKMODE=rename
WRAP_FNS = 11d070 11d0a0 124060 10e860 11c150 127ed0 129110
comma := ,
WRAP = $(patsubst %,-Wl$(comma)--wrap=f_%,$(WRAP_FNS))
PHASE_HOOKS = 1505a0 5111d0 5122e0 511300 13da10 13d260 405440 3f29e0 3c12f0 2b4590 4f94b0
DIFFICULTY_HOOKS = 1523a0 152370 3ed7d0 3edf90 3c4b80 3c3da0
MOD_A_HOOKS = 2fc860 2fc4e0 4014a0 3a7cb0 29efa0 3a89a0 3d61d0 3d60c0   # 405440 is already in PHASE_HOOKS
HD_HOOKS = 106440 106460 106480
MOD_PARAMS_HOOKS = 40d810 40dd70
STREAM_HOOKS = 106da0 106fa0 106b40      # OpenStream / ReadStream / CloseStream on the loose tree (runtime/stream_layer.c)
# background-load list serialisation (runtime/bgload.c): loader loop + its blocking calls, the three enqueue entries, kn::MemoryAllocator::free, id->pointer (stress/selftest step)
BGLOAD_HOOKS = 3a0c60 15edd0 3f99e0 3f9850 3a64f0 3a0aa0 3a0b30 3a0bd0 19c470 4ad270
# scripted input clock (runtime/null_input.c): 0x105e10 is called once per game frame (end-of-frame task and the settings menu loop)
INPUT_HOOKS = 105e10
FILE_HOOKS = 1f9a80 3a9cf0 131ef0 106000 1063d0 106700 106790 106870 $(HD_HOOKS) $(PHASE_HOOKS) $(DIFFICULTY_HOOKS) $(MOD_A_HOOKS) $(MOD_PARAMS_HOOKS) $(STREAM_HOOKS) $(BGLOAD_HOOKS) $(INPUT_HOOKS)
# object-id table guard (runtime/idtable.c): 0x4ad390 registers a 32 MiB window in the game's 64-slot id table and encodes the pointer
IDTABLE_HOOKS = 4ad390
FILE_HOOKS += $(IDTABLE_HOOKS)
# KH2Moose pool sizes (runtime/moose.c): cache layout, cache free bytes, data heap release / reset (the four functions whose pool-start instruction the DLL re-encodes), reserved block sizes
MOOSE_HOOKS = 39d590 39dab0 3e5f30 3e5f90 39e260 39e270
FILE_HOOKS += $(MOOSE_HOOKS)
# ---- Task 12: mod static edits ---------------------------------------------------------------------------------------------------
# The mod DLL patches code bytes in 28 exe functions and attaches two INLINE detours (inside 3c3fe0 and 360920).  Those 30 functions
# are re-lifted from the patched image into $(GEN)/mod_static_lifted.c and replace the vanilla lifted bodies through the alias list
# (vanilla bodies stay reachable as orig_f_<rva>).  MOD_STATIC_CHAINED = functions a runtime hook already owns: the hook keeps
# f_<rva> and forwards to mod_f_<rva>.  $(GEN) holds mod/game-derived generated files: local only, never committed.
MOD_STATIC_FUNCS = 165110 2fb7e0 2fc500 2fd940 2fdd40 2fe010 360920 390150 3a89a0 3a9950 3b11e0 3b1780 3c1190 3c2f00 3c3fe0 3c5170 \
                   3c5440 3c5750 3c5900 3c5a90 3c5ce0 3d8b40 3de460 3e37f0 3e8380 3f29e0 3f3110 400b20 401d60 422780
MOD_STATIC_INLINE = 3c3fe0 360920
MOD_STATIC_CHAINED = 3a89a0 3f29e0
GEN ?= $(ROOT)/gen
MOD_EDITS ?= /home/user/projects/kh2-nmc-analysis/scratch/mod-gummi/edit_list.json
MOD_EXE ?= $(ROOT)/pc/kh2fm.exe
KH_UNIVERSE ?= /home/user/projects/kh2-nmc-analysis/scratch/universe
KHLIFT ?= /home/user/projects/khnmc-decomp-edition/tools/khlift
KHLIFT_PY ?= /home/user/projects/kh2-nmc-analysis/scratch/S2-lifter/venv/bin/python
FILE_HOOKS += $(MOD_STATIC_FUNCS)
CFLAGS += -I$(GEN)
LIFTOBJ += $(O)/mod_static_lifted.o
# LIFT_CFLAGS is defined once, further down, with $(XFLAGS): mod_static_lifted.o must be compiled in the same address mode as the shards
# ---- Task 19: NeverlandFly.dll (runtime/mod_neverland.c) --------------------------------------------------------------------------
# The mod's third DLL installs no detour: it writes 54 bytes of code/data into three exe functions (and into the 0xCC padding behind
# two of them) and starts a thread that rewrites one immediate byte.  The three functions are re-lifted from the patched image by the
# Task 12 mechanism (same alias list, same generated C file, own table), with the DLL's edits applied on top of the main mod DLL's:
#   424570  YS::ACTION_LM::vfn7  motion ids of the swim/flight action (entry jump into a code cave behind the function)
#   306400  menu 3D model setup  two calls replaced by one (model y offset -25.0)
#   15b6d0  dk::COMMAND_DRAW     `add ebp,imm8` whose immediate the DLL thread rewrites (site handler kh_mod_site_15b960)
# $(GEN)/mod_neverland_edits.json and mod_neverland_table.h are decoded statically from the DLL by tools/gen_mod_neverland.py.
MOD_NEVERLAND_HOOKS = 424570 306400 15b6d0
MOD_NEVERLAND_DLL ?= /home/user/projects/SillyBillyMod1/dll/NeverlandFly.dll
MOD_NEVERLAND_EDITS = $(GEN)/mod_neverland_edits.json
MOD_LIFT_FUNCS = $(MOD_STATIC_FUNCS) $(MOD_NEVERLAND_HOOKS)
# runtime/mod_neverland.c owns f_<rva> of all three (call counters) and forwards to the re-lifted mod_f_<rva>
MOD_LIFT_CHAINED = $(MOD_STATIC_CHAINED) $(MOD_NEVERLAND_HOOKS)
FILE_HOOKS += $(MOD_NEVERLAND_HOOKS)
# FILE_HOOKS can name an RVA twice (a runtime hook that forwards to a re-lifted body is in two sub-lists): the hook tools get each RVA once
HOOKS_UNIQ = $(sort $(FILE_HOOKS))
all: $(BIN)
# Optional vkd3d graphics backend (KHNMC_GFX=vkd3d, runtime/gfx_vkd3d.c): compiled in when the vkd3d libraries exist under VKD3D_PREFIX
# (make GFX_VKD3D=0 compiles it out: KHNMC_GFX=vkd3d is then a named fatal).  Only gfx_vkd3d.o depends on the switch; the stamp relinks.
VKD3D_PREFIX ?= /home/user/work/khnmc-deps/vkd3d/usr
VKHEADERS_PREFIX ?= /home/user/work/khnmc-deps/vkheaders/usr
ifneq ($(wildcard $(VKD3D_PREFIX)/lib/libvkd3d.so),)
GFX_VKD3D ?= 1
else
GFX_VKD3D ?= 0
endif
ifeq ($(GFX_VKD3D),1)
$(O)/gfx_vkd3d.o: CFLAGS += -DKH_GFX_VKD3D=1 -DWIDL_C_INLINE_WRAPPERS -I$(VKD3D_PREFIX)/include/vkd3d -I$(VKHEADERS_PREFIX)/include
GFX_LDFLAGS = -L$(VKD3D_PREFIX)/lib -lvkd3d-utils -lvkd3d -ldl -Wl,--disable-new-dtags -Wl,-rpath,$(VKD3D_PREFIX)/lib
endif
$(O)/.gfx-vkd3d-$(GFX_VKD3D):
	@mkdir -p $(O) && rm -f $(O)/.gfx-vkd3d-* && touch $@
$(O)/gfx_vkd3d.o $(BIN): $(O)/.gfx-vkd3d-$(GFX_VKD3D)
# ---- lifted shards compiled here (every variant except the default one, which links the prebuilt archive) ----
SHARDS_DIR ?= $(LIFTED)/shards
SHARD_NAMES := $(patsubst %.c,%,$(notdir $(wildcard $(SHARDS_DIR)/shard_*.c)))
SHARD_OBJ = $(patsubst %,$(O)/lifted/%.o,$(SHARD_NAMES))
# the lifter's flags (lifted/tools/buildgen.py, docs/LIFT-POLICY.md section 8) + the address mode
LIFT_CFLAGS = -O1 -w -fno-strict-aliasing -fwrapv -fno-delete-null-pointer-checks -fno-math-errno -ffp-contract=off -I$(ROOT)/lifter-rt -I$(LIFTED)/gen $(XFLAGS) $(LIFT_EXTRA)
ifeq ($(HOOKMODE),objcopy)
$(O)/lifted/%.o: $(SHARDS_DIR)/%.c
	@mkdir -p $(O)/lifted && $(CC) $(LIFT_CFLAGS) -c $< -o $@
$(O)/libkh2lifted.a: $(SHARD_OBJ)
	@test -n "$(SHARD_OBJ)" || { echo "no shard_*.c in $(SHARDS_DIR)" >&2; exit 1; }
	rm -f $@ && ar rcs $@ $(SHARD_OBJ)
# Weak definitions also allow same-shard relocations to bind to the runtime hooks.
$(O)/libkh2loose.a: $(LIFTLIB) Makefile tools/alias_file_hooks.py
	@mkdir -p $(O)
	python3 tools/alias_file_hooks.py $< $@ $(FILE_HOOKS)
HOOK_LINK = $(WRAP)
else
# HOOKMODE=rename: the generated copy of the shards.  The stamp reruns the generator when a shard, the hook lists (Makefile) or the
# tool changes; the generator rewrites only files whose content changed, so unchanged shards are not recompiled.
# the expanded hook lists as a file that changes only when the lists do (also when they are overridden on the command line)
$(O)/hooks.list: FORCE
	@mkdir -p $(O); echo "hooks: $(strip $(HOOKS_UNIQ)) wrap: $(strip $(WRAP_FNS))" | cmp -s - $@ || echo "hooks: $(strip $(HOOKS_UNIQ)) wrap: $(strip $(WRAP_FNS))" > $@
FORCE:
$(O)/shards/.stamp: $(wildcard $(SHARDS_DIR)/shard_*.c) Makefile tools/hook_shards.py $(O)/hooks.list
	@mkdir -p $(O)/shards
	python3 tools/hook_shards.py shards $(SHARDS_DIR) $(O)/shards --hooks $(HOOKS_UNIQ) --wrap $(WRAP_FNS)
	@touch $@
SHARD_GEN = $(patsubst %,$(O)/shards/%.c,$(SHARD_NAMES))
$(SHARD_GEN): $(O)/shards/.stamp ;
$(O)/lifted/%.o: $(O)/shards/%.c
	@mkdir -p $(O)/lifted && $(CC) $(LIFT_CFLAGS) -c $< -o $@
$(O)/libkh2loose.a: $(SHARD_OBJ)
	@test -n "$(SHARD_OBJ)" || { echo "no shard_*.c in $(SHARDS_DIR)" >&2; exit 1; }
	rm -f $@ && ar rcs $@ $(SHARD_OBJ)
# forwarders: f_X -> __wrap_f_X for the wrapped functions, and f_X -> orig_f_X (with a NOTE) for a hooked function that NO linked
# object defines.  The scan covers every directly linked object ($(RTOBJ) and $(LIFTOBJ): a replacement may also be a re-lifted body
# in another lifted object), so it is driven by the final FILE_HOOKS list and by what is really in the link.
$(O)/hook_forwarders.c: $(RTOBJ) $(LIFTOBJ) Makefile tools/hook_shards.py $(O)/hooks.list
	nm $(RTOBJ) $(LIFTOBJ) > $(O)/link-objects.nm
	python3 tools/hook_shards.py forwarders $@ --nm $(O)/link-objects.nm --hooks $(HOOKS_UNIQ) --wrap $(WRAP_FNS)
$(O)/hook_forwarders.o: $(O)/hook_forwarders.c
	$(CC) $(CFLAGS) -c $< -o $@
HOOK_OBJ = $(O)/hook_forwarders.o
HOOK_LINK =
endif
# link check for every hook, both hook modes, ELF and Mach-O: one strong f_<rva> and a distinct orig_f_<rva> per hooked RVA
# (HOOK_STRICT=1: a hooked RVA that nothing replaces is an error instead of a NOTE)
all: $(O)/hook_link.ok
$(O)/hook_link.ok: $(BIN) tools/check_hook_link.py
	python3 tools/check_hook_link.py --bin $(BIN) --objs $(RTOBJ) $(LIFTOBJ) $(HOOK_OBJ) --hooks $(HOOKS_UNIQ) --wrap $(WRAP_FNS) $(if $(HOOK_OBJ),--forwarders $(HOOK_OBJ)) $(if $(filter 1,$(HOOK_STRICT)),--strict) && touch $@
$(O)/%.o: $(ROOT)/runtime/%.c $(wildcard $(ROOT)/runtime/*.h)
	@mkdir -p $(O) && $(CC) $(CFLAGS) -c $< -o $@
$(O)/khrt_cpu.o: $(ROOT)/lifter-rt/khrt_cpu.c
	@mkdir -p $(O) && $(CC) $(CFLAGS) -c $< -o $@
$(O)/khrt_rcp_tab.o: $(ROOT)/lifter-rt/khrt_rcp_tab.c
	@mkdir -p $(O) && $(CC) $(CFLAGS) -c $< -o $@
$(O)/rt_libm.o: $(ROOT)/lifter-rt/rt_libm.c
	@mkdir -p $(O) && $(CC) $(CFLAGS) -c $< -o $@
$(O)/kh_table.o: $(LIFTED)/gen/kh_table.c
	@mkdir -p $(O) && $(CC) $(CFLAGS) -c $< -o $@
$(O)/rt_imports_default.o: $(LIFTED)/gen/rt_imports_default.c
	@mkdir -p $(O) && $(CC) $(CFLAGS) -c $< -o $@
$(O)/nep_stubs.c: $(LIFTED)/gen/nep_stubs.c $(ROOT)/tools/gen_nep.sh
	@mkdir -p $(O) && sh $(ROOT)/tools/gen_nep.sh $< $@
$(O)/nep_stubs.o: $(O)/nep_stubs.c
	$(CC) $(CFLAGS) -c $< -o $@
$(BIN): $(RTOBJ) $(LIFTOBJ) $(HOOK_OBJ) $(O)/libkh2loose.a
	$(CC) -o $@ $(RTOBJ) $(LIFTOBJ) $(HOOK_OBJ) $(O)/libkh2loose.a -lm -lpthread $(LDEXPORT) $(HOOK_LINK) $(GFX_LDFLAGS)
# arm64 compile check of the hand-written runtime only (no lifted shards): clang -arch arm64 on macOS
arm64check:
	@for f in $(RTSRC) $(ROOT)/lifter-rt/khrt_cpu.c $(ROOT)/lifter-rt/rt_libm.c; do echo CC $$f; clang -arch arm64 -std=gnu11 -O1 -w -fno-strict-aliasing -fwrapv -fno-math-errno -ffp-contract=off -I$(ROOT)/lifter-rt -I$(ROOT)/runtime -I$(LIFTED)/gen -DKH_XLATE_MODE=2 -c $$f -o /tmp/khnmc_arm_chk.o || exit 1; done
# ---- Task 12 rules: generated table (python3 stdlib), re-lift (needs the lifter environment: KHLIFT, KHLIFT_PY, KH_UNIVERSE), link check
$(GEN)/mod_static_table.h: tools/gen_mod_static.py $(MOD_EDITS) Makefile
	python3 tools/gen_mod_static.py --edits $(MOD_EDITS) --exe $(MOD_EXE) --universe $(KH_UNIVERSE) --funcs "$(MOD_STATIC_FUNCS)" \
	    --inline "$(MOD_STATIC_INLINE)" --chained "$(MOD_STATIC_CHAINED)" --out $@ --report $(GEN)/mod_static_classify.json
GEN_MOD_NEVERLAND = python3 tools/gen_mod_neverland.py --dll $(MOD_NEVERLAND_DLL) --exe $(MOD_EXE) --universe $(KH_UNIVERSE) --edits $(MOD_EDITS) \
	    --out-json $(MOD_NEVERLAND_EDITS) --out-h $(GEN)/mod_neverland_table.h
$(MOD_NEVERLAND_EDITS): tools/gen_mod_neverland.py tools/gen_mod_static.py $(MOD_NEVERLAND_DLL) $(MOD_EDITS) Makefile
	$(GEN_MOD_NEVERLAND)
# one run writes both files; the header rule only regenerates when the header alone went missing
$(GEN)/mod_neverland_table.h: $(MOD_NEVERLAND_EDITS)
	@test -f $@ || $(GEN_MOD_NEVERLAND)
$(O)/mod_neverland.o $(O)/mod_neverland_test.o: $(GEN)/mod_neverland_table.h
$(GEN)/mod_static_lifted.c: tools/mod_static_lift.py $(MOD_EDITS) $(MOD_NEVERLAND_EDITS) $(LIFTED)/shards.json Makefile
	KH_UNIVERSE=$(KH_UNIVERSE) $(KHLIFT_PY) tools/mod_static_lift.py --khlift $(KHLIFT) --edits $(MOD_EDITS) --exe $(MOD_EXE) --extra-edits $(MOD_NEVERLAND_EDITS) \
	    --funcs "$(MOD_LIFT_FUNCS)" --chained "$(MOD_LIFT_CHAINED)" --vanilla-shards $(LIFTED)/shards --out $@ --report $(GEN)/mod_static_lift.json
$(O)/mod_static.o: $(GEN)/mod_static_table.h
$(O)/mod_static_lifted.o: $(GEN)/mod_static_lifted.c
	@mkdir -p $(O) && $(CC) $(LIFT_CFLAGS) -c $< -o $@
all: $(O)/mod_static_link.ok
$(O)/mod_static_link.ok: $(BIN) tools/check_mod_static_link.py
	python3 tools/check_mod_static_link.py --bin $(BIN) --obj $(O) --funcs "$(MOD_LIFT_FUNCS)" --chained "$(MOD_LIFT_CHAINED)" && touch $@
# native-oracle differential of the re-lifted functions against the patched bytes (x86-64 Linux dev host; not part of `all`).
# CPU-heavy: runs niced, MOD_STATIC_DIFFTEST_JOBS processes at a time (keep it low on a shared desktop).
MOD_STATIC_DIFFTEST_JOBS ?= 4
mod-static-difftest:
	KH_UNIVERSE=$(KH_UNIVERSE) nice -n 10 $(KHLIFT_PY) tools/mod_static_difftest.py --jobs $(MOD_STATIC_DIFFTEST_JOBS) --khlift $(KHLIFT) --edits $(MOD_EDITS) --exe $(MOD_EXE) \
	    --extra-edits $(MOD_NEVERLAND_EDITS) --funcs "$(MOD_LIFT_FUNCS)" --chained "$(MOD_LIFT_CHAINED)" --vanilla-shards $(LIFTED)/shards --lifted $(GEN)/mod_static_lifted.c \
	    --sites $(ROOT)/runtime/mod_static_sites.c --work $(O)/mod_static_difftest --out $(GEN)/mod_static_difftest.json $(MOD_STATIC_DIFFTEST_ARGS)
# the three NeverlandFly functions only (Task 19): same tool, own result file
mod-neverland-difftest: $(MOD_NEVERLAND_EDITS)
	KH_UNIVERSE=$(KH_UNIVERSE) nice -n 10 $(KHLIFT_PY) tools/mod_static_difftest.py --jobs $(MOD_STATIC_DIFFTEST_JOBS) --khlift $(KHLIFT) --edits $(MOD_EDITS) --exe $(MOD_EXE) \
	    --extra-edits $(MOD_NEVERLAND_EDITS) --funcs "$(MOD_LIFT_FUNCS)" --chained "$(MOD_LIFT_CHAINED)" --vanilla-shards $(LIFTED)/shards --lifted $(GEN)/mod_static_lifted.c \
	    --sites $(ROOT)/runtime/mod_static_sites.c --work $(O)/mod_static_difftest --only "$(shell echo $(MOD_NEVERLAND_HOOKS) | tr ' ' ',')" \
	    --out $(GEN)/mod_neverland_difftest.json $(MOD_STATIC_DIFFTEST_ARGS)
# compile every hand-written and lifter-rt object of the variant without the shards and without linking (`make -k rtobjs`: all errors of a port)
rtobjs: $(RTOBJ) $(LIFTOBJ)
clean:
	rm -rf $(O) $(BIN)
.PHONY: all clean arm64check rtobjs FORCE mod-static-difftest mod-neverland-difftest
