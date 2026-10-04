#version 450
layout(location=0) in vec3 n;
layout(location=1) in vec3 p;
layout(location=0) out vec4 outColor;
layout(push_constant) uniform PC { mat4 mvp; mat4 model; vec4 material; } pc;

float hash(vec3 q){
    return fract(sin(dot(q,vec3(127.1,311.7,74.7)))*43758.5453123);
}

void main(){
    int m=int(pc.material.w+0.5);
    vec3 N=normalize(n);

    // Fixed diagnostic lighting keeps the calibration object visible while
    // exercising transformed normals, diffuse response and specular highlights.
    vec3 L=normalize(vec3(-0.45,-0.55,0.72));
    vec3 V=normalize(vec3(0.25,-0.45,1.0));
    vec3 H=normalize(L+V);
    float ndl=max(dot(N,L),0.0);
    float ndh=max(dot(N,H),0.0);

    vec3 base = m==0 ? vec3(0.92,0.31,0.035)
              : m==1 ? vec3(0.035,0.24,0.92)
                     : vec3(0.55,0.59,0.64);

    // Fine, low-amplitude machining marks. Unlike the previous broad sine
    // pattern this does not visually warp the teeth or create a melted surface.
    float radial=length(p.xy);
    float angle=atan(p.y,p.x);
    float rings=sin(radial*150.0)*0.5+0.5;
    float brush=sin((p.x+p.y)*210.0 + hash(floor(p*80.0))*1.4)*0.5+0.5;
    float micro=mix(rings,brush,0.30);
    float variation=(micro-0.5)*0.055;

    float diffuse=0.34+0.66*ndl;
    float spec=pow(ndh,42.0)*(0.30+0.22*micro);
    float rim=pow(1.0-max(dot(N,V),0.0),3.0)*0.10;

    vec3 color=base*(diffuse+variation);
    color+=vec3(1.0,0.94,0.82)*spec;
    color+=base*rim;
    outColor=vec4(clamp(color,0.0,1.0),1.0);
}
