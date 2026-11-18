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

