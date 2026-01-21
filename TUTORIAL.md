# Building a Terminal-Based MIDI Synthesizer for macOS

## Introduction

In this tutorial, you'll learn how to build **terminalSynth**, a fully-functional terminal-based MIDI synthesizer for macOS. By the end, you'll have created a program that:

- Turns your keyboard into a musical instrument with 26 playable notes
- Switches between 128 different instrument sounds (piano, guitar, drums, etc.)
- Supports 16 simultaneous MIDI channels
- Includes a metronome with adjustable tempo
- Captures keyboard input at the hardware level (no typing echo in the terminal)

This project demonstrates real-world systems programming concepts including audio synthesis, hardware event handling, asynchronous timers, and the macOS Core frameworks.

## What You'll Learn

- How to use macOS AudioToolbox framework to generate musical notes
- How to build an audio processing graph (AUGraph) with synthesizer units
- How to capture raw keyboard input using IOKit HID (Human Interface Device)
- How to use CoreFoundation run loops for asynchronous event handling
- How MIDI protocol works (note-on/note-off messages, programs, channels)
- How to implement auto-repeat timers that fire while keys are held down
- How to manage state for multi-channel audio synthesis

## Prerequisites

**Required Knowledge:**
- Comfortable with C programming (pointers, structs, arrays)
- Basic understanding of callback functions
- Familiarity with command-line compilation

**Required Tools:**
- macOS computer (arm64 or Intel)
- Xcode Command Line Tools installed (`xcode-select --install`)
- Terminal access

**Optional Knowledge** (we'll explain these):
- MIDI protocol basics
- Event-driven programming
- Audio synthesis concepts

## Part 1: Understanding the Audio Pipeline

### The Big Picture

Before writing any code, let's understand the journey from keypresses to sound:

```
┌─────────────┐    ┌──────────┐    ┌──────────┐    ┌─────────┐    ┌──────────┐
│  Keyboard   │ -> │   HID    │ -> │   MIDI   │ -> │   DLS   │ -> │ Speaker  │
│   Press     │    │ Callback │    │  Event   │    │  Synth  │    │  Output  │
└─────────────┘    └──────────┘    └──────────┘    └─────────┘    └──────────┘
```

1. **Keyboard Press**: User presses a physical key
2. **HID Callback**: IOKit detects the keypress and calls our function
3. **MIDI Event**: We translate the key into a MIDI message (e.g., "play note C4")
4. **DLS Synth**: Apple's built-in synthesizer converts MIDI to audio waveforms
5. **Speaker Output**: Sound comes out of your speakers

Think of this like a factory assembly line: raw materials (keypresses) get transformed step-by-step into a finished product (sound waves).

### Why This Architecture?

**Why use MIDI instead of generating sound directly?**
MIDI (Musical Instrument Digital Interface) is a standardized protocol that separates *what* to play from *how* it sounds. This means:
- We can switch instruments without rewriting our code
- The DLS synthesizer handles all the complex audio synthesis
- We can leverage decades of MIDI standards

**Why use IOKit HID instead of standard input?**
Normal terminal input (`scanf`, `getchar`) has several limitations:
- Keys must be released before we detect them (buffered input)
- Can't detect key release events (important for "note off")
- Echo characters to the terminal (messy!)

IOKit HID gives us direct access to keyboard hardware events.

## Part 2: Setting Up the Audio Graph

### Understanding AUGraph

An **AUGraph** (Audio Unit Graph) is like a signal processing pipeline. Think of it as connecting musical equipment with cables:

```
┌─────────────┐       ┌──────────────┐
│ Synthesizer │ cable │ Audio Output │
│    Unit     │------>│     Unit     │
└─────────────┘       └──────────────┘
```

The synthesizer generates audio, and the output unit sends it to your speakers.

### Implementation: Creating the Audio Pipeline

Here's how we initialize the audio system:

```c
#include <AudioToolbox/AudioToolbox.h>

static AUGraph graph = NULL;
static AUNode synthNode = 0;
static AudioUnit synthUnit = NULL;

static bool init_audio(void) {
    OSStatus err;

    // Step 1: Create an empty graph
    err = NewAUGraph(&graph);
    if (err) return false;

    // Step 2: Define the synthesizer component
    AudioComponentDescription cd = {0};
    cd.componentType = kAudioUnitType_MusicDevice;
    cd.componentSubType = kAudioUnitSubType_DLSSynth;
    cd.componentManufacturer = kAudioUnitManufacturer_Apple;

    // Step 3: Add synthesizer node to graph
    err = AUGraphAddNode(graph, &cd, &synthNode);
    if (err) return false;

    // Step 4: Define the output component
    AUNode outputNode;
    cd.componentType = kAudioUnitType_Output;
    cd.componentSubType = kAudioUnitSubType_DefaultOutput;

    // Step 5: Add output node to graph
    err = AUGraphAddNode(graph, &cd, &outputNode);
    if (err) return false;

    // Step 6: Connect synthesizer output to speaker input
    err = AUGraphConnectNodeInput(graph, synthNode, 0, outputNode, 0);
    if (err) return false;

    // Step 7: Open the graph (allocate resources)
    err = AUGraphOpen(graph);
    if (err) return false;

    // Step 8: Get a reference to the synthesizer unit
    err = AUGraphNodeInfo(graph, synthNode, NULL, &synthUnit);
    if (err) return false;

    // Step 9: Initialize the graph (prepare for playback)
    err = AUGraphInitialize(graph);
    if (err) return false;

    // Step 10: Start the graph (begin processing)
    err = AUGraphStart(graph);
    if (err) return false;

    return true;
}
```

**What's happening here?**

- **Steps 1-5**: We're building the graph structure (defining nodes and their types)
- **Step 6**: We connect the nodes with a virtual "cable" (the synthesizer's output goes to the speaker's input)
- **Steps 7-10**: We allocate resources, prepare everything, and start the audio processing engine

**Why so many steps?**
This separation allows maximum flexibility. You could add effects (reverb, distortion) between the synth and output, or connect multiple synths to a mixer. We're using the simplest possible configuration.

### Alternative Approaches

**Could we use AVAudioEngine instead?**
Yes! AVAudioEngine is a higher-level API introduced in iOS 8 / macOS 10.10. It's easier to use but:
- AUGraph gives us finer control over MIDI events
- AUGraph has been the standard for professional audio apps for decades
- For simple MIDI playback, either works fine

**What about OpenAL or Core Audio directly?**
You could generate waveforms yourself using Core Audio's lower-level APIs, but then you'd need to:
- Implement your own instrument sounds (complex!)
- Handle polyphony (playing multiple notes simultaneously)
- Manage audio buffers and sample rates

The DLS synthesizer does all this for free.

## Part 3: Understanding MIDI Messages

### MIDI Crash Course

MIDI messages are simple 3-byte commands. The most important ones:

```
Note On:   [0x90 | channel] [note number] [velocity]
Note Off:  [0x80 | channel] [note number] [velocity]
Program:   [0xC0 | channel] [program number] [0]
```

**Note Number**: 0-127, where middle C (C4) = 60
**Velocity**: How hard the note is played (0-127)
**Channel**: MIDI supports 16 independent channels (0-15)
**Program**: Selects the instrument (0 = piano, 40 = violin, etc.)

### Playing Notes

Here's how we send MIDI events to our synthesizer:

```c
static int currentChannel = 0;

static void note_on(uint8_t note, uint8_t velocity) {
    if (synthUnit) {
        // 0x90 = note-on message, OR'd with channel number
        MusicDeviceMIDIEvent(synthUnit, 0x90 | currentChannel, note, velocity, 0);
    }
}

static void note_off(uint8_t note) {
    if (synthUnit) {
        // 0x80 = note-off message
        MusicDeviceMIDIEvent(synthUnit, 0x80 | currentChannel, note, 0, 0);
    }
}
```

**Why separate note-on and note-off?**
Real instruments don't instantly stop making sound when you release a key. Piano notes decay naturally, and synthesizers can sustain notes indefinitely. The note-off message tells the synthesizer "the player has released this key" and lets it handle the sound's decay naturally.

### Changing Instruments

MIDI defines 128 standard instruments called **General MIDI** (GM):

```c
static void program_change(int program) {
    if (synthUnit) {
        // 0xC0 = program change message
        MusicDeviceMIDIEvent(synthUnit, 0xC0 | currentChannel, program, 0, 0);
    }
    printf("Now playing: %s\n", gmNames[program]);
}
```

Programs 0-127 include everything from pianos (0-7) to guitars (24-31) to sound effects (122-127). Channel 10 is special: it's always a drum kit, where different note numbers trigger different drums.

## Part 4: Capturing Keyboard Input with IOKit HID

### Why HID?

**HID** (Human Interface Device) is the USB standard for keyboards, mice, and game controllers. IOKit is macOS's framework for talking to hardware.

Normal terminal input goes through several layers:
```
Hardware -> Kernel -> Terminal -> Shell -> Your Program
```

HID gives us direct access:
```
Hardware -> Kernel -> Your Program
```

This means:
- We detect keypresses the instant they happen
- We can tell when keys are *released* (not just pressed)
- No characters appear on screen
- We get hardware keycodes, not ASCII characters

### USB HID Keycodes

Unlike ASCII (where 'a' = 97), HID uses position-based codes:

```c
// USB HID keycodes (from the HID Usage Tables specification)
// a=0x04, b=0x05, c=0x06, ..., z=0x1D
static const uint16_t ESC_KEYCODE = 0x29;
static const uint16_t TAB_KEYCODE = 0x2B;
// ... etc
```

**Why different from ASCII?**
HID keycodes represent physical key positions, independent of language or keyboard layout. This means 'Q' is always the same keycode whether you're using QWERTY, AZERTY, or Dvorak.

### Mapping Keys to Notes

We create a lookup table mapping keycodes to MIDI note numbers:

```c
static const struct {
    uint16_t keycode;
    uint8_t note;
} keymap[] = {
    // Bottom row: z x c v b n m (MIDI 36-42)
    {0x1D, 36}, {0x1B, 37}, {0x06, 38}, {0x19, 39},
    {0x05, 40}, {0x11, 41}, {0x10, 42},

    // Middle row: a s d f g h j k l (MIDI 43-51)
    {0x04, 43}, {0x16, 44}, {0x07, 45}, {0x09, 46},
    {0x0A, 47}, {0x0B, 48}, {0x0D, 49}, {0x0E, 50}, {0x0F, 51},

    // Top row: q w e r t y u i o p (MIDI 52-61)
    {0x14, 52}, {0x1A, 53}, {0x08, 54}, {0x15, 55},
    {0x17, 56}, {0x1C, 57}, {0x18, 58}, {0x0C, 59},
    {0x12, 60}, {0x13, 61}
};

static int keycode_to_note(uint16_t keycode) {
    for (int i = 0; i < KEYMAP_SIZE; i++) {
        if (keymap[i].keycode == keycode)
            return keymap[i].note;
    }
    return -1;  // Key not mapped
}
```

**Why this layout?**
The three rows (z-m, a-l, q-p) mimic a piano keyboard's white keys. This gives us about 2 octaves (26 notes) that are comfortable to play. MIDI note 60 is middle C (C4), so our range is C2 to C#5.

### Setting Up the HID Manager

Here's how we initialize HID and register for keyboard events:

```c
#include <IOKit/hid/IOHIDManager.h>

static IOHIDManagerRef init_hid(void) {
    // Step 1: Create the HID manager
    IOHIDManagerRef manager = IOHIDManagerCreate(
        kCFAllocatorDefault,
        kIOHIDOptionsTypeNone
    );
    if (!manager) return NULL;

    // Step 2: Create a filter for keyboard devices
    CFMutableDictionaryRef dict = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 2,
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks
    );

    int page = kHIDPage_GenericDesktop;
    int usage = kHIDUsage_GD_Keyboard;
    CFNumberRef pageNum = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &page);
    CFNumberRef usageNum = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &usage);

    CFDictionarySetValue(dict, CFSTR(kIOHIDDeviceUsagePageKey), pageNum);
    CFDictionarySetValue(dict, CFSTR(kIOHIDDeviceUsageKey), usageNum);

    // Step 3: Tell the manager to only monitor keyboards
    IOHIDManagerSetDeviceMatching(manager, dict);

    CFRelease(pageNum);
    CFRelease(usageNum);
    CFRelease(dict);

    // Step 4: Register our callback function
    IOHIDManagerRegisterInputValueCallback(manager, hid_callback, NULL);

    // Step 5: Attach to the run loop (enables async events)
    IOHIDManagerScheduleWithRunLoop(
        manager,
        CFRunLoopGetCurrent(),
        kCFRunLoopDefaultMode
    );

    // Step 6: Open the manager (start monitoring)
    if (IOHIDManagerOpen(manager, kIOHIDOptionsTypeNone) != kIOReturnSuccess) {
        CFRelease(manager);
        return NULL;
    }

    return manager;
}
```

**What's with all the CoreFoundation types?**
IOKit is built on CoreFoundation, which uses reference-counted objects (like modern Objective-C or Swift). You create objects with `Create` functions and must `CFRelease` them when done.

The dictionary we create is a filter that says "I only want keyboard events, not mice or game controllers."

## Part 5: The HID Callback – Where the Magic Happens

### Understanding Callbacks

A **callback** is a function you give to a framework, saying "call this when X happens." It's like giving someone your phone number – you don't know when they'll call, but when they do, you answer.

Our HID callback gets called every time a key is pressed or released:

```c
static void hid_callback(void *context, IOReturn result,
                        void *sender, IOHIDValueRef value) {
    // Step 1: Extract the key information
    IOHIDElementRef element = IOHIDValueGetElement(value);
    uint32_t usagePage = IOHIDElementGetUsagePage(element);
    uint32_t usage = IOHIDElementGetUsage(element);
    long pressed = IOHIDValueGetIntegerValue(value);

    // Step 2: Ignore non-keyboard events
    if (usagePage != kHIDPage_KeyboardOrKeypad) return;

    // Step 3: Handle special keys (ESC, TAB, etc.)
    if (usage == ESC_KEYCODE && pressed) {
        printf("\n");
        CFRunLoopStop(CFRunLoopGetCurrent());
        return;
    }

    if (usage == TAB_KEYCODE && pressed) {
        toggle_metronome();
        return;
    }

    // Step 4: Check if it's a musical note key
    int note = keycode_to_note(usage);
    if (note >= 0) {
        if (pressed)
            note_on(note, 100);  // velocity = 100 (medium-loud)
        else
            note_off(note);
    }
}
```

**Key points:**

- `pressed` is 1 when the key goes down, 0 when it comes up
- We ignore the release of special keys (ESC, TAB) because we only care about the initial press
- Musical keys trigger both note-on (press) and note-off (release)

### Preventing Stuck Notes

What happens if the user switches MIDI channels while holding down a note? The note-off would go to the wrong channel, leaving a note stuck forever!

We solve this by tracking which channel each note is playing on:

```c
static int8_t heldNoteChannel[128];  // One slot for each MIDI note (0-127)

static void note_on(uint8_t note, uint8_t velocity) {
    if (synthUnit) {
        MusicDeviceMIDIEvent(synthUnit, 0x90 | currentChannel, note, velocity, 0);
        heldNoteChannel[note] = currentChannel;  // Remember!
    }
}

static void note_off(uint8_t note) {
    if (synthUnit && heldNoteChannel[note] >= 0) {
        // Send note-off to the original channel
        MusicDeviceMIDIEvent(synthUnit, 0x80 | heldNoteChannel[note], note, 0, 0);
        heldNoteChannel[note] = -1;  // Mark as not held
    }
}
```

We use `-1` to mean "this note isn't currently playing." When a key is pressed, we remember which channel it started on. When released, we send the note-off to that same channel.

## Part 6: The Run Loop – Event-Driven Programming

### What is a Run Loop?

A **run loop** is an infinite loop that waits for events and calls your callbacks. It's the foundation of event-driven programming:

```c
int main(void) {
    init_audio();
    init_hid();

    printf("Ready! Press keys to play.\n");

    // This blocks until CFRunLoopStop() is called
    CFRunLoopRun();

    // Cleanup happens here
    cleanup();
    return 0;
}
```

Without a run loop, your program would:
1. Initialize everything
2. Immediately reach the end of `main()`
3. Exit

The run loop keeps the program alive, waiting for keyboard events. When ESC is pressed, our callback calls `CFRunLoopStop()`, which breaks out of the loop and continues to cleanup.

**Think of it like a receptionist:**
- Sits at a desk (the run loop)
- Waits for phone calls (events)
- Routes each call to the right person (your callbacks)
- Keeps doing this until told to go home (CFRunLoopStop)

### Alternative: Polling Loop

We *could* write our own loop:

```c
while (!should_quit) {
    check_for_keyboard_events();
    usleep(10000);  // Sleep 10ms
}
```

But this wastes CPU. The run loop is smarter: it sleeps until an event arrives, then wakes up instantly. This is crucial for laptop battery life and system responsiveness.

## Part 7: Auto-Repeat Timers

### The Challenge

When changing instruments, we want:
1. Single tap of `[` or `]` → move one instrument
2. Hold down `[` or `]` → keep scrolling through instruments

This is exactly how holding down a key in a text editor repeats the character.

### Implementation with CFRunLoopTimer

```c
static CFRunLoopTimerRef programChangeTimer = NULL;
static int programChangeDirection = 0;  // -1 or +1

static void program_change_timer_callback(CFRunLoopTimerRef timer, void *info) {
    int newProgram = (channelPrograms[currentChannel] + programChangeDirection + 128) % 128;
    program_change(newProgram);
}

static void start_program_change_timer(int direction) {
    programChangeDirection = direction;

    // Immediate change
    int newProgram = (channelPrograms[currentChannel] + direction + 128) % 128;
    program_change(newProgram);

    // Cancel existing timer if any
    if (programChangeTimer) {
        CFRunLoopTimerInvalidate(programChangeTimer);
        CFRelease(programChangeTimer);
    }

    // Create new timer
    programChangeTimer = CFRunLoopTimerCreate(
        kCFAllocatorDefault,
        CFAbsoluteTimeGetCurrent() + 0.3,  // First repeat after 0.3s
        0.1,                                // Then every 0.1s
        0, 0,
        program_change_timer_callback,
        NULL
    );

    CFRunLoopAddTimer(CFRunLoopGetCurrent(), programChangeTimer, kCFRunLoopDefaultMode);
}

static void stop_program_change_timer(void) {
    if (programChangeTimer) {
        CFRunLoopTimerInvalidate(programChangeTimer);
        CFRelease(programChangeTimer);
        programChangeTimer = NULL;
    }
}
```

**How it works:**

1. Key pressed (`[` or `]`) → Call `start_program_change_timer()`
2. Immediately change program once
3. Create a timer that fires after 0.3s delay
4. Timer repeats every 0.1s until key is released
5. Key released → Call `stop_program_change_timer()`

**Why the 0.3s delay?**
This matches standard keyboard repeat behavior. Try holding a key in a text editor – there's a brief pause before it starts repeating. This prevents accidental repeats from slightly-too-long taps.

### The Math Behind Wrapping

```c
int newProgram = (channelPrograms[currentChannel] + direction + 128) % 128;
```

This looks complex, but it handles wraparound:
- If we're at program 0 and go backward (-1), we get: `(0 + (-1) + 128) % 128 = 127`
- If we're at program 127 and go forward (+1), we get: `(127 + 1 + 128) % 128 = 0`

The `+ 128` before the modulo ensures we never take the modulo of a negative number (which behaves weirdly in C).

## Part 8: Building a Metronome

### Concept

A metronome is just a timer that plays a click sound at regular intervals. For a tempo of 120 BPM (beats per minute):

```
60 seconds / 120 beats = 0.5 seconds per beat
```

### Using MIDI Channel 10 (Drums)

MIDI channel 10 is special: it's always a drum kit. Different note numbers trigger different percussion:

```c
// MIDI note 76 = Hi Wood Block (good metronome sound)
static void metronome_tick(CFRunLoopTimerRef timer, void *info) {
    if (synthUnit) {
        // 0x99 = note-on on channel 10 (9 in 0-indexed)
        MusicDeviceMIDIEvent(synthUnit, 0x99, 76, 100, 0);
    }
    schedule_next_metronome_tick();
}
```

**Why not just create a repeating timer?**
We use a one-shot timer that reschedules itself. This allows us to change the tempo without stopping and restarting the metronome:

```c
static void schedule_next_metronome_tick(void) {
    if (metronomeTimer) {
        CFRunLoopTimerInvalidate(metronomeTimer);
        CFRelease(metronomeTimer);
        metronomeTimer = NULL;
    }

    if (metronomeEnabled) {
        double interval = 60.0 / metronomeBPM;  // Convert BPM to seconds
        metronomeTimer = CFRunLoopTimerCreate(
            kCFAllocatorDefault,
            CFAbsoluteTimeGetCurrent() + interval,
            0,  // Non-repeating (one-shot)
            0, 0,
            metronome_tick,
            NULL
        );
        CFRunLoopAddTimer(CFRunLoopGetCurrent(), metronomeTimer, kCFRunLoopDefaultMode);
    }
}
```

Each tick calculates the *current* BPM setting, so tempo changes take effect on the next beat.

### Toggling On/Off

```c
static bool metronomeEnabled = false;

static void toggle_metronome(void) {
    metronomeEnabled = !metronomeEnabled;

    if (metronomeEnabled) {
        // Play first click immediately
        MusicDeviceMIDIEvent(synthUnit, 0x99, 76, 100, 0);
        schedule_next_metronome_tick();
        printf("\r\033[KMetronome ON (%d BPM)", metronomeBPM);
    } else {
        // Cancel the timer
        if (metronomeTimer) {
            CFRunLoopTimerInvalidate(metronomeTimer);
            CFRelease(metronomeTimer);
            metronomeTimer = NULL;
        }
        printf("\r\033[KMetronome OFF");
    }
    fflush(stdout);
}
```

**What's `\r\033[K`?**
These are ANSI escape codes:
- `\r` – Move cursor to start of line
- `\033[K` – Clear from cursor to end of line

This lets us update the status line without creating new lines.

## Part 9: Multi-Channel MIDI

### Why Channels Matter

MIDI supports 16 independent channels. Think of them like tracks in a recording studio:
- Channel 1: Piano
- Channel 2: Bass guitar
- Channel 3: Drums
- etc.

Each channel can play a different instrument simultaneously. Our synth uses one channel at a time, but lets you switch between them:

```c
static int currentChannel = 0;
static int channelPrograms[16] = {0};  // Each channel remembers its instrument

static void channel_change(int channel) {
    all_notes_off();  // Stop all currently playing notes
    currentChannel = channel;
    int program = channelPrograms[currentChannel];
    printf("\r\033[KCh %2d | Program %3d: %s",
           currentChannel + 1, program, gmNames[program]);
    fflush(stdout);
}
```

**Why call `all_notes_off()`?**
If you're holding down a note on channel 1 and switch to channel 2, the note would keep playing forever (you can't send a note-off to channel 2 for a note that started on channel 1). We prevent this by explicitly stopping all notes when switching.

### Per-Channel Program Memory

Each channel remembers its instrument:

```c
static void program_change(int program) {
    channelPrograms[currentChannel] = program;  // Save for this channel
    if (synthUnit) {
        MusicDeviceMIDIEvent(synthUnit, 0xC0 | currentChannel, program, 0, 0);
    }
    // ...
}
```

This creates a powerful workflow:
1. Set channel 1 to piano
2. Set channel 2 to violin
3. Switch between channels to play different instruments
4. The settings persist – switching back to channel 1 restores the piano

## Part 10: Terminal Configuration

### Disabling Echo

By default, terminals echo everything you type. We don't want key presses appearing as random characters:

```c
#include <termios.h>
#include <unistd.h>

static struct termios origTermios;

static void disable_echo(void) {
    // Step 1: Save original settings
    tcgetattr(STDIN_FILENO, &origTermios);

    // Step 2: Register cleanup function to run on exit
    atexit(restore_terminal);

    // Step 3: Create modified settings
    struct termios raw = origTermios;
    raw.c_lflag &= ~(ECHO | ICANON);

    // Step 4: Apply new settings
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

static void restore_terminal(void) {
    tcflush(STDIN_FILENO, TCIFLUSH);  // Clear any buffered input
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &origTermios);
}
```

**What's happening:**

- `ECHO` flag controls whether typed characters appear on screen
- `ICANON` flag controls line buffering (canonical mode)
- We disable both to get raw, immediate input
- `atexit()` ensures we restore settings even if the program crashes

**Why is this necessary if we're using HID?**
Even though HID gives us keypresses directly, the terminal is still processing them in the background. Without disabling echo, you'd see random characters appearing while you play notes.

## Part 11: Putting It All Together

### The Main Function

Here's the complete program flow:

```c
int main(void) {
    // Initialize note tracking
    memset(heldNoteChannel, -1, sizeof(heldNoteChannel));

    // Disable terminal echo
    disable_echo();

    // Print instructions
    printf("terminalSynth - Terminal MIDI Synthesizer\n");
    printf("Keys z-p play MIDI notes 36-61\n");
    printf("- = change tempo, Shift+(-/=) change MIDI channel\n");
    printf("[ ] change program (0-127)\n");
    printf("TAB toggle metronome\n");
    printf("ESC to quit\n\n");

    // Initialize audio
    if (!init_audio()) {
        fprintf(stderr, "Failed to initialize audio\n");
        return 1;
    }

    // Initialize HID
    IOHIDManagerRef manager = init_hid();
    if (!manager) {
        fprintf(stderr, "Failed to initialize HID\n");
        cleanup_audio();
        return 1;
    }

    // Show initial status
    printf("Ready!\n");
    printf("Ch %2d | Program %3d: %s",
           currentChannel + 1,
           channelPrograms[currentChannel],
           gmNames[channelPrograms[currentChannel]]);

    // Enter the run loop (blocks until ESC is pressed)
    CFRunLoopRun();

    // Cleanup
    IOHIDManagerClose(manager, kIOHIDOptionsTypeNone);
    CFRelease(manager);

    if (graph) {
        AUGraphStop(graph);
        DisposeAUGraph(graph);
    }

    return 0;
}
```

### Build and Run

```bash
# Compile
clang -framework AudioToolbox -framework IOKit -framework CoreFoundation \
      terminalSynth.c -o terminalSynth

# Run (requires Input Monitoring permission)
./terminalSynth
```

**First run:**
macOS will prompt you to grant Input Monitoring permission in System Settings > Privacy & Security > Input Monitoring. This is required for HID access.

## Part 12: Design Decisions and Alternatives

### Why C Instead of Swift or Objective-C?

**Advantages of C:**
- Direct access to low-level APIs without Swift bridging overhead
- Simpler for demonstrating concepts (no object-oriented boilerplate)
- More portable to other Unix-like systems

**When to use Swift instead:**
- Building a full GUI app (Swift + SwiftUI is easier)
- Need modern safety features (memory management, type safety)
- Want to publish on the App Store

### Why DLS Synth Instead of AVAudioUnitSampler?

**DLS Synth (our choice):**
- Built into every Mac since macOS 10.0
- Includes all 128 GM instruments out of the box
- Lower latency for MIDI events
- Battle-tested in professional audio apps

**AVAudioUnitSampler (alternative):**
- More modern API (introduced macOS 10.10)
- Better integration with AVAudioEngine
- Can load custom soundfonts (.sf2 files)

For a simple MIDI player, DLS Synth is perfect. If you wanted custom sounds (e.g., voice samples, vintage synths), AVAudioUnitSampler would be better.

### Could This Run on Linux or Windows?

The audio pipeline is macOS-specific, but the concept ports easily:

**Linux:**
- Replace AUGraph with ALSA or JACK
- Use FluidSynth for software synthesis
- Replace IOKit with libevdev for keyboard input

**Windows:**
- Use Windows MIDI API or DirectSound
- Replace IOKit with DirectInput or Raw Input

**Cross-platform:**
- Use PortMIDI (abstracts MIDI across platforms)
- Use SDL2 for keyboard input
- Use FluidSynth for synthesis

The architecture (keyboard → MIDI → synth → output) would be identical.

## Part 13: Exercises and Extensions

### Beginner Exercises

1. **Add octave shift keys**: Make `1` and `2` transpose all notes up/down one octave
2. **Show visual feedback**: Print a `*` when a note is played
3. **Add velocity control**: Use number keys 1-9 to set velocity (10-90)

### Intermediate Exercises

4. **Implement pitch bend**: Use arrow keys to bend pitch up/down
5. **Add chord presets**: Press F1-F12 to play common chords (C major, Am, etc.)
6. **Record and playback**: Press `R` to record notes, `P` to playback

### Advanced Exercises

7. **Multi-channel layering**: Play the same note on multiple channels simultaneously
8. **Load custom soundfonts**: Support .sf2 files via AVAudioUnitSampler
9. **MIDI file export**: Save your performance as a standard MIDI file
10. **Add audio effects**: Insert reverb, delay, or distortion into the AUGraph

### Complete Extension: Sustain Pedal

Here's a full example of adding a sustain pedal (spacebar):

```c
static bool sustainPedal = false;
static bool sustainedNotes[128] = {false};

// In hid_callback, add:
if (usage == SPACE_KEYCODE) {
    sustainPedal = pressed;
    if (!pressed) {
        // Release all sustained notes
        for (int i = 0; i < 128; i++) {
            if (sustainedNotes[i]) {
                note_off(i);
                sustainedNotes[i] = false;
            }
        }
    }
    return;
}

// Modify note_off:
static void note_off(uint8_t note) {
    if (sustainPedal) {
        sustainedNotes[note] = true;  // Mark for later release
        return;
    }
    // ... send MIDI note-off as before
}
```

This demonstrates how to add new features using the same patterns we've learned.

## Part 14: Debugging Tips

### Common Issues

**No sound:**
- Check System Settings > Sound > Output Device
- Verify `init_audio()` returned true
- Use `printf` to confirm MIDI events are being sent

**Keys not detected:**
- Grant Input Monitoring permission
- Try `sudo ./terminalSynth` (not recommended for production)
- Use `printf` in `hid_callback` to see if it's being called

**Stuck notes:**
- Ensure `note_off` is being called on key release
- Check that `heldNoteChannel` tracking is correct
- Add `all_notes_off()` on program exit as safety

**Metronome drift:**
- Ensure you're using one-shot timers, not repeating timers
- Calculate interval from current `metronomeBPM` on each tick
- Use `CFAbsoluteTimeGetCurrent()` for precise timing

### Debugging HID Events

Add this at the start of `hid_callback` to see all keypresses:

```c
printf("Page: 0x%x, Usage: 0x%x, Pressed: %ld\n", usagePage, usage, pressed);
```

This reveals the keycode for any key you press, helpful for adding new mappings.

## Conclusion

You've now learned how to build a complete, functional MIDI synthesizer from scratch. You've explored:

- **Audio synthesis** with AUGraph and Audio Units
- **Hardware input** with IOKit HID
- **Event-driven programming** with CoreFoundation run loops
- **MIDI protocol** for musical communication
- **Asynchronous timers** for auto-repeat and metronome
- **State management** for multi-channel MIDI

These techniques apply far beyond music software:
- **Game development**: HID for controllers, audio for sound effects
- **Device drivers**: IOKit for hardware communication
- **Real-time systems**: Run loops and timers for responsive apps

### Next Steps

- **Explore other audio units**: Try the sampler, effects, or mixers
- **Study the MIDI spec**: Learn about control changes, aftertouch, and sysex
- **Build a GUI**: Wrap this in a SwiftUI or AppKit interface
- **Contribute to open source**: Many music apps need low-latency MIDI handling

The full source code for this tutorial is in `/Users/tomw/Claude/terminalSynth/terminalSynth.c`.

Happy coding, and enjoy making music with your creation!

---

## Appendix A: Complete Key Reference

| Key(s) | Function | Notes |
|--------|----------|-------|
| `z x c v b n m` | Notes (MIDI 36-42) | Bottom row (C2-F#2) |
| `a s d f g h j k l` | Notes (MIDI 43-51) | Middle row (G2-D#3) |
| `q w e r t y u i o p` | Notes (MIDI 52-61) | Top row (E3-C#4) |
| `[` | Previous instrument | Hold for auto-repeat |
| `]` | Next instrument | Hold for auto-repeat |
| `-` | Decrease tempo | Hold for auto-repeat |
| `=` | Increase tempo | Hold for auto-repeat |
| `Shift + -` | Previous MIDI channel | Hold for auto-repeat |
| `Shift + =` | Next MIDI channel | Hold for auto-repeat |
| `TAB` | Toggle metronome | Default 120 BPM |
| `ESC` | Quit program | |

## Appendix B: General MIDI Program List (Excerpt)

| Number | Instrument | Category |
|--------|------------|----------|
| 0 | Acoustic Grand Piano | Piano |
| 1 | Bright Acoustic Piano | Piano |
| 4 | Electric Piano 1 | Piano |
| 24 | Acoustic Guitar (nylon) | Guitar |
| 25 | Acoustic Guitar (steel) | Guitar |
| 33 | Acoustic Bass | Bass |
| 40 | Violin | Strings |
| 56 | Trumpet | Brass |
| 71 | Clarinet | Reed |
| 73 | Flute | Pipe |

See `gmNames[]` array in the source for the complete list of all 128 instruments.

## Appendix C: MIDI Note Number Chart

| Octave | C | C# | D | D# | E | F | F# | G | G# | A | A# | B |
|--------|---|----|----|----|----|----|----|----|----|----|----|---|
| 0 | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 |
| 1 | 12 | 13 | 14 | 15 | 16 | 17 | 18 | 19 | 20 | 21 | 22 | 23 |
| 2 | 24 | 25 | 26 | 27 | 28 | 29 | 30 | 31 | 32 | 33 | 34 | 35 |
| 3 | 36 | 37 | 38 | 39 | 40 | 41 | 42 | 43 | 44 | 45 | 46 | 47 |
| 4 | 48 | 49 | 50 | 51 | 52 | 53 | 54 | 55 | 56 | 57 | 58 | 59 |
| 5 | **60** | 61 | 62 | 63 | 64 | 65 | 66 | 67 | 68 | 69 | 70 | 71 |

**Bold 60** = Middle C (C4)

terminalSynth maps keys to notes 36-61, spanning from C2 to C#4.

## Appendix D: Build Troubleshooting

**Error: "framework not found AudioToolbox"**
```bash
# Install Xcode Command Line Tools
xcode-select --install
```

**Error: "Permission denied" when running**
```bash
# Make executable
chmod +x terminalSynth
```

**Error: "HID initialization failed"**
- Open System Settings > Privacy & Security > Input Monitoring
- Add your terminal app (Terminal.app or iTerm2)
- Restart terminal and try again

**Linker warnings about deployment target**
```bash
# Specify minimum macOS version
clang -mmacosx-version-min=10.15 -framework AudioToolbox ...
```
