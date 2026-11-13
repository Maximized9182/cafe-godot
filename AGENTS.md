# Cafe Godot: Godot 4.7.2 for the Nintendo Wii U (handoff notes)

**Project name: Cafe Godot** (chosen by the user to fit the Wii U's internal cafe/coffee naming:
Cafe OS, Espresso, Latte). Use it for the port, its repo and user-facing text.

This file is the full state of a work-in-progress port of Godot 4.7.2 to the Wii U
(Cafe OS homebrew, run through Aroma). It was written at the end of the first working
session so another agent or person can continue without that conversation.

Status as of 2026-11-12: **the headless template boots on the console but crashes in
`Main::setup` because `malloc` runs out of memory almost immediately** (section 9). No
renderer, audio or input drivers exist.

---

## 1. Background and decisions

- **Starting point.** The existing community port, https://github.com/Homebrodot/platform-wiiu
  (12 commits, last touched April 2024, targets Godot 4.2), was reviewed and judged to be an
  early sketch: it doesn't compile (syntax errors throughout), has no working renderer, file
  access, audio or threads, and its export plugin returns `ERR_CANT_CREATE`. Nothing from it
  was reused except general ideas. The user does not own that repo.
- **Godot version: 4.7.2-stable.** Compared 4.2.2, 4.3, 4.4.1, 4.5.1, 4.6 and 4.7.2:
  - 4.3 split rendering into `RenderingDevice` + a per-API `RenderingDeviceDriver`
    (174 pure-virtual methods in 4.7.2; the 4.7 additions are ray tracing and HDR, which can be stubbed).
  - 4.4 added `detect.get_tools(env)`, letting a platform pick its own SCons tools.
  - 4.5 added the shader baker / `rendering_shader_container` (per-driver compiled shader format).
  - 4.5 **removed `ppc32`** from the allowed architectures. It only affects the arch enum
    and x86 flags, so it's re-added with a one-line patch (see section 4).
  - C++17 and `std::thread` are used in every version; nothing else differs for this port.
- **Compiler: Clang, not devkitPPC's GCC.** Godot has ~130 `thread_local` variables even in a
  minimal build. devkitPPC's GCC compiles them as native TLS (`.tbss`/`.tdata`, r2-relative
  `@tprel`), but wut's linker script has no TLS sections and `elf2rpl` doesn't support TLS,
  so the link fails. Clang's `-femulated-tls` turns them into calls to `__emutls_get_address`,
  which devkitPPC's `libgcc.a` already contains (it sits on top of wut's gthreads/pthreads).
  Linking still uses `powerpc-eabi-g++` + `wut.specs`, so newlib, libstdc++ and libgcc are
  the stock GCC-built ones. Alternatives considered and not taken: rebuilding devkitPPC GCC
  with `--disable-tls` (exact ABI, but needs a custom toolchain for everyone), adding native
  TLS to wut + `elf2rpl` (the proper fix, but a toolchain project of its own), rewriting
  `thread_local` in Godot (126+ sites, a permanent fork).
- **Headless first.** Milestone 1 is an `.rpx` that boots, loads a `.pck` and runs GDScript
  with Godot's dummy renderer and dummy audio, printing to the screen. Graphics come later.

## 2. Environment the work was done in

- Arch Linux VM (KVM, QEMU user-mode NAT at 10.0.2.15). Repo at `/home/user/wiiu/godot`,
  branch `wiiu-port`, shallow clone of `4.7.2-stable` (commit `ed1daf0`). **Nothing is
  committed**: the changes are uncommitted working-tree edits plus the untracked
  `platform/wiiu/` folder. A patch of everything may sit next to this file's repo as
  `/home/user/wiiu/wiiu-port.patch`.
- Tools: devkitPro at `/opt/devkitpro` (devkitPPC GCC 16.1.0, wut, wut-tools: `elf2rpl`,
  `wuhbtool`, `readrpl`, `udplogserver`), `clang` 22.1.8, SCons 4.11.1 (was in a temporary
  venv; reinstall with `pip install scons`).
- Godot 4.7.2 official Linux editor was used on the host side to export `.pck` files
  (`Godot_v4.7.2-stable_linux.x86_64`, from godotengine/godot-builds releases).
- No Cemu in the VM, no direct access to the console. The user has a Wii U with Aroma and
  the Claude desktop app.

## 3. Build and package

```bash
export DEVKITPRO=/opt/devkitpro DEVKITPPC=/opt/devkitpro/devkitPPC
scons platform=wiiu profile=platform/wiiu/profiles/headless_minimal.py -j8
```

Outputs in `bin/`:
- `godot.wiiu.template_debug.ppc32.elf` (~54 MB, linked)
- `godot.wiiu.template_debug.ppc32.stripped.elf` (the same minus `.note.GNU-stack`)
- `godot.wiiu.template_debug.ppc32.rpx` (~16.5 MB; `.text` is ~25 MB in memory)

`target=template_debug` is the default; `template_release` should also work (untested).
Editor builds are rejected by `detect.py`.

Export a game's pack with the desktop editor, then bundle:

```bash
Godot_v4.7.2-stable_linux.x86_64 --headless --path <project> --export-pack "<preset>" game.pck
platform/wiiu/tools/make_wuhb.sh --rpx bin/godot.wiiu.template_debug.ppc32.rpx \
    --pck game.pck --out MyGame.wuhb --name "My Game" [--short-name ..] [--author ..] \
    [--icon icon.png] [--tv-image tv.png] [--drc-image drc.png]
```

Image sizes: icon 128×128, TV splash 1280×720, GamePad (DRC) splash 854×480. The script
puts the pack at `/content/godot.pck` inside the bundle; the engine loads
`fs:/vol/content/godot.pck`.

Already built: `platform/wiiu/build/godot-hello.wuhb` (the test project below).

To run: copy the `.wuhb` to `sd:/wiiu/apps/` and launch it from the Wii U menu under Aroma.
Logs appear on screen (WHBLogConsole), in the Cafe OS log (WHBLogCafe) and over UDP
(WHBLogUdp, read with `udplogserver` on a machine on the same LAN; a NAT'd VM won't see it).

## 4. Changes to Godot core (keep these minimal)

| File | Change | Why |
|---|---|---|
| `platform_methods.py` | Add `"ppc32"` to `architectures` | Removed upstream in 4.5 |
| `core/templates/safe_refcount.h`, `core/templates/safe_list.h` | Wrap `static_assert(std::atomic<T>::is_always_lock_free)` in `#ifndef WIIU_ENABLED` | Espresso has no 64-bit atomics; `std::atomic<uint64_t>` uses locks (see `atomic64.c`). 535 of the first build's 543 errors were this assert |
| `core/io/file_access.h/.cpp` | New `_needs_byteswap()`: swap only when the file's byte order (`big_endian` flag) differs from the CPU's (`__BYTE_ORDER__`). The six `if (big_endian)` checks in `get_16/32/64` and `store_16/32/64` use it | **Big-endian bug**: these read and wrote raw host-order bytes, so every number read from a little-endian `.pck` on the big-endian Wii U came out byte-swapped. Desktop behaviour is unchanged |

`core/io/marshalls.h` (`encode_*/decode_*`) already uses byte shifts and is endian-safe.
Other host-endian assumptions (e.g. raw `get_buffer` into typed arrays in
`resource_format_binary.cpp`, image data) have **not** been audited yet.

## 5. The platform folder: `platform/wiiu/`

- `detect.py`: platform hooks. `get_tools` returns cc/c++/ar/link/textfile/zip.
  `get_flags` defaults: `arch=ppc32`, `target=template_debug`, all GPU backends off
  (vulkan/opengl3/d3d12/metal), `builtin_pcre2_with_jit=False`, raycast off, `optimize=size`.
  Option `wiiu_udp_log` (default on). `configure()`:
  - Compiles with `clang`/`clang++` using `--target=powerpc-unknown-linux-gnu -mcpu=750
    -mhard-float -mlong-double-64 -femulated-tls -fno-pic`, plus `-U` for
    `__linux__ __linux linux __gnu_linux__ __unix__ __unix unix`. Clang's own
    `powerpc-eabi` target was rejected because it makes `size_t` an `unsigned long`
    (GCC uses `unsigned int`), which breaks C++ name mangling against libstdc++.
    devkitPPC's `wchar_t` is `long` vs Clang's `int`; same size, and C++ mangles `wchar_t`
    as a builtin, so it's left alone.
  - Headers: `-nostdinc++ -nostdlibinc`, then devkitPPC libstdc++ (`powerpc-eabi/include/c++/<ver>`
    and its `powerpc-eabi` subdir), newlib (`powerpc-eabi/include`), wut (`wut/include`),
    and Clang's resource `include` last.
  - Links with `powerpc-eabi-g++ -mcpu=750 -meabi -mhard-float -specs=$WUT/share/wut.specs`,
    `-L$WUT/lib`, libs `wut m`. AR/RANLIB/AS are the `powerpc-eabi-` binutils. LTO forced off.
  - Defines: `ESPRESSO __WIIU__ __WUT__ WIIU_ENABLED UNIX_ENABLED UNIX_SOCKET_UNAVAILABLE
    PTHREAD_NO_RENAME` (+ `WIIU_UDP_LOG`).
  - `CPPPATH` gets `#platform/wiiu` and `#platform/wiiu/compat`.
- `SCsub`: builds `godot_wiiu.cpp os_wiiu.cpp atomic64.c posix_stubs.c crash_log.c tls_keys.c` with
  `env.add_program("#bin/godot", ...)` (`PROGSUFFIX=.elf`). **Removes `-s` from LINKFLAGS**:
  wut.specs links with `--emit-relocs` (needed by `elf2rpl`) and ld fails with
  "final link failed: invalid operation" when combined with `-s`. Then
  `powerpc-eabi-objcopy --remove-section=.note.GNU-stack` (Clang adds it; `elf2rpl` fails
  with "Failed to calculate offset for section .note.GNU-stack") and `elf2rpl` → `.rpx`.
- `platform_config.h`: just `#include <alloca.h>`.
- `godot_wiiu.cpp`: `main()` calls `WHBProcInit`, `WHBLogConsoleInit`, `WHBLogCafeInit`,
  optionally `WHBLogUdpInit`, then runs Godot with fixed args
  `--headless --main-pack fs:/vol/content/godot.pck` (`Main::setup` → `Main::start` →
  `OS_WiiU::run` → `Main::cleanup`). On failure or normal exit it prints "Press HOME to exit."
  and keeps drawing the console until `WHBProcIsRunning()` is false, then tears down.
- `os_wiiu.h/.cpp`: `OS_WiiU : OS_Unix`. `initialize()` calls `OS_Unix::initialize_core()`
  (FileAccessUnix/DirAccessUnix over newlib + wut's `fs:` devoptab). Main loop: while
  `WHBProcIsRunning()`: `DisplayServer::process_events()`, `Main::iteration()`,
  `WHBLogConsoleDraw()`. Paths: user data `fs:/vol/external01/wiiu/godot/<project>`, temp
  `fs:/vol/external01/tmp`, executable `fs:/vol/content/godot.rpx`. Feature tags `wiiu`,
  `game_console`, `homebrew`. `get_name()` = "Cafe OS", distribution "Cafe OS", 3 processors.
  Process/dynamic-library/shell functions return `ERR_UNAVAILABLE`. `WiiULogger` sends all
  engine output to `WHBLogWrite`. `initialize_joypads()` is empty.
- `atomic64.c`: implements `__atomic_{load,store,exchange,compare_exchange}_8` and
  `__atomic_{fetch_OP,OP_fetch}_8` for add/sub/and/or/xor/nand (both GCC and Clang lower
  `std::atomic<uint64_t>` to these libcalls; there's no libatomic in devkitPPC). Defined
  under `__asm__` names to avoid builtin-redeclaration errors. Uses 64 striped
  `OSSpinLock`s with `OSUninterruptibleSpinLock_Acquire/Release` so a lock holder can't be
  preempted by a higher-priority thread spinning on the same core. Zero-filled `OSSpinLock`
  equals `OSInitSpinLock`.
- `crash_log.c`: `abort()` is wrapped at link time (`-Wl,--wrap=abort` in `SCsub`), so every
  caller (engine, libstdc++, newlib) reaches `__wrap_abort` first. It logs the thread, the
  runtime address of `main` (to compute the load offset) and the stack back chain. On the
  main thread it keeps the log on screen until HOME, then `WHBProcShutdown` + `_Exit(1)`, so
  the console returns to the menu. On other threads it waits 5 s, then calls wut's real
  `abort()` (`OSFatal("Abort called.")`). Map addresses with
  `powerpc-eabi-addr2line -f -C -e bin/godot.wiiu.template_debug.ppc32.elf`.
- `tls_keys.c`: the `__syscall_tls_create/set/get/delete` hooks behind newlib's
  `pthread_key_*`. wut 1.9.1 doesn't define them, so `pthread_key_create` returned `ENOSYS`
  and libgcc's emutls called `abort()` on the first `thread_local` access
  (`Thread::make_main_thread()` in `Main::setup`); this was the first on-console crash.
  Keys map 1:1 onto Cafe OS thread-specific slots 0-13 (14/15 are wut's). Destructors are
  not run at thread exit, so each exiting thread leaks its emutls block.
- `posix_stubs.c`: `execvp popen pclose waitpid sigaction pipe setsid mkfifo`, all failing
  with `ENOSYS`/`ECHILD`. newlib declares them but nothing implements them.
- `compat/`: stub headers newlib lacks, found first via CPPPATH: `dlfcn.h` (dlopen always
  fails), `syslog.h` (no-ops), `sys/xattr.h` (ENOTSUP), `sys/resource.h` (`#include_next`
  plus a `struct rlimit` / `getrlimit` stub returning -1).
- `profiles/headless_minimal.py`: GDScript only; disables 3D, XR, navigation 2D/3D,
  physics 3D, advanced GUI, minizip, brotli, deprecated.
- `profiles/lvengine.py`: for the user's game (section 7): GDScript, `godot_physics_2d`,
  `text_server_fb` + `freetype`, `ogg` + `vorbis`; keeps advanced GUI (RichTextLabel needs
  it); disables 3D, XR, navigation, physics 3D. **Not built yet.**
- `tools/make_wuhb.sh`: packaging script (section 3).
- `test_project/`: hello-world project (`main.gd` prints OS name, engine version, the `wiiu`
  feature flag, `PackedByteArray([1,2,3,4]).decode_u32(0)` which must be `67305985`, and
  frames per second). Export preset "Pack" writes `../build/game.pck`.
- `.gitignore`: `build/`, `test_project/.godot/`.

## 6. Known risks and open questions

1. **Never run.** First test: boot `godot-hello.wuhb` on the console. Expected on-screen
   lines: "Godot for Wii U: starting up", "Hello from GDScript on Wii U (Cafe OS)",
   `decode_u32 ... 67305985`, then "frames in the last second: N" every second.
2. **Code size.** `.text` is ~25 MB. Unknown whether Cafe OS / Aroma loads that; if it
   refuses to start, try `template_release`, more `-Os`, fewer modules.
3. **Clang/GCC ABI mix.** Clang's linux target assumes a 16-byte-aligned stack; GCC's
   EABI-built newlib/libstdc++/wut use 8 bytes. Probably harmless (no AltiVec on the 750),
   but only visible at runtime. Clang reserves r2/r13 per SVR4, so it shouldn't clobber
   Cafe's small-data registers.
4. **`fs:/vol/content` mount.** Assumes wut's devoptab exposes WUHB content at
   `fs:/vol/content`. Unverified.
5. **Big-endian beyond FileAccess.** Binary resources, images and audio buffers may still
   assume little-endian in places.
6. **Main loop pacing.** Headless mode has no vsync; the loop may spin. Fine for now.
7. **Rendering approach is undecided.** Godot's `RenderingDevice` renderers (even the 2D
   canvas renderer) need SSBOs: `canvas_uniforms_inc.glsl` uses `std430 buffer` blocks
   (`Transforms`, `LightData`, `GlobalShaderUniformData`). wut's GX2 headers expose only
   uniform blocks: no storage buffers, no compute. The GPU (Latte) is roughly GL 3.3 /
   DX10.1-class, closer to what the Compatibility (GLES3) renderer needs. Options: emulate
   SSBOs inside a GX2 `RenderingDeviceDriver`, or put a GX2 backend under the Compatibility
   renderer (`drivers/gles3` talks to GL directly, so that means a GL-subset shim or a fork
   of that renderer). Shaders: Godot produces SPIR-V/GLSL; GX2 needs its own binaries, either
   compiled at runtime by the CafeGLSL compiler (`glslcompiler.rpl` on the console) or at
   export time via the 4.5+ shader container.

## 7. Test game: LVENGINE

At `/home/user/wiiu/LVENGINE` (the user's Undertale-style fan engine, 106 MB, own git repo).

- Godot 4.7, pure 2D, 640×480, `stretch/mode="viewport"`, integer scaling, 60 FPS cap.
  `rendering_method="mobile"` in project settings but feature tag says "GL Compatibility".
- Nodes used: Label, AudioStreamPlayer, ColorRect, TextureRect, RichTextLabel,
  Sprite2D/AnimatedSprite2D, TileMapLayer, Camera2D, CharacterBody2D, StaticBody2D, Area2D,
  RayCast2D, ProgressBar, VBoxContainer, Themes. No shaders, no 3D.
- Assets: PNG (12 MB), WAV + Ogg sounds, 3 TTF fonts. `videos/roring_nite.mp4` (7.5 MB)
  and two GIFs are unused; exclude them from the Wii U export.
- **Input is keyboard-only** (actions up/down/left/right/confirm/cancel/menu/fullscreen/
  quit/DEBUG). Needs joypad bindings once a Wii U input driver exists.
- **Music is loaded from the player's Steam Undertale install** (`scripts/playervars.gd:50-51`,
  Windows and Linux paths); on Wii U it'll be silent except for its own SFX. Could add an
  SD-card path.
- Fits the GamePad's 854×480 at 1×; a 720p TV at 1×, a 1080p TV at 2×.

## 8. Next steps, in order

1. Boot `godot-hello.wuhb` on the console; fix whatever breaks (watch for code-size limits,
   the `fs:/vol/content` path, byte-order issues, startup crashes; capture with
   `udplogserver`).
2. Build with `profiles/lvengine.py`, export LVENGINE's pack, bundle it, and boot it headless
   (exercises resource loading, autoloads, scene changes, timers/tweens).
3. Input: a joypad driver over VPAD (GamePad) and KPAD (Wiimote/Pro Controller), reporting
   Godot joypad events; HOME handled by ProcUI. Then add joypad bindings to LVENGINE.
4. Audio: an `AudioDriver` on `sndcore2` (AX).
5. A real `DisplayServer` for TV + GamePad, then the GX2 renderer (decide the approach in
   risk 7 first). Start 2D-only.
6. Editor-side export plugin (`platform/wiiu/export/`) so the editor can export `.wuhb`
   directly with icon/name/banners (would need a custom editor build including the platform).
7. Consider native TLS support in wut/`elf2rpl` upstream, which would let the port drop Clang.

## 9. Session 2 (2026-11-11/12): first console runs

Runs, in order:
1. `OSFatal("Abort called.")`: `emutls_init` aborted because `pthread_key_create` returned
   `ENOSYS` (wut has no `__syscall_tls_*` hooks). Fixed by `tls_keys.c`.
2. DSI at `CowData<_CoreConstant>::push_back` from `register_global_constants`: `malloc`
   returned NULL for 88 bytes, and Godot dereferenced the result.
3. Heap logging at the start of `main`: **newlib malloc arena 38956 bytes (5304 free), MEM2
   base heap 0 free.** wut's `__init_wut_sbrk_heap` takes all of MEM2's free space as the
   sbrk heap, so 0 free in MEM2 is expected. But the arena tops out at ~40 KB, so **MEM2 had
   only ~40 KB free when wut initialised, before any Godot code ran.**

Prepared but not run (built in the VM; the upload to the SD card may not have completed):
- `godot-hello.wuhb` with `wiiu_log_memory_bounds()` at the start of `main`: logs
  `OSGetMemBound(OS_MEM1/OS_MEM2)`, `sbrk(0)` and a `sbrk(1 MB)` probe.
- `/home/user/wiiu/memprobe/memprobe.wuhb`: a minimal wut app (built with stock devkitPPC
  GCC) printing the same numbers plus a 64 MB malloc test, as a baseline.

Next step: run memprobe, then godot-hello, and compare. If only Godot gets a tiny MEM2, the
image is eating the app's memory budget (RPL FILEINFO: textSize 0x17DC100, dataSize
0x57D000, loadSize 0x8B1460, tempSize 0xE6E920; memprobe: 0x10760 / 0x7000 / 0x4380 /
0x8028), so shrink the build. If memprobe is tiny too, it's a launch/environment issue.

Logging findings:
- `WHBLogPrint` and `WHBLogPrintf` allocate a 2048-byte buffer from the default heap (routed
  into newlib malloc) and print **nothing** when memory is gone. `WHBLogWrite` doesn't
  allocate. `crash_log.c`'s `log_static` formats into a static buffer and uses `WHBLogWrite`.
- `crash_log.c` also wraps `malloc/calloc/realloc` (`--wrap` in `SCsub`): logs failures and
  every allocation of 1 MB or more, plus `wiiu_log_heap_state(tag)`.
- Load offset: the RPX text runs 0x0a000000 above its link address (`main` linked at
  0x02004e84, runs at 0x0c004e84). Subtract it before `powerpc-eabi-addr2line`.

Known next landmines:
- **Main thread stack is 64 KB** (`stackSize 0x10000`, hardcoded by `elf2rpl`, no option).
  `register_global_constants` alone has a ~25 KB frame. Expect a stack overflow once the heap
  works. Fix: patch FILEINFO `stackSize` in the `.rpx` (and its CRC), or run the engine on a
  big-stack thread (ProcUI must still be pumped from the main thread).
- **Stack protector reads garbage**: Clang's ppc32-linux canary is `lwz rX,-28680(r2)`
  (TCB-0x7008), but on Cafe r2 is the SDA2 base. Add `-fno-stack-protector` (or a global guard
  if Clang supports it for ppc32) at the next full rebuild.

Console tooling (user's Wii U at 192.168.1.34, PC 192.168.1.22):
- FTP port 21, empty credentials; apps go in `/fs/vol/external01/wiiu/apps/`. FTP is down
  while the console sits on a crash screen.
- MochaPayload TCP shell: the PC broadcasts UDP beacon `WIIU_TCP_SYSLOG_BEACON_V1` on 4445, and
  the console connects back to TCP 7965. It streams the system log, including OSContext crash
  dumps with symbols. Commands: `iosu reboot`, `iosu shutdown`, `iosu reload`, `iosu help`.
  `platform/wiiu/tools/mocha_logger.py LOG FIFO` logs it to a file and sends lines written to
  the FIFO as commands.
- WHBLogUdp output: `udplogserver` (devkitPro tools) on the PC.
- SCons now lives in `~/scons-venv` on the VM: `~/scons-venv/bin/scons ...`.

## 10. Session 3 (2026-11-12): heap corruption root cause, crash screen, console

**Root cause of the boot-time malloc failure: static constructors ran in reverse order.**
wut.ld merges `.init_array` into the same list as GCC's `.ctors` (inside `.rodata`), and GCC's
EABI startup (`__init` -> `__do_global_ctors_aux`) walks that list backwards. That's right for
`.ctors`, wrong for Clang's default `.init_array`. A static `StringName` was built before
`StringName::allocator`'s constructor ran. Its `alloc()` ran with `page_size` 0, and the
allocator's later `configure(4096)` failed its `page_pool != nullptr` check. So every page was
0 bytes, and `StringName::_Data` objects overran the heap. In newlib's mallocr that shrank the
top chunk's size word, malloc took the "foreign sbrk" path with a tiny old top, and returned
NULL without setting errno.

Fix: `-fno-use-init-array` in `detect.py`, so Clang emits `.ctors`. `-fno-stack-protector` was
added at the same time: Clang's ppc32-linux canary is read from r2, which is the SDA2 base on
Cafe. Both need a full rebuild.

How it was found (the tooling is worth keeping):
- memprobe showed that a ~39 KB malloc arena is normal: malloc grows it with sbrk on demand.
- A `_sbrk_r` trace (`--wrap=_sbrk_r`) showed sbrk succeeding while malloc failed.
- `heap_check()` in `crash_log.c` runs on entry to and exit from every
  malloc/calloc/realloc/free and checks that top + size == sbrk(0). On the first mismatch it
  dumps the 64 bytes below top, walks the chunk chain, and names the block below top with a
  6-frame caller trace. It showed a 16-char String ("fs:/vol/content") followed by overrun
  bytes, and `PagedAllocator<StringName::_Data>` allocating 0-byte pages (16 bytes = Godot's
  prepad only).
- Heap addresses are the same from run to run for the same build.

Crash screen (`crash_log.c`):
- The console background is black, and turns red on `abort()` or on a DSI/ISI/PROGRAM
  exception (`OSSetExceptionCallbackEx`, GLOBAL_ALL_CORES).
- The exception callback saves the context and resumes the thread at `crash_exception_entry`
  on a private 64 KB stack. That logs the registers, SRR0/LR/DAR/DSISR and a backtrace with
  `OSGetSymbolName` names, then waits for HOME and calls `_Exit`.
- A crash on another thread parks that thread and hands over to the main thread through
  `wiiu_crash_check()`, which the main loop calls every frame.
- A second crash while the screen is up returns FALSE, so Cafe OS handles it.

On-screen console (`wiiu_console.c`, replaces WHBLogConsole):
- 1024-line history, wrapped at 68 columns; TV shows 27 rows, GamePad 17. D-pad up/down
  scrolls (hold to repeat), L/R pages. Registered as a WHB log handler.
- OSScreen buffers live in the MEM1 frame heap, allocated and freed in the ProcUI
  acquire/release callbacks. Rows are copied under the lock and rendered outside it.
- **Only call `OSScreenShutdown` while the app has the foreground.** After ProcUI releases it
  (exit through HOME), the display registers (e.g. 0xFC206084) are unmapped and the call
  faults, which hangs the console on the loading screen.

Wording used with the user: "Godot crashed" means the app crashed but HOME works; "console
crashed" means the console is hung and needs `iosu reboot` over the Mocha TCP shell.

The VM is started with `~/archvm.sh` on the host (qemu; ports 2222->22, 2121->21). If it was
launched as a background task of a Claude session, it stops when that session ends.

The local working copies of the platform files are in `~/Projects/wiiu-godot/tools/staging/`
on the host. The VM tree is the source of truth.

### 10.1 Correction: the real root cause was that static constructors never ran

The "reverse order" explanation above was wrong. **No C++ static constructor ran at all.** With
`-meabi`, GCC inserts a call to `__eabi()` at the top of `main`; `__eabi` calls `__init`, which
runs `__do_global_ctors_aux` over the `.ctors` list. wut's crt (`__rpx_start` -> `init_wut`)
only calls `__init_wut` and then `main`, so it relies on that hidden call. Our `main` is
compiled by Clang, which never emits it. (memprobe worked because its `main` was GCC-compiled.)
`StringName::Table::allocator` was therefore never configured, `page_size` stayed 0, and the
0-byte pages corrupted the heap.

Fix: `godot_wiiu.cpp` calls `__init()` as the first statement of `main`.
`-fno-use-init-array` stays: wut.ld puts `.init_array` into the same list, which
`__do_global_ctors_aux` walks backwards, so `.init_array` entries would run in reverse order.

### 10.2 Packaging: `godot.pck` instead of `--main-pack`

4.7 export templates reject `--main-pack` unless built with `disable_path_overrides=no`
(main.cpp:1894). The engine now gets only `--headless`, and `make_wuhb.sh` bundles the pack as
`content/godot.pck`. That is `<executable basename>.pck` next to
`OS_WiiU::get_executable_path()` (`fs:/vol/content/godot.rpx`), which is where templates look.

### 10.3 Big-endian: binary resources

`resource_format_binary.cpp` read packed arrays (int32/int64/float32/float64, Color, and the
real arrays behind Vector2/3/4) with a raw `get_buffer` into typed memory. On the Wii U a
PackedScene's `_bundled` int arrays came out byte-swapped
(`nprops[j].name = 16777216` = 0x01000000). Fixed with `fix_buffer_byte_order()`, which swaps
each element when `FileAccess::_needs_byteswap()` is true. Still not audited: images, audio
buffers, other `get_buffer` users outside this file.

First run that got this far (2026-11-12): engine banner printed, WorkerThreadPool started,
physics fell back to the dummy server, GDScript analysed, then the scene load failed on the
byte order above. `ERROR: Parameter "_create" is null. at: create (core/io/ip.cpp:336)` is
expected (no IP implementation on this platform yet).

### 10.4 Heap check and threads

`heap_check()` takes newlib's recursive `__malloc_lock` while it reads the heap. Without it,
worker threads allocating at the same time caused a false "top no longer ends at the break"
report.

### 10.5 (Resolved in 10.6) console hang on HOME

Twice, pressing HOME froze the whole console: no crash dump, `cos` commands stopped
answering, IOSU and Mocha kept running, the loading music kept playing. That points to a
PowerPC core stuck with interrupts disabled during the process switch. Once it worked (HOME
menu opened and returned). Not understood yet. Suspects: the `OSUninterruptibleSpinLock`s in
`wiiu_console.c`, `atomic64.c` and `tls_keys.c`.

### 10.6 Resolved: console hang on close / HOME

Cause: `WHBProcIsRunning()` calls `ProcUIShutdown()` itself when it sees the Exit message and
returns false. `main` then called `WHBProcIsRunning()` again (to decide whether to wait for
HOME), and `ProcUIProcessMessages` after shutdown raises
`OSPanic in "procui.c" at line 641`, which takes Cafe OS down. Earlier runs showed this as a
silent hang because the log handlers were already detached.
Fix: `wiiu_proc_is_running()` in `crash_log.c` latches the first false and never calls into
ProcUI again. The main loop (`OS_WiiU::run`), the exit wait and the crash screen all use it.
**Never call `WHBProcIsRunning()` directly.** `main` also ends with `_Exit(exit_code)` after
`WHBProcShutdown()`, so Godot's static destructors don't run after the engine is shut down.
Confirmed 2026-11-12: run, HOME menu and back several times, close from the HOME menu, and
the console returns to the Wii U Menu.

Clock: wut's `clock_gettime` supports only `CLOCK_REALTIME` (1) and `CLOCK_MONOTONIC` (4).
Godot's OS_Unix uses `CLOCK_MONOTONIC_RAW` (5) whenever it's defined, so ticks stayed 0 and
every frame's delta was 0. `OS_WiiU::get_ticks_usec()` overrides it with `OSGetSystemTime()`.
The hello project then runs at ~25 fps headless, limited by the per-frame console redraw
(clearing the TV buffer and drawing both screens). Redrawing only when the log changes would
fix that.

## 11. Planned (user requirements, not built yet)

**Once there is a renderer** (GX2 DisplayServer), the on-screen log becomes an overlay that is
off by default:

- The GamePad chord **ZL + L + D-pad Right + X + Y + TV, pressed in that order** (each button
  added while the previous ones are held) toggles the display between the game and the Godot
  log, and back. The log keeps the D-pad / L / R scrolling.
- It also works after a crash, where the game's last frame stays on screen.
- **On a crash, don't switch to the log automatically** (so the player doesn't get a
  jumpscare). Keep the last frame and draw a centred message on it:

      Godot crashed!
      Press ZL+L+RIGHT+X+Y+TV to see log
      Press HOME to exit

  The full red crash log (registers, backtrace) is what the chord shows.
- **Checked 2026-11-12:** the TV button does reach the app through `VPADRead`
  (`VPAD_BUTTON_TV`, 0x00010000; the joypad driver reports it as `JoyButton::MISC1`), so the
  chord can use it. HOME also shows in `hold` (0x2) but belongs to the system.
- Until graphics exist, the log stays the main screen (current behaviour: black, red on crash).

## 12. Input (joypad_wiiu.cpp), 2026-11-12

`JoypadWiiU` (created in `OS_WiiU::initialize_joypads`, polled once per frame at the top of
`OS_WiiU::run`'s loop) is the only GamePad reader while Godot runs. VPAD's `trigger` bits are
per read call, so the log console gets the same sample through `wiiu_console_handle_buttons`;
`wiiu_console_update_input` (its own `VPADRead`) is only for the crash and exit screens.

- Device 0: GamePad. Devices 1-4: KPAD channels 0-3 (Wii Remote, + Nunchuk, Classic
  Controller, Wii U Pro Controller; `WPADEnableURCC(TRUE)` for Pro/Bloopair pads). Changing
  the extension reconnects the device under a new name.
- **Buttons map by printed label** (the user's choice): Wii U A = `JoyButton::A`, B = B, X = X,
  Y = Y, - = BACK, + = START, L/R = shoulders, stick clicks, D-pad, TV = MISC1. ZL/ZR are
  trigger axes (0 or 1; analog on an original Classic Controller). Stick Y is negated (Godot
  is +Y down).
- GUIDs `cafe00...0N` are not in Godot's mapping DB, so raw values pass through unmapped.
- Verified on the console with the GamePad: every mapped button, both sticks in every
  direction, ZL/ZR, TV. KPAD is untested (no Wii Remote was connected).
- `LOG_GAMEPAD_BITS` in `joypad_wiiu.cpp` logs raw `hold` changes; turn it off when it gets noisy.
- LVENGINE: the user added joypad bindings to its InputMap themselves (synced from their PC
  working copy at `~/Projects/Godot/LVENGINE`, not from git).

## 13. Video (GX2), 2026-11-12

Stages, one console run each:
1. **Done:** DisplayServerWiiU ("Cafe OS") + wiiu_gx2.c clear/present, 60 fps vsync on TV and
   GamePad. Two fixes were needed: re-enable TV/DRC output every frame (WHBGfx does too), and
   **never start OSScreen before GX2**. The log console starts suspended and only brings up
   OSScreen in `wiiu_console_resume()` (crash screen, error exit). OSScreen-then-GX2 left both
   screens black until an overlay re-applied the GX2 scan buffers.
2. **Done:** textured quad with `shaders/texture.gsh` (Love Potion, MIT, see
   `shaders/README.md`; embedded by SCsub as `wiiu_shader_texture`). Verified: linear RGBA8
   upload, vertex fetch, byte-swapped uniform blocks, alpha blending.
3. **Written and building, NOT yet run on the console** (commit "WIP: GX2 2D renderer"):
   - `wiiu_gx2.h/.c`: GX2 layer (textures, render targets as sampleable colour buffers in
     MEM2, screen colour/scan buffers in the MEM1 frame heap tied to ProcUI, two 2 MB vertex
     arenas alternating per frame, scissor, batched triangles/lines, present).
   - `rasterizer_wiiu.h/.cpp`: RasterizerWiiU on RasterizerDummy. Swaps in
     TextureStorageWiiU (textures, proxies, render targets, canvas textures), UtilitiesWiiU
     (free() for those RIDs; the dummy's free() uses non-virtual owns_texture), and
     RasterizerCanvasWiiU (rect incl. region/flip/transpose/tile, nine-patch stretched,
     polygons, primitives, transform, clip/clip-ignore; colour = command * final_modulate *
     canvas modulate; final_transform is already in render-target pixels).
   - Blit: own fit per screen (largest whole-number scale with nearest filtering, else a
     linear downscale). Godot's dst_rect is ignored on purpose.
   - DisplayServerWiiU reports one 1280x720 fullscreen window/screen and installs
     RasterizerWiiU (dummy if GX2 failed). Presenting happens in RasterizerWiiU::end_frame;
     the OS loop no longer calls a present.
   Next: run Godot Hello (it has no visuals beyond the default clear colour; add a Label or
   ColorRect to the test project), then LVENGINE. Likely first issues: clear colour/alpha,
   render target and texture cache invalidation, font textures (LA8, converted to RGBA8),
   stretch.
4. Planned: nine-patch tile modes, MSDF text, materials/shaders, the log-overlay chord and
   crash overlay (section 11), drawing the log with GX2 instead of OSScreen.

Tooling notes:
- Start the VM (`~/archvm.sh`) and the Mocha logger detached (`setsid nohup ... &`), not as
  Claude background tasks, which die with the session.
- HOME round trips and closing via HOME work again with GX2 (stage 1 build). The user may
  close apps with Aroma's "close/restart" plugin instead.

## 14. Status 2026-11-12 (late): LVENGINE playable on the console

Verified on the console: GX2 2D rendering (rects, StyleBoxFlat polygons, Line2D, textures,
text), GamePad input, AX audio (Ogg music and, after the WAV fix, LVENGINE's sound effects),
user:// saves, clean exit and HOME. LVENGINE plays its menus and rooms with Undertale
music copied from the user's own Steam install to `/wiiu/godot/LVENGINE/mus/` on the SD card
(LVENGINE uses a custom user dir, so user:// is `fs:/vol/external01/wiiu/godot/LVENGINE/`).
LVENGINE's own Wii U music detection (playervars.gd, `cafe_path := "user://mus/"`) is in the
user's working copy, not git.

Fixes since section 13 (all in commits on wiiu-port):
- DisplayServerWiiU `window_can_draw`/`can_any_window_draw` true while GX2 owns the screens
  (headless returns false and Godot skipped rendering).
- GX2DrawDone only while in the foreground (it faulted after ProcUI released on exit).
- DirAccessWiiU: stat-checked `change_dir` (wut's chdir accepts missing dirs, so user:// was
  never created), `make_dir_recursive` from the SD root, `dir_exists` ignores a trailing "/".
- Profiles: webp (imported textures), regex (LVENGINE's typewriter label), brotli (Godot's
  built-in default font is WOFF2; without brotli every glyph drew as a hex box).
- AudioDriverWiiU ("AX"): two sndcore2 voices over 16-bit rings, 48 kHz, TV + GamePad.
- AudioStreamWAV: 16-bit samples byte-swapped on big-endian.
- Crash screen while GX2 runs: the last frame with a 50% red box and centred CafeStd text
  ("Godot crashed!", "Press [ZL]+[L]+[→]+[X]+[Y]+[TV] to see crash log", "Press [HOME] to
  exit"). The button icons are CafeStd private-use glyphs: A E000, B E001, X E002, Y E003,
  HOME E044, D-pad right E07C, L E083, ZL E085, TV E089. CafeStd comes from
  `OSGetSharedData(OS_SHAREDDATATYPE_FONT_STANDARD)` and is baked with FreeType at GX2 start-up
  (wiiu_system_font.cpp). OSScreen fallback if the crash is inside gx2.rpl or GX2 isn't up.
- Log toggle ZL+L+Right+X+Y+TV (in order, holding each), during play and on the crash screen;
  B closes it; drawn with GX2 and a 10x20 bitmap font (crash_font_data.c, generated by
  tools/make_crash_font.py from tools/DejaVuSansMono.ttf).
- On OSScreen screens the HOME menu is disabled; HOME (caught via ProcUI's
  HOME_BUTTON_DENIED callback) calls SYSLaunchMenu to return to the Wii U Menu. A clean game
  exit (code 0) also returns there.

Build helpers on the VM: `/home/user/wiiu/build_lv.sh` (LVENGINE) and
`/home/user/wiiu/build_hello.sh` (Godot Hello, lvengine profile). Both build, export, package.
The test project plays `music/knight_sideb.ogg` if present (git-ignored, user's file).

Open:
- KPAD controllers (Wii Remote, Pro, Classic) untested.
- Renderer gaps: nine-patch tile modes, MSDF text, canvas shaders/materials, lights,
  particles, meshes, canvas groups, back-buffer copies.
- TV output is 720p only, with whole-number scaling (LVENGINE's 640x480 shows at 1x on the TV).
- HOME menu while GX2 runs has been flaky in the past; the user often closes apps with
  Aroma's close/restart plugin.

## 15. Planned later (user ideas, 2026-11-12)

Not started; the user asked to keep these for later.
- **3D:** via a GX2 version of the Compatibility (GLES3) renderer's 3D side. The blocker is
  shaders: Godot generates GLSL at runtime, GX2 needs binaries, so it needs CafeGLSL (GLSL 3.3
  compiled on the console) with a shader cache on the SD card. Suggested order: CafeGLSL for
  2D canvas shaders first, then basic 3D (meshes, depth buffer, Camera3D, StandardMaterial3D
  subset, one directional light), then lights/shadows/skeletons.
- **Separate rendering for the TV and the GamePad** (today both show the same root viewport,
  fitted per screen).
- **GamePad features:** gyro/accelerometer, microphone, touchscreen (as touch/mouse input),
  and Wii Remote pointing (IR).
- **USB keyboard and mouse**, if Cafe OS exposes them to apps.
- Also from the fix list: 1080p TV mode (LVENGINE's 640x480 at 2x), on-screen keyboard
  (swkbd) for LineEdit/TextEdit, an editor export plugin, release templates.

## 16. Exporting games: `cafe-export` (2026-11-12)

`platform/wiiu/tools/cafe-export.sh` exports any Godot 4.7 project as a .wuhb from the user's
normal editor (no custom editor build):

    cafe-export <project-dir> [out.wuhb] [--preset NAME] [--name ...] [--short-name ...]
                [--author ...] [--icon png] [--tv-image png] [--drc-image png] [--upload HOST]

It runs `godot --headless --export-pack <preset>` (the first preset in export_presets.cfg by
default; any platform works, it only decides which resources go in), then `wuhbtool` with the
Cafe Godot template, then optionally uploads to `sd:/wiiu/apps/` over FTP. Name and icon come
from project.godot (config/name, config/icon, including uid:// icons via the .import files);
icons of 64 px or smaller are scaled with nearest-neighbour; splash images are fitted on black.

On the user's PC it's installed as `~/.local/bin/cafe-export` (symlink to
`~/Projects/wiiu-godot/tools/cafe-export.sh`). Files in `~/.local/share/cafe-godot/`:
`bin/wuhbtool` (copied from the VM's devkitPro) and
`templates/4.7.2/wiiu_template_debug.rpx`. **After rebuilding the engine, copy the new
`bin/godot.wiiu.template_debug.ppc32.rpx` there** (it's the lvengine profile build: GDScript,
2D physics, text_server_fb + freetype, ogg/vorbis, webp, regex, brotli). The user's editor
must be 4.7.x (theirs is 4.7.2 from Arch, the same commit ed1daf0bf as Cafe Godot).
Verified: LVENGINE exported and uploaded with it.

The later "real" option (an editor export platform in platform/wiiu/export/) needs a
custom editor build; see section 15.

## 17. GamePad touchscreen (2026-11-12)

`JoypadWiiU::process_touch` reads VPAD `tpNormal` each frame, calibrates it with
`VPADGetTPCalibratedPointEx(VPAD_CHAN_0, VPAD_TP_854X480, ...)` to GamePad pixels, and maps
it to Godot's window with `rasterizer_wiiu_drc_to_window()` (GamePad pixel -> render-target
pixel via the last GamePad blit's fit rect -> window pixel via Godot's BlitToScreen dst_rect).
Touches in the black bars are ignored. Events: InputEventScreenTouch / InputEventScreenDrag,
index 0; Godot's default emulate_mouse_from_touch turns them into clicks, so Controls work.
Verified on the console with Godot Hello's counter button. Samples with an invalid axis
(`validity != VPAD_VALID`) keep the previous position.

## 18. Session 4 (2026-11-13): funkin_godot, memory, meshes and particles

- **Names:** `OS.get_name()`, `get_distribution_name()` and `DisplayServer.get_name()` are all
  `"Cafe OS"` (user choice). Feature tags unchanged: `wiiu`, `game_console`, `homebrew`.
- **Build profile:** `profiles/cafe.py` is the general profile now (`build_lv.sh` uses it):
  lvengine.py plus `disable_3d = "no"` (3D node classes exist so scripts naming them compile;
  nothing 3D draws) and `module_interactive_music_enabled` (AudioStreamSynchronized, used by
  funkin_godot for song stems). The .rpx grew ~2 MB to 21.7 MB.
- **Texture memory:** `_upload` no longer duplicates images that are already RGBA8, and only
  textures up to 512x512 keep the CPU `Image` (for `texture_2d_get` / `get_image()`); bigger ones
  are GPU-only and `get_image()` returns null with a one-time warning. funkin_godot's art is
  ~634 MB as RGBA8, so this halves what a song needs. Next step if memory runs out: GX2 BC/DXT
  textures (game imports as VRAM compressed S3TC).
- **MeshStorageWiiU** (rasterizer_wiiu.cpp): sits on the dummy storage (same RIDs) and decodes
  each 2D surface once in `mesh_add_surface` via
  `RenderingServer::mesh_create_arrays_from_surface_data`, plus multimesh instance buffers (2D: 8
  floats, 3D: 12, then colour 4, custom 4). Real AABBs, so the canvas doesn't cull mesh items.
  **ArrayMesh resources loaded from files keep their vertex bytes little-endian (not swapped)**;
  run-time meshes (QuadMesh etc., CPUParticles2D, SurfaceTool) are native and fine.
- **Drawn now:** MeshInstance2D, MultiMeshInstance2D, CPUParticles2D (instances with a zero
  basis or zero alpha are skipped), nine-patch tile / tile fit (centre split into tiles, max 256
  per axis), `draw_animation_slice` (TileMap animated tiles use it). `_flush` submits in chunks
  of 49152 vertices so a batch never exceeds the 2 MB arena (wiiu_gx2_draw drops oversized draws).
- **Still not drawn:** GPUParticles2D (TYPE_PARTICLES), skeletons/blend shapes, canvas shaders
  (gdanimate uses `render_mode blend_add`/`blend_sub`: mapping render_mode blend lines and
  CanvasItemMaterial blend modes to GX2 blend state is the cheap first shader step), canvas
  groups, back-buffer copies, MSDF text, 2D lights, mipmaps (deferred: +1/3 memory).
- **Render test:** `platform/wiiu/tests/render_test` (all-script scene): spinning QuadMesh,
  ring of 16 multimesh quads, CPUParticles2D fountain, three nine-patches, red/blue slice blink,
  footer with the OS and display server names. Export with `cafe-export`.
- **funkin_godot** (`~/Projects/Godot/funkin_godot` on the host, the user's game; no game
  changes made): exports with cafe-export (141 MB wuhb, preset "Linux x86_64"). First runs:
  missing AnimatedSprite3D / AudioStreamSynchronized broke script compilation and softlocked song
  load (fixed by cafe.py). Song playback on the new build not confirmed yet. "Failed to open mods
  directory" is harmless (external mods only; `DirAccess.open("./")` fails on the Wii U).
- **Console crash seen once (not reproduced):** the HOME Menu (process 5) died with `0xCAFEBADD`
  in Aroma's homebrew_rpx_loader (`my_HBM_NN_ACP_ACPGetTitleMetaXmlByDevice` -> `snprintf` ->
  `__wums_getreent`, DAR "_wut"). Not Godot code; HOME worked fine afterwards with the same wuhb.
- **GDScript analyzer (core patch, modules/gdscript/gdscript_analyzer.cpp):** autoload constants
  are read with `get_validated_object()` (3 places). funkin_godot's TouchControls autoload
  `queue_free()`s itself off mobile; scripts naming it then made the analyzer read the freed
  node's vtable (DSI in `reduce_identifier` entering Freeplay). Desktop survives the same
  use-after-free only because the memory isn't reused yet.
