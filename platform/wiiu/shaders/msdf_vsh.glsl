#version 330
#extension GL_ARB_separate_shader_objects : enable

// Vertex shader for MSDF text. The engine sends vertices already in clip
// space, so no matrices: this pair uses uniform registers, not blocks.
layout(location = 0) in vec3 VertexPosition;
layout(location = 1) in vec2 VertexTexCoord;
layout(location = 2) in vec4 VertexColor;

layout(location = 0) out vec4 VaryingColor;
layout(location = 1) out vec2 VaryingTexCoord;

void main()
{
    gl_Position = vec4(VertexPosition, 1.0);
    VaryingColor = VertexColor;
    VaryingTexCoord = VertexTexCoord;
}
