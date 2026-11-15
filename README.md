# Cafe Godot

A Wii U port of [Godot Engine](https://godotengine.org) 4.7.2, for consoles running
[Aroma](https://aroma.foryour.cafe). It adds a `wiiu` platform (`platform/wiiu/`) that builds
Godot's export templates as a Wii U executable (`.rpx`). Games made in the regular Godot editor
are packed into a `.wuhb` with the template and run on the console.

It is not an emulator or a reimplementation: it's Godot itself, compiled for the Wii U's
PowerPC CPU, with new backends for the display, GPU (GX2), input, audio (AX), file system and
the HOME menu.

> Status: early but playable. Several full 2D games run, including a [Friday Night Funkin'
> engine](https://github.com/cherrythecool/funkin_godot) and an [Undertale-style RPG engine](https://github.com/Maximized9182/LVENGINE). Expect rough edges.

## What works

- **2D rendering on GX2**: sprites, text (TTF fonts), nine-patches, polygons, 2D meshes and
  multimeshes, `CPUParticles2D` and `GPUParticles2D` (drawn through an internal
  `CPUParticles2D`, so only `ParticleProcessMaterial` particles), `Parallax2D`, animated textures, and the canvas blend modes
  (mix, add, subtract, multiply, premultiplied alpha), and mipmaps for textures imported with
  them (the `*_WITH_MIPMAPS` filters). The same image goes to the TV and the GamePad.
- **MSDF fonts** (`FontFile.multichannel_signed_distance_field`), with outlines.
- **Input**: Wii U GamePad (buttons, sticks, touchscreen), Wii Remote (with Nunchuk or Classic
  Controller) and Pro Controller, as Godot joypads. Button 0 is A, as on an Xbox pad.
- **Audio** through AX, with Ogg Vorbis, MP3 and WAV.
- **GDScript**, 2D physics, `RichTextLabel`, RegEx, interactive music
  (`AudioStreamSynchronized` and friends).
- **Saves**: `user://` is `sd:/wiiu/godot/<project user dir>/`.
- **HOME menu**, closing from HOME and the Aroma close plugin.
- **Crash screen** with the error and a backtrace on both screens.

## What doesn't (yet)

- **3D**: 3D nodes exist, so scripts that use them compile, but nothing is drawn.
- **Shaders, partly**: see "Canvas shaders" below. Shaders with SDF, global/instance uniforms,
  uniform arrays or textures read in `vertex()` draw without the shader (blend mode only).
- Networking (`HTTPRequest`, multiplayer), the editor itself.
- Known issue: rarely, pressing HOME freezes the console in some games. Still being
  investigated.

## Canvas shaders

`canvas_item` shaders work: Godot's shader compiler turns them into GLSL, and
[CafeGLSL](https://github.com/Exzap/CafeGLSL) compiles that for GX2 on the console when the
shader loads (a few ms each). `fragment()` and `vertex()`, uniforms, `set_shader_parameter`,
`sampler2D` parameters, varyings, the screen texture (`hint_screen_texture`, and `BackBufferCopy`
nodes), `TIME`, `TEXTURE`, `UV`, `COLOR`, `SCREEN_UV` and friends are supported. `light()` is
ignored.

This needs `cafeglsl.rpl` on the SD card at `sd:/wiiu/libs/cafeglsl.rpl`. Build it with
`platform/wiiu/cafeglsl_rpl/build.sh` from CafeGLSL's Wii U release (the folder with
`lib/libcafeglsl.a`). Without it, shaders draw as before and MSDF text uses a precompiled shader.

## Building the export template

Requirements, on Linux:

- [devkitPro](https://devkitpro.org/wiki/Getting_Started) with `wut` and `wut-tools`
  (`wuhbtool`), at `/opt/devkitpro` or `$DEVKITPRO`.
- `clang`: the platform is compiled with Clang, which provides the thread-local storage wut
  lacks. Linking still uses devkitPPC's GCC, `wut.specs`, newlib and libstdc++.
- Python 3 and SCons.

```bash
export DEVKITPRO=/opt/devkitpro
export DEVKITPPC=$DEVKITPRO/devkitPPC
export PATH=$DEVKITPRO/tools/bin:$DEVKITPPC/bin:$PATH
scons platform=wiiu profile=platform/wiiu/profiles/cafe.py -j8
```

This produces `bin/godot.wiiu.template_debug.ppc32.rpx`. `profiles/cafe.py` is the general 2D
profile, with the modules most 2D games need. Pass `wiiu_udp_log=no` to stop sending engine
output over the network (it goes to `udplogserver` on your PC by default).

## Exporting a game

`platform/wiiu/tools/cafe-export.sh` exports a project and packs it into a `.wuhb` with an icon
and a name on the Wii U Menu:

1. Put the template at `~/.local/share/cafe-godot/templates/4.7.2/wiiu_template_debug.rpx` and
   `wuhbtool` at `~/.local/share/cafe-godot/bin/wuhbtool`, or set `CAFE_GODOT_HOME`.
2. Run it with a Godot 4.7.2 editor on your `PATH`, or set `GODOT`:

```bash
platform/wiiu/tools/cafe-export.sh path/to/project mygame.wuhb --name "My Game" --upload 192.168.1.x
```

- `--upload HOST` copies the result to `sd:/wiiu/apps/` over FTP (the `ftpiiu` plugin).
- `--pack-on-sd wiiu/apps/mygame` keeps the game's `.pck` outside the bundle, as a file on the
  SD card. This is recommended for large games.
- `--icon`, `--tv-image` and `--drc-image` set the menu icon and splash screens.
- `--help` lists everything.

Any export preset works. It only decides which resources go into the pack; the template does
the rest.

## On the console

| Combo (on the GamePad) | What it does |
|---|---|
| `ZR` + `R` + `←` + `Y` + `B` + `L` | Stats overlay: fps, frame time graph, CPU/GPU time, memory, draw calls. Drag it to any corner. |
| `ZL` + `L` + `→` + `X` + `Y` + `TV` | Log overlay: engine output on screen. D-pad, `L` and `R` scroll it. |

The OS name and display server are both reported as `"Cafe OS"` (`OS.get_name()`).

## Credits and license

Cafe Godot is based on [Godot Engine](https://github.com/godotengine/godot), © Juan Linietsky,
Ariel Manzur and the Godot Engine contributors, and is released under the same
[MIT license](LICENSE.txt). Godot's original readme is at
[godotengine/godot](https://github.com/godotengine/godot/blob/master/README.md).

Built with [devkitPro](https://devkitpro.org), [wut](https://github.com/devkitPro/wut) and the
[Aroma](https://github.com/wiiu-env) homebrew environment. This is an unofficial project, not
affiliated with the Godot Foundation or Nintendo.
