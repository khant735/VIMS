
#version 450
layout(location=0) in vec3 n;
layout(location=0) out vec4 outColor;
layout(push_constant) uniform PC { mat4 mvp; mat4 model; vec4 color; } pc;
void main(){ vec3 N=normalize(n); vec3 L=normalize(vec3(-.45,-.65,.8)); float d=max(dot(N,L),0.0); float s=pow(max(dot(reflect(-L,N),vec3(0,0,1)),0.0),24.0); outColor=vec4(pc.color.rgb*(.18+.72*d)+vec3(.35)*s,1); }