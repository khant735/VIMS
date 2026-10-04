#version 450
layout(location=0) in vec3 inPos;
layout(location=1) in vec3 inNormal;
layout(location=2) in vec2 inUV;
layout(location=0) out vec3 n;
layout(location=1) out vec3 p;
layout(location=2) out vec2 uv;
layout(push_constant) uniform PC { mat4 mvp; mat4 model; vec4 material; } pc;
void main(){
    gl_Position=pc.mvp*vec4(inPos,1);
    n=mat3(pc.model)*inNormal;
    p=inPos;
    uv=inUV;
}
