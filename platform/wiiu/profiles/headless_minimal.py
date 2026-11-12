# Smallest useful build for bring-up: GDScript only, no 3D, no GPU backend.
# Usage: scons platform=wiiu profile=platform/wiiu/profiles/headless_minimal.py
modules_enabled_by_default = "no"
module_gdscript_enabled = "yes"
disable_3d = "yes"
disable_xr = "yes"
disable_navigation_2d = "yes"
disable_navigation_3d = "yes"
disable_physics_3d = "yes"
disable_advanced_gui = "yes"
minizip = "no"
brotli = "no"
deprecated = "no"
