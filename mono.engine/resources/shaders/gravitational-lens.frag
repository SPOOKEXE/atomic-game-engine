#version 450
#extension GL_GOOGLE_include_directive : require

#include "projection.glsl"

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
	vec4 CameraDepth;
} pass;

bool RayAt(vec2 uv, out vec3 origin, out vec3 ray) {
	vec2 clip = ClipCoordinates(uv);
	vec4 nearPoint = pass.InverseViewProjection * vec4(clip, 0.0, 1.0);
	vec4 farPoint = pass.InverseViewProjection * vec4(clip, 0.999, 1.0);
	if (!(abs(nearPoint.w) > 1e-6) || !(abs(farPoint.w) > 1e-6)) {
		return false;
	}
	vec3 nearWorld = nearPoint.xyz / nearPoint.w;
	vec3 farWorld = farPoint.xyz / farPoint.w;
	// Orthographic inverse projections keep homogeneous w constant, so each
	// pixel has its own origin. Perspective and oblique views start at Eye.
	origin = abs(nearPoint.w - farPoint.w) < 1e-6 ? nearWorld : pass.Eye.xyz;
	vec3 direction = farWorld - origin;
	float length = length(direction);
	if (!(length > 1e-6)) {
		return false;
	}
	ray = direction / length;
	return true;
}

bool IntersectSphere(vec3 eye, vec3 ray, vec3 centre, float radius, out float enter) {
	vec3 offset = eye - centre;
	float halfB = dot(offset, ray);
	float discriminant = halfB * halfB - dot(offset, offset) + radius * radius;
	if (discriminant < 0.0) {
		return false;
	}
	float exit = -halfB + sqrt(discriminant);
	if (exit < 0.0) {
		return false;
	}
	enter = max(-halfB - sqrt(discriminant), 0.0);
	return true;
}

vec2 UvFor(vec3 world) {
	vec4 clip = pass.ViewProjection * vec4(world, 1.0);
	if (!(clip.w > 1e-6)) {
		return vec2(-1.0);
	}
	vec2 ndc = clip.xy / clip.w;
	return vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
}

void main() {
	vec3 eye, ray;
	if (!RayAt(inUv, eye, ray)) {
		outColour = texture(sceneColour, inUv);
		return;
	}
	float surfaceDistance = texture(linearDepth, inUv).r;
	float eyeDepth = dot(pass.CameraDepth, vec4(eye, 1.0));
	float rayForward = dot(pass.CameraDepth.xyz, ray);
	if (!(rayForward > 1e-6)) {
		outColour = texture(sceneColour, inUv);
		return;
	}
	vec2 warpedUv = inUv;
	vec3 emission = vec3(0.0);

	for (int index = 0; index < int(pass.TimeCount.y); index++) {
		Lens lens = pass.Lenses[index];
		float enter;
		if (!IntersectSphere(eye, ray, lens.CentreRadius.xyz, lens.CentreRadius.w, enter) ||
			surfaceDistance <= eyeDepth + rayForward * enter) {
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
		vec3 radial = lens.CentreRadius.xyz - closest;
		float radialLength = length(radial);
		if (!(radialLength > 1e-6)) {
			continue;
		}
		vec3 sideways = radial / radialLength;
		float swirl = lens.SpinPriority.x * pass.TimeCount.x;
		float spin = clamp(lens.SpinPriority.x, -1.0, 1.0);
		float bend = lens.AxisZStrength.w * edge * edge * 0.14;
		vec3 bentRay = normalize(ray + sideways * bend + cross(ray, sideways) * bend * spin * sin(swirl));
		float forward = dot(pass.CameraDepth.xyz, bentRay);
		if (!(forward > 1e-6)) {
			continue;
		}
		float travel = (surfaceDistance - eyeDepth) / forward;
		if (!(travel > 0.0)) {
			continue;
		}
		vec2 projected = UvFor(eye + bentRay * travel);
		if (any(lessThan(projected, vec2(0.0))) || any(greaterThan(projected, vec2(1.0)))) {
			continue;
		}
		warpedUv = clamp(projected, vec2(0.001), vec2(0.999));

		float horizon = 1.0 - smoothstep(inner * 0.78, inner, impact);
		float ring = exp(-abs(impact - inner) / max(outer - inner, 0.05) * 7.0) * edge;
		emission += vec3(1.0, 0.52, 0.12) * ring * lens.AxisZStrength.w * 1.8 * (1.0 - horizon);
	}

	vec3 colour = texture(sceneColour, warpedUv).rgb + emission;
	outColour = vec4(colour, 1.0);
}
