#version 450
#extension GL_GOOGLE_include_directive : require

// Depth-only pass. It shares the colour pass's mesh layout and resident rows.

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inTexCoord;

// The same storage decode `opaque.vert` includes. The two stages read the same forty bytes,
// and the day one of them was edited and the other was not is the day a shadow
// stopped matching the body casting it.
#include "instance.glsl"

layout(set = 1, binding = 0) uniform Light {
	mat4 ViewProjection;
} light;

// Where this vertex is in the world, for the seam cut in `shadow.frag`.
//
// **A depth-only pass that still has to know where a body was cut.** A half
// drawn whole into the shadow map casts a whole body's shadow, so somebody
// standing in a doorway would darken the near floor as if none of them had gone
// through - and the far half would darken the far floor twice over.
layout(location = 0) out vec3 outWorldPosition;
layout(location = 1) out vec2 outTexCoord;
layout(location = 2) flat out uint outAppearance;
layout(location = 3) flat out float outInstanceAlpha;

void main() {
	vec4 world = vec4(InstanceWorldPosition(InstanceRotation(), inPosition), 1.0);

	outWorldPosition = world.xyz;
	outTexCoord = inTexCoord;
	outAppearance = InstanceAppearance();
	outInstanceAlpha = InstanceColour().a;
	gl_Position = light.ViewProjection * world;
}
