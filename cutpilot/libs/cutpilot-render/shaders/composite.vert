#version 440

// One full-surface triangle generated from the vertex index; no vertex buffer.
// v_uv has (0,0) at the texture data origin (row 0), matching how sources are
// uploaded and how targets read back, so a chain of passes never flips.

layout(location = 0) out vec2 v_uv;

void main()
{
    vec2 pos = vec2((gl_VertexIndex == 1) ? 3.0 : -1.0,
                    (gl_VertexIndex == 2) ? 3.0 : -1.0);
    v_uv = vec2((pos.x + 1.0) * 0.5, 1.0 - (pos.y + 1.0) * 0.5);
    gl_Position = vec4(pos, 0.0, 1.0);
}
