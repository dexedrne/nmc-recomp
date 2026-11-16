# Name DB v2 coverage report

Input: `pc/kh2fm.exe` sha256 9002b2de6a1f91a7, corrected canonical universe (38,451 functions). Everything below is static analysis.

| quantity | count |
|---|---:|
| functions_total | 38,451 |
| named_real | 1,446 |
| named_role | 8,948 |
| named_class_vote | 3,121 |
| named_any | 13,515 |
| class_labelled | 10,684 |
| subsystem_labelled | 26,257 |
| unnamed | 24,936 |
| roots | 17,528 |
| roots_real | 636 |
| roots_named_any | 6,288 |
| roots_with_subsystem | 13,984 |
| classes_with_all_virtuals | 1,046 |
| types_with_vtables | 1,046 |
| ps2_matched_classes | 291 |
| ps2_classes_located_by_rtti | 287 |
| ps2_classes_with_function_transfer | 26 |
| ps2_functions_named | 64 |
| ps2_named_confident | 25 |
| ps2_named_high | 14 |

## Names by source

| source | functions |
|---|---:|
| rtti-vtable | 4,606 |
| callgraph-class-vote | 1,691 |
| rtti+vptr-store | 1,491 |
| class-region-adjacency | 1,430 |
| rtti-vtable-thunk | 1,319 |
| eh-funclet-parent | 1,151 |
| s7:data-table(name ptr+fn ptr, stride 0x50) | 419 |
| pe-imports | 340 |
| pe-export-table | 222 |
| thunk-target | 204 |
| rtti-vtable-thunk-target | 150 |
| clr-metadata-MethodDef | 110 |
| docs-spec | 105 |
| assert-string-fn-name | 86 |
| ps2-decomp:callgraph-propagation | 42 |
| s7:data-table(name ptr+fn ptr) | 40 |
| platform-seams-proposal | 36 |
| rtti-vtable-shared | 27 |
| panacea-openkh | 23 |
| ps2-decomp:table-content | 8 |
| ps2-decomp:class-ctor | 6 |
| ps2-decomp:class-dtor | 4 |
| s7:ps2-decomp:class-ctor | 2 |
| ps2-decomp:const-fingerprint | 1 |
| ps2-decomp:string-anchor | 1 |
| s7:ps2-decomp:string-anchor | 1 |

## Name tier x confidence

| tier/confidence | functions |
|---|---:|
| A-real-name/high | 1,288 |
| A-real-name/low | 47 |
| A-real-name/medium | 111 |
| B-class-role/high | 7,557 |
| B-class-role/low | 640 |
| B-class-role/medium | 751 |
| C-class-vote/low | 2,170 |
| C-class-vote/medium | 951 |

## Subsystems

| subsystem | functions | bytes | seed | adjacency | call-graph | named | real names |
|---|---:|---:|---:|---:|---:|---:|---:|
| (unlabeled) | 12,194 | 1,144,232 | 0 | 0 | 0 | 413 | 2 |
| sd-audio | 5,391 | 765,918 | 2741 | 2262 | 292 | 2803 | 103 |
| gummi | 4,252 | 758,716 | 1137 | 2036 | 1077 | 1643 | 4 |
| kh2:yasui-YS | 3,929 | 402,927 | 1970 | 1442 | 515 | 2197 | 13 |
| kh2:harata-dk | 2,449 | 503,478 | 526 | 1264 | 659 | 733 | 9 |
| settingmenu | 1,587 | 244,825 | 621 | 815 | 151 | 870 | 2 |
| kh2:taito-minigame-Ti | 1,152 | 114,220 | 773 | 268 | 111 | 840 | 0 |
| kh2:tozawa-menu-Tz | 989 | 220,298 | 152 | 502 | 335 | 221 | 23 |
| kh2:naka-kn | 981 | 228,757 | 399 | 440 | 141 | 511 | 6 |
| crt-stl-lib | 918 | 57,012 | 691 | 179 | 9 | 730 | 339 |
| gx-graphics | 765 | 231,428 | 262 | 243 | 260 | 310 | 52 |
| kh2:ppp-effects | 693 | 200,372 | 419 | 173 | 101 | 421 | 419 |
| axa-platform | 532 | 71,330 | 327 | 183 | 20 | 377 | 78 |
| kh2:global-game | 502 | 70,561 | 105 | 268 | 128 | 123 | 8 |
| libpng | 411 | 114,773 | 209 | 124 | 76 | 219 | 209 |
| filemgr | 335 | 68,935 | 95 | 105 | 14 | 221 | 67 |
| kh2:ishi-image-YI | 242 | 56,994 | 61 | 113 | 68 | 72 | 7 |
| kh2:worldmap | 230 | 13,519 | 209 | 16 | 5 | 210 | 1 |
| kh2:sugi-action-Ts | 160 | 20,318 | 111 | 37 | 12 | 116 | 0 |
| kh2:sasa-musical-sa | 143 | 21,252 | 80 | 48 | 15 | 97 | 0 |
| kh2:libyw-widget-YW | 132 | 15,870 | 76 | 53 | 3 | 85 | 0 |
| kh2:menu | 84 | 12,571 | 52 | 26 | 6 | 62 | 8 |
| steam | 83 | 5,358 | 61 | 21 | 0 | 54 | 19 |
| input | 73 | 16,233 | 50 | 15 | 4 | 50 | 27 |
| kh2:battle | 55 | 10,613 | 8 | 44 | 3 | 30 | 8 |
| movie | 52 | 7,503 | 19 | 8 | 0 | 52 | 18 |
| kh2:script | 32 | 11,244 | 9 | 14 | 9 | 14 | 9 |
| kh2:rikeda-effect-ryj | 29 | 4,265 | 10 | 12 | 7 | 11 | 1 |
| clr-atl | 25 | 4,260 | 5 | 14 | 2 | 6 | 2 |
| jobmgr-fibers | 17 | 1,355 | 13 | 4 | 0 | 12 | 0 |
| kh2:player | 13 | 5,916 | 11 | 2 | 0 | 11 | 11 |
| kh2:effects | 1 | 44 | 1 | 0 | 0 | 1 | 1 |

## Extent fixes

```
{
 "s7_old_size_differs": 3587,
 "roots_with_fragments": 3315,
 "bytes_added_by_merging": 1508979,
 "data_in_code_functions": 416,
 "data_in_code_bytes": 45064
}
```

## S7 re-validation

```
{
 "size_differs_merged": 3587,
 "superseded_by_v2_rtti(rtti+vptr-store)": 1699,
 "superseded_by_v2_rtti(address-adjacency)": 452,
 "carried(ps2-decomp:class-ctor)": 2,
 "rejected(string-xref)": 29,
 "rejected(clr-metadata-MethodDef)": 42,
 "superseded_by_v2_rtti(rtti-vtable)": 5591,
 "carried(string-xref)": 32,
 "carried(clr-metadata-MethodDef)": 110,
 "carried(pe-export-table)": 222,
 "rva_not_in_corrected_universe": 4,
 "carried(ps2-decomp:string-anchor)": 1,
 "carried(data-table)": 459,
 "carried(pe-imports)": 123,
 "rejected(ps2-decomp:string-anchor)": 1
}
```

## Panacea (OpenKH) byte patterns

| pattern | status | hits | function starts |
|---|---|---|---|
| pfn_Axa_CFileMan_LoadFile | match | 0x106700 | 0x106700 |
| pfn_Axa_CFileMan_LoadFileWithSize | match | 0x106790 | 0x106790 |
| pfn_Axa_CFileMan_LoadFileWithMalloc | match | 0x106870 | 0x106870 |
| pfn_Axa_CFileMan_GetFileSize | match | 0x1063d0 | 0x1063d0 |
| pfn_Axa_AxaResourceMan_SetResourceItem | match | 0x12f070 | 0x12f070 |
| pfn_Axa_PackageMan_GetFileInfo | match | 0x106940 | 0x106940 |
| pfn_Axa_CalcHash | match | 0x145930 | 0x145930 |
| pfn_Axa_SetReplacePath | match | 0x102840 | 0x102840 |
| pfn_Axa_FreeAllPackages | match | 0x1064d0 | 0x1064d0 |
| pfn_Axa_CFileMan_GetRemasteredCount | match | 0x106440 | 0x106440 |
| pfn_Axa_CFileMan_GetRemasteredEntry | match | 0x106480 | 0x106480 |
| pfn_Axa_PackageFile_GetRemasteredAsset | match | 0x107760 | 0x107760 |
| pfn_Axa_PackageFile_OpenFileImpl | match | 0x107240 | 0x107240 |
| pfn_Axa_AxaSoundStream__threadProc | match | 0x13c090 | 0x13c090 |
| pfn_Axa_OpenFile | match | 0x1070b0 | 0x1070b0 |
| pfn_Axa_DebugPrint | ambiguous | 0x143760, 0x152410 | 0x143760, 0x152410 |
| pfn_Axa_DecryptFile | match | 0x1062b0 | 0x1062b0 |
| pfn_Axa_DecompressFile | match | 0x4696f0 | 0x4696f0 |
| pfn_VAG_STREAM_play | match | 0x537420 | 0x537420 |
| pfn_VAG_STREAM_fadeOut | match | 0x5373e0 | 0x5373e0 |
| pfn_VAG_STREAM_setVolume | match | 0x5374a0 | 0x5374a0 |
| pfn_VAG_STREAM_exit | match | 0x5373a0 | 0x5373a0 |
| settingsfunc | match | 0x14d070 | 0x14d070 |
| volumefunc | match | 0x137d50 | 0x137d50 |
| framefunc | no-match | - | - |
| framefunc | no-match | - | - |
| LaunchGameEpic | no-match | - | - |
| LaunchGameSteam | launcher-pattern-rejected | 0x143690 | 0x143690 |
| axaAppMain | launcher-pattern-rejected | 0x14f1e0 | 0x14f1e0 |
| axaAppMain | launcher-pattern-rejected | 0x7c0e0, 0x266f70 | 0x7c0e0, 0x266f70 |

## PS2 transfer

```
{
 "anchors": {
  "string": 31,
  "const": 28,
  "ctor_dtor": 11,
  "table": 69,
  "table_detail": {
   "32": {
    "ps2_tables": 2844,
    "pc_tables": 4919,
    "common": 152,
    "anchors": 49
   },
   "16": {
    "ps2_tables": 2458,
    "pc_tables": 3494,
    "common": 183,
    "anchors": 20
   }
  }
 },
 "holdout": {
  "not-predicted": {
   "correct": 0,
   "total": 319
  },
  "predicted": {
   "correct": 20,
   "total": 21
  }
 },
 "class_agreement": {
  "string-anchor/None": 30,
  "string-anchor/True": 1,
  "const-fingerprint/None": 28,
  "table-content/None": 69,
  "class-dtor/True": 4,
  "class-ctor/True": 7,
  "callgraph-propagation/None": 202,
  "callgraph-propagation/True": 3
 }
}
```
