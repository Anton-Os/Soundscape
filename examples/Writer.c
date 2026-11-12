#include "Soundscape.h"

#ifdef SD_ENABLE_MIDI
	#include "MIDI.h" // MIDI Plugin
#endif

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Writer: exports what the library hears in audio files
// Every file is analyzed from start to end and the band intensities are written as a table (.csv), one row per window
// With SUPPORT_MIDI the strongest frequency of each window becomes a note and each beat a drum hit, written as a MIDI file (.mid)
// Without arguments the test tones in the assets are exported one after the other, from the lowest to the highest

#define WRITER_WINDOW 8192 // samples per window, long enough to tell low notes apart
#define WRITER_STEP 1024 // samples between windows
#define WRITER_SILENCE 0.01F // amplitude a frequency needs to count as a note, -40 dB
#define WRITER_HOLD 0.04 // a note holds while the frequency stays within 0.7 semitones of it
#define WRITER_TONES 14
#define WRITER_PATH 1024

typedef struct {
	const char* output; // name of the exported files without their extension
	unsigned bandCount;
	float tempo; // beats per minute of the MIDI file
} Writer_Options;

typedef struct {
	SdBands bands;
	SdSpectrum* spectrum;
	float window[WRITER_WINDOW];
	FILE* table;
	unsigned rowCount;
	double time; // seconds exported so far, files follow each other
	float tempo;
#ifdef SD_ENABLE_MIDI
	SdMidi* midi;
	int note; // note being held, -1 for silence
	unsigned velocity;
	double noteTime;
#endif
} Writer;

static const char* toneFiles[WRITER_TONES] = {
	"20hz-sine-freqies.mp3", "40hz-sine-freqies.mp3", "60hz-sine-freqies.mp3", "80hz-sine-freqies.mp3", "100hz-sine-freqies.mp3",
	"150hz-sine-freqies.mp3", "200hz-sine-freqies.mp3", "300hz-sine-freqies.mp3", "500hz-sine-freqies.mp3", "800hz-sine-freqies.mp3",
	"1000hz-sine-freqies.mp3", "2000hz-sine-freqies.mp3", "3000hz-sine-freqies.mp3", "5000hz-sine-freqies.mp3"
};

static const char* getFileName(const char* path){ // path without its directories
	const char* name = path;
	const char* c;
	for(c = path; *c != '\0'; c++) if(*c == '/' || *c == '\\') name = c + 1;
	return name;
}

static SdTrack* openTrack(const char* path){ // looks inside of the assets when the path alone fails
	char assetPath[WRITER_PATH];
	SdTrack* track = SdTrack_open(path);
	if(track == NULL && strlen(SD_ASSETS_DIR) + strlen(path) < WRITER_PATH){
		sprintf(assetPath, "%s%s", SD_ASSETS_DIR, path);
		track = SdTrack_open(assetPath);
	}
	return track;
}

// MIDI

#ifdef SD_ENABLE_MIDI
static void endNote(Writer* writer, double time){ // writes the note being held
	if(writer->note >= 0 && time > writer->noteTime){
		unsigned long tick = SdMidi_getTicks(writer->midi, writer->noteTime, writer->tempo);
		SdMidi_addNote(writer->midi, 1, tick, SdMidi_getTicks(writer->midi, time, writer->tempo) - tick, 0, (unsigned)writer->note, writer->velocity);
	}
	writer->note = -1;
}

static void writeNotes(Writer* writer, double time, float freq, float amplitude){
	int note = (amplitude >= WRITER_SILENCE)? SdMidi_getNote(freq) : -1;
	if(note >= 0 && writer->note >= 0 && fabs(log(freq / SdMidi_getFreq(writer->note))) < WRITER_HOLD) note = writer->note; // frequencies between two notes do not flicker

	if(note != writer->note){
		endNote(writer, time);
		writer->note = note;
		writer->noteTime = time;
		writer->velocity = 40 + (unsigned)(SdSignal_getLevel(amplitude, -40.0F, 0.0F) * 87.0F); // louder notes are struck harder
	}
	if(writer->bands.isBeat) SdMidi_addNote(writer->midi, 2, SdMidi_getTicks(writer->midi, time, writer->tempo), writer->midi->ticksPerBeat / 4, SD_MIDI_DRUMS, 36, 64 + (unsigned)(writer->bands.bass * 63.0F)); // bass drum
}
#endif

// Export

static int openExports(Writer* writer, const Writer_Options* options){
	char path[WRITER_PATH];
	unsigned b;
	if(strlen(options->output) + 5 > WRITER_PATH) return 0;

	sprintf(path, "%s.csv", options->output);
	writer->table = fopen(path, "w");
	if(writer->table == NULL){ printf("Table failed to open: %s\n", path); return 0; }
	fprintf(writer->table, "time,file");
	for(b = 0; b < writer->bands.count; b++) fprintf(writer->table, ",%.0fHz", SdBand_getFreq(b, writer->bands.count, writer->bands.minFreq, writer->bands.maxFreq));
	fprintf(writer->table, ",bass,mids,treble,loudness,beat,peak");
#ifdef SD_ENABLE_MIDI
	fprintf(writer->table, ",note");

	writer->midi = SdMidi_alloc(SD_MIDI_TICKS);
	writer->note = -1;
	if(writer->midi == NULL || SdMidi_addTrack(writer->midi, "Soundscape") < 0 || SdMidi_addTrack(writer->midi, "Notes") < 0 || SdMidi_addTrack(writer->midi, "Beats") < 0) return 0;
	SdMidi_addTempo(writer->midi, 0, 0, writer->tempo); // first track carries the tempo, the others carry the notes and the drums
	SdMidi_addTimeSignature(writer->midi, 0, 0, 4, 4);
	SdMidi_addProgram(writer->midi, 1, 0, 0, 73); // flute, close to the sound of a sine
#endif
	fprintf(writer->table, "\n");
	return 1;
}

static int exportFile(Writer* writer, const char* path){ // appends one audio file to the exports
	SdTrack* track = openTrack(path);
	float sampleRate, peakFreq = 0.0F, peakAmplitude = 0.0F;
	unsigned long long start;
	unsigned b, count;
	double duration;
	if(track == NULL){ printf("Audio file failed to load: %s\n", path); return 0; }

	sampleRate = SdTrack_getSampleRate(track);
	for(start = 0; (count = SdTrack_read(track, start, writer->window, WRITER_WINDOW)) == WRITER_WINDOW || (start == 0 && count > 0); start += WRITER_STEP){ // windows cut short by the end of the file smear across the bands
		double time = writer->time + (double)start / sampleRate;
		float amplitude, freq;

		SdSpectrum_transform(writer->spectrum, writer->window, WRITER_WINDOW);
		SdBands_processSpectrum(&writer->bands, writer->spectrum, sampleRate, WRITER_STEP / sampleRate);
		freq = SdSpectrum_getPeak(writer->spectrum, 16.0F, sampleRate * 0.5F, sampleRate, &amplitude);
		if(amplitude > peakAmplitude){ peakAmplitude = amplitude; peakFreq = freq; }

		fprintf(writer->table, "%.4f,%s", time, getFileName(path));
		for(b = 0; b < writer->bands.count; b++) fprintf(writer->table, ",%.3f", writer->bands.levels[b]);
		fprintf(writer->table, ",%.3f,%.3f,%.3f,%.3f,%d,%.1f", writer->bands.bass, writer->bands.mids, writer->bands.treble, writer->bands.loudness, writer->bands.isBeat, freq);
#ifdef SD_ENABLE_MIDI
		{
			char name[SD_MIDI_NAME];
			writeNotes(writer, time, freq, amplitude);
			fprintf(writer->table, ",%s", SdMidi_getNoteName(writer->note, name));
		}
#endif
		fprintf(writer->table, "\n");
		writer->rowCount++;
	}
	duration = (SdTrack_getLength(track) > 0)? SdTrack_getDuration(track) : (double)(start + count) / sampleRate;
	writer->time += duration;
#ifdef SD_ENABLE_MIDI
	endNote(writer, writer->time); // notes last until the end of their file and do not carry over into the next
#endif

	printf("%s: %.2f seconds, strongest frequency %.1f Hz", getFileName(path), duration, peakFreq);
#ifdef SD_ENABLE_MIDI
	{
		char name[SD_MIDI_NAME];
		printf(" (%s)", SdMidi_getNoteName(SdMidi_getNote(peakFreq), name));
	}
#endif
	printf("\n");
	SdTrack_close(track);
	return 1;
}

static void closeExports(Writer* writer, const Writer_Options* options){
	if(writer->table != NULL){
		fclose(writer->table);
		printf("Wrote %s.csv with %u rows of %u bands\n", options->output, writer->rowCount, writer->bands.count);
	}
#ifdef SD_ENABLE_MIDI
	if(writer->midi != NULL && writer->midi->trackCount == 3){
		char path[WRITER_PATH];
		SdMidi* written;
		sprintf(path, "%s.mid", options->output);
		if(SdMidi_write(writer->midi, path) && (written = SdMidi_read(path)) != NULL){ // read back to confirm the file
			printf("Wrote %s with %u tracks and %u notes over %.2f seconds\n", path, written->trackCount, SdMidi_getNoteCount(written), SdMidi_getSeconds(written, SdMidi_getLength(written)));
			SdMidi_free(written);
		} else printf("MIDI file failed to write: %s\n", path);
	}
	SdMidi_free(writer->midi);
#else
	printf("MIDI export requires SUPPORT_MIDI\n");
#endif
}

// Example

static void printUsage(void){
	printf(
		"Writer [options] [audio files]\n"
		"  Exports the band intensities of the audio files as a table (.csv) and, with SUPPORT_MIDI, the notes heard as a MIDI file (.mid)\n"
		"  Without audio files the test tones in the assets are exported\n"
		"  --output NAME  name of the exported files without their extension, Soundscape by default\n"
		"  --bands N      number of bands from 1 to %d, %d by default\n"
		"  --tempo BPM    tempo of the MIDI file, %.0f by default\n", SD_BANDS_MAX, SD_BANDS, 120.0
	);
}

int main(int argc, char** argv){
	Writer_Options options = { "Soundscape", SD_BANDS, 120.0F };
	Writer* writer;
	unsigned fileCount = 0, exportCount = 0;
	int a;

	for(a = 1; a < argc; a++){
		const char* value = (a + 1 < argc)? argv[a + 1] : NULL;
		if(strcmp(argv[a], "--output") == 0 && value != NULL){ options.output = value; argv[a] = NULL; argv[++a] = NULL; }
		else if(strcmp(argv[a], "--bands") == 0 && value != NULL){ options.bandCount = (unsigned)strtoul(value, NULL, 10); argv[a] = NULL; argv[++a] = NULL; }
		else if(strcmp(argv[a], "--tempo") == 0 && value != NULL){ options.tempo = (float)atof(value); argv[a] = NULL; argv[++a] = NULL; }
		else if(argv[a][0] == '-'){ printUsage(); return 1; } // includes --help
		else fileCount++;
	}
	if(options.tempo <= 0.0F) options.tempo = 120.0F;

	writer = (Writer*)calloc(1, sizeof(Writer));
	if(writer == NULL) return 1;
	writer->tempo = options.tempo;
	writer->spectrum = SdSpectrum_alloc(WRITER_WINDOW);
	SdBands_init(&writer->bands, options.bandCount, SD_MIN_FREQ, SD_MAX_FREQ);

	if(writer->spectrum != NULL && openExports(writer, &options)){
		if(fileCount == 0) for(a = 0; a < WRITER_TONES; a++) exportCount += exportFile(writer, toneFiles[a]);
		else for(a = 1; a < argc; a++) if(argv[a] != NULL) exportCount += exportFile(writer, argv[a]);
	}
	closeExports(writer, &options);

	SdSpectrum_free(writer->spectrum);
	free(writer);
	return (exportCount > 0)? 0 : 1;
}
