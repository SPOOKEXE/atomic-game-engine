// Directional shadow occlusion carried through local portal seams.
//
// The caller selects its sampler slot because forward materials and deferred
// lighting use different material layouts. The projection itself stays here so
// both paths map receivers back through a seam in exactly the same way.

#ifndef PORTAL_BEAM_SAMPLER_BINDING
	#error "PORTAL_BEAM_SAMPLER_BINDING must name the portal beam sampler slot"
#endif

#define MAX_BEAMS 6

layout(set = 2, binding = PORTAL_BEAM_SAMPLER_BINDING) uniform sampler2D beamMap;

layout(set = 3, binding = 2) uniform Beams {
	mat4 Light[MAX_BEAMS];
	mat4 Back[MAX_BEAMS];
	// xyz the near pane normal, w its offset along that normal.
	vec4 Plane[MAX_BEAMS];
	// xy the atlas scale, zw the atlas offset.
	vec4 Region[MAX_BEAMS];
	// x the number of live portal beams.
	vec4 Count;
} beams;

// Returns the directional visibility carried through every portal that reaches
// this receiver. A receiver already in the source room stays with the native
// shadow map, so only its mapped point beyond a source pane can lower the term.
float PortalBeamFactor(vec3 world) {
	float lit = 1.0;
	int count = int(beams.Count.x);
	for (int index = 0; index < MAX_BEAMS; index++) {
		if (index >= count) {
			break;
		}

		vec4 back = beams.Back[index] * vec4(world, 1.0);
		vec3 near = back.xyz / max(back.w, 1e-6);
		// A mapped receiver beyond the source pane is in the destination room.
		// A point on its source side stays with the native directional map.
		if (dot(near, beams.Plane[index].xyz) <= beams.Plane[index].w) {
			continue;
		}

		vec4 lightPosition = beams.Light[index] * vec4(near, 1.0);
		vec3 projected = lightPosition.xyz / max(lightPosition.w, 1e-6);
		if (projected.z > 1.0 || projected.z < 0.0) {
			continue;
		}

		vec2 uv = vec2(projected.x * 0.5 + 0.5, 0.5 - projected.y * 0.5);
		if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
			continue;
		}
		vec2 atlas = uv * beams.Region[index].xy + beams.Region[index].zw;
		float closest = texture(beamMap, atlas).r;
		lit = min(lit, (projected.z - 0.0025) <= closest ? 1.0 : 0.0);
	}
	return lit;
}
