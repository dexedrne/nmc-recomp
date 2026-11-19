#pragma once
#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
typedef CC_SHA256_CTX SlotHash;
#define slot_hash_init CC_SHA256_Init
#define slot_hash_add CC_SHA256_Update
#define slot_hash_end(out,ctx) CC_SHA256_Final(out,ctx)
#else
#include <openssl/sha.h>
typedef SHA256_CTX SlotHash;
#define slot_hash_init SHA256_Init
#define slot_hash_add SHA256_Update
#define slot_hash_end SHA256_Final
#endif
