#include "keymapping.h"

namespace {

SDL_Scancode swapScancode(SDL_Scancode scancode)
{
    switch (scancode) {
    case SDL_SCANCODE_LGUI:
        return SDL_SCANCODE_LCTRL;
    case SDL_SCANCODE_LCTRL:
        return SDL_SCANCODE_LGUI;
    case SDL_SCANCODE_RGUI:
        return SDL_SCANCODE_RCTRL;
    case SDL_SCANCODE_RCTRL:
        return SDL_SCANCODE_RGUI;
    default:
        return scancode;
    }
}

SDL_Keycode swapKeycode(SDL_Keycode keycode)
{
    switch (keycode) {
    case SDLK_LGUI:
        return SDLK_LCTRL;
    case SDLK_LCTRL:
        return SDLK_LGUI;
    case SDLK_RGUI:
        return SDLK_RCTRL;
    case SDLK_RCTRL:
        return SDLK_RGUI;
    default:
        return keycode;
    }
}

Uint16 swapModifiers(Uint16 mod)
{
    Uint16 swapped = mod & ~(KMOD_LCTRL | KMOD_RCTRL | KMOD_LGUI | KMOD_RGUI);

    if (mod & KMOD_LCTRL) {
        swapped |= KMOD_LGUI;
    }
    if (mod & KMOD_RCTRL) {
        swapped |= KMOD_RGUI;
    }
    if (mod & KMOD_LGUI) {
        swapped |= KMOD_LCTRL;
    }
    if (mod & KMOD_RGUI) {
        swapped |= KMOD_RCTRL;
    }

    return swapped;
}

}

SDL_Keysym KeyMapping::applyCmdCtrlSwap(const SDL_Keysym& keysym, bool swapCmdCtrl)
{
    if (!swapCmdCtrl) {
        return keysym;
    }

    SDL_Keysym swapped = keysym;
    swapped.scancode = swapScancode(keysym.scancode);
    swapped.sym = swapKeycode(keysym.sym);
    swapped.mod = swapModifiers(keysym.mod);
    return swapped;
}
