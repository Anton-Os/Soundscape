#include "Soundscape.h"

// Sample counter shared between the device thread and the application

#if defined(_MSC_VER)
	#ifndef WIN32_LEAN_AND_MEAN
		#define WIN32_LEAN_AND_MEAN
	#endif
	#include <windows.h>
	typedef volatile LONG64 SampleCounter;
	#define COUNTER_LOAD(counter) ((unsigned long long)InterlockedCompareExchange64((counter), 0, 0))
	#define COUNTER_STORE(counter, value) ((void)InterlockedExchange64((counter), (LONG64)(value)))
#else
	typedef unsigned long long SampleCounter;
	#define COUNTER_LOAD(counter) __atomic_load_n((counter), __ATOMIC_ACQUIRE)
	#define COUNTER_STORE(counter, value) __atomic_store_n((counter), (value), __ATOMIC_RELEASE)
#endif

struct SdStream {
	float ring[SD_RING_BUFFER];
	SampleCounter written;
	float sampleRate;
	int isContext, isDevice;

	ma_context context;
	ma_device device;
	ma_device_info* captureInfos;
	ma_uint32 captureCount;
};

static void onData(ma_device* device, void* output, const void* input, ma_uint32 frameCount){ // runs on the device thread
	SdStream* stream = (SdStream*)device->pUserData;
	const float* samples = (const float*)input;
	unsigned long long written;
	ma_uint32 s;
	if(stream == NULL || samples == NULL) return;

	written = COUNTER_LOAD(&stream->written);
	for(s = 0; s < frameCount; s++) stream->ring[(size_t)((written + s) & (SD_RING_BUFFER - 1))] = samples[s];
	COUNTER_STORE(&stream->written, written + frameCount);
	(void)output;
}

SdStream* SdStream_alloc(void){
	SdStream* stream = (SdStream*)calloc(1, sizeof(SdStream));
	if(stream != NULL) stream->sampleRate = 48000.0F;
	return stream;
}

void SdStream_free(SdStream* stream){
	if(stream == NULL) return;
	SdStream_stop(stream);
	if(stream->isContext) ma_context_uninit(&stream->context);
	free(stream);
}

unsigned SdStream_getDeviceCount(SdStream* stream){
	ma_device_info* playbackInfos = NULL;
	ma_uint32 playbackCount = 0;
	if(stream == NULL) return 0;

	if(!stream->isContext) stream->isContext = ma_context_init(NULL, 0, NULL, &stream->context) == MA_SUCCESS;
	if(!stream->isContext) return 0;
	if(ma_context_get_devices(&stream->context, &playbackInfos, &playbackCount, &stream->captureInfos, &stream->captureCount) != MA_SUCCESS) stream->captureCount = 0;
	return (unsigned)stream->captureCount;
}

const char* SdStream_getDeviceName(SdStream* stream, unsigned deviceIndex){
	if(SdStream_getDeviceCount(stream) == 0) return "default device";
	return stream->captureInfos[deviceIndex % stream->captureCount].name;
}

int SdStream_start(SdStream* stream, SD_StreamSource source, unsigned deviceIndex){
	ma_device_config config;
	unsigned count;
	if(stream == NULL) return 0;

	SdStream_stop(stream);
	if(source == SD_STREAM_Synth) return 0; // nothing to capture
	count = SdStream_getDeviceCount(stream);
	if(!stream->isContext) return 0;

	config = ma_device_config_init((source == SD_STREAM_Loopback)? ma_device_type_loopback : ma_device_type_capture);
	config.capture.format = ma_format_f32;
	config.capture.channels = 1; // miniaudio mixes the input down to mono
	config.sampleRate = 0; // native rate of the device
	config.dataCallback = onData;
	config.pUserData = stream;
	if(source == SD_STREAM_Microphone && count > 0) config.capture.pDeviceID = &stream->captureInfos[deviceIndex % count].id;

	if(ma_device_init(&stream->context, &config, &stream->device) != MA_SUCCESS) return 0;
	stream->isDevice = 1;
	stream->sampleRate = (stream->device.sampleRate > 0)? (float)stream->device.sampleRate : 48000.0F;
	COUNTER_STORE(&stream->written, 0);
	if(ma_device_start(&stream->device) != MA_SUCCESS){ SdStream_stop(stream); return 0; }
	return 1;
}

void SdStream_stop(SdStream* stream){
	if(stream == NULL) return;
	if(stream->isDevice) ma_device_uninit(&stream->device);
	stream->isDevice = 0;
}

int SdStream_isActive(const SdStream* stream){ return stream != NULL && stream->isDevice; }

float SdStream_getSampleRate(const SdStream* stream){ return (stream != NULL)? stream->sampleRate : 0.0F; }

unsigned long long SdStream_getSampleCount(const SdStream* stream){
	return (stream != NULL && stream->isDevice)? COUNTER_LOAD((SampleCounter*)&stream->written) : 0;
}

int SdStream_read(const SdStream* stream, float* samples, unsigned count){
	unsigned long long written = SdStream_getSampleCount(stream);
	unsigned s;
	if(written == 0 || samples == NULL) return 0;

	for(s = 0; s < count; s++){
		unsigned long long back = count - s; // samples before the newest one
		samples[s] = (back <= written && back <= SD_RING_BUFFER)? stream->ring[(size_t)((written - back) & (SD_RING_BUFFER - 1))] : 0.0F;
	}
	return 1;
}
