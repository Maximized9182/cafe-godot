#include "os_wiiu.h"

#include "dir_access_wiiu.h"
#include "display_server_wiiu.h"

#include "core/config/project_settings.h"
#include "core/os/main_loop.h"
#include "main/main.h"
#include "servers/display/display_server.h"

#include <coreinit/debug.h>
#include <coreinit/systeminfo.h>
#include <coreinit/time.h>
#include <sys/stat.h>
#include <whb/log.h>
#include <vpad/input.h>
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
	// make_dir_recursive that works on the SD card (see dir_access_wiiu.h).
	DirAccess::make_default<DirAccessWiiU>(DirAccess::ACCESS_RESOURCES);
	DirAccess::make_default<DirAccessWiiU>(DirAccess::ACCESS_USERDATA);
	DirAccess::make_default<DirAccessWiiU>(DirAccess::ACCESS_FILESYSTEM);
}

uint64_t OS_WiiU::get_ticks_usec() const {
	const uint64_t ticks = (uint64_t)(OSGetSystemTime() - wiiu_ticks_start);
	const uint64_t clock = (uint64_t)OSTimerClockSpeed;
	// Split the conversion so ticks * 1000000 can't overflow.
	return (ticks / clock) * 1000000ull + ((ticks % clock) * 1000000ull) / clock;
}

void OS_WiiU::initialize_joypads() {
	joypad = memnew(JoypadWiiU(Input::get_singleton()));
	keyboard_mouse = memnew(KeyboardMouseWiiU(Input::get_singleton()));
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
	if (keyboard_mouse) {
		memdelete(keyboard_mouse);
		keyboard_mouse = nullptr;
	}
	if (joypad) {
		memdelete(joypad);
		joypad = nullptr;
	}
}

bool OS_WiiU::_check_internal_feature_support(const String &p_feature) {
	return p_feature == "wiiu" || p_feature == "game_console" || p_feature == "homebrew";
}

String OS_WiiU::get_name() const {
	return "Cafe OS";
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

// Godot looks for the game's pack next to the executable (godot.pck).
// - .wuhb with the pack inside: fs:/vol/content/godot.pck.
// - .wuhb with the pack on the SD card (cafe-export --pack-on-sd): the bundle
//   only has fs:/vol/content/godot.pckdir, one line naming the folder, e.g.
//   fs:/vol/external01/wiiu/apps/funkin. Keeps the bundle's icon and name
//   while the game's reads bypass Aroma's bundle code (suspected in the
//   console hangs on HOME with .wuhb games, AGENTS.md section 18).
// - Plain .rpx: sd:/wiiu/apps/godot/godot.pck next to sd:/wiiu/apps/godot/godot.rpx.
String OS_WiiU::get_executable_path() const {
	static String path;
	if (path.is_empty()) {
		struct stat st;
		if (::stat("fs:/vol/content/godot.pck", &st) == 0) {
			path = "fs:/vol/content/godot.rpx";
		} else {
			FILE *f = fopen("fs:/vol/content/godot.pckdir", "r");
			if (f) {
				char dir[256] = {};
				if (fgets(dir, sizeof(dir), f)) {
					String d = String::utf8(dir).strip_edges();
					if (!d.is_empty() && ::stat((d + "/godot.pck").utf8().get_data(), &st) == 0) {
						path = d + "/godot.rpx";
					} else {
						WHBLogPrintf("godot.pckdir names %s, but there's no godot.pck there.", d.utf8().get_data());
					}
				}
				fclose(f);
			}
			if (path.is_empty()) {
				path = "fs:/vol/external01/wiiu/apps/godot/godot.rpx";
			}
		}
	}
	return path;
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
// Defined in crash_log.c.
extern "C" bool wiiu_chord_update(uint32_t hold, uint32_t trigger);
// Defined in wiiu_stats.cpp.
extern "C" bool wiiu_stats_chord_update(uint32_t hold, uint32_t trigger);
extern "C" void wiiu_stats_toggle(void);
extern "C" void wiiu_stats_mark(int p_mark);
extern "C" bool wiiu_log_overlay_visible(void);
extern "C" void wiiu_log_overlay_toggle(void);
extern "C" void wiiu_console_draw(void);

// Called from wiiu_proc_is_running (crash_log.c) when the HOME menu takes
// the foreground and when the app gets it back: the same notifications other
// platforms send when the app loses focus / goes to the background.
extern "C" void (*wiiu_proc_focus_callback)(bool p_focused);

static void wiiu_focus_changed(bool p_focused) {
	MainLoop *loop = OS::get_singleton()->get_main_loop();
	DisplayServerWiiU *ds = Object::cast_to<DisplayServerWiiU>(DisplayServer::get_singleton());
	if (p_focused) {
		if (loop) {
			loop->notification(MainLoop::NOTIFICATION_APPLICATION_RESUMED);
			loop->notification(MainLoop::NOTIFICATION_APPLICATION_FOCUS_IN);
		}
		if (ds) {
			ds->send_window_event(DisplayServerEnums::WINDOW_EVENT_FOCUS_IN);
		}
	} else {
		if (ds) {
			ds->send_window_event(DisplayServerEnums::WINDOW_EVENT_FOCUS_OUT);
		}
		if (loop) {
			loop->notification(MainLoop::NOTIFICATION_APPLICATION_FOCUS_OUT);
			loop->notification(MainLoop::NOTIFICATION_APPLICATION_PAUSED);
		}
	}
}

void OS_WiiU::run() {
	if (!main_loop) {
		return;
	}

	main_loop->initialize();
	wiiu_proc_focus_callback = wiiu_focus_changed;

	while (true) {
		if (!wiiu_proc_is_running()) {
			break;
		}
		wiiu_stats_mark(0); // Frame start (stats overlay timing).
		// Read every controller once per frame, before the engine runs the frame.
		// The same GamePad sample drives the log overlay: ZL+L+RIGHT+X+Y+TV
		// toggles it, and while it's shown D-pad / L / R scroll it.
		if (keyboard_mouse) {
			keyboard_mouse->process();
		}
		if (joypad) {
			joypad->process();
			const uint32_t hold = joypad->get_gamepad_hold();
			const uint32_t trigger = joypad->get_gamepad_trigger();
			// ZR+R+LEFT+Y+B+L toggles the stats overlay (wiiu_stats.cpp).
			if (wiiu_stats_chord_update(hold, trigger)) {
				wiiu_stats_toggle();
			}
			if (wiiu_chord_update(hold, trigger)) {
				wiiu_log_overlay_toggle();
			} else if (wiiu_log_overlay_visible() && (trigger & VPAD_BUTTON_B)) {
				wiiu_log_overlay_toggle(); // B closes the log.
			} else if (wiiu_log_overlay_visible()) {
				wiiu_console_handle_buttons(hold, trigger);
			}
		}
		DisplayServer::get_singleton()->process_events();
		if (Main::iteration()) {
			break;
		}
		wiiu_console_draw();
		// Show the crash screen here if another thread crashed (crash_log.c).
		wiiu_crash_check();
	}

	wiiu_proc_focus_callback = nullptr;
	main_loop->finalize();
}

OS_WiiU::OS_WiiU() {
	AudioDriverManager::add_driver(&audio_driver);
	DisplayServerWiiU::register_wiiu_driver();
	Vector<Logger *> loggers;
	loggers.push_back(memnew(WiiULogger));
	_set_logger(memnew(CompositeLogger(loggers)));
}
