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
