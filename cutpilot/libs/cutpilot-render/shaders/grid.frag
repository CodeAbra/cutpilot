#version 440

layout(location = 0) out vec4 fragColor;

// Camera and theme state, updated per frame on change only. std140 layout matches
// the C++ packing in CanvasRenderer.
layout(std140, binding = 0) uniform Block {
    vec2 viewportSize;   // color buffer size in physical pixels
    vec2 panPixels;      // world origin offset, in physical pixels
    float zoom;          // world-to-screen scale (1.0 == 100%)
    float dpr;           // device pixel ratio
    float minorPitch;    // grid minor pitch in logical world units (e.g. 24)
    float majorEvery;    // a major dot every N minor cells (e.g. 4 -> 96px)
    float yUp;           // 1.0 if gl_FragCoord's origin is bottom-left, else 0.0
    vec4 bgCanvas;       // canvas surface color
    vec4 gridDot;        // minor dot tint
    vec4 gridDotMajor;   // major dot tint
} u;

// Antialiased disc: 1.0 inside, 0.0 outside, smooth across one pixel at the edge.
float disc(float dist, float radius)
{
    float aa = 1.0;
    return 1.0 - smoothstep(radius - aa, radius + aa, dist);
}

void main()
{
    // Fragment position in the top-left-origin, y-down physical-pixel frame the
    // camera and the node layer work in. gl_FragCoord's origin is backend-dependent
    // (bottom-left on OpenGL, top-left on Metal/Vulkan/D3D), so the vertical flip is
    // driven by yUp rather than hard-coded to one convention.
    float fragY = (u.yUp > 0.5) ? (u.viewportSize.y - gl_FragCoord.y) : gl_FragCoord.y;
    vec2 fragPx = vec2(gl_FragCoord.x, fragY);

    // Convert to world coordinates. Screen = world * zoom * dpr + pan.
    float scale = u.zoom * u.dpr;
    vec2 world = (fragPx - u.panPixels) / scale;

    // Grid fades out as cells get too dense to read. The minor pitch in screen
    // pixels is minorPitch * zoom (logical); below ~35% zoom it fades to nothing.
    float gridFade = smoothstep(0.30, 0.40, u.zoom);

    vec4 color = u.bgCanvas;

    if (gridFade > 0.0) {
        // Distance, in physical pixels, to the nearest minor grid intersection.
        vec2 cell = world / u.minorPitch;
        vec2 nearest = round(cell) * u.minorPitch;
        vec2 deltaWorld = world - nearest;
        vec2 deltaPx = deltaWorld * scale;
        float dist = length(deltaPx);

        // Is this nearest intersection a major one?
        vec2 majorCell = round(cell / u.majorEvery) * u.majorEvery;
        vec2 majorNearest = majorCell * u.minorPitch;
        bool isMajor = all(lessThan(abs(nearest - majorNearest), vec2(0.5)));

        float radius = (isMajor ? 1.6 : 1.1) * u.dpr;
        vec4 dotColor = isMajor ? u.gridDotMajor : u.gridDot;

        float a = disc(dist, radius) * dotColor.a * gridFade;
        color.rgb = mix(color.rgb, dotColor.rgb, a);
    }

    fragColor = vec4(color.rgb, 1.0);
}
