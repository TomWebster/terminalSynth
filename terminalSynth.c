/**
 * terminalSynth.c - Terminal MIDI Synthesizer (no keyboard echo)
 *
 * Build: clang -framework AudioToolbox -framework IOKit -framework CoreFoundation terminalSynth.c -o terminalSynth
 *
 * Keyboard Layout:
 *   Top:    q w e r t y u i o p  (MIDI 52-61)
 *   Middle: a s d f g h j k l    (MIDI 43-51)
 *   Bottom: z x c v b n m        (MIDI 36-42)
 */

#include <AudioToolbox/AudioToolbox.h>
#include <IOKit/hid/IOHIDManager.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

// USB HID keycodes (a=0x04, b=0x05, ..., z=0x1D)
static const struct {
    uint16_t keycode;
    uint8_t note;
} keymap[] = {
    // Bottom row: z x c v b n m (MIDI 36-42)
    {0x1D, 36}, {0x1B, 37}, {0x06, 38}, {0x19, 39}, {0x05, 40}, {0x11, 41}, {0x10, 42},
    // Middle row: a s d f g h j k l (MIDI 43-51)
    {0x04, 43}, {0x16, 44}, {0x07, 45}, {0x09, 46}, {0x0A, 47}, {0x0B, 48}, {0x0D, 49}, {0x0E, 50}, {0x0F, 51},
    // Top row: q w e r t y u i o p (MIDI 52-61)
    {0x14, 52}, {0x1A, 53}, {0x08, 54}, {0x15, 55}, {0x17, 56}, {0x1C, 57}, {0x18, 58}, {0x0C, 59}, {0x12, 60}, {0x13, 61}
};

static const int KEYMAP_SIZE = sizeof(keymap) / sizeof(keymap[0]);
static const uint16_t ESC_KEYCODE = 0x29;
static const uint16_t TAB_KEYCODE = 0x2B;
static const uint16_t MINUS_KEYCODE = 0x2D;
static const uint16_t EQUALS_KEYCODE = 0x2E;
static const uint16_t LBRACKET_KEYCODE = 0x2F;
static const uint16_t RBRACKET_KEYCODE = 0x30;
static const uint16_t LSHIFT_KEYCODE = 0xE1;
static const uint16_t RSHIFT_KEYCODE = 0xE5;

// General MIDI program names
static const char* gmNames[] = {
    "Acoustic Grand Piano", "Bright Acoustic Piano", "Electric Grand Piano",
    "Honky-tonk Piano", "Electric Piano 1", "Electric Piano 2", "Harpsichord",
    "Clavi", "Celesta", "Glockenspiel", "Music Box", "Vibraphone", "Marimba",
    "Xylophone", "Tubular Bells", "Dulcimer", "Drawbar Organ", "Percussive Organ",
    "Rock Organ", "Church Organ", "Reed Organ", "Accordion", "Harmonica",
    "Tango Accordion", "Acoustic Guitar (nylon)", "Acoustic Guitar (steel)",
    "Electric Guitar (jazz)", "Electric Guitar (clean)", "Electric Guitar (muted)",
    "Overdriven Guitar", "Distortion Guitar", "Guitar Harmonics", "Acoustic Bass",
    "Electric Bass (finger)", "Electric Bass (pick)", "Fretless Bass", "Slap Bass 1",
    "Slap Bass 2", "Synth Bass 1", "Synth Bass 2", "Violin", "Viola", "Cello",
    "Contrabass", "Tremolo Strings", "Pizzicato Strings", "Orchestral Harp",
    "Timpani", "String Ensemble 1", "String Ensemble 2", "Synth Strings 1",
    "Synth Strings 2", "Choir Aahs", "Voice Oohs", "Synth Voice", "Orchestra Hit",
    "Trumpet", "Trombone", "Tuba", "Muted Trumpet", "French Horn", "Brass Section",
    "Synth Brass 1", "Synth Brass 2", "Soprano Sax", "Alto Sax", "Tenor Sax",
    "Baritone Sax", "Oboe", "English Horn", "Bassoon", "Clarinet", "Piccolo",
    "Flute", "Recorder", "Pan Flute", "Blown Bottle", "Shakuhachi", "Whistle",
    "Ocarina", "Lead 1 (square)", "Lead 2 (sawtooth)", "Lead 3 (calliope)",
    "Lead 4 (chiff)", "Lead 5 (charang)", "Lead 6 (voice)", "Lead 7 (fifths)",
    "Lead 8 (bass+lead)", "Pad 1 (new age)", "Pad 2 (warm)", "Pad 3 (polysynth)",
    "Pad 4 (choir)", "Pad 5 (bowed)", "Pad 6 (metallic)", "Pad 7 (halo)",
    "Pad 8 (sweep)", "FX 1 (rain)", "FX 2 (soundtrack)", "FX 3 (crystal)",
    "FX 4 (atmosphere)", "FX 5 (brightness)", "FX 6 (goblins)", "FX 7 (echoes)",
    "FX 8 (sci-fi)", "Sitar", "Banjo", "Shamisen", "Koto", "Kalimba", "Bag Pipe",
    "Fiddle", "Shanai", "Tinkle Bell", "Agogo", "Steel Drums", "Woodblock",
    "Taiko Drum", "Melodic Tom", "Synth Drum", "Reverse Cymbal", "Guitar Fret Noise",
    "Breath Noise", "Seashore", "Bird Tweet", "Telephone Ring", "Helicopter",
    "Applause", "Gunshot"
};

// Global state
static AUGraph graph = NULL;
static AUNode synthNode = 0;
static AudioUnit synthUnit = NULL;
static int channelPrograms[16] = {0};
static struct termios origTermios;
static CFRunLoopTimerRef programChangeTimer = NULL;
static int programChangeDirection = 0;
static CFRunLoopTimerRef channelChangeTimer = NULL;
static int channelChangeDirection = 0;
static int currentChannel = 0;
static CFRunLoopTimerRef metronomeTimer = NULL;
static bool metronomeEnabled = false;
static int metronomeBPM = 120;
static int8_t heldNoteChannel[128];  // -1 = not held, 0-15 = channel note is playing on
static bool shiftHeld = false;
static CFRunLoopTimerRef tempoChangeTimer = NULL;
static int tempoChangeDirection = 0;

static void restore_terminal(void) {
    tcflush(STDIN_FILENO, TCIFLUSH);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &origTermios);
}

static void disable_echo(void) {
    tcgetattr(STDIN_FILENO, &origTermios);
    atexit(restore_terminal);
    struct termios raw = origTermios;
    raw.c_lflag &= ~(ECHO | ICANON);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

static bool init_audio(void) {
    OSStatus err;
    err = NewAUGraph(&graph);
    if (err) return false;

    AudioComponentDescription cd = {0};
    cd.componentType = kAudioUnitType_MusicDevice;
    cd.componentSubType = kAudioUnitSubType_DLSSynth;
    cd.componentManufacturer = kAudioUnitManufacturer_Apple;

    err = AUGraphAddNode(graph, &cd, &synthNode);
    if (err) return false;

    AUNode outputNode;
    cd.componentType = kAudioUnitType_Output;
    cd.componentSubType = kAudioUnitSubType_DefaultOutput;

    err = AUGraphAddNode(graph, &cd, &outputNode);
    if (err) return false;

    err = AUGraphConnectNodeInput(graph, synthNode, 0, outputNode, 0);
    if (err) return false;

    err = AUGraphOpen(graph);
    if (err) return false;

    err = AUGraphNodeInfo(graph, synthNode, NULL, &synthUnit);
    if (err) return false;

    err = AUGraphInitialize(graph);
    if (err) return false;

    err = AUGraphStart(graph);
    if (err) return false;

    return true;
}

static void note_on(uint8_t note, uint8_t velocity) {
    if (synthUnit) {
        MusicDeviceMIDIEvent(synthUnit, 0x90 | currentChannel, note, velocity, 0);
        heldNoteChannel[note] = currentChannel;
    }
}

static void note_off(uint8_t note) {
    if (synthUnit && heldNoteChannel[note] >= 0) {
        MusicDeviceMIDIEvent(synthUnit, 0x80 | heldNoteChannel[note], note, 0, 0);
        heldNoteChannel[note] = -1;
    }
}

static void all_notes_off(void) {
    if (synthUnit) {
        for (int i = 0; i < 128; i++) {
            if (heldNoteChannel[i] >= 0) {
                MusicDeviceMIDIEvent(synthUnit, 0x80 | heldNoteChannel[i], i, 0, 0);
                heldNoteChannel[i] = -1;
            }
        }
    }
}

static void program_change(int program) {
    channelPrograms[currentChannel] = program;
    if (synthUnit) {
        MusicDeviceMIDIEvent(synthUnit, 0xC0 | currentChannel, program, 0, 0);
    }
    printf("\r\033[KCh %2d | Program %3d: %s", currentChannel + 1, program, gmNames[program]);
    fflush(stdout);
}

static void channel_change(int channel) {
    all_notes_off();
    currentChannel = channel;
    int program = channelPrograms[currentChannel];
    printf("\r\033[KCh %2d | Program %3d: %s", currentChannel + 1, program, gmNames[program]);
    fflush(stdout);
}

static void program_change_timer_callback(CFRunLoopTimerRef timer, void *info) {
    int newProgram = (channelPrograms[currentChannel] + programChangeDirection + 128) % 128;
    program_change(newProgram);
}

static void start_program_change_timer(int direction) {
    programChangeDirection = direction;
    int newProgram = (channelPrograms[currentChannel] + direction + 128) % 128;
    program_change(newProgram);

    if (programChangeTimer) {
        CFRunLoopTimerInvalidate(programChangeTimer);
        CFRelease(programChangeTimer);
    }

    programChangeTimer = CFRunLoopTimerCreate(kCFAllocatorDefault,
        CFAbsoluteTimeGetCurrent() + 0.3,  // initial delay
        0.1,                                // repeat interval
        0, 0,
        program_change_timer_callback,
        NULL);
    CFRunLoopAddTimer(CFRunLoopGetCurrent(), programChangeTimer, kCFRunLoopDefaultMode);
}

static void stop_program_change_timer(void) {
    if (programChangeTimer) {
        CFRunLoopTimerInvalidate(programChangeTimer);
        CFRelease(programChangeTimer);
        programChangeTimer = NULL;
    }
}

static void channel_change_timer_callback(CFRunLoopTimerRef timer, void *info) {
    currentChannel = (currentChannel + channelChangeDirection + 16) % 16;
    channel_change(currentChannel);
}

static void start_channel_change_timer(int direction) {
    channelChangeDirection = direction;
    currentChannel = (currentChannel + direction + 16) % 16;
    channel_change(currentChannel);

    if (channelChangeTimer) {
        CFRunLoopTimerInvalidate(channelChangeTimer);
        CFRelease(channelChangeTimer);
    }

    channelChangeTimer = CFRunLoopTimerCreate(kCFAllocatorDefault,
        CFAbsoluteTimeGetCurrent() + 0.3,
        0.1,
        0, 0,
        channel_change_timer_callback,
        NULL);
    CFRunLoopAddTimer(CFRunLoopGetCurrent(), channelChangeTimer, kCFRunLoopDefaultMode);
}

static void stop_channel_change_timer(void) {
    if (channelChangeTimer) {
        CFRunLoopTimerInvalidate(channelChangeTimer);
        CFRelease(channelChangeTimer);
        channelChangeTimer = NULL;
    }
}

static void metronome_tick(CFRunLoopTimerRef timer, void *info);

static void schedule_next_metronome_tick(void) {
    if (metronomeTimer) {
        CFRunLoopTimerInvalidate(metronomeTimer);
        CFRelease(metronomeTimer);
        metronomeTimer = NULL;
    }
    if (metronomeEnabled) {
        double interval = 60.0 / metronomeBPM;
        metronomeTimer = CFRunLoopTimerCreate(kCFAllocatorDefault,
            CFAbsoluteTimeGetCurrent() + interval,
            0,  // non-repeating
            0, 0,
            metronome_tick,
            NULL);
        CFRunLoopAddTimer(CFRunLoopGetCurrent(), metronomeTimer, kCFRunLoopDefaultMode);
    }
}

static void tempo_change(int bpm) {
    if (bpm < 20) bpm = 20;
    if (bpm > 300) bpm = 300;
    metronomeBPM = bpm;
    printf("\r\033[KTempo: %d BPM%s", metronomeBPM, metronomeEnabled ? " (ON)" : "");
    fflush(stdout);
}

static void tempo_change_timer_callback(CFRunLoopTimerRef timer, void *info) {
    tempo_change(metronomeBPM + tempoChangeDirection);
}

static void start_tempo_change_timer(int direction) {
    tempoChangeDirection = direction;
    tempo_change(metronomeBPM + direction);

    if (tempoChangeTimer) {
        CFRunLoopTimerInvalidate(tempoChangeTimer);
        CFRelease(tempoChangeTimer);
    }

    tempoChangeTimer = CFRunLoopTimerCreate(kCFAllocatorDefault,
        CFAbsoluteTimeGetCurrent() + 0.3,
        0.1,
        0, 0,
        tempo_change_timer_callback,
        NULL);
    CFRunLoopAddTimer(CFRunLoopGetCurrent(), tempoChangeTimer, kCFRunLoopDefaultMode);
}

static void stop_tempo_change_timer(void) {
    if (tempoChangeTimer) {
        CFRunLoopTimerInvalidate(tempoChangeTimer);
        CFRelease(tempoChangeTimer);
        tempoChangeTimer = NULL;
    }
}

static void metronome_tick(CFRunLoopTimerRef timer, void *info) {
    if (synthUnit) {
        // Play woodblock on drum channel (channel 10 = 0x99)
        MusicDeviceMIDIEvent(synthUnit, 0x99, 76, 100, 0);
    }
    schedule_next_metronome_tick();
}

static void toggle_metronome(void) {
    metronomeEnabled = !metronomeEnabled;

    if (metronomeEnabled) {
        // Play first tick immediately, then schedule next
        if (synthUnit) {
            MusicDeviceMIDIEvent(synthUnit, 0x99, 76, 100, 0);
        }
        schedule_next_metronome_tick();
        printf("\r\033[KMetronome ON (%d BPM)", metronomeBPM);
    } else {
        if (metronomeTimer) {
            CFRunLoopTimerInvalidate(metronomeTimer);
            CFRelease(metronomeTimer);
            metronomeTimer = NULL;
        }
        printf("\r\033[KMetronome OFF");
    }
    fflush(stdout);
}

static int keycode_to_note(uint16_t keycode) {
    for (int i = 0; i < KEYMAP_SIZE; i++) {
        if (keymap[i].keycode == keycode) return keymap[i].note;
    }
    return -1;
}

static void hid_callback(void *context, IOReturn result, void *sender, IOHIDValueRef value) {
    IOHIDElementRef element = IOHIDValueGetElement(value);
    uint32_t usagePage = IOHIDElementGetUsagePage(element);
    uint32_t usage = IOHIDElementGetUsage(element);
    long pressed = IOHIDValueGetIntegerValue(value);

    if (usagePage != kHIDPage_KeyboardOrKeypad) return;

    if (usage == ESC_KEYCODE && pressed) {
        printf("\n");
        CFRunLoopStop(CFRunLoopGetCurrent());
        return;
    }

    if (usage == TAB_KEYCODE && pressed) {
        toggle_metronome();
        return;
    }

    if (usage == LSHIFT_KEYCODE || usage == RSHIFT_KEYCODE) {
        shiftHeld = pressed;
        return;
    }

    if (usage == MINUS_KEYCODE) {
        if (shiftHeld) {
            if (pressed) start_channel_change_timer(-1);
            else stop_channel_change_timer();
        } else {
            if (pressed) start_tempo_change_timer(-1);
            else stop_tempo_change_timer();
        }
        return;
    }
    if (usage == EQUALS_KEYCODE) {
        if (shiftHeld) {
            if (pressed) start_channel_change_timer(1);
            else stop_channel_change_timer();
        } else {
            if (pressed) start_tempo_change_timer(1);
            else stop_tempo_change_timer();
        }
        return;
    }

    if (usage == LBRACKET_KEYCODE) {
        if (pressed) start_program_change_timer(-1);
        else stop_program_change_timer();
        return;
    }
    if (usage == RBRACKET_KEYCODE) {
        if (pressed) start_program_change_timer(1);
        else stop_program_change_timer();
        return;
    }

    int note = keycode_to_note(usage);
    if (note >= 0) {
        if (pressed) note_on(note, 100);
        else note_off(note);
    }
}

static IOHIDManagerRef init_hid(void) {
    IOHIDManagerRef manager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    if (!manager) return NULL;

    CFMutableDictionaryRef dict = CFDictionaryCreateMutable(kCFAllocatorDefault, 2,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

    int page = kHIDPage_GenericDesktop;
    int usage = kHIDUsage_GD_Keyboard;
    CFNumberRef pageNum = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &page);
    CFNumberRef usageNum = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &usage);

    CFDictionarySetValue(dict, CFSTR(kIOHIDDeviceUsagePageKey), pageNum);
    CFDictionarySetValue(dict, CFSTR(kIOHIDDeviceUsageKey), usageNum);
    IOHIDManagerSetDeviceMatching(manager, dict);

    CFRelease(pageNum);
    CFRelease(usageNum);
    CFRelease(dict);

    IOHIDManagerRegisterInputValueCallback(manager, hid_callback, NULL);
    IOHIDManagerScheduleWithRunLoop(manager, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);

    if (IOHIDManagerOpen(manager, kIOHIDOptionsTypeNone) != kIOReturnSuccess) {
        CFRelease(manager);
        return NULL;
    }

    return manager;
}

int main(void) {
    memset(heldNoteChannel, -1, sizeof(heldNoteChannel));
    disable_echo();

    printf("terminalSynth - Terminal MIDI Synthesizer\n");
    printf("Keys z-p play MIDI notes 36-61\n");
    printf("- = change tempo, Shift+(-/=) change MIDI channel\n");
    printf("[ ] change program (0-127)\n");
    printf("TAB toggle metronome\n");
    printf("ESC to quit\n\n");

    if (!init_audio()) {
        fprintf(stderr, "Failed to initialize audio\n");
        return 1;
    }

    IOHIDManagerRef manager = init_hid();
    if (!manager) {
        fprintf(stderr, "Failed to initialize HID\n");
        if (graph) {
            AUGraphStop(graph);
            DisposeAUGraph(graph);
        }
        return 1;
    }

    printf("Ready!\n");
    printf("Ch %2d | Program %3d: %s", currentChannel + 1, channelPrograms[currentChannel], gmNames[channelPrograms[currentChannel]]);

    CFRunLoopRun();

    IOHIDManagerClose(manager, kIOHIDOptionsTypeNone);
    CFRelease(manager);

    if (graph) {
        AUGraphStop(graph);
        DisposeAUGraph(graph);
    }

    return 0;
}
