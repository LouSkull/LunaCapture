# Bundled LunaScript plugins

Five ready-to-use starting points for the Luna Capture plugin SDK. Each file is a complete, working
plugin — copy it, change the id, and edit. Every plugin here is parsed by the shipping LunaScript
parser as part of the test suite (`LunaShotPluginExampleTests`), so they cannot silently rot.

## Installing any of these

1. Copy the `.luna` file into `%LOCALAPPDATA%\Luna Capture\Plugins\`
2. Open Luna Capture → **Plugins** → **Reload**
3. Review the permission list, then enable the plugin
4. For themes and locales, use the plugin's **Manage** menu to activate it

## The plugins

| File | What it gives you | Permissions |
| --- | --- | --- |
| [`translation-base.luna`](translation-base.luna) | **Every user-facing string in the app** (724 entries) as a fill-in-the-blank locale — the base for translating Luna into any language | `localization` |
| [`quick-panel-essentials.luna`](quick-panel-essentials.luna) | Region capture, replay save, recording toggle and folder shortcuts on the Quick Panel and tray, so you never alt-tab mid-game | `capture` `replay` `recording` `open_folders` `notifications` `ui` |
| [`session-watchdog.luna`](session-watchdog.luna) | Turns the quiet recording/Replay health events (recovering, recovered, failed) into overlay toasts so a capture hiccup is noticed immediately | `notifications` |
| [`capture-presets.luna`](capture-presets.luna) | One-press setting bundles: share (1080p60 H.264), archive (native HEVC), fast JPEG screenshots, lossless PNG screenshots | `settings` `notifications` |
| [`theme-starter.luna`](theme-starter.luna) | The complete semantic colour set filled in with a coherent dark palette — recolour in place instead of starting from an empty theme block | `theme` |

## Translating Luna Capture

`translation-base.luna` is the one to start from. It contains every string the interface can show,
grouped by the area it belongs to (main window, dialogs, Quick Panel, region editor, tray, and so on),
written as identity pairs:

```
"Capture instantly. Keep moving." = "Capture instantly. Keep moving.";
```

Translate only the **right** side. The left side is the lookup key and must stay byte-for-byte
identical to the English text. Untranslated entries can stay as they are, so a partial translation
works fine and you can fill it in gradually, reloading as you go.

A few practical notes:

- Strings containing `{0}` or `{1}` are templates — the app substitutes a file name, monitor number
  or count at runtime. Keep the placeholders; you may reorder them to suit your grammar.
- Only `\n`, `\t`, `\\` and `\"` are valid escapes.
- A locale holds at most 1024 entries and each string is capped at 512 characters. The base file uses
  724 entries, so there is room for the app to grow.
- The native UI does not reflow around longer text. Short labels drawn into fixed-width controls will
  clip, so prefer a translation of similar length and check it in the app.

## Writing your own

The complete language reference — every instruction, event, permission, command property, and all 65
theme properties — is in the SDK documentation: [`website/sdk.html`](../website/sdk.html), which also
ships offline with the app and opens from the **SDK** button on the Plugins page.

Two constraints worth knowing before you start:

- A plugin is **data, not code**. It cannot call Win32, read frames, touch arbitrary paths, or reach
  the network. Everything it does goes through the host's own guarded paths.
- Every top-level execution chain runs on a 1024-instruction budget, and a single action block holds
  at most 128 instructions.
