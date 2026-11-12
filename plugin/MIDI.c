#include "MIDI.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MIDI_BEAT_MICROS 500000.0 // microseconds per quarter note at 120 beats per minute
#define MIDI_MAX_QUANTITY 0x0FFFFFFFUL // largest value a variable length quantity holds

typedef struct {
	unsigned char* data;
	size_t size, capacity;
	int isFailed; // out of memory or a value that files cannot hold
} Midi_Buffer;

typedef struct {
	unsigned long tick;
	double micros; // microseconds per quarter note
	unsigned order;
} Midi_Tempo;

static const char* noteNames[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

static int isChannelEvent(const SdMidiEvent* event){ return event->status >= 0x80 && event->status < 0xF0; }

static int isBytesEvent(const SdMidiEvent* event){ return event->status == SD_MIDI_Meta || event->status == SD_MIDI_Sysex || event->status == 0xF7; }

static int isNoteOff(const SdMidiEvent* event){
	return (event->status & 0xF0) == SD_MIDI_NoteOff || ((event->status & 0xF0) == SD_MIDI_NoteOn && event->data[1] == 0);
}

static int isEndOfTrack(const SdMidiEvent* event){ return event->status == SD_MIDI_Meta && event->data[0] == SD_META_EndOfTrack; }

static unsigned getDataSize(unsigned char status){ return ((status & 0xF0) == SD_MIDI_Program || (status & 0xF0) == SD_MIDI_Pressure)? 1 : 2; }

// Buffer

static void pushBytes(Midi_Buffer* buffer, const void* bytes, size_t length){
	if(buffer->isFailed || length == 0) return;
	if(buffer->size + length > buffer->capacity){
		size_t capacity = (buffer->capacity > 0)? buffer->capacity * 2 : 256;
		unsigned char* data;
		while(capacity < buffer->size + length) capacity *= 2;
		data = (unsigned char*)realloc(buffer->data, capacity);
		if(data == NULL){ buffer->isFailed = 1; return; }
		buffer->data = data;
		buffer->capacity = capacity;
	}
	memcpy(buffer->data + buffer->size, bytes, length);
	buffer->size += length;
}

static void pushByte(Midi_Buffer* buffer, unsigned value){
	unsigned char byte = (unsigned char)(value & 0xFF);
	pushBytes(buffer, &byte, 1);
}

static void pushNumber(Midi_Buffer* buffer, unsigned long value, unsigned size){ // big endian
	while(size-- > 0) pushByte(buffer, (unsigned)(value >> (size * 8)));
}

static void pushQuantity(Midi_Buffer* buffer, unsigned long value){ // variable length, 7 bits per byte
	unsigned shift = 28;
	if(value > MIDI_MAX_QUANTITY){ buffer->isFailed = 1; return; } // files cannot hold it
	while(shift > 0){
		shift -= 7;
		if(shift == 0) pushByte(buffer, (unsigned)(value & 0x7F));
		else if((value >> shift) != 0) pushByte(buffer, (unsigned)(((value >> shift) & 0x7F) | 0x80));
	}
}

// Events

static SdMidiEvent* addEvent(SdMidi* midi, unsigned track, unsigned long tick){
	SdMidiTrack* target;
	SdMidiEvent* event;
	if(midi == NULL || track >= midi->trackCount) return NULL;

	target = &midi->tracks[track];
	if(target->count == target->capacity){
		unsigned capacity = (target->capacity > 0)? target->capacity * 2 : 64;
		SdMidiEvent* events = (SdMidiEvent*)realloc(target->events, capacity * sizeof(SdMidiEvent));
		if(events == NULL) return NULL;
		target->events = events;
		target->capacity = capacity;
	}
	event = &target->events[target->count++];
	memset(event, 0, sizeof(SdMidiEvent));
	event->tick = tick;
	return event;
}

static int addBytesEvent(SdMidi* midi, unsigned track, unsigned long tick, unsigned char status, unsigned char type, const void* bytes, unsigned length){
	unsigned char* copy = NULL;
	SdMidiEvent* event;
	if(bytes == NULL) length = 0;
	if(length > 0){
		copy = (unsigned char*)malloc(length);
		if(copy == NULL) return 0;
		memcpy(copy, bytes, length);
	}

	event = addEvent(midi, track, tick);
	if(event == NULL){ free(copy); return 0; }
	event->status = status;
	event->data[0] = type;
	event->bytes = copy;
	event->length = length;
	return 1;
}

SdMidi* SdMidi_alloc(unsigned ticksPerBeat){
	SdMidi* midi = (SdMidi*)calloc(1, sizeof(SdMidi));
	if(midi == NULL) return NULL;
	midi->ticksPerBeat = (ticksPerBeat == 0)? SD_MIDI_TICKS : (ticksPerBeat > 0x7FFF)? 0x7FFF : ticksPerBeat;
	return midi;
}

void SdMidi_free(SdMidi* midi){
	unsigned t, e;
	if(midi == NULL) return;
	for(t = 0; t < midi->trackCount; t++){
		for(e = 0; e < midi->tracks[t].count; e++) free(midi->tracks[t].events[e].bytes);
		free(midi->tracks[t].events);
	}
	free(midi->tracks);
	free(midi);
}

int SdMidi_addTrack(SdMidi* midi, const char* name){
	SdMidiTrack* tracks;
	if(midi == NULL || midi->trackCount >= 0xFFFF) return -1;

	tracks = (SdMidiTrack*)realloc(midi->tracks, (midi->trackCount + 1) * sizeof(SdMidiTrack));
	if(tracks == NULL) return -1;
	midi->tracks = tracks;
	memset(&midi->tracks[midi->trackCount], 0, sizeof(SdMidiTrack));
	midi->trackCount++;

	if(name != NULL && name[0] != '\0') SdMidi_addMeta(midi, midi->trackCount - 1, 0, SD_META_TrackName, name, (unsigned)strlen(name));
	return (int)(midi->trackCount - 1);
}

int SdMidi_addEvent(SdMidi* midi, unsigned track, unsigned long tick, unsigned char status, unsigned char data1, unsigned char data2){
	SdMidiEvent* event;
	if(status < 0x80 || status >= 0xF0) return 0; // channel events only
	event = addEvent(midi, track, tick);
	if(event == NULL) return 0;

	event->status = status;
	event->data[0] = data1 & 0x7F;
	event->data[1] = data2 & 0x7F;
	return 1;
}

int SdMidi_addMeta(SdMidi* midi, unsigned track, unsigned long tick, unsigned char type, const void* bytes, unsigned length){
	return addBytesEvent(midi, track, tick, SD_MIDI_Meta, type & 0x7F, bytes, length);
}

int SdMidi_addTempo(SdMidi* midi, unsigned track, unsigned long tick, float beatsPerMinute){
	unsigned long micros;
	unsigned char bytes[3];
	if(beatsPerMinute <= 0.0F) return 0;

	micros = (unsigned long)(60000000.0 / beatsPerMinute + 0.5); // microseconds per quarter note
	if(micros > 0xFFFFFFUL) micros = 0xFFFFFFUL;
	bytes[0] = (unsigned char)((micros >> 16) & 0xFF);
	bytes[1] = (unsigned char)((micros >> 8) & 0xFF);
	bytes[2] = (unsigned char)(micros & 0xFF);
	return SdMidi_addMeta(midi, track, tick, SD_META_Tempo, bytes, 3);
}

int SdMidi_addTimeSignature(SdMidi* midi, unsigned track, unsigned long tick, unsigned numerator, unsigned denominator){
	unsigned char bytes[4];
	unsigned power = 0;
	if(numerator == 0 || numerator > 255 || denominator == 0) return 0;

	while((1U << power) < denominator && power < 7) power++; // denominator is stored as a power of two
	bytes[0] = (unsigned char)numerator;
	bytes[1] = (unsigned char)power;
	bytes[2] = 24; // metronome click every quarter note
	bytes[3] = 8; // 32nd notes per quarter note
	return SdMidi_addMeta(midi, track, tick, SD_META_TimeSignature, bytes, 4);
}

int SdMidi_addProgram(SdMidi* midi, unsigned track, unsigned long tick, unsigned channel, unsigned program){
	return SdMidi_addEvent(midi, track, tick, (unsigned char)(SD_MIDI_Program | (channel & 0x0F)), (unsigned char)program, 0);
}

int SdMidi_addControl(SdMidi* midi, unsigned track, unsigned long tick, unsigned channel, unsigned controller, unsigned value){
	return SdMidi_addEvent(midi, track, tick, (unsigned char)(SD_MIDI_Control | (channel & 0x0F)), (unsigned char)controller, (unsigned char)value);
}

int SdMidi_addNote(SdMidi* midi, unsigned track, unsigned long tick, unsigned long duration, unsigned channel, unsigned note, unsigned velocity){
	if(note > 127) return 0;
	if(velocity > 127) velocity = 127;
	if(velocity == 0) velocity = 1; // a note on without velocity reads as a note off
	if(duration == 0) duration = 1;
	if(tick + duration < tick) return 0; // past the last tick

	if(!SdMidi_addEvent(midi, track, tick, (unsigned char)(SD_MIDI_NoteOn | (channel & 0x0F)), (unsigned char)note, (unsigned char)velocity)) return 0;
	if(SdMidi_addEvent(midi, track, tick + duration, (unsigned char)(SD_MIDI_NoteOff | (channel & 0x0F)), (unsigned char)note, 0)) return 1;
	midi->tracks[track].count--; // a note on is not left without its note off
	return 0;
}

// Writing

static int compareEvents(const void* first, const void* second){
	const SdMidiEvent* a = *(const SdMidiEvent* const*)first;
	const SdMidiEvent* b = *(const SdMidiEvent* const*)second;
	if(a->tick != b->tick) return (a->tick < b->tick)? -1 : 1;
	if(isNoteOff(a) != isNoteOff(b)) return (isNoteOff(a))? -1 : 1; // notes end before the next ones start
	return (a < b)? -1 : (a > b)? 1 : 0; // otherwise in the order they were added
}

static int writeTrack(const SdMidiTrack* track, Midi_Buffer* chunk){
	const SdMidiEvent** events = (const SdMidiEvent**)malloc((track->count + 1) * sizeof(const SdMidiEvent*));
	unsigned long lastTick = 0, endTick = 0;
	unsigned e, count = 0;
	if(events == NULL) return 0;

	for(e = 0; e < track->count; e++){
		const SdMidiEvent* event = &track->events[e];
		if(event->tick > endTick) endTick = event->tick;
		if(!isEndOfTrack(event) && (isChannelEvent(event) || isBytesEvent(event))) events[count++] = event; // the end of the track is written last
	}
	qsort(events, count, sizeof(const SdMidiEvent*), compareEvents);

	for(e = 0; e < count; e++){
		const SdMidiEvent* event = events[e];
		pushQuantity(chunk, event->tick - lastTick);
		lastTick = event->tick;

		pushByte(chunk, event->status);
		if(isChannelEvent(event)) pushBytes(chunk, event->data, getDataSize(event->status));
		else {
			if(event->status == SD_MIDI_Meta) pushByte(chunk, event->data[0]);
			pushQuantity(chunk, event->length);
			pushBytes(chunk, event->bytes, event->length);
		}
	}
	pushQuantity(chunk, endTick - lastTick);
	pushByte(chunk, SD_MIDI_Meta);
	pushByte(chunk, SD_META_EndOfTrack);
	pushByte(chunk, 0);

	free((void*)events);
	return !chunk->isFailed;
}

int SdMidi_write(const SdMidi* midi, const char* path){
	Midi_Buffer file = { NULL, 0, 0, 0 };
	FILE* output;
	unsigned t;
	int isWritten;
	if(midi == NULL || path == NULL || midi->trackCount == 0 || midi->ticksPerBeat == 0 || midi->ticksPerBeat > 0x7FFF) return 0;

	pushBytes(&file, "MThd", 4);
	pushNumber(&file, 6, 4);
	pushNumber(&file, (midi->trackCount > 1)? 1 : 0, 2); // format
	pushNumber(&file, midi->trackCount, 2);
	pushNumber(&file, midi->ticksPerBeat, 2);

	for(t = 0; t < midi->trackCount; t++){
		Midi_Buffer chunk = { NULL, 0, 0, 0 };
		if(!writeTrack(&midi->tracks[t], &chunk)) file.isFailed = 1;
		pushBytes(&file, "MTrk", 4);
		pushNumber(&file, (unsigned long)chunk.size, 4);
		pushBytes(&file, chunk.data, chunk.size);
		free(chunk.data);
	}
	if(file.isFailed){ free(file.data); return 0; }

	output = fopen(path, "wb");
	isWritten = output != NULL && fwrite(file.data, 1, file.size, output) == file.size;
	if(output != NULL && fclose(output) != 0) isWritten = 0;
	free(file.data);
	return isWritten;
}

// Reading

static unsigned long readNumber(const unsigned char* data, unsigned size){ // big endian
	unsigned long value = 0;
	unsigned b;
	for(b = 0; b < size; b++) value = (value << 8) | data[b];
	return value;
}

static int readQuantity(const unsigned char* data, size_t size, size_t* position, unsigned long* value){ // variable length, 7 bits per byte
	unsigned b;
	*value = 0;
	for(b = 0; b < 4 && *position < size; b++){
		unsigned char byte = data[(*position)++];
		*value = (*value << 7) | (byte & 0x7F);
		if((byte & 0x80) == 0) return 1;
	}
	return 0;
}

static int readTrack(SdMidi* midi, unsigned track, const unsigned char* data, size_t size){
	size_t position = 0;
	unsigned long tick = 0, delta, length;
	unsigned char status, running = 0; // channel events repeat the last status when they leave it out

	while(position < size){
		if(!readQuantity(data, size, &position, &delta) || position >= size) return 0;
		tick += delta;

		if(data[position] & 0x80){
			status = data[position++];
			if(status < 0xF0) running = status;
		} else if(running != 0) status = running;
		else return 0;

		if(status == SD_MIDI_Meta){
			unsigned char type;
			if(position >= size) return 0;
			type = data[position++];
			if(!readQuantity(data, size, &position, &length) || length > size - position) return 0;
			if(!addBytesEvent(midi, track, tick, SD_MIDI_Meta, type, data + position, (unsigned)length)) return 0;
			position += length;
			if(type == SD_META_EndOfTrack) return 1;
		} else if(status == SD_MIDI_Sysex || status == 0xF7){
			if(!readQuantity(data, size, &position, &length) || length > size - position) return 0;
			if(!addBytesEvent(midi, track, tick, status, 0, data + position, (unsigned)length)) return 0;
			position += length;
		} else if(status < 0xF0){
			unsigned dataSize = getDataSize(status);
			if(dataSize > size - position) return 0;
			if(!SdMidi_addEvent(midi, track, tick, status, data[position], (dataSize > 1)? data[position + 1] : 0)) return 0;
			position += dataSize;
		} else return 0; // other system messages do not belong in files
	}
	return 1;
}

SdMidi* SdMidi_read(const char* path){
	FILE* input;
	unsigned char* data = NULL;
	long fileSize = 0;
	size_t size, position;
	unsigned long division;
	SdMidi* midi = NULL;
	int isValid = 1;
	if(path == NULL) return NULL;

	input = fopen(path, "rb");
	if(input == NULL) return NULL;
	if(fseek(input, 0, SEEK_END) == 0 && (fileSize = ftell(input)) >= 14 && fseek(input, 0, SEEK_SET) == 0){
		data = (unsigned char*)malloc((size_t)fileSize);
		if(data != NULL && fread(data, 1, (size_t)fileSize, input) != (size_t)fileSize){ free(data); data = NULL; }
	}
	fclose(input);
	if(data == NULL) return NULL;
	size = (size_t)fileSize;

	division = readNumber(data + 12, 2);
	if(memcmp(data, "MThd", 4) != 0 || readNumber(data + 4, 4) < 6 || readNumber(data + 4, 4) > size - 8 || division == 0 || division > 0x7FFF){ // timecode divisions are left out
		free(data);
		return NULL;
	}
	midi = SdMidi_alloc((unsigned)division);
	position = 8 + (size_t)readNumber(data + 4, 4);

	while(midi != NULL && isValid && size - position >= 8){
		size_t chunkSize = (size_t)readNumber(data + position + 4, 4);
		int isTrack = memcmp(data + position, "MTrk", 4) == 0;
		position += 8;
		if(chunkSize > size - position) chunkSize = size - position; // cut short
		if(isTrack) isValid = SdMidi_addTrack(midi, NULL) >= 0 && readTrack(midi, midi->trackCount - 1, data + position, chunkSize);
		position += chunkSize; // other chunks are skipped
	}

	free(data);
	if(midi != NULL && (!isValid || midi->trackCount == 0)){ SdMidi_free(midi); midi = NULL; }
	return midi;
}

// Timing

unsigned long SdMidi_getLength(const SdMidi* midi){
	unsigned long length = 0;
	unsigned t, e;
	if(midi == NULL) return 0;
	for(t = 0; t < midi->trackCount; t++)
		for(e = 0; e < midi->tracks[t].count; e++)
			if(midi->tracks[t].events[e].tick > length) length = midi->tracks[t].events[e].tick;
	return length;
}

unsigned long SdMidi_getTicks(const SdMidi* midi, double seconds, float beatsPerMinute){
	if(midi == NULL || seconds <= 0.0 || beatsPerMinute <= 0.0F) return 0;
	return (unsigned long)(seconds * beatsPerMinute / 60.0 * midi->ticksPerBeat + 0.5);
}

static int compareTempos(const void* first, const void* second){
	const Midi_Tempo* a = (const Midi_Tempo*)first;
	const Midi_Tempo* b = (const Midi_Tempo*)second;
	if(a->tick != b->tick) return (a->tick < b->tick)? -1 : 1;
	return (a->order < b->order)? -1 : (a->order > b->order)? 1 : 0;
}

double SdMidi_getSeconds(const SdMidi* midi, unsigned long tick){
	Midi_Tempo* tempos;
	double seconds = 0.0, micros = MIDI_BEAT_MICROS;
	unsigned long lastTick = 0;
	unsigned t, e, count = 0, total = 0;
	if(midi == NULL || midi->ticksPerBeat == 0) return 0.0;

	for(t = 0; t < midi->trackCount; t++) total += midi->tracks[t].count;
	tempos = (Midi_Tempo*)malloc((total + 1) * sizeof(Midi_Tempo));
	if(tempos == NULL) return 0.0;
	for(t = 0; t < midi->trackCount; t++)
		for(e = 0; e < midi->tracks[t].count; e++){
			const SdMidiEvent* event = &midi->tracks[t].events[e];
			if(event->status != SD_MIDI_Meta || event->data[0] != SD_META_Tempo || event->length < 3) continue;
			tempos[count].tick = event->tick;
			tempos[count].micros = (double)readNumber(event->bytes, 3);
			tempos[count].order = count;
			count++;
		}
	qsort(tempos, count, sizeof(Midi_Tempo), compareTempos);

	for(t = 0; t < count && tempos[t].tick < tick; t++){ // every stretch plays at the tempo set before it
		seconds += (double)(tempos[t].tick - lastTick) * micros / (midi->ticksPerBeat * 1000000.0);
		lastTick = tempos[t].tick;
		micros = tempos[t].micros;
	}
	seconds += (double)(tick - lastTick) * micros / (midi->ticksPerBeat * 1000000.0);

	free(tempos);
	return seconds;
}

unsigned SdMidi_getNoteCount(const SdMidi* midi){
	unsigned t, e, count = 0;
	if(midi == NULL) return 0;
	for(t = 0; t < midi->trackCount; t++)
		for(e = 0; e < midi->tracks[t].count; e++){
			const SdMidiEvent* event = &midi->tracks[t].events[e];
			if((event->status & 0xF0) == SD_MIDI_NoteOn && event->data[1] > 0) count++;
		}
	return count;
}

// Notes

int SdMidi_getNote(float freq){
	double note;
	if(freq <= 0.0F) return -1;
	note = floor(69.0 + 12.0 * log(freq / 440.0) / log(2.0) + 0.5);
	return (note >= 0.0 && note <= 127.0)? (int)note : -1;
}

float SdMidi_getFreq(int note){ return 440.0F * (float)pow(2.0, (note - 69) / 12.0); }

const char* SdMidi_getNoteName(int note, char* name){
	if(name == NULL) return "";
	if(note < 0 || note > 127) name[0] = '\0';
	else sprintf(name, "%s%d", noteNames[note % 12], note / 12 - 1);
	return name;
}
