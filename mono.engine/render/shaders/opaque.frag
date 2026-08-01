#version 450

layout(location = 0) in vec3 inNormal;
layout(location = 1) in vec4 inColour;

layout(location = 0) out vec4 outColour;

// Fragment uniform buffers are set 3 for SPIR-V.
layout(set = 3, binding = 0) uniform Lighting {
	vec4 Direction;
	vec4 Ambient;
} lighting;

void main() {
	vec3 normal = normalize(inNormal);
	float lambert = max(dot(normal, -normalize(lighting.Direction.xyz)), 0.0);

	// A weak upward bounce, so faces pointing away from the light are shaded
	// rather than flat. Cheaper than a second light and enough to read shape.
	float bounce = max(normal.y, 0.0) * 0.15;

	vec3 lit = inColour.rgb * (lighting.Ambient.rgb + vec3(lambert + bounce));
	outColour = vec4(lit, inColour.a);
}
