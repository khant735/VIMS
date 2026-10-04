#version 450
layout(location=0) in vec3 n;
layout(location=1) in vec3 p;
layout(location=2) in vec2 uv;
layout(set=0,binding=0) uniform sampler2D cogTexture;
layout(location=0) out vec4 outColor;
layout(push_constant) uniform PC { mat4 mvp; mat4 model; vec4 material; } pc;

float hash21(vec2 q){
    q=fract(q*vec2(123.34,456.21));
    q+=dot(q,q+45.32);
    return fract(q.x*q.y);
}

void main(){
    int m=int(pc.material.w+0.5);
    vec3 N=normalize(n);
    vec3 L=normalize(vec3(-0.45,-0.55,0.72));
    vec3 V=normalize(vec3(0.25,-0.45,1.0));
    vec3 H=normalize(L+V);
    float ndl=max(dot(N,L),0.0);
    float ndh=max(dot(N,H),0.0);
    float rim=pow(1.0-max(dot(N,V),0.0),3.0);

    vec3 base;
    float spec;
    vec3 specColor;

    if(m==0){
        // Red-stained wood cog: broad organic grain with restrained highlights.
        float grain=sin(p.x*18.0 + sin(p.y*5.0)*2.2 + sin(p.x*3.0+p.y*2.0)*1.4);
        float fine=sin(p.x*47.0 + p.y*7.0)*0.35;
        float wood=0.5+0.5*(grain*0.75+fine*0.25);
        base=vec3(0.88,0.20,0.025)*mix(0.78,1.08,wood);
        spec=pow(ndh,22.0)*0.11;
        specColor=vec3(1.0,0.72,0.45);
    }else if(m==1){
        // Blue resin/plastic cog: clean, nearly uniform colour with a smooth dielectric highlight.
        float micro=(hash21(floor(uv*180.0))-0.5)*0.018;
        base=vec3(0.025,0.20,0.96)*(1.0+micro);
        spec=pow(ndh,72.0)*0.34;
        specColor=vec3(0.88,0.94,1.0);
    }else{
        // Silver spindle: the external fine-grain texture is used only for metal.
        vec3 texel=texture(cogTexture,uv*5.0).rgb;
        float metal=dot(texel,vec3(0.2126,0.7152,0.0722));
        base=vec3(0.64,0.68,0.73)*mix(0.88,1.12,metal);
        spec=pow(ndh,mix(48.0,86.0,metal))*mix(0.30,0.55,metal);
        specColor=vec3(0.94,0.97,1.0);
    }

    float diffuse=0.36+0.64*ndl;
    vec3 color=base*diffuse + specColor*spec;
    color+=base*rim*(m==2 ? 0.14 : 0.055);
    outColor=vec4(clamp(color,0.0,1.0),1.0);
}
