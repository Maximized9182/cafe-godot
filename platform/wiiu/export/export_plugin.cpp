#include "export_plugin.h"

#include "logo_svg.gen.h"

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/image.h"
#include "core/os/os.h"
#include "core/version.h"
#include "editor/editor_node.h"
#include "editor/editor_string_names.h"
#include "editor/export/editor_export.h"
#include "editor/file_system/editor_paths.h"
#include "editor/themes/editor_scale.h"
#include "scene/resources/image_texture.h"

#include "modules/svg/image_loader_svg.h"

// Where the template and wuhbtool live when nothing is configured (the same
// places tools/cafe-export.sh looks).
static String cafe_home() {
	const String env = OS::get_singleton()->get_environment("CAFE_GODOT_HOME");
	if (!env.is_empty()) {
		return env;
	}
	return OS::get_singleton()->get_environment("HOME").path_join(".local/share/cafe-godot");
}

void EditorExportPlatformWiiU::initialize() {
	if (EditorNode::get_singleton()) {
		Ref<Image> img = memnew(Image);
		const bool upsample = !Math::is_equal_approx(Math::round(EDSCALE), EDSCALE);
		ImageLoaderSVG::create_image_from_string(img, _wiiu_logo_svg, EDSCALE, upsample, false);
		logo = ImageTexture::create_from_image(img);
	}
}

Ref<Texture2D> EditorExportPlatformWiiU::get_logo() const {
	return logo;
}

void EditorExportPlatformWiiU::get_preset_features(const Ref<EditorExportPreset> &p_preset, List<String> *r_features) const {
	// The GPU reads S3TC (BCn); the engine decompresses it where needed.
	if (p_preset->get("texture_format/s3tc_bptc")) {
		r_features->push_back("s3tc");
		r_features->push_back("bptc");
	}
	if (p_preset->get("texture_format/etc2_astc")) {
		r_features->push_back("etc2");
		r_features->push_back("astc");
	}
}

void EditorExportPlatformWiiU::get_export_options(List<ExportOption> *r_options) const {
	r_options->push_back(ExportOption(PropertyInfo(Variant::STRING, "custom_template/debug", PROPERTY_HINT_GLOBAL_FILE, "*.rpx"), ""));
	r_options->push_back(ExportOption(PropertyInfo(Variant::STRING, "custom_template/release", PROPERTY_HINT_GLOBAL_FILE, "*.rpx"), ""));
	r_options->push_back(ExportOption(PropertyInfo(Variant::STRING, "tools/wuhbtool", PROPERTY_HINT_GLOBAL_FILE), ""));

	r_options->push_back(ExportOption(PropertyInfo(Variant::STRING, "application/name"), ""));
	r_options->push_back(ExportOption(PropertyInfo(Variant::STRING, "application/short_name"), ""));
	r_options->push_back(ExportOption(PropertyInfo(Variant::STRING, "application/author"), "Cafe Godot"));
	r_options->push_back(ExportOption(PropertyInfo(Variant::STRING, "application/icon", PROPERTY_HINT_GLOBAL_FILE, "*.png,*.jpg,*.webp,*.svg"), ""));
	r_options->push_back(ExportOption(PropertyInfo(Variant::STRING, "application/tv_splash", PROPERTY_HINT_GLOBAL_FILE, "*.png,*.jpg,*.webp"), ""));
	r_options->push_back(ExportOption(PropertyInfo(Variant::STRING, "application/gamepad_splash", PROPERTY_HINT_GLOBAL_FILE, "*.png,*.jpg,*.webp"), ""));
	r_options->push_back(ExportOption(PropertyInfo(Variant::STRING, "application/boot_sound", PROPERTY_HINT_GLOBAL_FILE, "*.btsnd"), ""));

	r_options->push_back(ExportOption(PropertyInfo(Variant::BOOL, "binary_format/pack_on_sd"), false));
	r_options->push_back(ExportOption(PropertyInfo(Variant::STRING, "binary_format/sd_folder"), ""));

	r_options->push_back(ExportOption(PropertyInfo(Variant::BOOL, "texture_format/s3tc_bptc"), true));
	r_options->push_back(ExportOption(PropertyInfo(Variant::BOOL, "texture_format/etc2_astc"), false));
}

List<String> EditorExportPlatformWiiU::get_binary_extensions(const Ref<EditorExportPreset> &p_preset) const {
	List<String> list;
	list.push_back("wuhb");
	return list;
}

String EditorExportPlatformWiiU::_find_template(const Ref<EditorExportPreset> &p_preset, bool p_debug) const {
	String custom = String(p_preset->get(p_debug ? "custom_template/debug" : "custom_template/release")).strip_edges();
	if (!custom.is_empty()) {
		return FileAccess::exists(custom) ? custom : String();
	}
	// The installed export templates (wiiu_debug.rpx / wiiu_release.rpx), then
	// the Cafe Godot home (the debug template; there is no release one yet).
	String err;
	const String installed = find_export_template(p_debug ? "wiiu_debug.rpx" : "wiiu_release.rpx", &err);
	if (!installed.is_empty() && FileAccess::exists(installed)) {
		return installed;
	}
	const String home = cafe_home().path_join("templates").path_join(vformat("%d.%d.%d", VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH)).path_join("wiiu_template_debug.rpx");
	if (FileAccess::exists(home)) {
		return home;
	}
	return String();
}

String EditorExportPlatformWiiU::_find_wuhbtool(const Ref<EditorExportPreset> &p_preset) const {
	const String configured = String(p_preset->get("tools/wuhbtool")).strip_edges();
	if (!configured.is_empty()) {
		return FileAccess::exists(configured) ? configured : String();
	}
	const String home = cafe_home().path_join("bin").path_join("wuhbtool");
	if (FileAccess::exists(home)) {
		return home;
	}
	// On the PATH.
	const Vector<String> dirs = OS::get_singleton()->get_environment("PATH").split(":", false);
	for (const String &dir : dirs) {
		const String candidate = dir.path_join("wuhbtool");
		if (FileAccess::exists(candidate)) {
			return candidate;
		}
	}
	return String();
}

bool EditorExportPlatformWiiU::has_valid_export_configuration(const Ref<EditorExportPreset> &p_preset, String &r_error, bool &r_missing_templates, bool p_debug) const {
	String err;
	bool valid = true;
	r_missing_templates = false;
	if (_find_template(p_preset, p_debug).is_empty()) {
		err += TTR("No Wii U export template found. Put the Cafe Godot template (wiiu_template_debug.rpx) under the Cafe Godot home (templates/<version>/) or set a custom template.") + "\n";
		r_missing_templates = true;
		valid = false;
	}
	if (_find_wuhbtool(p_preset).is_empty()) {
		err += TTR("wuhbtool not found. Set \"Tools > Wuhbtool\" in the export options, or put it in $CAFE_GODOT_HOME/bin or on the PATH.") + "\n";
		valid = false;
	}
	if (!err.is_empty()) {
		r_error = err;
	}
	return valid;
}

String EditorExportPlatformWiiU::_app_name(const Ref<EditorExportPreset> &p_preset) const {
	String name = String(p_preset->get("application/name")).strip_edges();
	if (name.is_empty()) {
		name = String(get_project_setting(p_preset, "application/config/name")).strip_edges();
	}
	return name.is_empty() ? String("Unnamed") : name;
}

Ref<Image> EditorExportPlatformWiiU::_project_icon(const Ref<EditorExportPreset> &p_preset) const {
	Error err = OK;
	String path = String(p_preset->get("application/icon")).strip_edges();
	if (path.is_empty()) {
		path = String(get_project_setting(p_preset, "application/config/icon")).strip_edges();
	}
	Ref<Image> icon;
	if (!path.is_empty()) {
		icon = _load_icon_or_splash_image(path, &err);
	}
	if (path.is_empty() || err != OK || icon.is_null() || icon->is_empty()) {
		icon = EditorNode::get_singleton()->get_editor_theme()->get_icon(SNAME("DefaultProjectIcon"), EditorStringName(EditorIcons))->get_image();
	}
	return icon;
}

// Scales to fit into WxH (small pixel art by whole numbers is left to the
// caller), centred on black: the Wii U Menu wants exact sizes.
Ref<Image> EditorExportPlatformWiiU::_fit_image(const Ref<Image> &p_src, int p_width, int p_height) {
	Ref<Image> src = p_src->duplicate();
	if (src->is_compressed()) {
		src->decompress();
	}
	src->convert(Image::FORMAT_RGBA8);
	const float scale = MIN((float)p_width / src->get_width(), (float)p_height / src->get_height());
	const int w = MAX(1, (int)Math::round(src->get_width() * scale));
	const int h = MAX(1, (int)Math::round(src->get_height() * scale));
	// Small pixel art (icons up to 64 px) stays crisp.
	src->resize(w, h, src->get_width() <= 64 ? Image::INTERPOLATE_NEAREST : Image::INTERPOLATE_LANCZOS);
	Ref<Image> out = Image::create_empty(p_width, p_height, false, Image::FORMAT_RGBA8);
	out->fill(Color(0, 0, 0, 1));
	out->blend_rect(src, Rect2i(0, 0, w, h), Point2i((p_width - w) / 2, (p_height - h) / 2));
	return out;
}

Error EditorExportPlatformWiiU::export_project(const Ref<EditorExportPreset> &p_preset, bool p_debug, const String &p_path, BitField<EditorExportPlatform::DebugFlags> p_flags, bool p_notify) {
	ExportNotifier notifier(*this, p_preset, p_debug, p_path, p_flags, p_notify);

	const String template_path = _find_template(p_preset, p_debug);
	const String wuhbtool = _find_wuhbtool(p_preset);
	if (template_path.is_empty() || wuhbtool.is_empty()) {
		add_message(EXPORT_MESSAGE_ERROR, TTR("Prepare Templates"), TTR("The Wii U template or wuhbtool is missing (see the export options)."));
		return ERR_FILE_NOT_FOUND;
	}
	const String base_dir = p_path.get_base_dir();
	if (!DirAccess::exists(base_dir)) {
		add_message(EXPORT_MESSAGE_ERROR, TTR("Export"), vformat(TTR("Target folder does not exist or is inaccessible: \"%s\""), base_dir));
		return ERR_FILE_BAD_PATH;
	}

	const String name = _app_name(p_preset);
	String short_name = String(p_preset->get("application/short_name")).strip_edges();
	if (short_name.is_empty()) {
		short_name = name;
	}
	const String author = String(p_preset->get("application/author")).strip_edges();
	const bool pack_on_sd = p_preset->get("binary_format/pack_on_sd");
	String sd_folder = String(p_preset->get("binary_format/sd_folder")).strip_edges().trim_prefix("/").trim_suffix("/");
	if (sd_folder.is_empty()) {
		sd_folder = String("wiiu/apps").path_join(p_path.get_file().get_basename());
	}

	// A clean work folder.
	const String work = EditorPaths::get_singleton()->get_temp_dir().path_join("wiiu_export");
	Ref<DirAccess> da = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);
	if (DirAccess::exists(work)) {
		da->change_dir(work);
		da->erase_contents_recursive();
	}
	da->make_dir_recursive(work.path_join("content"));

	// The pack: in the bundle, or beside the .wuhb (to copy to the SD card).
	const String pck_path = pack_on_sd ? base_dir.path_join("godot.pck") : work.path_join("content").path_join("godot.pck");
	Error err = save_pack(p_preset, p_debug, pck_path);
	if (err != OK) {
		return err; // The message comes from save_pack.
	}
	if (pack_on_sd) {
		// The bundle only says where on the SD card the pack is.
		Ref<FileAccess> f = FileAccess::open(work.path_join("content").path_join("godot.pckdir"), FileAccess::WRITE);
		if (f.is_null()) {
			add_message(EXPORT_MESSAGE_ERROR, TTR("Export"), TTR("Could not write the pack location file."));
			return ERR_CANT_CREATE;
		}
		f->store_string("fs:/vol/external01/" + sd_folder + "\n");
	}

	List<String> args;
	args.push_back(template_path);
	args.push_back(p_path);
	args.push_back("--content=" + work.path_join("content"));
	args.push_back("--name=" + name);
	args.push_back("--short-name=" + short_name);
	args.push_back("--author=" + author);

	// Icon, splash images, boot sound.
	Ref<Image> icon = _fit_image(_project_icon(p_preset), 128, 128);
	icon->save_png(work.path_join("icon.png"));
	args.push_back("--icon=" + work.path_join("icon.png"));

	const String tv_path = String(p_preset->get("application/tv_splash")).strip_edges();
	const String drc_path = String(p_preset->get("application/gamepad_splash")).strip_edges();
	const String sound_path = String(p_preset->get("application/boot_sound")).strip_edges();
	// The Wii U Menu only plays the boot sound (and shows splashes) when both
	// images exist; make them from the icon when only a sound is given.
	const bool want_splash = !tv_path.is_empty() || !drc_path.is_empty() || !sound_path.is_empty();
	if (want_splash) {
		auto splash = [&](const String &p_file, int p_w, int p_h, const String &p_out) {
			Ref<Image> src;
			if (!p_file.is_empty()) {
				Error ierr = OK;
				src = _load_icon_or_splash_image(p_file, &ierr);
				if (ierr != OK) {
					src.unref();
				}
			}
			if (src.is_null() || src->is_empty()) {
				src = _project_icon(p_preset);
				// Centred at 40% of the height, like cafe-export does.
				Ref<Image> small = _fit_image(src, p_h * 2 / 5, p_h * 2 / 5);
				Ref<Image> out = Image::create_empty(p_w, p_h, false, Image::FORMAT_RGBA8);
				out->fill(Color(0, 0, 0, 1));
				out->blit_rect(small, Rect2i(Point2i(), small->get_size()), Point2i((p_w - small->get_width()) / 2, (p_h - small->get_height()) / 2));
				out->save_png(p_out);
				return;
			}
			_fit_image(src, p_w, p_h)->save_png(p_out);
		};
		splash(tv_path, 1280, 720, work.path_join("tv.png"));
		splash(drc_path, 854, 480, work.path_join("drc.png"));
		args.push_back("--tv-image=" + work.path_join("tv.png"));
		args.push_back("--drc-image=" + work.path_join("drc.png"));
	}
	if (!sound_path.is_empty()) {
		args.push_back("--boot-sound=" + sound_path);
	}

	String output;
	int exit_code = 0;
	err = OS::get_singleton()->execute(wuhbtool, args, &output, &exit_code, true);
	if (err != OK || exit_code != 0) {
		add_message(EXPORT_MESSAGE_ERROR, TTR("Export"), vformat(TTR("wuhbtool failed (exit code %d): %s"), exit_code, output.strip_edges()));
		return ERR_CANT_CREATE;
	}
	if (pack_on_sd) {
		add_message(EXPORT_MESSAGE_INFO, TTR("Export"), vformat(TTR("Copy godot.pck (next to the .wuhb) to sd:/%s/ on the Wii U."), sd_folder));
	}
	return OK;
}
