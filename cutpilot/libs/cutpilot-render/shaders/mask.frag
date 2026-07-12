#version 440

// Multiply the image's alpha by the mask's signal. A matte input carries the
// signal in its alpha channel; anything else contributes its luminance scaled
// by its own alpha. No mask wired means a pass-through.

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    int invertMask;
    int hasImage;
    int imageMatte;
    int hasMask;
    int maskMatte;
};

layout(binding = 1) uniform sampler2D imageTex;
layout(binding = 2) uniform sampler2D maskTex;

void main()
{
    vec4 img = vec4(0.0);
    if (hasImage == 1) {
        img = texture(imageTex, v_uv);
        if (imageMatte == 1)
            img = vec4(vec3(img.a), 1.0);
    }

    float signal = 1.0;
    if (hasMask == 1) {
        vec4 m = texture(maskTex, v_uv);
        signal = (maskMatte == 1)
            ? m.a
            : dot(m.rgb, vec3(0.2126, 0.7152, 0.0722)) * m.a;
    }
    if (invertMask == 1)
        signal = 1.0 - signal;

    fragColor = vec4(img.rgb, img.a * signal);
}
