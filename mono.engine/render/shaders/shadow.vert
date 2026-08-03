#version 450

// The depth-only pass, from the light's point of view.
//
// **The same instance buffer the colour pass binds**, which is the whole reason
// this is cheap: the geometry is already uploaded and already ordered, so a
// shadow map costs one more draw over data that is on the device anyway. The
// only thing that differs is the matrix.
//
// The colour attribute is declared and unused. It has to be: the vertex input
// layout is part of the pipeline, and a pipeline that described the buffer
// differently from the one that binds it is undefined behaviour rather than a
// validation error on every backend.

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;

layout(location = 2) in vec4 inModel0;
layout(location = 3) in vec4 inModel1;
layout(location = 4) in vec4 inModel2;
layout(location = 5) in vec4 inModel3;
layout(location = 6) in vec4 inColour;

layout(set = 1, binding = 0) uniform Light {
	mat4 ViewProjection;
} light;

void main() {
	mat4 model = mat4(inModel0, inModel1, inModel2, inModel3);
	gl_Position = light.ViewProjection * model * vec4(inPosition, 1.0);
}
