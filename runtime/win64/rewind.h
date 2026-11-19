#pragma once
#include "savestate.h"

/* The record stream is private to this process. A disk slot contains only its
 * session key. Sizes and addresses never come from the slot file. */
typedef struct KhRecords {
    struct KhBlock *first, **tail, *read;
    int loading;
} KhRecords;
void kh_record(KhRecords *,void *,size_t);
void kh_record_pages(KhRecords *,void *,size_t);
#define KH_RECORD(r,x) kh_record((r),&(x),sizeof(x))
void kh_records_begin(KhRecords *,int load);
void kh_records_end(KhRecords *);
void kh_records_free(KhRecords *);

void kh_rewind_arena(KhRecords *);
void kh_rewind_input(KhRecords *);
void kh_rewind_gfx(KhRecords *);
void kh_rewind_route(KhRecords *);
void kh_rewind_phase(KhRecords *);
void kh_rewind_warp(KhRecords *);
void kh_rewind_mod(KhRecords *);
void kh_rewind_maps(KhRecords *);
void kh_rewind_ids(KhRecords *);
void kh_rewind_files(KhRecords *);
void kh_rewind_hd(KhRecords *);
void kh_rewind_cutscenes(KhRecords *);
const char *kh_rewind_fiber_check(int loading);
void kh_rewind_fibers(KhRecords *);
u32 *kh_rewind_rng(void);
u64 kh_audio_output_frame(void);
const char *kh_rewind_handles_check(void);
void kh_rewind_handles(KhRecords *);
const char *kh_rewind_crt_check(void);
void kh_rewind_crt(KhRecords *);
int ga_spread_on(void);
struct KhCrtFiles;
struct KhCrtFiles *kh_crt_files_capture(void);
void kh_crt_files_restore(struct KhCrtFiles *);
void kh_crt_files_discard(struct KhCrtFiles *);
