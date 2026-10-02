#version 450
layout(location=0) in vec2 v_vTexcoord;
layout(location=0) out vec4 finalColor;
layout(set=2,binding=0) uniform sampler2D source0;
layout(set=2,binding=1) uniform sampler2D source1;
layout(set=2,binding=2) uniform sampler2D source2;
layout(set=3,binding=0) uniform PostCamera {
 mat4 projMatrix; mat4 inverseViewProjection;
 vec4 cameraPosition4; vec4 ambient;
 vec2 dimension; float radius; float bias;
 float strength; int mode; int swapX; int showBackground;
 int environmentMapping; int aoEnabled;
};
#define cameraPosition cameraPosition4.xyz
#define v_vColour vec4(1)
#define use_8bit 0



#define MAX_SAMPLE 512










float seed = 234234.453;
float rand(vec2 pos) {
    float value = dot(pos, vec2(12.9898, 78.233));
    value = fract(sin(value + seed) * 43758.5453);
    seed++;
    return value;
}

float rand(vec3 pos) {
    float value = dot(pos, vec3(12.9898, 78.233, 45.164));
    value = fract(sin(value + seed) * 43758.5453);
    seed++;
    return value;
}

vec4 RunSsao() {
 vec4 nativeColor=vec4(0);

    vec3  cPosition = texture( source0, v_vTexcoord ).rgb;
    vec3  cNormal   = texture( source1,   v_vTexcoord ).rgb;
    cNormal = normalize(cNormal);

    nativeColor = vec4(0.);

    float occluded   = 0.;
    float raysTotal  = float(MAX_SAMPLE);

    vec3 rvec      = vec3(rand(v_vTexcoord), rand(v_vTexcoord), rand(v_vTexcoord)) * 2. - 1.;
    vec3 tangent   = normalize(rvec - cNormal * dot(rvec, cNormal));
    vec3 bitangent = cross(cNormal, tangent);
    mat3 tbn       = mat3(tangent, bitangent, cNormal);	//matrix to align the deviated vector to the normal hemisphere.

    for(int i = 0; i < MAX_SAMPLE; i++ ) {
        vec3  sNormal = tbn * vec3( rand(v_vTexcoord) * 2. - 1., rand(v_vTexcoord) * 2. - 1., rand(v_vTexcoord) ); // genetate random point inside the hemisphere.
        float scale   = length(sNormal);
        scale   = mix(0.1, 1.0, scale * scale);
        sNormal = normalize(sNormal) * scale;

        vec3  wPosition    = cPosition + sNormal * radius; //add random vector to current world position.
        float vecToCamDist = distance(wPosition, cameraPosition);

        vec4 projPos = projMatrix * vec4(wPosition, 1.);
        projPos.xyz /= projPos.w;
        projPos      = (projPos + 1.) / 2.;		//project new world position to view space.

        vec3  sPosition    = texture( source0, projPos.xy ).xyz;	//sample depth at the new point in the view space.
        if(sPosition == vec3(0.)) continue;

        float geoToCamDist = distance(sPosition, cameraPosition);

        if(distance(sPosition, cPosition) < radius)
        if(vecToCamDist - bias > geoToCamDist)
            occluded++;
    }

    nativeColor = vec4(vec3(1. - occluded / raysTotal * strength), 1.) * v_vColour;

 return nativeColor;
}








vec4 RunAoBlur() {
 vec4 nativeColor=vec4(0);
 if(radius<=0) return texture(source0,v_vTexcoord);

    vec3 cNormal = texture( source1, v_vTexcoord ).rgb;
    vec2 tx = 1. / dimension;

    vec3  sampled = vec3(0.);
    float weight = 0.;

    for(float i = -radius; i <= radius; i++)
    for(float j = -radius; j <= radius; j++) {
        vec2 pos = v_vTexcoord + vec2(i, j) * tx;
        if(pos.x < 0. || pos.y < 0. || pos.x > 1. || pos.y > 1.)
            continue;

        float str = 1. - length(vec2(i, j)) / radius;
        if(str < 0.) continue;

        vec3 _normal = texture( source1, pos ).rgb;
        if(distance(_normal, cNormal) > 0.2) continue;

        sampled += texture( source0, pos ).rgb * str;
        weight  += str;
    }

    nativeColor = vec4(sampled / weight, 1.) * v_vColour;

 return nativeColor;
}

//
// Simple passthrough fragment shader
//







vec4 RunNormalBlur() {
 vec4 nativeColor=vec4(0);
 if(radius<=0) return texture(source0,v_vTexcoord);

    vec3 current = texture( source0, v_vTexcoord ).rgb;
    if(length(current) == 0.) {
        nativeColor = vec4(0.);
        return nativeColor;
    }

    vec2  tx = 1. / dimension;
    vec3  sampled = vec3(0.);
    float weight = 0.;

    for(float i = -radius; i <= radius; i++)
    for(float j = -radius; j <= radius; j++) {
        vec2 pos = v_vTexcoord + vec2(i, j) * tx;
        if(pos.x < 0. || pos.y < 0. || pos.x > 1. || pos.y > 1.)
            continue;

        float str = 1. - length(vec2(i, j)) / radius;
        if(str < 0.) continue;

        vec3 _sample = texture( source0, pos ).rgb;
        if(length(_sample) == 0.)
            continue;

        sampled += _sample * str;
        weight  += str;
    }

    nativeColor = vec4(sampled / weight, 1.);

    if(use_8bit == 1) {
        nativeColor = nativeColor / 256. * v_vColour;
    }

 return nativeColor;
}






vec4 RunViewNormal() {
 vec4 nativeColor=vec4(0);

    vec4 viewNorm = texture(source0, v_vTexcoord);

    vec3 norm = normalize(viewNorm.xyz);

    norm   = (norm + 1.) * .5;
    if(swapX == 1) norm.x = 1. - norm.x;
    norm.y = 1. - norm.y;
    norm.z = 1. - norm.z;

    nativeColor = vec4(norm, viewNorm.a) * v_vColour;

 return nativeColor;
}
void main() {
 if(mode==5) {
  finalColor=texture(source0,v_vTexcoord);
  vec3 c=finalColor.rgb;
  finalColor.rgb=mix(c/12.92,pow((c+.055)/1.055,vec3(2.4)),greaterThan(c,vec3(.04045)));
  return;
 }

 if(mode==1) {finalColor=RunViewNormal();return;}
 if(mode==2) {finalColor=RunSsao();return;}
 if(mode==3) {finalColor=RunAoBlur();return;}
 if(mode==4) {finalColor=RunNormalBlur();return;}
 vec4 color=texture(source0,v_vTexcoord);
 vec4 background=vec4(0);
 if(showBackground!=0) {
  background=ambient;
  if(environmentMapping!=0) {
   vec2 clip=v_vTexcoord*2-1;
   vec4 farPosition=inverseViewProjection*vec4(clip,1,1);
   vec3 direction=normalize(cameraPosition-farPosition.xyz/farPosition.w);
   vec2 environmentUv=vec2(atan(direction.x,direction.y)/6.28318530718+.5,1-acos(direction.z)/3.14159265359);
   background*=texture(source2,environmentUv);
  }
 }
 finalColor=showBackground!=0?color+background*(1-color.a):color;
 if(aoEnabled!=0) finalColor.rgb*=texture(source1,v_vTexcoord).rgb;
}
