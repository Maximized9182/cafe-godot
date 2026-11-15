#include "rasterizer_wiiu.h"

#include "core/io/image.h"
#include "core/math/math_funcs.h"
#include "core/os/os.h"
#include "scene/resources/texture.h"
#include "servers/rendering/rendering_server_globals.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/rendering_server_default.h"

// ===========================================================================
// MaterialStorageWiiU
// ===========================================================================

MaterialStorageWiiU::MaterialStorageWiiU() {
	wiiu_singleton = this;
}

MaterialStorageWiiU::~MaterialStorageWiiU() {
	LocalVector<RID> shaders;
	for (const KeyValue<RID, CanvasProgram *> &E : shader_programs) {
		shaders.push_back(E.key);
	}
	for (const RID &rid : shaders) {
		_free_canvas_program(rid);
	}
	wiiu_singleton = nullptr;
}

// The blend_* word of a canvas_item shader's render_mode line.
static WiiUBlend blend_from_shader_code(const String &p_code) {
	if (!p_code.contains("canvas_item")) {
		return WIIU_BLEND_MIX;
	}
	int at = p_code.find("render_mode");
	while (at != -1) {
		const int end = p_code.find(";", at);
		if (end == -1) {
			break;
		}
		const String modes = p_code.substr(at + 11, end - at - 11);
		for (const String &m : modes.split(",")) {
			const String mode = m.strip_edges();
			if (mode == "blend_add") {
				return WIIU_BLEND_ADD;
			} else if (mode == "blend_sub") {
				return WIIU_BLEND_SUB;
			} else if (mode == "blend_mul") {
				return WIIU_BLEND_MUL;
			} else if (mode == "blend_premul_alpha") {
				return WIIU_BLEND_PREMUL_ALPHA;
			} else if (mode == "blend_disabled") {
				return WIIU_BLEND_DISABLED;
			}
		}
		at = p_code.find("render_mode", end);
	}
	return WIIU_BLEND_MIX;
}

// The render modes and features of a spatial shader that the 3D renderer
// (rasterizer_scene_wiiu.cpp) follows. StandardMaterial3D's generated code
// is the main case: its transparency, alpha scissor, vertex colour and
// texture filter settings show up as these lines.
static bool spatial_from_shader_code(const String &p_code, MaterialStorageWiiU::SpatialShader &r_shader) {
	if (!p_code.contains("shader_type spatial")) {
		return false;
	}
	MaterialStorageWiiU::SpatialShader sh;
	int at = p_code.find("render_mode");
	while (at != -1) {
		const int end = p_code.find(";", at);
		if (end == -1) {
			break;
		}
		const String modes = p_code.substr(at + 11, end - at - 11);
		for (const String &m : modes.split(",")) {
			const String mode = m.strip_edges();
			if (mode == "unshaded") {
				sh.unshaded = true;
			} else if (mode == "cull_disabled") {
				sh.cull = WIIU_CULL_DISABLED;
			} else if (mode == "cull_front") {
				sh.cull = WIIU_CULL_FRONT;
			} else if (mode == "blend_add") {
				sh.blend = WIIU_BLEND_ADD;
			} else if (mode == "blend_sub") {
				sh.blend = WIIU_BLEND_SUB;
			} else if (mode == "blend_mul") {
				sh.blend = WIIU_BLEND_MUL;
			} else if (mode == "blend_premul_alpha") {
				sh.blend = WIIU_BLEND_PREMUL_ALPHA;
			} else if (mode == "depth_test_disabled") {
				sh.depth_test = false;
			} else if (mode == "depth_draw_never") {
				sh.depth_draw = 0;
			} else if (mode == "depth_draw_always") {
				sh.depth_draw = 2;
			}
		}
		at = p_code.find("render_mode", end);
	}
	sh.alpha_scissor = p_code.contains("ALPHA_SCISSOR_THRESHOLD") || p_code.contains("ALPHA_HASH_SCALE");
	sh.transparent = !sh.alpha_scissor && (p_code.contains("ALPHA *=") || p_code.contains("ALPHA =") || sh.blend != WIIU_BLEND_MIX);
	sh.vertex_color = p_code.contains("albedo_tex *= COLOR");
	const int tex = p_code.find("sampler2D texture_albedo");
	if (tex != -1) {
		const int end = p_code.find(";", tex);
		const String hints = p_code.substr(tex, end == -1 ? -1 : end - tex);
		const bool linear = !hints.contains("filter_nearest");
		const bool mipmaps = hints.contains("mipmap");
		sh.filter = (linear ? WIIU_FILTER_LINEAR : 0) | (mipmaps ? WIIU_FILTER_MIPMAPS : 0);
		sh.repeat = !hints.contains("repeat_disable");
	}
	r_shader = sh;
	return true;
}

// The names drivers/gles3 gives the canvas built-ins (its CanvasShaderData);
// the fragment template below declares the ones a fragment() can use.
void MaterialStorageWiiU::_init_canvas_compiler() {
	if (canvas_compiler_ready) {
		return;
	}
	ShaderCompiler::DefaultIdentifierActions actions;
	actions.renames["VERTEX"] = "vertex";
	actions.renames["LIGHT_VERTEX"] = "light_vertex";
	actions.renames["SHADOW_VERTEX"] = "shadow_vertex";
	actions.renames["UV"] = "uv";
	actions.renames["POINT_SIZE"] = "point_size";
	actions.renames["MODEL_MATRIX"] = "model_matrix";
	actions.renames["CANVAS_MATRIX"] = "canvas_transform";
	actions.renames["SCREEN_MATRIX"] = "screen_transform";
	actions.renames["TIME"] = "time";
	actions.renames["PI"] = String::num(Math::PI);
	actions.renames["TAU"] = String::num(Math::TAU);
	actions.renames["E"] = String::num(Math::E);
	actions.renames["AT_LIGHT_PASS"] = "false";
	actions.renames["INSTANCE_CUSTOM"] = "instance_custom";
	actions.renames["COLOR"] = "color";
	actions.renames["NORMAL"] = "normal";
	actions.renames["NORMAL_MAP"] = "normal_map";
	actions.renames["NORMAL_MAP_DEPTH"] = "normal_map_depth";
	actions.renames["TEXTURE"] = "color_texture";
	actions.renames["TEXTURE_PIXEL_SIZE"] = "color_texture_pixel_size";
	actions.renames["NORMAL_TEXTURE"] = "normal_texture";
	actions.renames["SPECULAR_SHININESS_TEXTURE"] = "specular_texture";
	actions.renames["SPECULAR_SHININESS"] = "specular_shininess";
	actions.renames["SCREEN_UV"] = "screen_uv";
	actions.renames["REGION_RECT"] = "region_rect";
	actions.renames["SCREEN_PIXEL_SIZE"] = "screen_pixel_size";
	actions.renames["FRAGCOORD"] = "gl_FragCoord";
	actions.renames["POINT_COORD"] = "gl_PointCoord";
	actions.renames["INSTANCE_ID"] = "gl_InstanceID";
	actions.renames["VERTEX_ID"] = "gl_VertexID";
	actions.renames["CUSTOM0"] = "custom0";
	actions.renames["CUSTOM1"] = "custom1";
	actions.renames["LIGHT_POSITION"] = "light_position";
	actions.renames["LIGHT_DIRECTION"] = "light_direction";
	actions.renames["LIGHT_IS_DIRECTIONAL"] = "is_directional";
	actions.renames["LIGHT_COLOR"] = "light_color";
	actions.renames["LIGHT_ENERGY"] = "light_energy";
	actions.renames["LIGHT"] = "light";
	actions.renames["SHADOW_MODULATE"] = "shadow_modulate";
	actions.renames["texture_sdf"] = "texture_sdf";
	actions.renames["texture_sdf_normal"] = "texture_sdf_normal";
	actions.renames["sdf_to_screen_uv"] = "sdf_to_screen_uv";
	actions.renames["screen_uv_to_sdf"] = "screen_uv_to_sdf";
	actions.usage_defines["COLOR"] = "#define COLOR_USED\n";
	actions.usage_defines["SCREEN_UV"] = "#define SCREEN_UV_USED\n";
	actions.usage_defines["SCREEN_PIXEL_SIZE"] = "@SCREEN_UV";
	actions.usage_defines["NORMAL"] = "#define NORMAL_USED\n";
	actions.usage_defines["NORMAL_MAP"] = "#define NORMAL_MAP_USED\n";
	actions.usage_defines["SPECULAR_SHININESS"] = "#define SPECULAR_SHININESS_USED\n";
	actions.usage_defines["CUSTOM0"] = "#define CUSTOM0_USED\n";
	actions.usage_defines["CUSTOM1"] = "#define CUSTOM1_USED\n";
	actions.render_mode_defines["skip_vertex_transform"] = "#define SKIP_TRANSFORM_USED\n";
	actions.render_mode_defines["unshaded"] = "#define MODE_UNSHADED\n";
	actions.render_mode_defines["light_only"] = "#define MODE_LIGHT_ONLY\n";
	actions.render_mode_defines["world_vertex_coords"] = "#define USE_WORLD_VERTEX_COORDS\n";
	actions.global_buffer_array_variable = "global_shader_uniforms";
	actions.instance_uniform_index_variable = "read_draw_data_instance_offset";
	canvas_compiler.initialize(actions);
	canvas_compiler_ready = true;
}

// The fragment template: drivers/gles3/shaders/canvas.glsl's fragment main()
// without lights, normal maps and the screen/SDF textures. Vertex colour
// already holds the item's modulate, as GLES3's color_interp does.
static const char *canvas_vs_glsl =
		"#version 330\n"
		"#extension GL_ARB_separate_shader_objects : enable\n"
		"layout(location = 0) in vec3 VertexPosition;\n"
		"layout(location = 1) in vec2 VertexTexCoord;\n"
		"layout(location = 2) in vec4 VertexColor;\n"
		"layout(location = 0) out vec4 VaryingColor;\n"
		"layout(location = 1) out vec2 VaryingTexCoord;\n"
		"void main()\n"
		"{\n"
		"    gl_Position = vec4(VertexPosition, 1.0);\n"
		"    VaryingColor = VertexColor;\n"
		"    VaryingTexCoord = VertexTexCoord;\n"
		"}\n";

// User varyings get explicit locations after the template's (colour 0, UV 1),
// so both stages agree (drivers/gles3 leaves them out on low-end builds).
static String locate_varyings(const String &p_globals) {
	String out;
	int location = 2;
	for (const String &line : p_globals.split("\n")) {
		String l = line;
		String interp;
		for (const char *mode : { "flat ", "smooth ", "noperspective " }) {
			if (l.begins_with(mode)) {
				interp = mode;
				l = l.substr(interp.length());
			}
		}
		if (l.begins_with("out ") || l.begins_with("in ")) {
			int size = 1;
			if (l.contains("mat2")) {
				size = 2;
			} else if (l.contains("mat3")) {
				size = 3;
			} else if (l.contains("mat4")) {
				size = 4;
			}
			const int open = l.find("[");
			if (open != -1) {
				size *= MAX(1, l.substr(open + 1).to_int());
			}
			out += "layout(location = " + itos(location) + ") " + interp + l + "\n";
			location += size;
		} else {
			out += line + "\n";
		}
	}
	return out;
}

// The uniforms both stages declare: loose (register mode); GLES3 puts these in a block.
static String loose_uniforms(const ShaderCompiler::GeneratedCode &p_gen) {
	String s;
	for (const String &d : p_gen.defines) {
		s += d;
	}
	for (const String &line : p_gen.uniforms.split("\n")) {
		if (!line.strip_edges().is_empty()) {
			s += "uniform " + line + "\n";
		}
	}
	return s;
}

// Vertex shader for shaders with vertex() code: drivers/gles3/shaders/canvas.glsl's
// vertex main() for attribute draws. The item transform (local to render-target
// pixels) is model_matrix; CANVAS_MATRIX is identity (already in it) and
// SCREEN_MATRIX maps pixels to clip space.
static String canvas_vertex_glsl(const ShaderCompiler::GeneratedCode &p_gen) {
	String s = "#version 330\n"
			   "#extension GL_ARB_separate_shader_objects : enable\n"
			   "layout(location = 0) in vec3 VertexPosition;\n"
			   "layout(location = 1) in vec2 VertexTexCoord;\n"
			   "layout(location = 2) in vec4 VertexColor;\n"
			   "layout(location = 0) out vec4 VaryingColor;\n"
			   "layout(location = 1) out vec2 VaryingTexCoord;\n"
			   "uniform mat4 model_matrix;\n"
			   "uniform float time;\n"
			   "uniform vec2 color_texture_pixel_size;\n"
			   "uniform vec2 screen_pixel_size;\n";
	s += loose_uniforms(p_gen);
	s += locate_varyings(p_gen.stage_globals[ShaderCompiler::STAGE_VERTEX]);
	s += "void main()\n"
		 "{\n"
		 "\tvec2 vertex = VertexPosition.xy;\n"
		 "\tvec2 uv = VertexTexCoord;\n"
		 "\tvec4 color = VertexColor;\n"
		 "\tvec4 instance_custom = vec4(0.0);\n"
		 "\tvec4 custom0 = vec4(0.0);\n"
		 "\tvec4 custom1 = vec4(0.0);\n"
		 "\tfloat point_size = 1.0;\n"
		 "\tmat4 canvas_transform = mat4(1.0);\n"
		 "\tmat4 screen_transform = mat4(vec4(2.0 * screen_pixel_size.x, 0.0, 0.0, 0.0), vec4(0.0, -2.0 * screen_pixel_size.y, 0.0, 0.0),\n"
		 "\t\t\tvec4(0.0, 0.0, 1.0, 0.0), vec4(-1.0, 1.0, 0.0, 1.0));\n"
		 "#ifdef USE_WORLD_VERTEX_COORDS\n"
		 "\tvertex = (model_matrix * vec4(vertex, 0.0, 1.0)).xy;\n"
		 "#endif\n"
		 "\t{\n";
	if (const String *code = p_gen.code.getptr("vertex")) {
		s += *code;
	}
	s += "\n\t}\n"
		 "#if !defined(SKIP_TRANSFORM_USED) && !defined(USE_WORLD_VERTEX_COORDS)\n"
		 "\tvertex = (model_matrix * vec4(vertex, 0.0, 1.0)).xy;\n"
		 "#endif\n"
		 "\tVaryingColor = color;\n"
		 "\tVaryingTexCoord = uv;\n"
		 "\tgl_Position = screen_transform * vec4(vertex, 0.0, 1.0);\n"
		 "}\n";
	return s;
}

static String canvas_fragment_glsl(const ShaderCompiler::GeneratedCode &p_gen) {
	// The screen texture (hint_screen_texture) is "color_buffer" in the
	// generated code, as in drivers/gles3; the draw binds the back buffer copy.
	const String screen_texture = p_gen.uses_screen_texture ? "uniform sampler2D color_buffer;\n" : "";
	String s = "#version 330\n"
			   "#extension GL_ARB_separate_shader_objects : enable\n"
			   "layout(location = 0) in vec4 VertexColor;\n"
			   "layout(location = 1) in vec2 VertexTexCoord;\n"
			   "layout(location = 0) out vec4 frag_color;\n"
			   "uniform sampler2D color_texture;\n"
			   "uniform float time;\n"
			   "uniform vec2 color_texture_pixel_size;\n"
			   "uniform vec2 screen_pixel_size;\n" +
			screen_texture;
	s += loose_uniforms(p_gen);
	s += locate_varyings(p_gen.stage_globals[ShaderCompiler::STAGE_FRAGMENT]);
	s += "void main()\n"
		 "{\n"
		 "\tvec4 color = VertexColor;\n"
		 "\tvec2 uv = VertexTexCoord;\n"
		 "\tvec2 vertex = gl_FragCoord.xy;\n"
		 "\tvec4 region_rect = vec4(0.0, 0.0, 1.0 / color_texture_pixel_size);\n"
		 "\tcolor *= texture(color_texture, uv);\n"
		 "\tvec3 normal = vec3(0.0, 0.0, 1.0);\n"
		 "\tvec4 specular_shininess = vec4(1.0);\n"
		 "\tvec2 screen_uv = gl_FragCoord.xy * screen_pixel_size;\n"
		 "\tvec3 light_vertex = vec3(vertex, 0.0);\n"
		 "\tvec2 shadow_vertex = vertex;\n"
		 "\t{\n"
		 "\t\tfloat normal_map_depth = 1.0;\n"
		 "\t\tvec3 normal_map = vec3(0.0, 0.0, 1.0);\n";
	if (const String *code = p_gen.code.getptr("fragment")) {
		s += *code;
	}
	s += "\n\t}\n"
		 "\tfrag_color = color;\n"
		 "}\n";
	return s;
}

static void print_numbered(const String &p_source) {
	const Vector<String> lines = p_source.split("\n");
	for (int i = 0; i < lines.size(); i++) {
		print_line(vformat("[shader] %4d %s", i + 1, lines[i]));
	}
}

static bool is_float_or_vector(ShaderLanguage::DataType p_type) {
	switch (p_type) {
		case ShaderLanguage::TYPE_BOOL:
		case ShaderLanguage::TYPE_BVEC2:
		case ShaderLanguage::TYPE_BVEC3:
		case ShaderLanguage::TYPE_BVEC4:
		case ShaderLanguage::TYPE_INT:
		case ShaderLanguage::TYPE_IVEC2:
		case ShaderLanguage::TYPE_IVEC3:
		case ShaderLanguage::TYPE_IVEC4:
		case ShaderLanguage::TYPE_UINT:
		case ShaderLanguage::TYPE_UVEC2:
		case ShaderLanguage::TYPE_UVEC3:
		case ShaderLanguage::TYPE_UVEC4:
		case ShaderLanguage::TYPE_FLOAT:
		case ShaderLanguage::TYPE_VEC2:
		case ShaderLanguage::TYPE_VEC3:
		case ShaderLanguage::TYPE_VEC4:
		case ShaderLanguage::TYPE_MAT2:
		case ShaderLanguage::TYPE_MAT3:
		case ShaderLanguage::TYPE_MAT4:
			return true;
		default:
			return false;
	}
}

// Registers (vec4 each) a uniform of this type takes.
static uint32_t register_count(ShaderLanguage::DataType p_type) {
	switch (p_type) {
		case ShaderLanguage::TYPE_MAT2:
			return 2;
		case ShaderLanguage::TYPE_MAT3:
			return 3;
		case ShaderLanguage::TYPE_MAT4:
			return 4;
		default:
			return 1;
	}
}

MaterialStorageWiiU::CanvasProgram *MaterialStorageWiiU::_compile_canvas_program(RID p_shader, const String &p_code) {
	if (!p_code.contains("canvas_item")) {
		return nullptr;
	}
	_init_canvas_compiler();
	const String *path_ptr = shader_paths.getptr(p_shader);
	const String path = path_ptr ? *path_ptr : String("(no path)");

	bool uses_sdf = false;
	int blend_dummy = 0;
	HashMap<StringName, ShaderLanguage::ShaderNode::Uniform> uniforms;
	ShaderCompiler::IdentifierActions actions;
	actions.entry_point_stages["vertex"] = ShaderCompiler::STAGE_VERTEX;
	actions.entry_point_stages["fragment"] = ShaderCompiler::STAGE_FRAGMENT;
	actions.entry_point_stages["light"] = ShaderCompiler::STAGE_FRAGMENT;
	for (const char *mode : { "blend_add", "blend_mix", "blend_sub", "blend_mul", "blend_premul_alpha", "blend_disabled" }) {
		actions.render_mode_values[mode] = Pair<int *, int>(&blend_dummy, 0);
	}
	actions.usage_flag_pointers["texture_sdf"] = &uses_sdf;
	actions.usage_flag_pointers["texture_sdf_normal"] = &uses_sdf;
	actions.usage_flag_pointers["sdf_to_screen_uv"] = &uses_sdf;
	actions.usage_flag_pointers["screen_uv_to_sdf"] = &uses_sdf;
	actions.uniforms = &uniforms;
	ShaderCompiler::GeneratedCode gen;
	if (canvas_compiler.compile(RSE::SHADER_CANVAS_ITEM, p_code, &actions, path, gen) != OK) {
		return nullptr; // The dummy compiler already reported it.
	}

	const String *fragment = gen.code.getptr("fragment");
	if (fragment == nullptr || fragment->strip_edges().is_empty()) {
		return nullptr; // Only render_mode (CanvasItemMaterial): the blend is enough.
	}
	String unsupported;
	const String *vertex = gen.code.getptr("vertex");
	const bool local_vertices = vertex && !vertex->strip_edges().is_empty();
	if (uses_sdf) {
		unsupported = "SDF";
	}
	for (const KeyValue<StringName, ShaderLanguage::ShaderNode::Uniform> &E : uniforms) {
		if (!unsupported.is_empty()) {
			break;
		}
		const ShaderLanguage::ShaderNode::Uniform &u = E.value;
		if (u.scope != ShaderLanguage::ShaderNode::Uniform::SCOPE_LOCAL) {
			unsupported = "global/instance uniform " + String(E.key);
		} else if (u.array_size > 0) {
			unsupported = "uniform array " + String(E.key);
		} else if (ShaderLanguage::is_sampler_type(u.type) ? u.type != ShaderLanguage::TYPE_SAMPLER2D : !is_float_or_vector(u.type)) {
			unsupported = "uniform type of " + String(E.key);
		}
	}
	if (!unsupported.is_empty()) {
		print_line(vformat("[shader] %s: %s isn't supported yet; drawing without the shader", path, unsupported));
		return nullptr;
	}
	// Loads cafeglsl.rpl on the first shader that needs it (not at start-up).
	if (!wiiu_gx2_glsl_available()) {
		return nullptr;
	}

	const String ps = canvas_fragment_glsl(gen);
	const String vs = local_vertices ? canvas_vertex_glsl(gen) : String(canvas_vs_glsl);
	const String key = vs + "\n// ----\n" + ps; // Key into program_cache.
	WiiUProgram *program = nullptr;
	if (CachedProgram *cached = program_cache.getptr(key)) {
		cached->refs++;
		program = cached->program;
	} else {
		static char log[4096];
		const uint64_t start = OS::get_singleton()->get_ticks_msec();
		const CharString ps_utf8 = ps.utf8();
		const CharString vs_utf8 = vs.utf8();
		program = wiiu_gx2_program_compile(vs_utf8.get_data(), ps_utf8.get_data(), log, sizeof(log));
		const uint64_t ms = OS::get_singleton()->get_ticks_msec() - start;
		if (program == nullptr) {
			print_line(vformat("[shader] %s: GLSL compile failed after %d ms; drawing without the shader", path, ms));
			for (const String &line : String::utf8(log).split("\n")) {
				if (!line.strip_edges().is_empty()) {
					print_line("[shader] " + line);
				}
			}
			if (local_vertices) {
				print_numbered(vs);
			}
			print_numbered(ps);
			return nullptr;
		}
		if (wiiu_gx2_program_vertex_sampler_count(program) > 0) {
			print_line(vformat("[shader] %s: textures in vertex() aren't supported yet; drawing without the shader", path));
			wiiu_gx2_program_free(program);
			return nullptr;
		}
		print_line(vformat("[shader] %s: compiled in %d ms%s", path, ms, local_vertices ? " (with vertex())" : ""));
		CachedProgram c;
		c.program = program;
		c.refs = 1;
		program_cache.insert(key, c);
	}

	CanvasProgram *cp = memnew(CanvasProgram);
	cp->program = program;
	cp->source = key;
	cp->serial = ++program_serial;
	cp->uses_screen_texture = gen.uses_screen_texture;
	cp->local_vertices = local_vertices;
	for (const KeyValue<StringName, ShaderLanguage::ShaderNode::Uniform> &E : uniforms) {
		const ShaderLanguage::ShaderNode::Uniform &u = E.value;
		if (u.hint == ShaderLanguage::ShaderNode::Uniform::HINT_SCREEN_TEXTURE ||
				u.hint == ShaderLanguage::ShaderNode::Uniform::HINT_DEPTH_TEXTURE ||
				u.hint == ShaderLanguage::ShaderNode::Uniform::HINT_NORMAL_ROUGHNESS_TEXTURE) {
			continue;
		}
		CanvasProgram::Param param;
		param.name = E.key;
		param.type = u.type;
		param.hint = u.hint;
		param.filter = u.filter;
		param.repeat = u.repeat;
		const CharString id = ("m_" + String(E.key)).utf8();
		if (ShaderLanguage::is_sampler_type(u.type)) {
			param.location = wiiu_gx2_program_sampler_location(program, id.get_data());
			if (param.location >= 0) {
				cp->textures.push_back(param);
			}
		} else {
			param.location = wiiu_gx2_program_uniform_offset(program, id.get_data());
			param.vs_location = local_vertices ? wiiu_gx2_program_vertex_uniform_offset(program, id.get_data()) : -1;
			if (param.location < 0 && param.vs_location < 0) {
				continue; // Optimized out.
			}
			if (!u.default_value.is_empty()) {
				param.default_value = ShaderLanguage::constant_value_to_variant(u.default_value, u.type, u.array_size, u.hint);
			}
			if (param.location >= 0) {
				cp->reg_words = MAX(cp->reg_words, (uint32_t)param.location + 4 * register_count(u.type));
			}
			if (param.vs_location >= 0) {
				cp->vs_reg_words = MAX(cp->vs_reg_words, (uint32_t)param.vs_location + 4 * register_count(u.type));
			}
			cp->uniforms.push_back(param);
		}
	}
	return cp;
}

void MaterialStorageWiiU::_free_canvas_program(RID p_shader) {
	CanvasProgram **cp = shader_programs.getptr(p_shader);
	if (cp == nullptr) {
		return;
	}
	if (CachedProgram *cached = program_cache.getptr((*cp)->source)) {
		if (--cached->refs <= 0) {
			wiiu_gx2_program_free(cached->program);
			program_cache.erase((*cp)->source);
		}
	}
	memdelete(*cp);
	shader_programs.erase(p_shader);
}

void MaterialStorageWiiU::shader_free(RID p_rid) {
	shader_blend.erase(p_rid);
	shader_paths.erase(p_rid);
	spatial_shaders.erase(p_rid);
	_free_canvas_program(p_rid);
	RendererDummy::MaterialStorage::shader_free(p_rid);
}

void MaterialStorageWiiU::shader_set_path_hint(RID p_shader, const String &p_path) {
	shader_paths[p_shader] = p_path;
}

void MaterialStorageWiiU::shader_set_code(RID p_shader, const String &p_code) {
	RendererDummy::MaterialStorage::shader_set_code(p_shader, p_code);
	const WiiUBlend b = blend_from_shader_code(p_code);
	if (b == WIIU_BLEND_MIX) {
		shader_blend.erase(p_shader);
	} else {
		shader_blend[p_shader] = b;
	}
	SpatialShader spatial;
	if (spatial_from_shader_code(p_code, spatial)) {
		spatial_shaders[p_shader] = spatial;
	} else {
		spatial_shaders.erase(p_shader);
	}
	_free_canvas_program(p_shader);
	CanvasProgram *cp = _compile_canvas_program(p_shader, p_code);
	if (cp) {
		shader_programs.insert(p_shader, cp);
	}
}

void MaterialStorageWiiU::material_free(RID p_rid) {
	material_shader.erase(p_rid);
	material_data.erase(p_rid);
	RendererDummy::MaterialStorage::material_free(p_rid);
}

void MaterialStorageWiiU::material_set_shader(RID p_material, RID p_shader) {
	RendererDummy::MaterialStorage::material_set_shader(p_material, p_shader);
	material_shader[p_material] = p_shader;
	if (MaterialData *md = material_data.getptr(p_material)) {
		md->built_serial = 0;
	}
}

void MaterialStorageWiiU::material_set_param(RID p_material, const StringName &p_param, const Variant &p_value) {
	MaterialData &md = material_data[p_material];
	if (p_value.get_type() == Variant::NIL) {
		md.params.erase(p_param);
	} else {
		md.params[p_param] = p_value;
	}
	md.built_serial = 0;
}

Variant MaterialStorageWiiU::material_get_param(RID p_material, const StringName &p_param) const {
	const MaterialData *md = material_data.getptr(p_material);
	if (md) {
		if (const Variant *v = md->params.getptr(p_param)) {
			return *v;
		}
	}
	return Variant();
}

WiiUBlend MaterialStorageWiiU::get_material_blend(RID p_material) const {
	const RID *shader = material_shader.getptr(p_material);
	if (shader == nullptr) {
		return WIIU_BLEND_MIX;
	}
	const WiiUBlend *b = shader_blend.getptr(*shader);
	return b ? *b : WIIU_BLEND_MIX;
}

bool MaterialStorageWiiU::material_has_program(RID p_material) const {
	const RID *shader = material_shader.getptr(p_material);
	return shader && shader_programs.has(*shader);
}

bool MaterialStorageWiiU::material_uses_screen_texture(RID p_material) const {
	const RID *shader = material_shader.getptr(p_material);
	if (shader == nullptr) {
		return false;
	}
	CanvasProgram *const *cp = shader_programs.getptr(*shader);
	return cp && (*cp)->uses_screen_texture;
}

// One uniform into its registers: floats as floats, ints and bools as their
// 32-bit patterns (bools as ~0, Mesa's true), matrices one column per register.
static void pack_uniform(uint32_t *r_regs, ShaderLanguage::DataType p_type, const Variant &p_value) {
	float f[16] = {};
	int32_t n[4] = {};
	bool is_int = false;
	switch (p_type) {
		case ShaderLanguage::TYPE_BOOL:
			n[0] = p_value.booleanize() ? -1 : 0;
			is_int = true;
			break;
		case ShaderLanguage::TYPE_BVEC2:
		case ShaderLanguage::TYPE_BVEC3:
		case ShaderLanguage::TYPE_BVEC4: {
			const int bits = (int)p_value;
			for (int i = 0; i < 4; i++) {
				n[i] = (bits & (1 << i)) ? -1 : 0;
			}
			is_int = true;
		} break;
		case ShaderLanguage::TYPE_INT:
		case ShaderLanguage::TYPE_UINT:
			n[0] = (int32_t)(int64_t)p_value;
			is_int = true;
			break;
		case ShaderLanguage::TYPE_IVEC2:
		case ShaderLanguage::TYPE_UVEC2: {
			const Vector2i v = p_value;
			n[0] = v.x;
			n[1] = v.y;
			is_int = true;
		} break;
		case ShaderLanguage::TYPE_IVEC3:
		case ShaderLanguage::TYPE_UVEC3: {
			const Vector3i v = p_value;
			n[0] = v.x;
			n[1] = v.y;
			n[2] = v.z;
			is_int = true;
		} break;
		case ShaderLanguage::TYPE_IVEC4:
		case ShaderLanguage::TYPE_UVEC4: {
			const Vector4i v = p_value;
			n[0] = v.x;
			n[1] = v.y;
			n[2] = v.z;
			n[3] = v.w;
			is_int = true;
		} break;
		case ShaderLanguage::TYPE_FLOAT:
			f[0] = (float)p_value;
			break;
		case ShaderLanguage::TYPE_VEC2: {
			const Vector2 v = p_value;
			f[0] = v.x;
			f[1] = v.y;
		} break;
		case ShaderLanguage::TYPE_VEC3:
			if (p_value.get_type() == Variant::COLOR) {
				const Color c = p_value;
				f[0] = c.r;
				f[1] = c.g;
				f[2] = c.b;
			} else {
				const Vector3 v = p_value;
				f[0] = v.x;
				f[1] = v.y;
				f[2] = v.z;
			}
			break;
		case ShaderLanguage::TYPE_VEC4:
			if (p_value.get_type() == Variant::COLOR) {
				const Color c = p_value;
				f[0] = c.r;
				f[1] = c.g;
				f[2] = c.b;
				f[3] = c.a;
			} else if (p_value.get_type() == Variant::PLANE) {
				const Plane p = p_value;
				f[0] = p.normal.x;
				f[1] = p.normal.y;
				f[2] = p.normal.z;
				f[3] = p.d;
			} else if (p_value.get_type() == Variant::QUATERNION) {
				const Quaternion q = p_value;
				f[0] = q.x;
				f[1] = q.y;
				f[2] = q.z;
				f[3] = q.w;
			} else {
				const Vector4 v = p_value;
				f[0] = v.x;
				f[1] = v.y;
				f[2] = v.z;
				f[3] = v.w;
			}
			break;
		case ShaderLanguage::TYPE_MAT2:
			if (p_value.get_type() == Variant::TRANSFORM2D) {
				const Transform2D t = p_value;
				f[0] = t.columns[0].x;
				f[1] = t.columns[0].y;
				f[4] = t.columns[1].x;
				f[5] = t.columns[1].y;
			} else {
				const PackedFloat32Array a = p_value;
				for (int i = 0; i < 4 && i < a.size(); i++) {
					f[(i / 2) * 4 + (i % 2)] = a[i];
				}
			}
			break;
		case ShaderLanguage::TYPE_MAT3: {
			const Basis b = p_value;
			for (int c = 0; c < 3; c++) {
				const Vector3 col = b.get_column(c);
				f[c * 4 + 0] = col.x;
				f[c * 4 + 1] = col.y;
				f[c * 4 + 2] = col.z;
			}
		} break;
		case ShaderLanguage::TYPE_MAT4: {
			const Projection p = p_value;
			for (int c = 0; c < 4; c++) {
				for (int r = 0; r < 4; r++) {
					f[c * 4 + r] = p.columns[c][r];
				}
			}
		} break;
		default:
			break;
	}
	if (is_int) {
		memcpy(r_regs, n, sizeof(n));
	} else {
		memcpy(r_regs, f, 4 * 4 * register_count(p_type));
	}
}

static int filter_bits(ShaderLanguage::TextureFilter p_filter, int p_default) {
	switch (p_filter) {
		case ShaderLanguage::FILTER_NEAREST:
			return 0;
		case ShaderLanguage::FILTER_LINEAR:
			return WIIU_FILTER_LINEAR;
		case ShaderLanguage::FILTER_NEAREST_MIPMAP:
		case ShaderLanguage::FILTER_NEAREST_MIPMAP_ANISOTROPIC:
			return WIIU_FILTER_MIPMAPS;
		case ShaderLanguage::FILTER_LINEAR_MIPMAP:
		case ShaderLanguage::FILTER_LINEAR_MIPMAP_ANISOTROPIC:
			return WIIU_FILTER_LINEAR | WIIU_FILTER_MIPMAPS;
		default:
			return p_default;
	}
}

bool MaterialStorageWiiU::material_uses_local_vertices(RID p_material) const {
	const RID *shader = material_shader.getptr(p_material);
	if (shader == nullptr) {
		return false;
	}
	CanvasProgram *const *cp = shader_programs.getptr(*shader);
	return cp && (*cp)->local_vertices;
}

bool MaterialStorageWiiU::get_material_program(RID p_material, int p_filter, bool p_repeat, WiiUDrawState &r_state) {
	const RID *shader = material_shader.getptr(p_material);
	if (shader == nullptr) {
		return false;
	}
	CanvasProgram **cpp = shader_programs.getptr(*shader);
	if (cpp == nullptr) {
		return false;
	}
	const CanvasProgram *cp = *cpp;
	MaterialData &md = material_data[p_material];
	if (md.built_serial != cp->serial) {
		md.regs.resize(cp->reg_words);
		for (uint32_t i = 0; i < md.regs.size(); i++) {
			md.regs[i] = 0;
		}
		md.vs_regs.resize(cp->vs_reg_words);
		for (uint32_t i = 0; i < md.vs_regs.size(); i++) {
			md.vs_regs[i] = 0;
		}
		for (const CanvasProgram::Param &param : cp->uniforms) {
			const Variant *v = md.params.getptr(param.name);
			const Variant &value = v ? *v : param.default_value;
			if (param.location >= 0) {
				pack_uniform(md.regs.ptr() + param.location, param.type, value);
			}
			if (param.vs_location >= 0) {
				pack_uniform(md.vs_regs.ptr() + param.vs_location, param.type, value);
			}
		}
		md.built_serial = cp->serial;
	}
	// Textures every time: the resources behind the RIDs can change.
	md.textures.resize(cp->textures.size());
	TextureStorageWiiU *ts = TextureStorageWiiU::get_wiiu_singleton();
	for (uint32_t i = 0; i < cp->textures.size(); i++) {
		const CanvasProgram::Param &param = cp->textures[i];
		WiiUProgramTexture &t = md.textures[i];
		t.location = param.location;
		t.filter = filter_bits(param.filter, p_filter);
		t.repeat = param.repeat == ShaderLanguage::REPEAT_DEFAULT ? p_repeat : param.repeat == ShaderLanguage::REPEAT_ENABLE;
		t.texture = nullptr;
		const Variant *v = md.params.getptr(param.name);
		if (v && ts) {
			RID rid;
			if (v->get_type() == Variant::RID) {
				rid = *v;
			} else if (v->get_type() == Variant::OBJECT) {
				Ref<Texture2D> tex = *v;
				if (tex.is_valid()) {
					rid = tex->get_rid();
				}
			}
			if (rid.is_valid()) {
				t.texture = ts->resolve(rid).texture;
			}
		}
		if (t.texture == nullptr) {
			switch (param.hint) {
				case ShaderLanguage::ShaderNode::Uniform::HINT_DEFAULT_BLACK:
					t.texture = wiiu_gx2_black_texture();
					break;
				case ShaderLanguage::ShaderNode::Uniform::HINT_DEFAULT_TRANSPARENT:
					t.texture = wiiu_gx2_transparent_texture();
					break;
				default:
					t.texture = wiiu_gx2_white_texture();
					break;
			}
		}
	}
	r_state.program = cp->program;
	r_state.regs = md.regs.ptr();
	r_state.reg_words = md.regs.size();
	r_state.vs_regs = md.vs_regs.ptr();
	r_state.vs_reg_words = md.vs_regs.size();
	r_state.textures = md.textures.ptr();
	r_state.texture_count = md.textures.size();
	r_state.local_vertices = cp->local_vertices;
	return true;
}

// StandardMaterial3D's parameters (other spatial shaders: whichever of these
// names they have, white otherwise).
static Color param_color(const HashMap<StringName, Variant> &p_params, const char *p_name, const Color &p_default) {
	const Variant *v = p_params.getptr(StringName(p_name));
	return v && v->get_type() == Variant::COLOR ? Color(*v) : p_default;
}

static Vector3 param_vec3(const HashMap<StringName, Variant> &p_params, const char *p_name, const Vector3 &p_default) {
	const Variant *v = p_params.getptr(StringName(p_name));
	if (v && v->get_type() == Variant::VECTOR3) {
		return *v;
	}
	if (v && v->get_type() == Variant::VECTOR2) {
		const Vector2 v2 = *v;
		return Vector3(v2.x, v2.y, p_default.z);
	}
	return p_default;
}

bool MaterialStorageWiiU::get_spatial_material(RID p_material, SpatialMaterial &r_material) const {
	const RID *shader = material_shader.getptr(p_material);
	const SpatialShader *sh = shader ? spatial_shaders.getptr(*shader) : nullptr;
	if (sh == nullptr) {
		return false;
	}
	SpatialMaterial m;
	m.shader = *sh;
	const MaterialData *md = material_data.getptr(p_material);
	if (md) {
		m.albedo = param_color(md->params, "albedo", Color(1, 1, 1, 1));
		const Vector3 scale = param_vec3(md->params, "uv1_scale", Vector3(1, 1, 1));
		const Vector3 offset = param_vec3(md->params, "uv1_offset", Vector3());
		m.uv1 = Vector4(scale.x, scale.y, offset.x, offset.y);
		if (const Variant *v = md->params.getptr(StringName("alpha_scissor_threshold"))) {
			m.alpha_scissor_threshold = *v;
		}
		const Variant *tex = md->params.getptr(StringName("texture_albedo"));
		TextureStorageWiiU *ts = TextureStorageWiiU::get_wiiu_singleton();
		if (tex && ts) {
			RID rid;
			if (tex->get_type() == Variant::RID) {
				rid = *tex;
			} else if (tex->get_type() == Variant::OBJECT) {
				Ref<Texture2D> t = *tex;
				if (t.is_valid()) {
					rid = t->get_rid();
				}
			}
			if (rid.is_valid()) {
				m.texture = ts->resolve(rid).texture;
			}
		}
	}
	r_material = m;
	return true;
}
