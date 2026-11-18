# iOS shell plan for KHNMC: DECOMP EDITION (from the owner's TXRA iPhone port, runtime/ios + tools/ios)

Target device (owner): iPhone Duo, A20 Pro, 12 GB RAM, iOS 27.2; paid Apple Developer account; 400 GB free; Mac builds on the MacBook Air
(Xcode 27.2, windowless jobs only, shared lock, never touch the Duo outside an owner-gated build). Personal IPA, never published.

## Reusable from TXRA (proven)
- **Generated Xcode project**: `make_project.py` writes `project.pbxproj` with the standard library only (`--check` verifies the committed copy);
  one app target, no schemes. Frameworks: Foundation, GameController, Metal, OpenGLES, QuartzCore, UIKit. Reuse as `tools/ios/make_project.py`.
- **One-shot Mac build** `build.sh`: fetch+verify SDL (pinned SHA-256), build the static lib via SDL's own Xcode project (arm64 only, device + simulator),
  generate the game C from the owner's own data, `xcodebuild -target`, strip, scan the binary for embedded game data / build-machine paths,
  package the IPA (`zip -X`, fixed dates). Same flow here, with the lifted C instead of `generated.c`.
- **Scene lifecycle**: apps built with the iOS 27 SDK need a `UIApplicationSceneManifest`; SDL 2.30 predates it, so `ios_platform.m` supplies the scene
  delegate and moves SDL's window into the scene, and handles files opened with the app. Copy this file as is. It stops nothing when the
  app leaves the screen: KHNMC needs the suspend and resume steps below in addition.
- **Files-app data**: no game data in the app; the player puts it in `On My iPhone > <App>` (Info.plist file-sharing keys). KHNMC needs the baked
  loose data tree (about 42 GB incl. HD textures) there: Finder/Apple Devices file sharing or `devicectl`/AFC (the owner already uses AFC transfers).
  A data-presence screen like TXRA's ROM instructions screen, with a manifest/hash check of the tree.
- **Floating point**: `-ffp-contract=off` so arm64 does not fuse multiply-adds (keeps arm64 == x86-64 frames). Mandatory for the lifted C as well
  (see docs/lift policy once A1 reports).
- **Simulator workflow** with `simctl` (create/boot/install/launch with `--headless --frames N --input-script --frame-hashes` and golden hashes) and
  plain-C touch layout/hit-testing tested on Linux. Reuse the headless-replay idea for the KHNMC boot tests.
- Saves in `Documents/Saves` visible in Files; settings in Application Support; pause-menu screenshots.

## Different for KHNMC
- Graphics is NOT an SDL renderer: the lifted GX code drives a D3D12-on-Metal shim (docs/graphics, runtime/metal-shim). SDL is still fine for the
  window/Metal view (SDL_Metal_CreateView), controller events and audio output; Metal shader compilation (47 shaders via dxbc-spirv + SPIRV-Cross or hand MSL)
  needs Xcode's Metal toolchain (the TXRA notes say it is an optional download: install it on the MacBook Air).
- Memory: 12 GB device but a jetsam limit applies. With the paid account use `com.apple.developer.kernel.increased-memory-limit` (and
  extended-virtual-addressing if a fixed 0x140000000 guest base is used: unverified on iOS); measure with `os_proc_available_memory()` on the Duo.
  TXRA had no entitlements; this app will (so it must be signed with the owner's paid team: the signature carries his legal name, never publish).
- Streaming from a 42 GB tree: memory-map files, no preloading; thermal/frame pacing matter more (3D, 60 fps target unknown).
- Input: GameController (MFi) first, minimal touch overlay; haptics optional.
- Bundle id: neutral pseudonymous reverse-DNS (e.g. `beer.vyvanse.khnmc`), per the owner's identity rules.

## Suspend and resume (a shell step that is still to do; review 2026-11-17)
The guest runs on its own threads: the game thread presents, and the loaders and the mixer run beside it. Stopping a display link
does not stop any of them. `runtime/ios/ios_main.m` handles no lifecycle event today: it is a test shell that ends with its run. Before
the shell shows the game on the Duo, it needs the steps below. None of them is written or run yet.
- **One gate at the frame boundary.** The shell sets a flag. The present path checks it first, before it touches anything:
  `kh_vkd3d_present` in `runtime/gfx_vkd3d.c`, and the queue's `ExecuteCommandLists` for work submitted between presents. While the
  flag is set, the submitting thread waits there on a condition and holds no runtime lock. This is a stall of the port at a frame
  boundary, not a game pause: it does not open the game's pause menu. Open point: whether the PC game opens its pause menu when its
  window loses focus has not been checked. If it does, the shell must send the game the same window message, so that play matches the
  PC.
- **Leaving the screen** (`sceneWillResignActive:`: Control Center, a call banner; `sceneDidEnterBackground:`: the lock button, the
  home gesture). The steps, in this order:
  1. Set the gate.
  2. Wait, for a bounded time, until the game thread is parked at the gate and the presenting queue is idle (a fence on that queue,
     `vkQueueWaitIdle` through MoltenVK).
  3. Only then return from the delegate call. Metal rejects command buffers committed by an app in the background, and the system may
     end the app for it.
  4. Call `kh_coreaudio_stop()`. The mixer falls back to its own clock and discards (`runtime/ios/audio_coreaudio.c`).
  5. Set every pad to neutral, as `XInputGetState` reports it (`runtime/null_input.c`). A button that was held at the lock must not stay
     latched: its release event may never arrive.

  If the bound in step 2 expires (for example, during a long load), the shell logs it, counts it and returns anyway. The gate stays
  set, so that thread stops at its next submission.
- **Coming back** (`sceneDidBecomeActive:`), in this order:
  1. Take the layer's current size. If the swap chain reports that it is out of date, rebuild it before the first present.
  2. Call `kh_coreaudio_start()` and check its result. A failure is loud, and the mixer keeps its fallback clock. An audio session
     interruption ends with its own notification: restart there as well (see the header of `audio_coreaudio.c`).
  3. Read the current state of every connected `GCController`, not its last event. A controller that disconnected, or whose button came
     up during the suspension, sent nothing.
  4. Clear the gate.
- **Time.** Host time runs on during a suspension. The runtime's stuck budgets already survive a suspended process: they are cut into
  slices (`runtime/kh_compat.h`, "an app suspended by iOS"). A gate held while the process keeps running is a different case: for
  example, Control Center stays open for a minute while the other guest threads run on. No named stuck budget may count time parked at
  the gate (the load-list lock, the two waits for the audio pass). The guest's clocks are NOT rebased: after resume,
  `QueryPerformanceCounter` jumps forward, as on a PC after the machine sleeps, and what the game does with that long frame is its own
  code. Only deadlines the port itself adds (any present pacing) restart from the moment of resume.
- **Acceptance.**
  - On Linux, before the phone: a test-only trigger holds the gate during the default route for longer than the longest stuck budget,
    then clears it. The route must end ok, with no stuck fatal and the usual step-frame drift.
  - On the Duo, during the default route: lock and unlock, pull down Control Center, take a call, and disconnect the controller with
    a button held, then unlock. There must be no Metal error in the log and no crash, the sound must return, and no button may stay
    held.

## Build/sign flow (owner gated)
1. Lifted C and runtime build on build-host/Mac mini for development; the MacBook Air only does the final Xcode build, Metal compile, sign and install.
2. Signing with the paid team, install via Xcode/devicectl to the Duo; no TestFlight/App Store.
3. All Mac jobs windowless (launchd), holding `~/Developer/.mac-build.lock`, never opening Terminal windows, muted simulators, own folder `~/Developer/KHNMC`.
