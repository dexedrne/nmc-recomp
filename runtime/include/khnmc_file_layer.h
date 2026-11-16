// KHNMC DECOMP EDITION - platform seam: FILE LAYER (design sketch, not compiled).
// Source of truth for every offset: ../01-FILE-LAYER.md (read from kh2fm.exe sha256 9002b2de6a1f91a7..., static analysis only).
// Everything game-derived stays in scratch; this header contains only layouts and signatures.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace khnmc::file {

// ---- engine ABI structs (sizes/offsets verified against 0x106940/0x107240/0x107620/0x107760) ----
struct HedEntry {                // 0x20 bytes, array = whole <pkg>.hed file (FileCount = size >> 5)
  uint8_t hash[16];              // Axa::CalcHash(name-after-BasePath) (two qwords compared)
  int64_t offset;                // byte offset of the entry inside <pkg>.pkg
  int32_t dataLength, actualLength;
};
struct PkgEntry {                // 0x10 bytes at the start of every entry in the .pkg
  int32_t decompressedSize;      // PackageFile+0x12c
  int32_t remasteredCount;       // +0x130
  int32_t compressedSize;        // +0x134 ; >0 zlib; -1 = plain but DecryptFile'd; -2 (and <=0 in general) = plain
  int32_t creationDate;          // +0x138
};
struct RemasteredEntry {         // 0x30 bytes, `remasteredCount` of them follow the PkgEntry
  char    name[32];              // e.g. "-12.dds", "US_al0field_2dd0.png", "se/obj/se200"  (<=31 chars + NUL)
  int32_t offset;                // file offset of the asset payload (computed when the entry is opened)
  int32_t origOffset;            // key into the loaded file image: (file offset of the texture) + 0x20000000 flag;  -1 = audio slot
  int32_t decompressedSize;
  int32_t compressedSize;        // <=0: stored; -2 is what the loose tree uses
};

// ---- loose tree --------------------------------------------------------------------------------------
// <root>/<rel>                          the game file (rel = engine path with "<BasePath>/" removed)
// <root>/remastered/<rel>/<entry name>  one HD asset per remastered entry (name may contain '/')
// <root>/raw/<rel>                      optional: PkgEntry-wrapped replacement (Panacea "raw"); the baker does not emit it
struct TreeConfig {
  std::string root;              // baked tree (vanilla extraction overlaid with the NMC mod files)
  bool        useIndex = true;   // load <root>/.khnmc/tree.idx (lower-case rel path -> actual path/size) instead of walking the FS
};

class LooseTree {
 public:
  bool open(const TreeConfig&);
  // engine path (may carry the "<BasePath>/" prefix) -> normalised relative path ('/' separators, as stored in the index)
  static bool stripBasePath(std::string_view enginePath, std::string_view basePath, std::string* rel);
  bool exists(std::string_view rel, std::string* abs = nullptr, int64_t* size = nullptr) const;   // ASCII case-insensitive
  bool readAll(std::string_view rel, void* dst, int64_t cap, int64_t* size) const;
  bool remasteredDir(std::string_view rel, std::string* dir) const;                              // <root>/remastered/<rel>
};

// ---- remastered-asset table of the "currently opened file" ------------------------------------------------
// Engine protocol (ResourceEntry::ctor 0x12cd80): after LoadFile*(.., useHdAsset=1) the engine calls SetResourceItem,
// which calls GetRemasteredCount()/GetRemasteredEntry(i)/GetRemasteredAsset(i) for the file opened LAST (global state:
// PackageFiles[LastOpenedPackage]->CurrentFileName). The port keeps one `current` per thread-safe file-layer instance.
struct RemasteredTable {
  std::string rel;                               // file the table belongs to
  std::vector<RemasteredEntry> entries;          // ordered exactly like Panacea::ScanRemasteredFolder (see doc section 3.4)
};
// Build the table from the bytes of the loaded file and the files below <root>/remastered/<rel>/.
// `bytes` must be the final (mod-overlaid) file because origOffset values are offsets inside it.
bool buildRemasteredTable(const LooseTree&, std::string_view rel, const void* bytes, int64_t size, RemasteredTable* out);
// Container walkers (ports of GetIMDOffsets/GetIMZOffsets/GetTM2Offsets/GetDPDOffsets/GetDPXOffsets/GetPAXOffsets/GetRAWOffsets/GetBAROffsets).
// Return the ordered list of origOffset values; -1 marks an audio ("ORIGIN") slot. nullopt-like: returns false for unknown extensions.
bool walkAssetOffsets(std::string_view rel, const void* bytes, int64_t size, std::vector<int32_t>* offsets);

// ---- the 24 engine entry points the port replaces (guest ABI = Win64 x64 calling convention) -----------------
// RVAs are relative to image base 0x140000000.
struct EngineFileApi {
  // 0x106700  returns size; -1 not found; -2 read size mismatch; 0 if size==0 or SetResourceItem()==-1 (useHd)
  long (*LoadFile)(void* cfileman, const char* path, void* dst, bool useHdAsset);
  // 0x106790  returns size; 0 not found; -6 if dstCap < size (checked BEFORE reading); -2 mismatch
  long (*LoadFileWithSize)(void* cfileman, const char* path, void* dst, int dstCap, bool useHdAsset);
  // 0x106870  *sizeInOut := file size always; if the incoming *sizeInOut == -1 -> size-query only, returns NULL;
  //           else returns _aligned_malloc(size,16) holding the file; on read failure *sizeInOut = -2, returns NULL
  void* (*LoadFileWithMalloc)(void* cfileman, const char* path, int* sizeInOut, bool useHdAsset, const char* altBase);
  // 0x1063d0  size of the entry, else FindFirstFileA(path) size of a real file, else 0
  long (*GetFileSize)(void* cfileman, const char* path);
  // 0x12f070  registers a loaded buffer with the resource manager (strips BasePath), returns -1 on failure
  long (*SetResourceItem)(const char* path, int size, void* buf);
  // 0x106940  opens `path` in the package set and makes it the "current file"; returns the PackageFile* (non-null = found)
  void* (*GetFileInfo)(const char* path, const char* altBase);
  // 0x106440 / 0x106480 / 0x107760 (via thunk 0x106460)
  int64_t (*GetRemasteredCount)();
  RemasteredEntry* (*GetRemasteredEntry)(void* cfileman, int* outOrigOffset, int assetNum);
  void* (*GetRemasteredAsset)(void* pkgfile, uint32_t* outSize, int assetNum);  // 16-aligned malloc, caller frees
  // streaming (not covered by Panacea; needed for sound streams): 0x106da0 open, 0x106fa0 read, 0x106b40 close
  int  (*OpenStream)(void* cfileman, const char* path);
  long (*ReadStream)(void* cfileman, int fd, void* dst, int64_t off, int len);   // read at (off + entryOffset + 0x10)
  void (*CloseStream)(void* cfileman, int fd);
  // 0x1070b0 Axa::OpenFile(path, oflag) -> fd or -1 ; 0x145930 CalcHash (unused by the loose tree)
};

// ---- baker contract (tools/bake_nmc_tree.py) ------------------------------------------------------------------
// 1. copy vanilla extraction (<data>/kh2/**) to <root>; 2. overlay the 4,861 mod files (mod.yml: every asset is method `copy`);
// 3. for every <root>/remastered/<rel>/ directory: run walkAssetOffsets on <root>/<rel> and require
//    count(files below the directory) >= offsets.size()  (verified on 224 local files: 215 equal, 9 greater, 0 smaller);
// 4. write <root>/.khnmc/tree.idx (lower-case rel -> real path, size) and <root>/.khnmc/remastered.idx (per rel: ordered entry list
//    {name, origOffset, size}) so the runtime needs neither directory enumeration order nor the walkers.
}  // namespace khnmc::file
