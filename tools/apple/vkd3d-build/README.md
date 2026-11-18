# Building vkd3d 2.1 without autotools, and the D3D12 probe (Linux and arm64 macOS)

The Mac mini has no autotools, no bison 3 and no widl, so vkd3d is built from its release tarball with a plain script.
Nothing of vkd3d is stored in this repo: only our build script, config and probe.

Sources (downloaded and verified on the Linux host, see `~/work/khnmc-deps/apple/README.md`): `vkd3d-2.1.tar.xz` (Wine release key
signature), Vulkan headers, SPIRV-Headers, MoltenVK 1.4.2 (macOS and iOS release archives, GitHub digests).

On the Linux host, inside the extracted `vkd3d-2.1/`:
1. `cd libs/vkd3d-shader && flex -o preproc.yy.c preproc.l && bison -d -o preproc.tab.c preproc.y && flex -o hlsl.yy.c hlsl.l && bison -d -o hlsl.tab.c hlsl.y`
2. `perl -I<dir with JSON.pm from here> libs/vkd3d-shader/make_spirv include/private/spirv.core.grammar.json > include/private/spirv_grammar.h`
   (`JSON.pm` here is a stand-in that delegates to core Perl's JSON::PP; the real JSON module works too)
3. `printf '#define VKD3D_VCS_ID ""\n' > include/private/vkd3d_version.h`
4. copy `config.h` and `build_khnmc.sh` from here into `vkd3d-2.1/`. The IDL-derived headers are already in the tarball's `include/`.

Build (either system): `VKHEADERS=<dir with vulkan/> SPIRVHEADERS=<dir with spirv/> CC="clang -arch arm64" OUT=build-mac JOBS=1 sh build_khnmc.sh`
gives four static libraries (33 objects). On macOS `config.h` makes vkd3d `dlopen("libMoltenVK.dylib")`: run from a directory that
holds that name (a symlink is enough; `DYLD_LIBRARY_PATH` is stripped by SIP-protected launchers such as `nice`).

Probe: `vkprobe.c` (create a device, clear a render target, read a pixel back). With the STATIC libraries delete its
`#define INITGUID` line (the libraries already define the interface GUIDs), then
`clang -arch arm64 -O1 -w -DWIDL_C_INLINE_WRAPPERS -Ivkd3d-2.1/include -I<vulkan headers> vkprobe.c -o vkprobe libvkd3d-utils.a libvkd3d.a libvkd3d-shader.a libvkd3d-common.a -lm -lpthread`.

## Result 2026-11-17 (Mac mini, M4 Pro, macOS 26.6, over ssh, no window)
`OK device+render+readback: pixel BGRA = 191 128 64 255 (expect 191 128 64 255)` with MoltenVK 1.4.2 (Vulkan 1.4.357 on Metal).
vkd3d's warnings there: no geometry shaders, no pipeline statistics queries, no cull distance, no depth-clip-enable extension, no
stream output, no sampler min/max filtering, D24S8 mapped to D32F_S8. The game uses no geometry/tessellation/compute shaders; whether
any of its pipeline states turns depth clipping OFF is not checked yet (that would need the missing extension).
Left on the Mac mini: `~/khnmc-work/gfxprobe/` (59 MB: source, headers, MoltenVK dylib, static libs, probe, `probe.out`).

## iOS build (2026-11-18, MacBook Air, Xcode 27.2, iPhoneOS SDK 27.2; plan step 2 of docs/IOS-GRAPHICS-PLAN.md)
The prepared tree (the steps above, done on Linux) and the headers go to `~/Developer/KHNMC/deps/` on the MacBook Air (owner approved
11/18: vkd3d and MoltenVK there), with MoltenVK's iOS static library and headers from the release archive
(`MoltenVK/MoltenVK/static/MoltenVK.xcframework/ios-arm64/libMoltenVK.a`, `MoltenVK/MoltenVK/include`) in `deps/moltenvk-ios/`. Then:

    export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer     # an ssh session has none: xcrun finds no iphoneos SDK without it
    cd ~/Developer/KHNMC/deps/vkd3d-2.1
    VKHEADERS=$HOME/Developer/KHNMC/deps/vkheaders SPIRVHEADERS=$HOME/Developer/KHNMC/deps/spirv \
    CC="xcrun --sdk iphoneos clang -target arm64-apple-ios17.0" OUT=build-iphoneos JOBS=2 \
    EXTRA_CFLAGS="-ffile-prefix-map=$PWD/=vkd3d/" nice -n 10 sh build_khnmc.sh

Result: the four archives (33 objects: common 5, shader 19, vkd3d 7, utils 2), every object LC_BUILD_VERSION platform 2 (iOS),
minos 17.0; 555 imported names, none of the 23 that tools/check_ios_symbols.py forbids (`dlopen` is imported: vkd3d uses it only
when it is given no vkGetInstanceProcAddr, and the iOS build gives it one); `strings | grep -c /Users/` = 0. The whole deps folder is
65 MB. Use THIS directory's build_khnmc.sh (an older copy with `xargs -I` fails on macOS: "command line cannot be assembled").

