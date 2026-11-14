#pragma once

#include "core/os/mutex.h"
#include "core/os/thread.h"
#include "core/templates/safe_refcount.h"
#include "servers/audio/audio_server.h"

#include <coreinit/thread.h>
#include <sndcore2/voice.h>

// Audio output for Cafe Godot through AX (sndcore2), modelled on Love Potion's
// driver/audio (MIT, (c) 2018-present Serena S. Postelnek & Logan
// Hickok-Dickson).
//
// Two AX voices (left, right) loop forever over 16-bit ring buffers at 48 kHz
// and play on both the TV and the GamePad. A mixer thread keeps the rings
// about TARGET_AHEAD samples ahead of the voices' play position by asking
// Godot's AudioServer for RING_CHUNK frames at a time. AX reads samples in
// the CPU's native (big-endian) order.
class AudioDriverWiiU : public AudioDriver {
	static constexpr int MIX_RATE = 48000;
	static constexpr uint32_t RING_SAMPLES = 8192; // Per channel, ~171 ms.
	static constexpr uint32_t RING_CHUNK = 256; // Frames mixed per step.
	static constexpr uint32_t TARGET_AHEAD = 2048; // ~43 ms of latency (1024 crackled in FNF).

	// A Cafe OS thread rather than Godot's Thread: the core can only be chosen
	// at creation (OSSetThreadAffinity fails on a running thread).
	OSThread os_thread;
	uint8_t *os_thread_stack = nullptr;
	bool thread_started = false;
	Mutex mutex;
	SafeFlag exit_thread;
	bool owns_ax = false;

	AXVoice *voices[2] = { nullptr, nullptr };
	int16_t *rings[2] = { nullptr, nullptr };
	uint32_t write_pos = 0;
	int32_t *mix_buffer = nullptr; // RING_CHUNK stereo frames from the AudioServer.

	// Voices are stopped while the app is in the background (ProcUI release
	// callback) and started again on acquire. See audio_driver_wiiu.cpp.
	volatile bool voices_paused = false;
	// Set on acquire: realign write_pos with the voices before mixing again
	// (they restart at the offset where they stopped; not an underrun).
	volatile bool resync_pending = false;
	static uint32_t proc_released(void *p_context);
	static uint32_t proc_acquired(void *p_context);
	void set_voices_playing(bool p_playing);

	static int os_thread_entry(int p_argc, const char **p_argv);
	static void thread_func(void *p_udata);
	uint32_t play_position();

public:
	const char *get_name() const override { return "AX"; }

	Error init() override;
	void start() override;
	int get_mix_rate() const override { return MIX_RATE; }
	SpeakerMode get_speaker_mode() const override { return SPEAKER_MODE_STEREO; }
	float get_latency() override { return (float)TARGET_AHEAD / MIX_RATE; }

	void lock() override { mutex.lock(); }
	void unlock() override { mutex.unlock(); }
	void finish() override;
};
