#pragma once

#include "drivers/unix/dir_access_unix.h"

// DirAccessUnix fixed for Cafe OS (wut's devoptab):
//
// - change_dir: wut's chdir() doesn't check that the directory exists, so
//   DirAccessUnix::change_dir (and DirAccess::exists, which is built on it)
//   reported every path as existing. OS::ensure_user_data_dir then never
//   created user://, and saves failed. Check with stat (dir_exists) first.
//
// - make_dir_recursive:
// Godot's DirAccess::make_dir_recursive treats "fs:/" as a drive prefix and
// creates every level from there ("fs:/vol", "fs:/vol/external01", ...).
// Creating "fs:/vol" fails on Cafe OS with an error other than "already
// exists", so Godot couldn't create user:// (fs:/vol/external01/wiiu/godot/...)
// and every save failed. Paths on the SD card start from its root instead.
class DirAccessWiiU : public DirAccessUnix {
	GDSOFTCLASS(DirAccessWiiU, DirAccessUnix);

public:
	static constexpr const char *SD_ROOT = "fs:/vol/external01";

	// Cafe OS's stat may reject a directory path ending in "/" (e.g.
	// DirAccess.dir_exists_absolute("user://mus/")); check it without.
	bool dir_exists(String p_dir) override {
		while (p_dir.length() > 1 && p_dir.ends_with("/") && !p_dir.ends_with(":/") && !p_dir.ends_with("://")) {
			p_dir = p_dir.substr(0, p_dir.length() - 1);
		}
		return DirAccessUnix::dir_exists(p_dir);
	}

	Error change_dir(String p_dir) override {
		// Resolve ".." etc. first: wut's stat doesn't.
		String check = p_dir;
		if (check.is_relative_path()) {
			check = get_current_dir().path_join(check);
		}
		if (!dir_exists(check.simplify_path())) {
			return ERR_INVALID_PARAMETER;
		}
		return DirAccessUnix::change_dir(p_dir);
	}

	Error make_dir_recursive(const String &p_dir) override {
		String dir = p_dir.replace_char('\\', '/');
		const String root = String(SD_ROOT) + "/";
		if (!dir.begins_with(root)) {
			return DirAccessUnix::make_dir_recursive(p_dir);
		}
		const Vector<String> parts = dir.substr(root.length()).simplify_path().split("/", false);
		String current = SD_ROOT;
		for (int i = 0; i < parts.size(); i++) {
			current = current.path_join(parts[i]);
			Error err = make_dir(current);
			if (err != OK && err != ERR_ALREADY_EXISTS) {
				ERR_FAIL_V_MSG(err, vformat("Could not create directory: '%s'.", current));
			}
		}
		return OK;
	}
};
