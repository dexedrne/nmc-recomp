/* Task 16: reference arithmetic and value tables for the stock restore-item path of DLL 0x31260 (blocks 31441..3147b and
 * 31482..314df).  Shared by runtime/mod_difficulty_items_test.c (expected values for the port) and tools/native_item_oracle.c
 * (the same expressions compared with the DLL's own instruction blocks).  Deliberately written as plain C division, not as the
 * magic-number sequence the port transliterates, so the two are independent. */
#pragma once
#include <stdint.h>

/* DLL 31441..31476: pct is a zero-extended byte, mult and max are full 32-bit values, every product and the +9999 wrap at 32 bits. */
static inline int32_t kh_item_ref_numer(uint32_t pct, uint32_t mult, uint32_t max){ return (int32_t)(pct*mult*max+9999u); }
/* DLL 31482..314df: signed division truncating toward zero by -10000 when the command's Reserve byte is exactly 2, else by 10000. */
static inline int32_t kh_item_ref_amount(int32_t numer, int reserve_byte){ return reserve_byte==2 ? numer/-10000 : numer/10000; }

static const uint8_t  kh_item_pcts[]  = {0,1,40,60,100,255};                       /* shipped: 40 Potion, 60 Hi-Potion, 100 Ether, 255 Elixir */
static const uint32_t kh_item_mults[] = {0,1,100,150,200,250,65635u,0x7fffffffu,0xffffffffu};   /* caller: 100 or 100 + u16 */
static const uint32_t kh_item_maxes[] = {0,1,60,90,120,180,255,256,1000,0x7fffffffu,0xffffffffu};
static const uint8_t  kh_item_bytes[] = {0,2,1,3,255};                             /* only 2 reverses */
#define KH_ITEM_N(a) ((int)(sizeof(a)/sizeof*(a)))
