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
  delegate and moves SDL's window into the scene, and handles files opened with the app. Copy this file as is.
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

## Build/sign flow (owner gated)
1. Lifted C and runtime build on build-host/Mac mini for development; the MacBook Air only does the final Xcode build, Metal compile, sign and install.
2. Signing with the paid team, install via Xcode/devicectl to the Duo; no TestFlight/App Store.
3. All Mac jobs windowless (launchd), holding `~/Developer/.mac-build.lock`, never opening Terminal windows, muted simulators, own folder `~/Developer/KHNMC`.
