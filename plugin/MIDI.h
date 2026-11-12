#ifndef SOUNDSCAPE_MIDI_H
#define SOUNDSCAPE_MIDI_H

#ifdef __cplusplus
extern "C" {
#endif

// MIDI plugin: reads and writes standard MIDI files (.mid), built when SUPPORT_MIDI is set
// Events are kept per track with their time in ticks from the start of the song
// Notes follow the MIDI numbering, where 60 is middle C and 69 is the A at 440 Hz

#define SD_MIDI_TICKS 480 // default ticks per quarter note
#define SD_MIDI_TEMPO 120.0F // beats per minute until a tempo event says otherwise
#define SD_MIDI_DRUMS 9 // channel that General MIDI keeps for percussion
#define SD_MIDI_NAME 8 // characters to hold a note name

typedef enum {
	SD_MIDI_NoteOff = 0x80,
	SD_MIDI_NoteOn = 0x90,
	SD_MIDI_Aftertouch = 0xA0,
	SD_MIDI_Control = 0xB0,
	SD_MIDI_Program = 0xC0,
	SD_MIDI_Pressure = 0xD0,
	SD_MIDI_PitchBend = 0xE0,
	SD_MIDI_Sysex = 0xF0,
	SD_MIDI_Meta = 0xFF
} SD_MidiStatus;

typedef enum {
	SD_META_Text = 0x01,
	SD_META_TrackName = 0x03,
	SD_META_EndOfTrack = 0x2F,
	SD_META_Tempo = 0x51,
	SD_META_TimeSignature = 0x58,
	SD_META_KeySignature = 0x59
} SD_MidiMeta;

typedef struct {
	unsigned long tick;
	unsigned char status; // SD_MidiStatus, channel events carry their channel in the low 4 bits
	unsigned char data[2]; // note and velocity, controller and value or program, meta events keep their type in data[0]
	unsigned char* bytes; // contents of meta and sysex events
	unsigned length;
} SdMidiEvent;

typedef struct {
	SdMidiEvent* events; // in the order they were added, files are written sorted by time
	unsigned count, capacity;
} SdMidiTrack;

typedef struct {
	unsigned ticksPerBeat; // ticks per quarter note
	SdMidiTrack* tracks;
	unsigned trackCount;
} SdMidi;

SdMidi* SdMidi_alloc(unsigned ticksPerBeat); // empty song, 0 picks the default ticks
void SdMidi_free(SdMidi* midi);
SdMidi* SdMidi_read(const char* path); // NULL when the file is missing or malformed
int SdMidi_write(const SdMidi* midi, const char* path); // one track writes format 0, several write format 1, false when two events of a track lie more than 0x0FFFFFFF ticks apart

// Every add returns false when the track is missing or out of memory

int SdMidi_addTrack(SdMidi* midi, const char* name); // returns the track index, -1 on failure
int SdMidi_addEvent(SdMidi* midi, unsigned track, unsigned long tick, unsigned char status, unsigned char data1, unsigned char data2); // channel events
int SdMidi_addMeta(SdMidi* midi, unsigned track, unsigned long tick, unsigned char type, const void* bytes, unsigned length);
int SdMidi_addTempo(SdMidi* midi, unsigned track, unsigned long tick, float beatsPerMinute);
int SdMidi_addTimeSignature(SdMidi* midi, unsigned track, unsigned long tick, unsigned numerator, unsigned denominator);
int SdMidi_addProgram(SdMidi* midi, unsigned track, unsigned long tick, unsigned channel, unsigned program); // instrument for a channel
int SdMidi_addControl(SdMidi* midi, unsigned track, unsigned long tick, unsigned channel, unsigned controller, unsigned value);
int SdMidi_addNote(SdMidi* midi, unsigned track, unsigned long tick, unsigned long duration, unsigned channel, unsigned note, unsigned velocity); // note on and its note off

unsigned long SdMidi_getLength(const SdMidi* midi); // tick of the last event
unsigned long SdMidi_getTicks(const SdMidi* midi, double seconds, float beatsPerMinute); // time at a steady tempo converted to ticks
double SdMidi_getSeconds(const SdMidi* midi, unsigned long tick); // time of a tick, follows the tempo events
unsigned SdMidi_getNoteCount(const SdMidi* midi);

int SdMidi_getNote(float freq); // closest note to a frequency, -1 outside of the MIDI range
float SdMidi_getFreq(int note);
const char* SdMidi_getNoteName(int note, char* name); // such as C4 or F#2, name holds SD_MIDI_NAME characters

#ifdef __cplusplus
}
#endif

#endif
