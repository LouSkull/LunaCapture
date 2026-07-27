# Compact-keyboard shortcut recorder

LunaShot remains visually and technically version `1.0.0.0`.

## Input paths

Shortcut learning now listens through:

1. `WH_KEYBOARD_LL` for global Windows keyboard events;
2. Raw Input with `RIDEV_INPUTSINK` for physical keyboard make codes;
3. `WM_APPCOMMAND` for common media and browser keys;
4. focused-window `WM_KEYDOWN` as a final fallback.

## Fn behavior

Most compact keyboards do not expose `Fn` to Windows. The keyboard firmware transforms an Fn combination into a normal Windows key before the operating system receives it. LunaShot records that resulting key. For example, `Fn + P` can be stored and displayed as `Print Screen`.

Keys that only control keyboard hardware, such as backlight brightness or an onboard profile, cannot be assigned because no Windows input event exists.
