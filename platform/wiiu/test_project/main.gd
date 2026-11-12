extends Node

var frames := 0
var elapsed := 0.0


func _ready() -> void:
	print("Hello from GDScript on ", OS.get_name(), " (", OS.get_distribution_name(), ")")
	print("Engine ", Engine.get_version_info().string)
	print("Feature wiiu: ", OS.has_feature("wiiu"))
	var data := PackedByteArray([1, 2, 3, 4])
	print("decode_u32 (expect 67305985): ", data.decode_u32(0))
	print("1.5 * 2 = ", 1.5 * 2.0, "  big int: ", 1 << 40)
	print("user:// is ", OS.get_user_data_dir())


func _process(delta: float) -> void:
	frames += 1
	elapsed += delta
	if elapsed >= 1.0:
		print("frames in the last second: ", frames)
		frames = 0
		elapsed = 0.0
