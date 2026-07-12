#version 440

// Translate / scale / rotate about the image center, sampling the input with
// the inverse mapping in pixel space so rotation never distorts with aspect.
// Anything mapped from outside the input is transparent.

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    vec2 translate; // as a fraction of the image size
    vec2 texSize;
    float scale;
    float rotationRad;
    int hasImage;
    int imageMatte;
};

layout(binding = 1) uniform sampler2D imageTex;

void main()
{
    if (hasImage == 0) {
        fragColor = vec4(0.0);
        return;
    }

    vec2 centered = v_uv * texSize - 0.5 * texSize - translate * texSize;
    float c = cos(rotationRad);
    float s = sin(rotationRad);
    vec2 unrotated = vec2(c * centered.x + s * centered.y,
                          -s * centered.x + c * centered.y);
    vec2 sourcePx = unrotated / max(scale, 1e-6) + 0.5 * texSize;
    vec2 uv = sourcePx / texSize;

    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
        fragColor = vec4(0.0);
        return;
    }

    vec4 img = texture(imageTex, uv);
    if (imageMatte == 1)
        img = vec4(vec3(img.a), 1.0);
    fragColor = img;
}
