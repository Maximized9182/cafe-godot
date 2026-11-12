#include "os_wiiu.h"

#include "main/main.h"

#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <sysapp/launch.h>
#include <whb/log.h>
#include <whb/log_cafe.h>
#include <whb/log_udp.h>
#include <whb/proc.h>

#include <cstdio>
#include <cstdlib>

// Defined in crash_log.c.
extern "C" void wiiu_log_heap_state(const char *tag);
extern "C" void wiiu_log_memory_bounds(void);
extern "C" void wiiu_crash_screen_init(void);
extern "C" void wiiu_crash_note_procui_shutdown(void);
extern "C" bool wiiu_proc_is_running(void);

// Defined in wiiu_console.c.
extern "C" void wiiu_console_init(void);
extern "C" void wiiu_console_free(void);
extern "C" void wiiu_console_update_input(void);
extern "C" void wiiu_console_draw(void);
extern "C" void wiiu_console_resume(void);

// GCC's EABI startup: runs the static constructors (__do_global_ctors_aux over
// the .ctors list). With -meabi, GCC inserts a call to __eabi() (which calls
// __init) at the top of main; wut's crt relies on that and never calls it
// itself. Clang doesn't emit that call, so main has to.
extern "C" void __init(void);

// Keep the log on screen until the user leaves through the HOME menu.
static void wait_for_home_menu() {
	WHBLogPrint("Press HOME to exit. D-pad up/down or L/R scrolls the log.");
	while (wiiu_proc_is_running()) {
		wiiu_console_update_input();
		wiiu_console_draw();
		OSSleepTicks(OSMillisecondsToTicks(16));
	}
}

int main(int argc, char *argv[]) {
	WHBProcInit();
	wiiu_console_init();
	// Black console; it turns red if Godot crashes (see crash_log.c).
	wiiu_crash_screen_init();
	WHBLogCafeInit();
#ifdef WIIU_UDP_LOG
	WHBLogUdpInit();
#endif
	WHBLogPrint("Cafe Godot: starting up");
	wiiu_console_draw();
	// Run C++ static constructors (Godot's globals, e.g. StringName's
	// allocator, are unusable without them). This happens after ProcUI, the
	// console, the crash handlers and logging are up, so a constructor that
	// crashes or aborts shows on the crash screen instead of failing silently.
	// Nothing above this line may use C++ globals.
	__init();
	WHBLogPrint("Static constructors done.");
	wiiu_console_draw();
	wiiu_log_heap_state("start of main");
	wiiu_log_memory_bounds();

	int exit_code = EXIT_SUCCESS;
	{
		OS_WiiU os;

		// No arguments: the GX2 display server is the default, and the game's
		// pack is bundled as fs:/vol/content/godot.pck, next to the executable
		// path that OS_WiiU reports, which is where export templates look for it.
		char *engine_args[] = { nullptr };

		wiiu_log_heap_state("before Main::setup");
		Error err = Main::setup(argc > 0 ? argv[0] : (char *)"godot", 0, engine_args);
		if (err != OK) {
			WHBLogPrintf("Main::setup failed (error %d).", (int)err);
			exit_code = EXIT_FAILURE;
		} else {
			if (Main::start() == EXIT_SUCCESS) {
				os.run();
			} else {
				os.set_exit_code(EXIT_FAILURE);
			}
			Main::cleanup();
			exit_code = os.get_exit_code();
			WHBLogPrintf("Godot exited with code %d.", exit_code);
		}
	}

	if (wiiu_proc_is_running()) {
		if (exit_code == EXIT_SUCCESS) {
			// The game quit on its own without an error: go back to the Wii U
			// Menu. SYSLaunchMenu() asks the system to switch; ProcUI then sends
			// the Exit message, which ends this loop.
			SYSLaunchMenu();
			while (wiiu_proc_is_running()) {
				wiiu_console_draw();
				OSSleepTicks(OSMillisecondsToTicks(16));
			}
		} else {
			// Setup failed or the game exited with an error: keep the log on screen
			// so it can be read, until the user leaves through HOME. GX2 was shut
			// down with the display server, so the console can take the screen back.
			wiiu_console_resume();
			wait_for_home_menu();
		}
	}

#ifdef WIIU_UDP_LOG
	WHBLogUdpDeinit();
#endif
	WHBLogCafeDeinit();
	wiiu_console_free();
	wiiu_crash_note_procui_shutdown();
	WHBProcShutdown();
	// Leave with _Exit instead of returning: returning makes wut's crt call
	// exit(), which runs Godot's C++ static destructors after the engine (and
	// ProcUI and the log handlers) are already shut down. One of them faulting
	// there hung the console with nothing logged. The OS reclaims everything.
	_Exit(exit_code);
}
