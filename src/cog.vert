#version 450
layout(location=0) in vec3 inPos;
layout(location=1) in vec3 inNormal;
layout(location=0) out vec3 n;
layout(push_constant) uniform PC { mat4 mvp; mat4 model; vec4 color; } pc;
void main(){ gl_Position=pc.mvp*vec4(inPos,1); n=mat3(pc.model)*inNormal; }
