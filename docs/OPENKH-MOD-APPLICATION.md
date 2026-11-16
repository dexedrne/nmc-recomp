# How OpenKH applies a mod to the PC game (read from OpenKH source, Apache-2.0, master 41ac7b5, 2026-11-15)

Sources read: `OpenKh.Patcher/PatcherProcessor.cs`, `Metadata.cs`; `OpenKh.Tools.ModsManager` (ModsService, MainViewModel.PatchGame);
`OpenKh.Research.Panacea` (`Panacea.cpp`, `OpenKH.cpp`, `KingdomApi.h/.cpp`). Licence: Apache-2.0 (reuse allowed with notice).

## 1. Build step (Mods Manager "Build and Patch" -> OpenKh.Patcher)
- A mod is a folder with `mod.yml` (title, game, specifications, dependencies, `assets:` list). Each asset has a `name` (the in-game
  relative path, e.g. `obj/P_EX100.mset`), a `method`, `source[]` (file in the mod, or `type: internal` = a file from the user's own
  game extraction), optional `platform` (pc/ps2/both), `package`, `multi`, `required`, `game`.
- Methods: `copy` (verbatim), `bar`/`binarc` (patch sub-entries of a BAR), `bbsarc`, `imd`/`imgd`, `imz`/`imgz`, `fac`, `kh2msg`,
  `areadatascript` (ARD scripts), `kh1ardresource`, `bdscript`, `spawnpoint`, `listpatch` (typed table patches: trsr, item, fmlv, lvup,
  went, sstm, prty, bons, atkp, lvpm, objentry, plrp, cmd, slct, localset, jigsaw, enmp, shop, sklt, przt, magc, btlv, vtbl, limt...),
  `synthpatch`. Unknown method falls back to `copy`.
- Non-copy methods first copy the ORIGINAL file from the user's extraction (`data/kh2/...`) to the output, then patch it in place.
  Assets are processed in parallel; multiple mods are applied in order and a later mod's file overwrites an earlier one.
- Output = the "compiled mod" folder (`mod/kh2/<name>`), loose files in the game's own relative-path namespace. Files named
  `remastered/...` or `raw/...` are PC-only (HD textures, raw replacements). `scripts/` and `dlls/` entries are non-game files
  (LuaBackend scripts, mod DLLs). A `patch-package-map.txt` (`name $$$$ package/original/name`) records which game package
  (`kh2_first`, ...) each file belongs to.

## 2. Two ways OpenKH puts the result into the PC game
A. **Runtime overlay (Panacea)**: no game file is modified. `dbghelp.dll` proxy (Panacea) loads into the game, finds engine functions
   by byte-pattern scan, and hooks the engine's file layer (below). The owner's setup uses this (`enablePatching: false`,
   `mod_path=...\openkh\mod`, game dir contains `dbghelp.dll` + `LuaBackend.dll` + `panacea_settings.txt`).
B. **Repack**: Mods Manager moves staged files into per-package folders and uses `OpenKh.Egs` to rewrite the game's `.pkg/.hed`
   archives (Epic/Steam package format) in place.

## 3. Panacea's runtime hooks (the engine's own file API; `Axa` namespace, same names as in the exe's RTTI)
- Hooked: `Axa::CFileMan::LoadFile`, `LoadFileWithSize`, `LoadFileWithMalloc`, `GetFileSize`, `GetRemasteredCount`, `GetRemasteredEntry`;
  `Axa::PackageFile::OpenFileImpl`, `GetRemasteredAsset`; `Axa::PackageMan::GetFileInfo`; `Axa::SetReplacePath`, `FreeAllPackages`;
  `VAG_STREAM::play/fadeOut/setVolume/exit` (custom music via BASS/vgmstream); a per-frame hook; `DebugPrint`.
  Also uses `Axa::CalcHash`, `OpenFile`, `DecryptFile`, `DecompressFile` (the exe's own package crypto/compression).
- Lookup (`TransformFilePath`): for a requested game path the loader first tests `<mod>\raw\<name>` (raw replacement) then
  `<mod>\<name>` (original-format replacement); if the file exists it is read directly and returned instead of the package entry;
  otherwise the original package path runs. `GetFileSize` mirrors this so size queries agree.
- HD assets: the `<mod>\remastered\<package path>` folder is scanned; entries (name, offset, origOffset, decompressed/compressed size)
  are merged into / replace the package's `RemasteredEntry` table by walking the file's image containers (IMD, IMZ, TM2, DPD, DPX, PAX,
  RAW, BAR offsets) so the right remastered texture is matched to the right sub-image. Cached (`enable_cache`).
- Other: `LoadDLLs(<mod>\dll)` loads mod DLLs (this is how KH2Tweaks/KH2Moose/NeverlandFly get in); LuaBackend is separate.
- Function discovery is a 16-byte-stepped scan of the exe image for byte patterns with wildcards (`Hook(pfn, pattern, mask)`), then
  RIP-relative globals (PackageFiles array, BasePath, PCSettings, VolumeLevels) are read from fixed offsets inside those functions.

## 3b. Consequences for KHNMC: DECOMP EDITION
1. The file layer is a SMALL, well-defined seam: `Axa::CFileMan` (4 load/size functions), `PackageMan/PackageFile` (open + remastered
   assets), `AxaResourceMan::SetResourceItem`, `DecryptFile`/`DecompressFile`. Our port owns this seam, so the NMC data can be applied
   natively with Panacea's exact overlay semantics (raw/ then loose-file/ then package) instead of any hooking.
2. Preferred data path: run OpenKH's Patcher (Apache-2.0, .NET 8; no .NET installed on build-host/build-host/Mac mini yet, user-local `dotnet-install` works)
   over the owner's own extracted data + `SillyBillyMod1/mod.yml` to bake a merged `mod/kh2` tree once, on the owner's machine; the port then
   loads loose files from that tree (no runtime patching, no OpenKH code in the app). Remastered HD files need the merged-entry logic above.
3. The 35 detour hooks / 89 static edits / script syscalls (see G1-GATE) are the code-side half of the mod and are NOT handled by the Patcher.
4. Panacea's byte-pattern table is a ready map of the file-layer functions in the exe (verify each against our function universe).
5. Do not copy OpenKH code verbatim without keeping its notice; Re:Fined (different project) must not be used at all.
