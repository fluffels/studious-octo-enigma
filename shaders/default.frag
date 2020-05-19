#version 450
#extension GL_ARB_separate_shader_objects : enable

// TODO(jan): Somehow bind this the the number of textures in the BSP.
layout(binding=1) uniform sampler2D atlas[59];

layout(binding=2) uniform samplerBuffer lightMap;

layout(location=0) in vec2 inTexCoord;
layout(location=1) in flat uint inTexIdx;
layout(location=2) in vec2 inLightCoord;
layout(location=3) in flat int inLightIdx;
layout(location=4) in flat vec2 inExtent;

layout(location=0) out vec4 outColor;

void main() {
    vec3 texturedColor = texture(atlas[inTexIdx], inTexCoord).rgb;
    int lightIdx = int(
        inLightIdx + inLightCoord.x + inLightCoord.y * inExtent.x
    );
    float lightValue = texelFetch(lightMap, lightIdx).r;
    outColor = vec4(lightValue);
}
