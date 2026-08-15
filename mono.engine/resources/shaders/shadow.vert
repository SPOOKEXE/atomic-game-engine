#version 450

// Depth-only pass. It shares the colour pass's vertex layout and instance buffer.

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inTexCoord;

layout(location = 3) in vec4 inModel0;
layout(location = 4) in vec4 inModel1;
layout(location = 5) in vec4 inModel2;
layout(location = 6) in vec4 inModel3;
layout(location = 7) in vec4 inColour;
layout(location = 8) in vec4 inInverseScaleSquared;

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

void main() {
	mat4 model = mat4(inModel0, inModel1, inModel2, inModel3);
	vec4 world = model * vec4(inPosition, 1.0);

	outWorldPosition = world.xyz;
	gl_Position = light.ViewProjection * world;
}
