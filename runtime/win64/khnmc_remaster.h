/* khnmc_remaster.h : container walkers + sort comparator of the remastered-asset protocol (remaster.c) */
#pragma once
#include <stdint.h>
#include <stdio.h>
void kh_rm_ext(const char *path, char *out, size_t cap);
int  kh_rm_offsets(const char *path, const unsigned char *bytes, int64_t n, int32_t **out, int *count, int *err);   /* caller frees *out; returns 1 if the extension is a known container type */
int  kh_rm_less(const char *an, const char *bn);
#include <stdatomic.h>
extern _Atomic unsigned long long kh_rm_oob_keys;   /* out-of-array texture-info indexes met by the RAW walker (substituted by a never-matching offset) */
