/* seam report (counts for the run summary) */
#include "khnmc_rt.h"
void kh_gfx_report(FILE *f);
void kh_report_seams(FILE *f){
    fprintf(f,"kh: seam call counts: steam=%llu window=%llu gfx=%llu audio=%llu input=%llu mf=%llu; GX reached=%d\n",(unsigned long long)kh_cnt_steam_calls,(unsigned long long)kh_cnt_window_calls,
            (unsigned long long)kh_cnt_gfx_calls,(unsigned long long)kh_cnt_audio_calls,(unsigned long long)kh_cnt_input_calls,(unsigned long long)kh_cnt_mf_calls,(int)kh_gx_reached);
    kh_gfx_report(f); { extern void kh_gfx_tex_report(FILE*); kh_gfx_tex_report(f); } { extern void kh_gfx_mem_report(FILE*); kh_gfx_mem_report(f); } { extern void kh_hd_report(FILE*); kh_hd_report(f); } { extern void kh_audio_report(FILE*); kh_audio_report(f); } { extern void kh_stream_report(FILE*); kh_stream_report(f); } { extern void kh_data_deny_report(FILE*); kh_data_deny_report(f); } { extern void kh_events_stub_report(FILE*); kh_events_stub_report(f); } { extern void kh_music_report(FILE*); kh_music_report(f); } kh_com_dump(f);
}
