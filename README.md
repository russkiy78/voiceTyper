# voiceTyper

Local, privacy-friendly **voice typing** for the desktop. Put the cursor in any
text field, press a global hotkey, speak, and the recognized text is pasted into
the active field. Speech recognition runs **entirely locally** via
[whisper.cpp](https://github.com/ggerganov/whisper.cpp) — no Python, no cloud, no
external services.

- **Stack:** C++20 · Qt 6 · CMake · whisper.cpp (bundled, statically linked)
- **Platforms:** Ubuntu/Linux (X11) and Windows. macOS is architected for but not
  yet wired up (see [macOS status](#macos-status)).

---

## How it works

1. You place the cursor in any text field.
2. You press the global hotkey (default **Ctrl+Alt+Space**).
3. Recording starts; a small overlay appears in the corner with a timer + level meter.
4. You speak.
5. You stop by pressing the hotkey again **or** by saying a stop word (default **«стоп»**).
6. The audio is transcribed locally by whisper.cpp.
7. Voice **commands** (e.g. «новая строка» → newline) are applied to the text.
8. The result is placed on the clipboard, the paste shortcut is synthesized
   (`Ctrl+V` / `Cmd+V`), and your previous clipboard is restored shortly after.

---

## Module architecture

```
AppController            end-to-end coordinator (the pipeline)
├─ RecordingController   recording state machine
│   ├─ AudioRecorder         QAudioSource capture -> 16 kHz mono float
│   └─ CommandDetectionLoop  live "stop" detection while recording
├─ WhisperAsrEngine     whisper.cpp wrapper (IAsrEngine; NullAsrEngine fallback)
├─ CommandEngine        phrase/regex command matching + text reconstruction
├─ ClipboardPasteService save clipboard -> set text -> paste -> restore
│   └─ KeyboardPaster        platform keystroke synth (X11 / Win / mac)
├─ HotkeyService        global hotkey (X11 XGrabKey / Win RegisterHotKey / mac TODO)
├─ TextPostProcessor    NoOp now; HttpTextPostProcessor skeleton for future LLM cleanup
├─ SettingsStore        QSettings + commands.json
└─ UI: OverlayWindow · TrayController · SettingsWindow
```

Source lives under `src/` grouped by module (`app/`, `audio/`, `asr/`, `commands/`,
`clipboard/`, `hotkey/`, `postprocess/`, `settings/`, `ui/`, `core/`).

---

## Prerequisites

### Common
- CMake ≥ 3.21
- A C++20 compiler (GCC 11+, Clang 14+, or MSVC 2022)
- Qt 6 with the **Core, Gui, Widgets, Multimedia, Network** modules
- Git and a network connection the first time you build (to fetch whisper.cpp),
  unless you provide a local checkout (see [whisper.cpp](#whispercpp-dependency)).

### Ubuntu / Linux extras (X11)
The global hotkey and paste-keystroke synthesis use X11:

```bash
sudo apt install build-essential cmake git \
    libx11-dev libxtst-dev libxcb1-dev \
    libasound2-dev libpulse-dev    # for Qt Multimedia audio capture
```

If you installed Qt via the online installer (e.g. `~/Qt/6.11.1/gcc_64`), point
CMake at it with `-DCMAKE_PREFIX_PATH`. With a distro Qt6 (`qt6-base-dev`,
`qt6-multimedia-dev`) it is found automatically.

> **Wayland note:** keystroke synthesis (XTEST) and `XGrabKey` work for X11 and
> XWayland windows. Native Wayland sessions restrict global input by design; run
> an **X11 session** for full functionality on Wayland desktops for now. A native
> Wayland backend is a TODO.

### Windows extras
- Visual Studio 2022 (Desktop C++), CMake, Qt 6 (MSVC build). No extra system
  libraries are needed — the hotkey/paste use the Win32 API directly.

---

## whisper.cpp dependency

By default the build **fetches whisper.cpp** (`v1.7.6`) via CMake `FetchContent`
and links it statically — nothing extra to install. Resolution order:

1. `-DWHISPER_CPP_SOURCE_DIR=/path/to/whisper.cpp` (your local checkout)
2. `third_party/whisper.cpp/` if you vendor it (e.g. as a git submodule)
3. FetchContent from GitHub (needs network at configure time)

To build **without** any ASR backend (UI/plumbing only, uses `NullAsrEngine`):
`-DVOICETYPER_WITH_WHISPER=OFF`.

---

## Get a speech model

whisper.cpp needs a GGML model file. Download one with the helper script:

```bash
# Linux/macOS — default is small-q5_1 (~180 MB, multilingual)
scripts/download-model.sh
scripts/download-model.sh large-v3-turbo-q5_0   # recommended target (~570 MB)
```

```powershell
# Windows
./scripts/download-model.ps1
./scripts/download-model.ps1 large-v3-turbo-q5_0
```

Models land in `./models`. The app auto-detects `models/ggml-*.bin`, or you can
pick the file in **Settings → Whisper model**.

> **MVP recommendation:** `large-v3-turbo-q5_0` if the package size is acceptable;
> otherwise `medium-q5_0` or `small-q5_1`. The architecture supports swapping
> models later — only the path in Settings changes.

---

## Build & run — Ubuntu

```bash
git clone <this-repo> voiceTyper && cd voiceTyper

cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=$HOME/Qt/6.11.1/gcc_64      # omit if using distro Qt6

cmake --build build -j

scripts/download-model.sh                          # one-time model download
./build/voiceTyper
```

The app starts in the system tray (no main window). Left-click the tray icon or
press the hotkey to start/stop dictation; right-click for the menu.

## Build & run — Windows

```powershell
git clone <this-repo> voiceTyper; cd voiceTyper

cmake -S . -B build -DCMAKE_PREFIX_PATH="C:\Qt\6.11.1\msvc2022_64"
cmake --build build --config Release

./scripts/download-model.ps1
./build/Release/voiceTyper.exe
```

If Qt DLLs are not found at runtime, run `windeployqt` against the produced
`voiceTyper.exe`, or add the Qt `bin` directory to `PATH`.

---

## First run

1. Open **Settings** from the tray menu.
2. Choose your **recognition language** (default: Russian) — or `Auto-detect`.
3. Confirm/choose the **whisper model** path.
4. Set your **global hotkey** (default `Ctrl+Alt+Space`).
5. Review the **Commands (JSON)** editor; defaults include «новая строка», «стоп»,
   «табуляция», «запятая», «точка».
6. Save. Put your cursor in any editor and try it.

---

## Voice commands

Commands are **not hard-coded** — they live in `commands.json` (under your app
config dir; the bundled default is `config/commands.default.json`). Each command
maps a set of spoken **patterns** to an **action**.

```json
{
  "id": "newline",
  "enabled": true,
  "action": "insert_text",
  "value": "\n",
  "patterns": ["новая строка", "перенос строки", "следующая строка"],
  "match_mode": "normalized_phrase",
  "remove_command_from_output": true
}
```

- **Patterns are arbitrary** — to make «перенос» mean a newline, just add it to
  `patterns` (or replace the list entirely).
- `match_mode`: `normalized_phrase` (lowercased, whitespace-collapsed,
  punctuation-stripped phrase match — fully supported) or `regex`
  (QRegularExpression; applied after the phrase pass).
- `remove_command_from_output`: drop the spoken command from the final text.

### Actions
Implemented in the MVP: `insert_text`, `stop_recording`.
Reserved (schema-compatible, not yet active): `insert_symbol`,
`delete_previous_word`, `submit`, `escape`, `custom_hotkey`,
`custom_text_transform`. Unknown actions are preserved on save, not discarded.

### Stop-during-recording
`stop_recording` must work **mid-recording**, not just after. The
`CommandDetectionLoop` periodically (default every 2 s) runs a fast whisper pass
over the most recent audio tail (default 4 s) and ends recording when a stop
phrase is heard. The stop phrase is removed from the final text, and audio after
it is dropped.

---

## Acceptance scenarios → behavior

| # | Scenario | Where it's handled |
|---|----------|--------------------|
| 1 | Basic dictation & paste | `AppController` pipeline + `ClipboardPasteService` |
| 2 | «новая строка» → newline | `CommandEngine::processFinalText` |
| 3 | Custom newline phrase «перенос» | add to `patterns` (config-driven) |
| 4 | Voice «стоп» ends recording | `CommandDetectionLoop` → `stopRecordingByVoiceCommand` |
| 5 | Clipboard restored after paste | `ClipboardPasteService` (save → set → paste → restore) |

---

## macOS status

The architecture is cross-platform; macOS-specific pieces are stubbed with clear
TODOs:

- **Paste keystroke** (`KeyboardPaster_mac.cpp`): Quartz `CGEvent` Cmd+V is
  implemented but needs **Accessibility permission**; surface a prompt in the UI.
- **Global hotkey** (`HotkeyService_mac.cpp`): TODO — implement with Carbon
  `RegisterEventHotKey`; requires running as a proper `.app` bundle.
- Audio capture, ASR, commands, clipboard, overlay, tray, and settings are
  already portable (Qt + whisper.cpp).

---

## Roadmap / TODO

- macOS hotkey + Accessibility onboarding.
- Native **Wayland** input backend (virtual-keyboard protocol / uinput).
- `regex` reconstruction parity with the phrase pass (whitespace-inserting regex).
- Additional command actions (`submit`, `escape`, `delete_previous_word`, …).
- `HttpTextPostProcessor`: real HTTPS LLM cleanup with graceful fallback.
- Per-model selection UI and model auto-download from within the app.
```
