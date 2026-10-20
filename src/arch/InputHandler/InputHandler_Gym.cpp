#include "InputHandler_Gym.h"

#include <vector>

#include "RageInputDevice.h"

REGISTER_INPUT_HANDLER_CLASS(Gym);

void InputHandler_Gym::GetDevicesAndDescriptions(
    std::vector<InputDeviceInfo>& vDevicesOut) {
  vDevicesOut.push_back(InputDeviceInfo(DEVICE_KEYBOARD, "Gym"));
}
