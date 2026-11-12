#ifndef _WIN32
	#define _POSIX_C_SOURCE 199309L // for clock_gettime and nanosleep
#endif

#include "Soundscape.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
	#include <windows.h>
#else
	#include <time.h>
#endif

// Listener: prints band intensities, either from an audio file or from live audio at run-time
// An audio file plays through the speakers while the samples being heard are analyzed, --offline analyzes the whole file without playback
// Without a file the microphone is captured, falling back to a synthesized stream when there is no capture device
// Levels are printed as percentages per band, followed by the bass, mids, treble and loudness and the strongest frequency
// Bands sum every frequency they span with an FFT, --goertzel measures only the center of each band as the Topl demos do

#define LISTENER_STEP 1024 // samples between windows when analyzing without playback
#define LISTENER_FRAME (1.0 / 60.0) // seconds between windows at run-time
#define LISTENER_PATH 1024

typedef struct {
	const char* path; // audio file, live capture without it
	SD_StreamSource source;
	unsigned deviceIndex, bandCount;
	int isOffline, isGoertzel, isDeviceList;
	double seconds; // time limit, 0 for none
	double interval; // seconds between printed rows
} Listener_Options;

typedef struct {
	SdBands bands;
	SdSpectrum* spectrum; // NULL to measure one frequency per band
	float window[SD_WINDOW];
	float peakFreq; // strongest frequency of the latest window
	int isPeakMeasured; // found in the spectrum, otherwise the center of the strongest band stands in
	double sums[SD_BANDS_MAX]; // for the average levels
	unsigned frameCount;
	double printTime;
	int isBeat; // a beat landed since the last printed row
} Listener;

static const char* sourceNames[3] = { "Synthesized stream", "Microphone", "System audio (loopback)" };

static volatile sig_atomic_t isRunning = 1;

static void onInterrupt(int signalID){ (void)signalID; isRunning = 0; } // Ctrl+C ends the loops and prints the summary

static double getSeconds(void){
#ifdef _WIN32
	LARGE_INTEGER counter, frequency;
	QueryPerformanceCounter(&counter);
	QueryPerformanceFrequency(&frequency);
	return (double)counter.QuadPart / (double)frequency.QuadPart;
#else
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (double)now.tv_sec + (double)now.tv_nsec * 1e-9;
#endif
}

static void sleepSeconds(double seconds){
#ifdef _WIN32
	Sleep((DWORD)(seconds * 1000.0));
#else
	struct timespec duration;
	duration.tv_sec = (time_t)seconds;
	duration.tv_nsec = (long)((seconds - (double)duration.tv_sec) * 1e9);
	nanosleep(&duration, NULL);
#endif
}

// Output

static void printHeader(const Listener* listener){
	unsigned b;
	printf("%8s |", "time");
	for(b = 0; b < listener->bands.count; b++) printf(" %4.0f", SdBand_getFreq(b, listener->bands.count, listener->bands.minFreq, listener->bands.maxFreq));
	printf(" | bass mids treb | loud | peak Hz\n");
}

static void printRow(Listener* listener, double time){
	unsigned b;
	printf("%7.2fs |", time);
	for(b = 0; b < listener->bands.count; b++) printf(" %4.0f", listener->bands.levels[b] * 100.0F);
	printf(" | %4.0f %4.0f %4.0f | %4.0f | %7.1f%s\n", listener->bands.bass * 100.0F, listener->bands.mids * 100.0F, listener->bands.treble * 100.0F,
		listener->bands.loudness * 100.0F, listener->peakFreq, (listener->isBeat)? "  beat" : "");
	fflush(stdout);
	listener->isBeat = 0;
}

static void printSummary(const Listener* listener){
	unsigned b, strongest = 0;
	if(listener->frameCount == 0){ printf("Nothing was heard\n"); return; }

	printf("%8s |", "average");
	for(b = 0; b < listener->bands.count; b++){
		printf(" %4.0f", listener->sums[b] / listener->frameCount * 100.0);
		if(listener->sums[b] > listener->sums[strongest]) strongest = b;
	}
	printf("\nStrongest band is %u of %u, centered on %.0f Hz\n", strongest + 1, listener->bands.count,
		SdBand_getFreq(strongest, listener->bands.count, listener->bands.minFreq, listener->bands.maxFreq));
}

// Analysis

static void report(Listener* listener, double time, double interval){ // keeps the averages and prints a row once per interval
	unsigned b, strongest = 0;
	for(b = 0; b < listener->bands.count; b++){
		listener->sums[b] += listener->bands.levels[b];
		if(listener->bands.levels[b] > listener->bands.levels[strongest]) strongest = b;
	}
	if(!listener->isPeakMeasured) listener->peakFreq = SdBand_getFreq(strongest, listener->bands.count, listener->bands.minFreq, listener->bands.maxFreq);
	if(listener->bands.isBeat) listener->isBeat = 1;

	if(listener->frameCount++ == 0 || time - listener->printTime >= interval){
		printRow(listener, time);
		listener->printTime = time;
	}
}

static void analyze(Listener* listener, float sampleRate, double frameSecs){ // bands from the samples in the window
	if(listener->spectrum != NULL){
		SdSpectrum_transform(listener->spectrum, listener->window, SD_WINDOW);
		SdBands_processSpectrum(&listener->bands, listener->spectrum, sampleRate, frameSecs);
		listener->peakFreq = SdSpectrum_getPeak(listener->spectrum, 20.0F, sampleRate * 0.5F, sampleRate, NULL);
		listener->isPeakMeasured = 1;
	} else SdBands_process(&listener->bands, listener->window, SD_WINDOW, sampleRate, frameSecs);
}

static SdTrack* openTrack(const char* path){ // looks inside of the assets when the path alone fails
	char assetPath[LISTENER_PATH];
	SdTrack* audio = SdTrack_open(path);
	if(audio == NULL && strlen(SD_ASSETS_DIR) + strlen(path) < LISTENER_PATH){
		sprintf(assetPath, "%s%s", SD_ASSETS_DIR, path);
		audio = SdTrack_open(assetPath);
	}
	if(audio == NULL) printf("Audio file failed to load: %s\n", path);
	else printf("%s is %.2f seconds at %.0f Hz\n", path, SdTrack_getDuration(audio), SdTrack_getSampleRate(audio));
	return audio;
}

static void analyzeFile(Listener* listener, SdTrack* audio, const Listener_Options* options){ // the whole file without playback
	float sampleRate = SdTrack_getSampleRate(audio);
	double frameSecs = LISTENER_STEP / sampleRate;
	unsigned long long start;
	unsigned count;

	printHeader(listener);
	for(start = 0; isRunning; start += LISTENER_STEP){
		double time = (double)start / sampleRate;
		if(options->seconds > 0.0 && time >= options->seconds) break;
		count = SdTrack_read(audio, start, listener->window, SD_WINDOW);
		if(count < SD_WINDOW && (start > 0 || count == 0)) break; // windows cut short by the end of the file smear across the bands
		analyze(listener, sampleRate, frameSecs);
		report(listener, time, options->interval);
	}
}

static int playFile(Listener* listener, SdTrack* audio, const Listener_Options* options){ // analysis follows the playback
	double startTime = getSeconds(), lastTime = startTime;
	if(!SdTrack_play(audio, 0)) return 0;

	printHeader(listener);
	while(isRunning && SdTrack_isPlaying(audio)){
		double now = getSeconds();
		if(options->seconds > 0.0 && now - startTime >= options->seconds) break;
		if(now - startTime > SdTrack_getDuration(audio) + 1.0 && SdTrack_getDuration(audio) > 0.0) break; // playback stalled

		if(SdTrack_listen(audio, listener->window, SD_WINDOW)){
			analyze(listener, SdTrack_getSampleRate(audio), now - lastTime);
			report(listener, SdTrack_getTime(audio), options->interval);
		}
		lastTime = now;
		sleepSeconds(LISTENER_FRAME);
	}
	SdTrack_stop(audio);
	return 1;
}

static void listenStream(Listener* listener, const Listener_Options* options){ // live capture at run-time
	SdStream* stream = SdStream_alloc();
	SD_StreamSource source = options->source;
	double startTime = getSeconds(), lastTime = startTime;
	int isLive = source != SD_STREAM_Synth && SdStream_start(stream, source, options->deviceIndex);

	if(isLive){
		printf("Streaming from %s", sourceNames[source]);
		if(source == SD_STREAM_Microphone) printf(": %s", SdStream_getDeviceName(stream, options->deviceIndex));
		printf(" at %.0f Hz\n", SdStream_getSampleRate(stream));
	} else {
		if(source != SD_STREAM_Synth) printf("%s is unavailable%s, using the synthesized stream\n", sourceNames[source], (source == SD_STREAM_Loopback)? " (loopback needs WASAPI on Windows)" : "");
		else printf("Synthesized stream\n");
	}
	printf("Press Ctrl+C to stop\n");
	SdBands_setAdaptive(&listener->bands, isLive); // rooms and microphones differ, so live levels sit above the noise floor

	printHeader(listener);
	while(isRunning){
		double now = getSeconds();
		int isHeard = 1;
		if(options->seconds > 0.0 && now - startTime >= options->seconds) break;

		if(!isLive) SdBands_synthesize(&listener->bands, now - startTime, now - lastTime);
		else if(SdStream_read(stream, listener->window, SD_WINDOW)) analyze(listener, SdStream_getSampleRate(stream), now - lastTime);
		else isHeard = 0; // the device has not delivered samples yet
		if(isHeard) report(listener, now - startTime, options->interval);

		lastTime = now;
		sleepSeconds(LISTENER_FRAME);
	}
	SdStream_free(stream);
}

static void listDevices(void){
	SdStream* stream = SdStream_alloc();
	unsigned d, count = SdStream_getDeviceCount(stream);
	printf("%u capture device%s\n", count, (count == 1)? "" : "s");
	for(d = 0; d < count; d++) printf("  %u: %s\n", d, SdStream_getDeviceName(stream, d));
	SdStream_free(stream);
}

// Example

static void printUsage(void){
	printf(
		"Listener [options] [audio file]\n"
		"  With an audio file the file plays and the audio being heard is analyzed, without one the microphone is captured\n"
		"  --offline      analyze the whole file without playing it\n"
		"  --device N     microphone to capture, see --devices\n"
		"  --devices      list the capture devices\n"
		"  --loopback     capture the system audio in place of the microphone (Windows)\n"
		"  --synth        synthesized stream in place of the microphone\n"
		"  --bands N      number of bands from 1 to %d, %d by default\n"
		"  --goertzel     measure one frequency per band, in place of every frequency inside each band\n"
		"  --seconds S    stop after S seconds\n"
		"  --interval S   seconds between printed rows, 0.25 by default\n", SD_BANDS_MAX, SD_BANDS
	);
}

static int parseOptions(int argc, char** argv, Listener_Options* options){
	int a;
	for(a = 1; a < argc; a++){
		const char* value = (a + 1 < argc)? argv[a + 1] : NULL;
		if(strcmp(argv[a], "--offline") == 0) options->isOffline = 1;
		else if(strcmp(argv[a], "--devices") == 0) options->isDeviceList = 1;
		else if(strcmp(argv[a], "--loopback") == 0) options->source = SD_STREAM_Loopback;
		else if(strcmp(argv[a], "--synth") == 0) options->source = SD_STREAM_Synth;
		else if(strcmp(argv[a], "--goertzel") == 0) options->isGoertzel = 1;
		else if(strcmp(argv[a], "--device") == 0 && value != NULL){ options->deviceIndex = (unsigned)strtoul(value, NULL, 10); a++; }
		else if(strcmp(argv[a], "--bands") == 0 && value != NULL){ options->bandCount = (unsigned)strtoul(value, NULL, 10); a++; }
		else if(strcmp(argv[a], "--seconds") == 0 && value != NULL){ options->seconds = atof(value); a++; }
		else if(strcmp(argv[a], "--interval") == 0 && value != NULL){ options->interval = atof(value); a++; }
		else if(argv[a][0] == '-') return 0; // includes --help
		else options->path = argv[a];
	}
	return 1;
}

int main(int argc, char** argv){
	Listener_Options options = { NULL, SD_STREAM_Microphone, 0, SD_BANDS, 0, 0, 0, 0.0, 0.25 };
	Listener* listener;

	if(!parseOptions(argc, argv, &options)){ printUsage(); return 1; }
	if(options.isDeviceList){ listDevices(); return 0; }

	listener = (Listener*)calloc(1, sizeof(Listener));
	if(listener == NULL) return 1;
	SdBands_init(&listener->bands, options.bandCount, SD_MIN_FREQ, SD_MAX_FREQ);
	if(!options.isGoertzel) listener->spectrum = SdSpectrum_alloc(SD_WINDOW);
	signal(SIGINT, onInterrupt);

	if(options.path != NULL){
		SdTrack* audio = openTrack(options.path);
		if(audio == NULL){ SdSpectrum_free(listener->spectrum); free(listener); return 1; }
		if(options.isOffline) analyzeFile(listener, audio, &options);
		else if(!playFile(listener, audio, &options)){
			printf("Playback is unavailable, analyzing without it\n");
			analyzeFile(listener, audio, &options);
		}
		SdTrack_close(audio);
	} else listenStream(listener, &options);

	printSummary(listener);
	SdSpectrum_free(listener->spectrum);
	free(listener);
	return 0;
}
