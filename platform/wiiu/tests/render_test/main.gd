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
