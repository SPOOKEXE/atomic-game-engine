#version 450
#extension GL_GOOGLE_include_directive : require

#include "instance.glsl"
#include "packed-mesh.glsl"

layout(set = 1, binding = 0) uniform Light {
	mat4 ViewProjection;
} light;

layout(location = 0) out vec3 outWorldPosition;
layout(location = 1) out vec2 outTexCoord;
layout(location = 2) flat out uint outAppearance;
layout(location = 3) flat out float outInstanceAlpha;

void main() {
	InstanceRow instance = LoadInstance();
	vec4 rotation = InstanceRotation(instance);
	vec3 meshPosition = PackedPosition(uint(gl_VertexIndex));
	vec4 world = vec4(
		InstanceWorldPosition(rotation, InstanceScale(instance), InstancePosition(instance), meshPosition),
		1.0
	);
	outWorldPosition = world.xyz;
	outTexCoord = PackedUV(uint(gl_VertexIndex));
	outAppearance = InstanceAppearance(instance);
	outInstanceAlpha = InstanceColour(instance).a;
	gl_Position = light.ViewProjection * world;
}
