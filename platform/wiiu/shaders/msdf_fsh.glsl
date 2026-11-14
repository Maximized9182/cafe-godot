#version 330
#extension GL_ARB_separate_shader_objects : enable

// MSDF text (Godot's FontFile multichannel_signed_distance_field), ported from
// drivers/gles3/shaders/canvas.glsl.
layout (location = 0) in vec4 VertexColor;
layout (location = 1) in vec2 VertexTexCoord;

layout (location = 0) out vec4 VaryingColor;

uniform sampler2D texture0;

// x: px_range, y: outline size, zw: texture size in pixels.
layout(std140) uniform MsdfParams
{
    vec4 params;
} m;

float msdf_median(float r, float g, float b)
{
    return max(min(r, g), min(max(r, g), b));
}

void main()
{
    vec4 msdf_sample = texture(texture0, VertexTexCoord);
    float px_range = m.params.x;
    float outline_thickness = m.params.y;
    vec2 dest_size = vec2(1.0) / fwidth(VertexTexCoord);
    float px_size = max(0.5 * dot(vec2(px_range) / m.params.zw, dest_size), 1.0);
    float d = msdf_median(msdf_sample.r, msdf_sample.g, msdf_sample.b);
    float a;
    if (outline_thickness > 0.0) {
        float cr = clamp(outline_thickness, 0.0, (px_range / 2.0) - 1.0) / px_range;
        d = min(d, msdf_sample.a);
        a = clamp((d - 0.5 + cr) * px_size, 0.0, 1.0);
    } else {
        a = clamp((d - 0.5) * px_size + 0.5, 0.0, 1.0);
    }
    VaryingColor = vec4(VertexColor.rgb, VertexColor.a * a);
}
