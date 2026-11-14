#include "audio_driver_wiiu.h"

#include <coreinit/cache.h>
#include <coreinit/core.h>
#include <coreinit/memdefaultheap.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <proc_ui/procui.h>
#include <sndcore2/core.h>
#include <sndcore2/device.h>
#include <string.h>
#include <whb/log.h>

#include "wiiu_gx2.h"

// Underruns since start (the voices caught up with write_pos), shown by the
// stats overlay (wiiu_stats.cpp).
extern "C" {
volatile uint32_t wiiu_audio_underruns = 0;
}

// Where AXInit runs. AX keeps its own work (the "AXIST" thread) on that core,
// and the system expects it on the main core: AXInit on core 2 (2f960b4,
// against a buzz on long main-thread frames) made 1 in ~8 HOME presses stall
// for exactly 2.09 s in the system's sound shutdown, then the HOME menu and
// its applets stayed sluggish while the app sat half-released (2026-11-13).
// AXQuit from another core is ignored ("AXQuit() called from core 1 (AXInit()
// was on core 2)"). SDL2's Wii U port also keeps AX on core 1.
// 1 = the main thread (init() runs there); 2 = a helper thread on core 2.
#define WIIU_AX_INIT_CORE 1

static int ax_init_entry(int p_argc, const char **p_argv) {
	(void)p_argc;
	(void)p_argv;
	AXInitParams params = {};
	params.renderer = AX_INIT_RENDERER_48KHZ;
	params.pipeline = AX_INIT_PIPELINE_SINGLE;
	AXInitWithParams(&params);
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
		if (WIIU_AX_INIT_CORE == 2 && OSCreateThread(init_thread, ax_init_entry, 0, nullptr, stack + STACK_SIZE, STACK_SIZE, 16, OS_THREAD_ATTRIB_AFFINITY_CPU2)) {
			OSResumeThread(init_thread);
			int result = 0;
			OSJoinThread(init_thread, &result);
		} else {
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
	while (!ad->exit_thread.is_set()) {
		// In the background (HOME menu, or on the way out after SYSLaunchMenu)
		// the voices are stopped (proc_released) and the system owns AX:
		// don't touch AX until the app is back.
		if (!wiiu_gx2_can_draw() || ad->voices_paused) {
			OSSleepTicks(OSMillisecondsToTicks(20));
			continue;
		}
		const uint32_t play = ad->play_position();
		if (ad->resync_pending) {
			ad->resync_pending = false;
			ad->write_pos = (play + RING_CHUNK) % RING_SAMPLES;
		}
		uint32_t ahead = (ad->write_pos + RING_SAMPLES - play) % RING_SAMPLES;
		if (ahead > TARGET_AHEAD + RING_CHUNK) {
			// Never this far ahead normally: the voices passed write_pos (an
			// underrun) and "ahead" wrapped. Without this the thread slept while
			// AX looped stale samples for most of the ring (a buzz). Restart
			// writing just ahead of the play position.
			wiiu_audio_underruns = wiiu_audio_underruns + 1;
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

void AudioDriverWiiU::set_voices_playing(bool p_playing) {
	for (int ch = 0; ch < 2; ch++) {
		if (voices[ch]) {
			AXVoiceBegin(voices[ch]);
			AXSetVoiceState(voices[ch], p_playing ? AX_VOICE_STATE_PLAYING : AX_VOICE_STATE_STOPPED);
			AXVoiceEnd(voices[ch]);
		}
	}
}

// HOME menu: stop our voices before the system takes the foreground. Left
// looping through the switch, the system's sound shutdown ("AXIST thread has
// stopped", then the SNDD driver on core 2) waited ~15 s for them (the HOME
// menu stayed invisible; sometimes the console hung for good, with our last
// block repeating). Runs on the main thread inside ProcUIProcessMessages.
uint32_t AudioDriverWiiU::proc_released(void *p_context) {
	AudioDriverWiiU *ad = static_cast<AudioDriverWiiU *>(p_context);
	WHBLogPrint("[home] audio: muting"); // Debug (HOME freeze), temporary.
	// The voices are left alone (the system stops AX for the switch), as
	// SDL2 does; the mixer stops touching AX and the ring is silenced.
	ad->voices_paused = true;
	// Silence, so coming back doesn't replay the old ring.
	for (int ch = 0; ch < 2; ch++) {
		if (ad->rings[ch]) {
			memset(ad->rings[ch], 0, RING_SAMPLES * sizeof(int16_t));
			DCFlushRange(ad->rings[ch], RING_SAMPLES * sizeof(int16_t));
		}
	}
	WHBLogPrint("[home] audio: stopped");
	return 0;
}

uint32_t AudioDriverWiiU::proc_acquired(void *p_context) {
	AudioDriverWiiU *ad = static_cast<AudioDriverWiiU *>(p_context);
	if (ad->voices_paused) {
		ad->resync_pending = true;
		ad->voices_paused = false;
	}
	return 0;
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
	// Core 0, priority 15 (one above the default 16, as SDL2 does for its
	// audio thread): away from the main thread (core 1), where at equal
	// priority it waited out long frames (crackling), and ahead of Godot's
	// worker threads (priority 16, any core; no time slicing between equal
	// priorities, so a busy worker held the mixer off: short buzzes with the
	// stats overlay open). It was on core 2 until 2026-11-14; HOME freezes
	// hang in OSReleaseForeground waiting for core 2, and it was the only app
	// thread pinned there (SDL2 apps, which don't freeze, pin nothing).
	if (!OSCreateThread(&os_thread, os_thread_entry, 0, (char *)this, os_thread_stack + STACK_SIZE, STACK_SIZE, 15,
				OS_THREAD_ATTRIB_AFFINITY_CPU0)) {
		ERR_PRINT("Couldn't create the audio thread.");
		MEMFreeToDefaultHeap(os_thread_stack);
		os_thread_stack = nullptr;
		return;
	}
	OSSetThreadName(&os_thread, "Cafe Godot audio");
	OSResumeThread(&os_thread);
	thread_started = true;
	OSSleepTicks(OSMillisecondsToTicks(30));
	set_voices_playing(true);
	ProcUIRegisterCallback(PROCUI_CALLBACK_RELEASE, proc_released, this, 100);
	ProcUIRegisterCallback(PROCUI_CALLBACK_ACQUIRE, proc_acquired, this, 100);
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
