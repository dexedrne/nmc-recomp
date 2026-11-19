#pragma once
#include <stdint.h>
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
