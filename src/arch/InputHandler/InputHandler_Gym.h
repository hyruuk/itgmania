#ifndef INPUT_HANDLER_GYM_H
#define INPUT_HANDLER_GYM_H

#include <vector>

#include "InputHandler.h"

/* The only input driver of a gym process (InputDrivers=Gym). It reads no
 * device: a gamepad on the machine must reach the game only through the
 * program driving the gym, which presses the dance buttons itself, at the
 * times its steps say. It names the keyboard those presses come from. */
class InputHandler_Gym : public InputHandler {
 public:
  void GetDevicesAndDescriptions(std::vector<InputDeviceInfo>& vDevicesOut);
};

#endif
