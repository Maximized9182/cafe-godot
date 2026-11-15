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

The local working copies of the platform files are in `~/Projects/Vibecoded/wiiu-godot/tools/staging/`
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
`~/Projects/Vibecoded/wiiu-godot/tools/cafe-export.sh`). Files in `~/.local/share/cafe-godot/`:
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
- **Stats overlay (wiiu_stats.cpp):** ZR+R+LEFT+Y+B+L toggles a panel (top right; drag it on
  the GamePad, it snaps to the nearest corner on release; touches starting on it never reach the
  game). Shows FPS, frame time avg/min/max + 90-frame graph, Godot process/physics time, CPU
  (main thread % of a core and the process across 3 cores, from `OSThread::coreTimeConsumedNs`
  over the active-thread list, plus the busiest other thread), GPU time
  (`GX2SampleTop/BottomGPUCycle` around each frame, read 2 frames later), draws/vertices, MEM2
  used/largest free, MEM1 free, Godot heap and peak, GX2 texture/render-target bytes, object/node/
  orphan counts, GamePad battery, uptime. Counters live in wiiu_gx2.c (`wiiu_gx2_get_stats`).
- **Plain .rpx mode (2026-11-13):** without a bundle (no fs:/vol/content/godot.pck) the pack is
  read from sd:/wiiu/apps/godot/godot.pck next to sd:/wiiu/apps/godot/godot.rpx
  (`OS_WiiU::get_executable_path`). Used to test whether the app-switch hangs come from Aroma's
  .wuhb handling.
- **Audio threading (2026-11-13, verified on console):** the mixer is its own OSThread created on
  core 2 at priority 16, and AXInit runs from a short-lived thread on core 2 too. Before:
  mixer on core 1 at priority 16 crackled (waited out main-thread frames); at priority 12 it
  buzzed; with AX initialised from the main thread, every main-thread frame over ~34 ms gave a
  short AX buzz with no ring underrun (AX's CPU-side work presumably waits behind the main thread
  on core 1). Cores: 0 and 2 have 512 KB L2, core 1 (main thread) 2 MB.
- **App-switch hangs are tied to .wuhb (2026-11-13):** as a .wuhb, FNF hung the console on
  HOME / returning / closing / loading the menu ~5 times in a few hours (Mocha shell dead, our
  app already released or exited, GPU idle). As a plain .rpx (godot.rpx + godot.pck in
  sd:/wiiu/apps/godot/) the user opened and closed the HOME menu ~19 times with no hang. The one
  stack trace (0xCAFEBADD) was in Aroma's homebrew_rpx_loader. Suspect Aroma's bundle handling;
  ship big games as .rpx + pack until that's understood (cafe-export only makes .wuhb so far).
- **Pack on the SD card, bundle for icon/name (`cafe-export --pack-on-sd DIR`):** the .wuhb
  holds the .rpx, icon, name and `godot.pckdir` (one line, e.g.
  `fs:/vol/external01/wiiu/apps/funkin`); `godot.pck` lives in that folder. Test of the theory
  that the hangs come from Godot reading its pack through Aroma's bundle code
  (ContentRedirection/WUHBUtils) from several threads, while a background thread mid-read holds
  something the HOME menu needs. FNF exported this way on 2026-11-13 for the user to test.

### 2026-11-13 (late): tiled-texture garbage, HOME hang with the pack on SD

- Tiled textures (092ee2b) showed noise around FNF's health icons, notes and stage props; the 17:24 pre-tiling screenshot was clean. A GPU readback check after each upload (copy tiled back to linear, memcmp with the source) found random failures (the same 150x150 icon ok once, bad once): stale GPU cache, not a size rule. upload_tiled now invalidates CPU+TEXTURE on the staging buffer and TEXTURE+COLOR_BUFFER on the destination before and after GX2CopySurface. The readback check still runs for every texture (logs `[gx2] tiled check`; remove once confirmed clean, it costs a copy + GX2DrawDone per texture).
- The hybrid .wuhb (pack on SD, godot.pckdir) survived ~30 HOME rounds, then hung once. So bundle reads are not the (only) cause. In the hang: our release callback finished (GX2DrawDone 0 ms, MEM1 freed, "foreground released"), the audio heartbeat kept running for 40+ s, but the Mocha syslog never printed ProcUI's "Releasing foreground" and went silent. The main thread is stuck inside ProcUI's release after our callback.
- wiiu_threaddump.c: the audio thread's background heartbeat dumps every thread of the process (name, state, priority, PC/LR with OSGetSymbolName, mutex waited on + owner, queue, 10-frame back chain) after 3 s in the background and every 15 s. Next hang: read the `[threads]` lines in udp.log; the main thread is OSGetDefaultThread(1).
- Cause of the garbage (found 2026-11-13 late): freshly allocated memory that only the GPU writes (tiled textures via GX2CopySurface, render targets, screen colour buffers) can still hold dirty CPU cache lines from its previous owner. They get written back later, on top of the GPU's data: random foreign texels appearing after a correct upload (readbacks right after the copy passed). Fix: GX2Invalidate(GX2_INVALIDATE_MODE_CPU) (DCFlushRange) over such memory right after allocating it. The readback check and retries are removed (they caused a big stutter on loads).
- First thread dump of a slow HOME (menu showed after ~15 s, 22:44:40, build 606aa2c): our release callback done at once; Mocha: "Releasing foreground" +0 ms, "AXIST thread has stopped" +150 ms, then 2 s gap, "PROCESS 15 LOST FOREGROUND" / "PROCESS 5 GAIN FOREGROUND" at +2.1 s. Dumps at +2 s, +14 s, +29 s all the same: main thread parked in ProcUIProcessMessages (OSWaitEvent), "{SYS ProcUI Core 0}" state running inside OSReleaseForeground -> OSWaitRendezvous, "{SYS ProcUI Core 1}" running (spinning, no context), "{SYS ProcUI Core 2}" idle in OSWaitEvent on the idle event. In a normal background dump all three ProcUI core threads sit idle in OSWaitEvent. So a cross-core rendezvous in OSReleaseForeground waits for core 2, where our audio thread and AX (AXInit on core 2) live. Not GX2. Still needs a dump of a full hang.
- Fix (6490ca1, likely resolved, 2026-11-13, keep watching): the slow/hung HOME came from our two AX voices left looping through the ProcUI release. The system's sound shutdown ("AXIST thread has stopped", SNDD driver on core 2) waited for them: 2 s gap before "PROCESS n GAIN FOREGROUND", then ~13 s with the HOME menu invisible (rendezvous in OSReleaseForeground waiting for core 2), and sometimes it never finished (full hang, our last block repeating). While FNF sat half-released in the background, the HOME menu and the Wii U Menu stayed slow until FNF was closed. audio_driver_wiiu.cpp now registers ProcUI callbacks: release stops both voices and zeroes the rings, acquire plays them again (the underrun resync realigns write_pos). After it: 14 HOME rounds during songs, "Releasing foreground" to GAIN FOREGROUND ~150 ms, no stall. The thread dump (wiiu_threaddump.c), the [bg] heartbeat and the slow-frame log stay in for now; if a HOME hang comes back, read the [threads] lines in udp.log.
- Side note: FNF's ~1/3 s hitch on the first note is game-side CPU (slow-frame log: game 168 ms, render 3 ms, 0 textures made, 28 texture updates with 0 ms upload; font glyph work or GDScript). Not chased further.
- Debug cleanup: removed the [bg] heartbeat, the background thread dumps, the [procui] acquire/release chatter (a GPU timeout on release still logs), wiiu_main_state, the slow-frame log and per-frame texture counters, the [audio] underrun / voices lines, and the startup "AX initialised on core" / "Audio thread on core" / "Executable path" lines. wiiu_threaddump.c stays in the tree but isn't built: add it back to SCsub and call wiiu_thread_dump() from the audio thread's background branch (see git history before this commit) if a HOME hang returns.
- HOME stall came back (2026-11-13 late) with the voices already stopped. mocha.log over all boots: every slow release (5 of ~40 for pid 15) takes exactly 2.09 s from "ProcUI: Releasing foreground" to "PROCESS 15 LOST FOREGROUND", with "AXIST thread has stopped" late (150-300 ms vs ~35 ms); fast ones take 0.15 s. While the app sits half-released, the HOME menu and its applets (Wii Remote settings) stall too, until the app is closed. Thread dumps: ProcUI core 0/1 threads inside OSReleaseForeground -> OSWaitRendezvous, core 2's ProcUI thread idle (Cemu's proc_ui.cpp reimplementation: core threads run per-core callbacks, rendezvous A/B/C on mask 7, then OSReleaseForeground on each core). All slow releases are after 2f960b4 (AXInit on core 2); exit logs "WARNING ignoring AXQuit() called from core 1 (AXInit() was on core 2)". SDL2's Wii U audio keeps AX on core 1 ("AX functions need to run from the same core"). Test build: AXInit back on the main thread (WIIU_AX_INIT_CORE 1 in audio_driver_wiiu.cpp), audio thread still on core 2, voices still stopped on release. Watch for the old buzz on long frames.

### 2026-11-14: HOME stall fixed, most "freezes" were self-inflicted

- The 2.09 s HOME stall is fixed by AXInit on the main core (WIIU_AX_INIT_CORE 1, 63c7b37): 240 Godot releases in mocha.log since then, none over 1 s. AX on core 2 (2f960b4) caused it.
- Most HOME "freezes" on 2026-11-14 (12:03, 13:02, 13:13, 13:23, and the Mocha log dying at 13:09) came seconds after a failed FTP upload (curl exit 25) of funkin.wuhb while FNF was running: ftpiiu opens the mounted bundle of the running app for writing, the transfer fails, and the next HOME (the HOME menu reads the running .wuhb's meta/icon through RPXLoadingModule) freezes or crashes the console. **Never upload a .wuhb while that app runs; wait for the user to confirm it's closed.** The earlier HBM crash in RPXLoadingModule's ACPGetTitleMetaXml hook may have the same cause.
- Still open: one freeze with no upload nearby (FNF main menu after visiting story mode, 14:11). Render-test variants with the same engine did 190 HOME rounds without a freeze (plain 78, 250 MB held 63, ~100 MB tiled textures + looping Ogg 49). At ~1 freeze per 15-20 presses, a bisection step needs 60+ rounds.
- Also from this session: 60 fps in FNF from one frame in flight (595336b); stats-overlay buzz was mallinfo (~20 ms under the allocator lock), replaced by an sbrk counter (e743bb4); HOME now sends focus out/in and paused/resumed (b41df8f); the audio mixer thread is on core 0 at priority 15; the [home] breadcrumbs (b83c321) are still in and temporary.
- Mipmaps (78e844b): textures whose Image has mipmaps upload every level (tiled only: one linear staging surface per level, GX2CopySurface(src, 0, 0, dst, level, 0); the GX2Surface gets mipLevels and a separate `mipmaps` allocation; viewNumMips follows). wiiu_gx2_draw's filter is now WIIU_FILTER_* bits (linear, mipmaps); samplers [4][2] with mip filter LINEAR and LOD 0-14 for the *_WITH_MIPMAPS canvas filters, NONE otherwise. Verified on the console with the render test's coloured-level texture (quarter size shows level 2, blue). FNF imports without mipmaps, so it doesn't use them. Same screenshot verified the five blend modes and the nine-patch tiling.

- Side port (2026-11-14): "Deltarune Lightners Live Plus" (Godot 4.6.0) runs with nlaha/godot-midi built in as a custom module, kept outside this repo in /home/user/wiiu/lightners (host: ~/Projects/Vibecoded/wiiu-godot/lightners, README there). Build with custom_modules=... extra_suffix=midi; the regular template is untouched.

- MSDF text works on hardware (a955142, verified 2026-11-14 16:21): shaders/msdf_vsh.glsl + msdf_fsh.glsl compiled with CafeGLSL (third_party/cafeglsl on the host, `glslcompiler --uniform-mode register`), msdfgen module enabled in profiles/cafe.py, px_size computed on the CPU from the glyph scale. **Lesson for canvas shaders:** a per-draw uniform block written into the vertex arena read as zeros on the real GPU (Cemu was fine); per-draw parameters as pixel uniform registers (GX2SetPixelUniformReg, register shader mode) work. Shader sources: host ~/Projects/Vibecoded/wiiu-godot/shaders_src (need `#extension GL_ARB_separate_shader_objects : enable` for CafeGLSL).

- GPUParticles2D (b3b22ce): on WIIU_ENABLED builds the node creates an internal CPUParticles2D twin via convert_from_particles on ENTER_TREE and forwards set_emitting/restart/speed_scale/amount/texture/process_material (core patch in scene/2d/gpu_particles_2d.*). Only ParticleProcessMaterial particles; custom particle shaders draw nothing. Verified on the console (render test, 16:27).

### 2026-11-14 (evening): CafeGLSL as cafeglsl.rpl, canvas shaders

- **CafeGLSL can't be linked into the .rpx:** its ~5 MB of code pushed .text to 36.5 MB, past the 32 MB limit where long-branch stubs break things (a stub to `__cxa_atexit` misbehaved: global constructors looped until out of memory). SCsub now fails the build if .text >= 32 MB and prints the headroom (30.72 MB at 74c9f48, 1.28 MB left). `cafeglsl_path` in detect.py (static link) stays opt-in and shouldn't be used.
- **cafeglsl.rpl (d018c04, `platform/wiiu/cafeglsl_rpl/`):** CafeGLSL's Wii U static lib built as its own Cafe OS module with `build.sh /path/to/cafeglsl` (the folder with lib/libcafeglsl.a; on the VM /home/user/wiiu/cafeglsl). Goes to `sd:/wiiu/libs/cafeglsl.rpl`. wiiu_gx2.c's glsl_load() tries OSDynLoad_Acquire("cafeglsl"), then "~/wiiu/libs/cafeglsl.rpl", and finds InitGLSLCompiler / CompileShaderPair / FreeShaders. Without the RPL, runtime shaders are just off (MSDF falls back to the precompiled msdf.gsh).
  - rpl_main.c forwards the module's malloc family to MEMAllocFromDefaultHeapEx (the app's heap, i.e. Godot's malloc) with a 32-byte header holding {base, size}; `__init_wut_malloc` is a no-op.
  - **Entry point:** with wut's `__rpl_start` the module hung inside OSDynLoad_Acquire (the app never got past loading it). A custom entry `cafeglsl_start` (`-Wl,--entry=cafeglsl_start`) doing `__init_wut(); __eabi();` then `rpl_entry` works. Which step of `__rpl_start` hung was never pinned down.
  - CafeGLSL init takes ~370 ms once (at the first compile); each canvas shader then compiles in 7-8 ms.
- **Canvas shaders (74c9f48, verified on the console 17:33 with the render test's shader row):** MaterialStorageWiiU::shader_set_code runs Godot's ShaderCompiler with drivers/gles3's canvas renames (copied, gles3 isn't built), assembles a GLSL 330 fragment shader (template = gles3 canvas.glsl fragment main() minus lights/normal maps; vertex colour already holds modulate) and compiles it with a pass-through vertex shader through wiiu_gx2_program_compile. Programs with identical GLSL are shared (program_cache, refcounted).
  - Uniforms are loose (`uniform ` prefixed onto gen_code.uniforms) and live in pixel uniform registers: offsets from GX2GetPixelUniformVar("m_" + name), packed per material (floats as floats, ints raw, bools as ~0, matrices one column per register), uploaded with GX2SetPixelUniformReg. wiiu_gx2_draw adds the built-ins time, color_texture_pixel_size, screen_pixel_size. Material params are now stored (material_set_param/get_param overrides; the dummy dropped them). sampler2D params resolve through TextureStorageWiiU; defaults white / black / transparent by hint.
  - Batches split on the material (batch_material in _begin); draw_texture_rect clears it.
  - **Falls back** (draws without the shader, keeps the blend, logs `[shader] <path>: X isn't supported yet`): vertex() with code, screen texture, SDF, global/instance uniforms, uniform arrays, samplers other than sampler2D. light() is ignored. Shaders with no fragment code (CanvasItemMaterial) don't compile anything.
  - Render test shader row (verified): const, UV gradient, params from GDScript, uniform default, TIME, TEXTURE x tint, int/bool, sampler2D param (UV*2 clamps: repeat off by default), vertex() fallback grey.
  - Not checked yet: SCREEN_UV orientation (gl_FragCoord origin on GX2), real games' shaders. FNF exported with this template and uploaded 17:37 for the coverage check: read `[shader]` lines in udp.log.
- Next ideas, in order: run FNF/LVENGINE/Lightners and collect `[shader]` fallbacks; vertex() support (needs local vertices + item/canvas matrices on the GPU instead of the CPU transform); SCREEN_TEXTURE (back-buffer copy).
- Recovering a hung app: `cos killrestart` in the Mocha shell killed the hung app but COS then hit a fatal error; `iosu reboot` afterwards. Prefer `iosu reboot` straight away.
- Pushed to GitHub 2026-11-14 (up to 69cdaec).

### 2026-11-15: cleanup, heap check off, ProcUI release without the extra frame

- **Heap check off by default (6877204):** crash_log.c's heap check (newlib malloc lock + sbrk(0) + stack walks on entry and exit of every malloc/calloc/realloc/free, from the StringName overrun hunt in section 10, which it solved) had stayed in every build. It's now only built with `wiiu_heap_check=yes` (detect.py), which also adds the malloc/calloc/realloc/free --wrap flags; abort and _sbrk_r stay wrapped (crash screen, stats overlay heap size).
- **One draw state (6877204):** wiiu_gx2_draw(const WiiUDrawState *, ...) takes blend, MSDF parameters and the canvas shader (program, registers, extra textures, time); NULL / WIIU_DRAW_STATE_DEFAULT is a plain mix draw. The wiiu_gx2_set_blend/set_msdf/set_program globals are gone. MSDF and canvas shaders share one register-mode path (RegPipeline: fetch/vs/ps, texture sampler, built-in uniform offsets incl. MSDF `params`).
- **MSDF from msdf.gsh again; cafeglsl.rpl loads lazily (6877204):** the MSDF runtime compile at start-up (only a CafeGLSL test, ~370 ms per launch) is removed. CafeGLSL now loads on the first canvas shader that needs a program (after ShaderCompiler, the fragment check and the fallback checks), so CanvasItemMaterial-only games never load it.
- **rasterizer_wiiu.cpp split (916f719, no code changes):** texture_storage_wiiu.cpp (+ UtilitiesWiiU), mesh_storage_wiiu.cpp, material_storage_wiiu.cpp, rasterizer_canvas_wiiu.cpp; rasterizer_wiiu.cpp keeps RasterizerWiiU. All share rasterizer_wiiu.h.
- **ProcUI (833d2e8):** wiiu_proc_is_running sends focus out / paused, then calls ProcUIDrawDoneRelease right away (WHBProcIsRunning style). Before, the game ran one more frame first (like SDL2), so a long frame (scene load) delayed the release; suspect for the rare HOME freeze. First test 2026-11-15: 109 HOME menu openings in FNF/LVENGINE/render test, no freeze, longest release 0.84 s. Keep watching; revert if focus-dependent pausing breaks in some game.
- Verified on the console 2026-11-15: render test screenshot (MSDF, shader row, blends, mipmaps, particles) unchanged at 60 fps; FNF title + songs fine; start-to-title time unchanged at whole-second resolution (12 s).
- FNF note: its project binds ui_accept to joypad button 1 and ui_cancel to button 0 (Nintendo positions), while Cafe Godot maps by label (Wii U A = JoyButton::A = 0). So in FNF, A is back (and quits on the title screen) and B confirms. Engine mapping unchanged; fix in the game's bindings, or add an opt-in positional mapping later.
- Not done from the cleanup list: tools/staging (host) is stale; comments were left as they are.

### 2026-11-15 (later): screen texture and vertex() in canvas shaders

- **Screen texture (182d9ee):** as in drivers/gles3, the first item in a canvas whose shader reads the screen texture (`hint_screen_texture`, generated as `color_buffer`) flushes the batch and copies the render target into its back buffer (wiiu_gx2_render_target_copy_back_buffer: GX2CopySurface into a same-format surface made on first use, freed with the target); later items reuse it, BackBufferCopy nodes copy again (always the whole target). WiiUDrawState.screen_texture; RegPipeline.screen_texture_location. Verified: FNF's screen_grayscale.gdshader and the render test (grey right half of the ring, lined up) on hardware, so SCREEN_UV = gl_FragCoord * screen_pixel_size is the right way up on GX2.
- **vertex() (ac05fdd):** items whose shader has vertex() code are batched alone (local_item in _begin's key). Their vertices are transformed back to the item's local space (inverse of ci->final_transform, applied in _vertex) and wiiu_gx2_draw copies them unconverted (WiiUDrawState.local_vertices); a generated vertex shader (gles3 canvas.glsl attribute path) runs the user code on VERTEX, then model_matrix (the item transform, mat4 in vertex uniform registers) and a pixel-to-clip SCREEN_MATRIX. CANVAS_MATRIX is identity (already in model_matrix). Material uniforms used in vertex() get vertex registers (Param.vs_location, MaterialData.vs_regs). User varyings get explicit locations from 2 (template: colour 0, UV 1). Still falls back: textures read in vertex() (vertex samplers). Known difference: VERTEX includes draw_set_transform (command transforms), unlike Godot. Verified with the render test's vertex() box (uniform shift + slant).
- Next (user, 2026-11-15): gyro, microphone, USB keyboard and mouse.

### 2026-11-15 (later): GamePad motion, USB keyboard and mouse, microphone

- **Motion (joypad_wiiu.cpp):** VPAD accelerometer (g), gyro (turns/s) and magnetometer go to Input's device sensors (get_accelerometer/gravity/gyroscope/magnetometer; screen axes x right, y up, z out of the screen; gravity is a low-passed accelerometer) and, when the game enables them, to joypad 0's motion sensors (GamePadFeatures, joy_motion_sensors with SDL axes). Mapping from hardware readings (input test): screen = (a.x, -a.z, -a.y) for both accel and gyro; flat face up +z, upright +y, right edge down -x (7785ab8; the x fix is built but was not re-tested after the last change). Input.set_joy_motion_sensors_enabled only works after the GamePad connected (first frame, after _ready).
- **USB keyboard (keyboard_mouse_wiiu.cpp, nsyskbd):** HID usage -> Godot Key (US positions), unicode from nsyskbd except for special keys / private-use codes (a05c06f: Shift/Ctrl typed glyphs before; fix built, not re-tested), echo repeat after 500 ms at 30/s. Typing in a LineEdit works (1c4f:008f keyboard). KBD_LIB logged "kbd_init_keyboard : HIDGetIdle failed" for that keyboard but it worked.
- **USB mouse (nsyshid):** boot-interface mice (subclass 1, protocol 2) get SetProtocol(boot) and continuous 64-byte HIDReads (8-byte reads + stopping on the first error made a 18f8:0f99 receiver reset every 1-3 s; fixed in 25ff2d1). Cursor drawn by RasterizerWiiU on both screens (last_blit per screen); DisplayServerWiiU mouse mode/position/buttons. First reports and read errors are logged ([input] mouse ...). Seen once: the mouse was torn down by the system ~2.5 min after a keyboard was plugged into the other front port, came back after replugging. Not chased (user, 2026-11-15: later; the console is across the room).
- **Microphone (audio_driver_wiiu.cpp, mic.h):** input_start opens MIC_INSTANCE_0 with a 0x2800-sample ring; the mixer thread moves new samples (assumed 32 kHz mono) into Godot's input buffer, linearly resampled to 48 kHz, both channels. Needs audio/driver/enable_input. "microphone open" logged; level/pitch not checked yet.
- **tests/input_test** (host: tools/input_test, SD: wiiu/apps/input-test): sensor values, tilt ball, LineEdit, button, mic meter.
- Idea (user): an Aroma plugin that turns a USB mouse into GamePad touch input for every app (hook VPADRead, inject tpNormal; cursor needs a present hook). Check for an Aroma port of Maschell's HID to VPAD first.

### 2026-11-15 (later): boot sound for exported apps

- `cafe-export.sh --boot-sound FILE` (any audio; tools/make_btsnd.py converts with ffmpeg) puts `meta/bootSound.btsnd` in the .wuhb. Verified on the console with LVENGINE (Undertale's mus_intronoise.ogg): plays once at launch from the Wii U Menu.
- Needs the patched wuhbtool (platform/wiiu/tools/wuhbtool/: build.sh clones devkitPro/wut-tools at b22de1a, applies wuhbtool-cafe.patch = --boot-sound + stb_image instead of FreeImage, installs to ~/.local/share/cafe-godot/bin/wuhbtool; the original is kept as wuhbtool.devkitpro). The patch is GPL-2.0 like wut-tools.
- Aroma's Homebrew on Menu plugin serves `*.btsnd` (and the splash .tga files) from the bundle's meta/, but only launches a bundle as a "game" (splash screens + boot sound) when it has both bootTvTex and bootDrcTex. Without them it's a system app: no splash, no sound. cafe-export makes both splashes when --boot-sound is given and none are passed: the project's Godot boot splash (application/boot_splash/image) centred at its own size on boot_splash/bg_color, else the icon.
- .btsnd: 8-byte big-endian header (u32 type, s32 loop start) + 48 kHz 16-bit big-endian stereo PCM, max 960000 frames (vgmstream btsnd.c). Type 0 / loop 0 stayed SILENT on the console; type 2 with the loop start in 0.5 s of appended silence plays once (make_btsnd.py default; --loop loops the whole sound).
- The Wii U Menu reads the app list (and so the splash/sound) when it scans the SD card: after uploading a changed .wuhb, open any app (e.g. Health and Safety) and exit it: the relaunched Wii U Menu rescans (no console restart needed).

### 2026-11-15 (later): first 3D renderer

- **RasterizerSceneWiiU (rasterizer_scene_wiiu.cpp, 45ab588; verified on the console 15:38, 60 fps):** replaces the dummy scene renderer. render_buffers_create returns RenderSceneBuffersWiiU (keeps the viewport render target); render_scene binds it, clears it with the environment background (clear colour / colour x energy; sky = a fixed fallback colour, the sky isn't drawn; canvas/keep: no clear), clears depth, then draws every surface of the visible meshes and 3D multimeshes: opaque first (blending off), then transparent back to front (view depth of the surface AABB centre), depth write off for those unless depth_draw_always.
- **One built-in shader**, compiled with CafeGLSL at the first 3D frame (`[3d] 3D shader compiled` in the log; without cafeglsl.rpl 3D only clears). Registers: vertex mvp/model/normal_matrix (one column per register)/uv1; pixel albedo/ambient/params/flags/dir_*/omni_*0-3. Linear lighting: sRGB textures, albedo and light colours converted, output pow(1/2.2), no tone mapping. Lambert only (Godot's light energy includes PI, which cancels Lambert's 1/PI). Godot's omni attenuation formula.
- **Materials:** spatial shader code isn't compiled. MaterialStorageWiiU::shader_set_code parses spatial shaders into SpatialShader (render_mode: unshaded, cull_*, blend_*, depth_test_disabled, depth_draw_*; from StandardMaterial3D's generated code: `ALPHA *=` = transparent, ALPHA_SCISSOR_THRESHOLD / ALPHA_HASH_SCALE = alpha scissor, `albedo_tex *= COLOR` = vertex colour, the texture_albedo sampler line's filter/repeat hints). get_spatial_material reads the params albedo, texture_albedo, uv1_scale/offset, alpha_scissor_threshold. Material order: instance override, instance surface material, mesh surface material.
- **Lights (LightStorageWiiU):** directional/omni/spot lights are real RIDs now (type, colour, params, negative, cull mask, Dependency); UtilitiesWiiU reports INSTANCE_LIGHT, frees them and tracks their dependency. Light instances keep the transform. render_scene uses the first directional light and the first four omni lights in p_lights (scene-wide, not per object); spot lights are ignored.
- **GX2 (wiiu_gx2.c):** WiiUVertex3D (pos, normal, uv, colour; 48 bytes) meshes in MEM2 with u16/u32 big-endian indices (wiiu_gx2_mesh_create/free); a FLOAT_R32 depth buffer per render target, made by wiiu_gx2_clear_depth and re-bound in apply_target (GX2SetContextState drops it); wiiu_gx2_draw_mesh (register mode, LEQUAL, GX2SetRasterizerClipControlEx(TRUE, TRUE, FALSE) = GL clip z -1..1, front face CW). program_compile takes a vertex layout (2D or 3D); the blend switch is set_blend(). 2D draws still turn depth off every time. The render test re-exported with this template looks unchanged on the console (15:41): no 2D regression.
- **Front face:** GX2_FRONT_FACE_CW with Godot's GL-style projection and no y flip is right: in the 3D test "cull back" shows solid objects, "front" shows their insides (and hides the single-sided floor).
- **Meshes:** surfaces with Vector3 vertices (triangles or lines) also get a GPU copy (Surface.gpu, Surface.aabb); mesh_get_aabb returns 3D bounds when there are any; 3D multimesh AABB = mesh AABB moved by each instance (a0de628).
- **tests/scene3d_test** (host tools/scene3d_test, SD wiiu/apps/scene3d-test, pack on SD): spinning box, sphere behind it, checker floor (uv1_scale 4), transparent box, unshaded cylinder, SurfaceTool vertex-colour triangle, 6-cube MultiMesh, red omni, label on top; left stick orbits, right stick zooms, X cycles the cull mode. `Image.create()` is deprecated and missing in deprecated=no builds (the first upload was a grey screen: parse error); use create_empty.
- .text 30.77 MB (+0.05 MB).
- **Known gaps / next:** imported meshes (.glb/.mesh vertex bytes are little-endian and not swapped, see section 18), spot lights, specular, shadows, sky, fog/tone mapping, spatial shader code (ShaderCompiler, like canvas), skeletons/blend shapes, Sprite3D/Label3D (they update through mesh_surface_update_vertex_region, still a dummy no-op), 3D physics (physics_3d is off in cafe.py). Vertex colours are always converted from sRGB, but Godot treats them as linear unless vertex_color_is_srgb (midtones come out too dark). wiiu_gx2_mesh_free calls GX2DrawDone, so meshes rebuilt every frame (ImmediateMesh) would stall; free them a couple of frames later instead. With no environment the ambient light is black. Multimesh AABBs aren't refreshed when instance transforms change after the instance is in the scene.
- **Slimming (user, 2026-11-15): don't remove classes/nodes** (no build profile). Left: GCC LTO (detect.py says "LTO would need Clang's linker plugin", but devkitPPC is GCC; untested), per-game modules. Linked code by class: _bind_methods 4.6 MB, RenderingDevice + RD* 0.95 MB, *Extension classes 1.05 MB.

### 2026-11-15 (evening): LTO, 3D physics, compressed textures

- **LTO works (d90ff3c, verified on the console: render test unchanged at 60 fps, 3D tests, LVENGINE launches):** `wiiu_lto=thin|full` (detect.py). The old comment ("LTO would need Clang's linker plugin") was wrong: devkitPPC's GNU ld supports `-plugin`, and the host's LLVMgold.so (clang -print-file-name, else /usr/lib) works with it. Clang objects get `-flto`/`-flto=thin`; the link passes `-plugin-opt=mcpu=750`, `O2`, and the codegen flags that aren't IR attributes: `-emulated-tls`, `-use-ctors` (= -fno-use-init-array; without it static constructors run in reverse, see the clang_target comment), `-function-sections`, `-data-sections`, `jobs=8`. llvm-ar/llvm-ranlib for the archives; objects/libs get `.lto.o`/`.lto.a` so switching keeps the other build's objects. Full build ~7 min on the VM, no memory trouble (7 GB).
- .text: no LTO 30.77 MB, thin 29.69, full 28.98 (-1.79 MB).
- **3D physics (a6e394f):** cafe.py has `disable_physics_3d = "no"` and `module_godot_physics_3d_enabled = "yes"` (+2.12 MB with LTO). Jolt isn't built; projects set to Jolt fall back to Godot Physics 3D. Verified: the user's 3D FPS test project (CharacterBody3D, StaticBody3D, Area3D) loads and runs. **cafe.py now needs wiiu_lto=full** (31.11 MB without it it would be ~33 MB).
- **Compressed textures (349d27a):** textures used in 3D are imported VRAM-compressed (S3TC/BPTC) by default; TextureStorageWiiU::_upload already called decompress() but no decompressor was built ("Cannot convert to (or from) compressed formats", garbage stripes). `module_bcdec_enabled` in cafe.py, and thirdparty/misc/bcdec.h patched for big-endian (thirdparty/misc/patches/bcdec-0001-big-endian.patch: little-endian loads of the block words and bitstreams, byte-wise RGBA store). Verified on the console. ETC2/ASTC (mobile imports) still aren't decoded. Native BC1-3 on GX2 would save memory and load time later (BC7 has no GX2 format).
- .text now 31.11 MB (0.89 MB left).
- **The 32 MB limit:** PowerPC `bl` reaches +-32 MB (26-bit displacement). Past that GNU ld adds long-branch stubs (lis/addi/mtctr/bctr with absolute addresses); the guess (untested) is that --emit-relocs doesn't emit relocations for linker-generated stubs, so elf2rpl/the loader don't relocate them and they jump to the link-time address (the __cxa_atexit stub misbehaving in section "CafeGLSL can't be linked"). Check with readelf -r on a >32 MB ELF. Fixes if right: -mlongcall on a few files, a post-link pass adding the relocations, or teaching elf2rpl the stub pattern. Nothing public found about it (homebrew is far smaller).
- Other size ideas (user: don't remove classes/nodes): -Oz, LLVM merge-functions at LTO (`-plugin-opt=-enable-merge-functions`), template_release, third-party libraries as RPLs like cafeglsl.rpl.
- cafe-export: projects without config/icon get the Godot logo (platform/wiiu/tools/default_icon.png, from editor/icons/DefaultProjectIcon.svg; 649df17).
- tests/scene3d_test: left stick moves, right stick looks, X cycles the cull mode.
- Uploads: only the running app's own files are unsafe to overwrite; other apps' files can be uploaded while something runs. After uploading, opening and closing any app (Health and Safety) makes the Wii U Menu rescan.
- Cemu: copied the 3D test and cafeglsl.rpl to ~/.local/share/Cemu/sdcard; the 3D test didn't work there (not looked into; the `~/wiiu/libs/cafeglsl.rpl` path is resolved by Aroma, maybe not by Cemu).
