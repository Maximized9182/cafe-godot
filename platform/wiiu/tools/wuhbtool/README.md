# Patched wuhbtool

`wuhbtool-cafe.patch` changes devkitPro's [wuhbtool](https://github.com/devkitPro/wut-tools)
(GPL-2.0; the patch is under the same license):

- `--boot-sound=FILE.btsnd`: adds `meta/bootSound.btsnd`, which the Wii U Menu plays when the
  app starts (served from the bundle by Aroma's Homebrew on Menu plugin).
- Images are loaded with [stb_image](https://github.com/nothings/stb) instead of FreeImage.

`build.sh` clones wut-tools at a pinned commit, applies the patch, compiles and installs it as
`~/.local/share/cafe-godot/bin/wuhbtool`. `tools/make_btsnd.py` makes a `.btsnd` from any audio
(ffmpeg); `cafe-export.sh --boot-sound` does both steps.
