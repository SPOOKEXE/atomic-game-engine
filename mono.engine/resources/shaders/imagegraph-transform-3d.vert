#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec2 inUv;

layout(location = 0) out vec2 uv;

layout(set = 1, binding = 0) uniform Transform {
	mat4 modelViewProjection;
} transform;

void main() {
	uv = inUv;
	gl_Position = transform.modelViewProjection * vec4(inPosition, 1.0);
}
