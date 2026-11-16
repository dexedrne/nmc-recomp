# 1. File layer: Axa file API resolved in kh2fm.exe, and the loose-tree replacement

Scope: Batch B2, item 1. Static analysis only (pefile + capstone), nothing executed. exe = `pc/kh2fm.exe`, sha256 `9002b2de6a1f91a7...`, image base `0x140000000`; all addresses below are RVAs (add `0x140000000` for VAs).
Sources read: OpenKH Panacea (`OpenKH.cpp` Hook table, `Panacea.cpp`, `KingdomApi.h/.cpp`, Apache-2.0), the universe table (`scratch/universe/functions.json`), the S7 name DB, the real mod tree (`pc/mod-kh2`, 4,862 files) and `SillyBillyMod1/mod.yml`.
Machine-readable: `file_layer_functions.json`, `walk_check.json`, `name_proposals.csv`; scripts `pat.py`, `t1_*.py`, `remaster_walk.py`; header sketch `include/khnmc_file_layer.h`.

## 1.1 Panacea byte patterns resolved against kh2fm.exe

`pat.py` re-implements Panacea's `FindAllFuncs()` exactly: a scan of the mapped image at 16-byte steps, wildcard mask, first hit wins and the hook is removed from the list. Result: **all 24 file/sound/settings patterns match**, 23 of them uniquely; every match is a function start in the universe table (38,455 functions) and 22 of 24 are `.pdata` roots.

| Panacea symbol | RVA | size | match | direct call sites | S7 name before this work |
|---|---|---|---|---|---|
| Axa::CFileMan::LoadFile | 0x106700 | 143 | unique | 2 (both sd: 0x3f9850, 0x3f99e0) | none |
| Axa::CFileMan::LoadFileWithSize | 0x106790 | 154 | unique | **0** (never called directly) | none |
| Axa::CFileMan::LoadFileWithMalloc | 0x106870 | 184 | unique | 19 in 14 functions | none |
| Axa::CFileMan::GetFileSize | 0x1063d0 | 105 | unique | 3 (0x3f95f0, 0x3f9850, 0x3f99e0) | none |
| Axa::CFileMan::GetRemasteredCount | 0x106440 | 25 (leaf, no pdata) | unique | 2 (ResourceEntry 0x12cd80) | none |
| Axa::CFileMan::GetRemasteredEntry | 0x106480 | 71 (leaf) | unique | 1 (0x12cd80) | none |
| Axa::PackageFile::GetRemasteredAsset | 0x107760 | 446 | unique, reached through thunk 0x106460 (loads `PackageFiles[LastOpenedPackage]`) | 1 (thunk, from 0x12cd80) | none |
| Axa::PackageFile::OpenFileImpl | 0x107240 | 984 | unique; it is vtable slot 0 of `PackageFile` (vtable 0x585fb0) | 0 (virtual) | `Axa::PackageFile::vfn0` (correct) |
| Axa::PackageMan::GetFileInfo | 0x106940 | 381 | unique | 5 (all inside CFileMan) | none |
| Axa::AxaResourceMan::SetResourceItem | 0x12f070 | 207 | unique | 5 | none |
| Axa::CalcHash | 0x145930 | 123 | unique | 18 | none |
| Axa::SetReplacePath | 0x102840 | 29 (leaf) | unique | 1 (axaAppMain 0x14f479) | none |
| Axa::FreeAllPackages | 0x1064d0 | 217 | unique | 1 (axaAppMain 0x14f4c6) | `Axa::PackageFile::ctor_or_dtor` (class-adjacency guess, wrong role) |
| Axa::OpenFile | 0x1070b0 | 126 | unique | 19 | `Axa.fileIoOpen` |
| Axa::DecryptFile | 0x1062b0 | 232 | unique | 4 | none |
| Axa::DecompressFile | 0x4696f0 | 194 | unique | 2 | none |
| Axa::AxaSoundStream::_threadProc | 0x13c090 | 434 | unique | 0 (thread entry; address taken at 0x13a9e4) | none |
| Axa::DebugPrint | 0x143760 | 16 (leaf, body = `ret`) | **2 hits (0x143760, 0x152410)**; Panacea binds the lower one. 0x143760 has 54 direct callers in sd/Axa-sound code, 0x152410 has 1 | 54 | none |
| VAG_STREAM::play / fadeOut / setVolume / exit | 0x537420 / 0x5373e0 / 0x5374a0 / 0x5373a0 | 112 / 24 / 43 / 52 | unique each | 6 / 12 / 2 / 10 (gummi, sd, harata-dk, tozawa-menu) | none |
| settings accessor (Panacea `settingsfunc`) | 0x14d070 | 87 | unique | 2 | none |
| sd volume setter (Panacea `volumefunc`) | 0x137d50 | 82 | unique | 3 | none |

Launcher-only patterns in the same table: `framefunc` (both variants) and `LaunchGameEpic` do not match this exe. `LaunchGameSteam` matches **0x143690, a false positive**: it is the function that writes the process exit code into a named file mapping (CreateFileMappingA 0x1436bd, MapViewOfFile, 4 bytes; name at .rdata 0x5ae810), called from the WinMain shim 0x143720 after the game main returns. The `axaAppMain` pattern `40 53 B8` hits 0x7c0e0 first (not the app main); the longer 1.5+2.5-launcher variant `48 89 5C 24 ?? 57 B8` hits **0x14f1e0**, the real `axaAppMain` (G1's app main), so the two executables share this entry function.

**Globals read through RIP-relative operands** (Panacea reads these at fixed offsets inside the matched functions; all six offsets re-checked: they decode to the addresses below).

| Panacea name | formula | address | section | engine meaning |
|---|---|---|---|---|
| PackageFileCount | GetFileInfo+0x1A | 0x79cfd8 | .data | int, number of registered packages (7 on Steam) |
| LastOpenedPackage | GetRemasteredCount+3 | 0x79cfd4 | .data | int index of the package that satisfied the last GetFileInfo |
| PackageFiles | GetFileInfo+0xB1 | 0x79cfe0 | .data | `PackageFile*[16]` (0x79cfe0..0x79d060) |
| BasePath | SetResourceItem+0x3E | 0x2bd4340 | `.shared` | `char[128]`, written by 0x1023c0 = `sprintf("%s%s", AppInterface+0x280, "KH2ReSource")`; initial bytes "dummy_string" |
| PCSettingsPtr | settingsfunc+0x2B | 0x715368 | .data | payload of the 0x1fc-byte settings block at 0x715350 (magic "SET-", version 8, size 0x1fc) |
| VolumeLevels | volumefunc+0x2D | 0x5ab960 | .rdata | `float[11]` = 0.0, 0.1 ... 1.0 |

Additional engine globals found while reading the functions (new, needed by the port):

| address | meaning | evidence |
|---|---|---|
| 0x79cfc0 | `CFileMan*` singleton; passed as `this` by every loader caller (never dereferenced by the loaders themselves) | ctor 0x107180 stores it; 14 caller functions load it |
| 0x79cfc8 | dword, 3 after the CFileMan ctor; ResourceEntry only queries remastered entries when it equals 3 | 0x107180, 0x12cf49 |
| 0x79cfcc | platform id: 0 STEAM, 1 PS4, 2 XB1, 3 GGP, 4 EPIC (switch in 0x102860 appends `STEAM/`, `PS4/`, `XB1/`, `GGP/`, `EPIC/`); **DecryptFile does nothing unless it is 0** | 0x1062db, 0x102420, 0x102690, 0x102860, 0x102a60 |
| 0x79d060/68/70 | begin/end/cap of a vector of 16-byte stream records `{int fd; int pkgIndex; int64 entryOffset}` | 0x106da0, 0x106b40, 0x106fa0 |
| 0x2bd4010 | ReplacePath (`<root>KH2Replace`), written by SetReplacePath; read by the tozawa name_table loader 0x12f140, 0x12ee70, 0x148db0, AxaBaseName code | 0x102840 |
| 0x79cf00 | `AppInterface*` global: +0x274 language, +0x280 root path, +0x480 second path, +0x1290 flag | many |

## 1.2 What the engine file API does (as lifted)

Package set: 7 `PackageFile` objects are registered by `axaAppMain` (0x14f1e0, via 0x1065c0) from two static tables in .data: `image/dt/kh2_first .. kh2_sixth` (flag byte +0x104 = 1) and `image/SettingMenu` (flag 0); each name is turned into `<name>.hed` (read whole into memory, `FileCount = size >> 5`) and `<name>.pkg` (kept open as a CRT fd). On the Steam install these are `Image/dt/kh2_{first..sixth}.hed/.pkg` and `Image/SettingMenu.hed/.pkg` (verified by listing the install on build-host: 12 `kh2_*` files and the 2 `SettingMenu` files).

`PackageFile` layout (size 0x260, vtable 0x585fb0): +8 `HedEntry*`, +0x10 pkg fd, +0x14 FileCount, +0x18 CurrentOffset, +0x20 PkgFileName[260], +0x124 byte "encrypted/compressed package" (set from the package table), +0x12c `PkgEntry CurrentFileData`, +0x140 `RemasteredEntry* RemasteredData`, +0x148 `PkgEntry FileDataCopy`, +0x158 `CurrentFileName[260]`. This matches Panacea's `KingdomApi.h` exactly.

| function | behaviour read from the disassembly |
|---|---|
| `GetFileInfo(path, alt)` 0x106940 | **spin-waits (`_Thrd_sleep`) until PackageFileCount > 0.** `alt == NULL`: LastOpenedPackage := 0 and walk `PackageFiles[i]->vfn0(path, NULL)`; first true wins, LastOpenedPackage = i, return that package; none: return NULL (LastOpenedPackage is left == count). `alt != NULL`: only packages whose PkgFileName contains `SettingMenu.pkg` are tried, with `(path, alt)`. |
| `PackageFile::vfn0 = OpenFileImpl(path, alt)` 0x107240 | waits until the .hed is loaded; **if `path` equals the package's CurrentFileName it returns true without touching anything** (so repeated lookups keep the same remastered table); otherwise `path` must contain BasePath (when `alt` is NULL); resets state, copies the name, hashes the part after `BasePath/` (or after `alt/`) with `CalcHash`, linear-searches the HED for the 16-byte hash, on a miss retries once with the file component lower-cased; on a hit seeks the .pkg to `entry.offset`, reads the 0x10-byte `PkgEntry` into +0x12c and +0x148, allocates `remasteredCount` x 0x30 `RemasteredEntry` and reads them, and fills each entry's `offset` cumulatively: first = `0x10 + 0x30*count + (compressedSize>0 ? compressedSize : round16(decompressedSize))`, next = previous + (previous.compressedSize<0 ? round16(previous.decompressedSize) : previous.compressedSize). |
| `PackageFile::ReadCurrentInto(dst)` 0x107620 | seek to `CurrentOffset + 0x10 + 0x30*remasteredCount`; if size > 0x10 and the package flag +0x124 is set: when `compressedSize > 0` read `compressedSize` bytes to a temp, `DecryptFile`, `DecompressFile` into dst; when `compressedSize <= 0` read `decompressedSize` bytes and, if `compressedSize == -1`, `DecryptFile` in place; otherwise a plain read. Returns the size. |
| `LoadFile(this,path,dst,useHd)` 0x106700 | `GetFileInfo(path,0)`; NULL -> **-1**; read; if the byte count != `CurrentFileData.decompressedSize` -> **-2**; size 0 -> 0; if `useHd` -> `SetResourceItem(path,size,dst)`, a result of -1 turns the return into 0; else the size. |
| `LoadFileWithSize(this,path,dst,cap,useHd)` 0x106790 | NULL -> 0; `cap < decompressedSize` -> **-6** before reading; mismatch -> -2; `useHd` (5th arg on the stack) as above. 0 direct callers. |
| `LoadFileWithMalloc(this,path,&size,useHd,alt)` 0x106870 | `GetFileInfo(path, alt)`; NULL -> NULL; saves the incoming `*size`, **writes `*size := decompressedSize`**, if the incoming value was **-1 -> returns NULL (size query)**; `_aligned_malloc(size,16)`, read; negative read -> free, `*size = -2`, NULL; `useHd` -> `SetResourceItem`, -1 -> NULL. The `alt` argument selects SettingMenu (see GetFileInfo). |
| `GetFileSize(this,path)` 0x1063d0 | `GetFileInfo`; found -> `decompressedSize`; else `FindFirstFileA(path)` -> `nFileSizeLow` of a real file; else 0. (So game paths that are not in a package fall back to the real file system.) |
| `SetResourceItem(path,size,buf)` 0x12f070 | strips BasePath (via `strstr`) into a local path and forwards to 0x12e5c0, which registers the buffer with the resource manager; returns -1 on failure. |
| `GetRemasteredCount/Entry/Asset` | operate on **`PackageFiles[LastOpenedPackage]`**, i.e. on global state set by the last GetFileInfo, not on an argument. Count = `CurrentFileData.remasteredCount`; Entry(i) = `{&RemasteredData[i]; *out = origOffset}` or NULL when `i >= count` (or `RemasteredData == NULL`); Asset(i,&size) = `_aligned_malloc((size+0xff)&~0xff, 16)`, zeroed, `lseek(pkg, entry.offset + CurrentOffset)`, read (decrypt/decompress per entry flags), returns the buffer, or frees and returns NULL if the byte count differs from the entry size. |
| `OpenStream` 0x106da0 (twin 0x106ba0) / `ReadStream` 0x106fa0 / `CloseStream` 0x106b40 / `SeekStream` 0x106830 | the streaming path used by sound streams: look the name up in the HED of each package, **re-open the .pkg with a private fd** (`Axa::OpenFile(pkgName, 0x8000)`), seek to the entry, read the 0x10-byte header, push `{fd, pkgIndex, entryOffset}` to the vector at 0x79d060 and return the fd; `ReadStream(fd, dst, off, len)` seeks to `off + entryOffset + 0x10` and reads; `CloseStream` erases the record and `_close`s. Callers: 0x13c090 (AxaSoundStream::_threadProc, call 0x13c0f7), 0x13be30 (CStream start-up, 0x13be8a), 0x13cae0 (read, 0x13cb29), close from CStream ctor/dtors 0x135c10, 0x135ed0, 0x1361a0. |
| `Axa::OpenFile(path, oflag)` 0x1070b0 | `sprintf` + `_sopen_dispatch`; returns the fd or -1. |
| `DecryptFile(pkg, data, size, &hdr)` 0x1062b0 | **platform gate: returns immediately unless the platform id (0x79cfcc) is 0.** Otherwise it applies a block transform with a 0x118-byte cipher object to the first min(size,0x100) bytes only, in 16-byte blocks; the per-file 16-byte parameter block is `FileDataCopy` (`PackageFile+0x148`, a copy of the 16-byte PkgEntry header). Data-layer fact only; no key material was extracted and none is needed by the loose tree. |
| `DecompressFile(out,&size,in,compSize)` 0x4696f0 | in-memory zlib: the embedded zlib is **1.2.5** (string at 0x75e3f0); `inflateInit`, `inflate(Z_FINISH)`, `inflateEnd`; returns 0 and writes `total_out` back through `*size` on Z_STREAM_END, -3 for Z_NEED_DICT or truncated input (Z_BUF_ERROR with empty input), otherwise the zlib error code. |
| `CalcHash(name,len,out16)` 0x145930 | writes a 16-byte hash of the name (three helper calls 0x1463b0/0x1463e0/0x1464d0); used only to index HED entries. |

Package-bypass consequences: the HED/PKG parsing, `CalcHash`, `DecryptFile`, `DecompressFile`, `AddPackage` (0x1065c0), `PackageFile::ctor` (0x106000), `FreeAllPackages` (0x1064d0) and `GetFileInfo`'s package walk are not needed when the data is a loose tree. What must survive is the externally visible behaviour (sizes, return codes, the "current file" state, the stream fd API).

## 1.3 Loose-tree replacement semantics

### 1.3.1 Path mapping
Callers build paths as `sprintf("%s/%s", <BasePath or a directory>, <relative name>)`. The engine strips `BasePath + 1` (Panacea: `originalPath + strlen(BasePath) + 1`) to get the **relative game path**, e.g. `obj/P_EX100.mdlx`, `00progress.bin`, `msg/us/sys.bar`, `bgm/music052.win32.scd`. Language segments seen in the loader code: `jp us uk gr fr sp it fm` (0x374640, 0x12f140), file extensions with a language infix `.a.us` etc.

Port rule (equals Panacea `TransformFilePath`, minus its dev/extract tiers, with raw first):
1. `rel = path with the BasePath prefix and one '/' removed` (the port may simply set BasePath to the empty string or a fixed token; every consumer of the BasePath buffer is in the file layer or in 0x537420/0x148810/0x148980/0x148a80, which build `BasePath/<name>` for sound streams and resources).
2. look up `<tree>/raw/<rel>` (PkgEntry-wrapped override; the baked tree has none: mod.yml has zero `raw/` assets and the 4,861 mod assets are all `copy`), then `<tree>/<rel>` (plain file). Match ASCII case-insensitively and with `/` separators. The package layer itself lower-cases only the file component on a retry, and the PC build was developed on a case-insensitive FS, so the tree needs a lower-cased index (`.khnmc/tree.idx`) on a case-sensitive volume.
3. found: size = file size, compression state = -2 (plain), `remasteredCount` = size of the entry table (1.3.3). not found: behave as the package layer does for an unknown name (return values in 1.2).

### 1.3.2 The four loaders and GetFileSize
Pseudo-code (all four share `current = open(rel)`):
```
LoadFile(path, dst, useHd):           n = size(rel); if !found return -1; read n into dst; n==0 -> 0;
                                       if useHd { build/lookup RemasteredTable for rel; if SetResourceItem(path,n,dst)==-1 return 0 } return n
LoadFileWithSize(path,dst,cap,useHd):  if !found return 0; if n > cap return -6; same as LoadFile
LoadFileWithMalloc(path,&sz,useHd,alt): if !found return NULL; old=*sz; *sz=n; if old==-1 return NULL; p=aligned_alloc(16,n); read; useHd: as above (failure -> NULL)
GetFileSize(path):                     found ? size : (real-file fallback: size of the OS file named by `path`, else 0)
```
`current` (the file named by the latest successful open) must be updated on every successful open, per thread of control that can call `SetResourceItem` (the original keeps it in a process global and has no lock; callers are the loader threads: sd stream threads, the TRANSFER thread, main task, so the port should make `open`+`SetResourceItem` atomic with a mutex).

### 1.3.3 `useHdAsset` and the remastered-asset protocol (exact)
`useHdAsset` is the last argument of `LoadFile*`. When 0 the loader just returns the bytes. When 1 (call sites: 0x1366da.. KHHD2.8.config reads pass 0; tozawa `name_table.bin` (0x12f489, 0x12f4df), 0x36e08d, 0x374a05/0x374a4a, sd `LoadFile` calls (0x3f98da, 0x3f9a81) pass 1) the loader additionally calls `SetResourceItem(path, size, buf)`, which hands the loaded image to the resource manager. The resource manager (`ResourceEntry` machinery, function 0x12cd80, active only when 0x79cfc8 == 3) then **pulls** the HD textures for that file:

```
n = GetRemasteredCount()                    // for the file opened last
for i in 0..n-1:
    e = GetRemasteredEntry(this, &orig, i)  // e->name, orig = origOffset
    buf = GetRemasteredAsset(&size, i)      // whole DDS or PNG file, 16-aligned heap block, ownership passes to the engine
    register (name-without-leading-'-', key = orig & 0xffffff, buf, size)
```
Entry semantics: `origOffset` = file offset of the texture inside the loaded image plus `0x20000000` (the "HD link" flag, high byte); the engine matches by the low 24 bits. `-1` marks an audio slot (a BAR entry of type 31/34 that starts with "ORIGIN"). Names starting with `-` are index-style: the registered name drops the `-`; if the `.shared` dword at 0x2bd4000 (initially 0, writers not isolated) is non-zero the digits after `-` also overwrite the low 24 bits (the KH1/ReCoM convention; inactive for KH2 as shipped). The extension decides the decoder: `.dds` (DXT5 3,017 and A8R8G8B8 604 in the mod) or `.png` (130 in the mod); other names (the audio slots) are SCD files.

Panacea builds the table when the file is loaded (`GetRemasteredFiles` -> `ScanRemasteredFolder`) and caches it keyed by CurrentFileName; the port builds it the same way. Algorithm (KH2 branch):
1. `assetOffsets` = ordered `origOffset` list produced by the container walker chosen by the **file extension** (case-insensitive; `.a.xx` language archives count as `.a.xx`):
   `.imd` GetIMDOffsets, `.imz` GetIMZOffsets, `.tm2` GetTM2Offsets, `.pax` GetPAXOffsets, `.dpd` GetDPDOffsets, and `.2dd .2ld .a.fm .a.fr .a.gr .a.it .a.sp .a.us .a.uk .a.jp .bar .bin .mag .map .mdlx` GetBAROffsets. Unknown extension: the package's own entry offsets are used (nothing for the loose tree).
2. `modfiles` = every regular file below `<tree>/remastered/<rel>/` **recursively**, as relative names with `/` (BFS order of FindFirstFile, which is NTFS name order). Names are stored in a 32-byte field, so each relative name must be <= 31 characters (longest in the mod: 27).
3. stable-sort `modfiles`: `.dds` before `.png` before everything else; two `.dds` (or two `.png`) names whose first character after an optional leading `-` is a digit are ordered by `atoi`; anything else compares equal (so `US_al0field_2dd0..3.png` keep name order, and the audio entries `se/obj/...` stay last in name order).
4. if `modfiles.size() >= assetOffsets.size()`: `entries = first assetOffsets.size() files`, `entries[i].origOffset = assetOffsets[i]`, `compressedSize = -2`, `decompressedSize` = file size. Otherwise the remaining entries are taken from the package's own table (names like `-%d.dds` with size 0) and the asset is fetched from the original package: **impossible without packages, so the baked tree must satisfy `files >= offsets` for every remastered directory** (the baker checks it).
5. `GetRemasteredAsset(i)` reads `<tree>/remastered/<rel>/<entry name>` (if that file is missing Panacea falls back to the package).

Container walker rules (from `Panacea.cpp`, ported in `remaster_walk.py`; all offsets are int32 and wrap): IMD: magic `IMGD`, entry = `*(int*)(p+8) + base + 0x20000000`. IMZ: magic `IMGZ`, `count = *(int*)(p+12)`, then `{int off; int size}` pairs from `p+16`, each IMD at `p+off`. TM2: magic `TIM2`, `count = *(short*)(p+6)`, entries stepped by their own `next` size from `p+0x10`, entry = `*(short*)(e+12) + (base + 0x10 + sum of the previous `next` sizes) + 0x20000000`. PAX: magic `PAX_`, DPX at `p + *(int*)(p+12) + 12`; DPX: `*(int*)p * 0x20 + 4` to the DPD table; DPD: magic 0x96, textures keyed by `TBP0/shX/shY`, textures that share TBP0 collapse onto one canvas entry. RAW (BAR type 7 TIM): per-GS-info entries with PSM-dependent adjustments (`+ Pxl4Count + Modifier*0x10` for PSM 20, `+ Pxl8Count*0x10` otherwise) and each `TEXA` block inserted directly after its base texture. BAR (magic `BAR\x01`): entries are collected into five groups and concatenated in the order **TIM(7), PAX(18), TM2(10), general (IMD 24, IMZ 29, raw 36, nested BAR 46), audio(31/34 with "ORIGIN")**.

**Validation against real data.** `walk_check.json` runs the ported walkers over the 224 mod files that have a `remastered/` directory and a local copy of the game file (the other 69 directories belong to vanilla files that are not in the mod): **215 directories have exactly as many files as walker offsets, 9 have more (the extras are dropped by the truncation rule), 0 have fewer.** That confirms the recursive-count rule, the nested audio names (98 audio-slot files, 225 MB of SCD) and the walker ports on the first 224 files. Not checked: the 69 vanilla-only directories and the DPD tie-break (`std::sort` is not stable upstream).

### 1.3.4 Exact loose-tree layout
```
<tree>/                                  (baked: vanilla extraction overlaid by the mod; 4,862 mod files + vanilla)
  00progress.bin 00battle.bin 03system.bin ...   top-level data files
  obj/ ard/ msn/ map/ menu/ msg/ field2d/ itempic/ magic/ voice/ bgm/ vagstream/ se/ ...
  remastered/<rel game path>/<entry name>        e.g. remastered/obj/P_EX100.mdlx/-0.dds
  remastered/<rel>/<sub>/<name>                  audio slots: remastered/obj/F_PO080_RTN.a.us/se/obj/se200
  raw/<rel>                                      optional, unused
  .khnmc/tree.idx, .khnmc/remastered.idx         produced by the baker (lower-case index; per-file ordered entry list)
```
Facts from the real trees: the mod's `remastered/` holds 293 game-file directories (224 with a local game file + 69 vanilla-only; corrected, the original said 390) and 390 directories that directly contain files (97 of those are nested audio slot directories such as `se/obj`), 3,849 files, 700.6 MB (463 MB DDS, 225 MB SCD, 12 MB PNG); entry names are `-N.dds` (3,608), `-N.png` (37), descriptive `US_<map>_2dd<N>.png` / `US_<scene>_bar<N>.dds|png`, and 98 nested audio names; 390 directories x {`.us` 108, no extension 98 (those are the nested audio dirs seen from the root), `.mdlx` 51, `.imd` 30, `.mag` 25, `.2dd` 23, `.map` 20, `.bar` 20, `.bin` 11, `.2ld` 4}. The vanilla extraction on build-host (`data/kh2`, 103,070 entries) uses the same layout: `remastered/` next to the game folders (`obj`, `map`, `field2d/{us,fr,gr,it,sp}`, `npack/worldobj.bar`, ...), HD texture names exactly as stored in the package (`-0.dds` ... or descriptive names), plus `KH2.IDX`, `KHHD2.5.config`, `KHHD2.8.config`, `save_image/`, `ICON/`.

Baker requirements (`tools/bake_nmc_tree.py`): (a) copy vanilla then overlay; mod.yml is 4,861 `copy` assets, so no OpenKH patcher run is needed for the file bytes; (b) because the mod replaces some game files and some remastered files, re-run the walkers on the **final** bytes of each file that has a remastered directory and fail if `files < offsets`; (c) emit the two indexes; (d) include `Image/SettingMenu` content: it is a separate package (`SettingMenu.hed/.pkg`) and `data/kh2` appears to contain none of it (open item, section 1.6).

## 1.4 Can the package files be bypassed entirely?

For the **data path: yes.** Every consumer of `PackageFiles` / the package vector is a function of the file layer itself: `GetFileInfo` 0x106940, `OpenFile` 0x107240, `ReadCurrentInto` 0x107620, `GetRemasteredCount/Asset/Entry` (0x106440, 0x106460, 0x106480), `AddPackage` 0x1065c0 (callers: axaAppMain 0x14f3b5, 0x14f455), `FreeAllPackages` 0x1064d0 (0x14f4c6), `OpenStream` 0x106da0/0x106ba0, `ReadStream` 0x106fa0, `SeekStream` 0x106830. References to `g_CFileMan` 0x79cfc0 (the `this` pointer) occur in exactly 30 functions: axaAppMain 0x14f1e0, the CFileMan ctor/dtor 0x107180/0x107140, the ResourceEntry code 0x12cd80, the three SettingMenu loaders (0x4f0cf0, 0x4f0e50, 0x4f1120), sd stream/bank code (0x136640, 0x1437b0, 0x13ca50, 0x13cae0, 0x13be30, 0x13c090, 0x135c10, 0x135ed0, 0x1361a0, 0x3f95f0, 0x3f9850, 0x3f99e0, 0x3f9c60), tozawa 0x12f140, axa-platform 0xfef80, 0xff460, 0x36ca50, 0x36cdb0, 0x36cee0, 0x36dfc0, 0x374640, and the dev scarchers 0x564580/0x564750. They all go through the entry points above.

Consumers that bypass the CFileMan loaders and read real files with `Axa::OpenFile` (0x1070b0, 19 call sites): **movies** (`CCipherMovie` 0x13da10: `STEAM/juefigs/KH2ReSource/zmovie/en/*.mp4`, 12 files in the Steam install; FindFirstFileA + `_sopen`, an MP4 that begins with `ftyp` is read through a cipher layer), generic `BasePath/<name>` loaders 0x148810, 0x148980, 0x148a80 (whole-file reads into an 0x948-byte object), `anim_relation.txt` 0x372df0, 0x3f96d0 (sd), 0x4b72e0/0x4b7370 (ppp-effects), the dev-only loaders (`//caelum/proj/...`, `C:/tmp/...` strings in 0x36c750, 0x36d000, 0x36d370, 0x36dfc0, 0x563ed0, 0x564580, 0x564750, 0x564d40, 0x36ca50: unreachable in the shipping configuration but present). Sound streams (`VAG_STREAM::play` -> `AxaSoundStream` 0x13a8d0 -> `OpenStream`) read **inside** the packages today, so `vagstream/*.win32.scd` needs the stream-fd API above, not just the loaders.

Other places that name the packages: `SettingMenu` pkg selection by `strstr(name, "SettingMenu.pkg")` in GetFileInfo, `SaveImage` (`save_image/en<name>` templates through LoadFileWithMalloc at 0xff319, relative to BasePath), and the language tables in 0x374640 / 0x12f140.

## 1.5 Replacement plan (summary)
1. Do not run `AddPackage`/`FreeAllPackages`; replace `GetFileInfo`, the 4 loaders, `GetRemasteredCount/Entry/Asset`, `SetResourceItem` stays lifted (it only registers the buffer), plus `OpenStream/ReadStream/CloseStream/SeekStream` and `Axa::OpenFile` with the loose-tree versions (`include/khnmc_file_layer.h`). Keep `CalcHash`, `DecryptFile`, `DecompressFile` unlifted/unused.
2. Provide a pseudo `PackageFile` for the current file so that any remaining reader of `+0x12c/+0x130/+0x158` sees consistent data.
3. `BasePath` (0x2bd4340) and `ReplacePath` (0x2bd4010) are plain buffers: let the lifted `AppInterface` setters run with a fixed root, or write them directly.
4. Streams: `OpenStream(path)` returns an fd for `<tree>/<rel>`; `ReadStream(fd, dst, off, len)` must read at **`off`** (the loose file has no 0x10-byte PkgEntry header, so use `entryOffset = -0x10` or drop the `+0x10`).

## 1.6 Open items and unverified points
- SettingMenu package content in the baked tree (not part of `data/kh2`; extract with the OpenKH Egs tool, which is Apache-2.0).
- Whether `GetRemasteredAsset` buffers are freed by the engine with `_aligned_free` (assumed; 74 `_aligned_free` sites exist).
- Thread safety: the "current file" global is unsynchronised in the original; the port serialises open + SetResourceItem.
- The `.shared` dword 0x2bd4000 writers.
- The 9 mod directories whose file count exceeds the walker count: check whether the extra file is a deliberate audio entry on a file whose audio slot the engine does not register.
- 69 remastered directories whose game file is vanilla-only could not be validated locally.
