extends Node2D
# Cafe Godot input test: GamePad motion sensors, USB keyboard, USB mouse,
# GamePad microphone. Everything is shown as text so a screenshot tells the story.

var info: Label
var ball: ColorRect
var line: LineEdit
var meter: ColorRect
var capture: AudioEffectCapture
var last_key := "-"
var last_mouse := "-"
var wheel := 0
var clicks := 0
var mic_peak := 0.0

func _ready() -> void:
	RenderingServer.set_default_clear_color(Color(0.12, 0.12, 0.16))
	info = Label.new()
	info.position = Vector2(40, 30)
	info.add_theme_font_size_override("font_size", 18)
	add_child(info)

	# A ball that rolls with the GamePad's tilt (device gravity).
	var area := ColorRect.new()
	area.color = Color(0.2, 0.2, 0.26)
	area.position = Vector2(860, 40)
	area.size = Vector2(380, 300)
	add_child(area)
	ball = ColorRect.new()
	ball.color = Color(1, 0.8, 0.2)
	ball.size = Vector2(24, 24)
	ball.position = area.position + area.size / 2
	add_child(ball)

	line = LineEdit.new()
	line.placeholder_text = "Type here with a USB keyboard"
	line.position = Vector2(40, 470)
	line.size = Vector2(600, 40)
	add_child(line)
	line.grab_focus()

	var button := Button.new()
	button.text = "Click me with the mouse"
	button.position = Vector2(680, 470)
	button.pressed.connect(func(): clicks += 1)
	add_child(button)

	# Microphone: AudioStreamMicrophone into a silent bus with a capture effect.
	var bus := AudioServer.bus_count
	AudioServer.add_bus(bus)
	AudioServer.set_bus_name(bus, "Mic")
	AudioServer.set_bus_volume_db(bus, -80.0)
	capture = AudioEffectCapture.new()
	AudioServer.add_bus_effect(bus, capture)
	var player := AudioStreamPlayer.new()
	player.stream = AudioStreamMicrophone.new()
	player.bus = "Mic"
	add_child(player)
	player.play()
	var meter_bg := ColorRect.new()
	meter_bg.color = Color(0.2, 0.2, 0.26)
	meter_bg.position = Vector2(40, 560)
	meter_bg.size = Vector2(600, 30)
	add_child(meter_bg)
	meter = ColorRect.new()
	meter.color = Color(0.3, 1.0, 0.4)
	meter.position = meter_bg.position
	meter.size = Vector2(0, 30)
	add_child(meter)


func _input(event: InputEvent) -> void:
	if event is InputEventKey:
		last_key = "%s %s unicode=%d%s" % [OS.get_keycode_string(event.keycode), "down" if event.pressed else "up", event.unicode, " echo" if event.echo else ""]
	elif event is InputEventMouseButton:
		last_mouse = "button %d %s" % [event.button_index, "down" if event.pressed else "up"]
		if event.pressed and event.button_index == MOUSE_BUTTON_WHEEL_UP:
			wheel += 1
		elif event.pressed and event.button_index == MOUSE_BUTTON_WHEEL_DOWN:
			wheel -= 1

func _process(delta: float) -> void:
	# The GamePad connects on the first frame, after _ready.
	if Input.has_joy_motion_sensors(0) and not Input.is_joy_motion_sensors_enabled(0):
		Input.set_joy_motion_sensors_enabled(0, true)
	if capture.can_get_buffer(1):
		var frames := capture.get_buffer(capture.get_frames_available())
		var peak := 0.0
		for f in frames:
			peak = max(peak, abs(f.x))
		mic_peak = max(peak, mic_peak * 0.9)
	meter.size.x = clamp(mic_peak, 0.0, 1.0) * 600.0

	var grav := Input.get_gravity()
	# Gravity reads as the upward reaction force: tilting right gives -x, so roll by -x.
	ball.position += Vector2(-grav.x, grav.y) * 40.0 * delta
	ball.position = ball.position.clamp(Vector2(860, 40), Vector2(860 + 380 - 24, 40 + 300 - 24))

	info.text = "\n".join([
		"GamePad motion (device sensors, screen axes: x right, y up, z out of screen)",
		"  accelerometer %s m/s^2" % _v(Input.get_accelerometer()),
		"  gravity       %s" % _v(grav),
		"  gyroscope     %s rad/s" % _v(Input.get_gyroscope()),
		"  magnetometer  %s" % _v(Input.get_magnetometer()),
		"GamePad motion (joypad 0, SDL axes): has %s, enabled %s" % [Input.has_joy_motion_sensors(0), Input.is_joy_motion_sensors_enabled(0)],
		"  accel %s   gyro %s" % [_v(Input.get_joy_accelerometer(0)), _v(Input.get_joy_gyroscope(0))],
		"",
		"Keyboard: last key %s" % last_key,
		"Mouse: position %s  buttons %d  wheel %d  clicks %d  last %s  mode %d" % [get_viewport().get_mouse_position().round(), Input.get_mouse_button_mask(), wheel, clicks, last_mouse, Input.mouse_mode],
		"Microphone: peak %.3f (talk or blow into the GamePad)" % mic_peak,
		"",
		"Lie the GamePad flat face up: gravity z ~ +9.8. Hold it upright facing you: gravity y ~ +9.8.",
		"Hold it upright and turn it so the right edge points down: accelerometer x ~ -9.8; the ball rolls right.",
	])

func _v(v: Vector3) -> String:
	return "(%6.2f, %6.2f, %6.2f)" % [v.x, v.y, v.z]
