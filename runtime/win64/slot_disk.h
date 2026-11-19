#pragma once
#include "rewind.h"

void kh_disk_sync(KhRecords *,struct KhSyncSlot *);
void kh_disk_clock(KhRecords *,struct KhClockSlot *);
void kh_disk_timers(KhRecords *,struct KhTimerSlot *);
void kh_disk_sections(KhRecords *,struct KhCsSlot *);
void kh_disk_files(KhRecords *,struct KhCrtFiles *);
void kh_disk_streams(KhRecords *,struct KhStreamSlot *);
void kh_disk_audio(KhRecords *,struct KhAudioSlot *);
#define KH_SLOT_DOCS_MAX (256u<<20)
typedef struct { u8 *data; size_t size,capacity; } KhBootDocs;
const char *kh_slot_docs_capture(KhBootDocs *);
const char *kh_slot_docs_begin(const KhBootDocs *);
void kh_slot_docs_finish(void);
const char *kh_slot_autosave_documents(void);
const char *kh_slot_documents(void);
typedef struct { u64 poll; u8 pad[12]; u32 reserved; } KhPadChange;
typedef struct { KhPadChange *changes; size_t count; u64 polls; } KhPadHistory;
void kh_pad_capture(KhPadHistory *);
void kh_pad_restore(const KhPadHistory *);
int kh_pad_bootstrap(const KhPadHistory *);
int kh_pad_caught_up(void);
void kh_pad_stop_bootstrap(void);

typedef struct {
    u8 magic[8], build[32], game[32], data[32], settings[32], key[16];
    u64 frame, blocks, bytes, changes, polls, docs;
    u8 checksum[32];
} KhSlotHeader;
const char *kh_slot_identity(KhSlotHeader *);
const char *kh_slot_write(FILE *,KhSlotHeader *,KhRecords *,const KhPadHistory *,const KhBootDocs *);
const char *kh_slot_read(FILE *,const KhSlotHeader *,KhRecords *,KhPadHistory *,KhBootDocs *,const KhRecords *);

