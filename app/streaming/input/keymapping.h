#pragma once

#include "SDL_compat.h"

namespace KeyMapping
{

// If swapCmdCtrl is set, returns the keysym with Command (GUI) and Control swapped:
// the LGUI/LCTRL and RGUI/RCTRL scancodes and keycodes, and the KMOD_LGUI/KMOD_LCTRL
// and KMOD_RGUI/KMOD_RCTRL modifier bits. All other keys and modifiers are unchanged.
// If swapCmdCtrl is not set, returns the keysym unchanged.
// Used on macOS so that Cmd+C/V/Z/A arrive on Linux/Windows hosts as Ctrl+C/V/Z/A.
SDL_Keysym applyCmdCtrlSwap(const SDL_Keysym& keysym, bool swapCmdCtrl);

}
