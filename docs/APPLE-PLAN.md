# From the Linux build to the iPhone: plan (2026-11-16 night)

State: on x86-64 Linux the lifted game boots, plays the field and battle route headless, renders its title screen through vkd3d
(CPU Vulkan), mixes real sound, and carries the mod's code layer. Nothing has run on Apple hardware yet except compile checks.

## Step 1 (task 20): the same headless build on arm64 macOS (Mac mini, over ssh)
Goal: `khnmc --selftest` (all checks) and the null-backend field route on Apple Silicon. This proves the three things iOS shares with
macOS: arm64 code generation of the 173 MB of lifted C, the Darwin runtime (Mach-O, 16 KiB pages, no GNU linker tricks), and the float
policy (docs/LIFT-POLICY.md) on real arm64.

Known work:
- **Timing layer**: `pthread_condattr_setclock`, `clock_nanosleep`/`TIMER_ABSTIME` and `pthread_mutex_timedlock` do not exist on Darwin
  (arm64 compile check 2026-11-16: bgload.c, k32.c, threads.c fail; shell.c, null_audio.c, audio_mix.c pass monotonic deadlines to
  `pthread_cond_timedwait`, which is a realtime-clock call there). One compat header: condition init, timed wait from a monotonic
  deadline (`pthread_cond_timedwait_relative_np`), sleep-until, mutex timed lock.
- **Hooks without objcopy/--wrap**: the Linux build weakens `f_<rva>` in the archive and adds `orig_f_<rva>` aliases with GNU objcopy,
  and uses `-Wl,--wrap` for seven functions. Mach-O has neither. Plan: in the Darwin build's COPY of the shards rename the definition
  line of every hooked function to `orig_f_<rva>` (`__real_f_<rva>` for the wrapped seven, plus a generated one-line `f_<rva>` that calls
  `__wrap_f_<rva>`); call sites and `kh_table.c` keep `f_<rva>` and bind to the runtime's strong definition. A hooked RVA without a runtime
  definition becomes a link error (loud), which is what we want. The hook lists stay in one place (the Makefile variables).
- **Memory**: the image is mapped at guest 0x140000000 with 4 KiB section granularity; macOS/iOS pages are 16 KiB (map the whole image
  read-write: lifted code never executes guest bytes). Check that 0x140000000 is free in a macOS arm64 process (the dyld shared cache
  starts at 0x180000000) and what iOS needs (extended virtual addressing entitlement, or a relocated image base: the lifter's flat
  addresses make relocation expensive, so find out early). The compact guest arena from task 17 must sit in the same window.
- Fault reports (`ucontext` on Darwin), `dl_iterate_phdr` (vkd3d backend only, compiled out here), `-rdynamic`.
- Data for the route: only the files the route reads (about 320 files per the file log), not the 60 GB tree.

## Step 2 (task 21): pixels on Apple
Two candidates; try (a) first because it reuses what was proven tonight:
- (a) **vkd3d over MoltenVK** (Vulkan on Metal). The backend `runtime/win64/gfx_vkd3d.c` (bundle replay, swapchain on resources, frame
  dumps) is reused unchanged. Needs vkd3d built for arm64 Darwin (plain C; generated headers can come from the 2.1 release), MoltenVK,
  and an arm64 call bridge: vkd3d's C methods take explicit pointers for struct returns, so one generic trampoline (rcx,rdx,r8,r9,stack
  -> x0..x7,stack) covers every all-integer method; the few methods with float parameters (ClearDepthStencilView and similar) get an
  exception table. The game uses no geometry/tessellation/compute shaders (docs/graphics), which is what MoltenVK lacks.
  Offscreen rendering works without a window, so the Mac mini can produce frames over ssh. LGPL is acceptable only because the app is
  personal and never distributed.
- (b) **own D3D12-on-Metal shim** for the 75 methods (docs/graphics, runtime/metal-shim, shaders-msl): no third-party stack, but every
  D3D12 semantic the game relies on has to be re-implemented; the vkd3d Linux build is the pixel reference. Fallback if (a) is too slow
  or MoltenVK rejects something.

## Step 3: the iOS app (MacBook Air, owner-gated)
App shell per docs/IOS-SHELL.md: Metal layer, GameController input, audio output from the software mixer (task 13 writes the mixed
stream; an AudioUnit/AVAudioEngine render callback replaces the WAV writer), data in the Files area, increased-memory entitlement,
signing with the owner's paid team, install on the Duo. Cutscenes/movies are out of scope (owner).

## Not blocked on Apple, still open on Linux
Task 12 (static edits) merge, task 17 (heap/id table + KH2Moose heap sizes), task 18 (field/battle rendering + video), task 19
(NeverlandFly.dll), scripted item use on Less Than Nothing, the consumable-extension sites, save/load round trip, long-play soak.

## Update 2026-11-17 01:55 (before task 20 handed back; its run folder shows the selftest passing on the Mac mini in bias mode)
- **Bias mode changes the graphics bridge.** The Linux vkd3d backend hands HOST vkd3d objects straight to the lifted code and forwards
  vtable calls through one trampoline. That only works when guest pointers are host pointers (identity). On Apple the runtime uses
  bias mode (guest 0x140000000.. = host 0x300000000..): a host object's vtable pointer, read by lifted code, would be translated
  again and miss. So the Apple backend needs GUEST-side COM objects (as the null recorder already has: 75 methods on 19 interfaces
  with KH_HOSTFN vtables) whose methods translate pointers, unwrap/wrap interface arguments and call the real implementation in C.
  What sits underneath can still be vkd3d over MoltenVK (first choice: same D3D12 semantics, proven backend logic) or Metal directly.
  This wrapped bridge is also correct on Linux in bias mode, where it can be checked against the trampoline backend frame for frame
  before any Mac is involved: task 21a (Linux), then 21b (Mac mini with MoltenVK, offscreen).
- **Third-party pieces are downloaded and verified** on build-host (`~/work/khnmc-deps/apple/README.md`): vkd3d 2.1 source (good signature by
  the Wine release key), MoltenVK 1.4.2 for macOS and iOS (digests match GitHub's), SPIRV-Headers. Owner approved them inside
  `~/khnmc-work` on the Mac mini.
- **MacBook Air is online** (owner, 01:52): Xcode 27.2, iPhoneOS SDK 27.2, Metal compiler present, 8 GB RAM, 21 GB free. Rules: own
  folder `~/Developer/KHNMC`, shared lock `~/Developer/.mac-build.lock` (two conventions are in use on that Mac: an fcntl lock on the
  file, and "file exists with a description = busy"; honour both and check `pgrep -x xcodebuild`), DEVELOPER_DIR per command, no Terminal
  windows, mute before any simulator run, never touch the "iPhone Duo" simulator or the physical phone, no retry loops, windowless
  launchd job for anything needing the login session; signing/installing on the phone stays owner-gated. Task 26: compile the runtime and
  the lifted C for iOS there (2 jobs at most: 8 GB RAM), then boot the selftest inside an iOS Simulator of our own.
