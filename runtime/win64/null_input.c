/* null_input.c : XInput / DirectInput8 / HID null backends: no pads, no keyboard or mouse state.
 * XInputGetState/SetState (xinput1_4 ord2/ord3) report ERROR_DEVICE_NOT_CONNECTED; DirectInput8Create returns an object whose CreateDevice
 * yields devices that acquire fine and always return zeroed state, and whose EnumDevices enumerates nothing; SetupAPI/HID report no devices
 * (the raw DualShock4 driver then never starts a pad). */
#include "khnmc_rt.h"
static int C_di,C_dev;
static int h_di(KCtx *g,u64 o,int s,const char *c){ (void)o;(void)c;
    if(s==3){ wr64(g,A2(g),kh_com_obj(C_dev,16)); SET_RAX(g,0); return 1; }          /* CreateDevice(guid, **ppDevice, outer) */
    SET_RAX(g,0); return 1; }                                                          /* EnumDevices etc.: S_OK, no callbacks */
static int h_dev(KCtx *g,u64 o,int s,const char *c){ (void)o;(void)c;
    if(s==9){ memset(hp(g,A2(g)),0,(size_t)(u32)A1(g)); SET_RAX(g,0); return 1; }    /* GetDeviceState(cb, data): all zero */
    SET_RAX(g,0); return 1; }
void kh_seam_input_init(void){ C_di=kh_com_class("di:IDirectInput8",16,0,h_di); C_dev=kh_com_class("di:IDirectInputDevice8",40,0,h_dev); }
IMP(xinput1_4,ord2){ kh_cnt_input_calls++; SET_RAX(g,1167); }
IMP(xinput1_4,ord3){ kh_cnt_input_calls++; SET_RAX(g,1167); }
IMP(dinput8,DirectInput8Create){ kh_cnt_input_calls++; wr64(g,A3(g),kh_com_obj(C_di,16)); SET_RAX(g,0); }
IMP(setupapi,SetupDiGetClassDevsW){ kh_cnt_input_calls++; SET_RAX(g,(u64)-1); kh_set_last_error(13); }
IMP(setupapi,SetupDiEnumDeviceInterfaces){ kh_cnt_input_calls++; kh_set_last_error(259); SET_RAX(g,0); }
IMP(setupapi,SetupDiDestroyDeviceInfoList){ kh_cnt_input_calls++; SET_RAX(g,1); }
IMP(hid,HidD_GetHidGuid){ kh_cnt_input_calls++; memset(hp(g,A0(g)),0,16); }
