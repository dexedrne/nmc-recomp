/* khnmc_hd.h : remastered (HD) asset protocol over the loose tree (hd_assets.c).
 * The loaders in file_layer.c call kh_hd_begin() before the lifted SetResourceItem and kh_hd_end() after it; while a table is current the hooks
 * f_106440 (GetRemasteredCount), f_106480 (GetRemasteredEntry) and f_106460 (GetRemasteredAsset thunk) serve it, otherwise they fall through
 * to the untouched lifted originals (real-package mode). */
#pragma once
#include "khnmc_rt.h"
#include <stdint.h>

typedef struct HdTable HdTable;
/* Makes the table of the file `rel` (path relative to the tree root, '/' separated) current for this thread and returns the previously current table.
 * `bytes`/`n` are the loaded bytes of the file (the container walkers run on them). Never returns NULL; zero entries when there is no remastered folder. */
HdTable *kh_hd_begin(KCtx *g, const char *root, const char *rel, const unsigned char *bytes, int64_t n);
void kh_hd_end(HdTable *previous);
void kh_hd_report(FILE *f);
int kh_hd_selftest(KCtx *g);
/* `khnmc --hd-check [report.tsv]`: builds the table of every game file of the tree KHNMC_DATA with the code a run uses and prints the totals
 * (files by rule, entries by source, the container walker against the package tables).  Host only: no guest image, nothing runs.
 * Returns the exit status: 0, 1 when a table could not be built, 2 on set-up errors. */
int kh_hd_check(const char *report);
/* Round 6: the format the game's DDS reader f_118a90 gives a header (87/77/74/71; -1 no HD texture; 0 DXGI_FORMAT_UNKNOWN; -2 not a
 * DDS), and the repair of a DXT1..5 header with a nonzero RGBBitCount / masks (1 = repaired). */
int kh_dds_game_format(const unsigned char *b,size_t n);
int kh_dds_repair_header(unsigned char *b,size_t n);
