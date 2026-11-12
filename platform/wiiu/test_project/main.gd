extends Node

var frames := 0
var elapsed := 0.0
# Last reported side (-1, 0, 1) of each joypad axis, keyed by "device:axis".
var axis_side := {}


func _ready() -> void:
	print("Hello from GDScript on ", OS.get_name(), " (", OS.get_distribution_name(), ")")
	print("Engine ", Engine.get_version_info().string)
	print("Feature wiiu: ", OS.has_feature("wiiu"))
	var data := PackedByteArray([1, 2, 3, 4])
	print("decode_u32 (expect 67305985): ", data.decode_u32(0))
	print("1.5 * 2 = ", 1.5 * 2.0, "  big int: ", 1 << 40)
	print("user:// is ", OS.get_user_data_dir())
	print("Display server: ", DisplayServer.get_name())
	print("Joypads: ", Input.get_connected_joypads())
	Input.joy_connection_changed.connect(_on_joy_connection_changed)
	_add_test_visuals()


# Renderer test (stage 3): one of each kind of 2D draw command.
func _add_test_visuals() -> void:
	RenderingServer.set_default_clear_color(Color(0.29, 0.18, 0.12))  # Coffee brown.

	var rect := ColorRect.new()  # RECT, untextured
	rect.color = Color(0.9, 0.5, 0.1)
	rect.position = Vector2(80, 80)
	rect.size = Vector2(240, 120)
	add_child(rect)

	var panel := Panel.new()  # StyleBoxFlat: POLYGON, rounded corners
	var style := StyleBoxFlat.new()
	style.bg_color = Color(0.2, 0.5, 0.9)
	style.set_corner_radius_all(16)
	style.border_color = Color(1, 1, 1)
	style.set_border_width_all(4)
	panel.add_theme_stylebox_override("panel", style)
	panel.position = Vector2(380, 80)
	panel.size = Vector2(240, 120)
	add_child(panel)

	var label := Label.new()  # Text: RECTs from the font atlas texture
	label.text = "Cafe Godot on Wii U"
	label.add_theme_font_size_override("font_size", 40)
	label.position = Vector2(80, 240)
	add_child(label)

	var line := Line2D.new()  # POLYGON triangle strip
	line.points = PackedVector2Array([Vector2(80, 360), Vector2(300, 440), Vector2(520, 360), Vector2(740, 440)])
	line.width = 12
	line.default_color = Color(0.3, 0.9, 0.4)
	add_child(line)


func _on_joy_connection_changed(device: int, connected: bool) -> void:
	print("joy ", device, " connected=", connected, " name=", Input.get_joy_name(device))


func _input(event: InputEvent) -> void:
	if event is InputEventJoypadButton:
		print("joy ", event.device, " button ", event.button_index, " pressed=", event.pressed)
	elif event is InputEventJoypadMotion:
		# Report sticks/triggers only when they cross +-0.5, not every tiny move.
		var side := 0
		if event.axis_value > 0.5:
			side = 1
		elif event.axis_value < -0.5:
			side = -1
		var key := str(event.device, ":", event.axis)
		if axis_side.get(key, 0) != side:
			axis_side[key] = side
			print("joy ", event.device, " axis ", event.axis, " -> ", side)


func _process(delta: float) -> void:
	# Hold X + Y on the GamePad to test the crash screen.
	if Input.is_joy_button_pressed(0, JOY_BUTTON_X) and Input.is_joy_button_pressed(0, JOY_BUTTON_Y):
		OS.crash("Cafe Godot crash-screen test (X + Y)")
	frames += 1
	elapsed += delta
	if elapsed >= 1.0:
		print("frames in the last second: ", frames)
		frames = 0
		elapsed = 0.0
