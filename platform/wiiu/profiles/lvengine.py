# 2D game profile sized for LVENGINE: GDScript, 2D physics, TTF fonts, Ogg Vorbis audio.
# Usage: scons platform=wiiu profile=platform/wiiu/profiles/lvengine.py
modules_enabled_by_default = "no"
module_gdscript_enabled = "yes"
module_godot_physics_2d_enabled = "yes"
module_text_server_fb_enabled = "yes"
module_freetype_enabled = "yes"
module_ogg_enabled = "yes"
module_vorbis_enabled = "yes"
# Imported textures are stored as lossless WebP inside .ctex (Godot 4 default
# unless rendering/textures/lossless_compression/force_png is set).
module_webp_enabled = "yes"
# RegEx (LVENGINE's typewriter label, scripts/utdr-label.gd).
module_regex_enabled = "yes"
disable_3d = "yes"
disable_xr = "yes"
disable_navigation_2d = "yes"
disable_navigation_3d = "yes"
disable_physics_3d = "yes"
# RichTextLabel lives behind advanced GUI, and LVENGINE uses it.
disable_advanced_gui = "no"
minizip = "no"
brotli = "no"
deprecated = "no"
