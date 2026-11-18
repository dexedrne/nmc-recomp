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

## App icon (owner's choice, 2026-11-17)
The icon is a clean 1024x1024 redraw of the mod's own emblem (the red-outlined kneeling silhouette with two keyblades, from the
mod's `icon.png`), chosen by the owner from three candidates. It is derived from the mod's art, so it is NOT committed: it lives at
`~/Documents/khnmc/app-icon/AppIcon-1024.png` (opaque RGB, as iOS requires). The app shell step adds it at build time as a
single-size `AppIcon` set in an asset catalog (one 1024x1024 image; Xcode derives the other sizes).

## Memory on the iPhone (2026-11-17)
- What the port reserves (bias mode): 12 GiB of guest space (image and main heap 1.8 GiB, second heap zone 10.4 GiB) plus a 5 GiB
  guard below the image: 17 GiB of ADDRESS SPACE, no access, committed only where used. What it uses: on the longest route (to the
  first camp menu, 22,000 game frames) the main heap reaches about 1.5 GiB, the second zone under 0.1 GiB, peak resident memory
  0.9 GB on Linux and 0.76 GB on the Mac mini with the GPU renderer.
- iOS limits these separately: the address space an app may reserve (`com.apple.developer.kernel.extended-virtual-addressing`) and
  the memory it may use before the system ends it (`com.apple.developer.kernel.increased-memory-limit`). The owner's paid developer
  team can have both; the app and the probe (`tools/runtime/ios/probe/probe.entitlements.example`) ask for both. The probe on the
  signed phone is the first iPhone step: reserve the real layout, touch what the routes touch, and read `os_proc_available_memory()`.
- If a layout without extended addressing is ever needed: the second zone is almost empty, so an iOS layout of 2-4 GiB (and a
  smaller guard) is possible; the object-id windows of the main heap must keep their place (docs: implementation notes).

## Measured on the iPhone Duo (A20 Pro, 12 GB, iOS 27.2), 2026-11-17: the probe app, signed with the owner's team, installed over the network
| | no entitlements | increased-memory-limit + extended-virtual-addressing |
|---|---|---|
| memory the system allows the process at start | 3,368 MiB | **6,136 MiB** |
| 12 GiB contiguous reservation (the port's layout) | FAILED (largest 6,144 MiB, 7 GiB in all) | **OK** (64 GiB and more reservable in pieces) |
| 1,536 MiB committed and written in 64 MiB steps | done (1,825 MiB left) | done (4,594 MiB left) |
Both builds: 16 KiB pages (mprotect at a 4 KiB offset fails, as expected), 59 threads with large stacks alive together, fork refused,
mkstemp / tmpfile in the app's TMPDIR work, /tmp not writable, and a 16.667 ms sleep takes about 20 ms on average (frame pacing on
the phone must follow the display link, not sleeps). Physical memory reported: 11,570 MiB.
Conclusion: with the owner's paid team and both entitlements the port's address layout fits as it is, and its measured peak use
(about 1.5 GiB of guest heap, 0.9 GB resident) leaves several GB of headroom. Reports: `run-J26/ios/probe/probe-{plain,ent}.txt`
in the main work tree. How it was done: the probe project built with `CODE_SIGNING_ALLOWED=YES`, automatic signing for the owner's team
(id only in the Mac's `~/Developer/KHNMC/state/device.env`) and `-allowProvisioningUpdates` in a windowless `launchctl submit` job on the MacBook (the signing keychain is in the
GUI session), then `xcrun devicectl device install app / process launch / copy from` over the network to the paired phone.

## The game app on the iPhone itself, 2026-11-18
The KHNMC app (lifted game, runtime, the mod), signed with the owner's team and both memory entitlements, installed over the
network, runs its whole selftest on the iPhone Duo: **169,479 checks, 0 failures**, in about 16 s, three runs in a row. The 381
checks that need a child process (`fork`, refused on iOS) are reported NOT RUN, as designed. The first attempt, which started while
the phone was still unlocking, failed one timing check of the thread tests (a stopwatch started after the thread it timed); the
check now starts the stopwatch before the thread and prints what it measured if it ever fails again.
How: `tools/runtime/ios/mac/sign_app.sh ent` in a windowless `launchctl submit` job (signing needs the GUI session's keychain), then
`tools/runtime/ios/mac/device_run.sh selftest` over ssh (install, khnmc.conf + kh2fm.exe into the app's Documents, launch with
retries while the phone is locked, poll the status file, fetch the log). Team and device ids live only in
`~/Developer/KHNMC/state/device.env` on the Mac. The phone must stay unlocked with the app in front while it runs.
Next on the phone: the null-backend field route (needs the route data subset in the app's Documents), then the graphics stack
(vkd3d + MoltenVK built for iOS) for a real picture.

## The game data on the iPhone, 2026-11-18
`tools/runtime/ios/push_data.sh run` copies profile A (the data tree minus prune-A.list: 60,439 files, 41.4 GB) and pkgdir/ (0.97 GB)
into the app's Documents/data and Documents/pkgdir: 21 chunks of 2 GB, rsync to the Mac (about 75 MB/s), then `devicectl device copy
to` (about 45 MiB/s over the network tunnel; plugging the cable in did not change the transport), the next chunk staged while the
current one goes to the phone, so the Mac never holds more than two chunks. `push_data.sh verify` lists the phone's folders and
compares every name and byte size with the manifest: 60,441 of 60,441. The app marks data/, pkgdir/ and kh2fm.exe as excluded
from iCloud backup at every start (written to Documents/khnmc-backup.txt) and keeps the screen awake while it runs.
**Never use `devicectl device copy to --remove-existing-content`:** aimed at one sub-folder, it emptied the whole app container
(the first full copy was lost that way and sent again). devicectl has no delete; stray files are left or removed by the app.

## The game plays on the iPhone, 2026-11-18 (null graphics backend, full data, scripted controller)
`tools/runtime/ios/mac/device_run.sh route INPUT FRAMES DIFFICULTY` on the iPhone Duo with the profile-A data:
- **Field route** (input/route-field-battle.txt, difficulty 0, 7,000 game frames): all 14 steps (boot, title, new game, difficulty,
  the first field, a battle, 8 hits) in 141 s including the app's start: the game's own 60 fps pace.
- **Story route** (input/route-story.txt, difficulty 3, 9,300 game frames): all 58 steps, done at game frame 8,980 (Linux 9,193):
  both opening maps with their missions and objective texts read from the game's messages, on into Twilight Town (02:07) and the Accessory Shop's Talk command. Every scripted walk took the same number of
  game frames as on Linux (e.g. +267, +36, +289, +123); the two fights differ by a few hundred frames, as fights do.
- Memory: the bias layout with the full 12 GiB arena and the low guard reserved as on the Mac; the guest heap's high-water about
  1.1 GB, about 255 MB of it resident at the end of the field route.
- Sound goes to the null device for now (no output yet); the picture is not presented yet (next: docs on the iPhone graphics path).
- Test runs get fresh game folders under the app's tmp/ (KH_ROOT, KH_DOCS in khnmc.conf): the game's own config from an earlier run
  (Documents/Saves/.../config1525.dat) makes it boot past the first-time screens, which a route that waits for them never sees
  (the first story run on the phone ended so: 0 of 58 steps). The runtime now creates the game's Documents folder itself.

## Sound on the iPhone, 2026-11-18
runtime/ios/audio_coreaudio.c (written on Linux for task 28, never compiled until now) is built into the iPhone library (COREAUDIO=1 in
tools/runtime/ios/mac/build.sh) and compiled cleanly the first time; the app links AVFoundation and AudioToolbox, sets KHNMC_AUDIO=pull
(khnmc.conf can still say `env KHNMC_AUDIO=null`), takes the SoloAmbient session (follows the silent switch), and starts the RemoteIO
unit through a hook the runtime calls once the mixer's pull ring exists (`kh_audio_pull_ready`, runtime/khnmc_audio_out.h).
Story route with sound on the iPhone: `RemoteIO started, 48000 Hz float32 stereo, largest slice 4096 frames, ring target 8192 of
16384`; 9,121,920 frames delivered (190 s), 1 underrun of 960 frames, 0 overruns; the route 58 of 58 as without sound.
Still open for sound: interruptions (a call, Siri), route changes (headphones) and background/foreground stop and start
(docs/IOS-SHELL.md, "Suspend and resume").

## App icon on the phone, and a sound incident (2026-11-18)
The owner's icon is in the app: tools/runtime/ios/Assets.xcassets (a single-size AppIcon set; Contents.json committed, the PNG never:
tools/runtime/ios/sync.sh copies `~/Documents/khnmc/app-icon/AppIcon-1024.png` into the Mac's copy, a named error if it is missing),
ASSETCATALOG_COMPILER_APPICON_NAME=AppIcon; the signed app carries Assets.car and CFBundleIconName AppIcon.
**Incident:** the first selftest after sound was wired ran with the RemoteIO unit started: the audio-out test pushes a counter signal
(values far above full scale) through the same pull ring and rebuilds the ring, and that reached the speaker as loud noise (the
owner: "iphone just made a scary bad sound"); the test also failed (2 checks) because the device took its frames. Fixed in four
places: a selftest run never starts an audio device (`kh_run_is_selftest`, runtime/win64/loader.c, null_audio.c); the ring refuses to
be rebuilt while a device is attached (`kh_audio_out_attach_device`, named fatal `audio-ring-rebuilt-with-device`, a selftest case
in a child that fails without the guard: checked by removing it); the CoreAudio render callback clamps every sample to [-1, 1] and
NaN to 0 before the speaker; device_run.sh writes `env KHNMC_AUDIO=null` for every test run unless SOUND=1. After the fix the
phone's selftest is silent and passes (169,479 checks... the audio-out suite's new fork case is NOT RUN on iOS, as designed).

## The controller (2026-11-18)
The app reads a game controller through the GameController framework (runtime/win64/ios/ios_main.m) and hands the runtime the whole
state on every change (`kh_input_host_pad_set`, runtime/win64/null_input.c): XInput pad 0 with every button by position (A bottom /
cross, B right, X left, Y top, LB/RB, menu = START, options = BACK, stick clicks, d-pad), both triggers and BOTH sticks (a script never
drove the right stick; the camera needs it). `KHNMC_INPUT=host` (the app's default) turns it on when no input script is loaded and
never in a selftest; a disconnect releases the pad. Selftest: 6 new checks (every field incl. the right stick, packet numbers, pad 0
only, off again), shown to fail with the right stick dropped; on the phone 122 main checks, 0 failures.
`device_run.sh play` starts the game as the owner plays it (controller, sound, no script, no time limit); after every test run the
play configuration is put back, so the app's icon always starts the game.
Controller check on the iPhone with the owner's PS5 DualSense (2026-11-18, `device_run.sh play`): all 14 buttons arrive as the right
XInput bits in the order pressed (Cross A 0x1000, Circle B 0x2000, Square X 0x4000, Triangle Y 0x8000, L1 0x100, R1 0x200, Options START
0x10, Create BACK 0x20, L3 0x40, R3 0x80, d-pad 0x1/0x2/0x4/0x8); the right stick showed full up (+32767) during the R3 press. Triggers and
the left stick go through the same code but the log showed only button changes; the runtime now also logs each trigger's and stick
direction's first full deflection once.

## The presenter on Apple's GPU (Mac mini, M4 Pro, MoltenVK 1.4.2; 2026-11-18, task 36 merged)
- Plan step 1 on the Mac: the merged tree built with MoltenVK loaded (dylib) and linked statically (GFX_VULKAN_STATIC=1, the way the
  iPhone app links it): the GPU selftest 170,496 checks, 0 failures, both builds; `presentation extensions: surface=1 headless=1
  metal=1 swapchain=1 display_timing=1 metal_objects=1 layer_settings=1`.
- Plan step 9 (headless): graphics-only selftest with the presenter at 2670x1878 (the iPhone Duo's inner screen): vkd3d 184, gfx present
  281, wrap device 125 checks, 0 failures; the presented picture within 1 level of the D3D12 readback and of the CPU pattern. The title
  route (2,700 game frames) with the presenter on: all 2,699 presents, swapchain made once, never rebuilt; cost per present mean
  0.146 ms, p99 0.26 ms (lavapipe: 5.7 ms); acquire mean 0.012 ms; the check mode against the D3D12 readback: 8 ok, 0 failed, largest
  difference 1.00 level, largest mean 0.018.
- Seen on the way: the game creates about two command allocators and lists per frame (CreateCommandAllocator 5,540 by frame 2,700);
  worth a look for the phone's CPU time (a pool in the wrapper), not a correctness question.


## ORDERED_MEM on Apple silicon (Mac mini, M4 Pro; 2026-11-18, task 39 merged)

`ORDERED_MEM=1` makes naturally aligned guest loads acquire loads and stores release stores (runtime/win64/implementation notes). Measured on
the Mac mini from `~/work/khnmc-build` (`run-Y39-mac/`, Mac build dir `~/khnmc-work/build-y39`, both builds `GFX_VKD3D=1`):

- **The plain build really reorders on Apple silicon.** The membench litmus test (one thread writes data then a flag, another reads the
  flag then the data; 4,000,000 rounds): plain accesses saw the flag ahead of the data 1,736,980 times (the plain build's own LD/ST
  175,820 times); acquire/release 0 times, and the ordered build's own LD/ST 0 times. On x86 all three are 0. Guest code written for
  x86 may rely on that order (flags and queues between the game's threads), so the plain build carries a real, if unobserved, risk on
  the phone.
- **Code:** the plain binary has 15 LDAPR / 1,964 LDAR / 728 STLR (the runtime's own atomics); the ordered one 297,211 LDAPR /
  1,964 LDAR / 307,346 STLR; 117 of 117 lifted shards, the re-lifted mod and 66 runtime units built with it (the report line).
- **Selftests:** plain 170,153 / 0, ordered 170,158 / 0; with graphics on the GPU (vkd3d + MoltenVK) plain 170,773 / 0, ordered
  170,778 / 0.
- **Cost, microbenchmark** (ns per step, best of 5): load64 0.910 -> 0.959 (+5.4 %), load32 +4.3 %, store64 +2.8 %, load+store
  0.949 -> 1.070 (+12.7 %).
- **Cost, the game** (null backend, frames clock, unpaced, default route to 7,000 frames, alternated plain/ordered/ordered/plain;
  identical route results): wall 27 s -> 28 s, CPU 31 s -> 32 s (about +3 %), RSS 512 -> 517 MB.
- **GPU story route** (host clock, MoltenVK, 12,000 frames, one run each; both 58 of 58): plain wall 281 s / CPU 127 s, ordered
  275 s / 125 s: no visible cost. (The second pair was refused by the Mac mini's 30 % memory floor: its owner's work was using the
  memory. Not rerun.)

Recommendation for the owner: build the Apple targets (Mac and iPhone) with `ORDERED_MEM=1` by default. The default stays 0 until the
owner decides and the phone's own numbers exist (implementation notes section 6, "The phone"; needs the MacBook on power).

## The graphics stack on the iPhone (2026-11-18, MAC-STEPS-38 steps 1-4)

- **Build** (`KHNMC_IOS_MACBOOK_OK=1 GFX=1 tools/ios/build.sh device`): libkhnmc-rt.a with the graphics backend, OTHER_LDFLAGS
  expanded to 26 words (4 vkd3d archives, libMoltenVK.a, libc++, Metal / QuartzCore / IOSurface / CoreGraphics), xcodebuild ok;
  check_hook_link 118 hooks, check_mod_static_link 35 functions, check_ios_symbols: 578 imports, none forbidden (mktemp allowed for
  MoltenVK); app 37 MB; one home-like string (the synthetic `/Users/khnmc/Documents`).
- **Signed** (GFX=1, both memory entitlements, "the signed app has the graphics backend").
- **Plain selftest on the phone** (graphics app, null backend): exit 0, 173,770 checks, 0 failures (406 fatal-path checks not run:
  no child processes on iOS).
- **The A20's GPU** (`GFX=1 device_run.sh selftest`, vkd3d 2.1 + MoltenVK 1.4.2): exit 0, 174,391 checks, 0 failures: vkd3d
  223 / 0, the presenter 304 / 0 (222 presents on MoltenVK's headless surface, each compared with the D3D12 readback and a CPU-made
  picture), the wrapped device 125 / 0. 0 vkd3d err lines, 0 MoltenVK errors (fixme 5, warn 25: the usual feature-level notes:
  no geometry shader, stream output, depth clip, cull distance, pipeline statistics).
- **Device report:** 'Apple A20 Pro GPU', Vulkan 1.1.357 through MoltenVK. **textureCompressionBC=1: BC1, BC2 and BC3 UNORM are
  sampleable in optimal tiling, so the game's BC textures are used as they are** (no CPU decode, plan C1 not needed; ASTC and ETC2 are
  offered too). B8G8R8A8 colour attachment + blend + blit, D32_FLOAT_S8X24 depth-stencil, MSAA 1/2/4/8. One heap, 11,570 MiB
  DEVICE_LOCAL, with a HOST_VISIBLE + COHERENT + CACHED type (unified memory) and a LAZILY_ALLOCATED one; VK_EXT_memory_budget budget
  8,192 MiB. Presentation: surface, headless, metal, swapchain, display_timing, metal_objects, layer_settings all enabled.
  Memory: os_proc_available_memory 6,036 MiB at start, phys_footprint 107 MiB; the gfx-memory-low soft stop is armed (512 MiB margin).
- **M1: the title sequence rendered by the A20** (offscreen, `GFX=1 DUMP_EVERY=40 FETCH="240 920 2000 2600" HASHLOG=1 CLOCK=frames
  device_run.sh route input/title-i21-hold1.txt 2700 none`): exit 0, 2,702 presents in 66 s with 67 picture dumps, 0 vkd3d err
  lines; memory peak phys_footprint 1,159 MiB with 4,985 MiB still available (gpuMemoryAllocated 809 MiB); the soft stop never fired.
  **Against the same run on the Mac mini's M4 Pro (MoltenVK, same build sources): the four fetched frames (GAME SETTINGS, the title
  intro, the NMC title menu twice) are 100.000 % identical pixels, and 2,693 of the 2,702 presents have the same picture hash at the
  same game frame.** The 9 others are pacing, not drawing: around the two loads the phone shows each Mac picture one present earlier
  (every Mac picture appears on the phone). 108 MoltenVK `[mvk-error]` lines, all the same: iOS refuses MTLCounterSampleBuffers for
  vkd3d's timestamp query pools and MoltenVK falls back to emulated timestamps (benign; worth silencing later). Pictures (game-derived,
  not committed): ~/Documents/khnmc/phone-frames/m1-20261118/.
- **ORDERED_MEM on the phone** (A20 Pro; the graphics app, plain then `ORDERED_MEM=1`, then the plain app re-signed; implementation notes
  section 6): the ordered app is 48 MB (plain 37 MB), all link checks pass, 117 of 117 shards built with LDAPR (rcpc) / STLR.
  - Litmus, 4,000,000 rounds: plain accesses saw the flag ahead of the data 720,043 times, the plain build's own LD/ST 670,698
    times; acquire/release 0, the ordered build's own LD/ST 0. **The phone reorders guest memory accesses that x86 keeps in order.**
  - Microbenchmark (ns per step): load64 0.837 -> 0.874 (+4.3 %), load32 +4.2 %, store64 +4.2 %, load+store 0.866 -> 0.969 (+12.0 %).
  - Story route (null backend, host clock, 12,000 frames): both 58 of 58, exit 0; pace 49.3 (plain) and 49.4 (ordered) game frames
    per second, i.e. the pacing, not the CPU, sets the speed.
  - **Found on the way: the null-backend app runs at about 49 fps, not 60** (`vsync: source=sleep`, 600 game frames in 12.17 s).
    That is the sleep-based WaitForVBlank on iOS (plan fact 9: a 16.667 ms sleep overshoots); the display link of IOS-GRAPHICS-PLAN
    step 10 (task 41) replaces it.
  - Recommendation unchanged: `ORDERED_MEM=1` by default for the Apple targets. Owner decision pending.
- **Owner decision 2026-11-18: `ORDERED_MEM=1` is the default for the Apple targets.** Makefile: `ORDERED_MEM ?= 1` when IOS_SDK is
  set or the host is Darwin, 0 elsewhere (Linux unchanged); tools/ios/mac/build.sh and sign_app.sh default to 1; tools/darwin_route.sh
  starts ./khnmc-om by default; tools/ios/common.sh passes an explicit ORDERED_MEM from Linux to the Mac. The names follow the variant
  (ordered = obj-om, obj-iphoneos-om, khnmc-om), so plain and ordered objects never mix; `ORDERED_MEM=0` builds the plain variant.
  Checked: Mac mini default `make` = ORDERED_MEM 1 / khnmc-om, its selftest 170,158 / 0 with "acquire/release ... 117 lifted shards";
  MacBook default build = obj-iphoneos-om, 48 MB, signed with ORDERED_MEM=1. The phone's selftest of the default app waits for the
  phone (it was unavailable to the MacBook at 09:35).
- **The story route on the A20's GPU** (2026-11-18, installed and run from the Mac mini over Wi-Fi; the ORDERED_MEM=1 graphics app,
  offscreen, host clock, `GFX=1 DUMP_EVERY=1500 FETCH="3000 6000 9000 10500" HASHLOG=1 device_run.sh route input/route-story.txt 12000
  3 2400`): exit 0, **58 of 58 steps**, 11,999 presents in 267 s with readbacks; memory peak phys_footprint 1,690 MiB with 4,454 MiB
  still available (gpuMemoryAllocated 943 MiB); 0 vkd3d err lines. Warnings: 15,951 x "ResourceBarrier: Aliasing barriers not implemented
  yet" (vkd3d ignores D3D12 aliasing barriers; nothing visible in the frames), the feature-level notes, 108 x the emulated-timestamp
  MoltenVK message. The fetched frames (a battle with the style rank, the Twilight Town gang at the accessory shop with a talk box, the
  mission pause screen) look right. Pictures: ~/Documents/khnmc/phone-frames/story-gpu-20261118/ (not committed).
- **First picture on the phone's screen** (2026-11-18 12:36 ET, a PREVIEW build of task 41 before its fix pass, ORDERED_MEM=1, installed
  from the Mac mini): `GFX=1 PRESENT=1 device_run.sh route input/title-i21-hold1.txt 2700 none 1500` exit 0; then `GFX=1 PRESENT=1
  play` left running for the owner. Display-link pacing: 60.00 waits per second, 0 late, 0 missed ticks; the game at 60.00 frames per
  second after the opening loads (52.4 and 58.2 during them); present cost 0.11-0.16 ms; one swapchain, never rebuilt; surface caps:
  usage 0x1f (TRANSFER_DST and TRANSFER_SRC yes), FIFO only, 2-3 images, B8G8R8A8_UNORM; 0 vkd3d err lines.
- **The drawable is 2853x2007, not the panel's 2670x1878, and that is right:** UIKit's nativeScale on the inner screen is 3.0 (MoltenVK:
  "contents scale 3.0"), i.e. iOS composites a 3x frame buffer (951x669 points x 3) and the display hardware scales it to the panel
  (the iPhone 12 mini scheme; no UIKit API gives the panel size there). Rendering at 2670x1878 would make iOS upscale it to 2853x2007
  and the hardware downscale it again: about 12 % less GPU work for a softer picture. A possible battery option (plan step 13), not a fix.
- **Frame counter** (owner 2026-11-18, after playing: "need a frame counter"): `KHNMC_HUD=1` in khnmc.conf turns on Apple's Metal
  performance HUD on the game's layer after the first present (CAMetalLayer.developerHUDProperties, iOS 16+: frame rate, frame time,
  GPU time, memory). device_run.sh writes it for every run with the picture (`HUD=0` turns it off) and has `NOLAUNCH=1` for play mode
  (the configuration goes to the phone, the game is not started; the icon starts it). Installed 16:37 with the selftest passing.
- **Owner's first play session (2026-11-18 afternoon), his reports and their tasks:** the HUD in the 16:9 band (task 42); black bars in
  the main menu, the pause screen and letterboxed scenes (task 46); pre-rendered movies hang on black until the pause menu's Skip
  (task 44); Xbox prompts with a DualSense (task 45: the game's own HID driver knows only the DualShock 4: 054C:05C4 / 09CC / 0BA0);
  about 22 fps in Twilight Town's Station Plaza and Tram Common when characters move (task 48, from his save); portrait with live
  rotation wanted (task 47). Elsewhere a steady 59.5-60 fps. Owner rule: the game never runs on the Mac mini.
