/* seam report (counts for the run summary) */
#include "khnmc_rt.h"
void kh_gfx_report(FILE *f);
void kh_report_seams(FILE *f){
    fprintf(f,"kh: seam call counts: steam=%llu window=%llu gfx=%llu audio=%llu input=%llu mf=%llu; GX reached=%d\n",(unsigned long long)kh_cnt_steam_calls,(unsigned long long)kh_cnt_window_calls,
            (unsigned long long)kh_cnt_gfx_calls,(unsigned long long)kh_cnt_audio_calls,(unsigned long long)kh_cnt_input_calls,(unsigned long long)kh_cnt_mf_calls,(int)kh_gx_reached);
    kh_gfx_report(f); kh_com_dump(f);
}
