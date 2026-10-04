#version 450
layout(location=0) in vec3 n;
layout(location=1) in vec3 p;
layout(location=2) in vec2 uv;
layout(set=0,binding=0) uniform sampler2D materialAtlas;
layout(location=0) out vec4 outColor;
layout(push_constant) uniform PC { mat4 mvp; mat4 model; vec4 material; } pc;

vec3 sampleMaterial(int m, vec2 tc){
    vec2 tiled=fract(tc);
    float u=(float(m)+tiled.x)/3.0;
    return texture(materialAtlas,vec2(u,tiled.y)).rgb;
}

void main(){
    int m=clamp(int(pc.material.w+0.5),0,2);
    vec3 N=normalize(n);
    vec3 L=normalize(vec3(-0.45,-0.55,0.72));
    vec3 V=normalize(vec3(0.25,-0.45,1.0));
    vec3 H=normalize(L+V);
    float ndl=max(dot(N,L),0.0);
    float ndh=max(dot(N,H),0.0);
    float rim=pow(1.0-max(dot(N,V),0.0),3.0);

    // All visible surface detail now comes from a real packaged image:
    // 0=cog_wood.ppm, 1=cog_resin.ppm, 2=spindle_metal.ppm.
    vec3 texel=sampleMaterial(m,uv*(m==0?2.0:(m==1?1.5:4.0)));
    vec3 base=texel;
    float spec;
    vec3 specColor;

    if(m==0){
        spec=pow(ndh,20.0)*0.08;
        specColor=vec3(1.0,0.72,0.50);
    }else if(m==1){
        spec=pow(ndh,70.0)*0.32;
        specColor=vec3(0.90,0.95,1.0);
    }else{
        spec=pow(ndh,82.0)*0.48;
        specColor=vec3(0.96,0.98,1.0);
    }

    float diffuse=0.38+0.62*ndl;
    vec3 color=base*diffuse+specColor*spec;
    color+=base*rim*(m==2?0.13:0.05);
    outColor=vec4(clamp(color,0.0,1.0),1.0);
}
