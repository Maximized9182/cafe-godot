#pragma once

#include "editor/export/editor_export_platform.h"

class ImageTexture;

// Cafe Godot: exports a project as a Wii U app (.wuhb): the project pack goes
// into the bundle (or onto the SD card), next to the Cafe Godot template
// (.rpx), packed with wuhbtool. Same job as tools/cafe-export.sh, from the
// editor's Export dialog.
class EditorExportPlatformWiiU : public EditorExportPlatform {
	GDCLASS(EditorExportPlatformWiiU, EditorExportPlatform);

	Ref<ImageTexture> logo;

	String _find_template(const Ref<EditorExportPreset> &p_preset, bool p_debug) const;
	String _find_wuhbtool(const Ref<EditorExportPreset> &p_preset) const;
	String _app_name(const Ref<EditorExportPreset> &p_preset) const;
	Ref<Image> _project_icon(const Ref<EditorExportPreset> &p_preset) const;
	static Ref<Image> _fit_image(const Ref<Image> &p_src, int p_width, int p_height);

public:
	virtual void get_preset_features(const Ref<EditorExportPreset> &p_preset, List<String> *r_features) const override;
	virtual void get_export_options(List<ExportOption> *r_options) const override;

	virtual String get_name() const override { return "Wii U"; }
	virtual String get_os_name() const override { return "Wii U"; }
	virtual Ref<Texture2D> get_logo() const override;

	virtual bool has_valid_export_configuration(const Ref<EditorExportPreset> &p_preset, String &r_error, bool &r_missing_templates, bool p_debug = false) const override;
	virtual bool has_valid_project_configuration(const Ref<EditorExportPreset> &p_preset, String &r_error) const override { return true; }
	virtual List<String> get_binary_extensions(const Ref<EditorExportPreset> &p_preset) const override;
	virtual Error export_project(const Ref<EditorExportPreset> &p_preset, bool p_debug, const String &p_path, BitField<EditorExportPlatform::DebugFlags> p_flags = 0, bool p_notify = true) override;

	virtual void get_platform_features(List<String> *r_features) const override {
		r_features->push_back("wiiu");
		r_features->push_back("cafe");
	}
	virtual void resolve_platform_feature_priorities(const Ref<EditorExportPreset> &p_preset, HashSet<String> &p_features) override {}

	virtual void initialize() override;
};
