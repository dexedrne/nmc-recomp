#pragma once
#define KH_TITLE_LAYOUT "menu/us/title.2ld"
/* Title screens (lane TI, 2026-11-22): 0 ROXAS MAY CRY, 1 XION ALWAYS CRIES, 2 AXEL WON'T CRY.  KHNMC_TITLE_SCREEN=roxas|xion|axel
 * (khnmc.conf; default roxas) picks one, R3 on the title screen cycles through the available ones.  Each screen = a picture
 * (Roxas: remastered/menu/us/title.2ld/-4.png of the tree / rmc pack; Xion / Axel: the same path in that look's pack) + a logo
 * (<KHNMC_TITLE_LOGO_DIR>/{roxas,xion,axel}.png + .2ld).  With <logo dir>/screens.2ld every picture and logo is resident and
 * R3 switches live; without it the choice applies at the next title load (the screen's own logo .2ld, T80L). */
enum { KH_TITLE_ROXAS, KH_TITLE_XION, KH_TITLE_AXEL, KH_TITLE_SCREENS };
#define KH_TITLE_SCREENS_GEOMETRY 79092   /* byte of b"T90G" in the screens layout (tools/pack_title_screens.py) */
int kh_title_variant(int *picture);
void kh_title_logo_path(int id,const char *suffix,char *host,size_t cap);
int kh_title_screen(void);                     /* the screen shown now: the setting, Roxas when its pack is missing */
int kh_title_screen_available(int screen);     /* its picture is present (Roxas: always) */
int kh_title_screen_picture(int screen,char *host,size_t cap);   /* the host file of its picture slot (0 = the tree's own) */
int kh_title_screen_next(int screen);          /* the next available screen after `screen` (wraps; Roxas at least) */
const char *kh_title_screen_name(int screen);
