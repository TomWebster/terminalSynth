# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

terminalSynth is a macOS terminal-based MIDI synthesizer that converts keyboard input into piano notes using the AudioToolbox framework. Written in C, it targets macOS (arm64/Intel).

## Build Commands

```bash
# Build terminalSynth
clang -framework AudioToolbox -framework IOKit -framework CoreFoundation terminalSynth.c -o terminalSynth

# Build synthSnag
clang -framework AudioToolbox -framework IOKit -framework CoreFoundation synthSnag.c -o synthSnag
```

## Running

```bash
./terminalSynth   # Requires Input Monitoring permission in System Settings
./synthSnag       # Requires Input Monitoring permission in System Settings
```

Press ESC to quit.

## Controls

| Key | Function |
|-----|----------|
| `z x c v b n m` | Play notes (MIDI 36-42) |
| `a s d f g h j k l` | Play notes (MIDI 43-51) |
| `q w e r t y u i o p` | Play notes (MIDI 52-61) |
| `[` `]` | Change program/instrument (0-127, hold to repeat) |
| `-` `=` | Change MIDI channel (1-16, hold to repeat) |
| `TAB` | Toggle metronome (120 BPM) |
| `ESC` | Quit |

Each MIDI channel remembers its own program selection.

## Architecture

```
Keyboard Input → Key Mapping → MIDI Event → AUGraph → Audio Output
```

**Audio Pipeline:** Creates an AUGraph with a DLS Synthesizer audio unit connected to the default output unit. MIDI events (0x90 note-on, 0x80 note-off) are sent via `MusicDeviceMIDIEvent()`.

**Implementation:** Both files use IOKit HID for direct keyboard access, event-driven via CFRunLoop callback.

**Key Functions (terminalSynth.c):**
- `keycode_to_note()` - Maps USB HID keycodes to MIDI note numbers (36-61)
- `hid_callback()` - Async HID event handler that triggers MIDI events on key press/release
- `program_change()` - Switches between 128 General MIDI instruments on current channel
- `channel_change()` - Switches between 16 MIDI channels
- `toggle_metronome()` - Starts/stops metronome timer using drum channel
- `start_program_change_timer()` / `start_channel_change_timer()` - Auto-repeat while key held

## Frameworks

- **AudioToolbox** - macOS audio synthesis (DLS Synth audio unit)
- **IOKit** - USB HID keyboard access
- **CoreFoundation** - Run loop for async event handling
