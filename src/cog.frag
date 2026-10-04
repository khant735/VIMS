#version 450
layout(location=0) in vec3 n;
layout(location=1) in vec3 p;
layout(location=0) out vec4 outColor;
layout(push_constant) uniform PC { mat4 mvp; mat4 model; vec4 material; } pc;

float hash(vec2 q){return fract(sin(dot(q,vec2(127.1,311.7)))*43758.5453123);}
float noise(vec2 q){vec2 i=floor(q),f=fract(q);f=f*f*(3.0-2.0*f);return mix(mix(hash(i),hash(i+vec2(1,0)),f.x),mix(hash(i+vec2(0,1)),hash(i+vec2(1,1)),f.x),f.y);}
void main(){
 vec3 N=normalize(n);vec3 L=normalize(vec3(-.45,-.65,.8));float d=max(dot(N,L),0.0);
 float spec=pow(max(dot(reflect(-L,N),vec3(0,0,1)),0.0),24.0);
 int m=int(pc.material.w+0.5);vec3 base;
 if(m==0){ // wood: grain continues over faces, teeth and edges from object position
   float grain=sin((p.x*9.0+noise(p.xy*3.5)*3.0)+sin(p.y*2.2))*0.5+0.5;
   float fine=noise(p.xy*18.0);base=mix(vec3(.22,.075,.018),vec3(.72,.34,.075),grain*.72+fine*.28);
   spec*=.22;
 }else if(m==1){ // moulded blue resin/plastic
   float mott=noise(p.xy*22.0)*.08;base=vec3(.035,.20,.62)+mott;spec*=1.15;
 }else{ // machined spindle metal
   float rings=.5+.5*sin(length(p.xy)*95.0);base=vec3(.34,.37,.40)+rings*.07;spec*=1.8;
 }
 outColor=vec4(base*(.20+.76*d)+vec3(.42)*spec,1);
}
