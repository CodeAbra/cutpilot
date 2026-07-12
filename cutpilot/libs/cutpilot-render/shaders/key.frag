#version 440

// Chroma or luma key. Chroma removes pixels near the key color: alpha fades
// in over the softness band beyond the tolerance. Luma removes dark pixels
// by luminance the same way. The matte the node also outputs is this alpha.

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    vec4 keyColor;
    int lumaKey;
    float tolerance;
    float softness;
    int hasImage;
    int imageMatte;
};

layout(binding = 1) uniform sampler2D imageTex;

void main()
{
    vec4 img = vec4(0.0);
    if (hasImage == 1) {
        img = texture(imageTex, v_uv);
        if (imageMatte == 1)
            img = vec4(vec3(img.a), 1.0);
    }

    float measure = (lumaKey == 1)
        ? dot(img.rgb, vec3(0.2126, 0.7152, 0.0722))
        : distance(img.rgb, keyColor.rgb) * 0.57735027;
    float keep = smoothstep(tolerance, tolerance + max(softness, 1e-4), measure);

    fragColor = vec4(img.rgb, img.a * keep);
}
