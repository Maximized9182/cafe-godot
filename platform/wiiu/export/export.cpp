#include "export.h"

#include "export_plugin.h"

#include "core/object/class_db.h"
#include "editor/export/editor_export.h"

void register_wiiu_exporter_types() {
	GDREGISTER_VIRTUAL_CLASS(EditorExportPlatformWiiU);
}

void register_wiiu_exporter() {
	Ref<EditorExportPlatformWiiU> platform;
	platform.instantiate();
	EditorExport::get_singleton()->add_export_platform(platform);
}
