#!/usr/bin/env bash
# Bundle a Wii U export template and an exported .pck into a .wuhb for Aroma.
#
# Usage:
#   make_wuhb.sh --rpx godot.rpx --pck game.pck --out MyGame.wuhb \
#                --name "My Game" [--short-name "MyGame"] [--author "Me"] \
#                [--icon icon.png] [--tv-image tv.png] [--drc-image drc.png]
#
# Image sizes expected by the Wii U menu: icon 128x128, TV splash 1280x720,
# GamePad (DRC) splash 854x480.
set -euo pipefail

DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
WUHBTOOL="$DEVKITPRO/tools/bin/wuhbtool"

rpx="" pck="" out="" name="" short_name="" author="" icon="" tv="" drc=""
while [[ $# -gt 0 ]]; do
	case "$1" in
		--rpx) rpx="$2"; shift 2 ;;
		--pck) pck="$2"; shift 2 ;;
		--out) out="$2"; shift 2 ;;
		--name) name="$2"; shift 2 ;;
		--short-name) short_name="$2"; shift 2 ;;
		--author) author="$2"; shift 2 ;;
		--icon) icon="$2"; shift 2 ;;
		--tv-image) tv="$2"; shift 2 ;;
		--drc-image) drc="$2"; shift 2 ;;
		-h|--help) sed -n '2,10p' "$0"; exit 0 ;;
		*) echo "Unknown option: $1" >&2; exit 1 ;;
	esac
done

[[ -n "$rpx" && -n "$pck" && -n "$out" && -n "$name" ]] || { echo "--rpx, --pck, --out and --name are required." >&2; exit 1; }
[[ -f "$rpx" ]] || { echo "No such file: $rpx" >&2; exit 1; }
[[ -f "$pck" ]] || { echo "No such file: $pck" >&2; exit 1; }

content="$(mktemp -d)"
trap 'rm -rf "$content"' EXIT
# Export templates look for <executable basename>.pck next to the executable.
# OS_WiiU::get_executable_path() is fs:/vol/content/godot.rpx, so the engine
# loads fs:/vol/content/godot.pck (no --main-pack: 4.7 templates reject it
# unless built with disable_path_overrides=no).
cp "$pck" "$content/godot.pck"

args=("$rpx" "$out" "--content=$content" "--name=$name" "--short-name=${short_name:-$name}")
[[ -n "$author" ]] && args+=("--author=$author")
[[ -n "$icon" ]] && args+=("--icon=$icon")
[[ -n "$tv" ]] && args+=("--tv-image=$tv")
[[ -n "$drc" ]] && args+=("--drc-image=$drc")

"$WUHBTOOL" "${args[@]}"
echo "Wrote $out"
