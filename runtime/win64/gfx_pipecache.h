/* gfx_pipecache.h : the disk pipeline cache (TASK43; IOS-GRAPHICS-PLAN.md section 4 step 12).  runtime/gfx_pipecache.c implements
 * the file, the rules and the reports; the Vulkan side is the gipa wrapper of runtime/gfx_vkd3d.c.  No Vulkan or D3D12 type here.
 *
 * WHAT IT IS.  vkd3d makes one VkPipelineCache per device, empty (vkd3d 2.1 device.c 2409-2430), uses it for every graphics pipeline
 * (state.c 4124; compute pipelines get VK_NULL_HANDLE: state.c 2509-2510), and never reads it back or merges it (vkGetPipelineCacheData
 * and vkMergePipelineCaches are loaded, vulkan_procs.h 164 and 169, and called nowhere).  The gipa wrapper answers vkd3d's
 * vkCreatePipelineCache with the real one called with pInitialData from a file, and the runtime writes vkGetPipelineCacheData back to
 * that file.  On MoltenVK the cache holds the MSL source of every converted shader (MVKPipeline.mm writeData: shader module key,
 * conversion configuration, result info, compressed MSL), so a later launch skips the SPIR-V -> MSL conversion; it does NOT hold a
 * compiled Metal library or pipeline state: MoltenVK compiles every loaded MSL again inside vkCreatePipelineCache
 * (MVKShaderLibraryCache::addShaderLibrary -> MVKShaderLibrary -> compileLibrary), i.e. at device creation, not at the first draw.
 *
 * WHERE.   KHNMC_GFX_PIPECACHE=<path>   the file.  Unset: no cache, nothing changes; EXCEPT in an app-container build (KH_IOS_PATHS: the
 *                                       iPhone app, and the same code forced on a desk machine), where the default is
 *                                       <home>/Library/Caches/khnmc/pipe.bin (never Documents: the backup and the owner's Files view
 *                                       see that; iOS may empty Library/Caches, which is a first run again).
 *          KHNMC_GFX_PIPECACHE=off      no cache, also in the app.
 *          KHNMC_GFX_PIPECACHE_MAX_MB=n the size cap of the payload (1..1024, default 64).
 *          KHNMC_GFX_PIPECACHE_RESET=1  the file is removed at start-up: this run is a first run and writes a new file (a measurement's
 *                                       run 1 on the phone, where nothing else can delete it).
 *          A --selftest run never uses the configured file (its own cache tests write into the scratch directory).
 *
 * THE FILE.  Our header (72 bytes, little-endian), then the driver's bytes:
 *     0  magic "KHNMCPC\0"      8  version (1)      12  header length (72)
 *    16  vendorID  20  deviceID  24  driverVersion   28  reserved (0)       of the VkPhysicalDevice that wrote it
 *    32  pipelineCacheUUID (16)
 *    48  payload length (u64)   56  payload checksum (FNV-1a 64)   64  header checksum (FNV-1a 64 of bytes 0..63)
 *  A file that is missing is a first run.  One that cannot be read, is truncated, is not ours (garbage), has a damaged header, another
 *  version, a wrong length, a wrong checksum, is above the cap or was written by ANOTHER DEVICE (UUID, vendor, device or driver
 *  version differ) is named in the log (kh_pc_result_name), counted, and an EMPTY cache is used: the run goes on.  The same when the
 *  driver refuses bytes that passed our checks (pipecache-driver-refused: the real vkCreatePipelineCache is called again without them).
 *  THE LOAD GUARD (review C43 n=12).  A driver that crashes, hangs or is killed INSIDE vkCreatePipelineCache on bytes whose checksum is
 *  right would do so again at every launch.  So <path>.loading is written right before the bytes are handed over (kh_pc_load returns
 *  KH_PC_OK) and removed when the driver's call has returned (kh_pc_note_loaded / kh_pc_note_driver_refused).  A marker found at a load
 *  is pipecache-load-unfinished: named, counted, the file is REMOVED, an empty cache is used.  (A launch the owner ends during a long
 *  but healthy load costs one first run; a failure the bytes cause AFTER the call returned is not covered.)
 *
 * WHEN IT IS WRITTEN.  Only at two points, never from the render path:
 *     KH_PC_AT_PARKED   the suspend's step 6 (runtime/app_life.c, the life worker): the suspend gate is set and nothing is admitted;
 *     KH_PC_AT_EXIT     kh_exit_process (runtime/loader.c), before the exit reports.
 *  kh_gfx_pipecache_save checks that itself: a call with the gate clear, or with a thread still admitted, or an "exit" save outside
 *  the process exit is REFUSED, loudly, counted, and writes nothing.  While the driver's bytes are fetched no pipeline is created (the
 *  wrapper holds new creation calls for that moment and waits, bounded, for the ones in flight).  The bytes go to <path>.tmp, are
 *  synced, and renamed into place.  Nothing is written when no pipeline was created since the last load or save, when the bytes are
 *  the ones already on disk, or when they are above the cap (refused, said so, the old file is kept). */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct { uint8_t uuid[16]; uint32_t vendor, device, driver; } KhPcDev;   /* VkPhysicalDeviceProperties: pipelineCacheUUID, vendorID, deviceID, driverVersion */

#define KH_PC_HEADER_BYTES 72u
#define KH_PC_VERSION 1u
#define KH_PC_DEFAULT_CAP (64ull<<20)

/* what a load found (kh_pc_result_name gives the name the log uses) */
enum { KH_PC_OK=0, KH_PC_MISSING, KH_PC_UNREADABLE, KH_PC_TRUNCATED, KH_PC_GARBAGE, KH_PC_HEADER_CORRUPT, KH_PC_VERSION_UNKNOWN, KH_PC_TOO_LARGE,
       KH_PC_LENGTH, KH_PC_CHECKSUM, KH_PC_FOREIGN_DEVICE, KH_PC_EMPTY, KH_PC_DRIVER_REFUSED, KH_PC_LOAD_UNFINISHED, KH_PC_N };
const char *kh_pc_result_name(int r);

/* ---- pure: the file image ---- */
uint64_t kh_pc_fnv(const void *p,size_t n);
void kh_pc_header_make(uint8_t h[KH_PC_HEADER_BYTES],const KhPcDev *dev,const void *payload,size_t len);
/* a whole file image in memory: KH_PC_OK and the payload's offset and length, or what is wrong with it (why: a text for the log) */
int  kh_pc_parse(const uint8_t *file,size_t n,const KhPcDev *dev,uint64_t cap,size_t *off,size_t *len,char *why,size_t whycap);
/* ---- files ---- */
/* reads at most cap + header bytes of `path`.  KH_PC_OK: *file is the malloc'd image (the caller frees it), *payload points into it */
int  kh_pc_file_load(const char *path,const KhPcDev *dev,uint64_t cap,uint8_t **file,const uint8_t **payload,size_t *len,char *why,size_t whycap);
/* header + payload to <path>.tmp, fsync, rename onto path.  0 = done; -1 = failed (why says where; the temporary file is removed and
 * `path` is as it was).  A directory of `path` that is not there (a named path under a directory nothing made yet, e.g. @CACHES@; or
 * iOS emptied Library/Caches while the app ran) is made, once, said and counted (review C43 n=6, n=9) */
int  kh_pc_file_save(const char *path,const KhPcDev *dev,const void *payload,size_t len,char *why,size_t whycap);

/* ---- configuration (start-up, runtime/null_d3d12.c kh_seam_gfx_init) ---- */
void kh_pc_init(int vkd3d_backend_on);
int  kh_pc_active(void);                        /* 1 = a cache file is configured for this run */
const char *kh_pc_path(void);                   /* "" when not */
uint64_t kh_pc_cap(void);

/* ---- the Vulkan side registers how the driver's bytes are fetched (runtime/gfx_vkd3d.c) ---- */
enum { KH_PCF_OK=0, KH_PCF_NONE, KH_PCF_TOO_LARGE, KH_PCF_BUSY, KH_PCF_FAILED };
typedef struct {
    int  (*dirty)(void);                        /* 1 = pipelines were created through the live cache since the last commit, 0 = none, -1 = no live cache */
    /* excludes pipeline creation, then the bytes: KH_PCF_OK (*data malloc'd, the exclusion HELD until done()), or NONE (no live cache, or the
     * driver has nothing), TOO_LARGE (*len = the size, above cap: not fetched), BUSY (creations still in flight after the bound),
     * FAILED (why).  Only after KH_PCF_OK is done() called.  deadline_ns (0 = none): the host time at which the save's caller stops
     * waiting; the wait for creation calls in flight ends KH_PC_SAVE_MARGIN_MS before it (review C43 n=11) */
    int  (*fetch)(uint64_t cap,uint64_t deadline_ns,void **data,size_t *len,KhPcDev *dev,char *why,size_t whycap);
    void (*done)(int committed);                /* ends the exclusion; committed = the bytes are on disk (or already were) */
} KhPcProvider;
void kh_pc_set_provider(const KhPcProvider *p);

/* ---- load: called by the wrapped vkCreatePipelineCache for a device that is not a probe ---- */
/* KH_PC_OK: *file (malloc'd, the caller frees it after the driver's call), *payload, *len; else the named result, already logged and
 * counted, and an empty cache is to be used */
int  kh_pc_load(const KhPcDev *dev,uint8_t **file,const uint8_t **payload,size_t *len);
void kh_pc_note_loaded(size_t len,uint64_t payload_sum,const KhPcDev *dev,uint64_t driver_ns);   /* the driver took the bytes: the "loaded N bytes" line */
void kh_pc_note_driver_refused(size_t len,int vk_result);                    /* it did not: named, counted; the caller creates an empty cache */
void kh_pc_note_probe_skipped(void);                                         /* a capability-probe device: no bytes handed over (counted) */

/* ---- save ---- */
enum { KH_PC_AT_PARKED=1, KH_PC_AT_EXIT=2 };
enum { KH_PCS_OFF=0, KH_PCS_WRITTEN, KH_PCS_UNCHANGED, KH_PCS_REFUSED, KH_PCS_NOTHING, KH_PCS_OVER_CAP, KH_PCS_BUSY, KH_PCS_LATE, KH_PCS_FAILED,
       KH_PCS_KEPT };   /* KEPT: the file could not be READ at the load (not its content) and this run made no pipeline: it is not replaced */
/* deadline_ns: 0 = none; else the host time by which the caller's bound ends (a suspend): with less than KH_PC_SAVE_MARGIN_MS left the
 * save is skipped (KH_PCS_LATE, counted), before the driver is asked AND again before the file is written; the wait for creation calls
 * in flight is cut to what is left (review C43 n=11).  What stays unbounded: one driver call and one file write, each started only
 * with the margin left */
#define KH_PC_SAVE_MARGIN_MS 100u
int  kh_gfx_pipecache_save(int at,uint64_t deadline_ns);
void kh_gfx_pipecache_save_point(uint64_t deadline_ns);   /* the suspend's step 6 (app_life.c): the gate is set, nothing is admitted */
void kh_gfx_pipecache_exit(void);                         /* kh_exit_process: the process is ending; the last save */
void kh_pc_report(FILE *f);                               /* exit: one line when a cache was configured or a save point was reached, else nothing */

/* ---- selftests ---- */
int  kh_pc_selftest(void);                                /* every --selftest (KHNMC_SELFTEST_ONLY=pipecache): no device */
void kh_pc_test_config(const char *path,uint64_t cap);    /* selftests: this file and cap (NULL = back to the run's own configuration) */
void kh_pc_test_exiting(int on);
typedef struct { uint64_t load_ok, load_bytes, load_missing, load_rejected, load_by[KH_PC_N], driver_refused, probes,
                 save_written, save_bytes, save_unchanged_clean, save_unchanged_same, save_refused, save_nothing, save_over_cap, save_busy, save_late, save_failed, save_off,
                 save_late_fetch, save_kept, disk_gone, disk_changed, dirs_made, marker_failed; } KhPcCounters;   /* review C43: late after the fetch (in save_late too); kept after an unread file; the file gone / changed under us; directories made; load markers that could not be written */
void kh_pc_counters(KhPcCounters *c);
uint64_t kh_pc_save_points(void);                         /* calls of kh_gfx_pipecache_save_point (the suspend's step 6), configured or not */

#ifdef __cplusplus
}
#endif
