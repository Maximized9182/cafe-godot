extends Node3D

# Cafe Godot 3D test: what each object checks is in its comment. Left stick
# orbits the camera, right stick up/down zooms.

var box: MeshInstance3D
var camera: Camera3D
var label: Label
var yaw := 0.0
var pitch := 0.45
var distance := 6.0


func _material(color: Color) -> StandardMaterial3D:
	var m := StandardMaterial3D.new()
	m.albedo_color = color
	return m


func _add(mesh: Mesh, material: Material, pos: Vector3) -> MeshInstance3D:
	var mi := MeshInstance3D.new()
	mi.mesh = mesh
	mi.material_override = material
	mi.position = pos
	add_child(mi)
	return mi


func _ready() -> void:
	# Background colour and ambient light.
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.1, 0.12, 0.2)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color(0.35, 0.35, 0.4)
	var we := WorldEnvironment.new()
	we.environment = env
	add_child(we)

	# Directional light from the upper left.
	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-50, -35, 0)
	sun.light_energy = 1.0
	add_child(sun)

	# Red omni light near the right front of the floor.
	var omni := OmniLight3D.new()
	omni.position = Vector3(1.8, 0.4, 1.4)
	omni.light_color = Color(1, 0.2, 0.1)
	omni.light_energy = 2.0
	omni.omni_range = 3.0
	add_child(omni)

	# Floor: checker texture, repeated 4x (texture + uv1_scale + repeat).
	var img := Image.create(64, 64, false, Image.FORMAT_RGBA8)
	for y in 64:
		for x in 64:
			var on := ((x / 32) + (y / 32)) % 2 == 0
			img.set_pixel(x, y, Color(0.85, 0.85, 0.85) if on else Color(0.35, 0.35, 0.4))
	img.generate_mipmaps()
	var floor_mat := StandardMaterial3D.new()
	floor_mat.albedo_texture = ImageTexture.create_from_image(img)
	floor_mat.uv1_scale = Vector3(4, 4, 1)
	var plane := PlaneMesh.new()
	plane.size = Vector2(8, 8)
	_add(plane, floor_mat, Vector3.ZERO)

	# Orange box, spinning (lit faces, back faces culled).
	var bm := BoxMesh.new()
	box = _add(bm, _material(Color(1, 0.55, 0.1)), Vector3(0, 0.6, 0))

	# Blue sphere partly behind the box (depth test).
	var sphere := SphereMesh.new()
	sphere.radius = 0.6
	sphere.height = 1.2
	_add(sphere, _material(Color(0.2, 0.45, 1)), Vector3(0.9, 0.6, -0.9))

	# Green half-transparent box in front of the yellow cylinder (blending, sorting).
	var glass := _material(Color(0.2, 1, 0.3, 0.5))
	glass.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	_add(BoxMesh.new(), glass, Vector3(-1.6, 0.5, 0.6))

	# Unshaded yellow cylinder (no lighting: flat colour).
	var flat := _material(Color(1, 0.9, 0.1))
	flat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	var cyl := CylinderMesh.new()
	cyl.top_radius = 0.4
	cyl.bottom_radius = 0.4
	cyl.height = 1.6
	_add(cyl, flat, Vector3(-1.8, 0.8, -1.2))

	# Vertex colours (SurfaceTool mesh, vertex_color_use_as_albedo, unshaded,
	# both sides drawn): red/green/blue corners.
	var st := SurfaceTool.new()
	st.begin(Mesh.PRIMITIVE_TRIANGLES)
	st.set_color(Color(1, 0, 0))
	st.add_vertex(Vector3(-0.6, 0, 0))
	st.set_color(Color(0, 1, 0))
	st.add_vertex(Vector3(0, 1.1, 0))
	st.set_color(Color(0, 0, 1))
	st.add_vertex(Vector3(0.6, 0, 0))
	var tri_mat := StandardMaterial3D.new()
	tri_mat.vertex_color_use_as_albedo = true
	tri_mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	tri_mat.cull_mode = BaseMaterial3D.CULL_DISABLED
	_add(st.commit(), tri_mat, Vector3(2.2, 0.05, -1.6))

	# A row of small cubes in one MultiMeshInstance3D.
	var mm := MultiMesh.new()
	mm.transform_format = MultiMesh.TRANSFORM_3D
	var small := BoxMesh.new()
	small.size = Vector3(0.3, 0.3, 0.3)
	small.material = _material(Color(0.8, 0.3, 0.9))
	mm.mesh = small
	mm.instance_count = 6
	for i in 6:
		mm.set_instance_transform(i, Transform3D(Basis(Vector3.UP, i * 0.3), Vector3(-2.5 + i, 0.15, 2.6)))
	var mmi := MultiMeshInstance3D.new()
	mmi.multimesh = mm
	add_child(mmi)

	camera = Camera3D.new()
	add_child(camera)
	camera.current = true

	# 2D on top of 3D.
	var layer := CanvasLayer.new()
	add_child(layer)
	label = Label.new()
	label.position = Vector2(24, 16)
	label.add_theme_font_size_override("font_size", 28)
	layer.add_child(label)


func _process(delta: float) -> void:
	box.rotate_y(delta * 0.8)
	box.rotate_x(delta * 0.3)
	var stick := Input.get_vector("ui_left", "ui_right", "ui_up", "ui_down")
	yaw -= stick.x * delta * 1.5
	pitch = clamp(pitch - stick.y * delta * 1.0, -0.2, 1.4)
	distance = clamp(distance + Input.get_joy_axis(0, JOY_AXIS_RIGHT_Y) * delta * 4.0, 2.5, 12.0)
	camera.position = Vector3(sin(yaw) * cos(pitch), sin(pitch), cos(yaw) * cos(pitch)) * distance + Vector3(0, 0.5, 0)
	camera.look_at(Vector3(0, 0.5, 0))
	label.text = "Cafe 3D Test   %d fps   left stick: orbit, right stick: zoom" % Engine.get_frames_per_second()
