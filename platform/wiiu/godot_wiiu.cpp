#include "os_wiiu.h"

#include "main/main.h"

#include <coreinit/thread.h>
#include <coreinit/time.h>
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

		// No real display or audio drivers yet: run headless. The game's pack is
		// bundled as fs:/vol/content/godot.pck, next to the executable path that
		// OS_WiiU reports, which is where export templates look for it.
		char arg_headless[] = "--headless";
		char *engine_args[] = { arg_headless };

		wiiu_log_heap_state("before Main::setup");
		Error err = Main::setup(argc > 0 ? argv[0] : (char *)"godot", 1, engine_args);
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
		// The game quit on its own: hand control back to the system so ProcUI can shut us down.
		wait_for_home_menu();
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
