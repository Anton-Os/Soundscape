#include "Soundscape.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define BANDS_RELEASE 6.0 // decay rate of the smoothed levels
#define BANDS_FLOOR_RISE 0.04F // noise floor rise per second
#define BANDS_FLOOR_GAIN 2.5F // scale of the levels above the noise floor
#define BEAT_AVERAGE 2.0 // rate the bass average follows the bass
#define BEAT_THRESHOLD 1.3F // bass over its average that counts as a beat
#define BEAT_MARGIN 0.08F
#define BEAT_COOLDOWN 0.22F // seconds between beats
#define BEAT_FADE 5.0
#define HANN_SPREAD 1.5F // power a hann window spreads from one frequency into its neighbor bins

struct SdSpectrum {
	unsigned size, binCount;
	kiss_fftr_cfg config;
	float* window; // hann window
	kiss_fft_scalar* input; // windowed samples
	kiss_fft_cpx* output;
	float* bins; // amplitude per bin
	float loudness; // of the samples before the window applies
};

static float clamp01(float x){ return (x < 0.0F)? 0.0F : (x > 1.0F)? 1.0F : x; }

// Band

float SdBand_getFreq(unsigned band, unsigned count, float minFreq, float maxFreq){
	return minFreq * (float)pow(maxFreq / minFreq, (count > 1)? (float)band / (float)(count - 1) : 0.0F); // log spaced bands
}

void SdBand_getRange(unsigned band, unsigned count, float minFreq, float maxFreq, float* lowFreq, float* highFreq){
	float freq = SdBand_getFreq(band, count, minFreq, maxFreq);
	float step = (count > 1)? (float)pow(maxFreq / minFreq, 0.5 / (count - 1)) : (float)sqrt(maxFreq / minFreq); // half a band on the log scale
	if(lowFreq != NULL) *lowFreq = (count > 1)? freq / step : minFreq;
	if(highFreq != NULL) *highFreq = (count > 1)? freq * step : maxFreq;
}

float SdBand_goertzel(const float* samples, unsigned count, float freq, float sampleRate){
	float coeff = 2.0F * (float)cos(2.0 * SD_PI * freq / sampleRate);
	float s1 = 0.0F, s2 = 0.0F, power;
	unsigned n;

	if(samples == NULL || count < 2) return 0.0F;
	for(n = 0; n < count; n++){
		float window = 0.5F - 0.5F * (float)cos(2.0 * SD_PI * n / (count - 1)); // hann window
		float s = samples[n] * window + coeff * s1 - s2;
		s2 = s1; s1 = s;
	}
	power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
	return (power > 0.0F)? (float)sqrt(power) / (count * 0.25F) : 0.0F; // amplitude normalized for the window
}

// Spectrum

SdSpectrum* SdSpectrum_alloc(unsigned size){
	SdSpectrum* spectrum = (SdSpectrum*)calloc(1, sizeof(SdSpectrum));
	unsigned n;
	if(spectrum == NULL) return NULL;

	if(size < 2) size = 2;
	spectrum->size = size + (size % 2); // real transforms need an even size
	spectrum->binCount = spectrum->size / 2 + 1;
	spectrum->config = kiss_fftr_alloc((int)spectrum->size, 0, NULL, NULL);
	spectrum->window = (float*)malloc(spectrum->size * sizeof(float));
	spectrum->input = (kiss_fft_scalar*)calloc(spectrum->size, sizeof(kiss_fft_scalar));
	spectrum->output = (kiss_fft_cpx*)calloc(spectrum->binCount, sizeof(kiss_fft_cpx));
	spectrum->bins = (float*)calloc(spectrum->binCount, sizeof(float));
	if(spectrum->config == NULL || spectrum->window == NULL || spectrum->input == NULL || spectrum->output == NULL || spectrum->bins == NULL){
		SdSpectrum_free(spectrum);
		return NULL;
	}

	for(n = 0; n < spectrum->size; n++) spectrum->window[n] = 1.0F;
	SdSignal_applyWindow(spectrum->window, spectrum->size);
	return spectrum;
}

void SdSpectrum_free(SdSpectrum* spectrum){
	if(spectrum == NULL) return;
	if(spectrum->config != NULL) kiss_fftr_free(spectrum->config);
	free(spectrum->window);
	free(spectrum->input);
	free(spectrum->output);
	free(spectrum->bins);
	free(spectrum);
}

unsigned SdSpectrum_getSize(const SdSpectrum* spectrum){ return (spectrum != NULL)? spectrum->size : 0; }

unsigned SdSpectrum_getBinCount(const SdSpectrum* spectrum){ return (spectrum != NULL)? spectrum->binCount : 0; }

const float* SdSpectrum_getBins(const SdSpectrum* spectrum){ return (spectrum != NULL)? spectrum->bins : NULL; }

float SdSpectrum_getBinFreq(const SdSpectrum* spectrum, unsigned bin, float sampleRate){ return (spectrum != NULL)? bin * sampleRate / spectrum->size : 0.0F; }

const float* SdSpectrum_transform(SdSpectrum* spectrum, const float* samples, unsigned count){
	unsigned n, used;
	if(spectrum == NULL) return NULL;

	used = (samples == NULL)? 0 : (count < spectrum->size)? count : spectrum->size;
	if(used > 0) samples += count - used; // newest samples when there are more than the window holds
	spectrum->loudness = SdSignal_getLoudness(samples, used);
	for(n = 0; n < spectrum->size; n++) spectrum->input[n] = (n < used)? samples[n] * spectrum->window[n] : 0.0F;

	kiss_fftr(spectrum->config, spectrum->input, spectrum->output);
	for(n = 0; n < spectrum->binCount; n++){
		float real = (float)spectrum->output[n].r, imag = (float)spectrum->output[n].i;
		spectrum->bins[n] = (float)sqrt(real * real + imag * imag) / (spectrum->size * 0.25F); // amplitude normalized for the window
	}
	spectrum->bins[0] *= 0.5F; // the first and last bin have no mirrored half
	spectrum->bins[spectrum->binCount - 1] *= 0.5F;
	return spectrum->bins;
}

float SdSpectrum_getAmplitude(const SdSpectrum* spectrum, float lowFreq, float highFreq, float sampleRate){
	float binWidth, position, power = 0.0F;
	unsigned bin, first, last;
	if(spectrum == NULL || sampleRate <= 0.0F || highFreq < lowFreq) return 0.0F;

	binWidth = sampleRate / spectrum->size;
	first = (unsigned)ceil(lowFreq / binWidth);
	last = (unsigned)ceil(highFreq / binWidth); // one past the range
	if(last > spectrum->binCount) last = spectrum->binCount;
	if(first < last){
		for(bin = first; bin < last; bin++) power += spectrum->bins[bin] * spectrum->bins[bin];
		return (float)sqrt(power / HANN_SPREAD);
	}

	position = (lowFreq + highFreq) * 0.5F / binWidth; // ranges narrower than a bin read between the two closest bins
	if(position >= (float)(spectrum->binCount - 1)) return spectrum->bins[spectrum->binCount - 1];
	bin = (unsigned)position;
	return spectrum->bins[bin] + (spectrum->bins[bin + 1] - spectrum->bins[bin]) * (position - (float)bin);
}

float SdSpectrum_getPeak(const SdSpectrum* spectrum, float minFreq, float maxFreq, float sampleRate, float* amplitude){
	float binWidth, left, center, right, curve, offset = 0.0F;
	unsigned bin, first, last, peak;
	if(amplitude != NULL) *amplitude = 0.0F;
	if(spectrum == NULL || sampleRate <= 0.0F || spectrum->binCount < 3) return 0.0F;

	binWidth = sampleRate / spectrum->size;
	first = (unsigned)(minFreq / binWidth);
	last = (unsigned)ceil(maxFreq / binWidth);
	if(first < 1) first = 1;
	if(last > spectrum->binCount - 2) last = spectrum->binCount - 2;
	if(first > last) return 0.0F;

	peak = first;
	for(bin = first; bin <= last; bin++) if(spectrum->bins[bin] > spectrum->bins[peak]) peak = bin;
	if(spectrum->bins[peak] <= 1e-7F) return 0.0F; // silence

	left = (float)log(spectrum->bins[peak - 1] + 1e-12F); // parabola through the peak and its neighbors on a log scale
	center = (float)log(spectrum->bins[peak] + 1e-12F);
	right = (float)log(spectrum->bins[peak + 1] + 1e-12F);
	curve = left - 2.0F * center + right;
	if(curve < 0.0F) offset = 0.5F * (left - right) / curve;
	if(offset < -0.5F || offset > 0.5F) offset = 0.0F;

	if(amplitude != NULL) *amplitude = (float)exp(center - 0.25F * (left - right) * offset);
	return ((float)peak + offset) * binWidth;
}

// Bands

static void updateBands(SdBands* bands, double frameSecs){
	float dt = (float)frameSecs;
	float decay = (float)exp(-frameSecs * BANDS_RELEASE); // fast attack, smooth release
	unsigned bassCount = (bands->count + 3) / 4, midsCount = (bands->count * 3 + 4) / 8, trebleCount; // 4, 6 and 6 out of 16 bands
	unsigned b;

	if(bassCount + midsCount > bands->count) midsCount = bands->count - bassCount;
	trebleCount = bands->count - bassCount - midsCount;

	bands->bass = 0.0F; bands->mids = 0.0F; bands->treble = 0.0F;
	for(b = 0; b < bands->count; b++){
		bands->levels[b] = (bands->raw[b] > bands->levels[b] * decay)? bands->raw[b] : bands->levels[b] * decay;
		if(b < bassCount) bands->bass += bands->levels[b] / bassCount;
		else if(b < bassCount + midsCount) bands->mids += bands->levels[b] / midsCount;
		else bands->treble += bands->levels[b] / trebleCount;
	}

	bands->bassAverage += (bands->bass - bands->bassAverage) * (1.0F - (float)exp(-frameSecs * BEAT_AVERAGE));
	bands->beatCooldown -= dt;
	bands->isBeat = bands->bass > bands->bassAverage * BEAT_THRESHOLD + BEAT_MARGIN && bands->beatCooldown <= 0.0F;
	if(bands->isBeat){ bands->beat = 1.0F; bands->beatCooldown = BEAT_COOLDOWN; }
	bands->beat *= (float)exp(-frameSecs * BEAT_FADE);
}

static float getBandLevel(SdBands* bands, unsigned band, float amplitude, float dt){
	float level = SdSignal_getLevel(amplitude, bands->minDb, bands->maxDb);
	if(!bands->isAdaptive) return level;

	bands->floors[band] = (level < bands->floors[band])? level : bands->floors[band] + dt * BANDS_FLOOR_RISE; // floor drops at once and rises slowly
	return clamp01((level - bands->floors[band]) * BANDS_FLOOR_GAIN); // rooms and microphones differ, so levels sit above the noise floor
}

void SdBands_init(SdBands* bands, unsigned count, float minFreq, float maxFreq){
	if(bands == NULL) return;
	memset(bands, 0, sizeof(SdBands));
	bands->count = (count < 1)? 1 : (count > SD_BANDS_MAX)? SD_BANDS_MAX : count;
	bands->minFreq = (minFreq > 0.0F)? minFreq : SD_MIN_FREQ;
	bands->maxFreq = (maxFreq > bands->minFreq)? maxFreq : bands->minFreq;
	SdBands_setAdaptive(bands, 0);
}

void SdBands_setAdaptive(SdBands* bands, int isAdaptive){
	unsigned b;
	if(bands == NULL) return;
	bands->isAdaptive = (isAdaptive != 0);
	bands->minDb = (isAdaptive)? -70.0F : -60.0F;
	bands->maxDb = (isAdaptive)? -10.0F : 0.0F;
	for(b = 0; b < SD_BANDS_MAX; b++) bands->floors[b] = 1.0F; // floors drop to the input on the next window
}

void SdBands_process(SdBands* bands, const float* samples, unsigned count, float sampleRate, double frameSecs){
	unsigned b;
	if(bands == NULL || samples == NULL || sampleRate <= 0.0F) return;

	for(b = 0; b < bands->count; b++){
		float freq = SdBand_getFreq(b, bands->count, bands->minFreq, bands->maxFreq);
		bands->raw[b] = getBandLevel(bands, b, SdBand_goertzel(samples, count, freq, sampleRate), (float)frameSecs);
	}
	bands->loudness = SdSignal_getLoudness(samples, count);
	updateBands(bands, frameSecs);
}

void SdBands_processSpectrum(SdBands* bands, const SdSpectrum* spectrum, float sampleRate, double frameSecs){
	unsigned b;
	if(bands == NULL || spectrum == NULL || sampleRate <= 0.0F) return;

	for(b = 0; b < bands->count; b++){
		float lowFreq, highFreq;
		SdBand_getRange(b, bands->count, bands->minFreq, bands->maxFreq, &lowFreq, &highFreq);
		bands->raw[b] = getBandLevel(bands, b, SdSpectrum_getAmplitude(spectrum, lowFreq, highFreq, sampleRate), (float)frameSecs);
	}
	bands->loudness = spectrum->loudness;
	updateBands(bands, frameSecs);
}

void SdBands_synthesize(SdBands* bands, double time, double frameSecs){
	float t = (float)time;
	float beat = (float)pow(fabs(sin(t * SD_PI * 2.0 * 0.9)), 10.0); // 108 bpm pulse
	unsigned b;
	if(bands == NULL) return;

	bands->loudness = 0.0F;
	for(b = 0; b < bands->count; b++){
		float f = (float)b / bands->count;
		float melody = 0.5F + 0.5F * (float)sin(t * (0.9F + f * 2.5F) + b * 1.1F);
		float shimmer = 0.5F + 0.5F * (float)sin(t * 6.0F + b * 2.7F);
		bands->raw[b] = clamp01(beat * (1.0F - f) * 0.95F + melody * 0.4F * (0.4F + f) + shimmer * 0.2F * f);
		bands->loudness += bands->raw[b] / bands->count;
	}
	updateBands(bands, frameSecs);
}
