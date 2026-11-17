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
