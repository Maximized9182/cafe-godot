#include "audio_driver_wiiu.h"

#include <coreinit/cache.h>
#include <coreinit/core.h>
#include <coreinit/memdefaultheap.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <sndcore2/core.h>
#include <sndcore2/device.h>
#include <string.h>
#include <whb/log.h>

#include "wiiu_gx2.h"

// AXInit runs on core 2: AX's own CPU-side work (the "AXIST" thread)
// presumably stays on the core that initialised it, and on core 1 it waited
// behind long main-thread frames (red spikes in the stats graph), so AX
// replayed its last block (a short buzz).
static int ax_init_entry(int p_argc, const char **p_argv) {
	(void)p_argc;
	(void)p_argv;
	AXInitParams params = {};
	params.renderer = AX_INIT_RENDERER_48KHZ;
	params.pipeline = AX_INIT_PIPELINE_SINGLE;
	AXInitWithParams(&params);
	WHBLogPrintf("AX initialised on core %u.", (unsigned)OSGetCoreId());
	return 0;
}

Error AudioDriverWiiU::init() {
	owns_ax = !AXIsInit();
	if (owns_ax) {
		constexpr uint32_t STACK_SIZE = 64 * 1024;
		uint8_t *stack = (uint8_t *)MEMAllocFromDefaultHeapEx(STACK_SIZE, 16);
		ERR_FAIL_NULL_V(stack, ERR_OUT_OF_MEMORY);
		OSThread *init_thread = (OSThread *)MEMAllocFromDefaultHeapEx(sizeof(OSThread), 16);
		if (init_thread == nullptr) {
			MEMFreeToDefaultHeap(stack);
			ERR_FAIL_V(ERR_OUT_OF_MEMORY);
		}
		memset(init_thread, 0, sizeof(OSThread));
		if (OSCreateThread(init_thread, ax_init_entry, 0, nullptr, stack + STACK_SIZE, STACK_SIZE, 16, OS_THREAD_ATTRIB_AFFINITY_CPU2)) {
			OSResumeThread(init_thread);
			int result = 0;
			OSJoinThread(init_thread, &result);
		} else {
			WHBLogPrint("Couldn't start the AX init thread; initialising AX here.");
			ax_init_entry(0, nullptr);
		}
		MEMFreeToDefaultHeap(init_thread);
		MEMFreeToDefaultHeap(stack);
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
	// Runs on core 2 at normal priority (see start()): on core 1 at equal
	// priority it waited out the main thread's frames (crackling); above the
	// main thread's priority it starved the system's own audio work there
	// (a short buzz every frame).
	WHBLogPrintf("Audio thread on core %u, priority %d.", (unsigned)OSGetCoreId(), (int)OSGetThreadPriority(OSGetCurrentThread()));
	uint32_t underruns = 0;
	OSTime last_beat = OSGetSystemTime();
	while (!ad->exit_thread.is_set()) {
		// In the background (HOME menu, or on the way out after SYSLaunchMenu)
		// the system stops AX and hands it over: don't touch AX until the app
		// is back. Polling AXGetVoiceOffsets every 2 ms during the switch is
		// the suspect for intermittent console hangs on HOME (2026-11-13).
		if (!wiiu_gx2_can_draw()) {
			// Debug (HOME hang hunt): once a second, note that this thread runs.
			if (OSTicksToMilliseconds(OSGetSystemTime() - last_beat) >= 1000) {
				last_beat = OSGetSystemTime();
				WHBLogPrintf("[bg] audio thread alive (AX untouched); main thread: %s", (const char *)wiiu_main_state);
			}
			OSSleepTicks(OSMillisecondsToTicks(20));
			continue;
		}
		const uint32_t play = ad->play_position();
		uint32_t ahead = (ad->write_pos + RING_SAMPLES - play) % RING_SAMPLES;
		if (ahead > TARGET_AHEAD + RING_CHUNK) {
			// Never this far ahead normally: the voices passed write_pos (an
			// underrun) and "ahead" wrapped. Without this the thread slept while
			// AX looped stale samples for most of the ring (a buzz). Restart
			// writing just ahead of the play position.
			underruns++;
			WHBLogPrintf("[audio] underrun %u (play %u, write %u)", (unsigned)underruns, (unsigned)play, (unsigned)ad->write_pos);
			ad->write_pos = (play + RING_CHUNK) % RING_SAMPLES;
			ahead = RING_CHUNK;
		}
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

int AudioDriverWiiU::os_thread_entry(int p_argc, const char **p_argv) {
	(void)p_argc;
	thread_func((void *)p_argv);
	return 0;
}

void AudioDriverWiiU::start() {
	if (voices[0] == nullptr || voices[1] == nullptr) {
		return;
	}
	exit_thread.clear();
	// Fill the first TARGET_AHEAD samples before the voices start.
	write_pos = 0;
	constexpr uint32_t STACK_SIZE = 256 * 1024;
	os_thread_stack = (uint8_t *)MEMAllocFromDefaultHeapEx(STACK_SIZE, 16);
	ERR_FAIL_NULL_MSG(os_thread_stack, "Couldn't allocate the audio thread's stack.");
	memset(&os_thread, 0, sizeof(os_thread));
	// Core 2, priority 16 (the default): away from the main thread (core 1).
	if (!OSCreateThread(&os_thread, os_thread_entry, 0, (char *)this, os_thread_stack + STACK_SIZE, STACK_SIZE, 16,
				OS_THREAD_ATTRIB_AFFINITY_CPU2)) {
		ERR_PRINT("Couldn't create the audio thread.");
		MEMFreeToDefaultHeap(os_thread_stack);
		os_thread_stack = nullptr;
		return;
	}
	OSSetThreadName(&os_thread, "Cafe Godot audio");
	OSResumeThread(&os_thread);
	thread_started = true;
	OSSleepTicks(OSMillisecondsToTicks(30));
	for (int ch = 0; ch < 2; ch++) {
		AXVoiceBegin(voices[ch]);
		AXSetVoiceState(voices[ch], AX_VOICE_STATE_PLAYING);
		AXVoiceEnd(voices[ch]);
	}
}

void AudioDriverWiiU::finish() {
	exit_thread.set();
	if (thread_started) {
		int result = 0;
		OSJoinThread(&os_thread, &result);
		thread_started = false;
	}
	if (os_thread_stack) {
		MEMFreeToDefaultHeap(os_thread_stack);
		os_thread_stack = nullptr;
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
