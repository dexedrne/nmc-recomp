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
LIFTOBJ += obj/mod_static_lifted.o
LIFT_CFLAGS = -std=gnu11 -O1 -w -fno-strict-aliasing -fwrapv -fno-delete-null-pointer-checks -fno-math-errno -ffp-contract=off -I$(ROOT)/lifter-rt -I$(LIFTED)/gen
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
all: khnmc
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
obj/gfx_vkd3d.o: CFLAGS += -DKH_GFX_VKD3D=1 -DWIDL_C_INLINE_WRAPPERS -I$(VKD3D_PREFIX)/include/vkd3d -I$(VKHEADERS_PREFIX)/include
GFX_LDFLAGS = -L$(VKD3D_PREFIX)/lib -lvkd3d-utils -lvkd3d -ldl -Wl,--disable-new-dtags -Wl,-rpath,$(VKD3D_PREFIX)/lib
endif
obj/.gfx-vkd3d-$(GFX_VKD3D):
	@mkdir -p obj && rm -f obj/.gfx-vkd3d-* && touch $@
obj/gfx_vkd3d.o khnmc: obj/.gfx-vkd3d-$(GFX_VKD3D)
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
	$(CC) -o $@ $(RTOBJ) $(LIFTOBJ) obj/libkh2loose.a -lm -lpthread -rdynamic $(WRAP) $(GFX_LDFLAGS)
# arm64 compile check of the hand-written runtime only (no lifted shards): clang -arch arm64 on macOS
arm64check:
	@for f in $(RTSRC) $(ROOT)/lifter-rt/khrt_cpu.c $(ROOT)/lifter-rt/rt_libm.c; do echo CC $$f; clang -arch arm64 -std=gnu11 -O1 -w -fno-strict-aliasing -fwrapv -fno-math-errno -ffp-contract=off -I$(ROOT)/lifter-rt -I$(ROOT)/runtime -I$(LIFTED)/gen -I$(GEN) -DKH_XLATE_MODE=2 -c $$f -o /tmp/khnmc_arm_chk.o || exit 1; done
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
obj/mod_neverland.o obj/mod_neverland_test.o: $(GEN)/mod_neverland_table.h
$(GEN)/mod_static_lifted.c: tools/mod_static_lift.py $(MOD_EDITS) $(MOD_NEVERLAND_EDITS) $(LIFTED)/shards.json Makefile
	KH_UNIVERSE=$(KH_UNIVERSE) $(KHLIFT_PY) tools/mod_static_lift.py --khlift $(KHLIFT) --edits $(MOD_EDITS) --exe $(MOD_EXE) --extra-edits $(MOD_NEVERLAND_EDITS) \
	    --funcs "$(MOD_LIFT_FUNCS)" --chained "$(MOD_LIFT_CHAINED)" --vanilla-shards $(LIFTED)/shards --out $@ --report $(GEN)/mod_static_lift.json
obj/mod_static.o: $(GEN)/mod_static_table.h
obj/mod_static_lifted.o: $(GEN)/mod_static_lifted.c
	@mkdir -p obj && $(CC) $(LIFT_CFLAGS) -c $< -o $@
all: obj/mod_static_link.ok
obj/mod_static_link.ok: khnmc tools/check_mod_static_link.py
	python3 tools/check_mod_static_link.py --bin khnmc --obj obj --funcs "$(MOD_LIFT_FUNCS)" --chained "$(MOD_LIFT_CHAINED)" && touch $@
# native-oracle differential of the re-lifted functions against the patched bytes (x86-64 Linux dev host; not part of `all`).
# CPU-heavy: runs niced, MOD_STATIC_DIFFTEST_JOBS processes at a time (keep it low on a shared desktop).
MOD_STATIC_DIFFTEST_JOBS ?= 4
mod-static-difftest:
	KH_UNIVERSE=$(KH_UNIVERSE) nice -n 10 $(KHLIFT_PY) tools/mod_static_difftest.py --jobs $(MOD_STATIC_DIFFTEST_JOBS) --khlift $(KHLIFT) --edits $(MOD_EDITS) --exe $(MOD_EXE) \
	    --extra-edits $(MOD_NEVERLAND_EDITS) --funcs "$(MOD_LIFT_FUNCS)" --chained "$(MOD_LIFT_CHAINED)" --vanilla-shards $(LIFTED)/shards --lifted $(GEN)/mod_static_lifted.c \
	    --sites $(ROOT)/runtime/mod_static_sites.c --work obj/mod_static_difftest --out $(GEN)/mod_static_difftest.json $(MOD_STATIC_DIFFTEST_ARGS)
# the three NeverlandFly functions only (Task 19): same tool, own result file
mod-neverland-difftest: $(MOD_NEVERLAND_EDITS)
	KH_UNIVERSE=$(KH_UNIVERSE) nice -n 10 $(KHLIFT_PY) tools/mod_static_difftest.py --jobs $(MOD_STATIC_DIFFTEST_JOBS) --khlift $(KHLIFT) --edits $(MOD_EDITS) --exe $(MOD_EXE) \
	    --extra-edits $(MOD_NEVERLAND_EDITS) --funcs "$(MOD_LIFT_FUNCS)" --chained "$(MOD_LIFT_CHAINED)" --vanilla-shards $(LIFTED)/shards --lifted $(GEN)/mod_static_lifted.c \
	    --sites $(ROOT)/runtime/mod_static_sites.c --work obj/mod_static_difftest --only "$(shell echo $(MOD_NEVERLAND_HOOKS) | tr ' ' ',')" \
	    --out $(GEN)/mod_neverland_difftest.json $(MOD_STATIC_DIFFTEST_ARGS)
clean:
	rm -rf obj khnmc
.PHONY: all clean arm64check mod-static-difftest mod-neverland-difftest
