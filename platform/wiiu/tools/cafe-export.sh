#!/usr/bin/env bash
# cafe-export: export a Godot 4.7 project as a Wii U app (.wuhb) for Cafe Godot.
#
# 1. Exports the project's .pck with your Godot editor (--headless --export-pack).
# 2. Packs it with the Cafe Godot template (.rpx) into a .wuhb with wuhbtool.
# 3. Optionally uploads it to the Wii U's SD card over FTP (wiiu/apps/).
#
# Usage:
#   cafe-export <project-dir> [output.wuhb] [options]
#
# Options:
#   -p, --preset NAME      Export preset whose resources/filters are used (any platform;
#                          default: the first preset in export_presets.cfg)
#   -n, --name NAME        App name on the Wii U Menu (default: config/name)
#   -s, --short-name NAME  Short name (default: the name)
#   -a, --author NAME      Author (default: "Cafe Godot")
#   -i, --icon FILE        Icon, scaled to 128x128 (default: the project icon)
#       --tv-image FILE    TV splash, fitted to 1280x720 on black
#       --drc-image FILE   GamePad splash, fitted to 854x480 on black
#   -t, --template FILE    Cafe Godot template .rpx
#                          (default: $CAFE_GODOT_HOME/templates/4.7.2/wiiu_template_debug.rpx)
#   -u, --upload HOST      Upload to the Wii U at HOST (FTP, empty credentials)
#       --godot PATH       Godot editor binary (default: $GODOT or "godot")
#   -h, --help             Show this help
#
# Environment: CAFE_GODOT_HOME (default ~/.local/share/cafe-godot) holds templates/ and
# bin/wuhbtool (from devkitPro's wut-tools).

set -euo pipefail

CAFE_HOME="${CAFE_GODOT_HOME:-$HOME/.local/share/cafe-godot}"
GODOT_BIN="${GODOT:-godot}"
WUHBTOOL="$CAFE_HOME/bin/wuhbtool"
TEMPLATE="$CAFE_HOME/templates/4.7.2/wiiu_template_debug.rpx"
TEMPLATE_VERSION="4.7"

usage() { sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'; }
die() { echo "cafe-export: $*" >&2; exit 1; }

project="" out="" preset="" name="" short_name="" author="Cafe Godot"
icon="" tv="" drc="" upload=""
while [[ $# -gt 0 ]]; do
	case "$1" in
		-p|--preset) preset="$2"; shift 2 ;;
		-n|--name) name="$2"; shift 2 ;;
		-s|--short-name) short_name="$2"; shift 2 ;;
		-a|--author) author="$2"; shift 2 ;;
		-i|--icon) icon="$2"; shift 2 ;;
		--tv-image) tv="$2"; shift 2 ;;
		--drc-image) drc="$2"; shift 2 ;;
		-t|--template) TEMPLATE="$2"; shift 2 ;;
		-u|--upload) upload="$2"; shift 2 ;;
		--godot) GODOT_BIN="$2"; shift 2 ;;
		-h|--help) usage; exit 0 ;;
		-*) die "unknown option $1 (see --help)" ;;
		*)
			if [[ -z "$project" ]]; then project="$1"
			elif [[ -z "$out" ]]; then out="$1"
			else die "too many arguments (see --help)"; fi
			shift ;;
	esac
done

[[ -n "$project" ]] || { usage; exit 1; }
project="$(cd "$project" && pwd)"
[[ -f "$project/project.godot" ]] || die "no project.godot in $project"
[[ -x "$WUHBTOOL" ]] || die "wuhbtool not found at $WUHBTOOL"
[[ -f "$TEMPLATE" ]] || die "template not found at $TEMPLATE"
command -v "$GODOT_BIN" >/dev/null || die "Godot editor '$GODOT_BIN' not found (use --godot)"

version="$("$GODOT_BIN" --version 2>/dev/null | tail -1)"
[[ "$version" == "$TEMPLATE_VERSION".* ]] ||
	echo "cafe-export: warning: editor is $version, template is for $TEMPLATE_VERSION.x; the pack may not load." >&2

# A key's value from project.godot ("key=value" or key="value").
project_value() {
	# "|" as the sed delimiter: keys contain "/" (config/name).
	grep -m1 "^$1=" "$project/project.godot" | sed -e "s|^$1=||" -e 's/^"//' -e 's/"$//' || true
}

[[ -n "$name" ]] || name="$(project_value config/name)"
[[ -n "$name" ]] || name="$(basename "$project")"
[[ -n "$short_name" ]] || short_name="$name"
[[ -n "$out" ]] || out="$PWD/$(echo "$name" | tr -c 'A-Za-z0-9._-' '_' | sed 's/_*$//').wuhb"

if [[ -z "$preset" ]]; then
	[[ -f "$project/export_presets.cfg" ]] ||
		die "no export_presets.cfg: add any export preset in the editor (e.g. Linux), or use --preset"
	preset="$(grep -m1 '^name=' "$project/export_presets.cfg" | sed -e 's/^name=//' -e 's/^"//' -e 's/"$//')"
	[[ -n "$preset" ]] || die "no preset found in export_presets.cfg"
fi

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/content"

echo "cafe-export: exporting \"$name\" with preset \"$preset\"..."
if ! "$GODOT_BIN" --headless --path "$project" --export-pack "$preset" "$work/content/godot.pck" > "$work/export.log" 2>&1 ||
		[[ ! -s "$work/content/godot.pck" ]]; then
	tail -20 "$work/export.log" >&2
	die "pack export failed (full log above)"
fi

# Scales an image to WxH: pixel-perfect for small pixel art, fitted on black otherwise.
fit_image() { # src dst W H
	local src="$1" dst="$2" w="$3" h="$4" sw
	sw="$(magick identify -format '%w' "$src[0]" 2>/dev/null || echo 0)"
	if [[ "$w" == 128 && "$sw" -gt 0 && "$sw" -le 64 ]]; then
		magick "$src[0]" -filter point -resize "${w}x${h}" -background none -gravity center -extent "${w}x${h}" "PNG32:$dst"
	else
		magick "$src[0]" -resize "${w}x${h}" -background black -gravity center -extent "${w}x${h}" "PNG32:$dst"
	fi
}

args=("$TEMPLATE" "$work/out.wuhb" "--content=$work/content" "--name=$name" "--short-name=$short_name" "--author=$author")

if [[ -z "$icon" ]]; then
	res_icon="$(project_value config/icon)"
	if [[ "$res_icon" == uid://* ]]; then
		# The file whose .import (or .uid) records that UID.
		ref="$(grep -rlF --include='*.import' --include='*.uid' --exclude-dir=.godot "$res_icon" "$project" | head -1 || true)"
		[[ -n "$ref" ]] && icon="${ref%.import}" && icon="${icon%.uid}"
	elif [[ "$res_icon" == res://* ]]; then
		icon="$project/${res_icon#res://}"
	fi
	[[ -n "$res_icon" && -z "$icon" ]] && echo "cafe-export: warning: couldn't find the project icon $res_icon." >&2
fi
if [[ -n "$icon" ]]; then
	if command -v magick >/dev/null && fit_image "$icon" "$work/icon.png" 128 128 2>/dev/null; then
		args+=("--icon=$work/icon.png")
	else
		echo "cafe-export: warning: couldn't convert icon $icon; packaging without one." >&2
	fi
fi
if [[ -n "$tv" ]]; then
	fit_image "$tv" "$work/tv.png" 1280 720 && args+=("--tv-image=$work/tv.png")
fi
if [[ -n "$drc" ]]; then
	fit_image "$drc" "$work/drc.png" 854 480 && args+=("--drc-image=$work/drc.png")
fi

"$WUHBTOOL" "${args[@]}" > "$work/wuhb.log" 2>&1 || { cat "$work/wuhb.log" >&2; die "wuhbtool failed"; }
mv "$work/out.wuhb" "$out"
echo "cafe-export: wrote $out ($(du -h "$out" | cut -f1))"

if [[ -n "$upload" ]]; then
	echo "cafe-export: uploading to $upload..."
	if curl -sS --connect-timeout 10 -u : -T "$out" "ftp://$upload:21/fs/vol/external01/wiiu/apps/"; then
		echo "cafe-export: uploaded to sd:/wiiu/apps/$(basename "$out")"
	else
		die "upload failed (an app that's running can't be replaced; close it first)"
	fi
fi
