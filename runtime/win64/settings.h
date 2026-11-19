#pragma once
#include <stdint.h>
#include <stddef.h>
/* 0 success, -1 unknown setting or invalid value. */
int kh_setting_set(const char *name,const char *value);
void kh_settings_init(void);
int kh_setting_menu_count(void);
int kh_setting_action(int index);
int kh_cut_menu_request(int load,int slot);
const char *kh_cut_menu_status(void);
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
const uint8_t *kh_look_notice(void);
void kh_look_font_loaded(const char *path,const uint8_t *bar,size_t size);
extern uint8_t kh_look_font[65536];

enum { CHORD_IDLE, CHORD_PRESS, CHORD_HOLD, CHORD_CLICK };
int kh_stick_step(int *state,uint64_t *start,unsigned buttons,uint64_t now);
int kh_stick_action(uint64_t frame);
