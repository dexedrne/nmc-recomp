#pragma once
#include "khnmc_rt.h"

extern _Atomic u64 kh_display_ticks, kh_present_ns, kh_acquire_ns, kh_slot_wait_ns;
void kh_frame_time(KCtx *g,u64 frame);

/* Optional work counters for recorded routes on the null and wrapped backends. */
extern _Atomic int kh_frame_work,kh_frame_log;
extern _Atomic u64 kh_vertices, kh_blend_vertices, kh_texture_uploads, kh_file_reads, kh_bg_wait_ns;
void kh_frame_time_init(void);
