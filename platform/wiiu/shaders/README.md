# GX2 shaders

`texture.gsh` and `color.gsh` are compiled GX2 shader groups (GFD files) taken from
Löve Potion (https://github.com/lovebrew/LovePotion), MIT license,
© 2018 - Present Serena S. Postelnek & Logan Hickok-Dickson.
The GLSL they were compiled from is next to them (`transform_vsh.glsl` with
`texture_fsh.glsl` / `color_fsh.glsl`).

Vertex inputs: `VertexPosition` (vec3, location 0), `VertexTexCoord` (vec2, 1),
`VertexColor` (vec4, 2). Uniform block `Transformation` (std140): `mat4 mdlvMtx`,
`mat4 projMtx`; GX2 reads uniform blocks little-endian, so every 32-bit word is
byte-swapped before upload. `texture.gsh` samples `texture0` and multiplies it by the
vertex colour.

SCsub embeds the `.gsh` files into the binary (see `wiiu_shaders.gen.c`).
