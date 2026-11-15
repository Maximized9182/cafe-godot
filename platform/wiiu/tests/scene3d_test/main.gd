extends Node3D

# Cafe Godot 3D test: what each object checks is in its comment. Left stick
# moves (flies where the camera looks), right stick looks around, X cycles the cull mode of the
# lit materials (back -> front -> disabled): with the right front-face
# winding, "back" shows solid objects and "front" shows their insides.
# Y cycles the background: colour -> procedural sky -> physical sky.

var box: MeshInstance3D
var camera: Camera3D
var label: Label
var yaw := 0.0 # Right stick: where the camera looks.
var pitch := -0.4
const MOVE_SPEED := 3.0 # Metres per second at full tilt.
const LOOK_SPEED := 2.0 # Radians per second at full tilt.
var materials: Array[StandardMaterial3D] = []
var cull := BaseMaterial3D.CULL_BACK
const CULL_NAMES := ["back", "front", "disabled"]
var env: Environment
var background := 0
const BACKGROUND_NAMES := ["colour", "procedural sky", "physical sky"]


func _material(color: Color) -> StandardMaterial3D:
	var m := StandardMaterial3D.new()
	m.albedo_color = color
	materials.append(m)
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
	env = Environment.new()
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
	sun.shadow_enabled = true
	add_child(sun)

	# Red omni light near the right front of the floor.
	var omni := OmniLight3D.new()
	omni.position = Vector3(1.8, 0.4, 1.4)
	omni.light_color = Color(1, 0.2, 0.1)
	omni.light_energy = 2.0
	omni.omni_range = 3.0
	add_child(omni)

	# Cyan spot light shining down on the left of the floor (cone edge, specular).
	var spot := SpotLight3D.new()
	spot.position = Vector3(-1.0, 3.0, 1.8)
	spot.rotation_degrees = Vector3(-90, 0, 0)
	spot.light_color = Color(0.3, 0.9, 1.0)
	spot.light_energy = 3.0
	spot.spot_range = 5.0
	spot.spot_angle = 30.0
	add_child(spot)

	# Shiny spheres: chrome (metallic 1, roughness 0.2) and glossy red plastic
	# (roughness 0.15): sun and spot highlights, sky/ambient reflections.
	var ball := SphereMesh.new()
	ball.radius = 0.35
	ball.height = 0.7
	var chrome := _material(Color(0.9, 0.9, 0.9))
	chrome.metallic = 1.0
	chrome.roughness = 0.2
	_add(ball, chrome, Vector3(1.6, 0.35, 0.9))
	var plastic := _material(Color(0.8, 0.05, 0.05))
	plastic.roughness = 0.15
	_add(ball, plastic, Vector3(-0.6, 0.35, 1.6))

	# Floor: checker texture, repeated 4x (texture + uv1_scale + repeat).
	var img := Image.create_empty(64, 64, false, Image.FORMAT_RGBA8)
	for y in 64:
		for x in 64:
			var on := ((x / 32) + (y / 32)) % 2 == 0
			img.set_pixel(x, y, Color(0.85, 0.85, 0.85) if on else Color(0.35, 0.35, 0.4))
	img.generate_mipmaps()
	var floor_mat := StandardMaterial3D.new()
	materials.append(floor_mat)
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
	camera.position = Vector3(0, 3, 6) # Behind the floor's front edge, looking down at the scene.

	# 2D on top of 3D.
	var layer := CanvasLayer.new()
	add_child(layer)
	label = Label.new()
	label.position = Vector2(24, 16)
	label.add_theme_font_size_override("font_size", 28)
	layer.add_child(label)


# Colour background with a fixed ambient colour, or a sky that also lights the scene.
func _set_background() -> void:
	if background == 0:
		env.background_mode = Environment.BG_COLOR
		env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
		return
	var sky := Sky.new()
	sky.sky_material = ProceduralSkyMaterial.new() if background == 1 else PhysicalSkyMaterial.new()
	env.sky = sky
	env.background_mode = Environment.BG_SKY
	env.ambient_light_source = Environment.AMBIENT_SOURCE_SKY


func _unhandled_input(event: InputEvent) -> void:
	if event is InputEventJoypadButton and event.pressed and event.button_index == JOY_BUTTON_Y:
		background = (background + 1) % 3
		_set_background()
	if event is InputEventJoypadButton and event.pressed and event.button_index == JOY_BUTTON_X:
		cull = (cull + 1) % 3
		for m in materials:
			m.cull_mode = cull


func _process(delta: float) -> void:
	box.rotate_y(delta * 0.8)
	box.rotate_x(delta * 0.3)
	var rs := Vector2(Input.get_joy_axis(0, JOY_AXIS_RIGHT_X), Input.get_joy_axis(0, JOY_AXIS_RIGHT_Y))
	if rs.length() > 0.2:
		yaw -= rs.x * delta * LOOK_SPEED
		pitch = clamp(pitch - rs.y * delta * LOOK_SPEED, -1.5, 1.5)
	camera.rotation = Vector3(pitch, yaw, 0)
	var stick := Input.get_vector("ui_left", "ui_right", "ui_up", "ui_down")
	camera.position += camera.basis * Vector3(stick.x, 0, stick.y) * MOVE_SPEED * delta
	label.text = "Cafe 3D Test   %d fps   cull: %s   background: %s\nleft stick: move, right stick: look, X: cull mode, Y: background" % [Engine.get_frames_per_second(), CULL_NAMES[cull], BACKGROUND_NAMES[background]]
