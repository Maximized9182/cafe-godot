extends Node2D
# Cafe Godot renderer test: meshes, multimeshes, CPUParticles2D, nine-patch
# tile modes and animation slices. Each part has a caption.

class SliceDrawer extends Node2D:
	func _draw() -> void:
		# Red for the first half second, blue for the second.
		draw_animation_slice(1.0, 0.0, 0.5)
		draw_rect(Rect2(0, 0, 120, 60), Color.RED)
		draw_animation_slice(1.0, 0.5, 1.0)
		draw_rect(Rect2(0, 0, 120, 60), Color.BLUE)
		draw_end_animation()

var fps_label: Label

func _checker(size: int, cells: int) -> ImageTexture:
	var img := Image.create_empty(size, size, false, Image.FORMAT_RGBA8)
	var c := size / cells
	for y in size:
		for x in size:
			var on := ((x / c) + (y / c)) % 2 == 0
			img.set_pixel(x, y, Color(1, 0.8, 0.2) if on else Color(0.2, 0.4, 1))
	return ImageTexture.create_from_image(img)

func _frame(size: int, border: int) -> ImageTexture:
	# White border, checkered centre (for nine-patch).
	var img := Image.create_empty(size, size, false, Image.FORMAT_RGBA8)
	for y in size:
		for x in size:
			var edge := x < border or y < border or x >= size - border or y >= size - border
			var on := ((x / 4) + (y / 4)) % 2 == 0
			img.set_pixel(x, y, Color.WHITE if edge else (Color(0.9, 0.2, 0.3) if on else Color(0.1, 0.1, 0.1)))
	return ImageTexture.create_from_image(img)

func _caption(text: String, pos: Vector2) -> void:
	var l := Label.new()
	l.text = text
	l.position = pos
	add_child(l)

func _ready() -> void:
	RenderingServer.set_default_clear_color(Color(0.15, 0.15, 0.2))
	var checker := _checker(64, 4)

	# MeshInstance2D with a QuadMesh, rotating.
	var quad := QuadMesh.new()
	quad.size = Vector2(120, 120)
	var mi := MeshInstance2D.new()
	mi.mesh = quad
	mi.texture = checker
	mi.position = Vector2(140, 170)
	mi.name = "Spin"
	add_child(mi)
	_caption("MeshInstance2D (QuadMesh)", Vector2(40, 250))

	# MultiMeshInstance2D: 16 coloured quads in a ring.
	var mm := MultiMesh.new()
	mm.transform_format = MultiMesh.TRANSFORM_2D
	mm.use_colors = true
	var small := QuadMesh.new()
	small.size = Vector2(24, 24)
	mm.mesh = small
	mm.instance_count = 16
	for i in 16:
		var a := TAU * i / 16.0
		mm.set_instance_transform_2d(i, Transform2D(a, Vector2(cos(a), sin(a)) * 70))
		mm.set_instance_color(i, Color.from_hsv(i / 16.0, 0.8, 1))
	var mmi := MultiMeshInstance2D.new()
	mmi.multimesh = mm
	mmi.texture = checker
	mmi.position = Vector2(420, 170)
	add_child(mmi)
	_caption("MultiMeshInstance2D", Vector2(340, 250))

	# CPUParticles2D fountain.
	var p := CPUParticles2D.new()
	p.amount = 80
	p.lifetime = 1.5
	p.direction = Vector2.UP
	p.spread = 25
	p.initial_velocity_min = 150
	p.initial_velocity_max = 250
	p.gravity = Vector2(0, 300)
	p.scale_amount_min = 4
	p.scale_amount_max = 8
	var ramp := Gradient.new()
	ramp.set_color(0, Color(1, 1, 0.3, 1))
	ramp.set_color(1, Color(1, 0.2, 0.1, 0))
	p.color_ramp = ramp
	p.position = Vector2(700, 240)
	add_child(p)
	_caption("CPUParticles2D", Vector2(640, 250))

	# Nine-patches: stretch, tile, tile fit.
	var frame := _frame(32, 6)
	var modes := [NinePatchRect.AXIS_STRETCH_MODE_STRETCH, NinePatchRect.AXIS_STRETCH_MODE_TILE, NinePatchRect.AXIS_STRETCH_MODE_TILE_FIT]
	var names := ["stretch", "tile", "tile fit"]
	for i in 3:
		var np := NinePatchRect.new()
		np.texture = frame
		np.patch_margin_left = 6
		np.patch_margin_top = 6
		np.patch_margin_right = 6
		np.patch_margin_bottom = 6
		np.axis_stretch_horizontal = modes[i]
		np.axis_stretch_vertical = modes[i]
		np.position = Vector2(40 + i * 230, 330)
		np.size = Vector2(210, 130)
		add_child(np)
		_caption("NinePatchRect " + names[i], Vector2(40 + i * 230, 465))

	# Animation slices: should blink red/blue every half second.
	var sd := SliceDrawer.new()
	sd.position = Vector2(780, 120)
	add_child(sd)
	_caption("draw_animation_slice (red/blue blink)", Vector2(760, 190))

	# Blend modes: white-ish squares over a colour band (CanvasItemMaterial).
	var band := ColorRect.new()
	band.color = Color(0.2, 0.5, 0.9)
	band.position = Vector2(760, 330)
	band.size = Vector2(480, 130)
	add_child(band)
	var blends := [CanvasItemMaterial.BLEND_MODE_MIX, CanvasItemMaterial.BLEND_MODE_ADD, CanvasItemMaterial.BLEND_MODE_SUB, CanvasItemMaterial.BLEND_MODE_MUL, CanvasItemMaterial.BLEND_MODE_PREMULT_ALPHA]
	var blend_names := ["mix", "add", "sub", "mul", "premul"]
	for i in blends.size():
		var sq := ColorRect.new()
		sq.color = Color(1, 0.6, 0.2, 0.8)
		sq.position = Vector2(775 + i * 92, 350)
		sq.size = Vector2(70, 90)
		var m := CanvasItemMaterial.new()
		m.blend_mode = blends[i]
		sq.material = m
		add_child(sq)
		_caption(blend_names[i], Vector2(775 + i * 92, 465))

	_add_mipmap_row()
	_add_gpu_particles()
	_add_msdf_row()
	_add_shader_row(checker)
	_add_screen_texture_case()

	fps_label = Label.new()
	fps_label.position = Vector2(40, 640)
	add_child(fps_label)

func _process(delta: float) -> void:
	$Spin.rotation += delta
	fps_label.text = "%d fps  |  Cafe Render Test (%s / %s)" % [Engine.get_frames_per_second(), OS.get_name(), DisplayServer.get_name()]


# Mipmaps: a fine checker shrunk 4x without and with mipmaps (moire vs smooth
# grey), and a texture whose mip levels are solid colours (red, green, blue,
# yellow, ...) so the screen shows which level the GPU samples (4x smaller:
# level 2, blue).
func _add_mipmap_row() -> void:
	var fine := Image.create_empty(256, 256, false, Image.FORMAT_RGBA8)
	for y in 256:
		for x in 256:
			var on := ((x / 2) + (y / 2)) % 2 == 0
			fine.set_pixel(x, y, Color.WHITE if on else Color.BLACK)
	var no_mips := ImageTexture.create_from_image(fine)
	var with_mips_img := fine.duplicate()
	with_mips_img.generate_mipmaps()
	var with_mips := ImageTexture.create_from_image(with_mips_img)

	var colors := [Color.RED, Color.GREEN, Color.BLUE, Color.YELLOW, Color.MAGENTA, Color.CYAN, Color.WHITE, Color.ORANGE, Color.PURPLE]
	var data := PackedByteArray()
	var size := 256
	var level := 0
	while true:
		var c: Color = colors[level % colors.size()]
		for i in size * size:
			data.append(c.r8)
			data.append(c.g8)
			data.append(c.b8)
			data.append(255)
		if size == 1:
			break
		size /= 2
		level += 1
	var levels_tex := ImageTexture.create_from_image(Image.create_from_data(256, 256, true, Image.FORMAT_RGBA8, data))

	var entries := [[no_mips, CanvasItem.TEXTURE_FILTER_LINEAR, "no mips"], [with_mips, CanvasItem.TEXTURE_FILTER_LINEAR_WITH_MIPMAPS, "mipmaps"], [levels_tex, CanvasItem.TEXTURE_FILTER_LINEAR_WITH_MIPMAPS, "level 2 = blue"]]
	for i in entries.size():
		var sprite := Sprite2D.new()
		sprite.texture = entries[i][0]
		sprite.texture_filter = entries[i][1]
		sprite.centered = false
		sprite.scale = Vector2(0.25, 0.25)
		sprite.position = Vector2(840 + i * 130, 500)
		add_child(sprite)
		_caption(entries[i][2], Vector2(840 + i * 130, 570))


# MSDF text: the same font as a plain bitmap and as a multichannel signed
# distance field, both scaled up 2.5x (MSDF stays sharp, bitmap blurs). The
# MSDF one also has an outline (drawn by the same shader).
func _add_msdf_row() -> void:
	var base: FontFile = load("res://fonts/NotoSans-Regular.ttf")
	var msdf: FontFile = base.duplicate()
	msdf.multichannel_signed_distance_field = true
	var fonts := [[base, "bitmap"], [msdf, "MSDF"]]
	for i in fonts.size():
		var label := Label.new()
		label.text = fonts[i][1]
		label.add_theme_font_override("font", fonts[i][0])
		label.add_theme_font_size_override("font_size", 16)
		label.add_theme_color_override("font_color", Color(1, 0.85, 0.3))
		if i == 1:
			label.add_theme_constant_override("outline_size", 4)
			label.add_theme_color_override("font_outline_color", Color(0.4, 0.1, 0.6))
		label.position = Vector2(40 + i * 260, 525)
		label.scale = Vector2(2.5, 2.5)
		add_child(label)


# GPUParticles2D with a ParticleProcessMaterial: on the Wii U an internal
# CPUParticles2D converted from it draws the particles.
func _add_gpu_particles() -> void:
	var img := Image.create_empty(6, 6, false, Image.FORMAT_RGBA8)
	img.fill(Color.WHITE)
	var p := GPUParticles2D.new()
	p.texture = ImageTexture.create_from_image(img)
	p.amount = 48
	p.lifetime = 1.5
	var m := ParticleProcessMaterial.new()
	m.direction = Vector3(0, -1, 0)
	m.spread = 30.0
	m.initial_velocity_min = 80.0
	m.initial_velocity_max = 120.0
	m.gravity = Vector3(0, 98, 0)
	m.scale_min = 1.0
	m.scale_max = 2.0
	var ramp := Gradient.new()
	ramp.set_color(0, Color(0.3, 1.0, 0.6))
	ramp.set_color(1, Color(0.2, 0.4, 1.0, 0.0))
	var ramp_tex := GradientTexture1D.new()
	ramp_tex.gradient = ramp
	m.color_ramp = ramp_tex
	p.process_material = m
	p.position = Vector2(1160, 220)
	add_child(p)
	_caption("GPUParticles2D", Vector2(1090, 262))


# canvas_item shaders compiled at runtime (cafeglsl.rpl). What each should show:
# const: teal. uv: red-green gradient. param: orange (set from GDScript).
# default: magenta (uniform default). TIME: pulsing purple. TEXTURE: pink
# checker. int/bool: green (red: wrong int/bool packing). sampler: checker from
# a sampler2D parameter. vertex(): grey (not supported yet: drawn without the
# shader; red would mean it ran).
func _add_shader_row(checker: Texture2D) -> void:
	var cases := [
		["const", "COLOR = vec4(0.0, 0.8, 0.7, 1.0);", {}],
		["uv", "COLOR = vec4(UV, 0.0, 1.0);", {}],
		["param", "uniform float amount = 0.2;\nuniform vec4 tint : source_color = vec4(0.0);", {"amount": 1.0, "tint": Color(1, 0.5, 0)}, "COLOR = tint * amount;"],
		["default", "uniform vec4 c = vec4(1.0, 0.0, 1.0, 1.0);", {}, "COLOR = c;"],
		["TIME", "COLOR = vec4(0.5 + 0.5 * sin(TIME * 3.0), 0.2, 0.9, 1.0);", {}],
		["TEXTURE", "", {}, "COLOR = texture(TEXTURE, UV) * vec4(1.0, 0.5, 0.6, 1.0);", true],
		["int/bool", "uniform int mode = 2;\nuniform bool on = false;", {"on": true}, "COLOR = (on && mode == 2) ? vec4(0.1, 0.9, 0.2, 1.0) : vec4(1.0, 0.0, 0.0, 1.0);"],
		["sampler", "uniform sampler2D pattern : hint_default_black;", {"pattern": checker}, "COLOR = texture(pattern, UV * 2.0);"],
		["vertex()", "VERTEX_CODE", {}, "COLOR = vec4(1.0, 0.0, 0.0, 1.0);"],
	]
	for i in cases.size():
		var c: Array = cases[i]
		var globals := ""
		var body: String = c[1]
		if c.size() > 3:
			globals = c[1]
			body = c[3]
		var code := "shader_type canvas_item;\n"
		if globals == "VERTEX_CODE":
			code += "void vertex() {\n\tVERTEX.x += 4.0;\n}\n"
		else:
			code += globals + "\n"
		code += "void fragment() {\n\t" + body + "\n}\n"
		var sh := Shader.new()
		sh.code = code
		var mat := ShaderMaterial.new()
		mat.shader = sh
		for k in c[2]:
			mat.set_shader_parameter(k, c[2][k])
		var item: CanvasItem
		if c.size() > 4 and c[4]:
			var sprite := Sprite2D.new()
			sprite.texture = checker
			sprite.centered = false
			sprite.scale = Vector2(60.0 / 64.0, 60.0 / 64.0)
			item = sprite
		else:
			var rect := ColorRect.new()
			rect.color = Color(0.6, 0.6, 0.6)
			rect.size = Vector2(60, 60)
			item = rect
		item.material = mat
		item.position = Vector2(580 + i * 72, 600)
		add_child(item)
		var cap := Label.new()
		cap.text = c[0]
		cap.add_theme_font_size_override("font_size", 12)
		cap.position = Vector2(580 + i * 72, 662)
		add_child(cap)


# Screen texture (hint_screen_texture): a grey-scale rect over the right half
# of the MultiMesh ring. Right half grey, left half in colour, lined up; a
# mirrored or shifted copy means SCREEN_UV is wrong.
func _add_screen_texture_case() -> void:
	var sh := Shader.new()
	sh.code = "shader_type canvas_item;\nuniform sampler2D screen_tex : hint_screen_texture;\nvoid fragment() {\n\tCOLOR.rgb = vec3(dot(texture(screen_tex, SCREEN_UV).rgb, vec3(1.0 / 3.0)));\n}\n"
	var mat := ShaderMaterial.new()
	mat.shader = sh
	var rect := ColorRect.new()
	rect.material = mat
	rect.position = Vector2(420, 90)
	rect.size = Vector2(100, 160)
	add_child(rect)
