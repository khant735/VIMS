#version 450
layout(location=0) in vec3 n;
layout(location=1) in vec3 p;
layout(location=0) out vec4 outColor;
layout(push_constant) uniform PC { mat4 mvp; mat4 model; vec4 material; } pc;
float hash(vec2 q){return fract(sin(dot(q,vec2(127.1,311.7)))*43758.5453123);}
float noise(vec2 q){vec2 i=floor(q),f=fract(q);f=f*f*(3.0-2.0*f);return mix(mix(hash(i),hash(i+vec2(1,0)),f.x),mix(hash(i+vec2(0,1)),hash(i+vec2(1,1)),f.x),f.y);}
void main(){
 int m=int(pc.material.w+0.5); vec3 base;
 if(m==0){float grain=.5+.5*sin(p.x*13.0+noise(p.xy*5.0)*4.0);base=mix(vec3(.28,.09,.025),vec3(.95,.52,.13),grain);}
 else if(m==1){base=vec3(.06,.34,.95)+noise(p.xy*20.0)*.08;}
 else {base=vec3(.62,.67,.72);}
 // Deliberately emissive diagnostic material: if this is still black the
 // failure is not normals/lighting but shader packaging/push constants/output.
 outColor=vec4(base,1.0);
}
