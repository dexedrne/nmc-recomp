# 3. Input: XInput / DirectInput8 / HID in kh2fm.exe, and the GameController (MFi) + touch replacement

Scope: Batch B2, item 3. Static analysis only. Machine-readable: `input_surface.json`, `input_entry_points.json`; header sketch `include/khnmc_input.h`; scripts `t3_json.py`. RVAs relative to `0x140000000`. Tags: **H** read from disassembly/data, **M** inference.

## 3.1 What the exe uses

| API | imports / sites | where | role |
|---|---|---|---|
| XInput (`xinput1_4`) | `ord2` = XInputGetState: 2 sites (0x1339de probe lambda 0x133970, 0x134d67 read 0x134d10); `ord3` = XInputSetState: 1 site (0x13253a, rumble lambda 0x132500) | pad cluster | the primary pad path (H for the ordinals' roles: argument shapes match, `ERROR_DEVICE_NOT_CONNECTED` 0x48f handled) |
| DirectInput8 (`dinput8`) | `DirectInput8Create` 1 site (0x1335ee, body 0x1335b0 = S7 `Axa.DXInput.init` 0x1335a0) | pad cluster | mouse (`GUID_SysMouse` .rdata 0x5d8590, `c_dfDIMouse` 0x5dc640), keyboard (`GUID_SysKeyboard` 0x5d85a0, `c_dfDIKeyboard` 0x5dca40), `EnumDevices(DI8DEVCLASS_GAMECTRL)` with callback 0x131ee0 -> 0x133d80 for generic joysticks; `SetCooperativeLevel` (vtbl+0x68), `Acquire` (+0x38) |
| HID (`hid.dll`, `setupapi`) | 11 + 4 imports, 62 + 5 sites (10 functions), all in 0x44bcc0..0x44ea80 | raw HID cluster 0x448000..0x450400 (152 functions, mislabelled `sd-audio` by S7) | DualShock 4 (Sony VID 0x054c, PID 0x05c4 / 0x09cc at 0x44a7f0/0x44a820/0x44a860), two reader threads (`ReadFile` + `_endthread`), output reports by `WriteFile` (rumble/light) |
| user32 | `GetForegroundWindow` (0x134ff0), `GetCursorPos` + `ScreenToClient` (0x105810), `GetWindowLongPtrA` (0x1335b0) | | focus gate, mouse pointer, hwnd for DInput |
| WMI/COM | `CoCreateInstance`, `CoSetProxyBlanket`, `SysAllocString` in 0x131ef0 | | the classic "is this DirectInput device an XInput device" test, run by the XInput probe |

Not used: `GetAsyncKeyState`, `GetKeyboardState`, raw input (`WM_INPUT`), `XInputEnable`, `XInputGetCapabilities`, `XInputGetKeystroke`; the window procedure (0x10eb40) does no key handling. Keyboard and mouse therefore arrive only through DirectInput.

## 3.2 The engine's pad abstraction (class names and layout)

S7 RTTI/lambda names: `struct GamePad`, `union Axa::PADDATA`, `Axa::DebugPad` / `DebugPadImpl` (6 virtuals, developer pad, not on the shipping path as far as call sites show), 12 `std::_Func_impl_no_alloc` lambda classes in the 0x131000..0x136000 cluster (173 functions) of three signatures: `void(GamePad*)` x5, `void(GamePad*, PADDATA&)` x4, `void(GamePad*, unsigned char, unsigned char)` x3. `Axa.DXInput.init` (0x1335a0) / `Axa.DXInput.exit` (0x133250 -> 0x133260, singleton at 0x8bb290) are named in S7; `Axa::RumblingEntry` (input) belongs to the sound manager.

Layout (H unless noted), derived from 0x133970 (probe), 0x134d10 (XInput read), 0x133e00 (PADDATA builder), 0x134ff0 (per-pad poll):

```
GamePad (0x140 bytes), array of 8 at .data 0x8bb2a8 (DXInput singleton at 0x8bb290, critical section 0x8bbcc8, hwnd 0x8bbcb8, XInput used-slot bytes[4] at 0x8bb280)
  +0x10 int   slot index (XInput user index 0..3), -1 = none
  +0x18 u64   raw button mask, PS2 bit order (below)
  +0x20 f32 rx, +0x24 ry, +0x28 lx, +0x2c ly      normalised [-1,1], Y negated (DOWN positive); PS2 pad byte order RX,RY,LX,LY (corrected: previously listed as lx,ly,rx,ry)
  +0x48 struct PadProfile { u64 id; u8 buttonMap[16] @+0x50; u8 axisMap[8] @+0x60 }   id first dword = vendor/product word (Sony 0x05c4054c / 0x09cc054c)
  +0x4c int   connection type (1 = XInput)  [0x133450(i) returns it]
  +0x68 int driver handle, +0x70 IDirectInputDevice8*, +0x7c byte present   (M)
  +0x80 std::function<void(GamePad*, PADDATA&)>        READ       (impl pointer at +0xb8; this is what 0x134ff0 calls)
  +0xc0 std::function<void(GamePad*, u8*, u8*)>        RUMBLE
  +0x100 std::function<void(GamePad*)>                 POLL / disconnect cleanup
```
`PADDATA` (partial, 0x133e00): `u16 buttons @0` (PS2 order), `u8 axes[4] @2..5` (0x80 centre, built by 0x132b50 from the axis remap bytes; digital fall-back values 4096.0f/-1.0f/1.0f), `u8 present = 1 @0x1f`, `u8 slot @0x40`; at least 0x41 bytes. 0x133e00 also clears simultaneous opposite d-pad pairs (UP+DOWN mask 0x0050, LEFT+RIGHT mask 0x00a0) after applying the 16-entry button remap table (`byte>>2` = source index, bit 1 = analog source, bit 0 = negative direction, 0xFF = identity).

### Button bit order (H) = PS2 pad word
bit 0 SELECT, 1 L3, 2 R3, 3 START, 4 UP, 5 RIGHT, 6 DOWN, 7 LEFT, 8 L2, 9 R2, 10 L1, 11 R1, 12 TRIANGLE, 13 CIRCLE, 14 CROSS, 15 SQUARE, bits 16/17 = XInput 0x0400 / 0x0800 (undefined) mapped through.

### XInput -> raw mask table in 0x134d10 (H)
DPAD_UP 0x0001 -> bit4; DPAD_DOWN 0x0002 -> bit6; DPAD_LEFT 0x0004 -> bit7; DPAD_RIGHT 0x0008 -> bit5; START 0x0010 -> bit3; BACK 0x0020 -> bit0; LEFT_THUMB 0x0040 -> bit1; RIGHT_THUMB 0x0080 -> bit2; LEFT_SHOULDER 0x0100 -> bit10; RIGHT_SHOULDER 0x0200 -> bit11; 0x0400 -> bit16; 0x0800 -> bit17; A 0x1000 -> bit14 (CROSS); B 0x2000 -> bit13 (CIRCLE); X 0x4000 -> bit15 (SQUARE); Y 0x8000 -> bit12 (TRIANGLE); left trigger >= 0x80 -> bit8 (L2); right trigger >= 0x80 -> bit9 (R2). **Triggers are digital in the engine** (threshold 0x80 of 255).
Sticks: `f = (int16 + 0.5) / 32767.5` (.rdata 0x623a4c = 0.5, 0x5aaa50 = 32767.5), Y negated, then `0x133480`: `r = sqrt(x^2+y^2)`; if `r <= 0.015748` (=2/127, constant 0x5aaa38) the pair passes through; otherwise `s = r / (dominant axis)`, `out = clamp(x*s), clamp(y*s)` to [-1,1] (dominant axis stretched to the vector length: a circle-to-square gate). The **right** stick (sThumbRX/RY at XINPUT_STATE+0x34/+0x36 relative to the state buffer at rsp+0x28) lands at +0x20/+0x24 and the **left** stick (sThumbLX/LY) at +0x28/+0x2c, matching the PS2 pad byte order RX, RY, LX, LY that 0x133e00 writes to PADDATA bytes 2..5 (corrected: the first version said left at +0x20).

### Rumble (H)
`rumble(GamePad*, u8* a, u8* b)`: XInput path builds `XINPUT_VIBRATION{ wLeftMotorSpeed = (a<<8)|a, wRightMotorSpeed = (b<<8)|b }` and calls `XInputSetState(pad->slot, ...)` (0x132500). Game side: `Axa::RumblingEntry` (ctor 0x138ab0, 4 `DebugPrint` sites) and entry points 0x1363b0 / 0x138290 / 0x138440 / 0x135a10 called from settingmenu and axa-platform (the settings pad-vibration test) and the per-frame sound manager.

### Polling model (H)
No events, no callbacks. Every frame the AppInterface input update **0x105810** (1,521 bytes: also the mouse via `GetCursorPos`/`ScreenToClient`, the Steam controller-type probe 0x14d2c0 and window-size query) calls **`0x134ff0(padIndex, PADDATA*)`** for each pad. For pad 0, 0x134ff0 first runs the keyboard-as-pad reader (`lambda_b59f6af6` body 0x132f90, on the DXInput singleton); then, **only if `GetForegroundWindow() == hwnd` and the slot is valid (`[pad+0x48] != 0`, `[pad+0x10] >= 0`)**, it invokes the pad's READ function. Hot-plug: when a slot is empty, the probe lambda 0x133970 scans the four XInput indices, skips controllers whose device word is a Sony DS4 id, marks `g_xinputUsed[slot]`, and installs the three std::function objects (XInput set: read 0x1325a0 -> 0x134d10, rumble 0x132500, poll 0x1325f0 which clears the used byte). The DInput set (read `dd31243a` 0x1327e0 -> 0x134430, 2,260 bytes; cleanup 0x1325b0 = Unacquire/Release of `[pad+0x70]`), the raw-HID set (read `7d3ebf57` 0x132620, rumble 0x1324c0, poll 0x132550) and the keyboard set (read 0x132f90, rumble/poll `ret` stubs) follow the same shape.

### Keyboard and mouse (H)
DirectInput keyboard (scancode state) and mouse (relative state). The only mouse consumers are `Axa::AppInterface` 0x105810 (pointer position for the settings menu) and `SettingMenu` (0x39bf00, 0x4edd60 call it). Key bindings are DIK codes in `PCSettings` (default table written by 0x14ce50: +0x24 onward, e.g. 0x38 = DIK_LMENU, 0x1c = DIK_RETURN, 0x39 = DIK_SPACE, 0x0e = DIK_BACK, 0x2a = DIK_LSHIFT, 0x36 = DIK_RSHIFT, 0x9d = DIK_RCONTROL; M for the exact meaning of each slot) together with 8 pad-config entries of 0x20 bytes at +0x104 (first dword 0xffff, remainder 0xff = identity map). The settings classes involved: `SettingMenu::CGamePadMenu`, `CKeyboardMenu`, `CKeyboardType`, `CKeyConfig`, `CPadCalibration`, `CButtonIcon`, `CButttonGuide`.

## 3.3 Replacement: GameController (MFi) first, minimal touch overlay

Goal: the lifted `AppInterface` input update, `DXInput` pad table and PADDATA conversion keep running; only the backends change.

1. **Do not run** `DXInput.init` (DirectInput/HID/WMI). Create the 8-slot pad table (`.data 0x8bb2a8`) and the DXInput singleton fields the lifted code reads (hwnd global 0x8bbcb8 can hold a dummy and `GetForegroundWindow` is replaced by a function that returns it: input must never be gated off).
2. Provide **one backend set** (`PadSource` in `include/khnmc_input.h`) that implements the three std::function signatures:
   - READ: fill `RawPadState` (`buttons`, `rx ry lx ly` in that memory order, Y negated = down positive), run the engine's own `0x133480` stick shaping and `0x133e00` conversion, set `PADDATA[0x40] = slot`.
   - RUMBLE: two `uint8` motor levels -> `CHHapticEngine` (GCController.haptics) with locality `.leftHandle` / `.rightHandle` where available, otherwise default; scale 0..255 -> 0..1.
   - POLL/cleanup: release the slot.
3. **GCExtendedGamepad mapping** (MFi, DualSense, DualShock 4, Xbox, Switch Pro share it): A -> CROSS (14), B -> CIRCLE (13), X -> SQUARE (15), Y -> TRIANGLE (12), left/right shoulder -> L1 (10) / R1 (11), left/right trigger (analog >= 0.5, the engine's own threshold) -> L2 (8) / R2 (9), buttonMenu -> START (3), buttonOptions (or the touchpad button on DualShock/DualSense) -> SELECT (0), thumbstick buttons -> L3 (1) / R3 (2), d-pad -> UP/DOWN/LEFT/RIGHT (4/6/7/5), left/right thumbstick -> (x, -y) per stick. Labels differ on Nintendo layouts (A/B swapped); map by physical position, not by label. buttonHome stays with the system. Note `GCController.physicalInputProfile` for non-extended controllers (MFi micro/standard): degrade L2/R2 to unavailable.
4. **Touch overlay** (only when no GCController is connected, hidden otherwise): left floating stick (lx, ly), four face buttons (CIRCLE, CROSS, SQUARE, TRIANGLE), L1/R1/L2/R2 shoulder pads, START, SELECT, and an optional d-pad for menu navigation; a long-press on the right half can map to the camera (R1/L1 pair as on the PS2 layout). Layout data in `TouchLayout`; the engine only sees the same PADDATA. Haptic feedback via UIImpactFeedbackGenerator is optional.
5. **Menus**: hide or stub the PC-only settings (`CKeyboardMenu`, `CKeyConfig`, `CKeyboardType`, `CPadCalibration`, `CResolution`, `CScreenMode`, `CVSyncMode`) since they edit PC fields of `PCSettings`; keep `CGamePadMenu` / `CButtonIcon` limited to vibration and glyph style. The glyph set the game shows comes from `ISteamInput` controller type (0x14d2c0, returns 1, 2, 3 or 5; default 5); the port should return the glyph family of the connected GCController (see 04-STEAM-FACTS.md: this is a Steam dependency the owner has not ruled on).
6. **Do not add a second dead zone**: the engine already shapes the sticks (3.2). Feed the raw GC axis values.

## 3.4 Open items
- Exact `PADDATA` size/union and every consumer field beyond buttons+axes (the 0x105810 consumers publish into game structures not traced here).
- The DInput read (0x134430, 2,260 bytes) and the HID read (0x132620) were not decoded; they are not needed for MFi.
- Whether the lifted code calls `0x134ff0` for pads 1..7 in single-player (it is called per pad index; slot assignment policy of `0x105810` not traced).
