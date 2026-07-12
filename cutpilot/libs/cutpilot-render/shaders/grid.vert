#version 440

// A single full-surface triangle covers the whole canvas; the three clip-space
// corners are generated from the vertex index, so no vertex buffer is bound. The
// fragment shader does all the grid work in world space.
//
// Triangle corners chosen so the triangle's inscribed region covers the [-1, 1]
// clip rectangle: (-1,-1), (3,-1), (-1,3).

void main()
{
    vec2 pos = vec2((gl_VertexIndex == 1) ? 3.0 : -1.0,
                    (gl_VertexIndex == 2) ? 3.0 : -1.0);
    gl_Position = vec4(pos, 0.0, 1.0);
}
