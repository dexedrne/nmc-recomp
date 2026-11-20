#pragma once
#include <stdint.h>
#include <stddef.h>
/* 0 success, -1 unknown setting or invalid value. */
int kh_setting_set(const char *name,const char *value);
void kh_menu_input_device(const char *device,const char *element);
unsigned short kh_menu_map_buttons(unsigned short buttons,int nintendo);
const char *kh_menu_choice(const char *name);
void kh_settings_init(void);
int kh_setting_menu_count(void);
int kh_setting_action(int index);
const char *kh_setting_unavailable(int index);
int kh_cut_menu_request(int load,int slot);
const char *kh_cut_menu_unavailable(void);
const char *kh_cut_menu_status(void);
enum { KH_TITLE_CLOSED, KH_TITLE_LIST, KH_TITLE_SETTINGS };
void kh_title_pad_family(int nintendo,int playstation);
void kh_menu_title(int screen);
int kh_title_settings_key(void);
int kh_menu_active(void);
struct KCtx;
int kh_menu_pad(struct KCtx *context,unsigned buttons,int lt,int rt,int lx,int ly,int rx,int ry);
enum { KH_MENU_TEXT_SIZE=768 };
int kh_menu_text(uint8_t text[KH_MENU_TEXT_SIZE]);
uint32_t kh_menu_texel(unsigned x,unsigned y,uint32_t bg);
float kh_drop_chance(float chance);
struct kh_option {
    const char *name,*label,*initial,*choices;
    double minimum,maximum,step;
};
extern const struct kh_option kh_options[];
extern const int kh_option_count;
int kh_setting_live(int index);
double kh_live_setting_value(int index);

int kh_settings_look(void);
void kh_settings_reset_look(void);
int kh_settings_hooded(void);
const uint8_t *kh_look_notice(void);
void kh_look_font_loaded(const char *path,const uint8_t *bar,size_t size);
extern uint8_t kh_look_font[65536];

enum { CHORD_IDLE, CHORD_PRESS, CHORD_HOLD, CHORD_CLICK };
int kh_stick_step(int *state,uint64_t *start,unsigned buttons,uint64_t now);
int kh_stick_action(uint64_t frame);
