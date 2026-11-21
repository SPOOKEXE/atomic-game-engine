#version 450

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColour;

layout(set = 2, binding = 0) uniform sampler2D litImage;
layout(set = 2, binding = 1) uniform sampler2D depthImage;
layout(set = 2, binding = 2) uniform sampler2D environmentImage;

layout(set = 3, binding = 0) uniform Pass {
	mat4 InverseViewProjection;
	mat4 LightViewProjection;
	vec4 Planes;
	vec4 Target;
	vec4 Direction;
	vec4 Ambient;
	vec4 OutdoorAmbient;
	vec4 Direct;
	vec4 Eye;
	vec4 CameraDepth;
	vec4 FogColour;
	vec4 Fog;
	vec4 Shadow;
	// x density, y radius in pixels, z brightness, w seed bits.
	vec4 StarShape;
	// rgb tint, w colour variation.
	vec4 StarColour;
	// x twinkle, y horizon fade, z seconds wrapped to one hour, w enabled.
	vec4 StarMotion;
} pass;

const float PI = 3.14159265358979323846;

// Cells along one cube-face edge. A cell spans about 0.7 degrees at a face centre.
const float STAR_CELLS = 128.0;

uint StarHash(uvec4 key) {
	uint h = key.x * 0x8da6b343u ^ key.y * 0xd8163841u ^ key.z * 0xcb1ab31fu ^ key.w * 0x165667b1u;
	h ^= h >> 16;
	h *= 0x7feb352du;
	h ^= h >> 15;
	h *= 0x846ca68bu;
	h ^= h >> 16;
	return h;
}

float StarUnit(inout uint state) {
	state = StarHash(uvec4(state, 0x9e3779b9u, 0u, 0u));
	return float(state >> 8) * (1.0 / 16777216.0);
}

// The point on the unit cube for face coordinates in [-1, 1] (or beyond, for
// a neighbour cell past the face edge).
vec3 StarFacePoint(uint face, vec2 uv) {
	float s = (face & 1u) == 0u ? 1.0 : -1.0;
	uint axis = face >> 1;
	if (axis == 0u) return vec3(s, uv.y, uv.x * s);
	if (axis == 1u) return vec3(uv.x, s, uv.y * s);
	return vec3(-uv.x * s, uv.y, s);
}

// The cube face and face coordinates a direction falls on.
void StarFace(vec3 direction, out uint face, out vec2 uv) {
	vec3 a = abs(direction);
	if (a.x >= a.y && a.x >= a.z) {
		face = direction.x >= 0.0 ? 0u : 1u;
		uv = vec2(direction.z * sign(direction.x), direction.y) / a.x;
	} else if (a.y >= a.z) {
		face = direction.y >= 0.0 ? 2u : 3u;
		uv = vec2(direction.x, direction.z * sign(direction.y)) / a.y;
	} else {
		face = direction.z >= 0.0 ? 4u : 5u;
		uv = vec2(-direction.x * sign(direction.z), direction.y) / a.z;
	}
}

// Point stars from a stable cube-face grid. Each cell holds at most one star
// at a jittered centre; the pixel tests its own cell and the eight around it,
// across face edges too, and keeps the brightest disc so a corner cell reached
// twice never doubles. The disc radius is a fixed number of screen pixels.
vec3 Stars(vec3 direction, vec3 background, float pixelAngle) {
	float horizon = pass.StarMotion.y;
	float elevation = horizon > 0.0 ? smoothstep(0.0, horizon, direction.y) : step(0.0, direction.y);
	if (elevation <= 0.0) return vec3(0.0);

	// A bright sky (day, lit cloud) hides stars the way it does outdoors.
	float skyLuminance = dot(background, vec3(0.2126, 0.7152, 0.0722));
	float visibility = elevation / (1.0 + skyLuminance * skyLuminance * 64.0);
	if (visibility < 0.002) return vec3(0.0);

	uint seed = floatBitsToUint(pass.StarShape.w);
	float cellAngle = 2.0 / STAR_CELLS;
	// Up to 0.6 cells keeps every disc inside the 3x3 neighbourhood.
	float radius = min(pass.StarShape.y * pixelAngle, cellAngle * 0.6);
	uint face;
	vec2 uv;
	StarFace(direction, face, uv);
	vec2 cell = floor((uv * 0.5 + 0.5) * STAR_CELLS);

	vec3 best = vec3(0.0);
	float bestLuminance = 0.0;
	for (int y = -1; y <= 1; ++y) {
		for (int x = -1; x <= 1; ++x) {
			vec2 neighbourUv = (cell + vec2(x, y) + 0.5) / STAR_CELLS * 2.0 - 1.0;
			uint ownerFace = face;
			vec2 ownerUv = neighbourUv;
			if (any(greaterThan(abs(neighbourUv), vec2(1.0))))
				StarFace(normalize(StarFacePoint(face, neighbourUv)), ownerFace, ownerUv);
			uvec2 ownerCell = uvec2(clamp(floor((ownerUv * 0.5 + 0.5) * STAR_CELLS), 0.0, STAR_CELLS - 1.0));
			uint state = StarHash(uvec4(ownerCell, ownerFace, seed));
			if (StarUnit(state) >= pass.StarShape.x) continue;

			vec2 jitter = vec2(StarUnit(state), StarUnit(state)) * 0.7 + 0.15;
			vec2 centreUv = (vec2(ownerCell) + jitter) / STAR_CELLS * 2.0 - 1.0;
			vec3 centre = normalize(StarFacePoint(ownerFace, centreUv));
			float distance = length(direction - centre);
			if (distance >= radius + pixelAngle) continue;

			// One-pixel antialiased edge over a slightly brighter core.
			float coverage = clamp((radius - distance) / pixelAngle + 0.5, 0.0, 1.0);
			float core = 1.0 - 0.4 * clamp(distance / radius, 0.0, 1.0);
			// Most stars are faint and a few are bright.
			float magnitude = 0.06 + 0.94 * pow(StarUnit(state), 5.0);

			float spread = (StarUnit(state) * 2.0 - 1.0) * pass.StarColour.w;
			vec3 tint = spread >= 0.0 ? mix(vec3(1.0), vec3(1.0, 0.72, 0.48), spread)
									  : mix(vec3(1.0), vec3(0.62, 0.78, 1.0), -spread);

			// Whole cycles per hour, matching the wrapped clock.
			float cycles = floor(mix(1800.0, 7200.0, StarUnit(state)));
			float phase = StarUnit(state) * 2.0 * PI;
			float flicker = 0.5 + 0.5 * sin(pass.StarMotion.z * cycles * (2.0 * PI / 3600.0) + phase);
			float twinkle = 1.0 - pass.StarMotion.x * flicker;

			vec3 star = pass.StarColour.rgb * tint * (pass.StarShape.z * magnitude * twinkle * coverage * core);
			float luminance = dot(star, vec3(0.2126, 0.7152, 0.0722));
			if (luminance > bestLuminance) {
				best = star;
				bestLuminance = luminance;
			}
		}
	}
	return best * visibility;
}

vec3 ViewDirection(vec2 uv) {
	vec2 clip = vec2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
	vec4 farPoint = pass.InverseViewProjection * vec4(clip, 1.0, 1.0);
	vec3 world = farPoint.xyz / max(abs(farPoint.w), 1e-6);
	return normalize(world - pass.Eye.xyz);
}

void main() {
	vec4 lit = texture(litImage, inUv);
	float depth = texture(depthImage, inUv * pass.Target.zw).r;
	if (pass.Fog.w < 0.5 || depth < 0.999999) {
		outColour = lit;
		return;
	}

	vec3 direction = ViewDirection(inUv);
	vec2 environmentUv = vec2(
		atan(direction.x, direction.z) / (2.0 * PI) + 0.5,
		asin(clamp(direction.y, -1.0, 1.0)) / -PI + 0.5
	);
	vec3 colour = texture(environmentImage, environmentUv).rgb;
	if (pass.StarMotion.w > 0.5) {
		// The angle one scene pixel spans here, from the neighbouring pixel's ray.
		float pixelAngle = length(ViewDirection(inUv + vec2(1.0 / max(pass.Target.x, 1.0), 0.0)) - direction);
		colour += Stars(direction, colour, max(pixelAngle, 1e-6));
	}
	outColour = vec4(colour, lit.a);
}
