#pragma once
#include "savestate.h"

/* Resident owner records and the pointer-free disk sections share this stream. */
typedef struct KhRecords {
    struct KhBlock *first, **tail, *read;
    int loading;
    size_t position;
} KhRecords;
void kh_record(KhRecords *,void *,size_t);
void kh_record_value(KhRecords *,void *,size_t);
#define KH_VALUE(r,x) kh_record_value((r),&(x),sizeof(x))
void kh_record_time(KhRecords *,u64 *);
void kh_record_pages(KhRecords *,void *,size_t);
#define KH_RECORD(r,x) kh_record((r),&(x),sizeof(x))
void kh_records_begin(KhRecords *,int load);
void kh_records_end(KhRecords *);
void kh_records_free(KhRecords *);

void kh_rewind_arena(KhRecords *);
void kh_rewind_input(KhRecords *);
void kh_rewind_aspect(KhRecords *);
void kh_rewind_gfx(KhRecords *);
void kh_rewind_route(KhRecords *);
void kh_rewind_phase(KhRecords *);
void kh_rewind_warp(KhRecords *);
void kh_rewind_window(KhRecords *,u64 *pending_size);
void kh_rewind_shell(KhRecords *);
void kh_rewind_xion(KhRecords *);
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

struct KhGpuCut;
const char *kh_gpu_cut_check(struct KhGpuCut **,int load);
void kh_gpu_cut_free(struct KhGpuCut *);
void kh_gpu_cut_apply(struct KhGpuCut *,int load);
