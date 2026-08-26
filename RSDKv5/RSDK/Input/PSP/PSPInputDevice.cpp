using namespace RSDK;
#include <pspctrl.h>

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
  }
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
