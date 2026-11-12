#include "audio_driver_wiiu.h"

#include <coreinit/cache.h>
#include <coreinit/memdefaultheap.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <sndcore2/core.h>
#include <sndcore2/device.h>
#include <string.h>

Error AudioDriverWiiU::init() {
	owns_ax = !AXIsInit();
	if (owns_ax) {
		AXInitParams params = {};
		params.renderer = AX_INIT_RENDERER_48KHZ;
		params.pipeline = AX_INIT_PIPELINE_SINGLE;
		AXInitWithParams(&params);
	}
	ERR_FAIL_COND_V_MSG(!AXIsInit(), ERR_CANT_OPEN, "AX (sndcore2) failed to initialise.");

	mix_buffer = memnew_arr(int32_t, RING_CHUNK * 2);
	for (int ch = 0; ch < 2; ch++) {
		rings[ch] = (int16_t *)MEMAllocFromDefaultHeapEx(RING_SAMPLES * sizeof(int16_t), 0x40);
		ERR_FAIL_NULL_V(rings[ch], ERR_OUT_OF_MEMORY);
		memset(rings[ch], 0, RING_SAMPLES * sizeof(int16_t));
		DCFlushRange(rings[ch], RING_SAMPLES * sizeof(int16_t));

		AXVoice *voice = AXAcquireVoice(31, nullptr, nullptr);
		ERR_FAIL_NULL_V_MSG(voice, ERR_CANT_CREATE, "Couldn't acquire an AX voice.");
		voices[ch] = voice;

		AXVoiceBegin(voice);
		AXSetVoiceType(voice, AX_VOICE_TYPE_UNKNOWN);

		AXVoiceOffsets offsets = {};
		offsets.dataType = AX_VOICE_FORMAT_LPCM16;
		offsets.loopingEnabled = AX_VOICE_LOOP_ENABLED;
		offsets.loopOffset = 0;
		offsets.endOffset = RING_SAMPLES - 1;
		offsets.currentOffset = 0;
		offsets.data = rings[ch];
		AXSetVoiceOffsets(voice, &offsets);

		AXSetVoiceSrcType(voice, AX_VOICE_SRC_TYPE_LINEAR);
		AXSetVoiceSrcRatio(voice, (float)MIX_RATE / (float)AXGetInputSamplesPerSec());

		AXVoiceVeData ve = {};
		ve.volume = 0x8000;
		AXSetVoiceVe(voice, &ve);

		// One speaker channel per voice (index 0 = left, 1 = right), on both
		// the TV and the GamePad.
		AXVoiceDeviceMixData mix[6] = {};
		mix[ch].bus[0].volume = 0x8000;
		AXSetVoiceDeviceMix(voice, AX_DEVICE_TYPE_TV, 0, mix);
		AXSetVoiceDeviceMix(voice, AX_DEVICE_TYPE_DRC, 0, mix);
		AXVoiceEnd(voice);
	}
	return OK;
}

// Where the voices are playing in the ring (both run in lockstep).
uint32_t AudioDriverWiiU::play_position() {
	AXVoiceOffsets offsets;
	AXGetVoiceOffsets(voices[0], &offsets);
	return offsets.currentOffset % RING_SAMPLES;
}

void AudioDriverWiiU::thread_func(void *p_udata) {
	AudioDriverWiiU *ad = static_cast<AudioDriverWiiU *>(p_udata);
	while (!ad->exit_thread.is_set()) {
		const uint32_t play = ad->play_position();
		const uint32_t ahead = (ad->write_pos + RING_SAMPLES - play) % RING_SAMPLES;
		if (ahead >= TARGET_AHEAD) {
			OSSleepTicks(OSMillisecondsToTicks(2));
			continue;
		}

		ad->lock();
		ad->audio_server_process(RING_CHUNK, ad->mix_buffer);
		ad->unlock();

		// Godot mixes into 32-bit samples; AX wants 16-bit, one ring per channel.
		const uint32_t start = ad->write_pos;
		for (uint32_t i = 0; i < RING_CHUNK; i++) {
			const uint32_t pos = (start + i) % RING_SAMPLES;
			ad->rings[0][pos] = (int16_t)(ad->mix_buffer[i * 2 + 0] >> 16);
			ad->rings[1][pos] = (int16_t)(ad->mix_buffer[i * 2 + 1] >> 16);
		}
		// The DSP reads from memory, not the CPU cache.
		for (int ch = 0; ch < 2; ch++) {
			if (start + RING_CHUNK <= RING_SAMPLES) {
				DCFlushRange(ad->rings[ch] + start, RING_CHUNK * sizeof(int16_t));
			} else {
				const uint32_t first = RING_SAMPLES - start;
				DCFlushRange(ad->rings[ch] + start, first * sizeof(int16_t));
				DCFlushRange(ad->rings[ch], (RING_CHUNK - first) * sizeof(int16_t));
			}
		}
		ad->write_pos = (start + RING_CHUNK) % RING_SAMPLES;
	}
}

void AudioDriverWiiU::start() {
	if (voices[0] == nullptr || voices[1] == nullptr) {
		return;
	}
	exit_thread.clear();
	// Fill the first TARGET_AHEAD samples before the voices start.
	write_pos = 0;
	thread.start(thread_func, this);
	OSSleepTicks(OSMillisecondsToTicks(30));
	for (int ch = 0; ch < 2; ch++) {
		AXVoiceBegin(voices[ch]);
		AXSetVoiceState(voices[ch], AX_VOICE_STATE_PLAYING);
		AXVoiceEnd(voices[ch]);
	}
}

void AudioDriverWiiU::finish() {
	exit_thread.set();
	if (thread.is_started()) {
		thread.wait_to_finish();
	}
	for (int ch = 0; ch < 2; ch++) {
		if (voices[ch]) {
			AXVoiceBegin(voices[ch]);
			AXSetVoiceState(voices[ch], AX_VOICE_STATE_STOPPED);
			AXVoiceEnd(voices[ch]);
			AXFreeVoice(voices[ch]);
			voices[ch] = nullptr;
		}
		if (rings[ch]) {
			MEMFreeToDefaultHeap(rings[ch]);
			rings[ch] = nullptr;
		}
	}
	if (mix_buffer) {
		memdelete_arr(mix_buffer);
		mix_buffer = nullptr;
	}
	if (owns_ax) {
		AXQuit();
		owns_ax = false;
	}
}
