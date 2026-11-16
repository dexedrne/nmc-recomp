// KHNMC DECOMP EDITION - platform seam: INPUT (GameController/MFi + minimal touch overlay). Design sketch, not compiled.
// Evidence: ../03-INPUT.md, ../input_surface.json
#pragma once
#include <cstdint>

namespace khnmc::input {

// Engine pad word: PS2 order. Verified from the XInput read function 0x134d10 (bit tests) and 0x133e00 (16-bit PADDATA).
enum PadBit : uint8_t {
  kSelect = 0, kL3 = 1, kR3 = 2, kStart = 3, kUp = 4, kRight = 5, kDown = 6, kLeft = 7,
  kL2 = 8, kR2 = 9, kL1 = 10, kR1 = 11, kTriangle = 12, kCircle = 13, kCross = 14, kSquare = 15,
  kExtra0 = 16, kExtra1 = 17 };                       // 16/17 = XInput 0x0400/0x0800 (undefined), never set by MFi

// Raw per-pad state the lifted code reads (GamePad+0x18 .. +0x2c; pads = 8 x 0x140 bytes at .data 0x8bb2a8). The port's `read` function
// (the std::function at GamePad+0x80, rumble at +0xC0, poll/cleanup at +0x100) fills this and calls the lifted 0x133e00 to produce PADDATA.
struct RawPadState {
  uint64_t buttons;                                   // GamePad+0x18, bits as above
  float    rx, ry, lx, ly;                            // GamePad+0x20..0x2c in PS2 order RX,RY,LX,LY (corrected; first draft had lx,ly,rx,ry), [-1,1], Y axes: DOWN POSITIVE (the exe negates XInput Y)
};
// PADDATA as produced by 0x133e00 (partial): u16 buttons @0; u8 axes[4] @2..5 (centre 0x80); u8 present @0x1f; u8 slot @0x40.
struct PadData { uint16_t buttons; uint8_t axes[4]; uint8_t pad[0x38]; };    // size >= 0x41, full union not recovered

// Stick shaping done by the engine itself (0x133480, applied after (v+0.5)/32767.5 and the Y negation): r = sqrt(x^2+y^2);
// if r <= 0.015748 (=2/127) the pair is passed through; otherwise s = r / max(|x|,|y|)-axis value and both axes become
// clamp(x*s), clamp(y*s) to [-1,1]  (dominant axis is stretched to the vector length: circle-to-square gate).
// => the port should feed RAW normalised GC stick values and let the lifted 0x133480 do the shaping; apply NO extra dead zone.
// Triggers are DIGITAL in the engine: L2/R2 set when trigger >= 0x80 (0.5). XInput default mapping:
//   A->Cross B->Circle X->Square Y->Triangle LB->L1 RB->R1 LT->L2 RT->R2 Back->Select Start->Start LS->L3 RS->R3 dpad->dpad.

// GameController (GCController / GCExtendedGamepad) mapping, extended profile (MFi, DualSense, Xbox, Switch Pro all expose it):
//   buttonA -> kCross(14)   buttonB -> kCircle(13)   buttonX -> kSquare(15)   buttonY -> kTriangle(12)
//   leftShoulder -> kL1(10) rightShoulder -> kR1(11) leftTrigger(analog>=0.5) -> kL2(8) rightTrigger -> kR2(9)
//   buttonMenu -> kStart(3) buttonOptions -> kSelect(0) leftThumbstickButton -> kL3(1) rightThumbstickButton -> kR3(2)
//   dpad -> kUp/kDown/kLeft/kRight (opposite pairs cleared by the engine)
//   leftThumbstick -> (lx,ly = x, -y)   rightThumbstick -> (rx,ry = x, -y)     (GC y is up-positive, engine wants down-positive)
//   buttonHome: system (do not map).  Cross/Circle swap: the PC build uses Cross=confirm; honour the engine's own button-config.
enum class Profile : uint8_t { Extended, DualSenseLike, Touch };

struct TouchLayout {                                  // minimal on-screen overlay (only shown when no GCController is connected)
  struct Region { float cx, cy, r; PadBit bit; };    // normalised screen coords, circles
  static constexpr int kMaxRegions = 16;
  Region regions[kMaxRegions]; int count = 0;
  float stickLeftCx = 0.14f, stickLeftCy = 0.72f, stickR = 0.11f;     // virtual left stick (floating origin) -> lx,ly
  // right stick optional (camera): second floating stick or two-finger drag; default: none (camera on L1/R1 as the game does)
};

class PadSource {
 public:
  virtual ~PadSource() = default;
  virtual bool connected() const = 0;
  virtual void poll(RawPadState*) = 0;                 // called once per frame before the lifted DXInput update (poll model verified: per frame, no events)
  virtual void rumble(uint8_t smallMotor, uint8_t largeMotor) = 0;   // engine passes two uint8 (XInput wLeft/wRight = v*257)
};
PadSource* createGameControllerSource(int slot);       // GCController, rumble via CHHapticEngine (GCHaptics)
PadSource* createTouchSource(const TouchLayout&);

// Keyboard/mouse: DirectInput keyboard is used only for the PC key-config path and the settings-menu mouse pointer. On iOS these
// are not exposed; the settings menu (SettingMenu::CKeyboardMenu/CKeyConfig/CGamePadMenu/CPadCalibration) is hidden or stubbed.
}  // namespace khnmc::input
