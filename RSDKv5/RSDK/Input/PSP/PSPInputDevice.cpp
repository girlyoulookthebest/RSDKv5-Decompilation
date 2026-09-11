using namespace RSDK;
#include <pspctrl.h>
#include <psppower.h>

InputState mappings[12];

static int32 remap[12] = {
  PSP_CTRL_UP,
  PSP_CTRL_DOWN,
  PSP_CTRL_LEFT,
  PSP_CTRL_RIGHT,
  PSP_CTRL_CROSS,
  PSP_CTRL_CIRCLE,
  NULL,
  PSP_CTRL_SQUARE,
  PSP_CTRL_TRIANGLE,
  NULL,
  PSP_CTRL_START,
  PSP_CTRL_SELECT
};

int32 last_buttons = 0;

// Wider than the engine's 0.3: a PSP stick is rarely centred and drifts as it
// wears, and a false direction in a menu is far more noticeable than a slightly
// larger dead area in gameplay.
#define PSP_STICK_DEADZONE 0.3f

// A stick direction engages past ENGAGE and releases only below RELEASE. With a
// single threshold, a stick resting near it crosses it again and again, and
// each crossing is a fresh press to a menu -- the cursor moved on its own.
#define PSP_STICK_ENGAGE  0.5f
#define PSP_STICK_RELEASE 0.3f
#define PSP_STICK_LOG 0

void RSDK::SKU::InputDevicePSP::UpdateInput()
{
  SceCtrlData ctrl_data;
  sceCtrlPeekBufferPositive(&ctrl_data, 1);
  
  int32 kDown  = ctrl_data.Buttons;
  int32 kPress = kDown & ~last_buttons; // rising edge: down now, wasn't down last frame

  if (kDown)
    this->anyPress = 1;

  for (int i = 0; i < 12; i++) {
    mappings[i].down = kDown & remap[i];
    mappings[i].press = kPress & remap[i];
  }
  // The analog stick reports 0..255 per axis, centred at 128. Ly grows
  // downwards, so it is negated to match RSDK's convention of positive = up.
  // Scaled by 128 so a fully deflected stick reaches 1.0.
  this->hDelta_L = ((float)ctrl_data.Lx - 128.0f) / 128.0f;
  this->vDelta_L = -((float)ctrl_data.Ly - 128.0f) / 128.0f;

  // Playing with only the stick dimmed the screen on hardware until a button
  // was pressed: the firmware's idle timer is reset by buttons, evidently not
  // by the stick. Tick it while the stick is held, which is the reset a button
  // press gives -- and nothing while the pad is untouched, so sleep still works.
  if (this->hDelta_L > PSP_STICK_DEADZONE || this->hDelta_L < -PSP_STICK_DEADZONE || this->vDelta_L > PSP_STICK_DEADZONE
      || this->vDelta_L < -PSP_STICK_DEADZONE)
    scePowerTick(PSP_POWER_TICK_ALL);

#if PSP_STICK_LOG
  {
      static int32 n = 0;
      static uint8 loX = 255, hiX = 0, loY = 255, hiY = 0;
      if (ctrl_data.Lx < loX) loX = ctrl_data.Lx;
      if (ctrl_data.Lx > hiX) hiX = ctrl_data.Lx;
      if (ctrl_data.Ly < loY) loY = ctrl_data.Ly;
      if (ctrl_data.Ly > hiY) hiY = ctrl_data.Ly;
      if (++n == 600) {
          FILE *sf = fopen("stick.log", "w");
          if (sf) {
              fprintf(sf, "over 600 frames: Lx %d..%d   Ly %d..%d  (centre should be ~128)\n", (int)loX, (int)hiX, (int)loY, (int)hiY);
              fprintf(sf, "as deltas: h %.3f..%.3f  v %.3f..%.3f  (deadzone %.2f)\n",
                      (loX - 128.0f) / 128.0f, (hiX - 128.0f) / 128.0f,
                      -(hiY - 128.0f) / 128.0f, -(loY - 128.0f) / 128.0f, PSP_STICK_DEADZONE);
              fclose(sf);
          }
      }
  }
#endif

  last_buttons = ctrl_data.Buttons;
}

// RSDK::ProcessInput() (Input.cpp) runs a shared edge-detector over every
// device's output: it expects each backend to report the RAW "is this button
// held right now" state into .press every frame, and it derives the real
// one-frame .press / sustained .down pair from that itself (see the
// `if (cont[i]->press) { if (cont[i]->down) cont[i]->press = false; else
// cont[i]->down = true; } else cont[i]->down = false;` loop). Feeding it
// mappings[i].press (which is only true on the single rising-edge frame, per
// UpdateInput's kPress calculation) instead of mappings[i].down (true for as
// long as the button stays held) meant every button read as released again
// one frame after being pressed -- held input like walking never sustained.
void RSDK::SKU::InputDevicePSP::ProcessInput(int32 controllerID)
{
  static bool stickUp = false, stickDown = false, stickLeft = false, stickRight = false;
  stickUp    = this->vDelta_L > (stickUp ? PSP_STICK_RELEASE : PSP_STICK_ENGAGE);
  stickDown  = this->vDelta_L < -(stickDown ? PSP_STICK_RELEASE : PSP_STICK_ENGAGE);
  stickLeft  = this->hDelta_L < -(stickLeft ? PSP_STICK_RELEASE : PSP_STICK_ENGAGE);
  stickRight = this->hDelta_L > (stickRight ? PSP_STICK_RELEASE : PSP_STICK_ENGAGE);

  for (int i = 0; i < PLAYER_COUNT; i++) {
    if (i == 2)
      continue;

    controller[i].keyUp.press       |= mappings[0].down;
    controller[i].keyDown.press     |= mappings[1].down;
    controller[i].keyLeft.press     |= mappings[2].down;
    controller[i].keyRight.press    |= mappings[3].down;
    controller[i].keyA.press        |= mappings[4].down;
    controller[i].keyB.press        |= mappings[5].down;
    controller[i].keyC.press        |= mappings[6].down;
    controller[i].keyX.press        |= mappings[7].down;
    controller[i].keyY.press        |= mappings[8].down;
    controller[i].keyZ.press        |= mappings[9].down;
    controller[i].keyStart.press    |= mappings[10].down;
    controller[i].keySelect.press   |= mappings[11].down;

    // The stick drives its own direction keys, as the engine's other backends
    // do, rather than the d-pad. Player.c and UFO_Player.c read stick->keyUp
    // alongside the d-pad, so gameplay is unaffected.
    stickL[i].keyUp.press    |= stickUp;
    stickL[i].keyDown.press  |= stickDown;
    stickL[i].keyLeft.press  |= stickLeft;
    stickL[i].keyRight.press |= stickRight;
  }

  // Centre the magnitudes too, not just the derived directions. A PSP stick
  // rarely rests at exactly 128 on both axes, so publishing the raw value
  // leaves a permanent small deflection for anything that reads it.
  float hOut = this->hDelta_L;
  float vOut = this->vDelta_L;

  if (hOut > -PSP_STICK_DEADZONE && hOut < PSP_STICK_DEADZONE)
      hOut = 0.0f;
  if (vOut > -PSP_STICK_DEADZONE && vOut < PSP_STICK_DEADZONE)
      vOut = 0.0f;

  stickL[controllerID].hDelta = hOut;
  stickL[controllerID].vDelta = vOut;
}

// code below here borrowed liberally from the other backends and
// modified accordingly
RSDK::SKU::InputDevicePSP *RSDK::SKU::InitPSPDevice(uint32 id) {
  if (inputDeviceCount == INPUTDEVICE_COUNT)
    return NULL;

  if (inputDeviceList[inputDeviceCount] && 
      inputDeviceList[inputDeviceCount]->active)
    return NULL;

  if (inputDeviceList[inputDeviceCount])
    delete inputDeviceList[inputDeviceCount];

  inputDeviceList[inputDeviceCount] = new InputDevicePSP();
  sceCtrlSetSamplingCycle(0);
  sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

  InputDevicePSP *device = (InputDevicePSP*) inputDeviceList[inputDeviceCount];
  device->gamepadType = (DEVICE_API_NONE << 16) | (DEVICE_TYPE_CONTROLLER << 8) | (DEVICE_PS4 << 0);
  device->disabled = false;
  device->id = id;
  device->active = true;
  device->anyPress = 1;
  device->hDelta_L = 0.0f;
  device->vDelta_L = 0.0f;

  inputSlots[0] = device->id;
  inputSlotDevices[0] = device;
  device->isAssigned = true;
  
  inputDeviceCount++;
  return device;
}

void RSDK::SKU::InitPSPInputAPI() {
  uint32 id = 1;
  GenerateHashCRC(&id, "PSPDevice0");

  inputDeviceCount = 0;

  InputDevicePSP* device = InitPSPDevice(id);
  if (device) {
    device->controllerID = 1;
  }

  return;
}
