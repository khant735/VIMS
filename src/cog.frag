#version 450
layout(location=0) in vec3 n;
layout(location=1) in vec3 p;
layout(location=2) in vec2 uv;
layout(set=0,binding=0) uniform sampler2D cogTexture;
layout(location=0) out vec4 outColor;
layout(push_constant) uniform PC { mat4 mvp; mat4 model; vec4 material; } pc;

void main(){
    int m=int(pc.material.w+0.5);
    vec3 N=normalize(n);
    vec3 L=normalize(vec3(-0.45,-0.55,0.72));
    vec3 V=normalize(vec3(0.25,-0.45,1.0));
    vec3 H=normalize(L+V);
    float ndl=max(dot(N,L),0.0);
    float ndh=max(dot(N,H),0.0);

    vec3 tint=m==0 ? vec3(0.96,0.34,0.045)
             : m==1 ? vec3(0.045,0.28,0.98)
                    : vec3(0.62,0.66,0.72);

    // This is a genuine Vulkan sampled texture. Texture luminance provides
    // restrained brushed-metal variation while the diagnostic tint identifies
    // each gear independently.
    vec3 texel=texture(cogTexture,uv*5.0).rgb;
    float metal=dot(texel,vec3(0.2126,0.7152,0.0722));
    vec3 base=tint*mix(0.94,1.04,metal);

    float diffuse=0.38+0.62*ndl;
    float roughness=mix(0.34,0.27,metal);
    float shininess=mix(28.0,58.0,1.0-roughness);
    float spec=pow(ndh,shininess)*mix(0.18,0.27,metal);
    float rim=pow(1.0-max(dot(N,V),0.0),3.0)*0.07;

    vec3 color=base*diffuse;
    color+=vec3(1.0,0.95,0.86)*spec;
    color+=base*rim;
    outColor=vec4(clamp(color,0.0,1.0),1.0);
}
