#version 450

// LensShader contract: sampler 0 is HDR scene colour, sampler 1 is linear
// depth. Set 3 binding 0 is the engine-owned LensPass block. This shader may
// warp a scene sample and add light, but it cannot name a target or resource.

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColour;

layout(set = 2, binding = 0) uniform sampler2D sceneColour;
layout(set = 2, binding = 1) uniform sampler2D linearDepth;

struct Lens {
	vec4 CentreRadius;
	vec4 AxisXInner;
	vec4 AxisYFalloff;
	vec4 AxisZStrength;
	vec4 SpinPriority;
};

layout(set = 3, binding = 0) uniform LensPass {
	mat4 ViewProjection;
	mat4 InverseViewProjection;
	vec4 Target;
	vec4 Eye;
	vec4 TimeCount;
	Lens Lenses[16];
} pass;

vec3 RayAt(vec2 uv) {
	vec2 clip = vec2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
	vec4 farPoint = pass.InverseViewProjection * vec4(clip, 1.0, 1.0);
	return normalize(farPoint.xyz / max(abs(farPoint.w), 1e-6) - pass.Eye.xyz);
}

bool IntersectSphere(vec3 eye, vec3 ray, vec3 centre, float radius, out float enter) {
	vec3 offset = eye - centre;
	float halfB = dot(offset, ray);
	float discriminant = halfB * halfB - dot(offset, offset) + radius * radius;
	if (discriminant < 0.0) {
		return false;
	}
	enter = max(-halfB - sqrt(discriminant), 0.0);
	return true;
}

vec2 UvFor(vec3 world) {
	vec4 clip = pass.ViewProjection * vec4(world, 1.0);
	vec2 ndc = clip.xy / max(abs(clip.w), 1e-6);
	return vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
}

void main() {
	vec3 eye = pass.Eye.xyz;
	vec3 ray = RayAt(inUv);
	float surfaceDistance = texture(linearDepth, inUv).r;
	vec2 warpedUv = inUv;
	vec3 emission = vec3(0.0);

	for (int index = 0; index < int(pass.TimeCount.y); index++) {
		Lens lens = pass.Lenses[index];
		float enter;
		if (!IntersectSphere(eye, ray, lens.CentreRadius.xyz, lens.CentreRadius.w, enter) ||
			surfaceDistance <= enter) {
			continue;
		}

		vec3 toCentre = lens.CentreRadius.xyz - eye;
		float along = dot(toCentre, ray);
		vec3 closest = eye + ray * max(along, 0.0);
		float impact = length(lens.CentreRadius.xyz - closest);
		float outer = lens.CentreRadius.w;
		float inner = lens.AxisXInner.w;
		float transitionOuter = max(mix(inner, outer, lens.AxisYFalloff.w), inner + 0.0001);
		float edge = 1.0 - smoothstep(inner, transitionOuter, impact);
		vec3 sideways = normalize(lens.CentreRadius.xyz - closest + vec3(1e-6));
		float swirl = lens.SpinPriority.x * pass.TimeCount.x;
		vec3 dragged = sideways * cos(swirl) + cross(ray, sideways) * sin(swirl);
		float bend = lens.AxisZStrength.w * edge * edge * 0.14;
		vec3 bentRay = normalize(ray + dragged * bend);
		warpedUv = clamp(UvFor(eye + bentRay * surfaceDistance), vec2(0.001), vec2(0.999));

		float horizon = 1.0 - smoothstep(inner * 0.78, inner, impact);
		float ring = exp(-abs(impact - inner) / max(outer - inner, 0.05) * 7.0) * edge;
		emission += vec3(1.0, 0.52, 0.12) * ring * lens.AxisZStrength.w * 1.8 * (1.0 - horizon);
	}

	vec3 colour = texture(sceneColour, warpedUv).rgb + emission;
	outColour = vec4(colour, 1.0);
}
