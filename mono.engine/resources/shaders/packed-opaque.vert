#version 450
#extension GL_GOOGLE_include_directive : require

#include "instance.glsl"
#include "packed-mesh.glsl"

layout(set = 1, binding = 0) uniform Frame {
	mat4 ViewProjection;
	mat4 LightViewProjection;
	mat4 SurfaceViewProjection;
} frame;

layout(location = 0) out vec3 outNormal;
layout(location = 1) out vec4 outColour;
layout(location = 2) out vec4 outLightPosition;
layout(location = 3) out vec4 outSurfacePosition;
layout(location = 4) out vec2 outTexCoord;
layout(location = 5) out vec3 outWorldPosition;
layout(location = 6) flat out uint outAppearance;
layout(location = 7) flat out vec3 outSurfaceColour;
layout(location = 8) flat out vec4 outEmission;
layout(location = 9) flat out uvec2 outFeaturePolicy;

void main() {
	InstanceRow instance = LoadInstance();
	vec3 position = InstancePosition(instance);
	vec3 scale = InstanceScale(instance);
	vec4 rotation = InstanceRotation(instance);
	vec3 meshPosition = PackedPosition(uint(gl_VertexIndex));
	vec3 meshNormal = PackedNormal(uint(gl_VertexIndex));
	outNormal = InstanceWorldNormal(rotation, scale, meshNormal);
	outColour = InstanceColour(instance);
	outTexCoord = PackedUV(uint(gl_VertexIndex));
	outAppearance = InstanceAppearance(instance);
	outSurfaceColour = InstanceSurfaceColour(instance);
	outEmission = InstanceEmission(instance);
	outFeaturePolicy = uvec2(InstanceFeatureEnable(instance), InstanceFeatureDisable(instance));
	vec4 world = vec4(InstanceWorldPosition(rotation, scale, position, meshPosition), 1.0);
	outWorldPosition = world.xyz;
	outLightPosition = frame.LightViewProjection * world;
	outSurfacePosition = frame.SurfaceViewProjection * world;
	gl_Position = frame.ViewProjection * world;
}
