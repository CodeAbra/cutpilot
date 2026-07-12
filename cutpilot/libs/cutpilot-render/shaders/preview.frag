#version 440

// The preview's present pass: place the A (and B) composite results in the
// view, apply the compare mode — single, wipe, side-by-side, per-pixel
// difference, overlay — draw the checkerboard under transparency inside the
// content rects and the surround outside, and run every displayed color
// through the display transform.

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    vec4 rectA;    // x, y, w, h in target pixels
    vec4 rectB;
    vec4 surround;
    vec4 divider;
    vec2 viewSize;
    float wipe;    // 0..1 across the view
    float overlayOpacity;
    int mode;      // 0 single, 1 wipe, 2 side-by-side, 3 difference, 4 overlay
    int hasA;
    int hasB;
    float checker; // checker cell size in target pixels
};

layout(binding = 1) uniform sampler2D texA;
layout(binding = 2) uniform sampler2D texB;

bool inside(vec2 p, vec4 rect)
{
    return rect.z > 0.0 && rect.w > 0.0 && p.x >= rect.x && p.y >= rect.y
        && p.x <= rect.x + rect.z && p.y <= rect.y + rect.w;
}

vec4 sampleInto(sampler2D tex, int present, vec4 rect, vec2 p)
{
    if (present == 0 || !inside(p, rect))
        return vec4(0.0);
    vec2 uv = (p - rect.xy) / rect.zw;
    return texture(tex, uv);
}

// The display transform: decode the encoded working values to linear light,
// apply the display grade — neutral until a grade ships — and re-encode for
// the display surface.
vec3 srgbToLinear(vec3 c)
{
    bvec3 low = lessThanEqual(c, vec3(0.04045));
    return mix(pow((c + 0.055) / 1.055, vec3(2.4)), c / 12.92, vec3(low));
}

vec3 linearToSrgb(vec3 c)
{
    bvec3 low = lessThanEqual(c, vec3(0.0031308));
    return mix(1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, c * 12.92, vec3(low));
}

vec3 displayTransform(vec3 c)
{
    return linearToSrgb(srgbToLinear(c));
}

void main()
{
    vec2 p = v_uv * viewSize;

    vec4 content = vec4(0.0);
    bool contentArea = false;

    if (mode == 2) {
        contentArea = inside(p, rectA) || inside(p, rectB);
        content = (p.x < viewSize.x * 0.5)
            ? sampleInto(texA, hasA, rectA, p)
            : sampleInto(texB, hasB, rectB, p);
    } else {
        contentArea = inside(p, rectA);
        vec4 a = sampleInto(texA, hasA, rectA, p);
        vec4 b = sampleInto(texB, hasB, rectA, p);
        if (mode == 1) {
            content = (p.x < wipe * viewSize.x) ? a : b;
        } else if (mode == 3) {
            content = vec4(abs(a.rgb - b.rgb), max(a.a, b.a));
        } else if (mode == 4) {
            float as = b.a * overlayOpacity;
            float ao = as + a.a * (1.0 - as);
            vec3 co = ao > 0.0
                ? (as * b.rgb + a.a * a.rgb * (1.0 - as)) / ao
                : vec3(0.0);
            content = vec4(co, ao);
        } else {
            // Single mode presents A; with A unpinned it shows B rather
            // than an empty view.
            content = hasA == 1 ? a : b;
        }
    }

    float cell = max(checker, 1.0);
    float parity = mod(floor(p.x / cell) + floor(p.y / cell), 2.0);
    vec3 checkerColor = mix(surround.rgb * 0.85, surround.rgb * 1.45, parity);
    vec3 background = contentArea ? checkerColor : surround.rgb;

    vec3 rgb = mix(background, displayTransform(content.rgb),
                   clamp(content.a, 0.0, 1.0));

    if (mode == 1 && abs(p.x - wipe * viewSize.x) <= 1.0)
        rgb = mix(rgb, divider.rgb, divider.a);

    fragColor = vec4(rgb, 1.0);
}
