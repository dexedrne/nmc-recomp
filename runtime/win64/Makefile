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
#   make GFX_VKD3D=1 (on Darwin)  -> the same build WITH the wrapped graphics bridge (KHNMC_GFX=vkd3dw KHNMC_GFX_DRIVER=system):
#                                    vkd3d's four STATIC libraries and MoltenVK, see "Task 21b" below.  Off unless asked for.
# SHARDS_DIR = where the lifted shard_*.c are read from (default $(LIFTED)/shards; never written).
#
# Task 26:
#   IOS_PATHS=1      the iOS code paths on this host (-DKH_IOS_PATHS=1, runtime/kh_compat.h, runtime/host_env.c): no fork, no fixed
#                    /tmp, app-container directories, Documents/khnmc.conf.  Only the hand-written runtime depends on it, so the
#                    variant keeps its own runtime objects and binary ($(O) and $(BIN) get the suffix -iospaths) and REUSES the
#                    lifted shard objects and archive of the same build without the switch ($(OL)).  `all` then also runs
#                    tools/check_ios_symbols.py (the program must not import fork, system, popen, tmpfile, ...).
#   make IOS_PATHS=1              -> obj-iospaths/, ./khnmc-iospaths   (Linux; on Darwin the same names, bias mode + rename hooks)
#   make [IOS_PATHS=1] hostshell  -> $(BIN)-shell: the link an iOS app makes (the game as two static libraries + a shell that owns
#                    main() and runs the game on a second thread: tools/ios/host_shell.c), with the same link checks.  `applib`
#                    makes only the libraries: $(O)/libkhnmc-rt.a (every directly linked object; loader.c with
#                    -DKH_MAIN_NAME=kh_main) and $(OL)/libkh2loose.a.
#   IOS_SDK=iphonesimulator|iphoneos   PREPARED, NOT RUN (no iOS SDK on the machines this was written on): `make IOS_SDK=... applib`
#                    compiles everything for that SDK with xcrun clang (arm64, bias mode, rename hooks, obj-<sdk>/); the app
#                    itself is linked by the Xcode project tools/ios/make_project.py writes.  See tools/ios/.
#   TASK38: `make IOS_SDK=iphoneos GFX_VKD3D=1 applib` puts the graphics backend (wrapped bridge, vkd3d 2.1 for iOS, MoltenVK linked
#                    statically) into libkhnmc-rt.a; the app then links with KHNMC_GFX_LDFLAGS = `make -s IOS_SDK=iphoneos GFX_VKD3D=1
#                    print-GFX_APP_LDFLAGS` (tools/ios/mac/build.sh and sign_app.sh, GFX=1).  IOS_SDK=iphonesimulator GFX_VKD3D=1 is
#                    an error (no simulator slice of MoltenVK).  `make IOS_PATHS=1 GFX_VKD3D=1 hostshell` is the same link on Linux.
#   GEN_PREBUILT=1   $(GEN)/mod_static_lifted.c and the three tables are INPUTS (copied from the Linux dev host, where the lifter
#                    environment lives) and are never regenerated; a missing one is an error that says so.  Default on Darwin.
ROOT ?= .
LIFTED ?= $(ROOT)/lifted
CC ?= gcc
OPT ?= -O2 -g
UNAME_S := $(shell uname -s)
IOS_PATHS ?= 0
ifeq ($(filter $(IOS_PATHS),0 1),)
$(error IOS_PATHS must be 0 or 1)
endif
ifeq ($(IOS_PATHS),1)
IOSSFX = -iospaths
IOSFLAGS = -DKH_IOS_PATHS=1
# TASK38 (IOS-GRAPHICS-PLAN step 3): such a build has the vkd3d backend only when the command line asks for it (`make IOS_PATHS=1
# GFX_VKD3D=1 ...`: the wrapped bridge KHNMC_GFX=vkd3dw, the one a bias build has; the vkd3d selftest's files go to kh_tmp_path, not a
# fixed /tmp).  Otherwise it has none, as before (the old reason, "an iOS build is a bias build, which never has it", predates the
# wrapped bridge).  GFX_VKD3D from the environment is ignored here, as it always was.
ifneq ($(origin GFX_VKD3D),command line)
GFX_VKD3D = 0
endif
endif
# Task 39: ORDERED_MEM=1 (-DKH_ORDERED_MEM=1, lifter-rt/khrt.h and runtime/khnmc_rt.h): every naturally aligned 1/2/4/8-byte guest load
# is an acquire load and every such store a release store (arm64: LDAPR / STLR; x86-64: the same MOVs plus compiler ordering), the
# x86 TSO order that guest code may rely on.  Default 0 on every host until it is measured on the Mac mini and the phone and the owner
# decides.  Every object depends on it, the lifted shards too (no re-lift, a recompile): the variant has its own object directories
# and binary (suffix -om: obj-om/ + ./khnmc-om, obj-x2-objcopy-om/ + ./khnmc-x2-objcopy-om, obj-iphoneos-om/, on Darwin obj-om/ +
# ./khnmc-om), and the identity build compiles the shards here instead of linking the prebuilt (plain) archive.  ORDERED_MEM=0 or unset
# changes no command line and no object.  OM_RCPC=1 (default with ORDERED_MEM=1 on a Mac) adds the rcpc target feature: LDAPR instead
# of LDAR (macOS arm64 targets have it already; the iOS 17 floor is the A12, which has it; OM_RCPC=0 measures LDAR).
# Owner decision 2026-11-18: ORDERED_MEM=1 is the DEFAULT for the Apple targets (macOS and the iPhone SDK builds): both CPUs reorder guest
# accesses that x86 keeps in order (docs/APPLE-PLAN.md "ORDERED_MEM on Apple silicon" and "... on the phone": litmus 1.7M / 720k of 4M
# reads out of order with plain accesses, 0 with acquire/release; cost about 4 % per access, no visible cost in the game).  Elsewhere the
# default stays 0.  `ORDERED_MEM=0` on the command line still builds the plain variant; the directory and binary names follow the
# variant, not the default (ordered = the -om names everywhere), so objects of the two variants never meet.
ifneq ($(IOS_SDK),)
ORDERED_MEM ?= 1
endif
ifeq ($(UNAME_S),Darwin)
ORDERED_MEM ?= 1
endif
ORDERED_MEM ?= 0
ifeq ($(filter $(ORDERED_MEM),0 1),)
$(error ORDERED_MEM must be 0 or 1)
endif
ifeq ($(ORDERED_MEM),1)
OMSFX = -om
OMFLAGS = -DKH_ORDERED_MEM=1
ifeq ($(UNAME_S),Darwin)
OM_RCPC ?= 1
ifeq ($(OM_RCPC),1)
OMFLAGS += -Xclang -target-feature -Xclang +rcpc
endif
endif
endif
# PREPARED, NOT RUN: an iOS SDK as the target (needs Xcode; arm64 only)
IOS_MIN ?= 17.0
ifneq ($(IOS_SDK),)
ifeq ($(IOS_SDK),iphoneos)
IOS_TRIPLE = arm64-apple-ios$(IOS_MIN)
else ifeq ($(IOS_SDK),iphonesimulator)
IOS_TRIPLE = arm64-apple-ios$(IOS_MIN)-simulator
else
$(error IOS_SDK must be iphoneos or iphonesimulator)
endif
ifneq ($(UNAME_S),Darwin)
$(error IOS_SDK needs a Mac with Xcode)
endif
CC = xcrun --sdk $(IOS_SDK) clang -target $(IOS_TRIPLE)
OL = obj-$(IOS_SDK)$(OMSFX)
BIN = $(OL)/khnmc-not-an-app
endif
ifeq ($(UNAME_S),Darwin)
XLATE ?= 2
HOOKMODE ?= rename
OL ?= obj$(OMSFX)
BIN ?= khnmc$(OMSFX)$(IOSSFX)
# the vkd3d backend is OFF on Darwin unless `make GFX_VKD3D=1` asks for it (Task 21b, block below); IOS_PATHS=1 keeps it off unless the
# command line asks (TASK38)
GFX_VKD3D ?= 0
LDEXPORT =
GEN_PREBUILT ?= 1
else
LDEXPORT = -rdynamic
endif
GEN_PREBUILT ?= 0
XLATE ?= 0
HOOKMODE ?= objcopy
# $(OL): objects of the lifted shards and their archive (they do not depend on IOS_PATHS); $(O): everything else of the variant
ifeq ($(XLATE)-$(HOOKMODE)-$(ORDERED_MEM),0-objcopy-0)
OL ?= obj
BIN ?= khnmc$(IOSSFX)
LIFTLIB ?= $(LIFTED)/lib/x86/libkh2lifted.a
else ifeq ($(XLATE)-$(HOOKMODE),0-objcopy)
# task 39: the identity build with ORDERED_MEM=1: the prebuilt archive is plain, so the shards are compiled here (identity addresses)
OL ?= obj-om
BIN ?= khnmc-om$(IOSSFX)
LIFTLIB = $(OL)/libkh2lifted.a
else
OL ?= obj-x$(XLATE)-$(HOOKMODE)$(OMSFX)
BIN ?= khnmc-x$(XLATE)-$(HOOKMODE)$(OMSFX)$(IOSSFX)
LIFTLIB = $(OL)/libkh2lifted.a
XFLAGS = -DKH_XLATE_MODE=$(XLATE)
endif
O ?= $(OL)$(IOSSFX)
# vkd3d graphics in a bias build (XLATE != 0): only the WRAPPED bridge (KHNMC_GFX=vkd3dw, runtime/gfx_wrap.c) exists there.  The trampoline
# bridge (KHNMC_GFX=vkd3d) hands guest pointers to vkd3d as host pointers and is compiled only for x86-64 identity builds: in every other
# build it is the named fatal gfx-vkd3d-needs-identity.  Darwin: only with `make GFX_VKD3D=1` (static vkd3d over MoltenVK, Task 21b).
ifeq ($(filter $(HOOKMODE),objcopy rename),)
$(error HOOKMODE must be objcopy or rename)
endif
CFLAGS = -std=gnu11 $(OPT) -w -fno-strict-aliasing -fwrapv -fno-delete-null-pointer-checks -fno-math-errno -ffp-contract=off -pthread \
         -I$(ROOT)/lifter-rt -I$(ROOT)/runtime -I$(LIFTED)/gen $(XFLAGS) $(IOSFLAGS) $(OMFLAGS) $(EXTRA)
RTSRC = $(wildcard $(ROOT)/runtime/*.c)
# TASK28, PREPARED, NOT RUN: COREAUDIO=1 adds the Apple output-unit glue (runtime/ios/audio_coreaudio.c, an audio unit whose render
# callback calls kh_audio_out_pull) and AudioToolbox.  Darwin / iOS SDK builds only; never built on the machines this was written on.
ifeq ($(COREAUDIO),1)
ifneq ($(UNAME_S),Darwin)
$(error COREAUDIO=1 needs a Mac (AudioToolbox))
endif
RTSRC += $(ROOT)/runtime/ios/audio_coreaudio.c
CFLAGS += -DKH_AUDIO_COREAUDIO=1
COREAUDIO_LDFLAGS = -framework AudioToolbox -framework CoreFoundation
endif
RTOBJ = $(patsubst $(ROOT)/runtime/%.c,$(O)/%.o,$(RTSRC))
LIFTOBJ = $(O)/khrt_cpu.o $(O)/khrt_rcp_tab.o $(O)/rt_libm.o $(O)/kh_table.o $(O)/rt_imports_default.o $(O)/nep_stubs.o
# link-time entry probes (runtime/gxwrap.c): ld --wrap on Linux, renamed definitions + generated forwarders with HOOKMODE=rename
WRAP_FNS = 11d070 11d0a0 124060 10e860 11c150 127ed0 129110
comma := ,
WRAP = $(patsubst %,-Wl$(comma)--wrap=f_%,$(WRAP_FNS))
PHASE_HOOKS = 1505a0 5111d0 5122e0 511300 13da10 13d260 405440 3f29e0 3c12f0 2b4590 4f94b0
DIFFICULTY_HOOKS = 1523a0 152370 3ed7d0 3edf90 3c4b80 3c3da0
MOD_A_HOOKS = 2fc860 2fc4e0 4014a0 3a7cb0 29efa0 3a89a0 3d61d0 3d60c0   # 405440 is already in PHASE_HOOKS
# Task 23 (runtime/mod_hooks_b.c): the mod DLL detours that were still missing.  3f29e0 is already in PHASE_HOOKS (the phase counter runs first).
MOD_B_HOOKS = 3f27d0 3c4780 351390 368820 4060e0 3d1730 3f65a0 3f6620 3f6510 3a17f0 153850 15aab0 15aba0 31dd70
HD_HOOKS = 106440 106460 106480
MOD_PARAMS_HOOKS = 40d810 40dd70
STREAM_HOOKS = 106da0 106fa0 106b40      # OpenStream / ReadStream / CloseStream on the loose tree (runtime/stream_layer.c)
# background-load list serialisation (runtime/bgload.c): loader loop + its blocking calls, the three enqueue entries, kn::MemoryAllocator::free, id->pointer (stress/selftest step)
BGLOAD_HOOKS = 3a0c60 15edd0 3f99e0 3f9850 3a64f0 3a0aa0 3a0b30 3a0bd0 19c470 4ad270
# scripted input clock (runtime/null_input.c): 0x105e10 is called once per game frame (end-of-frame task and the settings menu loop)
# Task 27: 0x2b7420 = the New Game menu task (runtime/phase_log.c: alive between entry and return; the route's newgame-* conditions)
INPUT_HOOKS = 105e10 2b7420
# Task 30 (diagnostic, runtime/route_state.c, KHNMC_TRACE_PLACE=1): YS::OBJ::set_pos 0x3b64d0 / 0x3b6510 and the rotation setters 0x3b6420 / 0x3b6560, passed through
PLACE_HOOKS = 3b64d0 3b6510 3b6420 3b6560 3b3200 3da940 419230
INPUT_HOOKS += $(PLACE_HOOKS)
# Task 31 (runtime/route_objective.c): 0x17b710 shows a line in the INFORMATION bar; the hook logs the line's text (`kh: information:`) and passes the call through
# and 0x3ad9a0 (message id -> text) is noted while a system menu runs: the camp menu's labels (`kh: menu text:`)
INFO_HOOKS = 17b710 3ad9a0
INPUT_HOOKS += $(INFO_HOOKS)
FILE_HOOKS = 1f9a80 3a9cf0 131ef0 106000 1063d0 106700 106790 106870 $(HD_HOOKS) $(PHASE_HOOKS) $(DIFFICULTY_HOOKS) $(MOD_A_HOOKS) $(MOD_B_HOOKS) $(MOD_PARAMS_HOOKS) $(STREAM_HOOKS) $(BGLOAD_HOOKS) $(INPUT_HOOKS)
# object-id table guard (runtime/idtable.c): 0x4ad390 registers a 32 MiB window in the game's 64-slot id table and encodes the pointer
IDTABLE_HOOKS = 4ad390
FILE_HOOKS += $(IDTABLE_HOOKS)
# KH2Moose pool sizes (runtime/moose.c): cache layout, cache free bytes, data heap release / reset (the four functions whose pool-start instruction the DLL re-encodes), reserved block sizes
MOOSE_HOOKS = 39d590 39dab0 3e5f30 3e5f90 39e260 39e270
FILE_HOOKS += $(MOOSE_HOOKS)
# Task 32 (runtime/events_stub.c, KHNMC_EVENTS=stub): 3a1f70 = find an event entry (BAR type 22) of the loaded area file by name; its only caller is the event starter 3ac350
EVENT_HOOKS = 3a1f70
FILE_HOOKS += $(EVENT_HOOKS)
# Task 37 (runtime/aspect.c, NATIVE-FILL-PLAN.md section 7): KHNMC_ASPECT=fill.  11c1e0 = H1, post-call: on a screen narrower than 16:9
# the largest-16:9 fit keeps the full height, so 0x14e050 / 0x4ed940 draw no bars; 186920 = the map pass, a pass-through that records
# the frame of the 3D view for H1's condition (H2b's m11 edit joined it in task 40, below).  The plan listed 2bd190 126500 156940
# 1569b0 156a70 156e20 126fd0 4b7530.  With KHNMC_ASPECT unset both are exact pass-throughs.
ASPECT_HOOKS = 11c1e0 186920
ifneq ($(shell grep -l '^void f_1948c0' $(ROOT)/runtime/aspect.c),)
ASPECT_HOOKS += 1948c0
endif
# Task 40 (runtime/aspect.c; plan steps 4, 5 and the H4/H5 part of 7): 186920 also = H2b (cam+0x98 / r around the map pass), 2bd190 =
# H2c (the same around the effect pass), 156f10 = H4a (the three full-screen fades, f_156940 / f_1569b0 / f_156a70, draw inside through
# it), 156e20 = H4b (the event letterbox bars, after it), 126fd0 = H5 (full-height scissor rects); 126500 and 19d040 are pass-throughs
# that record the measurement M with KHNMC_ASPECT_LOG=2 (the dk loop's setScreenMode, the cam+0x84 readers), as does bgload.c's 4ad270.
ASPECT_HOOKS += 2bd190 156f10 156e20 126fd0 126500 19d040
# Task 46 (runtime/aspect.c, H7): 11a330 = the 2D primitive submit (count vertices of the current vertex block; every sprite, rectangle
# and layout piece goes through it).  In fill mode on a screen narrower than 16:9 a 4-vertex rectangle that spans the whole height of
# the centred 16:9 band (a menu backdrop, a dim / fade rectangle) is stretched in y to the frame's first and last row before the
# lifted body draws it.  With nothing set or KHNMC_ASPECT=pc it is one flag test and the lifted body.
ASPECT_HOOKS += 11a330
# Task 49 (runtime/aspect.c, "the zoom of the tall screen"; KHNMC_FOV_ZOOM, fill mode, a screen taller than wide): 19cad0 = the camera
# matrix build (the scene cameras' distance cam+0x40 times the zoom around it, restored), 19d010 = the distance accessor (returns the
# distance the matrices were built with).  With KHNMC_ASPECT unset or pc both are one flag test and the lifted body.
ASPECT_HOOKS += 19cad0 19d010
FILE_HOOKS += $(ASPECT_HOOKS)
# Task 44, review (runtime/null_mf.c): the movie seam's three pass-through hooks.  5375d0 = the movie module's command function
# (command 0 = start: a request the game drops at its own guard is seen and said), 2cbd80 = the event's end (did the movie's event end
# by itself, by Skip or another way; which clean-up branch), 13d690 = the player's picture (the engine never frees it: the empty
# movie's picture is made once and handed out again).  13da10 (Movie::open) is in PHASE_HOOKS (phase_log.c tells the seam).
MOVIE_HOOKS = 5375d0 2cbd80 13d690
FILE_HOOKS += $(MOVIE_HOOKS)
# ---- Task 12: mod static edits ---------------------------------------------------------------------------------------------------
# The mod DLL patches code bytes in 28 exe functions and attaches two INLINE detours (inside 3c3fe0 and 360920).  Those 30 functions
# are re-lifted from the patched image into $(GEN)/mod_static_lifted.c and replace the vanilla lifted bodies through the alias list
# (vanilla bodies stay reachable as orig_f_<rva>).  MOD_STATIC_CHAINED = functions a runtime hook already owns: the hook keeps
# f_<rva> and forwards to mod_f_<rva> (the DLL's Detours trampoline runs the patched body: true for 3a89a0).
# MOD_STATIC_REPLACED (Task 23) = functions whose DLL detour never calls the trampoline: the runtime hook (3f29e0: DLL 0x306c0 in
# runtime/mod_hooks_b.c) replaces the function outright, the re-lifted body mod_f_<rva> is linked but dead, exactly like the patched
# bytes on the PC; tools/check_mod_static_link.py fails the build if any runtime object references it.
# $(GEN) holds mod/game-derived generated files: local only, never committed.
MOD_STATIC_FUNCS = 165110 2fb7e0 2fc500 2fd940 2fdd40 2fe010 360920 390150 3a89a0 3a9950 3b11e0 3b1780 3c1190 3c2f00 3c3fe0 3c5170 \
                   3c5440 3c5750 3c5900 3c5a90 3c5ce0 3d8b40 3de460 3e37f0 3e8380 3f29e0 3f3110 400b20 401d60 422780
MOD_STATIC_INLINE = 3c3fe0 360920
MOD_STATIC_CHAINED = 3a89a0
MOD_STATIC_REPLACED = 3f29e0
# Review of Task 23: every function detour of KH2Tweaks_Refactor.dll whose handler never reaches its trampoline (the 16 REPLACES rows
# of the table in implementation notes section 2; re-derived from the DLL: no reference to the slot between the handler's start and its
# INT3 padding).  The runtime object that defines f_<rva> must not reference orig_f_<rva>: a forward to the vanilla body is a path
# the PC game does not have (tools/check_mod_static_link.py --dll-replaces fails the build).
MOD_DLL_REPLACES = 29efa0 4014a0 3a9cf0 3c4b80 351390 2fc860 2fc4e0 3ed7d0 3c12f0 1f9a80 3f29e0 3f27d0 15aba0 15aab0 153850 3edf90
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
MOD_LIFT_CHAINED = $(MOD_STATIC_CHAINED) $(MOD_STATIC_REPLACED) $(MOD_NEVERLAND_HOOKS)
FILE_HOOKS += $(MOD_NEVERLAND_HOOKS)
# ---- Task 23: KH2Moose.dll's two ini-switched CODE patches (runtime/moose.c) ---------------------------------------------------------
#   92680   [kh2.fixes] scd_priority bit 1: the call at exe 0x927e2 becomes `xor eax,eax` + a 3-byte NOP
#   2b4590  [kh2.hacks] fast_title:         exe 0x2b46eb `lea edi,[r12-0xc]` becomes `mov edi,0x11`   (2b4590 is already in PHASE_HOOKS)
# Decoded statically from the DLL by tools/gen_mod_moose.py ($(GEN)/mod_moose_edits.json, mod_moose_table.h) and re-lifted by the Task 12
# mechanism (a third function table, kh_mod_moose_fn_table).  runtime/moose.c owns f_<rva> of both and runs mod_f_<rva> when the ini
# turns the patch on, orig_f_<rva> when it does not (the link check's --choice class).
MOD_MOOSE_FUNCS = 92680 2b4590
MOD_MOOSE_DLL ?= /home/user/projects/SillyBillyMod1/dll/KH2Moose.dll
MOD_MOOSE_EDITS = $(GEN)/mod_moose_edits.json
MOD_LIFT_FUNCS += $(MOD_MOOSE_FUNCS)
MOD_LIFT_CHAINED += $(MOD_MOOSE_FUNCS)
FILE_HOOKS += 92680
MOD_EXTRA_EDITS = --extra-edits $(MOD_NEVERLAND_EDITS) --extra-edits $(MOD_MOOSE_EDITS)
MOD_TWEAKS_DLL ?= /home/user/projects/SillyBillyMod1/dll/KH2Tweaks_Refactor.dll
# FILE_HOOKS can name an RVA twice (a runtime hook that forwards to a re-lifted body is in two sub-lists): the hook tools get each RVA once
HOOKS_UNIQ = $(sort $(FILE_HOOKS))
all: $(BIN)
# Optional vkd3d graphics backend (KHNMC_GFX=vkd3d: runtime/gfx_vkd3d.c, KHNMC_GFX=vkd3dw: runtime/gfx_wrap.c on the shared part of
# gfx_vkd3d.c): compiled in when the vkd3d libraries exist under VKD3D_PREFIX (make GFX_VKD3D=0 compiles it out: both are then named
# fatals).  Only gfx_vkd3d.o and gfx_wrap.o depend on the switch; the stamp relinks.
VKD3D_PREFIX ?= /home/user/work/khnmc-deps/vkd3d/usr
VKHEADERS_PREFIX ?= /home/user/work/khnmc-deps/vkheaders/usr
ifneq ($(wildcard $(VKD3D_PREFIX)/lib/libvkd3d.so),)
GFX_VKD3D ?= 1
else
GFX_VKD3D ?= 0
endif
# TASK36 (IOS-GRAPHICS-PLAN step 1): GFX_VULKAN_STATIC=1 links the Vulkan implementation instead of loading it at run time
# (-DKH_GFX_VULKAN_STATIC: vkd3d gets the program's own vkGetInstanceProcAddr through the gipa wrapper; gfx_vkd3d.o then references
# no dlopen).  Linux: -lvulkan (the loader; the runtime still selects lavapipe and refuses any device that is not a CPU), a test build
# only, default 0.  Darwin: MoltenVK's static library MOLTENVK_STATIC (PREPARED, NOT RUN on this host), the way the iOS app links it.
# runtime/gfx_present.c (the presenter, plan step 7) is compiled with the backend's flags; without the backend only its host part
# (the suspend gate, the display pacing of WaitForVBlank) remains.
GFX_VULKAN_STATIC ?= 0
ifeq ($(filter $(GFX_VULKAN_STATIC),0 1),)
$(error GFX_VULKAN_STATIC must be 0 or 1)
endif
# TASK38 (IOS-GRAPHICS-PLAN step 3, G1): an iPhone app cannot load a Vulkan library at run time, so IOS_SDK=iphoneos GFX_VKD3D=1 always
# links MoltenVK statically (GFX_VULKAN_STATIC=1).  The Simulator has no graphics backend: MoltenVK's iOS static library has only an
# ios-arm64 slice, so a Simulator build keeps the null backend.
ifeq ($(IOS_SDK)-$(GFX_VKD3D),iphoneos-1)
ifeq ($(GFX_VULKAN_STATIC),0)
ifeq ($(origin GFX_VULKAN_STATIC),command line)
$(error IOS_SDK=iphoneos GFX_VKD3D=1 GFX_VULKAN_STATIC=0: an iPhone app links MoltenVK statically, always; leave GFX_VULKAN_STATIC out)
endif
endif
GFX_VULKAN_STATIC = 1
endif
ifeq ($(IOS_SDK)-$(GFX_VKD3D),iphonesimulator-1)
$(error IOS_SDK=iphonesimulator GFX_VKD3D=1: there is no graphics backend for the Simulator (MoltenVK's iOS static library, MoltenVK.xcframework/ios-arm64, has no simulator slice: IOS-GRAPHICS-PLAN G1); build the Simulator app without GFX_VKD3D=1, i.e. with the null backend)
endif
GFX_STAMP = .gfx-vkd3d-$(GFX_VKD3D)$(if $(filter 1,$(GFX_VULKAN_STATIC)),-vkstatic)
ifeq ($(GFX_VKD3D),1)
ifeq ($(UNAME_S),Darwin)
# ---- Task 21b: the wrapped bridge on arm64 macOS (vkd3d over MoltenVK, i.e. Vulkan on Metal) ------------------------------------------
# `make GFX_VKD3D=1` on Darwin.  vkd3d 2.1 is linked STATICALLY from the four libraries tools/apple/vkd3d-build/build_khnmc.sh makes
# (no autotools on that host); its headers come from the same source tree, the Vulkan headers from VKHEADERS_DIR.  (The SPIR-V headers
# next to them are needed to BUILD vkd3d only, not for these two objects.)  Differences from the Linux block below:
#   * -DKH_GFX_VKD3D_STATIC=1: the static libraries already define the D3D12 interface GUIDs, so gfx_vkd3d.c does not (INITGUID there
#     would be duplicate symbols);
#   * no rpath and no -l: the archives are named by path and are prerequisites of the link;
#   * the Vulkan library is MoltenVK's dylib, compiled in as an ABSOLUTE path (-DKH_GFX_VULKAN_LIB; KHNMC_GFX_VULKAN_LIB overrides it
#     at run time).  Never through DYLD_LIBRARY_PATH: the system strips that from protected launchers such as nice;
#   * only the wrapped bridge exists there (bias addresses, arm64: no Windows-ABI trampoline), and the runtime picks no driver on its
#     own: KHNMC_GFX=vkd3dw KHNMC_GFX_DRIVER=system selects the Metal device.
# A missing piece is an error here, by name, not a build without graphics.
ifeq ($(IOS_SDK),iphoneos)
# ---- TASK38 (IOS-GRAPHICS-PLAN step 3, G1-G4): the iPhone app ------------------------------------------------------------------------
# `make IOS_SDK=iphoneos GFX_VKD3D=1 applib` (tools/ios/mac/build.sh with GFX=1) on the MacBook Air: vkd3d 2.1 built for iOS
# (tools/apple/vkd3d-build/README.md in the repo, "iOS build": four archives in build-iphoneos, compiled against deps/vkheaders) and
# MoltenVK's iOS static library with its headers (deps/moltenvk-ios, from the 1.4.2 release archive).  The graphics objects are
# compiled against the SAME Vulkan headers vkd3d was compiled with (VKHEADERS_DIR, first on the include path); MOLTENVK_INCLUDE comes
# after it and is there for MoltenVK's own private API header only (the memory census: gpuMemoryAllocated, -DKH_GFX_MVK_API).
# The app itself is linked by Xcode: GFX_APP_LDFLAGS is what tools/ios/mac/build.sh and sign_app.sh hand to xcodebuild as
# KHNMC_GFX_LDFLAGS (`make -s IOS_SDK=iphoneos GFX_VKD3D=1 print-GFX_APP_LDFLAGS`); Foundation and UIKit are already in the project.
GFX_APPLE_DIR ?= $(HOME)/Developer/KHNMC/deps
VKD3D_SRC ?= $(GFX_APPLE_DIR)/vkd3d-2.1
VKD3D_LIBDIR ?= $(VKD3D_SRC)/build-iphoneos
VKHEADERS_DIR ?= $(GFX_APPLE_DIR)/vkheaders
MOLTENVK_STATIC ?= $(GFX_APPLE_DIR)/moltenvk-ios/static/MoltenVK.xcframework/ios-arm64/libMoltenVK.a
MOLTENVK_INCLUDE ?= $(GFX_APPLE_DIR)/moltenvk-ios/include
else
GFX_APPLE_DIR ?= $(HOME)/khnmc-work/gfxprobe
VKD3D_SRC ?= $(GFX_APPLE_DIR)/vkd3d-2.1
VKD3D_LIBDIR ?= $(VKD3D_SRC)/build-mac
VKHEADERS_DIR ?= $(GFX_APPLE_DIR)/vkheaders
MOLTENVK_LIB ?= $(GFX_APPLE_DIR)/moltenvk/MoltenVK/dynamic/dylib/macOS/libMoltenVK.dylib
endif
VKD3D_STATIC = $(VKD3D_LIBDIR)/libvkd3d-utils.a $(VKD3D_LIBDIR)/libvkd3d.a $(VKD3D_LIBDIR)/libvkd3d-shader.a $(VKD3D_LIBDIR)/libvkd3d-common.a
ifeq ($(IOS_SDK),iphoneos)
GFX_NEED = $(VKD3D_STATIC) $(VKD3D_SRC)/include/vkd3d_d3d12.h $(VKD3D_SRC)/include/vkd3d.h $(VKHEADERS_DIR)/vulkan/vulkan.h $(MOLTENVK_STATIC) $(MOLTENVK_INCLUDE)/MoltenVK/mvk_private_api.h
GFX_MISSING = $(filter-out $(wildcard $(GFX_NEED)),$(GFX_NEED))
ifneq ($(strip $(GFX_MISSING)),)
$(error IOS_SDK=iphoneos GFX_VKD3D=1: missing $(GFX_MISSING) (GFX_APPLE_DIR=$(GFX_APPLE_DIR): VKD3D_SRC, VKD3D_LIBDIR, VKHEADERS_DIR, MOLTENVK_STATIC, MOLTENVK_INCLUDE; see tools/apple/vkd3d-build/README.md in the repo, "iOS build"))
endif
else ifeq ($(GFX_VULKAN_STATIC),1)
# TASK36, PREPARED, NOT RUN: MoltenVK linked statically (the release's static xcframework; macOS slice here, the app links the ios-arm64
# one), no dylib and no loader; the Metal surface and the Metal-objects export are compiled in (VK_USE_PLATFORM_METAL_EXT)
MOLTENVK_STATIC ?= $(GFX_APPLE_DIR)/moltenvk/MoltenVK/static/MoltenVK.xcframework/macos-arm64_x86_64/libMoltenVK.a
GFX_NEED = $(VKD3D_STATIC) $(VKD3D_SRC)/include/vkd3d_d3d12.h $(VKD3D_SRC)/include/vkd3d.h $(VKHEADERS_DIR)/vulkan/vulkan.h $(MOLTENVK_STATIC)
else
GFX_NEED = $(VKD3D_STATIC) $(VKD3D_SRC)/include/vkd3d_d3d12.h $(VKD3D_SRC)/include/vkd3d.h $(VKHEADERS_DIR)/vulkan/vulkan.h $(MOLTENVK_LIB)
endif
GFX_MISSING = $(filter-out $(wildcard $(GFX_NEED)),$(GFX_NEED))
ifneq ($(strip $(GFX_MISSING)),)
$(error GFX_VKD3D=1 on Darwin: missing $(GFX_MISSING) (VKD3D_SRC, VKD3D_LIBDIR, VKHEADERS_DIR, MOLTENVK_LIB / MOLTENVK_STATIC; see tools/apple/vkd3d-build/README.md in the repo))
endif
ifeq ($(IOS_SDK),iphoneos)
# TASK38: the iPhone flags.  -DKH_GFX_MVK_API: the census reads MoltenVK's statistics (runtime/gfx_vkd3d.c).  The frameworks MoltenVK
# needs on iOS (its UserGuide: Metal, QuartzCore, IOSurface, CoreGraphics, Foundation, UIKit; no IOKit, no AppKit) and libc++ (MoltenVK
# is C++).  GFX_LDFLAGS (a link the Makefile would make itself) = GFX_APP_LDFLAGS + the two frameworks the Xcode project already has.
$(O)/gfx_vkd3d.o $(O)/gfx_wrap.o $(O)/gfx_present.o: CFLAGS += -DKH_GFX_VKD3D=1 -DKH_GFX_VKD3D_STATIC=1 -DKH_GFX_VULKAN_STATIC=1 -DVK_USE_PLATFORM_METAL_EXT -DKH_GFX_MVK_API=1 -DWIDL_C_INLINE_WRAPPERS -I$(VKD3D_SRC)/include -I$(VKHEADERS_DIR) -I$(MOLTENVK_INCLUDE)
GFX_APP_LDFLAGS = $(VKD3D_STATIC) $(MOLTENVK_STATIC) -lc++ -framework Metal -framework QuartzCore -framework IOSurface -framework CoreGraphics
GFX_LDFLAGS = $(GFX_APP_LDFLAGS) -framework Foundation -framework UIKit
$(BIN): $(VKD3D_STATIC) $(MOLTENVK_STATIC)
else ifeq ($(GFX_VULKAN_STATIC),1)
$(O)/gfx_vkd3d.o $(O)/gfx_wrap.o $(O)/gfx_present.o: CFLAGS += -DKH_GFX_VKD3D=1 -DKH_GFX_VKD3D_STATIC=1 -DKH_GFX_VULKAN_STATIC=1 -DVK_USE_PLATFORM_METAL_EXT -DWIDL_C_INLINE_WRAPPERS -I$(VKD3D_SRC)/include -I$(VKHEADERS_DIR)
GFX_LDFLAGS = $(VKD3D_STATIC) $(MOLTENVK_STATIC) -lc++ -framework Metal -framework QuartzCore -framework IOSurface -framework CoreGraphics -framework Foundation -framework IOKit -framework AppKit
$(BIN): $(VKD3D_STATIC) $(MOLTENVK_STATIC)
else
ifeq ($(filter /%,$(MOLTENVK_LIB)),)
$(error MOLTENVK_LIB must be an absolute path (got $(MOLTENVK_LIB)): the runtime loads exactly that file)
endif
$(O)/gfx_vkd3d.o $(O)/gfx_wrap.o $(O)/gfx_present.o: CFLAGS += -DKH_GFX_VKD3D=1 -DKH_GFX_VKD3D_STATIC=1 -DWIDL_C_INLINE_WRAPPERS -I$(VKD3D_SRC)/include -I$(VKHEADERS_DIR) '-DKH_GFX_VULKAN_LIB="$(MOLTENVK_LIB)"'
GFX_LDFLAGS = $(VKD3D_STATIC)
$(BIN): $(VKD3D_STATIC)
endif
else
$(O)/gfx_vkd3d.o $(O)/gfx_wrap.o $(O)/gfx_present.o: CFLAGS += -DKH_GFX_VKD3D=1 -DWIDL_C_INLINE_WRAPPERS -I$(VKD3D_PREFIX)/include/vkd3d -I$(VKHEADERS_PREFIX)/include
GFX_LDFLAGS = -L$(VKD3D_PREFIX)/lib -lvkd3d-utils -lvkd3d -ldl -Wl,--disable-new-dtags -Wl,-rpath,$(VKD3D_PREFIX)/lib
ifeq ($(GFX_VULKAN_STATIC),1)
$(O)/gfx_vkd3d.o $(O)/gfx_wrap.o $(O)/gfx_present.o: CFLAGS += -DKH_GFX_VULKAN_STATIC=1
GFX_LDFLAGS += -lvulkan
endif
endif
endif
$(O)/$(GFX_STAMP):
	@mkdir -p $(O) && rm -f $(O)/.gfx-vkd3d-* && touch $@
$(O)/gfx_vkd3d.o $(O)/gfx_wrap.o $(O)/gfx_present.o $(BIN): $(O)/$(GFX_STAMP)
# ---- lifted shards compiled here (every variant except the default one, which links the prebuilt archive) ----
SHARDS_DIR ?= $(LIFTED)/shards
SHARD_NAMES := $(patsubst %.c,%,$(notdir $(wildcard $(SHARDS_DIR)/shard_*.c)))
SHARD_OBJ = $(patsubst %,$(OL)/lifted/%.o,$(SHARD_NAMES))
# the lifter's flags (lifted/tools/buildgen.py, docs/LIFT-POLICY.md section 8) + the address mode
LIFT_CFLAGS = -O1 -w -fno-strict-aliasing -fwrapv -fno-delete-null-pointer-checks -fno-math-errno -ffp-contract=off -I$(ROOT)/lifter-rt -I$(LIFTED)/gen $(XFLAGS) $(OMFLAGS) $(LIFT_EXTRA)
# task 39: the selftest counts the shards compiled with ORDERED_MEM=1 in the binary against this number (runtime/ordered_mem.c)
$(O)/ordered_mem.o: CFLAGS += -DKH_OM_SHARDS=$(words $(SHARD_NAMES))
# task 39: no object of any variant lists lifter-rt/*.h as a prerequisite (an edit of khrt.h rebuilt nothing).  The ORDERED_MEM=1
# variant does (its code is in khrt.h); the plain variants are left as they were, so this change rebuilds none of their objects.
ifeq ($(ORDERED_MEM),1)
$(SHARD_OBJ) $(RTOBJ) $(LIFTOBJ) $(O)/loader_app.o $(O)/hook_forwarders.o: $(wildcard $(ROOT)/lifter-rt/*.h)
endif
ifeq ($(HOOKMODE),objcopy)
$(OL)/lifted/%.o: $(SHARDS_DIR)/%.c
	@mkdir -p $(OL)/lifted && $(CC) $(LIFT_CFLAGS) -c $< -o $@
$(OL)/libkh2lifted.a: $(SHARD_OBJ)
	@test -n "$(SHARD_OBJ)" || { echo "no shard_*.c in $(SHARDS_DIR)" >&2; exit 1; }
	rm -f $@ && ar rcs $@ $(SHARD_OBJ)
# Weak definitions also allow same-shard relocations to bind to the runtime hooks.
$(OL)/libkh2loose.a: $(LIFTLIB) Makefile tools/alias_file_hooks.py
	@mkdir -p $(OL)
	python3 tools/alias_file_hooks.py $< $@ $(FILE_HOOKS)
HOOK_LINK = $(WRAP)
else
# HOOKMODE=rename: the generated copy of the shards.  The stamp reruns the generator when a shard, the hook lists (Makefile) or the
# tool changes; the generator rewrites only files whose content changed, so unchanged shards are not recompiled.
# the expanded hook lists as a file that changes only when the lists do (also when they are overridden on the command line)
$(OL)/hooks.list: FORCE
	@mkdir -p $(OL); echo "hooks: $(strip $(HOOKS_UNIQ)) wrap: $(strip $(WRAP_FNS))" | cmp -s - $@ || echo "hooks: $(strip $(HOOKS_UNIQ)) wrap: $(strip $(WRAP_FNS))" > $@
FORCE:
$(OL)/shards/.stamp: $(wildcard $(SHARDS_DIR)/shard_*.c) Makefile tools/hook_shards.py $(OL)/hooks.list
	@mkdir -p $(OL)/shards
	python3 tools/hook_shards.py shards $(SHARDS_DIR) $(OL)/shards --hooks $(HOOKS_UNIQ) --wrap $(WRAP_FNS)
	@touch $@
SHARD_GEN = $(patsubst %,$(OL)/shards/%.c,$(SHARD_NAMES))
$(SHARD_GEN): $(OL)/shards/.stamp ;
$(OL)/lifted/%.o: $(OL)/shards/%.c
	@mkdir -p $(OL)/lifted && $(CC) $(LIFT_CFLAGS) -c $< -o $@
$(OL)/libkh2loose.a: $(SHARD_OBJ)
	@test -n "$(SHARD_OBJ)" || { echo "no shard_*.c in $(SHARDS_DIR)" >&2; exit 1; }
	rm -f $@ && ar rcs $@ $(SHARD_OBJ)
# forwarders: f_X -> __wrap_f_X for the wrapped functions, and f_X -> orig_f_X (with a NOTE) for a hooked function that NO linked
# object defines.  The scan covers every directly linked object ($(RTOBJ) and $(LIFTOBJ): a replacement may also be a re-lifted body
# in another lifted object), so it is driven by the final FILE_HOOKS list and by what is really in the link.
$(O)/hook_forwarders.c: $(RTOBJ) $(LIFTOBJ) Makefile tools/hook_shards.py $(OL)/hooks.list
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
	@mkdir -p $(@D) && $(CC) $(CFLAGS) -c $< -o $@
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
$(BIN): $(RTOBJ) $(LIFTOBJ) $(HOOK_OBJ) $(OL)/libkh2loose.a
	$(CC) -o $@ $(RTOBJ) $(LIFTOBJ) $(HOOK_OBJ) $(OL)/libkh2loose.a -lm -lpthread $(LDEXPORT) $(HOOK_LINK) $(GFX_LDFLAGS) $(COREAUDIO_LDFLAGS)
# ---- Task 26: the game as two static libraries and a shell that owns main() (what an iOS app links) ----
APP_RTOBJ = $(filter-out $(O)/loader.o,$(RTOBJ)) $(O)/loader_app.o
$(O)/loader_app.o: $(ROOT)/runtime/loader.c $(wildcard $(ROOT)/runtime/*.h)
	@mkdir -p $(O) && $(CC) $(CFLAGS) -DKH_MAIN_NAME=kh_main -c $< -o $@
$(O)/libkhnmc-rt.a: $(APP_RTOBJ) $(LIFTOBJ) $(HOOK_OBJ)
	rm -f $@ && ar rcs $@ $(APP_RTOBJ) $(LIFTOBJ) $(HOOK_OBJ)
	$(if $(filter 1,$(GFX_VKD3D)),@nm -g $@ > $@.syms 2>/dev/null && test -s $@.syms || { echo "applib: nm could not read $@ (non-zero exit status or no symbols): whether it has the graphics backend is unknown" >&2; rm -f $@ $@.syms; exit 1; }; grep -q ' T _*kh_gfxc_device_new$$' $@.syms || { echo "applib: GFX_VKD3D=1 but $@ has no graphics backend (no kh_gfxc_device_new: gfx_vkd3d.o was compiled without KH_GFX_VKD3D)" >&2; rm -f $@ $@.syms; exit 1; }; rm -f $@.syms)
applib: $(O)/libkhnmc-rt.a $(OL)/libkh2loose.a
ifeq ($(UNAME_S),Darwin)
APP_WHOLE = -Wl,-force_load,$(O)/libkhnmc-rt.a
else
APP_WHOLE = -Wl,--whole-archive $(O)/libkhnmc-rt.a -Wl,--no-whole-archive
endif
$(BIN)-shell: $(ROOT)/tools/ios/host_shell.c $(O)/libkhnmc-rt.a $(OL)/libkh2loose.a
	$(CC) $(CFLAGS) -o $@ $(ROOT)/tools/ios/host_shell.c $(APP_WHOLE) $(OL)/libkh2loose.a -lm -lpthread $(LDEXPORT) $(HOOK_LINK) $(GFX_LDFLAGS)
hostshell: $(O)/hostshell.ok
$(O)/hostshell.ok: $(BIN)-shell tools/check_hook_link.py tools/check_mod_static_link.py tools/check_ios_symbols.py
	python3 tools/check_hook_link.py --bin $(BIN)-shell --objs $(APP_RTOBJ) $(LIFTOBJ) $(HOOK_OBJ) --hooks $(HOOKS_UNIQ) --wrap $(WRAP_FNS) $(if $(HOOK_OBJ),--forwarders $(HOOK_OBJ)) $(if $(filter 1,$(HOOK_STRICT)),--strict)
	python3 tools/check_mod_static_link.py --bin $(BIN)-shell --obj $(O) --skip $(O)/loader.o --funcs "$(MOD_LIFT_FUNCS)" --chained "$(MOD_STATIC_CHAINED) $(MOD_NEVERLAND_HOOKS)" --replaced "$(MOD_STATIC_REPLACED)" --choice "$(MOD_MOOSE_FUNCS)" --dll-replaces "$(MOD_DLL_REPLACES)"
	$(if $(filter 1,$(IOS_PATHS)),python3 tools/check_ios_symbols.py --bin $(BIN)-shell $(IOS_SYMBOLS_ALLOW))
	@touch $@
# the same three checks for a program linked elsewhere from these libraries (the app binary Xcode links: tools/ios/mac/build.sh);
# PREPARED, NOT RUN with an app binary:  make IOS_SDK=<sdk> appcheck APP_BIN=<path to KHNMC.app/KHNMC>
appcheck:
	@test -n "$(APP_BIN)" && test -f "$(APP_BIN)" || { echo "appcheck: APP_BIN=<linked program> is required" >&2; exit 2; }
	python3 tools/check_hook_link.py --bin $(APP_BIN) --objs $(APP_RTOBJ) $(LIFTOBJ) $(HOOK_OBJ) --hooks $(HOOKS_UNIQ) --wrap $(WRAP_FNS) $(if $(HOOK_OBJ),--forwarders $(HOOK_OBJ)) $(if $(filter 1,$(HOOK_STRICT)),--strict)
	python3 tools/check_mod_static_link.py --bin $(APP_BIN) --obj $(O) --skip $(O)/loader.o --funcs "$(MOD_LIFT_FUNCS)" --chained "$(MOD_STATIC_CHAINED) $(MOD_NEVERLAND_HOOKS)" --replaced "$(MOD_STATIC_REPLACED)" --choice "$(MOD_MOOSE_FUNCS)" --dll-replaces "$(MOD_DLL_REPLACES)"
	python3 tools/check_ios_symbols.py --bin $(APP_BIN) $(IOS_SYMBOLS_ALLOW)
# Task 26: a KH_IOS_PATHS program must not import what an iOS app cannot use (fork, system, popen, exec*, posix_spawn, tmpfile, ...)
# TASK38 (IOS-GRAPHICS-PLAN G4): a graphics build (GFX_VKD3D=1) may import exactly one of those names, mktemp, and only because
# MoltenVK's iOS static library imports _mktemp (and _mkfifo, which is not on the list), apparently for its GPU-capture pipe (llvm-nm of
# the 1.4.2 release's MoltenVK.xcframework/ios-arm64/libMoltenVK.a).  Every other forbidden import still fails the check.  On Linux the
# hostshell links vkd3d's SHARED libraries and dlopens the Vulkan loader, so there the check sees the runtime's own imports only (it
# says whether an allowed name was imported at all); only the app's appcheck sees MoltenVK's and vkd3d's.
IOS_SYMBOLS_ALLOW = $(if $(filter 1,$(GFX_VKD3D)),--allow mktemp)
ifeq ($(IOS_PATHS),1)
all: $(O)/ios_symbols.ok
$(O)/ios_symbols.ok: $(BIN) tools/check_ios_symbols.py
	python3 tools/check_ios_symbols.py --bin $(BIN) $(IOS_SYMBOLS_ALLOW) && touch $@
endif
# arm64 compile check of the hand-written runtime only (no lifted shards): clang -arch arm64 on macOS
arm64check:
	@for f in $(RTSRC) $(ROOT)/lifter-rt/khrt_cpu.c $(ROOT)/lifter-rt/rt_libm.c; do echo CC $$f; clang -arch arm64 -std=gnu11 -O1 -w -fno-strict-aliasing -fwrapv -fno-math-errno -ffp-contract=off -I$(ROOT)/lifter-rt -I$(ROOT)/runtime -I$(LIFTED)/gen -DKH_XLATE_MODE=2 $(OMFLAGS) -c $$f -o /tmp/khnmc_arm_chk.o || exit 1; done
# ---- Task 12 rules: generated table (python3 stdlib), re-lift (needs the lifter environment: KHLIFT, KHLIFT_PY, KH_UNIVERSE), link check
ifeq ($(GEN_PREBUILT),1)
# the generated files are inputs here (tools/darwin_build.sh copies gen/ from the Linux dev host); a rule without prerequisites
# runs only when the file is missing, and then it is an error, not a silent build without the mod's static edits
$(GEN)/mod_static_table.h $(GEN)/mod_neverland_table.h $(GEN)/mod_moose_table.h $(GEN)/mod_static_lifted.c:
	@echo "GEN_PREBUILT=1: $@ is missing.  It is generated on the Linux dev host (make there, then copy gen/: tools/darwin_build.sh does)" >&2; exit 1
else
$(GEN)/mod_static_table.h: tools/gen_mod_static.py $(MOD_EDITS) Makefile
	python3 tools/gen_mod_static.py --edits $(MOD_EDITS) --exe $(MOD_EXE) --universe $(KH_UNIVERSE) --funcs "$(MOD_STATIC_FUNCS)" \
	    --inline "$(MOD_STATIC_INLINE)" --chained "$(MOD_STATIC_CHAINED) $(MOD_STATIC_REPLACED)" --out $@ --report $(GEN)/mod_static_classify.json
GEN_MOD_NEVERLAND = python3 tools/gen_mod_neverland.py --dll $(MOD_NEVERLAND_DLL) --exe $(MOD_EXE) --universe $(KH_UNIVERSE) --edits $(MOD_EDITS) \
	    --out-json $(MOD_NEVERLAND_EDITS) --out-h $(GEN)/mod_neverland_table.h
$(MOD_NEVERLAND_EDITS): tools/gen_mod_neverland.py tools/gen_mod_static.py $(MOD_NEVERLAND_DLL) $(MOD_EDITS) Makefile
	$(GEN_MOD_NEVERLAND)
# one run writes both files; the header rule only regenerates when the header alone went missing
$(GEN)/mod_neverland_table.h: $(MOD_NEVERLAND_EDITS)
	@test -f $@ || $(GEN_MOD_NEVERLAND)
GEN_MOD_MOOSE = python3 tools/gen_mod_moose.py --dll $(MOD_MOOSE_DLL) --exe $(MOD_EXE) --universe $(KH_UNIVERSE) --edits $(MOD_EDITS) \
	    --out-json $(MOD_MOOSE_EDITS) --out-h $(GEN)/mod_moose_table.h
$(MOD_MOOSE_EDITS): tools/gen_mod_moose.py tools/gen_mod_static.py $(MOD_MOOSE_DLL) $(MOD_EDITS) Makefile
	$(GEN_MOD_MOOSE)
$(GEN)/mod_moose_table.h: $(MOD_MOOSE_EDITS)
	@test -f $@ || $(GEN_MOD_MOOSE)
$(GEN)/mod_static_lifted.c: tools/mod_static_lift.py $(MOD_EDITS) $(MOD_NEVERLAND_EDITS) $(MOD_MOOSE_EDITS) $(LIFTED)/shards.json Makefile
	KH_UNIVERSE=$(KH_UNIVERSE) $(KHLIFT_PY) tools/mod_static_lift.py --khlift $(KHLIFT) --edits $(MOD_EDITS) --exe $(MOD_EXE) $(MOD_EXTRA_EDITS) \
	    --funcs "$(MOD_LIFT_FUNCS)" --chained "$(MOD_LIFT_CHAINED)" --vanilla-shards $(LIFTED)/shards --out $@ --report $(GEN)/mod_static_lift.json
endif
$(O)/mod_neverland.o $(O)/mod_neverland_test.o: $(GEN)/mod_neverland_table.h
$(O)/moose.o: $(GEN)/mod_moose_table.h
$(O)/mod_static.o: $(GEN)/mod_static_table.h
$(O)/mod_static_lifted.o: $(GEN)/mod_static_lifted.c
	@mkdir -p $(O) && $(CC) $(LIFT_CFLAGS) -c $< -o $@
all: $(O)/mod_static_link.ok
$(O)/mod_static_link.ok: $(BIN) tools/check_mod_static_link.py
	python3 tools/check_mod_static_link.py --bin $(BIN) --obj $(O) --funcs "$(MOD_LIFT_FUNCS)" --chained "$(MOD_STATIC_CHAINED) $(MOD_NEVERLAND_HOOKS)" --replaced "$(MOD_STATIC_REPLACED)" --choice "$(MOD_MOOSE_FUNCS)" --dll-replaces "$(MOD_DLL_REPLACES)" && touch $@
# native-oracle differential of the re-lifted functions against the patched bytes (x86-64 Linux dev host; not part of `all`).
# CPU-heavy: runs niced, MOD_STATIC_DIFFTEST_JOBS processes at a time (keep it low on a shared desktop).
MOD_STATIC_DIFFTEST_JOBS ?= 4
mod-static-difftest:
	KH_UNIVERSE=$(KH_UNIVERSE) nice -n 10 $(KHLIFT_PY) tools/mod_static_difftest.py --jobs $(MOD_STATIC_DIFFTEST_JOBS) --khlift $(KHLIFT) --edits $(MOD_EDITS) --exe $(MOD_EXE) \
	    $(MOD_EXTRA_EDITS) --funcs "$(MOD_LIFT_FUNCS)" --chained "$(MOD_LIFT_CHAINED)" --vanilla-shards $(LIFTED)/shards --lifted $(GEN)/mod_static_lifted.c \
	    --sites $(ROOT)/runtime/mod_static_sites.c --work $(O)/mod_static_difftest --out $(GEN)/mod_static_difftest.json $(MOD_STATIC_DIFFTEST_ARGS)
# the three NeverlandFly functions only (Task 19): same tool, own result file
mod-neverland-difftest: $(MOD_NEVERLAND_EDITS)
	KH_UNIVERSE=$(KH_UNIVERSE) nice -n 10 $(KHLIFT_PY) tools/mod_static_difftest.py --jobs $(MOD_STATIC_DIFFTEST_JOBS) --khlift $(KHLIFT) --edits $(MOD_EDITS) --exe $(MOD_EXE) \
	    $(MOD_EXTRA_EDITS) --funcs "$(MOD_LIFT_FUNCS)" --chained "$(MOD_LIFT_CHAINED)" --vanilla-shards $(LIFTED)/shards --lifted $(GEN)/mod_static_lifted.c \
	    --sites $(ROOT)/runtime/mod_static_sites.c --work $(O)/mod_static_difftest --only "$(shell echo $(MOD_NEVERLAND_HOOKS) | tr ' ' ',')" \
	    --out $(GEN)/mod_neverland_difftest.json $(MOD_STATIC_DIFFTEST_ARGS)
# the two KH2Moose functions only (Task 23): same tool, own result file
mod-moose-difftest: $(MOD_NEVERLAND_EDITS) $(MOD_MOOSE_EDITS)
	KH_UNIVERSE=$(KH_UNIVERSE) nice -n 10 $(KHLIFT_PY) tools/mod_static_difftest.py --jobs $(MOD_STATIC_DIFFTEST_JOBS) --khlift $(KHLIFT) --edits $(MOD_EDITS) --exe $(MOD_EXE) \
	    $(MOD_EXTRA_EDITS) --funcs "$(MOD_LIFT_FUNCS)" --chained "$(MOD_LIFT_CHAINED)" --vanilla-shards $(LIFTED)/shards --lifted $(GEN)/mod_static_lifted.c \
	    --sites $(ROOT)/runtime/mod_static_sites.c --work $(O)/mod_static_difftest --only "$(shell echo $(MOD_MOOSE_FUNCS) | tr ' ' ',')" \
	    --out $(GEN)/mod_moose_difftest.json $(MOD_STATIC_DIFFTEST_ARGS)
# Task 23: every KH2Tweaks_Refactor.dll pointer slot the hand ports rely on, resolved statically from the DLL's scanners against the exe
mod-dll-slots:
	python3 tools/mod_dll_slots.py --dll $(MOD_TWEAKS_DLL) --exe $(MOD_EXE) --expect tools/mod_dll_slots_h23.txt --json $(GEN)/mod_dll_slots.json
# compile every hand-written and lifter-rt object of the variant without the shards and without linking (`make -k rtobjs`: all errors of a port)
rtobjs: $(RTOBJ) $(LIFTOBJ)
# `make -s print-OL`, `print-BIN`: the value of a variable of this variant (tools/darwin_remote_build.sh)
print-%: ; @echo $($*)
clean:
	rm -rf $(O) $(BIN)
.PHONY: all clean arm64check rtobjs FORCE mod-static-difftest mod-neverland-difftest mod-moose-difftest mod-dll-slots applib hostshell appcheck
