#pragma once

#include "core/os/mutex.h"
#include "core/os/thread.h"
#include "core/templates/safe_refcount.h"
#include "servers/audio/audio_server.h"

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
	static constexpr uint32_t RING_SAMPLES = 4096; // Per channel, ~85 ms.
	static constexpr uint32_t RING_CHUNK = 256; // Frames mixed per step.
	static constexpr uint32_t TARGET_AHEAD = 1024; // ~21 ms of latency.

	Thread thread;
	Mutex mutex;
	SafeFlag exit_thread;
	bool owns_ax = false;

	AXVoice *voices[2] = { nullptr, nullptr };
	int16_t *rings[2] = { nullptr, nullptr };
	uint32_t write_pos = 0;
	int32_t *mix_buffer = nullptr; // RING_CHUNK stereo frames from the AudioServer.

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
