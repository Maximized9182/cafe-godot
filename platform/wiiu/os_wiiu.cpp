#include "os_wiiu.h"

#include "display_server_wiiu.h"

#include "core/config/project_settings.h"
#include "core/os/main_loop.h"
#include "main/main.h"
#include "servers/display/display_server.h"

#include <coreinit/debug.h>
#include <coreinit/systeminfo.h>
#include <coreinit/time.h>
#include <whb/log.h>
#include <whb/proc.h>

#include <cstdio>

// Paths: WUHB bundle content is mounted at fs:/vol/content, the SD card at fs:/vol/external01.
static const char *WIIU_SD_ROOT = "fs:/vol/external01";
static const char *WIIU_USER_ROOT = "fs:/vol/external01/wiiu/godot";

void WiiULogger::logv(const char *p_format, va_list p_list, bool p_err) {
	if (!should_log(p_err)) {
		return;
	}
	char buf[1024];
	vsnprintf(buf, sizeof(buf), p_format, p_list);
	// WHBLogWrite doesn't add a newline; Godot's messages already carry one.
	WHBLogWrite(buf);
}

// System time at startup, so get_ticks_usec() counts from engine start like
// the other platforms do.
static OSTime wiiu_ticks_start = 0;

void OS_WiiU::initialize() {
	wiiu_ticks_start = OSGetSystemTime();
	OS_Unix::initialize_core();
}

uint64_t OS_WiiU::get_ticks_usec() const {
	const uint64_t ticks = (uint64_t)(OSGetSystemTime() - wiiu_ticks_start);
	const uint64_t clock = (uint64_t)OSTimerClockSpeed;
	// Split the conversion so ticks * 1000000 can't overflow.
	return (ticks / clock) * 1000000ull + ((ticks % clock) * 1000000ull) / clock;
}

void OS_WiiU::initialize_joypads() {
	joypad = memnew(JoypadWiiU(Input::get_singleton()));
}

void OS_WiiU::set_main_loop(MainLoop *p_main_loop) {
	main_loop = p_main_loop;
}

MainLoop *OS_WiiU::get_main_loop() const {
	return main_loop;
}

void OS_WiiU::delete_main_loop() {
	if (main_loop) {
		memdelete(main_loop);
	}
	main_loop = nullptr;
}

void OS_WiiU::finalize() {
	delete_main_loop();
	if (joypad) {
		memdelete(joypad);
		joypad = nullptr;
	}
}

bool OS_WiiU::_check_internal_feature_support(const String &p_feature) {
	return p_feature == "wiiu" || p_feature == "game_console" || p_feature == "homebrew";
}

String OS_WiiU::get_name() const {
	return "Wii U";
}

String OS_WiiU::get_distribution_name() const {
	return "Cafe OS";
}

String OS_WiiU::get_version() const {
	return "";
}

String OS_WiiU::get_processor_name() const {
	return "IBM Espresso";
}

int OS_WiiU::get_processor_count() const {
	return 3;
}

String OS_WiiU::get_unique_id() const {
	return "";
}

String OS_WiiU::get_executable_path() const {
	return "fs:/vol/content/godot.rpx";
}

String OS_WiiU::get_user_data_dir(const String &p_user_dir) const {
	return String(WIIU_USER_ROOT).path_join(p_user_dir);
}

String OS_WiiU::get_data_path() const {
	return WIIU_USER_ROOT;
}

String OS_WiiU::get_config_path() const {
	return WIIU_USER_ROOT;
}

String OS_WiiU::get_cache_path() const {
	return String(WIIU_USER_ROOT).path_join("cache");
}

String OS_WiiU::get_temp_path() const {
	return String(WIIU_SD_ROOT).path_join("tmp");
}

Error OS_WiiU::execute(const String &p_path, const List<String> &p_arguments, String *r_pipe, int *r_exitcode, bool read_stderr, Mutex *p_pipe_mutex, bool p_open_console) {
	return ERR_UNAVAILABLE;
}

Dictionary OS_WiiU::execute_with_pipe(const String &p_path, const List<String> &p_arguments, bool p_blocking) {
	return Dictionary();
}

Error OS_WiiU::create_process(const String &p_path, const List<String> &p_arguments, ProcessID *r_child_id, bool p_open_console) {
	return ERR_UNAVAILABLE;
}

Error OS_WiiU::kill(const ProcessID &p_pid) {
	return ERR_UNAVAILABLE;
}

int OS_WiiU::get_process_id() const {
	return 1;
}

bool OS_WiiU::is_process_running(const ProcessID &p_pid) const {
	return false;
}

int OS_WiiU::get_process_exit_code(const ProcessID &p_pid) const {
	return -1;
}

Error OS_WiiU::open_dynamic_library(const String &p_path, void *&p_library_handle, GDExtensionData *p_data) {
	return ERR_UNAVAILABLE;
}

Error OS_WiiU::shell_open(const String &p_uri) {
	return ERR_UNAVAILABLE;
}

void OS_WiiU::alert(const String &p_alert, const String &p_title) {
	WHBLogPrintf("%s: %s", p_title.utf8().get_data(), p_alert.utf8().get_data());
}

// Defined in crash_log.c.
extern "C" void wiiu_crash_check(void);
extern "C" bool wiiu_proc_is_running(void);

// Defined in wiiu_console.c.
extern "C" void wiiu_console_handle_buttons(uint32_t hold, uint32_t trigger);
extern "C" void wiiu_console_draw(void);

void OS_WiiU::run() {
	if (!main_loop) {
		return;
	}

	main_loop->initialize();

	while (wiiu_proc_is_running()) {
		// Read every controller once per frame, before the engine runs the frame.
		// The same GamePad sample scrolls the log (D-pad, L/R).
		if (joypad) {
			joypad->process();
			wiiu_console_handle_buttons(joypad->get_gamepad_hold(), joypad->get_gamepad_trigger());
		}
		DisplayServer::get_singleton()->process_events();
		if (Main::iteration()) {
			break;
		}
		wiiu_console_draw();
		// Show the crash screen here if another thread crashed (crash_log.c).
		wiiu_crash_check();
	}

	main_loop->finalize();
}

OS_WiiU::OS_WiiU() {
	DisplayServerWiiU::register_wiiu_driver();
	Vector<Logger *> loggers;
	loggers.push_back(memnew(WiiULogger));
	_set_logger(memnew(CompositeLogger(loggers)));
}
