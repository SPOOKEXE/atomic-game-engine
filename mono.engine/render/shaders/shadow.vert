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

void main() {
	mat4 model = mat4(inModel0, inModel1, inModel2, inModel3);
	gl_Position = light.ViewProjection * model * vec4(inPosition, 1.0);
}
