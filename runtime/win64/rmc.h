#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char *host;
    const char *rel;
    int64_t size;
    int reported;
} KhRmcFile;

void kh_rmc_init(const char *root);
int kh_rmc_path(const char *rel,char *host,size_t cap);
int kh_data_path(const char *root,const char *rel,char *host,size_t cap);
KhRmcFile *kh_rmc_find(const char *rel);
/* Contiguous, case-insensitive range of files below a canonical directory. */
size_t kh_rmc_children(const char *dir, KhRmcFile **first);
void kh_rmc_report(KhRmcFile *file);
int kh_rmc_sync(const char *docs,const char *selected);
