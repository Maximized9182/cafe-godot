# General 2D game profile (LVENGINE, Zheeometry Jash, funkin_godot): GDScript, 2D physics,
# TTF fonts, Ogg/MP3 audio, 3D nodes without a 3D renderer.
# Usage: scons platform=wiiu profile=platform/wiiu/profiles/cafe.py
modules_enabled_by_default = "no"
module_gdscript_enabled = "yes"
module_godot_physics_2d_enabled = "yes"
# 3D physics (CharacterBody3D, RigidBody3D, Area3D...): Godot Physics 3D. Jolt isn't built;
# projects set to Jolt fall back to it.
module_godot_physics_3d_enabled = "yes"
module_text_server_fb_enabled = "yes"
module_freetype_enabled = "yes"
# MSDF fonts (FontFile.multichannel_signed_distance_field): glyphs become distance
# fields at runtime; drawn with shaders/msdf.gsh.
module_msdfgen_enabled = "yes"
module_ogg_enabled = "yes"
module_vorbis_enabled = "yes"
# Imported textures are stored as lossless WebP inside .ctex (Godot 4 default
# unless rendering/textures/lossless_compression/force_png is set).
module_webp_enabled = "yes"
# Decompresses VRAM-compressed (S3TC/BPTC, BC1-7) textures, which 3D textures are
# imported as by default; thirdparty/misc/bcdec.h is patched for big-endian.
module_bcdec_enabled = "yes"
# RegEx (LVENGINE's typewriter label, scripts/utdr-label.gd).
module_regex_enabled = "yes"
# MP3 music (AudioStreamMP3), common in games.
module_mp3_enabled = "yes"
# 3D nodes exist (scripts that name 3D classes compile) but draw nothing.
disable_3d = "no"
disable_xr = "yes"
disable_navigation_2d = "yes"
disable_navigation_3d = "yes"
disable_physics_3d = "no"
# RichTextLabel lives behind advanced GUI, and LVENGINE uses it.
disable_advanced_gui = "no"
minizip = "no"
# Godot's built-in default font is WOFF2, which needs brotli; without it every
# glyph of the default font draws as a hex box.
brotli = "yes"
deprecated = "no"
# AudioStreamSynchronized / AudioStreamPlaylist / AudioStreamInteractive
# (funkin_godot layers song stems with AudioStreamSynchronized).
module_interactive_music_enabled = "yes"
