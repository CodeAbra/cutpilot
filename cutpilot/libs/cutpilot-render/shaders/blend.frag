#version 440

// Layer blend over a base: the separable standard blend formulas with the
// blended color mixed by the base's alpha, then source-over composited.
// Straight (unpremultiplied) alpha throughout; a matte input reads its alpha
// as an opaque gray image.

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    int mode;
    float opacity;
    int hasBase;
    int hasOver;
    int baseMatte;
    int overMatte;
};

layout(binding = 1) uniform sampler2D baseTex;
layout(binding = 2) uniform sampler2D overTex;

vec4 fetch(sampler2D tex, int present, int matte, vec2 uv)
{
    if (present == 0)
        return vec4(0.0);
    vec4 c = texture(tex, uv);
    if (matte == 1)
        return vec4(vec3(c.a), 1.0);
    return c;
}

vec3 blendColor(vec3 b, vec3 s)
{
    if (mode == 1)
        return b * s;
    if (mode == 2)
        return b + s - b * s;
    if (mode == 3)
        return mix(2.0 * b * s,
                   1.0 - 2.0 * (1.0 - b) * (1.0 - s),
                   step(vec3(0.5), b));
    if (mode == 4)
        return min(b + s, vec3(1.0));
    return s;
}

void main()
{
    vec4 base = fetch(baseTex, hasBase, baseMatte, v_uv);
    vec4 over = fetch(overTex, hasOver, overMatte, v_uv);

    float as = clamp(over.a * opacity, 0.0, 1.0);
    float ab = base.a;
    vec3 mixed = mix(over.rgb, blendColor(base.rgb, over.rgb), ab);
    float ao = as + ab * (1.0 - as);
    vec3 co = (as * mixed + ab * base.rgb * (1.0 - as)) / max(ao, 1e-6);
    fragColor = vec4(co, ao);
}
