#pragma once

#include "core/io/logger.h"
#include "drivers/unix/os_unix.h"
#include "audio_driver_wiiu.h"
#include "joypad_wiiu.h"
#include "servers/audio/audio_server.h"

class MainLoop;

// Sends engine output to WHBLog (on-screen console, Cafe OS log and optionally UDP).
class WiiULogger : public Logger {
public:
	virtual void logv(const char *p_format, va_list p_list, bool p_err) override _PRINTF_FORMAT_ATTRIBUTE_2_0;
};

class OS_WiiU : public OS_Unix {
	MainLoop *main_loop = nullptr;
	JoypadWiiU *joypad = nullptr;
	AudioDriverWiiU audio_driver;

protected:
	virtual void initialize() override;
	virtual void set_main_loop(MainLoop *p_main_loop) override;
	virtual void delete_main_loop() override;
	virtual void finalize() override;
	virtual bool _check_internal_feature_support(const String &p_feature) override;

public:
	virtual void initialize_joypads() override;
	virtual MainLoop *get_main_loop() const override;

	virtual String get_name() const override;
	// OS_Unix uses clock_gettime(CLOCK_MONOTONIC_RAW), which wut doesn't support
	// (only CLOCK_REALTIME and CLOCK_MONOTONIC): it fails with EINVAL and the
	// ticks stay at 0, so every frame's delta would be 0.
	virtual uint64_t get_ticks_usec() const override;
	virtual String get_distribution_name() const override;
	virtual String get_version() const override;
	virtual String get_processor_name() const override;
	virtual int get_processor_count() const override;
	virtual String get_unique_id() const override;

	virtual String get_executable_path() const override;
	virtual String get_user_data_dir(const String &p_user_dir) const override;
	virtual String get_data_path() const override;
	virtual String get_config_path() const override;
	virtual String get_cache_path() const override;
	virtual String get_temp_path() const override;

	virtual Error execute(const String &p_path, const List<String> &p_arguments, String *r_pipe = nullptr, int *r_exitcode = nullptr, bool read_stderr = false, Mutex *p_pipe_mutex = nullptr, bool p_open_console = false) override;
	virtual Dictionary execute_with_pipe(const String &p_path, const List<String> &p_arguments, bool p_blocking = true) override;
	virtual Error create_process(const String &p_path, const List<String> &p_arguments, ProcessID *r_child_id = nullptr, bool p_open_console = false) override;
	virtual Error kill(const ProcessID &p_pid) override;
	virtual int get_process_id() const override;
	virtual bool is_process_running(const ProcessID &p_pid) const override;
	virtual int get_process_exit_code(const ProcessID &p_pid) const override;

	virtual Error open_dynamic_library(const String &p_path, void *&p_library_handle, GDExtensionData *p_data = nullptr) override;
	virtual Error shell_open(const String &p_uri) override;
	virtual void alert(const String &p_alert, const String &p_title = "ALERT!") override;

	// Runs the engine main loop until the game quits or the HOME menu closes the app.
	void run();

	OS_WiiU();
};
