#version 450

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColour;

layout(set = 2, binding = 0) uniform sampler2D litImage;
layout(set = 2, binding = 1) uniform sampler2D depthImage;

struct Volume {
	// xyz: centre. w: fraction of each local half extent used to soften the edge.
	vec4 Origin;
	vec4 AxisX;
	vec4 AxisY;
	// xyz: local +Z axis. w: local Z half extent.
	vec4 AxisZ;
	// rgb: scattering colour. w: density multiplier.
	vec4 ColourDensity;
	// x: extinction. y: noise scale. z: noise strength. w: seed.
	vec4 ExtinctionNoise;
	// x: march steps. y: shadow steps. z: X half extent. w: Y half extent.
	vec4 Steps;
};

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
	vec4 FogColour;
	vec4 Fog;
	vec4 Shadow;
	vec4 SeamCentre[2];
	vec4 SeamOutward[2];
	vec4 SeamFirst[2];
	vec4 SeamSecond[2];
	Volume Volumes[4];
	vec4 VolumeCount;
} pass;

float Hash(vec3 point) {
	return fract(sin(dot(point, vec3(127.1, 311.7, 74.7))) * 43758.5453123);
}

float Noise(vec3 point) {
	vec3 cell = floor(point);
	vec3 fraction = fract(point);
	fraction = fraction * fraction * (3.0 - 2.0 * fraction);
	float a = Hash(cell + vec3(0.0, 0.0, 0.0));
	float b = Hash(cell + vec3(1.0, 0.0, 0.0));
	float c = Hash(cell + vec3(0.0, 1.0, 0.0));
	float d = Hash(cell + vec3(1.0, 1.0, 0.0));
	float e = Hash(cell + vec3(0.0, 0.0, 1.0));
	float f = Hash(cell + vec3(1.0, 0.0, 1.0));
	float g = Hash(cell + vec3(0.0, 1.0, 1.0));
	float h = Hash(cell + vec3(1.0, 1.0, 1.0));
	float low = mix(mix(a, b, fraction.x), mix(c, d, fraction.x), fraction.y);
	float high = mix(mix(e, f, fraction.x), mix(g, h, fraction.x), fraction.y);
	return mix(low, high, fraction.z);
}

vec3 WorldAt(vec2 uv, float depth) {
	vec2 clip = vec2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
	vec4 world = pass.InverseViewProjection * vec4(clip, depth, 1.0);
	return world.xyz / max(abs(world.w), 1e-6);
}

bool Interval(Volume volume, vec3 origin, vec3 direction, float maximum, out float enter, out float exit) {
	vec3 offset = origin - volume.Origin.xyz;
	vec3 localOrigin = vec3(dot(offset, volume.AxisX.xyz), dot(offset, volume.AxisY.xyz), dot(offset, volume.AxisZ.xyz));
	vec3 localDirection = vec3(dot(direction, volume.AxisX.xyz), dot(direction, volume.AxisY.xyz), dot(direction, volume.AxisZ.xyz));
	vec3 extent = vec3(volume.Steps.z, volume.Steps.w, volume.AxisZ.w);
	if (volume.AxisX.w > 0.5) {
		// An ellipsoid is a unit sphere in extent-scaled local space. Solving its
		// ray interval here keeps the later march and self-shadow loop unchanged.
		vec3 scaledOrigin = localOrigin / extent;
		vec3 scaledDirection = localDirection / extent;
		float a = dot(scaledDirection, scaledDirection);
		float b = dot(scaledOrigin, scaledDirection);
		float c = dot(scaledOrigin, scaledOrigin) - 1.0;
		float discriminant = b * b - a * c;
		if (!(a > 0.0) || discriminant < 0.0) {
			return false;
		}
		float root = sqrt(discriminant);
		enter = max((-b - root) / a, 0.0);
		exit = min((-b + root) / a, maximum);
		return exit > enter;
	}
	vec3 inverse = 1.0 / max(abs(localDirection), vec3(1e-5)) * sign(localDirection + vec3(1e-5));
	vec3 first = (-extent - localOrigin) * inverse;
	vec3 second = (extent - localOrigin) * inverse;
	vec3 low = min(first, second);
	vec3 high = max(first, second);
	enter = max(max(low.x, low.y), max(low.z, 0.0));
	exit = min(min(high.x, high.y), min(high.z, maximum));
	return exit > enter;
}

float DensityAt(Volume volume, vec3 world) {
	vec3 offset = world - volume.Origin.xyz;
	vec3 local = vec3(dot(offset, volume.AxisX.xyz), dot(offset, volume.AxisY.xyz), dot(offset, volume.AxisZ.xyz));
	vec3 extent = vec3(volume.Steps.z, volume.Steps.w, volume.AxisZ.w);
	float edge = volume.AxisX.w > 0.5
		? 1.0 - length(local / extent)
		: min(min(1.0 - abs(local.x) / extent.x, 1.0 - abs(local.y) / extent.y), 1.0 - abs(local.z) / extent.z);
	float fade = volume.Origin.w > 0.0 ? smoothstep(0.0, volume.Origin.w, edge) : 1.0;
	float signal = Noise(local * volume.ExtinctionNoise.y + volume.ExtinctionNoise.www);
	return volume.ColourDensity.w * fade * mix(1.0, signal, volume.ExtinctionNoise.z);
}

float LightTransmittance(Volume volume, vec3 point) {
	vec3 toLight = normalize(-pass.Direction.xyz);
	float enter;
	float exit;
	if (!Interval(volume, point + toLight * 0.01, toLight, pass.Planes.y, enter, exit)) {
		return 1.0;
	}
	uint steps = uint(clamp(volume.Steps.y, 1.0, 32.0));
	float delta = (exit - enter) / float(steps);
	float opticalDepth = 0.0;
	for (uint index = 0u; index < 32u; index++) {
		if (index >= steps) {
			break;
		}
		opticalDepth += DensityAt(volume, point + toLight * (enter + (float(index) + 0.5) * delta)) * delta;
	}
	return exp(-volume.ExtinctionNoise.x * opticalDepth);
}

void main() {
	vec4 base = texture(litImage, inUv);
	if (pass.VolumeCount.x < 0.5) {
		outColour = base;
		return;
	}

	vec3 surface = WorldAt(inUv, texture(depthImage, inUv * pass.Target.zw).r);
	vec3 ray = normalize(surface - pass.Eye.xyz);
	float maximum = length(surface - pass.Eye.xyz);
	vec3 colour = base.rgb;
	for (uint volumeIndex = 0u; volumeIndex < 4u; volumeIndex++) {
		if (volumeIndex >= uint(pass.VolumeCount.x)) {
			break;
		}
		Volume volume = pass.Volumes[volumeIndex];
		float enter;
		float exit;
		if (!Interval(volume, pass.Eye.xyz, ray, maximum, enter, exit)) {
			continue;
		}

		uint steps = uint(clamp(volume.Steps.x, 1.0, 64.0));
		float delta = (exit - enter) / float(steps);
		float transmittance = 1.0;
		vec3 scattering = vec3(0.0);
		for (uint index = 0u; index < 64u; index++) {
			if (index >= steps) {
				break;
			}
			vec3 point = pass.Eye.xyz + ray * (enter + (float(index) + 0.5) * delta);
			float density = DensityAt(volume, point);
			float light = LightTransmittance(volume, point);
			vec3 source = volume.ColourDensity.rgb * (pass.Ambient.rgb + pass.Direct.rgb * light) * density;
			scattering += transmittance * source * delta;
			transmittance *= exp(-volume.ExtinctionNoise.x * density * delta);
		}
		colour = colour * transmittance + scattering;
	}
	outColour = vec4(colour, base.a);
}
