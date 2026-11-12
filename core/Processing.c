#include "Soundscape.h"

#define TRACK_CHUNK 1024 // samples decoded at a time

struct SdTrack {
	char* path;
	ma_decoder decoder; // for analysis
	ma_sound sound; // for playback
	int isSound, isPaused;
	float sampleRate;
	unsigned long long length;
	unsigned long long cursor; // samples decoded so far
	float recent[SD_RING_BUFFER]; // newest decoded samples, overlapping reads are served from here
};

static ma_engine engine; // shared by every track that plays
static unsigned engineUsers = 0;

static int acquireEngine(void){
	if(engineUsers == 0 && ma_engine_init(NULL, &engine) != MA_SUCCESS) return 0;
	engineUsers++;
	return 1;
}

static void releaseEngine(void){
	if(engineUsers > 0 && --engineUsers == 0) ma_engine_uninit(&engine);
}

// Signal

float SdSignal_getRMS(const float* samples, unsigned count){
	double sum = 0.0;
	unsigned s;
	if(samples == NULL || count == 0) return 0.0F;
	for(s = 0; s < count; s++) sum += (double)samples[s] * samples[s];
	return (float)sqrt(sum / count);
}

float SdSignal_getDecibels(float amplitude){ return 20.0F * (float)log10(fabs(amplitude) + 1e-7); }

float SdSignal_getLevel(float amplitude, float minDb, float maxDb){
	float level = (maxDb > minDb)? (SdSignal_getDecibels(amplitude) - minDb) / (maxDb - minDb) : 0.0F;
	return (level < 0.0F)? 0.0F : (level > 1.0F)? 1.0F : level;
}

float SdSignal_getLoudness(const float* samples, unsigned count){ return SdSignal_getLevel(SdSignal_getRMS(samples, count), -60.0F, -10.0F); }

void SdSignal_applyWindow(float* samples, unsigned count){
	unsigned n;
	if(samples == NULL || count < 2) return;
	for(n = 0; n < count; n++) samples[n] *= 0.5F - 0.5F * (float)cos(2.0 * SD_PI * n / (count - 1)); // hann window
}

// Track

static unsigned decodeTrack(SdTrack* track){ // next samples in order, 0 at the end of the file
	float chunk[TRACK_CHUNK];
	ma_uint64 framesRead = 0;
	unsigned s;

	ma_decoder_read_pcm_frames(&track->decoder, chunk, TRACK_CHUNK, &framesRead);
	if(framesRead > TRACK_CHUNK) framesRead = TRACK_CHUNK;
	for(s = 0; s < (unsigned)framesRead; s++) track->recent[(size_t)((track->cursor + s) & (SD_RING_BUFFER - 1))] = chunk[s];
	track->cursor += framesRead;
	return (unsigned)framesRead;
}

SdTrack* SdTrack_open(const char* path){
	SdTrack* track;
	ma_decoder_config config;
	ma_uint64 length = 0;
	size_t pathSize;
#ifdef _WIN32
	size_t c;
#endif

	if(path == NULL) return NULL;
	track = (SdTrack*)calloc(1, sizeof(SdTrack));
	if(track == NULL) return NULL;

	pathSize = strlen(path) + 1;
	track->path = (char*)malloc(pathSize);
	if(track->path == NULL){ free(track); return NULL; }
	memcpy(track->path, path, pathSize);
#ifdef _WIN32
	for(c = 0; c < pathSize; c++) if(track->path[c] == '/') track->path[c] = '\\';
#endif

	config = ma_decoder_config_init(ma_format_f32, 1, 0); // mono floats at the native sample rate
	if(ma_decoder_init_file(track->path, &config, &track->decoder) != MA_SUCCESS){
		free(track->path);
		free(track);
		return NULL;
	}
	track->sampleRate = (float)track->decoder.outputSampleRate;
	if(ma_decoder_get_length_in_pcm_frames(&track->decoder, &length) == MA_SUCCESS) track->length = length;
	return track;
}

void SdTrack_close(SdTrack* track){
	if(track == NULL) return;
	SdTrack_stop(track);
	ma_decoder_uninit(&track->decoder);
	free(track->path);
	free(track);
}

float SdTrack_getSampleRate(const SdTrack* track){ return (track != NULL)? track->sampleRate : 0.0F; }

unsigned long long SdTrack_getLength(const SdTrack* track){ return (track != NULL)? track->length : 0; }

double SdTrack_getDuration(const SdTrack* track){ return (track != NULL && track->sampleRate > 0.0F)? (double)track->length / track->sampleRate : 0.0; }

unsigned SdTrack_read(SdTrack* track, unsigned long long start, float* samples, unsigned count){
	unsigned read = 0, s;
	if(track == NULL || samples == NULL) return 0;

	// Files are decoded in order, since seeking inside of an mp3 lands early by the delay of its encoder
	if(start < track->cursor && track->cursor - start > SD_RING_BUFFER){ // older than the recent samples, decoding starts over
		if(ma_decoder_seek_to_pcm_frame(&track->decoder, 0) == MA_SUCCESS) track->cursor = 0;
		else count = 0;
	}
	while(read < count){
		unsigned long long index = start + read;
		if(index < track->cursor) samples[read++] = track->recent[(size_t)(index & (SD_RING_BUFFER - 1))];
		else if(decodeTrack(track) == 0) break; // end of the file
	}
	for(s = read; s < count; s++) samples[s] = 0.0F;
	return read;
}

int SdTrack_play(SdTrack* track, int isLooping){
	if(track == NULL) return 0;
	SdTrack_stop(track);
	if(!acquireEngine()) return 0; // no output device

	if(ma_sound_init_from_file(&engine, track->path, 0, NULL, NULL, &track->sound) != MA_SUCCESS){
		releaseEngine();
		return 0;
	}
	track->isSound = 1;
	ma_sound_set_looping(&track->sound, (isLooping)? MA_TRUE : MA_FALSE);
	ma_sound_start(&track->sound);
	return 1;
}

void SdTrack_stop(SdTrack* track){
	if(track == NULL) return;
	if(track->isSound){
		ma_sound_uninit(&track->sound);
		track->isSound = 0;
		releaseEngine();
	}
	track->isPaused = 0;
}

void SdTrack_setPaused(SdTrack* track, int isPaused){
	if(track == NULL) return;
	if(track->isSound && (isPaused != 0) != (track->isPaused != 0)){
		if(isPaused) ma_sound_stop(&track->sound);
		else ma_sound_start(&track->sound);
	}
	track->isPaused = (isPaused != 0);
}

int SdTrack_isPlaying(const SdTrack* track){
	return track != NULL && track->isSound && !track->isPaused && !ma_sound_at_end(&track->sound);
}

static double getSoundTime(const SdTrack* track){ // the sound and the decoder can run at different sample rates
	ma_uint64 cursor = 0;
	ma_uint32 sampleRate = 0;
	if(ma_sound_get_cursor_in_pcm_frames(&track->sound, &cursor) != MA_SUCCESS) return 0.0;
	if(ma_sound_get_data_format(&track->sound, NULL, NULL, &sampleRate, NULL, 0) != MA_SUCCESS || sampleRate == 0) return 0.0;
	return (double)cursor / (double)sampleRate;
}

double SdTrack_getTime(const SdTrack* track){ return (track != NULL && track->isSound)? getSoundTime(track) : 0.0; }

int SdTrack_listen(SdTrack* track, float* samples, unsigned count){
	unsigned long long cursor; // read the samples currently being heard
	if(!SdTrack_isPlaying(track) || samples == NULL) return 0;

	cursor = (unsigned long long)(getSoundTime(track) * track->sampleRate);
	SdTrack_read(track, (cursor > count)? cursor - count : 0, samples, count);
	return 1;
}
