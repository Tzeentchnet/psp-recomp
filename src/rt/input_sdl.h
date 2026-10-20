#ifndef PSP_RECOMP_INPUT_SDL_H
#define PSP_RECOMP_INPUT_SDL_H

#include "input_config.h"

#include <SDL3/SDL.h>

static inline SDL_Scancode sr_input_sdl_scancode(int key) {
    if (key >= 'A' && key <= 'Z')
        return (SDL_Scancode)(SDL_SCANCODE_A + key - 'A');
    if (key >= '0' && key <= '9') {
        if (key == '0') return SDL_SCANCODE_0;
        return (SDL_Scancode)(SDL_SCANCODE_1 + key - '1');
    }
    switch (key) {
    case SR_KEY_ENTER: return SDL_SCANCODE_RETURN;
    case SR_KEY_LEFT_SHIFT: return SDL_SCANCODE_LSHIFT;
    case SR_KEY_RIGHT_SHIFT: return SDL_SCANCODE_RSHIFT;
    case SR_KEY_UP: return SDL_SCANCODE_UP;
    case SR_KEY_RIGHT: return SDL_SCANCODE_RIGHT;
    case SR_KEY_DOWN: return SDL_SCANCODE_DOWN;
    case SR_KEY_LEFT: return SDL_SCANCODE_LEFT;
    case SR_KEY_SPACE: return SDL_SCANCODE_SPACE;
    case SR_KEY_TAB: return SDL_SCANCODE_TAB;
    case SR_KEY_BACKSPACE: return SDL_SCANCODE_BACKSPACE;
    case SR_KEY_LEFT_CTRL: return SDL_SCANCODE_LCTRL;
    case SR_KEY_RIGHT_CTRL: return SDL_SCANCODE_RCTRL;
    default: return SDL_SCANCODE_UNKNOWN;
    }
}

static inline int sr_input_sdl_key_down(void *context, int key) {
    const bool *keys = (const bool *)context;
    SDL_Scancode scancode = sr_input_sdl_scancode(key);
    return scancode != SDL_SCANCODE_UNKNOWN && keys[scancode];
}

static inline int sr_input_sdl_pad_down(void *context, int button) {
    SDL_Gamepad *pad = (SDL_Gamepad *)context;
    switch (button) {
    case SR_PAD_SOUTH:
        return SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_SOUTH);
    case SR_PAD_EAST:
        return SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_EAST);
    case SR_PAD_WEST:
        return SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_WEST);
    case SR_PAD_NORTH:
        return SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_NORTH);
    case SR_PAD_START:
        return SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_START);
    case SR_PAD_BACK:
        return SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_BACK);
    case SR_PAD_LEFT_SHOULDER:
        return SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
    case SR_PAD_RIGHT_SHOULDER:
        return SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
    case SR_PAD_LEFT_TRIGGER:
        return SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 8192;
    case SR_PAD_RIGHT_TRIGGER:
        return SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 8192;
    case SR_PAD_DPAD_UP:
        return SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_UP);
    case SR_PAD_DPAD_RIGHT:
        return SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    case SR_PAD_DPAD_DOWN:
        return SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    case SR_PAD_DPAD_LEFT:
        return SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT);
    case SR_PAD_LEFT_STICK:
        return SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_LEFT_STICK);
    case SR_PAD_RIGHT_STICK:
        return SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_RIGHT_STICK);
    default:
        return 0;
    }
}

#endif
