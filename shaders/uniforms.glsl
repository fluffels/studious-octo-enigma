layout(binding=0) uniform Uniform {
    mat4x4 mvp;
    vec3 origin;
    float elapsedS;
    float light[12];
} uniforms;
