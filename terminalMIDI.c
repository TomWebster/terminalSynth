/**
 * terminalMIDI.c - Terminal MIDI Synthesizer with 16-track recorder (optimised)
 *
 * Build: clang -framework AudioToolbox -framework IOKit -framework CoreFoundation terminalMIDI.c -o terminalMIDI
 *
 * Optimisations:
 *   - O(1) keycode lookup table (was O(n) linear search)
 *   - Drift-corrected beat scheduling using mach_absolute_time
 *   - Tempo-adaptive playback timer interval
 *   - Metronome synced to beat 1 of master clock
 *
 * Keyboard Layout:
 *   Top:    q w e r t y u i o p  (MIDI notes, octave adjustable)
 *   Middle: a s d f g h j k l
 *   Bottom: z x c v b n m
 *
 * Controls:
 *   SPACE     = Start/Stop master clock
 *   CAPSLOCK  = Start/Stop recording (requires clock running)
 *   TAB       = Toggle metronome
 *   LEFT/RIGHT = Octave down/up
 *   UP/DOWN   = Tempo up/down (hold)
 *   - =       = MIDI channel down/up
 *   [ ]       = Program change down/up (hold)
 *   /         = Save MIDI file
 *   ESC       = Quit
 */

#include <AudioToolbox/AudioToolbox.h>
#include <IOKit/hid/IOHIDManager.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <time.h>

// Constants
#define MAX_EVENTS_PER_TRACK 10000
#define BEATS_PER_BAR 4
#define TOTAL_BARS 4
#define TOTAL_BEATS (BEATS_PER_BAR * TOTAL_BARS)
#define MIDI_TRACKS 16
#define TICKS_PER_BEAT 480  // Standard MIDI resolution
#define TICKS_PER_16TH (TICKS_PER_BEAT / 4)  // 120 ticks per 16th note

// MIDI event structure
typedef struct {
    uint32_t tick;          // Tick position within the loop (0 to totalLoopTicks-1)
    uint8_t status;         // Note on (0x90) or note off (0x80)
    uint8_t note;
    uint8_t velocity;
} MIDIEvent;

// Track structure
typedef struct {
    MIDIEvent events[MAX_EVENTS_PER_TRACK];
    int eventCount;
    int program;
} MIDITrack;

// Direct keycode-to-note lookup table (value = noteOffset + 1, 0 = unmapped)
// O(1) lookup indexed by USB HID keycode, max keycode 0x1D (29)
static const uint8_t keymapLUT[32] = {
    [0x04] = 8,  [0x05] = 5,  [0x06] = 3,  [0x07] = 10,  // a, b, c, d
    [0x08] = 19, [0x09] = 11, [0x0A] = 12, [0x0B] = 13,  // e, f, g, h
    [0x0C] = 24, [0x0D] = 14, [0x0E] = 15, [0x0F] = 16,  // i, j, k, l
    [0x10] = 7,  [0x11] = 6,  [0x12] = 25, [0x13] = 26,  // m, n, o, p
    [0x14] = 17, [0x15] = 20, [0x16] = 9,  [0x17] = 21,  // q, r, s, t
    [0x18] = 23, [0x19] = 4,  [0x1A] = 18, [0x1B] = 2,   // u, v, w, x
    [0x1C] = 22, [0x1D] = 1,                              // y, z
};

// HID keycodes
static const uint16_t ESC_KEYCODE = 0x29;
static const uint16_t TAB_KEYCODE = 0x2B;
static const uint16_t CAPSLOCK_KEYCODE = 0x39;
static const uint16_t SPACE_KEYCODE = 0x2C;
static const uint16_t MINUS_KEYCODE = 0x2D;
static const uint16_t EQUALS_KEYCODE = 0x2E;
static const uint16_t LBRACKET_KEYCODE = 0x2F;
static const uint16_t RBRACKET_KEYCODE = 0x30;
static const uint16_t SLASH_KEYCODE = 0x38;
static const uint16_t DELETE_KEYCODE = 0x2A;      // Backspace/Delete
static const uint16_t BACKTICK_KEYCODE = 0x35;    // ` key for quantize toggle
static const uint16_t RIGHT_ARROW_KEYCODE = 0x4F;
static const uint16_t LEFT_ARROW_KEYCODE = 0x50;
static const uint16_t DOWN_ARROW_KEYCODE = 0x51;
static const uint16_t UP_ARROW_KEYCODE = 0x52;

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

// Global state - Audio
static AUGraph graph = NULL;
static AUNode synthNode = 0;
static AudioUnit synthUnit = NULL;
static struct termios origTermios;

// Global state - MIDI
static MIDITrack tracks[MIDI_TRACKS];
static int currentChannel = 0;
static int currentOctave = 3;  // Base octave (C3 = MIDI 36)
static int8_t heldNoteChannel[128];

// Global state - Transport
static bool clockRunning = false;
static bool recording = false;
static bool recordArmed = false;     // Waiting for next beat to start recording
static bool capsLockOn = false;      // Track Caps Lock state for record sync
static bool metronomeEnabled = true;
static bool quantizeEnabled = false; // Global quantize to 16th notes
static int metronomeBPM = 120;
static int currentBeat = 0;          // 0 to TOTAL_BEATS-1
static int recordStartBeat = 0;      // Beat where recording started
static int beatsRecorded = 0;        // Count of beats recorded

// Global state - Timing (using mach_absolute_time for precision)
static mach_timebase_info_data_t timebaseInfo;
static uint64_t clockStartTime = 0;     // When clock started (mach ticks)
static uint64_t loopStartTime = 0;      // When current loop started
static uint64_t nanosPerTick = 0;       // Nanoseconds per MIDI tick
static uint64_t nanosPerBeat = 0;       // Nanoseconds per beat (for timer scheduling)
static uint64_t nextBeatMachTime = 0;   // Next beat in mach ticks (drift-corrected)
static uint32_t totalLoopTicks = TICKS_PER_BEAT * TOTAL_BEATS;

// Global state - Timers
static CFRunLoopTimerRef beatTimer = NULL;
static CFRunLoopTimerRef playbackTimer = NULL;  // High-resolution playback timer
static CFRunLoopTimerRef programChangeTimer = NULL;
static int programChangeDirection = 0;
static CFRunLoopTimerRef tempoChangeTimer = NULL;
static int tempoChangeDirection = 0;

// Global state - Playback tracking
static uint32_t lastPlaybackTick = 0;
static bool playbackWrapped = false;  // Track loop wrap for playback

// Forward declarations
static void beat_tick(CFRunLoopTimerRef timer, void *info);
static void playback_tick(CFRunLoopTimerRef timer, void *info);
static void update_status_display(void);
static void schedule_next_beat(void);
static void start_playback_timer(void);
static void stop_playback_timer(void);
static void start_recording_on_beat(void);
static void stop_recording(void);

// Terminal handling
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

// Timing functions using mach_absolute_time for high precision
static void init_timing(void) {
    mach_timebase_info(&timebaseInfo);
}

static uint64_t mach_to_nanos(uint64_t mach_ticks) {
    return mach_ticks * timebaseInfo.numer / timebaseInfo.denom;
}

static uint64_t nanos_to_mach(uint64_t nanos) {
    return nanos * timebaseInfo.denom / timebaseInfo.numer;
}

static void update_timing_constants(void) {
    // Calculate nanoseconds per MIDI tick based on BPM
    // 1 beat = TICKS_PER_BEAT ticks
    // 1 minute = metronomeBPM beats
    // So: nanos per tick = (60 * 1e9) / (BPM * TICKS_PER_BEAT)
    nanosPerTick = (uint64_t)(60.0 * 1e9 / (metronomeBPM * TICKS_PER_BEAT));
    nanosPerBeat = (uint64_t)(60.0 * 1e9 / metronomeBPM);
}

static uint32_t get_current_tick(void) {
    if (!clockRunning) return 0;
    uint64_t now = mach_absolute_time();
    uint64_t elapsedNanos = mach_to_nanos(now - loopStartTime);
    uint32_t tick = (uint32_t)(elapsedNanos / nanosPerTick);
    return tick % totalLoopTicks;
}

// Audio initialization
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

// MIDI functions
static void note_on_internal(int channel, uint8_t note, uint8_t velocity) {
    if (synthUnit && note < 128) {
        MusicDeviceMIDIEvent(synthUnit, 0x90 | channel, note, velocity, 0);
    }
}

static void note_off_internal(int channel, uint8_t note) {
    if (synthUnit && note < 128) {
        MusicDeviceMIDIEvent(synthUnit, 0x80 | channel, note, 0, 0);
    }
}

static void note_on(uint8_t note, uint8_t velocity) {
    if (note >= 128) return;

    note_on_internal(currentChannel, note, velocity);
    heldNoteChannel[note] = currentChannel;

    // Record if recording
    if (recording && clockRunning) {
        MIDITrack *track = &tracks[currentChannel];
        if (track->eventCount < MAX_EVENTS_PER_TRACK) {
            uint32_t tick = get_current_tick();
            // Quantize to 16th notes if enabled
            if (quantizeEnabled) {
                tick = ((tick + TICKS_PER_16TH / 2) / TICKS_PER_16TH) * TICKS_PER_16TH;
                tick = tick % totalLoopTicks;
            }
            track->events[track->eventCount].tick = tick;
            track->events[track->eventCount].status = 0x90;
            track->events[track->eventCount].note = note;
            track->events[track->eventCount].velocity = velocity;
            track->eventCount++;
        }
    }
}

static void note_off(uint8_t note) {
    if (note >= 128 || heldNoteChannel[note] < 0) return;

    int channel = heldNoteChannel[note];
    note_off_internal(channel, note);
    heldNoteChannel[note] = -1;

    // Record if recording
    if (recording && clockRunning) {
        MIDITrack *track = &tracks[channel];
        if (track->eventCount < MAX_EVENTS_PER_TRACK) {
            uint32_t tick = get_current_tick();
            // Quantize to 16th notes if enabled
            if (quantizeEnabled) {
                tick = ((tick + TICKS_PER_16TH / 2) / TICKS_PER_16TH) * TICKS_PER_16TH;
                tick = tick % totalLoopTicks;
            }
            track->events[track->eventCount].tick = tick;
            track->events[track->eventCount].status = 0x80;
            track->events[track->eventCount].note = note;
            track->events[track->eventCount].velocity = 0;
            track->eventCount++;
        }
    }
}

static void all_notes_off(void) {
    if (synthUnit) {
        for (int i = 0; i < 128; i++) {
            if (heldNoteChannel[i] >= 0) {
                note_off_internal(heldNoteChannel[i], i);
                heldNoteChannel[i] = -1;
            }
        }
    }
}

static void program_change(int program) {
    if (recording) return;  // Can't change during recording
    tracks[currentChannel].program = program;
    if (synthUnit) {
        MusicDeviceMIDIEvent(synthUnit, 0xC0 | currentChannel, program, 0, 0);
    }
    update_status_display();
}

static void channel_change(int channel) {
    if (recording) return;  // Can't change during recording
    // Send note-off for all 128 notes on the channel we're leaving
    if (synthUnit) {
        for (int i = 0; i < 128; i++) {
            MusicDeviceMIDIEvent(synthUnit, 0x80 | currentChannel, i, 0, 0);
        }
    }
    // Clear held note tracking for notes on this channel
    for (int i = 0; i < 128; i++) {
        if (heldNoteChannel[i] == currentChannel) {
            heldNoteChannel[i] = -1;
        }
    }
    currentChannel = channel;
    // Apply program for this channel
    if (synthUnit) {
        MusicDeviceMIDIEvent(synthUnit, 0xC0 | currentChannel, tracks[currentChannel].program, 0, 0);
    }
    update_status_display();
}

static void clear_current_track(void) {
    if (recording) return;  // Can't clear during recording
    tracks[currentChannel].eventCount = 0;
    update_status_display();
}

// Playback - play recorded events for a tick range
static void play_events_in_range(uint32_t startTick, uint32_t endTick) {
    for (int t = 0; t < MIDI_TRACKS; t++) {
        MIDITrack *track = &tracks[t];
        for (int i = 0; i < track->eventCount; i++) {
            MIDIEvent *ev = &track->events[i];
            bool inRange;
            if (startTick <= endTick) {
                inRange = (ev->tick >= startTick && ev->tick < endTick);
            } else {
                // Wrapped around
                inRange = (ev->tick >= startTick || ev->tick < endTick);
            }

            if (inRange) {
                if (ev->status == 0x90) {
                    note_on_internal(t, ev->note, ev->velocity);
                } else if (ev->status == 0x80) {
                    note_off_internal(t, ev->note);
                }
            }
        }
    }
}

// High-resolution playback timer callback
static void playback_tick(CFRunLoopTimerRef timer, void *info) {
    if (!clockRunning) return;

    uint32_t currentTick = get_current_tick();

    // Handle wrap-around
    if (currentTick < lastPlaybackTick) {
        // We've wrapped - play events from lastPlaybackTick to end, then 0 to currentTick
        play_events_in_range(lastPlaybackTick, totalLoopTicks);
        play_events_in_range(0, currentTick);
        playbackWrapped = true;
    } else {
        // Normal case - play events in range
        play_events_in_range(lastPlaybackTick, currentTick);
    }

    lastPlaybackTick = currentTick;
}

// Calculate optimal playback timer interval based on tempo
static double calculate_playback_interval(void) {
    // Seconds per tick = 60 / (BPM * TICKS_PER_BEAT)
    // Use half the tick duration to ensure we check twice per tick
    // Clamp between 1ms (high tempo) and 5ms (low tempo) for efficiency
    double secsPerTick = 60.0 / (metronomeBPM * TICKS_PER_BEAT);
    double interval = secsPerTick * 0.5;
    if (interval < 0.001) interval = 0.001;  // Min 1ms
    if (interval > 0.005) interval = 0.005;  // Max 5ms
    return interval;
}

static void start_playback_timer(void) {
    if (playbackTimer) {
        CFRunLoopTimerInvalidate(playbackTimer);
        CFRelease(playbackTimer);
    }

    double interval = calculate_playback_interval();
    playbackTimer = CFRunLoopTimerCreate(kCFAllocatorDefault,
        CFAbsoluteTimeGetCurrent(),
        interval,
        0, 0,
        playback_tick,
        NULL);
    CFRunLoopAddTimer(CFRunLoopGetCurrent(), playbackTimer, kCFRunLoopDefaultMode);
}

static void stop_playback_timer(void) {
    if (playbackTimer) {
        CFRunLoopTimerInvalidate(playbackTimer);
        CFRelease(playbackTimer);
        playbackTimer = NULL;
    }
}

// Transport functions
static void beat_tick(CFRunLoopTimerRef timer, void *info) {
    if (!clockRunning) return;

    int beatInBar = currentBeat % BEATS_PER_BAR;

    // Reset loop timing on beat 1 BEFORE metronome plays
    // This ensures the downbeat is at tick 0 of the master clock
    if (currentBeat == 0) {
        loopStartTime = mach_absolute_time();
        lastPlaybackTick = 0;
        playbackWrapped = false;
    }

    // Metronome - now properly aligned with beat 1
    if (metronomeEnabled && synthUnit) {
        uint8_t velocity = (beatInBar == 0) ? 120 : 80;
        uint8_t note = (beatInBar == 0) ? 76 : 77;  // Hi/Lo wood block
        MusicDeviceMIDIEvent(synthUnit, 0x99, note, velocity, 0);
    }

    // Start recording if armed (recording starts on this beat)
    if (recordArmed && capsLockOn) {
        start_recording_on_beat();
    }

    // Ensure recording is only active when Caps Lock is on
    if ((recording || recordArmed) && !capsLockOn) {
        stop_recording();
    }

    // Count beats while recording, auto-stop after 16 beats
    if (recording) {
        beatsRecorded++;
        if (beatsRecorded > TOTAL_BEATS) {
            stop_recording();
        }
    }

    // Update display before incrementing so it shows current beat
    update_status_display();

    // Advance beat counter
    currentBeat = (currentBeat + 1) % TOTAL_BEATS;

    schedule_next_beat();
}

// Drift-corrected scheduling using mach_absolute_time
static void schedule_next_beat(void) {
    if (beatTimer) {
        CFRunLoopTimerInvalidate(beatTimer);
        CFRelease(beatTimer);
        beatTimer = NULL;
    }

    if (clockRunning) {
        // Calculate next beat time in mach ticks (drift-corrected)
        nextBeatMachTime += nanos_to_mach(nanosPerBeat);

        // Convert mach time delta to seconds for CFRunLoopTimer
        uint64_t now = mach_absolute_time();
        int64_t deltaMach = (int64_t)(nextBeatMachTime - now);
        double delaySecs = (deltaMach > 0) ? mach_to_nanos(deltaMach) / 1e9 : 0.0;

        beatTimer = CFRunLoopTimerCreate(kCFAllocatorDefault,
            CFAbsoluteTimeGetCurrent() + delaySecs,
            0,  // Non-repeating
            0, 0,
            beat_tick,
            NULL);
        CFRunLoopAddTimer(CFRunLoopGetCurrent(), beatTimer, kCFRunLoopDefaultMode);
    }
}

static void start_clock(void) {
    if (clockRunning) return;

    clockRunning = true;
    currentBeat = 0;
    uint64_t now = mach_absolute_time();
    clockStartTime = now;
    loopStartTime = now;
    nextBeatMachTime = now;  // Initialize for drift-corrected scheduling
    lastPlaybackTick = 0;
    playbackWrapped = false;
    update_timing_constants();

    // Start high-resolution playback timer
    start_playback_timer();

    // Trigger first beat immediately
    beat_tick(NULL, NULL);
}

static void stop_clock(void) {
    if (!clockRunning) return;

    clockRunning = false;
    recording = false;
    currentBeat = 0;

    // Send All Notes Off (CC 123) on all 16 MIDI channels
    if (synthUnit) {
        for (int ch = 0; ch < 16; ch++) {
            MusicDeviceMIDIEvent(synthUnit, 0xB0 | ch, 123, 0, 0);
        }
    }
    memset(heldNoteChannel, -1, sizeof(heldNoteChannel));

    stop_playback_timer();

    if (beatTimer) {
        CFRunLoopTimerInvalidate(beatTimer);
        CFRelease(beatTimer);
        beatTimer = NULL;
    }

    update_status_display();
}

static void toggle_clock(void) {
    if (clockRunning) {
        stop_clock();
    } else {
        start_clock();
    }
}

static void arm_recording(void) {
    if (!clockRunning || recording || recordArmed) return;
    recordArmed = true;
    update_status_display();
}

static void start_recording_on_beat(void) {
    // Called from beat_tick when armed
    recordArmed = false;
    recording = true;
    recordStartBeat = currentBeat;
    beatsRecorded = 0;

    // New events are appended to existing track data (overdub mode)
    update_status_display();
}

static void stop_recording(void) {
    if (!recording && !recordArmed) return;
    recording = false;
    recordArmed = false;
    update_status_display();
}

static void sync_recording_to_capslock(void) {
    if (capsLockOn) {
        // Caps Lock turned ON -> arm recording
        arm_recording();
    } else {
        // Caps Lock turned OFF -> stop recording
        stop_recording();
    }
}

// Tempo functions
static void tempo_change(int bpm) {
    if (recording) return;  // Can't change during recording
    if (bpm < 20) bpm = 20;
    if (bpm > 300) bpm = 300;
    metronomeBPM = bpm;
    update_timing_constants();
    // Restart playback timer with new tempo-optimized interval
    if (clockRunning && playbackTimer) {
        start_playback_timer();
    }
    update_status_display();
}

static void tempo_change_timer_callback(CFRunLoopTimerRef timer, void *info) {
    tempo_change(metronomeBPM + tempoChangeDirection);
}

static void start_tempo_change_timer(int direction) {
    if (recording) return;
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

// Program change with auto-repeat
static void program_change_timer_callback(CFRunLoopTimerRef timer, void *info) {
    int newProgram = (tracks[currentChannel].program + programChangeDirection + 128) % 128;
    program_change(newProgram);
}

static void start_program_change_timer(int direction) {
    if (recording) return;
    programChangeDirection = direction;
    int newProgram = (tracks[currentChannel].program + direction + 128) % 128;
    program_change(newProgram);

    if (programChangeTimer) {
        CFRunLoopTimerInvalidate(programChangeTimer);
        CFRelease(programChangeTimer);
    }

    programChangeTimer = CFRunLoopTimerCreate(kCFAllocatorDefault,
        CFAbsoluteTimeGetCurrent() + 0.3,
        0.1,
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

// Octave functions
static void octave_up(void) {
    if (currentOctave < 8) {
        currentOctave++;
        update_status_display();
    }
}

static void octave_down(void) {
    if (currentOctave > 0) {
        currentOctave--;
        update_status_display();
    }
}

// Toggle metronome
static void toggle_metronome(void) {
    metronomeEnabled = !metronomeEnabled;
    update_status_display();
}

// Quantize all tracks to 16th note grid
static void quantize_all_tracks(void) {
    for (int t = 0; t < MIDI_TRACKS; t++) {
        MIDITrack *track = &tracks[t];
        for (int i = 0; i < track->eventCount; i++) {
            uint32_t tick = track->events[i].tick;
            // Round to nearest 16th note
            uint32_t quantized = ((tick + TICKS_PER_16TH / 2) / TICKS_PER_16TH) * TICKS_PER_16TH;
            // Wrap if quantized past loop end
            track->events[i].tick = quantized % totalLoopTicks;
        }
    }
}

// Toggle quantize
static void toggle_quantize(void) {
    quantizeEnabled = !quantizeEnabled;
    if (quantizeEnabled) {
        quantize_all_tracks();
    }
    update_status_display();
}

// MIDI File Save Function
static void write_variable_length(FILE *f, uint32_t value) {
    uint8_t buffer[4];
    int count = 0;

    buffer[count++] = value & 0x7F;
    while ((value >>= 7) > 0) {
        buffer[count++] = (value & 0x7F) | 0x80;
    }

    // Write in reverse order
    for (int i = count - 1; i >= 0; i--) {
        fputc(buffer[i], f);
    }
}

static void write_big_endian_16(FILE *f, uint16_t value) {
    fputc((value >> 8) & 0xFF, f);
    fputc(value & 0xFF, f);
}

static void write_big_endian_32(FILE *f, uint32_t value) {
    fputc((value >> 24) & 0xFF, f);
    fputc((value >> 16) & 0xFF, f);
    fputc((value >> 8) & 0xFF, f);
    fputc(value & 0xFF, f);
}

// Compare function for sorting events
static int compare_events(const void *a, const void *b) {
    const MIDIEvent *ea = (const MIDIEvent *)a;
    const MIDIEvent *eb = (const MIDIEvent *)b;
    if (ea->tick < eb->tick) return -1;
    if (ea->tick > eb->tick) return 1;
    return 0;
}

static void save_midi_file(void) {
    time_t now = time(NULL);
    struct tm *gmt = gmtime(&now);
    char filename[64];
    strftime(filename, sizeof(filename), "%Y%m%d_%H%M%S_GMT.mid", gmt);

    FILE *f = fopen(filename, "wb");
    if (!f) {
        printf("\r\033[KError: Could not create file %s", filename);
        fflush(stdout);
        return;
    }

    // Count tracks with events
    int trackCount = 1;  // Tempo track
    for (int i = 0; i < MIDI_TRACKS; i++) {
        if (tracks[i].eventCount > 0) trackCount++;
    }

    // MIDI Header
    fwrite("MThd", 1, 4, f);
    write_big_endian_32(f, 6);              // Header length
    write_big_endian_16(f, 1);              // Format 1
    write_big_endian_16(f, trackCount);     // Number of tracks
    write_big_endian_16(f, TICKS_PER_BEAT); // Ticks per quarter note

    // Tempo track
    fwrite("MTrk", 1, 4, f);
    long trackLenPos = ftell(f);
    write_big_endian_32(f, 0);  // Placeholder for track length

    long trackStart = ftell(f);

    // Tempo meta event
    write_variable_length(f, 0);  // Delta time
    fputc(0xFF, f);               // Meta event
    fputc(0x51, f);               // Tempo
    fputc(0x03, f);               // Length
    uint32_t microsPerBeat = 60000000 / metronomeBPM;
    fputc((microsPerBeat >> 16) & 0xFF, f);
    fputc((microsPerBeat >> 8) & 0xFF, f);
    fputc(microsPerBeat & 0xFF, f);

    // Time signature
    write_variable_length(f, 0);
    fputc(0xFF, f);
    fputc(0x58, f);
    fputc(0x04, f);
    fputc(BEATS_PER_BAR, f);  // Numerator
    fputc(2, f);               // Denominator (2 = quarter note)
    fputc(24, f);              // MIDI clocks per metronome click
    fputc(8, f);               // 32nd notes per quarter

    // End of track
    write_variable_length(f, 0);
    fputc(0xFF, f);
    fputc(0x2F, f);
    fputc(0x00, f);

    // Update track length
    long trackEnd = ftell(f);
    fseek(f, trackLenPos, SEEK_SET);
    write_big_endian_32(f, (uint32_t)(trackEnd - trackStart));
    fseek(f, trackEnd, SEEK_SET);

    // Write each track with events
    for (int t = 0; t < MIDI_TRACKS; t++) {
        MIDITrack *track = &tracks[t];
        if (track->eventCount == 0) continue;

        // Sort events by tick
        qsort(track->events, track->eventCount, sizeof(MIDIEvent), compare_events);

        fwrite("MTrk", 1, 4, f);
        trackLenPos = ftell(f);
        write_big_endian_32(f, 0);  // Placeholder
        trackStart = ftell(f);

        // Program change
        write_variable_length(f, 0);
        fputc(0xC0 | t, f);
        fputc(track->program, f);

        // Write events
        uint32_t lastTick = 0;
        for (int i = 0; i < track->eventCount; i++) {
            MIDIEvent *ev = &track->events[i];
            uint32_t delta = ev->tick - lastTick;
            lastTick = ev->tick;

            write_variable_length(f, delta);
            fputc(ev->status | t, f);
            fputc(ev->note, f);
            fputc(ev->velocity, f);
        }

        // End of track
        write_variable_length(f, 0);
        fputc(0xFF, f);
        fputc(0x2F, f);
        fputc(0x00, f);

        // Update track length
        trackEnd = ftell(f);
        fseek(f, trackLenPos, SEEK_SET);
        write_big_endian_32(f, (uint32_t)(trackEnd - trackStart));
        fseek(f, trackEnd, SEEK_SET);
    }

    fclose(f);
    printf("\r\033[KSaved: %s", filename);
    fflush(stdout);
}

// Status display
static void update_status_display(void) {
    int bar = currentBeat / BEATS_PER_BAR + 1;
    int beatInBar = currentBeat % BEATS_PER_BAR + 1;

    printf("\r\033[K");

    // Transport status
    if (clockRunning) {
        if (recording) {
            printf("\033[31m[REC %d/%d]\033[0m ", beatsRecorded, TOTAL_BEATS);
        } else if (recordArmed) {
            printf("\033[33m[ARM]\033[0m ");
        } else {
            printf("\033[32m[PLAY]\033[0m ");
        }
        printf("%d.%d ", bar, beatInBar);
    } else {
        printf("[STOP] ");
    }

    // Tempo, metronome, and quantize
    printf("%3dBPM ", metronomeBPM);
    printf("%s ", metronomeEnabled ? "M" : "-");
    printf("%s ", quantizeEnabled ? "Q" : "-");

    // Channel and octave
    printf("Ch%2d Oct%d ", currentChannel + 1, currentOctave);

    // Program (truncate name if too long)
    char progName[20];
    strncpy(progName, gmNames[tracks[currentChannel].program], 19);
    progName[19] = '\0';
    printf("P%03d:%.19s ", tracks[currentChannel].program, progName);

    // Event count for current track
    printf("[%d]", tracks[currentChannel].eventCount);

    fflush(stdout);
}

// Key mapping - O(1) lookup table version
static int keycode_to_note(uint16_t keycode) {
    if (keycode >= 32) return -1;
    uint8_t offset = keymapLUT[keycode];
    if (offset == 0) return -1;
    int note = (currentOctave * 12) + (offset - 1);
    return (note < 128) ? note : -1;
}

// HID callback
static void hid_callback(void *context, IOReturn result, void *sender, IOHIDValueRef value) {
    IOHIDElementRef element = IOHIDValueGetElement(value);
    uint32_t usagePage = IOHIDElementGetUsagePage(element);
    uint32_t usage = IOHIDElementGetUsage(element);
    long pressed = IOHIDValueGetIntegerValue(value);

    if (usagePage != kHIDPage_KeyboardOrKeypad) return;

    // ESC - Quit
    if (usage == ESC_KEYCODE && pressed) {
        printf("\n");
        CFRunLoopStop(CFRunLoopGetCurrent());
        return;
    }

    // SPACE - Toggle clock
    if (usage == SPACE_KEYCODE && pressed) {
        toggle_clock();
        return;
    }

    // CAPSLOCK - Recording synced to Caps Lock state
    if (usage == CAPSLOCK_KEYCODE && pressed) {
        capsLockOn = !capsLockOn;  // Toggle tracks actual Caps Lock state
        sync_recording_to_capslock();
        return;
    }

    // TAB - Toggle metronome
    if (usage == TAB_KEYCODE && pressed) {
        toggle_metronome();
        return;
    }

    // Arrow keys
    if (usage == LEFT_ARROW_KEYCODE && pressed) {
        octave_down();
        return;
    }
    if (usage == RIGHT_ARROW_KEYCODE && pressed) {
        octave_up();
        return;
    }
    if (usage == UP_ARROW_KEYCODE) {
        if (pressed) start_tempo_change_timer(1);
        else stop_tempo_change_timer();
        return;
    }
    if (usage == DOWN_ARROW_KEYCODE) {
        if (pressed) start_tempo_change_timer(-1);
        else stop_tempo_change_timer();
        return;
    }

    // MINUS - Channel down
    if (usage == MINUS_KEYCODE && pressed) {
        channel_change((currentChannel - 1 + 16) % 16);
        return;
    }

    // EQUALS - Channel up
    if (usage == EQUALS_KEYCODE && pressed) {
        channel_change((currentChannel + 1) % 16);
        return;
    }

    // Brackets - Program change
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

    // SLASH - Save
    if (usage == SLASH_KEYCODE && pressed) {
        save_midi_file();
        return;
    }

    // DELETE - Clear current track
    if (usage == DELETE_KEYCODE && pressed) {
        clear_current_track();
        return;
    }

    // BACKTICK - Toggle quantize
    if (usage == BACKTICK_KEYCODE && pressed) {
        toggle_quantize();
        return;
    }

    // Note keys
    int note = keycode_to_note(usage);
    if (note >= 0) {
        if (pressed) note_on(note, 100);
        else note_off(note);
    }
}

// HID initialization
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

// Main
int main(void) {
    // Initialize arrays
    memset(heldNoteChannel, -1, sizeof(heldNoteChannel));
    memset(tracks, 0, sizeof(tracks));

    init_timing();
    update_timing_constants();
    disable_echo();

    printf("terminalMIDI - 16-Track MIDI Recorder (optimised)\n");
    printf("══════════════════════════════════════════════════\n");
    printf("Notes:     z-m, a-l, q-p (3 rows)\n");
    printf("SPACE      Start/Stop clock\n");
    printf("CAPSLOCK   Record (while clock running)\n");
    printf("TAB        Toggle metronome\n");
    printf("`          Toggle quantize (16th notes)\n");
    printf("←/→        Octave down/up\n");
    printf("↑/↓        Tempo up/down (hold)\n");
    printf("-/=        Channel down/up\n");
    printf("[/]        Program down/up (hold)\n");
    printf("DELETE     Clear current track\n");
    printf("/          Save MIDI file\n");
    printf("ESC        Quit\n");
    printf("══════════════════════════════════════════════════\n");
    printf("Loop: %d bars x %d beats = %d beats total\n\n", TOTAL_BARS, BEATS_PER_BAR, TOTAL_BEATS);

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

    update_status_display();
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
