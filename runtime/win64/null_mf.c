/* null_mf.c : the Media Foundation / D3D11 movie seam.  This port has NO movie decoder (owner 2026-11-16: pre-rendered movies are out
 * of scope) and its data trees hold no movie file.  Task 44: a movie must not hang the game.
 *
 * What the game does (exe RVAs; implementation notes has the whole path).  A movie is played by ONE event command, type 0x44, of an event
 * (cutscene) entry of an area file: 0x2db030 in the event's load pass, 0x2db1c0 in its timeline.  0x2db1c0 builds
 * "<BasePath>/zmovie/<en|jp>/<name>.mp4" and talks to the movie module through the 8 commands of 0x5375d0 (reached through 0x528090):
 *   0 start(path)   FindFirstFileA(path) (0x13f280); a missing file: return -1, NOTHING is queued.  Else a request is queued and the
 *                   movie task 0x537560 runs the player's state machine 0x4738f0 once per frame until its state is 9
 *   1 "not yet"     0x2db1c0 yields in a loop until this is 0: the player has drawn-or-skipped its first frame ([player+0x130])
 *   5 "ended"       inner state 10: the reader said end of stream.  The event's per-frame task (0x2cf980, movie mode) then ends the
 *                   event (0x2cbd80): the NATURAL END.  The event's frame counter is command 7 = the movie's position
 *   2 stop  3 / 4 pause, resume  6 always 1  7 position in event frames
 * Before this task: no movie file, so command 0 failed, and the event waited at frame 0 for an end that only a movie can report (5
 * is never true, 7 stays 0): the black screen the owner saw, until the pause screen's Skip set the event's skip flag (0x2cfcd0).
 * (With a file present the old seam was worse: MFCreateSourceReaderFromURL failed and 0x13d500 read through the null reader.)
 *
 *   KHNMC_MOVIES unset / `end`   every movie is an EMPTY movie: it opens, reports end of stream at its first read and the game's own
 *                                player and event code end it as at a movie's natural end, at event frame 0.  No frame is decoded or
 *                                shown; the player draws its own (zeroed, black) 384x216 picture for the frames it lives.  How:
 *                                 - the two file checks of the movie path (FindFirstFileA at 0x13f2a0 and 0x13da60) are answered
 *                                   "present" whether or not a file is there (kh_movie_find, called by k32.c);
 *                                 - the header read of Movie::open 0x13da10 (Axa::OpenFile + _read of 0x100 bytes) gets a stand-in
 *                                   whose bytes 4..7 are `ftyp`, i.e. "a plain MP4": the engine's cipher step, which rewrites the first
 *                                   0x100 bytes of the file IN PLACE and starts a thread that writes them back later, is skipped, and a
 *                                   movie file that does exist is never opened (kh_movie_open_fd, called by crt.c while the hook of
 *                                   0x13da10 in phase_log.c says an open is running);
 *                                 - D3D11CreateDevice, the DXGI device manager, the attribute store, the source reader and its media
 *                                   types are strict null objects that serve exactly the calls of this path (below); the reader's
 *                                   ReadSample answers MF_SOURCE_READERF_ENDOFSTREAM with no sample for both streams;
 *                                 - the audio side is the runtime's ordinary XAudio2 seam (a second engine, a mastering voice and a
 *                                   float 48 kHz stereo source voice that never gets a buffer).
 *                                Every other method of these objects is a named fatal (`movie-unexpected-call`), nothing answers 0
 *                                by default; a reader made outside Movie::open is the named fatal `movie-open-hook`; a Movie::open
 *                                that fails is the named fatal `movie-open-failed` (the scene would wait for ever, and the three
 *                                ending movies run in scenes that have no pause screen: no Skip).
 *   KHNMC_MOVIES=fail            the seam as it was before task 44: the file checks are the file system's, the objects accept every
 *                                call and MFCreateSourceReaderFromURL fails with MF_E_UNSUPPORTED_BYTESTREAM_TYPE.  The game's own
 *                                calls and answers are the old ones (also SwitchToThread: the plain yield, runtime/threads.c); the
 *                                log has the lines below in addition
 *   anything else                named fatal `movies-mode` (at start-up)
 *
 * What is reported, and where it is seen (review of task 44).  Four hooks, all pass-throughs of the game's own code:
 *   0x5375d0 (movie command; only command 0 is looked at)   `kh: movie: #N <name> requested ...` comes from the file check inside it.
 *                 Command 0 has a guard BEFORE that check (exe 0x537632..0x537649): a player object that still exists with a non-zero
 *                 [+0x11c] / [+0x120] makes it answer 0 with no file check, no request and no movie task, and the scene then waits
 *                 for an end no movie reports (the PC game does the same).  That is `kh: movie: REQUEST DROPPED ...`, counted.
 *   0x2cbd80 (the event's end)   `kh: movie: #N <name>: the event ended by itself ...` when the caller is the natural-end site of the
 *                 event's per-frame task (0x2cf980: movie command 5 answered "ended", return address 0x2cf99f), `... ended by SKIP`
 *                 when it is the skip site (0x2cfa66: the event's skip flag 0x140b64f9c).  Also which branch the clean-up takes: as a
 *                 task (0x2cbe30 with its task: movie command 2 stops the movie and destroys the player) or directly (0x2cbe30(0):
 *                 command 2 is skipped, the player survives and the NEXT request is dropped by the guard above).
 *   0x13da10 (Movie::open)       phase_log.c: the stand-in header, `movie-open-hook`, `movie-open-failed`.
 *   0x13d690 (the player's picture)   the engine makes a width x height x 4 texture per movie and never frees it (0x473c5c only
 *                 clears the pointer; its texture collector 0x12ba80 takes textures released by name, 0x12b580, which the player
 *                 does not do): with a real backend that was 8.3 MB per movie, one every 90 s at the title.  The empty movie's
 *                 picture is 384 x 216 (still a zeroed page run) and the SAME picture is handed out again for every later movie as
 *                 long as the texture manager still lists it with its name and size (else the engine makes a new one): counted.
 * The reader's last Release prints `... player torn down: reader released ...`: that is the PLAYER's end (it comes the same way after
 * a natural end, a Skip and a soft reset), not the event's.  Counters at exit (`kh: movies:`) when a movie was requested or dropped.
 * With no movie requested the default mode does nothing the old seam did not do: its classes are registered at the first movie, not
 * at start-up, so the guest arena of a run without a movie is the same as before. */
#include "kh_look.h"
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

u64 kh_game_frames(void);                       /* null_input.c: frames the game thread has made */
u64 kh_com_freed(void);                         /* com.c: objects freed at their last Release (selftest) */
enum { MV_UNSET=-1, MV_END=0, MV_FAIL=1 };
static int mv_mode=MV_UNSET;
static _Atomic u64 n_req, n_req_present, n_probe_open, n_open, n_open_ok, n_standin, n_readers, n_ended, n_vreads, n_areads, n_duration, n_d3d11, n_fail_readers;
/* review of task 44: n_ended = readers released (the player's teardown).  The event side: */
static _Atomic u64 n_req_nopause, n_cmd0, n_queued, n_not_queued, n_dropped, n_cmd0_odd, n_ev_natural, n_ev_skip, n_ev_other, n_ev_direct, n_pic_made, n_pic_reused, n_pic_lost, n_pic_uncached;
static char req_name[160];                      /* the movie of the last request: game thread only */
static __thread int in_open;                    /* Movie::open 0x13da10 runs on this thread (default mode) */
static __thread int in_cmd0;                    /* movie command 0 (0x5375d0) runs on this thread: its hook is in this build */
/* the movie whose event has not ended yet (game thread only): serial 0 = a request the game dropped at its guard */
static struct { int pending, queued, dropped; u64 serial, frame, req0; char name[160]; } cur;

static int movies_mode(void){
    int m=mv_mode; if(m!=MV_UNSET) return m;
    const char *e=getenv("KHNMC_MOVIES");
    if(!e||!*e||!strcmp(e,"end")) m=MV_END;
    else if(!strcmp(e,"fail")) m=MV_FAIL;
    else kh_fatal(NULL,"movies-mode: KHNMC_MOVIES=%s is not a mode (unset or `end` = every movie is an empty movie that ends at once, as at its natural end; `fail` = the seam before task 44: the game waits for the pause screen's Skip)",e);
    mv_mode=m; return m;
}

/* ---- the lenient objects of KHNMC_MOVIES=fail (the whole seam before task 44; also registered in the default mode, unused) -------- */
static int C_attr,C_mgr,C_mt,C_d3d11,C_ctx;
static int h_lenient(KCtx *g,u64 o,int s,const char *c){ (void)g;(void)o;(void)s;(void)c; return 0; }
void kh_seam_mf_init(void){
    C_attr=kh_com_class("mf:IMFAttributes",48,0,h_lenient); C_mgr=kh_com_class("mf:IMFDXGIDeviceManager",16,0,h_lenient); C_mt=kh_com_class("mf:IMFMediaType",56,0,h_lenient);
    C_d3d11=kh_com_class("d3d11:ID3D11Device",48,0,h_lenient); C_ctx=kh_com_class("d3d11:ID3D11DeviceContext",64,0,h_lenient);
    movies_mode();                               /* a wrong KHNMC_MOVIES ends the run here, not at the first movie */
}

/* ---- the file side (default mode) --------------------------------------------------------------------------------------------- */
#define SITE_REQUEST 0x13f2a6u                  /* return address of FindFirstFileA in 0x13f280: the check of movie command 0 (0x5375d0) */
#define SITE_OPEN    0x13da66u                  /* return address of FindFirstFileA in Movie::open 0x13da10 */
static const char *leaf(const char *p){ const char *a=strrchr(p,'/'), *b=strrchr(p,'\\'); if(b>a) a=b; return a?a+1:p; }
#define PAUSE_OFF    (GS_BASE+0xabb878ull)      /* pause-disable bits (0x157250 sets, 0x157260 clears): the Start-button task 0x1572b0 does nothing while it is not 0 */
#define PAUSE_OFF_EVENT 2u                      /* bit 1: the event starter 0x3ac350 sets it for an event type with bit 7 (0x3ac402): ena / sec / sec2 */
#define EV_STATE     (GS_BASE+0xb65210ull)      /* the event's state; 4 = ended (0x2cbd80 returns at once then) */
#define EV_TASKED_A  (GS_BASE+0x2a11478ull)     /* 0x3ac220: an event runner exists */
#define EV_TASKED_B  (GS_BASE+0x728440ull)      /* 0x1dcb20: the byte the runner sets (0x1dcd00) for an event type whose `evtp` flag bit 0 is set */
#define SITE_EV_NATURAL 0x2cf99fu               /* return address of 0x2cbd80 in 0x2cf980: movie command 5 answered "ended" */
#define SITE_EV_SKIP    0x2cfa66u               /* return address of 0x2cbd80 in 0x2cf980: the event's skip flag 0x140b64f9c is set */
#define TEX_MGR      (GS_BASE+0x8aeef8ull)      /* the GX texture manager: list head +0x18, an entry's next +8, name +0x14, width / height words +0x94 / +0x96 (0x12b650) */
/* the line of a request (its own function: the selftest reads both texts) */
static void say_request(FILE *f,u64 n,const char *name,u64 frame,const char *win,int present,int m,u32 pause_off){
    int nopause=(pause_off&PAUSE_OFF_EVENT)!=0;
    char scene[160]; if(nopause) snprintf(scene,sizeof scene,"this scene has NO pause screen (pause-disable bits 0x%x: the event's type has bit 7), so there is no Skip in it",pause_off);
    else snprintf(scene,sizeof scene,"the pause screen is not disabled by the event's type (pause-disable bits 0x%x)",pause_off);
    if(m==MV_END)
        fprintf(f,"kh: movie: #%llu %s requested at game frame %llu (\"%s\"; the file is %s; %s): this port has no movie decoder: the movie is an empty one that ends at once, as at its natural end (KHNMC_MOVIES=end, the default)\n",
                (unsigned long long)n,name,(unsigned long long)frame,win,present?"present, and is not opened":"absent",scene);
    else
        fprintf(f,"kh: movie: #%llu %s requested at game frame %llu (\"%s\"; the file is %s; %s): KHNMC_MOVIES=fail, the seam before task 44: %s\n",
                (unsigned long long)n,name,(unsigned long long)frame,win,present?"present":"absent",scene,
                present?"the source reader will fail":nopause?"the game queues no movie and its scene waits on a black screen FOR EVER: nothing ends it":"the game queues no movie and its scene waits (a black screen) for the pause screen's Skip");
}
/* k32.c, FindFirstFileA: 1 = this is a file check of the movie path and it is answered "present" */
int kh_movie_find(KCtx *g,const char *win,const char *host,u64 site_rva){
    if(site_rva!=SITE_REQUEST&&site_rva!=SITE_OPEN) return 0;
    int m=movies_mode();
    if(site_rva==SITE_OPEN){ if(m==MV_END) n_probe_open++; return m==MV_END; }
    if(!in_cmd0) kh_fatal(g,"movie-cmd-hook: the file check of movie command 0 (exe 0x13f2a0) outside the hook of 0x5375d0 (runtime/null_mf.c; Makefile MOVIE_HOOKS): the hook is not in this build or the check has another caller; without it a request the game drops at its guard, and the end of the movie's event, are not seen");
    struct stat st; int present=host&&*host&&stat(host,&st)==0&&S_ISREG(st.st_mode);
    u64 n=++n_req; if(present) n_req_present++;
    u32 pause_off=rd32(g,PAUSE_OFF); if(pause_off&PAUSE_OFF_EVENT) n_req_nopause++;
    snprintf(req_name,sizeof req_name,"%s",leaf(win));
    say_request(stderr,n,req_name,kh_game_frames(),win,present,m,pause_off);
    return m==MV_END;
}
/* ---- movie command 0 and the event's end (the hooks of 0x5375d0 and 0x2cbd80 below; plain values, so the selftest can call them) ---- */
void kh_movie_cmd0_begin(KCtx *g,const char *path){
    (void)g; in_cmd0++; n_cmd0++;
    if(cur.pending) fprintf(stderr,"kh: movie: %s%s: a new movie command 0 at game frame %llu while this movie's event has not ended (requested at game frame %llu): its end will not be reported\n",
                            cur.dropped?"(dropped) ":"",cur.name,(unsigned long long)kh_game_frames(),(unsigned long long)cur.frame);
    memset(&cur,0,sizeof cur); cur.req0=n_req; cur.frame=kh_game_frames(); snprintf(cur.name,sizeof cur.name,"%s",leaf(path?path:""));
}
void kh_movie_cmd0_end(KCtx *g,s32 ret){
    (void)g; if(!in_cmd0) return;
    in_cmd0--; cur.pending=1;
    if(n_req!=cur.req0){                                                                 /* the file check ran: the request has its number and its line */
        cur.serial=n_req;
        if(ret==0){ cur.queued=1; n_queued++; return; }
        n_not_queued++;                                                                  /* -1: the game found no file (KHNMC_MOVIES=fail) and queued nothing */
        if(movies_mode()==MV_END){ n_cmd0_odd++;
            fprintf(stderr,"kh: movie: #%llu %s: movie command 0 answered %d although the file check said \"present\" (KHNMC_MOVIES=end): NOTHING IS QUEUED and the scene will wait for an end no movie reports\n",(unsigned long long)cur.serial,cur.name,(int)ret); }
        return;
    }
    if(ret==0){ cur.dropped=1; n_dropped++;
        fprintf(stderr,"kh: movie: REQUEST DROPPED BY THE GAME: movie command 0 for %s at game frame %llu answered 0 at its own guard (exe 0x537632..0x537649: a player object of an earlier movie still exists; that movie's event ended without the stop command 2, i.e. through the direct branch of 0x2cbd80): no file check, no movie task, NOTHING IS QUEUED (dropped request %llu of this run).  The scene now waits for an end no movie reports; the PC game does the same.  KHNMC_MOVIES does not matter here\n",
                cur.name,(unsigned long long)cur.frame,(unsigned long long)n_dropped);
        return; }
    n_cmd0_odd++;
    fprintf(stderr,"kh: movie: %s: movie command 0 at game frame %llu answered %d without reaching its file check: a path of 0x5375d0 this seam does not know; nothing is queued\n",cur.name,(unsigned long long)cur.frame,(int)ret);
}
/* the hook of 0x2cbd80: site_rva = its caller's return address, already_ended = the event's state is 4 (the call does nothing),
 * tasked = both conditions of its task branch hold (0x2cbdb1..0x2cbdc1) */
void kh_movie_event_end(KCtx *g,u64 site_rva,int already_ended,int tasked){
    (void)g; if(!cur.pending||already_ended) return;
    cur.pending=0;
    char who[200]; if(cur.dropped) snprintf(who,sizeof who,"(dropped request) %s",cur.name); else snprintf(who,sizeof who,"#%llu %s",(unsigned long long)cur.serial,cur.name);
    u64 now=kh_game_frames(), dt=now>=cur.frame?now-cur.frame:0;
    const char *branch=tasked?"its clean-up runs as a task (0x2cbe30: movie command 2 stops the movie and destroys the player)"
                             :"its clean-up runs DIRECTLY (0x2cbe30(0): movie command 2 is skipped, the player object SURVIVES, and the game's guard will drop the next movie request)";
    if(!tasked) n_ev_direct++;
    if(site_rva==SITE_EV_NATURAL){ n_ev_natural++;
        fprintf(stderr,"kh: movie: %s: the event ended by itself at game frame %llu, %llu game frames after the request (natural end: movie command 5 answered \"ended\", exe 0x2cf99a -> 0x2cbd80); %s\n",who,(unsigned long long)now,(unsigned long long)dt,branch); }
    else if(site_rva==SITE_EV_SKIP){ n_ev_skip++;
        fprintf(stderr,"kh: movie: %s: the event was ended by SKIP at game frame %llu, %llu game frames after the request (the event's skip flag 0x140b64f9c, exe 0x2cfa61 -> 0x2cbd80): it did NOT end by itself; %s\n",who,(unsigned long long)now,(unsigned long long)dt,branch); }
    else { n_ev_other++;
        fprintf(stderr,"kh: movie: %s: the event was ended at game frame %llu, %llu game frames after the request, by a caller of 0x2cbd80 that is neither the natural-end nor the skip site of 0x2cf980 (return address RVA 0x%llx): it did NOT end as a movie ends; %s\n",who,(unsigned long long)now,(unsigned long long)dt,(unsigned long long)site_rva,branch); }
}
void orig_f_5375d0(KCtx *);
void f_5375d0(KCtx *g){                                                                  /* (command, argument): only command 0 = start(path) is looked at */
    if((u32)KH_RCX(g)!=0){ orig_f_5375d0(g); return; }
    kh_movie_cmd0_begin(g,KH_RDX(g)?gstr(g,KH_RDX(g)):""); orig_f_5375d0(g); kh_movie_cmd0_end(g,(s32)(u32)KH_RAX(g));
}
void orig_f_2cbd80(KCtx *);
void f_2cbd80(KCtx *g){
    extern void kh_theater_scene_skip(void);
    if(guest_return_addr(g)==GS_BASE+SITE_EV_SKIP) kh_theater_scene_skip();
    if(cur.pending) kh_movie_event_end(g,guest_return_addr(g)-GS_BASE,rd32(g,EV_STATE)==4,rd64(g,EV_TASKED_A)!=0&&rd8(g,EV_TASKED_B)!=0);
    extern int kh_cutscenes_end_pending(KCtx *);
    extern void kh_cutscenes_end_snapshot(KCtx *,int);
    int reason=kh_cutscenes_end_pending(g);
    orig_f_2cbd80(g);
    kh_scene_pack_end();
    if(reason) kh_cutscenes_end_snapshot(g,reason);
}
/* 1 = an empty movie's reader exists (default mode): runtime/threads.c makes SwitchToThread a 1 ms wait on the virtual clock then */
int kh_movie_live(void){ return mv_mode==MV_END&&n_readers>n_ended; }
/* phase_log.c, the hook of Movie::open 0x13da10 (rcx = the player, rdx = the path) */
void kh_movie_open_begin(KCtx *g){ (void)g; if(movies_mode()!=MV_END) return; in_open++; n_open++; }
void kh_movie_open_end(KCtx *g){
    if(!in_open) return;
    in_open--;
    if((u32)KH_RAX(g)){ n_open_ok++; return; }
    kh_fatal(g,"movie-open-failed: #%llu %s: Movie::open (exe 0x13da10) FAILED in the default mode (KHNMC_MOVIES=end; %llu of %llu opens worked before): the player would go to its error state, movie command 5 would never answer \"ended\" and the scene would wait for ever (for ena / sec / sec2 without a pause screen: no Skip).  The empty movie must open: one of the calls of 0x13dd40 / 0x13d260 was refused (the XAudio2 source voice, the sound thread, a media type)",
             (unsigned long long)n_req,req_name,(unsigned long long)n_open_ok,(unsigned long long)n_open);
}
/* crt.c, _sopen_dispatch: a descriptor >= 0 to hand out instead of opening the file (the header read of Movie::open), else -1 */
int kh_movie_open_fd(KCtx *g){
    if(!in_open) return -1;
    static const u8 head[12]={0,0,0,0x20,'f','t','y','p','i','s','o','m'};      /* a 32-byte `ftyp` box, then zeros: the engine reads 0x100 bytes and compares bytes 4..7 */
    u8 b[0x100]; memset(b,0,sizeof b); memcpy(b,head,sizeof head);
    int p[2]; if(pipe(p)) kh_fatal(g,"movie-standin: pipe() failed (%s): the stand-in for the movie's first 0x100 bytes cannot be made",strerror(errno));
    if(write(p[1],b,sizeof b)!=(ssize_t)sizeof b) kh_fatal(g,"movie-standin: the stand-in header could not be written (%s)",strerror(errno));
    close(p[1]); n_standin++; return p[0];
}

/* ---- the strict null objects of the default mode ------------------------------------------------------------------------------- */
static const u8 IID_UNK[16]      ={0,0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
static const u8 IID_DEV1[16]     ={0x29,0xfb,0x4b,0xa0,0xef,0x08,0xd6,0x43,0xa4,0x9c,0xa9,0xbd,0xbd,0xcb,0xe6,0x86};   /* ID3D11Device1 */
static const u8 IID_MTHREAD[16]  ={0x00,0x4e,0x7e,0x9b,0x2c,0x34,0x06,0x41,0xa1,0x9f,0x4f,0x27,0x04,0xf6,0x89,0xf0};   /* ID3D10Multithread */
static const u8 IID_AUDIOMT[16]  ={0xc3,0xad,0xa0,0x26,0x26,0xce,0x72,0x46,0x93,0x04,0x69,0x55,0x2e,0xdd,0x3f,0xaf};   /* IMFAudioMediaType */
static const u8 K_MAJOR[16]      ={0x8e,0xa1,0xeb,0x48,0xc9,0xf8,0x87,0x46,0xbf,0x11,0x0a,0x74,0xc9,0xf9,0x6a,0x8f};   /* MF_MT_MAJOR_TYPE */
static const u8 K_SUBTYPE[16]    ={0x9a,0x4c,0xe3,0xf7,0xe8,0x42,0x14,0x47,0xb7,0x4b,0xcb,0x29,0xd7,0x2c,0x35,0xe5};   /* MF_MT_SUBTYPE */
static const u8 K_FRAME_SIZE[16] ={0x3d,0xc3,0x52,0x16,0xb2,0xd6,0x12,0x40,0xb8,0x34,0x72,0x03,0x08,0x49,0xa3,0x7d};   /* MF_MT_FRAME_SIZE */
static const u8 K_STRIDE[16]     ={0x48,0x4e,0x4b,0x64,0x02,0x1e,0x16,0x45,0xb0,0xeb,0xc0,0x1c,0xa9,0xd4,0x9a,0xc6};   /* MF_MT_DEFAULT_STRIDE */
static const u8 K_FRAME_RATE[16] ={0xe8,0xa2,0x59,0xc4,0x2c,0x3d,0x44,0x4e,0xb1,0x32,0xfe,0xe5,0x15,0x6c,0x7b,0xb0};   /* MF_MT_FRAME_RATE */
static const u8 K_D3D_MANAGER[16]={0xa2,0x2d,0x82,0xec,0xe9,0xe1,0x29,0x4b,0xa0,0xd8,0x56,0x3c,0x71,0x9f,0x52,0x69};   /* MF_SOURCE_READER_D3D_MANAGER */
static const u8 K_HW_XFORMS[16]  ={0x1c,0xa9,0x34,0xa6,0x2b,0x82,0xb9,0x41,0xa4,0x94,0x4d,0xe4,0x64,0x36,0x12,0xb0};   /* MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS */
static const u8 K_ADV_VIDEO[16]  ={0x2c,0xda,0x81,0x0f,0x37,0xb5,0x72,0x46,0xa8,0xb2,0xa6,0x81,0xb1,0x73,0x07,0xa3};   /* MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING */
static const u8 K_DURATION[16]   ={0x33,0x0d,0x99,0x6c,0x8e,0xbb,0x7a,0x47,0x85,0x98,0x0d,0x5d,0x96,0xfc,0xd8,0x8a};   /* MF_PD_DURATION */
static const u8 T_VIDEO[16]      ={0x76,0x69,0x64,0x73,0,0,0x10,0,0x80,0,0,0xaa,0,0x38,0x9b,0x71};
static const u8 T_AUDIO[16]      ={0x61,0x75,0x64,0x73,0,0,0x10,0,0x80,0,0,0xaa,0,0x38,0x9b,0x71};
static const u8 F_NV12[16]       ={0x4e,0x56,0x31,0x32,0,0,0x10,0,0x80,0,0,0xaa,0,0x38,0x9b,0x71};
static const u8 F_RGB32[16]      ={0x16,0,0,0,0,0,0x10,0,0x80,0,0,0xaa,0,0x38,0x9b,0x71};
static const u8 F_FLOAT[16]      ={0x03,0,0,0,0,0,0x10,0,0x80,0,0,0xaa,0,0x38,0x9b,0x71};
/* the empty movie's formats: the frame rate the engine's own constructor presets (0x13cc30: 30.0 pictures a second; its size preset
 * is 1920 x 1080) and the sound format the engine asks for (MFAudioFormat_Float) at the mixer's rate.
 * The picture: the player (0x13d690) makes a width x height x 4 texture from an UNINITIALISED malloc buffer and draws it while it lives
 * (0x473520: a quad whose corners come from the player's own fields +0x11c..+0x128, texture coordinates = the texture's size: the
 * picture is drawn scaled, whatever its size; the two size getters 0x473f60 / 0x473f80 have no caller).  The engine never frees that
 * texture (see the hook of 0x13d690), so the size is as small as it can be and still black: 384 x 216 x 4 = 331,776 bytes is more
 * than the guest arena's 256 KiB size-class limit (core.c BIG_LIMIT), i.e. a page run, which reads as zero fresh or reused; anything
 * at or below the limit would come from a size class, i.e. recycled bytes.  16:9 like the preset; 25 times less than 1920 x 1080. */
#define MOVIE_W 384u
#define MOVIE_H 216u
#define MOVIE_FPS 30u
#define MOVIE_PIC_BYTES ((u64)MOVIE_W*MOVIE_H*4)
_Static_assert(MOVIE_PIC_BYTES>256u*1024u,"the empty movie's picture must be a page run of the guest arena (more than core.c's BIG_LIMIT), or it is not black");
#define STREAM_VIDEO 0xfffffffcu                /* MF_SOURCE_READER_FIRST_VIDEO_STREAM */
#define STREAM_AUDIO 0xfffffffdu                /* MF_SOURCE_READER_FIRST_AUDIO_STREAM */
#define MF_E_INVALIDSTREAMNUMBER 0xC00D36B3u
#define MF_E_NO_MORE_TYPES       0xC00D36B9u
#define MF_E_ATTRIBUTENOTFOUND   0xC00D36E6u
#define E_POINTER_               0x80004003u
#define E_INVALIDARG_            0x80070057u
enum { MT_NEW=0, MT_VNATIVE, MT_ANATIVE, MT_VCURRENT, MT_ACURRENT };
/* media type: +32 kind, +36 bit 0 major set / bit 1 subtype set, +40 major, +56 subtype, +72 WAVEFORMATEX (18 bytes) */
#define MT_EXTRA 64
/* reader: +32 bit 0 video output set / bit 1 audio output set, +40 the video output subtype, +56 video reads, +64 audio reads, +72 serial, +80 name */
#define RD_EXTRA 208
static int E_attr=-1,E_mgr,E_mt,E_reader,E_dev,E_ctx,E_mth;
static pthread_mutex_t em=PTHREAD_MUTEX_INITIALIZER;
static int gis(KCtx *g,u64 p,const u8 *id){ return p&&!memcmp(hp(g,p),id,16); }
static void gname(KCtx *g,u64 p,char *out,size_t cap){
    if(!p){ snprintf(out,cap,"(null)"); return; }
    const u8 *b=hp(g,p); snprintf(out,cap,"{%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x}",b[3],b[2],b[1],b[0],b[5],b[4],b[7],b[6],b[8],b[9],b[10],b[11],b[12],b[13],b[14],b[15]);
}
static void unexpected(KCtx *g,const char *iface,int slot,const char *why,u64 guid) __attribute__((noreturn));
static void unexpected(KCtx *g,const char *iface,int slot,const char *why,u64 guid){
    char gn[48]=""; if(guid) gname(g,guid,gn,sizeof gn);
    kh_fatal(g,"movie-unexpected-call: %s slot %d (vtbl+0x%x): %s%s%s; the empty movie of KHNMC_MOVIES=end serves only the calls of the game's movie path at a movie without a picture (call-site RVA 0x%llx)",
             iface,slot,slot*8,why,guid?" ":"",gn,(unsigned long long)(guest_return_addr(g)-GS_BASE));
}
static int qi_same(KCtx *g,u64 o){ u64 pp=A2(g); if(!pp){ SET_RAX(g,E_POINTER_); return 1; } wr64(g,pp,o); kh_com_ref_add(g,o); SET_RAX(g,0); return 1; }
static u64 new_mt(KCtx *g,int kind,const u8 *major,const u8 *sub){
    u64 o=kh_com_obj(E_mt,MT_EXTRA); u32 have=0; wr32(g,o+32,(u32)kind);
    if(major){ memcpy(hp(g,o+40),major,16); have|=1; } if(sub){ memcpy(hp(g,o+56),sub,16); have|=2; } wr32(g,o+36,have);
    if(kind==MT_ACURRENT){ u64 w=o+72; wr16(g,w,3); wr16(g,w+2,2); wr32(g,w+4,48000); wr32(g,w+8,384000); wr16(g,w+12,8); wr16(g,w+14,32); wr16(g,w+16,0); }   /* IEEE float, 2 channels, 48000 Hz */
    return o;
}
static int h_mt(KCtx *g,u64 o,int s,const char *c){
    u32 kind=rd32(g,o+32), have=rd32(g,o+36); u64 key=A1(g), out=A2(g);
    switch(s){
    case 1: case 2: return 0;
    case 0: if(gis(g,key,IID_UNK)||(kind==MT_ACURRENT&&gis(g,key,IID_AUDIOMT))) return qi_same(g,o);
            unexpected(g,c,s,"QueryInterface for an interface this media type is not asked for by the movie path",key);
    case 7: if(kind==MT_VCURRENT&&gis(g,key,K_STRIDE)){ if(!out){ SET_RAX(g,E_POINTER_); return 1; } wr32(g,out,MOVIE_W); SET_RAX(g,0); return 1; }          /* GetUINT32 */
            unexpected(g,c,s,"GetUINT32 of an attribute the empty movie's media types do not have:",key);
    case 8: if(kind==MT_VCURRENT&&(gis(g,key,K_FRAME_SIZE)||gis(g,key,K_FRAME_RATE))){ if(!out){ SET_RAX(g,E_POINTER_); return 1; }                      /* GetUINT64 */
                wr64(g,out,gis(g,key,K_FRAME_SIZE)?((u64)MOVIE_W<<32|MOVIE_H):((u64)MOVIE_FPS<<32|1)); SET_RAX(g,0); return 1; }
            unexpected(g,c,s,"GetUINT64 of an attribute the empty movie's media types do not have:",key);
    case 10: { int k=gis(g,key,K_MAJOR)?1:gis(g,key,K_SUBTYPE)?2:0;                                                                             /* GetGUID */
            if(!k) unexpected(g,c,s,"GetGUID of an attribute that is neither the major type nor the subtype:",key);
            if(!out){ SET_RAX(g,E_POINTER_); return 1; }
            if(!(have&(u32)k)){ SET_RAX(g,MF_E_ATTRIBUTENOTFOUND); return 1; }                   /* the stand-in has no stream format: a native type has a major type only */
            memcpy(hp(g,out),hp(g,o+(k==1?40:56)),16); SET_RAX(g,0); return 1; }
    case 24: { int k=gis(g,key,K_MAJOR)?1:gis(g,key,K_SUBTYPE)?2:0;                                                                             /* SetGUID */
            if(kind!=MT_NEW||!k) unexpected(g,c,s,kind!=MT_NEW?"SetGUID on a media type the reader handed out":"SetGUID of an attribute that is neither the major type nor the subtype:",kind!=MT_NEW?0:key);
            if(!out){ SET_RAX(g,E_POINTER_); return 1; }
            memcpy(hp(g,o+(k==1?40:56)),hp(g,out),16); wr32(g,o+36,have|(u32)k); SET_RAX(g,0); return 1; }
    case 38: if(kind==MT_ACURRENT){ SET_RAX(g,o+72); return 1; }                                                                                /* IMFAudioMediaType::GetAudioFormat */
            unexpected(g,c,s,"GetAudioFormat on a media type that is not the reader's current audio type",0);
    }
    unexpected(g,c,s,"a method of IMFMediaType / IMFAudioMediaType the movie path does not call",0);
}
static int h_attr(KCtx *g,u64 o,int s,const char *c){
    u64 key=A1(g);
    switch(s){
    case 1: case 2: return 0;
    case 0: if(gis(g,key,IID_UNK)) return qi_same(g,o); unexpected(g,c,s,"QueryInterface for an interface other than IUnknown:",key);
    case 21: if(gis(g,key,K_HW_XFORMS)||gis(g,key,K_ADV_VIDEO)){ wr32(g,o+32,rd32(g,o+32)+1); SET_RAX(g,0); return 1; }                           /* SetUINT32 */
             unexpected(g,c,s,"SetUINT32 of an attribute that is not one of the two the movie path sets:",key);
    case 27: if(gis(g,key,K_D3D_MANAGER)){ wr64(g,o+40,A2(g)); SET_RAX(g,0); return 1; }                                                          /* SetUnknown */
             unexpected(g,c,s,"SetUnknown of an attribute that is not MF_SOURCE_READER_D3D_MANAGER:",key);
    }
    unexpected(g,c,s,"a method of IMFAttributes the movie path does not call",0);
}
static int h_mgr(KCtx *g,u64 o,int s,const char *c){
    switch(s){
    case 1: case 2: return 0;
    case 0: if(gis(g,A1(g),IID_UNK)) return qi_same(g,o); unexpected(g,c,s,"QueryInterface for an interface other than IUnknown:",A1(g));
    case 7: if((u32)A2(g)!=rd32(g,o+32)||!A1(g)){ SET_RAX(g,E_INVALIDARG_); return 1; } wr64(g,o+40,A1(g)); SET_RAX(g,0); return 1;                 /* ResetDevice(device, token) */
    }
    unexpected(g,c,s,"a method of IMFDXGIDeviceManager the movie path does not call",0);
}
static int h_reader(KCtx *g,u64 o,int s,const char *c){
    u32 stream=(u32)A1(g), set=rd32(g,o+32);
    switch(s){
    case 1: return 0;
    case 2: if(COM_REFS(g,o)==1){ u64 n=++n_ended;                                     /* the PLAYER's teardown (0x13cf90 releases the reader last): after a natural end, a Skip or a soft reset alike */
                fprintf(stderr,"kh: movie: #%llu %s: player torn down: reader released at game frame %llu after %llu video and %llu audio reads, each answered end of stream with no sample (reader %llu of %llu; how the movie's EVENT ended is the line `the event ended ...`)\n",
                        (unsigned long long)rd64(g,o+72),(const char*)hp(g,o+80),(unsigned long long)kh_game_frames(),(unsigned long long)rd64(g,o+56),(unsigned long long)rd64(g,o+64),(unsigned long long)n,(unsigned long long)n_readers); }
            return 0;
    case 0: if(gis(g,A1(g),IID_UNK)) return qi_same(g,o); unexpected(g,c,s,"QueryInterface for an interface other than IUnknown:",A1(g));
    case 5: { u64 pp=A3(g); if(!pp){ SET_RAX(g,E_POINTER_); return 1; } wr64(g,pp,0);                                                           /* GetNativeMediaType(stream, index, **type) */
            if(stream!=STREAM_VIDEO&&stream!=STREAM_AUDIO){ SET_RAX(g,MF_E_INVALIDSTREAMNUMBER); return 1; }
            if((u32)A2(g)!=0){ SET_RAX(g,MF_E_NO_MORE_TYPES); return 1; }
            wr64(g,pp,new_mt(g,stream==STREAM_VIDEO?MT_VNATIVE:MT_ANATIVE,stream==STREAM_VIDEO?T_VIDEO:T_AUDIO,NULL)); SET_RAX(g,0); return 1; }
    case 6: { u64 pp=A2(g); if(!pp){ SET_RAX(g,E_POINTER_); return 1; } wr64(g,pp,0);                                                           /* GetCurrentMediaType(stream, **type) */
            if(stream!=STREAM_VIDEO&&stream!=STREAM_AUDIO){ SET_RAX(g,MF_E_INVALIDSTREAMNUMBER); return 1; }
            if(!(set&(stream==STREAM_VIDEO?1u:2u))) unexpected(g,c,s,"GetCurrentMediaType of a stream whose output type the game has not set (the movie path sets it first)",0);
            wr64(g,pp,stream==STREAM_VIDEO?new_mt(g,MT_VCURRENT,T_VIDEO,hp(g,o+40)):new_mt(g,MT_ACURRENT,T_AUDIO,F_FLOAT)); SET_RAX(g,0); return 1; }
    case 7: { u64 t=A3(g);                                                                                                                         /* SetCurrentMediaType(stream, reserved, type) */
            if(stream!=STREAM_VIDEO&&stream!=STREAM_AUDIO){ SET_RAX(g,MF_E_INVALIDSTREAMNUMBER); return 1; }
            if(!t||rd64(g,t)!=kh_com_vtbl(E_mt)||(rd32(g,t+36)&3)!=3) unexpected(g,c,s,"SetCurrentMediaType with a media type that is not one of MFCreateMediaType with a major type and a subtype",0);
            if(stream==STREAM_VIDEO){
                if(!gis(g,t+40,T_VIDEO)||(!gis(g,t+56,F_NV12)&&!gis(g,t+56,F_RGB32))) unexpected(g,c,s,"SetCurrentMediaType of the video stream to something other than NV12 or RGB32 video, subtype",t+56);
                memcpy(hp(g,o+40),hp(g,t+56),16); wr32(g,o+32,set|1);
            } else {
                if(!gis(g,t+40,T_AUDIO)||!gis(g,t+56,F_FLOAT)) unexpected(g,c,s,"SetCurrentMediaType of the audio stream to something other than float PCM audio, subtype",t+56);
                wr32(g,o+32,set|2);
            }
            SET_RAX(g,0); return 1; }
    case 9: { u64 pa=A3(g), pf=ARG(g,4), pt=ARG(g,5), ps=ARG(g,6);                                                                              /* ReadSample(stream, control, *actual, *flags, *time, **sample) */
            if(stream!=STREAM_VIDEO&&stream!=STREAM_AUDIO){ SET_RAX(g,MF_E_INVALIDSTREAMNUMBER); return 1; }
            if(stream==STREAM_VIDEO){ wr64(g,o+56,rd64(g,o+56)+1); n_vreads++; } else { wr64(g,o+64,rd64(g,o+64)+1); n_areads++; }
            if(pa) wr32(g,pa,stream==STREAM_VIDEO?0:1); if(pf) wr32(g,pf,2); if(pt) wr64(g,pt,0); if(ps) wr64(g,ps,0);      /* MF_SOURCE_READERF_ENDOFSTREAM, no sample */
            SET_RAX(g,0); return 1; }
    case 12: { u64 pv=A3(g);                                                                                                                       /* GetPresentationAttribute(stream, guid, *PROPVARIANT) */
            if(stream!=0xffffffffu||!gis(g,A2(g),K_DURATION)) unexpected(g,c,s,"GetPresentationAttribute of something other than the media source's MF_PD_DURATION:",A2(g));
            if(!pv){ SET_RAX(g,E_POINTER_); return 1; }
            memset(hp(g,pv),0,24); wr16(g,pv,21); wr64(g,pv+8,0); n_duration++; SET_RAX(g,0); return 1; }                     /* VT_UI8, 0: the empty movie has no length */
    }
    unexpected(g,c,s,"a method of IMFSourceReader the movie path does not call at a movie without a picture (stream selection, seek, flush, service)",0);
}
static int h_dev(KCtx *g,u64 o,int s,const char *c){
    switch(s){
    case 1: case 2: return 0;
    case 0: if(gis(g,A1(g),IID_UNK)||gis(g,A1(g),IID_DEV1)) return qi_same(g,o);
            if(gis(g,A1(g),IID_MTHREAD)){ u64 pp=A2(g); if(!pp){ SET_RAX(g,E_POINTER_); return 1; } wr64(g,pp,kh_com_obj(E_mth,16)); SET_RAX(g,0); return 1; }
            unexpected(g,c,s,"QueryInterface for an interface other than ID3D11Device1 and ID3D10Multithread:",A1(g));
    case 40: if(A1(g)) wr64(g,A1(g),kh_com_obj(E_ctx,16)); return 1;                                                                            /* GetImmediateContext(**context): void */
    }
    unexpected(g,c,s,"a method of ID3D11Device the movie path calls only for a decoded picture (the empty movie has none)",0);
}
static int h_ctx(KCtx *g,u64 o,int s,const char *c){
    if(s==1||s==2) return 0;
    if(s==0){ if(gis(g,A1(g),IID_UNK)) return qi_same(g,o); unexpected(g,c,s,"QueryInterface for an interface other than IUnknown:",A1(g)); }
    unexpected(g,c,s,"a method of ID3D11DeviceContext the movie path calls only for a decoded picture (the empty movie has none)",0);
}
static int h_mth(KCtx *g,u64 o,int s,const char *c){
    if(s==1||s==2) return 0;
    if(s==0){ if(gis(g,A1(g),IID_UNK)) return qi_same(g,o); unexpected(g,c,s,"QueryInterface for an interface other than IUnknown:",A1(g)); }
    if(s==5){ u32 was=rd32(g,o+32); wr32(g,o+32,(u32)A1(g)?1:0); SET_RAX(g,was); return 1; }                                                 /* SetMultithreadProtected(BOOL): the state before */
    unexpected(g,c,s,"a method of ID3D10Multithread the movie path does not call",0);
}
/* ---- the player's picture (the hook of exe 0x13d690; default mode only) ----------------------------------------------------------
 * 0x13d690(inner): w = [impl+0x24], h = [impl+0x28] (the reader's MF_MT_FRAME_SIZE); malloc(w*h*4); 0x12a200(texture manager, {buffer,
 * w, h}) -> a GX texture named "no_name<N>.png"; [inner+0x18] = it; free(buffer); return it (the player keeps it in [player]).
 * Nothing of the engine frees it: the player's teardown (0x473c4e..0x473c5c, 0x473b30, 0x473da0) clears or frees only the holders,
 * and the manager's collector (0x12ba80) takes a texture only after 0x12b580 released it by name, which the movie code never calls.
 * So the PC keeps one picture of the movie's size per movie played, and so did this port (8,294,400 bytes each at 1920 x 1080).
 * Here the first empty movie's picture is kept and handed out again for every later one, as long as the manager still lists that
 * very texture with its name and its size (else the engine makes a new one, and that is said).  The picture is black and nothing
 * ever writes to it (0x13e7f0 uploads only when there is a sample).  A real movie player must remove this hook's reuse. */
static u64 pic_tex; static char pic_name[40];
static int pic_listed(KCtx *g,u64 tex){                                                  /* is `tex` an entry of the manager's list? */
    u64 mgr=rd64(g,TEX_MGR); if(!mgr||!tex) return 0;
    u64 n=0; for(u64 t=rd64(g,mgr+0x18);t&&n<1000000;t=rd64(g,t+8),n++) if(t==tex) return 1;
    return 0;
}
static int pic_same(KCtx *g,u64 tex){ return rd16(g,tex+0x94)==MOVIE_W&&rd16(g,tex+0x96)==MOVIE_H&&!strncmp((const char*)hp(g,tex+0x14),pic_name,sizeof pic_name); }
/* the decision alone (the selftest calls it with a made-up manager): 1 = `*out` is the kept picture and it is handed out again */
int kh_movie_picture_reuse(KCtx *g,u64 *out){
    if(!pic_tex) return 0;
    if(pic_listed(g,pic_tex)&&pic_same(g,pic_tex)){ n_pic_reused++; *out=pic_tex; return 1; }
    n_pic_lost++;
    fprintf(stderr,"kh: movie: the kept picture (texture 0x%llx \"%s\") is no longer in the texture manager's list with its name and size: the engine freed or replaced it; the engine makes a new picture for this movie (kept pictures lost so far: %llu)\n",
            (unsigned long long)pic_tex,pic_name,(unsigned long long)n_pic_lost);
    pic_tex=0; return 0;
}
/* a picture the engine just made: kept for the next movies when it is what this seam believes it is */
void kh_movie_picture_made(KCtx *g,u64 tex){
    if(!tex) return;
    n_pic_made++;
    if(pic_listed(g,tex)&&rd16(g,tex+0x94)==MOVIE_W&&rd16(g,tex+0x96)==MOVIE_H&&rd8(g,tex+0x14)){ pic_tex=tex; memset(pic_name,0,sizeof pic_name); strncpy(pic_name,(const char*)hp(g,tex+0x14),sizeof pic_name-1); return; }
    n_pic_uncached++; pic_tex=0;
    fprintf(stderr,"kh: movie: the picture the engine made (texture 0x%llx) is not an entry of the texture manager's list with a name and the size %u x %u (size words %u x %u): it is NOT kept for the next movie; every movie leaves its own picture behind, as on the PC (%llu so far)\n",
            (unsigned long long)tex,MOVIE_W,MOVIE_H,(unsigned)rd16(g,tex+0x94),(unsigned)rd16(g,tex+0x96),(unsigned long long)n_pic_uncached);
}
void orig_f_13d690(KCtx *);
void f_13d690(KCtx *g){                                                                  /* rcx = the inner movie object; returns the picture */
    if(movies_mode()!=MV_END){ orig_f_13d690(g); return; }
    u64 inner=KH_RCX(g), tex=0;
    if(kh_movie_picture_reuse(g,&tex)){ wr64(g,inner+0x18,tex); SET_RAX(g,tex); KH_RET(g); return; }   /* the engine's function did not run: pop the return address like its `ret` (without it the caller's stack is 8 bytes off: seen, the second movie of a run faulted) */
    orig_f_13d690(g); kh_movie_picture_made(g,KH_RAX(g));
}
static void end_classes(void){
    pthread_mutex_lock(&em);
    if(E_attr<0){
        E_mgr=kh_com_class("mf:end:IMFDXGIDeviceManager",10,COM_STRICT|COM_FREE,h_mgr); E_mt=kh_com_class("mf:end:IMFMediaType",39,COM_STRICT|COM_FREE,h_mt);
        E_reader=kh_com_class("mf:end:IMFSourceReader",13,COM_STRICT|COM_FREE,h_reader); E_dev=kh_com_class("d3d11:end:ID3D11Device",50,COM_STRICT|COM_FREE,h_dev);
        E_ctx=kh_com_class("d3d11:end:ID3D11DeviceContext",115,COM_STRICT|COM_FREE,h_ctx); E_mth=kh_com_class("d3d11:end:ID3D10Multithread",6,COM_STRICT|COM_FREE,h_mth);
        E_attr=kh_com_class("mf:end:IMFAttributes",33,COM_STRICT|COM_FREE,h_attr);
    }
    pthread_mutex_unlock(&em);
}
static int end_mode(void){ if(movies_mode()!=MV_END) return 0; if(E_attr<0) end_classes(); return 1; }

IMP(mfplat,MFStartup){ kh_cnt_mf_calls++; SET_RAX(g,0); }
IMP(mfplat,MFShutdown){ kh_cnt_mf_calls++; SET_RAX(g,0); }
IMP(mfplat,MFCreateAttributes){ kh_cnt_mf_calls++; wr64(g,A0(g),kh_com_obj(end_mode()?E_attr:C_attr,16)); SET_RAX(g,0); }
IMP(mfplat,MFCreateDXGIDeviceManager){ kh_cnt_mf_calls++; wr32(g,A0(g),1);
    if(end_mode()){ u64 o=kh_com_obj(E_mgr,16); wr32(g,o+32,1); wr64(g,A1(g),o); } else wr64(g,A1(g),kh_com_obj(C_mgr,16));
    SET_RAX(g,0); }
IMP(mfplat,MFCreateMediaType){ kh_cnt_mf_calls++; wr64(g,A0(g),end_mode()?new_mt(g,MT_NEW,NULL,NULL):kh_com_obj(C_mt,16)); SET_RAX(g,0); }
IMP(mfplat,MFllMulDiv){ kh_cnt_mf_calls++; __int128 p=(__int128)(s64)A0(g)*(s64)A1(g)+(s64)A3(g); s64 c=(s64)A2(g); SET_RAX(g,c?(u64)(s64)(p/c):0); }
IMP(mfreadwrite,MFCreateSourceReaderFromURL){ kh_cnt_mf_calls++;                     /* (url, attributes, **reader) */
    if(!end_mode()){ n_fail_readers++; wr64(g,A2(g),0); SET_RAX(g,0xC00D36C4u); return; }                                            /* MF_E_UNSUPPORTED_BYTESTREAM_TYPE, as before task 44 */
    if(!A2(g)){ SET_RAX(g,E_POINTER_); return; }
    if(!in_open) kh_fatal(g,"movie-open-hook: MFCreateSourceReaderFromURL outside Movie::open (0x13da10): the hook of 0x13da10 (phase_log.c) is not in this build or the reader has another caller; without it the header read of the open is not the stand-in");
    u64 o=kh_com_obj(E_reader,RD_EXTRA); char u[600]; u[0]=0; if(A0(g)) gw_to_utf8(g,A0(g),u,sizeof u);
    wr64(g,o+72,n_req); snprintf((char*)hp(g,o+80),RD_EXTRA-48,"%s",leaf(u)); n_readers++;
    wr64(g,A2(g),o); SET_RAX(g,0); }
IMP(d3d11,D3D11CreateDevice){ kh_cnt_mf_calls++; /* (adapter, type, sw, flags, levels*, n, sdk, **dev, *level, **ctx) */
    if(end_mode()){ n_d3d11++; if(ARG(g,7)) wr64(g,ARG(g,7),kh_com_obj(E_dev,16)); if(ARG(g,8)) wr32(g,ARG(g,8),0xb100); if(ARG(g,9)) wr64(g,ARG(g,9),kh_com_obj(E_ctx,16)); SET_RAX(g,0); return; }
    if(ARG(g,7)) wr64(g,ARG(g,7),kh_com_obj(C_d3d11,16)); if(ARG(g,8)) wr32(g,ARG(g,8),0xb100); if(ARG(g,9)) wr64(g,ARG(g,9),kh_com_obj(C_ctx,16)); SET_RAX(g,0); }

void kh_movies_report(FILE *f){
    u64 r=n_req, d=n_dropped; if(!r&&!d) return;
    u64 ev=n_ev_natural+n_ev_skip+n_ev_other, open_ev=(r+d)>ev?(r+d)-ev:0;
    char evs[420]; snprintf(evs,sizeof evs,"events: %llu ended by themselves (natural end), %llu by Skip, %llu otherwise, %llu not ended when the run ended; %llu of the ended through the direct branch of 0x2cbd80 (the player survives)",
                            (unsigned long long)n_ev_natural,(unsigned long long)n_ev_skip,(unsigned long long)n_ev_other,(unsigned long long)open_ev,(unsigned long long)n_ev_direct);
    if(mv_mode==MV_FAIL){ fprintf(f,"kh: movies: mode fail (the seam before task 44): %llu requested (%llu with the file present, %llu absent: the scene of each waited on a black screen; %llu of the requests in a scene WITHOUT a pause screen, where nothing ends the wait), %llu DROPPED by the game's own guard; the source reader was refused %llu times; %s\n",
                                  (unsigned long long)r,(unsigned long long)n_req_present,(unsigned long long)(r-n_req_present),(unsigned long long)n_req_nopause,(unsigned long long)d,(unsigned long long)n_fail_readers,evs); return; }
    int good=n_queued==r&&n_ev_natural==r&&!d&&!n_ev_skip&&!n_ev_other&&!n_ev_direct&&!n_cmd0_odd&&n_ended==n_readers&&n_readers==r;
    fprintf(f,"kh: movies: mode end: %llu requested (%llu with the file present, %llu absent; %llu in a scene without a pause screen), %llu queued, %llu DROPPED by the game's own guard; %llu of %llu opens worked (%llu stand-in headers read, %llu D3D11 devices), %llu readers: %llu video and %llu audio reads, all end of stream; "
              "%llu players torn down (reader released); the length was asked %llu times; %s; pictures: %llu made by the engine (%u x %u, %llu bytes each, never freed by it), %llu times the kept one handed out again, %llu kept ones lost, %llu not kept%s\n",
            (unsigned long long)r,(unsigned long long)n_req_present,(unsigned long long)(r-n_req_present),(unsigned long long)n_req_nopause,(unsigned long long)n_queued,(unsigned long long)d,
            (unsigned long long)n_open_ok,(unsigned long long)n_open,(unsigned long long)n_standin,(unsigned long long)n_d3d11,
            (unsigned long long)n_readers,(unsigned long long)n_vreads,(unsigned long long)n_areads,(unsigned long long)n_ended,(unsigned long long)n_duration,evs,
            (unsigned long long)n_pic_made,MOVIE_W,MOVIE_H,(unsigned long long)MOVIE_PIC_BYTES,(unsigned long long)n_pic_reused,(unsigned long long)n_pic_lost,(unsigned long long)n_pic_uncached,
            good?"":"; NOT every requested movie ended by itself (the run ended inside one, one was dropped, skipped or ended another way: see the `kh: movie:` lines)");
}

/* ---- selftest ------------------------------------------------------------------------------------------------------------------ */
static int checks,bad;
#define T(c) do{ checks++; if(!(c)){ bad++; fprintf(stderr,"movies selftest FAIL: %s (line %d)\n",#c,__LINE__); } }while(0)
extern void rt_imp_kernel32__FindFirstFileA(KCtx *g); extern void rt_imp_kernel32__FindClose(KCtx *g); extern void rt_imp_kernel32__FindNextFileA(KCtx *g);
extern void rt_imp_api_ms_win_crt_stdio_l1_1_0___sopen_dispatch(KCtx *g); extern void rt_imp_api_ms_win_crt_stdio_l1_1_0___read(KCtx *g); extern void rt_imp_api_ms_win_crt_stdio_l1_1_0___close(KCtx *g);
/* a guest call with up to 10 arguments; `ret` is the return address the callee sees (an import's call site) */
static u64 gcall(KCtx *g,void (*imp)(KCtx*),u64 target,u64 ret,int n,const u64 *a){
    u64 sv[5]={KH_RSP(g),KH_RCX(g),KH_RDX(g),KH_R8(g),KH_R9(g)}, sp=((sv[0]-0x200)&~15ull);
    for(int i=4;i<n;i++) wr64(g,sp+8ull*(u64)i,a[i]);
    sp-=8; wr64(g,sp,ret); KH_RSP(g)=sp; KH_RCX(g)=n>0?a[0]:0; KH_RDX(g)=n>1?a[1]:0; KH_R8(g)=n>2?a[2]:0; KH_R9(g)=n>3?a[3]:0;
    if(imp) imp(g); else kh_dispatch(g,target);
    u64 r=KH_RAX(g); KH_RSP(g)=sv[0]; KH_RCX(g)=sv[1]; KH_RDX(g)=sv[2]; KH_R8(g)=sv[3]; KH_R9(g)=sv[4]; return r;
}
#define SENT 0x7ff7ffff0000ull
#define IMPC(imp,ret,...) gcall(g,imp,0,ret,(int)(sizeof((u64[]){__VA_ARGS__})/8),(u64[]){__VA_ARGS__})
#define M(obj,slot,...) gcall(g,NULL,rd64(g,rd64(g,(obj))+8ull*(slot)),SENT,(int)(sizeof((u64[]){(obj),##__VA_ARGS__})/8),(u64[]){(obj),##__VA_ARGS__})
static u64 gguid(KCtx *g,const u8 *id){ u64 p=ga_alloc(16,16); memcpy(hp(g,p),id,16); return p; }
static void test_mode(const char *v){ if(v) setenv("KHNMC_MOVIES",v,1); else unsetenv("KHNMC_MOVIES"); mv_mode=MV_UNSET; }
/* the game's own sequence at one movie (Movie::open 0x13da10 -> 0x13ef00, 0x13dd40, 0x13d500, 0x13d260; first pump 0x13dfc0; teardown
 * 0x13cf90), every answer checked; `what` selects a wrong call for the fatal cases (0 = none) */
static void engine_sequence(KCtx *g,int what){
    u64 s=ga_calloc(1,256), freed0=kh_com_freed();
    u64 G_unk=gguid(g,IID_UNK),G_dev1=gguid(g,IID_DEV1),G_mth=gguid(g,IID_MTHREAD),G_amt=gguid(g,IID_AUDIOMT),G_major=gguid(g,K_MAJOR),G_sub=gguid(g,K_SUBTYPE),G_fs=gguid(g,K_FRAME_SIZE),
        G_st=gguid(g,K_STRIDE),G_fr=gguid(g,K_FRAME_RATE),G_mgr=gguid(g,K_D3D_MANAGER),G_hw=gguid(g,K_HW_XFORMS),G_adv=gguid(g,K_ADV_VIDEO),G_dur=gguid(g,K_DURATION),G_nv12=gguid(g,F_NV12),G_float=gguid(g,F_FLOAT);
    /* 0x13ef00: D3D11CreateDevice(adapter, 0, 0, 0x820, levels, 2, 7, &dev, NULL, NULL); QI ID3D11Device1; QI ID3D10Multithread; SetMultithreadProtected(1); GetImmediateContext */
    u64 r=IMPC(rt_imp_d3d11__D3D11CreateDevice,SENT,0,0,0,0x820,s+200,2,7,s,0,0); u64 dev=rd64(g,s); T((u32)r==0&&dev&&COM_REFS(g,dev)==1);
    r=M(dev,0,G_dev1,s+8); u64 dev1=rd64(g,s+8); T((u32)r==0&&dev1==dev&&COM_REFS(g,dev)==2);
    r=M(dev1,0,G_mth,s+16); u64 mth=rd64(g,s+16); T((u32)r==0&&mth&&mth!=dev);
    T((u32)M(mth,5,1)==0); T((u32)M(mth,5,0)==1);                                    /* the state before: FALSE, then TRUE */
    T(M(mth,2)==0);
    wr64(g,s+24,0); M(dev1,40,s+24); u64 ctx=rd64(g,s+24); T(ctx&&ctx!=dev);
    if(what==1) M(dev,5,0,0,s+32);                                                   /* CreateTexture2D: only for a decoded picture */
    if(what==2) M(ctx,46,0,0,0,0);                                                   /* CopySubresourceRegion */
    /* 0x13dd40: device manager, attributes, reader */
    wr32(g,s+40,0); r=IMPC(rt_imp_mfplat__MFCreateDXGIDeviceManager,SENT,s+40,s+48); u64 mgr=rd64(g,s+48); u32 token=rd32(g,s+40); T((u32)r==0&&mgr&&token==1);
    T((u32)M(mgr,7,dev1,token+1)==E_INVALIDARG_); T((u32)M(mgr,7,dev1,token)==0);
    r=M(mgr,0,G_unk,s+56); T((u32)r==0&&rd64(g,s+56)==mgr&&COM_REFS(g,mgr)==2);
    r=IMPC(rt_imp_mfplat__MFCreateAttributes,SENT,s+64,3); u64 at=rd64(g,s+64); T((u32)r==0&&at);
    T((u32)M(at,27,G_mgr,mgr)==0); T((u32)M(at,21,G_hw,1)==0); T((u32)M(at,21,G_adv,1)==0);
    if(what==3) M(at,21,G_fs,1);
    u64 url=ga_alloc(64,16); utf8_to_gw(g,url,32,"C:/x/zmovie/en/selftest.mp4"); u64 nr0=n_readers;
    T(kh_movie_live()==0);                                                           /* no reader yet: SwitchToThread is the plain yield */
    if(what!=10) kh_movie_open_begin(g);                                             /* the reader is made inside Movie::open (the hook of 0x13da10) */
    r=IMPC(rt_imp_mfreadwrite__MFCreateSourceReaderFromURL,SENT,url,at,s+72); u64 rd=rd64(g,s+72); T((u32)r==0&&rd&&n_readers==nr0+1&&!strcmp((const char*)hp(g,rd+80),"selftest.mp4"));
    T(kh_movie_live()==1);                                                           /* from here to the reader's last Release a yield is 1 ms on the virtual clock */
    /* 0x13d500 for the video stream: native type, its major type, a new type with major + NV12, SetCurrentMediaType */
    T((u32)M(rd,5,STREAM_VIDEO,1,s+80)==MF_E_NO_MORE_TYPES&&rd64(g,s+80)==0); T((u32)M(rd,5,7,0,s+80)==MF_E_INVALIDSTREAMNUMBER);
    r=M(rd,5,STREAM_VIDEO,0,s+80); u64 nat=rd64(g,s+80); T((u32)r==0&&nat);
    r=M(nat,10,G_major,s+96); T((u32)r==0&&!memcmp(hp(g,s+96),T_VIDEO,16)); T((u32)M(nat,10,G_sub,s+96)==MF_E_ATTRIBUTENOTFOUND);
    r=IMPC(rt_imp_mfplat__MFCreateMediaType,SENT,s+88); u64 nt=rd64(g,s+88); T((u32)r==0&&nt);
    T((u32)M(nt,10,G_major,s+112)==MF_E_ATTRIBUTENOTFOUND);
    T((u32)M(nt,24,G_major,s+96)==0); T((u32)M(nt,24,G_sub,G_nv12)==0);
    if(what==4) M(rd,6,STREAM_VIDEO,s+128);                                          /* current type before it was set */
    if(what==5){ M(nt,24,G_sub,G_float); M(rd,7,STREAM_VIDEO,0,nt); }                /* a video output that is not NV12 / RGB32 */
    T((u32)M(rd,7,STREAM_VIDEO,0,nt)==0); T(M(nt,2)==0); T(M(nat,2)==0);
    /* 0x13dd40: the current video type: subtype, frame size, default stride, frame rate */
    r=M(rd,6,STREAM_VIDEO,s+128); u64 vcur=rd64(g,s+128); T((u32)r==0&&vcur);
    r=M(vcur,10,G_sub,s+112); T((u32)r==0&&!memcmp(hp(g,s+112),F_NV12,16));
    r=M(vcur,8,G_fs,s+136); T((u32)r==0&&(rd64(g,s+136)>>32)==384&&(u32)rd64(g,s+136)==216);          /* the small picture: 384 x 216 (review of task 44) */
    r=M(vcur,7,G_st,s+144); T((u32)r==0&&rd32(g,s+144)==384);
    r=M(vcur,8,G_fr,s+136); T((u32)r==0&&(rd64(g,s+136)>>32)==30&&(u32)rd64(g,s+136)==1);
    if(what==6) M(vcur,8,G_dur,s+136);
    if(what==7) M(vcur,0,G_amt,s+136);                                                /* the video type is no IMFAudioMediaType */
    T(M(vcur,2)==0);
    /* 0x13d260: the audio stream: native, new type with major + float, set; current type -> IMFAudioMediaType -> GetAudioFormat */
    r=M(rd,5,STREAM_AUDIO,0,s+80); nat=rd64(g,s+80); T((u32)r==0&&nat); r=M(nat,10,G_major,s+96); T((u32)r==0&&!memcmp(hp(g,s+96),T_AUDIO,16));
    IMPC(rt_imp_mfplat__MFCreateMediaType,SENT,s+88); nt=rd64(g,s+88); T((u32)M(nt,24,G_major,s+96)==0); T((u32)M(nt,24,G_sub,G_float)==0);
    T((u32)M(rd,7,STREAM_AUDIO,0,nt)==0); T(M(nt,2)==0); T(M(nat,2)==0);
    r=M(rd,6,STREAM_AUDIO,s+128); u64 amt=rd64(g,s+128); T((u32)r==0&&amt);
    r=M(amt,0,G_amt,s+152); T((u32)r==0&&rd64(g,s+152)==amt&&COM_REFS(g,amt)==2);
    u64 wf=M(amt,38); T(wf==amt+72&&rd16(g,wf)==3&&rd16(g,wf+2)==2&&rd32(g,wf+4)==48000&&rd32(g,wf+8)==384000&&rd16(g,wf+12)==8&&rd16(g,wf+14)==32&&rd16(g,wf+16)==0);
    T(M(amt,2)==1);
    T(M(at,2)==0); T(M(mgr,2)==1); T(M(mgr,2)==0);
    { u64 ax=KH_RAX(g); KH_RAX(g)=what==11?0:1; kh_movie_open_end(g); KH_RAX(g)=ax; }  /* Movie::open returns 1 (what 11: it fails = the named fatal movie-open-failed) */
    /* 0x13dfc0, the first pump: ReadSample(audio), ReadSample(video): end of stream, no sample; again: the same */
    for(int k=0;k<2;k++){
        wr32(g,s+160,77); wr32(g,s+164,0); wr64(g,s+168,99); wr64(g,s+176,0x1234);
        r=M(rd,9,STREAM_AUDIO,0,s+160,s+164,s+168,s+176); T((u32)r==0&&rd32(g,s+160)==1&&rd32(g,s+164)==2&&rd64(g,s+168)==0&&rd64(g,s+176)==0);
        wr32(g,s+160,77); wr32(g,s+164,0); wr64(g,s+168,99); wr64(g,s+176,0x1234);
        r=M(rd,9,STREAM_VIDEO,0,s+160,s+164,s+168,s+176); T((u32)r==0&&rd32(g,s+160)==0&&rd32(g,s+164)==2&&rd64(g,s+168)==0&&rd64(g,s+176)==0);
    }
    T(rd64(g,rd+56)==2&&rd64(g,rd+64)==2); T((u32)M(rd,9,3,0,s+160,s+164,s+168,s+176)==MF_E_INVALIDSTREAMNUMBER);
    memset(hp(g,s+184),0xee,24); r=M(rd,12,0xffffffffu,G_dur,s+184); T((u32)r==0&&rd16(g,s+184)==21&&rd64(g,s+192)==0);            /* 0x13d870 / 0x13d7f0: the length */
    if(what==8) M(rd,10,STREAM_VIDEO);                                               /* Flush */
    if(what==9) M(rd,8,G_unk,s+184);                                                 /* SetCurrentPosition */
    /* 0x13cf90, teardown: QI ID3D10Multithread, SetMultithreadProtected(0), releases */
    r=M(dev1,0,G_mth,s+16); mth=rd64(g,s+16); T((u32)r==0&&mth); T((u32)M(mth,5,0)==0); T(M(mth,2)==0);
    u64 ne0=n_ended;
    T(M(dev1,2)==1); T(M(dev,2)==0); T(M(ctx,2)==0); T(M(amt,2)==0); T(kh_movie_live()==1); T(M(rd,2)==0); T(n_ended==ne0+1); T(kh_movie_live()==0);
    T(kh_com_freed()==freed0+13);              /* device, 2 multithread, context, manager, attributes, reader, 2 native, 2 new, 2 current types: all gone */
    u64 gs[]={G_unk,G_dev1,G_mth,G_amt,G_major,G_sub,G_fs,G_st,G_fr,G_mgr,G_hw,G_adv,G_dur,G_nv12,G_float,url,s}; for(size_t i=0;i<sizeof gs/8;i++) ga_free(gs[i]);
}
static int in_child(KCtx *g,int what,char *out,size_t cap){
    FILE *cf=kh_tmpfile(); if(!cf) kh_fatal(g,"movies selftest: tmpfile failed");
    pid_t c=kh_fork(); if(c<0) kh_fatal(g,"movies selftest: fork failed");
    if(!c){ if(dup2(fileno(cf),STDERR_FILENO)<0) _exit(98);
            if(what==100) movies_mode();
            else if(what==101){ u64 gp=ga_strdup("C:/x/zmovie/en/nohook.mp4"), fd=ga_alloc(0x140,16); IMPC(rt_imp_kernel32__FindFirstFileA,GS_BASE+SITE_REQUEST,gp,fd); }   /* the file check of command 0 outside the hook of 0x5375d0 */
            else engine_sequence(g,what);
            _exit(bad?97:99); }
    int st=0; if(waitpid(c,&st,0)!=c){ fclose(cf); return -1; }
    rewind(cf); size_t n=fread(out,1,cap-1,cf); out[n]=0; fclose(cf); return WIFEXITED(st)?WEXITSTATUS(st):-1;
}
/* a made-up texture manager for the picture checks: `n` textures of 0x100 bytes linked from [mgr+0x18] by their +8 */
static u64 fake_tex(KCtx *g,const char *name,u32 w,u32 h){ u64 t=ga_calloc(1,0x100); snprintf((char*)hp(g,t+0x14),32,"%s",name); wr16(g,t+0x94,(u16)w); wr16(g,t+0x96,(u16)h); return t; }
static int fake_vsleep_ret, fake_vsleep_calls; static u32 fake_vsleep_ms;
static int fake_vsleep(u32 ms){ fake_vsleep_calls++; fake_vsleep_ms=ms; return fake_vsleep_ret; }
int kh_yield(int movie_live,int (*vsleep)(u32 ms)); u64 kh_yield_vsleeps(void);                /* threads.c */
extern void rt_imp_kernel32__SwitchToThread(KCtx *g);
int kh_movies_selftest(KCtx *g){
    checks=bad=0;
    const char *e0=getenv("KHNMC_MOVIES"); char *old=e0?strdup(e0):NULL; int save_mode=mv_mode;
    u64 sv[27]={n_req,n_req_present,n_probe_open,n_open,n_open_ok,n_standin,n_readers,n_ended,n_vreads,n_areads,n_duration,n_d3d11,n_fail_readers,
                n_req_nopause,n_cmd0,n_queued,n_not_queued,n_dropped,n_cmd0_odd,n_ev_natural,n_ev_skip,n_ev_other,n_ev_direct,n_pic_made,n_pic_reused,n_pic_lost,n_pic_uncached};
    char sv_name[sizeof req_name]; memcpy(sv_name,req_name,sizeof req_name);
    __typeof__(cur) sv_cur=cur; u64 sv_pic=pic_tex; char sv_picname[sizeof pic_name]; memcpy(sv_picname,pic_name,sizeof pic_name);
    u32 sv_pause=rd32(g,PAUSE_OFF); u64 sv_mgr=rd64(g,TEX_MGR);
    char dir[KH_TMP_MAX],fp[KH_TMP_MAX+64]; int have_dir=0; u64 gp=0, fd=0, h;
    /* 1. the switch */
    test_mode(NULL); T(movies_mode()==MV_END); test_mode(""); T(movies_mode()==MV_END); test_mode("end"); T(movies_mode()==MV_END); test_mode("fail"); T(movies_mode()==MV_FAIL);
    /* 2. the file checks: only the two call sites of the movie path, only in the default mode; a file that is absent is "present".
     *    (review: a directory that cannot be made fails the suite and still puts everything back; it returned with KHNMC_MOVIES=fail set) */
    kh_tmp_path(dir,sizeof dir,"khnmc-movie-XXXXXX"); have_dir=mkdtemp(dir)!=NULL; T(have_dir);
    if(!have_dir){ fprintf(stderr,"movies selftest: the temporary directory %s cannot be made (%s): the suite stops here, FAILED, with the mode and the counters put back\n",dir,strerror(errno)); goto restore; }
    snprintf(fp,sizeof fp,"%s/absent.mp4",dir); gp=ga_strdup(fp); fd=ga_alloc(0x140,16);
    test_mode("end"); wr32(g,PAUSE_OFF,0);
    { u64 r0=n_req, p0=n_probe_open, q0=n_queued, c0=n_cmd0, np0=n_req_nopause;
      T(kh_movie_find(g,fp,fp,0x13f2a0)==0&&kh_movie_find(g,fp,fp,0x107105)==0&&n_req==r0);                                    /* another call site: not a movie check */
      h=IMPC(rt_imp_kernel32__FindFirstFileA,SENT,gp,fd); T(h==(u64)-1);                                                          /* the file system's answer for any other caller */
      kh_movie_cmd0_begin(g,fp); T(n_cmd0==c0+1&&cur.pending==0);                                                                 /* movie command 0 (the hook of 0x5375d0) */
      h=IMPC(rt_imp_kernel32__FindFirstFileA,GS_BASE+SITE_REQUEST,gp,fd); T(h!=(u64)-1&&h!=0&&n_req==r0+1&&n_req_present==sv[1]&&!strcmp(req_name,"absent.mp4"));
      T(!strcmp((const char*)hp(g,fd+0x2c),"absent.mp4")&&rd32(g,fd)==0x80);
      T(IMPC(rt_imp_kernel32__FindNextFileA,SENT,h,fd)==0); T(IMPC(rt_imp_kernel32__FindClose,SENT,h)==1);
      kh_movie_cmd0_end(g,0); T(n_queued==q0+1&&cur.pending==1&&cur.queued==1&&cur.dropped==0&&cur.serial==r0+1&&!strcmp(cur.name,"absent.mp4")&&n_req_nopause==np0);
      h=IMPC(rt_imp_kernel32__FindFirstFileA,GS_BASE+SITE_OPEN,gp,fd); T(h!=(u64)-1&&n_req==r0+1&&n_probe_open==p0+1); T(IMPC(rt_imp_kernel32__FindClose,SENT,h)==1);
      kh_movie_event_end(g,SITE_EV_NATURAL,0,1); T(cur.pending==0);
      test_mode("fail"); u64 nq0=n_not_queued, odd0=n_cmd0_odd;
      kh_movie_cmd0_begin(g,fp);
      h=IMPC(rt_imp_kernel32__FindFirstFileA,GS_BASE+SITE_REQUEST,gp,fd); T(h==(u64)-1&&n_req==r0+2);                            /* as before task 44: absent is absent (and the request is still logged) */
      kh_movie_cmd0_end(g,-1); T(n_not_queued==nq0+1&&n_cmd0_odd==odd0&&cur.pending==1&&cur.queued==0&&n_queued==q0+1);            /* the game queued nothing: expected in this mode */
      h=IMPC(rt_imp_kernel32__FindFirstFileA,GS_BASE+SITE_OPEN,gp,fd); T(h==(u64)-1&&n_probe_open==p0+1);
      kh_movie_event_end(g,SITE_EV_SKIP,0,1); T(cur.pending==0); }
    /* 3. the header read of Movie::open: a stand-in `ftyp` header while the open runs, in the default mode only; a file that exists is
     *    neither read nor changed */
    { static const char body[]="THIS IS THE OWNER'S FILE AND MUST NOT BE READ OR WRITTEN"; snprintf(fp,sizeof fp,"%s/present.mp4",dir);
      FILE *f=fopen(fp,"wb"); T(f!=NULL); if(f){ fwrite(body,1,sizeof body,f); fclose(f); } u64 gq=ga_strdup(fp), pfh=ga_alloc(16,16), buf=ga_alloc(0x200,16);
      u64 sr[5]={KH_RAX(g),KH_RDX(g),0,0,0}; test_mode("end"); u64 s0=n_standin, o0=n_open, ok0=n_open_ok, pr0=n_req_present;
      kh_movie_cmd0_begin(g,fp); h=IMPC(rt_imp_kernel32__FindFirstFileA,GS_BASE+SITE_REQUEST,gq,fd); T(h!=(u64)-1&&n_req_present==pr0+1); IMPC(rt_imp_kernel32__FindClose,SENT,h);
      kh_movie_cmd0_end(g,0); kh_movie_event_end(g,SITE_EV_NATURAL,0,1);
      T(kh_movie_open_fd(g)==-1);                                                                                                 /* no open runs */
      wr32(g,pfh,0x55); T((u32)IMPC(rt_imp_api_ms_win_crt_stdio_l1_1_0___sopen_dispatch,SENT,gp,0x8002,0x40,0,pfh,0)==ENOENT&&rd32(g,pfh)==(u32)-1);   /* outside an open: the file system */
      KH_RDX(g)=gq; kh_movie_open_begin(g); T(n_open==o0+1);
      wr32(g,pfh,0x55); T((u32)IMPC(rt_imp_api_ms_win_crt_stdio_l1_1_0___sopen_dispatch,SENT,gq,0x8002,0x40,0,pfh,0)==0); int sfd=(int)rd32(g,pfh); T(sfd>=0&&n_standin==s0+1);
      memset(hp(g,buf),0xcc,0x200); T((s32)IMPC(rt_imp_api_ms_win_crt_stdio_l1_1_0___read,SENT,(u64)sfd,buf,0x100)==0x100);
      T(!memcmp(hp(g,buf+4),"ftyp",4)&&rd8(g,buf+3)==0x20&&rd8(g,buf+0xff)==0&&rd8(g,buf+0x100)==0xcc); T(!strstr((const char*)hp(g,buf),"OWNER"));
      T((s32)IMPC(rt_imp_api_ms_win_crt_stdio_l1_1_0___read,SENT,(u64)sfd,buf,0x100)==0);                                         /* nothing after the 0x100 bytes */
      T((u32)IMPC(rt_imp_api_ms_win_crt_stdio_l1_1_0___close,SENT,(u64)sfd)==0);
      wr32(g,pfh,0x55); T((u32)IMPC(rt_imp_api_ms_win_crt_stdio_l1_1_0___sopen_dispatch,SENT,gp,0x8002,0x40,0,pfh,0)==0&&n_standin==s0+2); close((int)rd32(g,pfh));   /* an absent file too */
      KH_RAX(g)=1; kh_movie_open_end(g); T(n_open_ok==ok0+1); T(kh_movie_open_fd(g)==-1);
      test_mode("fail"); KH_RDX(g)=gq; kh_movie_open_begin(g); T(n_open==o0+1&&kh_movie_open_fd(g)==-1); KH_RAX(g)=0; kh_movie_open_end(g); T(n_open_ok==ok0+1);   /* `fail`: no open is counted, a failed one is the old behaviour, no fatal */
      { char got[sizeof body+8]; size_t n=0; f=fopen(fp,"rb"); if(f){ n=fread(got,1,sizeof got,f); fclose(f); } T(n==sizeof body&&!memcmp(got,body,sizeof body)); }
      KH_RAX(g)=sr[0]; KH_RDX(g)=sr[1]; ga_free(gq); ga_free(pfh); ga_free(buf); unlink(fp); }
    /* 4. the engine's sequence against the null objects */
    test_mode("end"); T(end_mode()==1); engine_sequence(g,0);
    /* 5. the black picture: the player's 384 x 216 x 4 buffer is a page run, zero when fresh and when reused; it is 331,776 bytes */
    { u64 n=MOVIE_PIC_BYTES, p=ga_alloc(n,16); T(n==331776); T(p&&rd64(g,p)==0&&rd64(g,p+n-8)==0&&rd64(g,p+n/2)==0); memset(hp(g,p),0xd7,n); ga_free(p);
      u64 q=ga_alloc(n,16); T(q&&rd64(g,q)==0&&rd64(g,q+n-8)==0&&rd64(g,q+n/2)==0); ga_free(q); }
    /* 6. KHNMC_MOVIES=fail: the old objects: the reader is refused, everything else answers 0; no reader is ever "live" */
    { test_mode("fail"); T(end_mode()==0); u64 s=ga_calloc(1,64), f0=n_fail_readers; wr64(g,s,0x77);
      T((u32)IMPC(rt_imp_mfreadwrite__MFCreateSourceReaderFromURL,SENT,0,0,s)==0xC00D36C4u&&rd64(g,s)==0&&n_fail_readers==f0+1); T(kh_movie_live()==0);
      IMPC(rt_imp_mfplat__MFCreateMediaType,SENT,s+8); u64 mt=rd64(g,s+8); T(mt&&rd64(g,mt)==kh_com_vtbl(C_mt)); T((u32)M(mt,24,0,0)==0);
      IMPC(rt_imp_d3d11__D3D11CreateDevice,SENT,0,0,0,0,0,0,0,s+16,s+24,s+32); T(rd64(g,rd64(g,s+16))==kh_com_vtbl(C_d3d11)&&rd32(g,s+24)==0xb100&&rd64(g,rd64(g,s+32))==kh_com_vtbl(C_ctx));
      ga_free(s); }
    /* 7. the exit line: says "ended by itself" only for events that ended at the natural-end site, and says what else happened */
    { test_mode("end"); T(movies_mode()==MV_END); FILE *rf=kh_tmpfile(); T(rf!=NULL); if(rf){
        n_req=3; n_req_present=1; n_req_nopause=1; n_queued=3; n_dropped=0; n_cmd0_odd=0; n_open=3; n_open_ok=3; n_standin=3; n_d3d11=3; n_readers=3; n_vreads=3; n_areads=3; n_ended=3; n_duration=0;
        n_ev_natural=2; n_ev_skip=0; n_ev_other=0; n_ev_direct=0; n_pic_made=1; n_pic_reused=2; n_pic_lost=0; n_pic_uncached=0;
        kh_movies_report(rf);                                                            /* a: one event not ended */
        n_ev_natural=3; kh_movies_report(rf);                                            /* b: all good */
        n_ev_natural=2; n_ev_skip=1; kh_movies_report(rf);                               /* c: one ended by Skip: the readers are all released, and it is still not "by itself" */
        n_ev_natural=3; n_ev_skip=0; n_dropped=1; kh_movies_report(rf);                  /* d: a dropped request */
        n_dropped=0; n_ev_direct=1; kh_movies_report(rf);                                /* e: an end through the direct branch */
        n_ev_direct=0; n_ended=2; kh_movies_report(rf);                                  /* f: a player not torn down */
        n_ended=3; n_req=0; n_dropped=0; kh_movies_report(rf);                           /* nothing requested: no line */
        n_req=0; n_dropped=2; n_ev_skip=2; n_ev_natural=0; n_queued=0; n_readers=0; n_ended=0; kh_movies_report(rf);   /* g: only dropped requests: a line */
        rewind(rf); char bb[8000]; size_t n=fread(bb,1,sizeof bb-1,rf); bb[n]=0; fclose(rf);
        char *ln[8]; int nl=0; for(char *q=bb;*q&&nl<8;){ ln[nl++]=q; char *e=strchr(q,'\n'); if(!e) break; *e=0; q=e+1; }
        T(nl==7);                                                                        /* no movie requested and none dropped: no line */
        if(nl==7){
          T(strstr(ln[0],"mode end: 3 requested (1 with the file present, 2 absent; 1 in a scene without a pause screen), 3 queued, 0 DROPPED")&&strstr(ln[0],"3 of 3 opens worked")&&strstr(ln[0],"3 players torn down (reader released)"));
          T(strstr(ln[0],"events: 2 ended by themselves (natural end), 0 by Skip, 0 otherwise, 1 not ended when the run ended")&&strstr(ln[0],"NOT every requested movie ended by itself"));
          T(strstr(ln[0],"pictures: 1 made by the engine (384 x 216, 331776 bytes each, never freed by it), 2 times the kept one handed out again, 0 kept ones lost, 0 not kept"));
          T(strstr(ln[1],"events: 3 ended by themselves (natural end), 0 by Skip, 0 otherwise, 0 not ended when the run ended")&&!strstr(ln[1],"NOT every"));
          T(strstr(ln[2],"2 ended by themselves (natural end), 1 by Skip")&&strstr(ln[2],"NOT every requested movie ended by itself"));
          T(strstr(ln[3],"1 DROPPED by the game's own guard")&&strstr(ln[3],"NOT every"));
          T(strstr(ln[4],"1 of the ended through the direct branch")&&strstr(ln[4],"NOT every"));
          T(strstr(ln[5],"2 players torn down")&&strstr(ln[5],"NOT every"));
          T(strstr(ln[6],"mode end: 0 requested")&&strstr(ln[6],"2 DROPPED by the game's own guard")&&strstr(ln[6],"0 ended by themselves (natural end), 2 by Skip")&&strstr(ln[6],"NOT every")); }
        test_mode("fail"); T(movies_mode()==MV_FAIL); rf=kh_tmpfile(); T(rf!=NULL); if(rf){ n_req=2; n_req_present=0; n_req_nopause=1; n_dropped=0; n_fail_readers=0; n_ev_natural=0; n_ev_skip=1; n_ev_other=0; n_ev_direct=0;
          kh_movies_report(rf); rewind(rf); n=fread(bb,1,sizeof bb-1,rf); bb[n]=0; fclose(rf);
          T(strstr(bb,"mode fail (the seam before task 44): 2 requested (0 with the file present, 2 absent")&&strstr(bb,"1 of the requests in a scene WITHOUT a pause screen")&&strstr(bb,"events: 0 ended by themselves (natural end), 1 by Skip, 0 otherwise, 1 not ended when the run ended")); } } }
    /* 8. the request line: what the scene can do about a movie that does not come (review: ena / sec / sec2 have no pause screen) */
    { FILE *rf=kh_tmpfile(); T(rf!=NULL); if(rf){
        say_request(rf,7,"ena.mp4",1000,"C:/z/ena.mp4",0,MV_END,2); say_request(rf,8,"opn.mp4",1001,"C:/z/opn.mp4",0,MV_END,0);
        say_request(rf,9,"ena.mp4",1002,"C:/z/ena.mp4",0,MV_FAIL,2); say_request(rf,10,"opn.mp4",1003,"C:/z/opn.mp4",0,MV_FAIL,0); say_request(rf,11,"opn.mp4",1004,"C:/z/opn.mp4",1,MV_FAIL,1);
        rewind(rf); char bb[4000]; size_t n=fread(bb,1,sizeof bb-1,rf); bb[n]=0; fclose(rf);
        char *ln[6]; int nl=0; for(char *q=bb;*q&&nl<6;){ ln[nl++]=q; char *e=strchr(q,'\n'); if(!e) break; *e=0; q=e+1; }
        T(nl==5);
        if(nl==5){
          T(strstr(ln[0],"#7 ena.mp4 requested at game frame 1000")&&strstr(ln[0],"this scene has NO pause screen (pause-disable bits 0x2")&&strstr(ln[0],"an empty one that ends at once"));
          T(strstr(ln[1],"#8 opn.mp4")&&strstr(ln[1],"the pause screen is not disabled by the event's type (pause-disable bits 0x0)")&&!strstr(ln[1],"NO pause screen"));
          T(strstr(ln[2],"KHNMC_MOVIES=fail")&&strstr(ln[2],"waits on a black screen FOR EVER")&&!strstr(ln[2],"for the pause screen's Skip"));
          T(strstr(ln[3],"KHNMC_MOVIES=fail")&&strstr(ln[3],"for the pause screen's Skip")&&!strstr(ln[3],"FOR EVER"));
          T(strstr(ln[4],"the file is present")&&strstr(ln[4],"the source reader will fail")&&!strstr(ln[4],"NO pause screen")); } }                /* bit 0 alone is not the event's bit */
      /* the request reads the game's pause-disable bits: bit 1 counts the request as one in a scene without a pause screen */
      test_mode("end"); snprintf(fp,sizeof fp,"%s/absent.mp4",dir); u64 np0=n_req_nopause;
      wr32(g,PAUSE_OFF,2); kh_movie_cmd0_begin(g,fp); h=IMPC(rt_imp_kernel32__FindFirstFileA,GS_BASE+SITE_REQUEST,gp,fd); IMPC(rt_imp_kernel32__FindClose,SENT,h); kh_movie_cmd0_end(g,0); kh_movie_event_end(g,SITE_EV_NATURAL,0,1); T(n_req_nopause==np0+1);
      wr32(g,PAUSE_OFF,1); kh_movie_cmd0_begin(g,fp); h=IMPC(rt_imp_kernel32__FindFirstFileA,GS_BASE+SITE_REQUEST,gp,fd); IMPC(rt_imp_kernel32__FindClose,SENT,h); kh_movie_cmd0_end(g,0); kh_movie_event_end(g,SITE_EV_NATURAL,0,1); T(n_req_nopause==np0+1);
      wr32(g,PAUSE_OFF,0); }
    /* 9. movie command 0 and the event's end (the plain-value halves of the hooks of 0x5375d0 and 0x2cbd80) */
    { test_mode("end"); snprintf(fp,sizeof fp,"%s/absent.mp4",dir);
      u64 q0=n_queued, d0=n_dropped, odd0=n_cmd0_odd, na0=n_ev_natural, sk0=n_ev_skip, ot0=n_ev_other, di0=n_ev_direct, r0=n_req;
#define REQ(ret) do{ kh_movie_cmd0_begin(g,fp); h=IMPC(rt_imp_kernel32__FindFirstFileA,GS_BASE+SITE_REQUEST,gp,fd); IMPC(rt_imp_kernel32__FindClose,SENT,h); kh_movie_cmd0_end(g,ret); }while(0)
      kh_movie_event_end(g,SITE_EV_NATURAL,0,1); T(n_ev_natural==na0&&n_ev_skip==sk0&&n_ev_other==ot0);                           /* no movie is waiting: an event's end is not a movie's */
      REQ(0); T(n_queued==q0+1&&cur.pending&&cur.queued);
      kh_movie_event_end(g,SITE_EV_NATURAL,1,1); T(cur.pending==1&&n_ev_natural==na0);                                            /* the event's state is already 4: 0x2cbd80 does nothing, nothing is counted */
      kh_movie_event_end(g,SITE_EV_NATURAL,0,1); T(cur.pending==0&&n_ev_natural==na0+1&&n_ev_skip==sk0&&n_ev_other==ot0&&n_ev_direct==di0);   /* the natural end, as a task */
      kh_movie_event_end(g,SITE_EV_SKIP,0,1); T(n_ev_skip==sk0);                                                                  /* once per movie */
      REQ(0); kh_movie_event_end(g,SITE_EV_SKIP,0,1); T(cur.pending==0&&n_ev_skip==sk0+1&&n_ev_natural==na0+1&&n_ev_other==ot0);   /* ended by Skip: not "by itself" */
      REQ(0); kh_movie_event_end(g,0x2cfa37,0,1); T(cur.pending==0&&n_ev_other==ot0+1&&n_ev_natural==na0+1&&n_ev_skip==sk0+1);     /* another caller of 0x2cbd80 */
      REQ(0); kh_movie_event_end(g,SITE_EV_NATURAL,0,0); T(n_ev_natural==na0+2&&n_ev_direct==di0+1);                              /* the direct branch: the player survives */
      T(n_queued==q0+4&&n_dropped==d0&&n_cmd0_odd==odd0&&n_req==r0+4);
      REQ(-1); T(n_cmd0_odd==odd0+1&&cur.queued==0&&cur.pending==1&&n_queued==q0+4); kh_movie_event_end(g,SITE_EV_SKIP,0,1);         /* default mode: "present" and still not queued is said */
      kh_movie_cmd0_begin(g,fp); kh_movie_cmd0_end(g,0);                                                                          /* the game's guard: answer 0 with no file check */
      T(n_dropped==d0+1&&cur.pending==1&&cur.dropped==1&&cur.queued==0&&cur.serial==0&&n_req==r0+5&&n_queued==q0+4&&!strcmp(cur.name,"absent.mp4"));
      kh_movie_event_end(g,SITE_EV_SKIP,0,1); T(cur.pending==0&&n_ev_skip==sk0+3);                                                 /* its scene's end is still seen */
      kh_movie_cmd0_begin(g,fp); kh_movie_cmd0_end(g,5); T(n_cmd0_odd==odd0+2&&n_dropped==d0+1); kh_movie_event_end(g,SITE_EV_SKIP,0,1);   /* an answer this seam does not know */
      kh_movie_cmd0_end(g,0); T(n_dropped==d0+1&&n_queued==q0+4);                                                                 /* an end without a begin does nothing */
#undef REQ
    }
    /* 10. the kept picture (the plain-value halves of the hook of 0x13d690), against a made-up texture manager */
    { test_mode("end"); u64 mgr=ga_calloc(1,0x40), t1=fake_tex(g,"no_name1.png",1,1), t2=fake_tex(g,"no_name2.png",MOVIE_W,MOVIE_H), t3=fake_tex(g,"no_name3.png",64,64), out=0x55;
      wr64(g,mgr+0x18,t1); wr64(g,t1+8,t2); wr64(g,t2+8,t3); wr64(g,TEX_MGR,mgr); pic_tex=0;
      u64 m0=n_pic_made, re0=n_pic_reused, lo0=n_pic_lost, un0=n_pic_uncached;
      T(kh_movie_picture_reuse(g,&out)==0&&out==0x55&&n_pic_reused==re0&&n_pic_lost==lo0);                                        /* nothing kept yet: the engine makes one */
      kh_movie_picture_made(g,0); T(n_pic_made==m0&&pic_tex==0);                                                                  /* the engine made none */
      kh_movie_picture_made(g,t2); T(n_pic_made==m0+1&&pic_tex==t2&&n_pic_uncached==un0&&!strcmp(pic_name,"no_name2.png"));
      T(kh_movie_picture_reuse(g,&out)==1&&out==t2&&n_pic_reused==re0+1);                                                         /* every later movie gets the same picture */
      /* the hook itself on its reuse path (the engine's 0x13d690 does not run): a guest call with its return address on the guest
       * stack must come back with that address popped, the picture in rax and in [inner+0x18], and nothing else of the stack touched */
      { u64 inner=ga_calloc(1,0x28), sp0=KH_RSP(g), ax0=KH_RAX(g), cx0=KH_RCX(g), sp=(sp0-0x100)&~15ull; wr64(g,sp,0x1111111111111111ull); wr64(g,sp-8,0x14047aa99ull); wr64(g,sp-16,0x2222222222222222ull);
        KH_RSP(g)=sp-8; KH_RCX(g)=inner; KH_RAX(g)=0x77; f_13d690(g);
        T(KH_RSP(g)==sp); T(KH_RAX(g)==t2&&rd64(g,inner+0x18)==t2&&n_pic_reused==re0+2); T(rd64(g,sp)==0x1111111111111111ull&&rd64(g,sp-8)==0x14047aa99ull&&rd64(g,sp-16)==0x2222222222222222ull);
        KH_RSP(g)=sp0; KH_RAX(g)=ax0; KH_RCX(g)=cx0; ga_free(inner); }
      wr16(g,t2+0x94,MOVIE_W+1); out=0x55; T(kh_movie_picture_reuse(g,&out)==0&&out==0x55&&n_pic_lost==lo0+1&&pic_tex==0); wr16(g,t2+0x94,MOVIE_W);   /* another size at that address: not ours */
      kh_movie_picture_made(g,t2); snprintf((char*)hp(g,t2+0x14),32,"no_name9.png"); T(kh_movie_picture_reuse(g,&out)==0&&n_pic_lost==lo0+2); snprintf((char*)hp(g,t2+0x14),32,"no_name2.png");   /* another name: not ours */
      kh_movie_picture_made(g,t2); wr64(g,t1+8,t3); T(kh_movie_picture_reuse(g,&out)==0&&n_pic_lost==lo0+3&&n_pic_reused==re0+2); wr64(g,t1+8,t2);   /* unlinked from the manager's list: the engine freed it */
      T(n_pic_made==m0+3&&n_pic_uncached==un0);
      kh_movie_picture_made(g,t3); T(n_pic_uncached==un0+1&&pic_tex==0);                                                          /* listed, but not the empty movie's size: not kept */
      u64 t4=fake_tex(g,"no_name4.png",MOVIE_W,MOVIE_H); kh_movie_picture_made(g,t4); T(n_pic_uncached==un0+2&&pic_tex==0);       /* the right size, but not in the manager's list: not kept */
      wr64(g,TEX_MGR,0); kh_movie_picture_made(g,t2); T(n_pic_uncached==un0+3&&pic_tex==0); T(kh_movie_picture_reuse(g,&out)==0);   /* no manager */
      wr64(g,TEX_MGR,sv_mgr); ga_free(mgr); ga_free(t1); ga_free(t2); ga_free(t3); ga_free(t4); }
    /* 11. SwitchToThread (runtime/threads.c): 1 ms on the virtual clock only while an empty movie's reader exists; else the plain yield
     *     it always was (KHNMC_MOVIES=fail, every run without a movie, and the host clock, where the virtual sleep says no) */
    { u64 y0=kh_yield_vsleeps();
      fake_vsleep_calls=0; fake_vsleep_ret=1; T(kh_yield(0,fake_vsleep)==0&&fake_vsleep_calls==0&&kh_yield_vsleeps()==y0);          /* no movie: the clock is not asked */
      T(kh_yield(1,fake_vsleep)==1&&fake_vsleep_calls==1&&fake_vsleep_ms==1&&kh_yield_vsleeps()==y0+1);                           /* a movie, the frames clock: 1 ms, counted */
      fake_vsleep_ret=0; T(kh_yield(1,fake_vsleep)==0&&fake_vsleep_calls==2&&kh_yield_vsleeps()==y0+1);                           /* a movie, the host clock: the plain yield */
      T(kh_movie_live()==0); T(IMPC(rt_imp_kernel32__SwitchToThread,SENT)==1&&kh_yield_vsleeps()==y0+1); }                        /* the import, no movie: TRUE, nothing counted */
    /* 12. the named fatals */
    if(!kh_fork_ok()) kh_selftest_not_run("movies","the named fatals movies-mode, movie-open-hook, movie-open-failed, movie-cmd-hook and movie-unexpected-call (14 children)",28);
    else {
        char out[16384]; int rc;
        test_mode("skip"); rc=in_child(g,100,out,sizeof out); T(rc==3); T(strstr(out,"movies-mode: KHNMC_MOVIES=skip is not a mode")!=NULL);
        test_mode("end");
        rc=in_child(g,10,out,sizeof out); T(rc==3); T(strstr(out,"movie-open-hook: MFCreateSourceReaderFromURL outside Movie::open")!=NULL);
        rc=in_child(g,11,out,sizeof out); T(rc==3); T(strstr(out,"movie-open-failed: ")!=NULL&&strstr(out,"Movie::open (exe 0x13da10) FAILED in the default mode")!=NULL);
        rc=in_child(g,101,out,sizeof out); T(rc==3); T(strstr(out,"movie-cmd-hook: the file check of movie command 0")!=NULL);
        static const char *want[10]={NULL,"d3d11:end:ID3D11Device slot 5","d3d11:end:ID3D11DeviceContext slot 46","SetUINT32 of an attribute that is not one of the two","GetCurrentMediaType of a stream whose output type the game has not set",
                                     "other than NV12 or RGB32 video","GetUINT64 of an attribute the empty movie's media types do not have","QueryInterface for an interface this media type is not asked for","mf:end:IMFSourceReader slot 10","mf:end:IMFSourceReader slot 8"};
        for(int w=1;w<10;w++){ rc=in_child(g,w,out,sizeof out); T(rc==3); T(strstr(out,"movie-unexpected-call:")!=NULL&&strstr(out,want[w])!=NULL); if(rc!=3||!strstr(out,want[w])) fprintf(stderr,"  fatal case %d: exit %d, wanted `%s`\n",w,rc,want[w]); }
        rc=in_child(g,0,out,sizeof out); T(rc==99); T(strstr(out,"kh: FATAL:")==NULL);
    }
restore:
    if(have_dir) rmdir(dir);
    if(gp) ga_free(gp); if(fd) ga_free(fd);
    char after[80]; if(old){ setenv("KHNMC_MOVIES",old,1); snprintf(after,sizeof after,"KHNMC_MOVIES=%s again",old); free(old); } else { unsetenv("KHNMC_MOVIES"); snprintf(after,sizeof after,"KHNMC_MOVIES unset again"); }
    mv_mode=save_mode; n_req=sv[0]; n_req_present=sv[1]; n_probe_open=sv[2]; n_open=sv[3]; n_open_ok=sv[4]; n_standin=sv[5]; n_readers=sv[6]; n_ended=sv[7]; n_vreads=sv[8]; n_areads=sv[9]; n_duration=sv[10]; n_d3d11=sv[11]; n_fail_readers=sv[12];
    n_req_nopause=sv[13]; n_cmd0=sv[14]; n_queued=sv[15]; n_not_queued=sv[16]; n_dropped=sv[17]; n_cmd0_odd=sv[18]; n_ev_natural=sv[19]; n_ev_skip=sv[20]; n_ev_other=sv[21]; n_ev_direct=sv[22]; n_pic_made=sv[23]; n_pic_reused=sv[24]; n_pic_lost=sv[25]; n_pic_uncached=sv[26];
    memcpy(req_name,sv_name,sizeof req_name); cur=sv_cur; pic_tex=sv_pic; memcpy(pic_name,sv_picname,sizeof pic_name); in_cmd0=0;
    wr32(g,PAUSE_OFF,sv_pause); wr64(g,TEX_MGR,sv_mgr);
    fprintf(stderr,"kh: movies selftest: %d checks, %d failures (the switch, the two file checks, the stand-in header, the game's call sequence against the empty movie, the black picture, KHNMC_MOVIES=fail, the exit line, the request line, movie command 0 and the event's end, the kept picture, the yield, the named fatals); %s, the mode and the counters are as before the suite\n",checks,bad,after);
    return bad;
}
