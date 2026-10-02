#version 450
layout(location=0) in vec3 in_Position;
layout(location=1) in vec3 in_Normal;
layout(location=2) in vec2 in_TextureCoord;
layout(location=3) in vec4 in_Colour;
layout(location=4) in vec3 in_Barycentric;
layout(set=1,binding=0) uniform SourceCameraVertex { mat4 view; mat4 projection; vec4 clipping; };
layout(location=0) out vec2 v_vTexcoord;
layout(location=1) out vec4 v_vColour;
layout(location=2) out vec3 v_vNormal;
layout(location=3) out vec3 v_barycentric;
layout(location=4) out vec4 v_worldPosition;
layout(location=5) out vec3 v_viewPosition;
layout(location=6) out vec3 v_viewNormal;
layout(location=7) out float v_cameraDistance;
void main() {
 vec4 objectPosition=vec4(in_Position,1);
 gl_Position=projection*view*objectPosition;
 v_worldPosition=objectPosition;
 v_viewPosition=gl_Position.xyz;
 v_vColour=in_Colour; v_vTexcoord=in_TextureCoord;
 v_vNormal=normalize(in_Normal);
 v_viewNormal=normalize((view*vec4(in_Normal,0)).xyz);
 // The source encoded-depth port uses pre-divide clip z, independently of the depth test.
 v_cameraDistance=((gl_Position.z-clipping.x)/abs(clipping.y-clipping.x))*.5+.5;
 v_barycentric=in_Barycentric;
}
