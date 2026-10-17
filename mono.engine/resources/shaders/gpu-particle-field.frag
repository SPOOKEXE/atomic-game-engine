#version 450
layout(location = 0) in vec2 inTexCoord;
layout(location = 1) in vec4 inColour;
layout(location = 2) in vec3 inWorldPosition;
layout(set = 2, binding = 0) uniform sampler2D particleTexture;
layout(set = 3, binding = 0) uniform Material { vec4 Flags, Illumination, FogColour, Fog, Eye; } material;
layout(location = 0) out vec4 outColour;
void main() {
	vec4 result = texture(particleTexture, inTexCoord) * inColour;
	result.a *= 1.0 - smoothstep(0.42, 1.0, length(inTexCoord - vec2(0.5)) * 2.0);
	result.rgb *= mix(vec3(1.0), material.Illumination.rgb, material.Flags.z);
	float interval = max(material.Fog.y - material.Fog.x, 0.0001);
	float fog = clamp((distance(inWorldPosition, material.Eye.xyz) - material.Fog.x) / interval, 0.0, 1.0);
	result.rgb = mix(result.rgb, material.FogColour.rgb, fog);
	if (result.a < 0.00001) discard;
	result.rgb *= result.a; result.a *= 1.0 - material.Flags.y; outColour = result;
}
