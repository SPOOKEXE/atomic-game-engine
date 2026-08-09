#version 450

// Screen-space ambient occlusion: how shut in each pixel is.
//
// **The engine ships this one, which is what `NodeKindSpec::DefaultShader` is
// for.** `raster` runs whatever a node names and does nothing without one —
// right for a kind that *is* "somebody's shader". `ssao` is not that: it has one
// correct implementation, so an author drops the node and it works, and a node
// naming its own shader still wins.
//
// **Normals now, and depth alone before `gbuffer` existed.** The first version
// reconstructed the slope from depth because there was no normal buffer to
// read; it was a real occlusion term rather than a placeholder, and this is the
// "gets better rather than different" it was written to expect. What the normal
// buys is the hemisphere: occlusion is what blocks light arriving at a surface,
// so only samples on the lit side count, and a depth-only version cannot tell
// which side that is. Flat walls stop occluding themselves at grazing angles.
//
// **It also fixes the sampler count**, which was a latent fault: the catalogue
// declares both inputs as required, the renderer binds every declared read in
// order, and the pipeline was built for however many were wired. A node wiring
// the normal it was told to wire got two bindings and a shader expecting one.
//
// **Sampling in a spiral rather than a fixed kernel.** A fixed offset pattern
// makes a visible grid on flat surfaces; rotating each pixel's spiral by a hash
// of its position turns that into noise, which is what the depth-aware blur the
// catalogue mentions is for. Noise a blur can remove beats structure it cannot.

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outOcclusion;

layout(set = 2, binding = 0) uniform sampler2D depthImage;
layout(set = 2, binding = 1) uniform sampler2D normalImage;

// What this pass is shading. **Every raster pass is given this block**, a
// shipped effect and one an author writes alike — see `PassUniforms` in
// `Renderer.cpp`. A shader that does not declare it is not penalised: the
// pipeline reserves the slot either way and an unused buffer costs nothing,
// which is what keeps `tint.frag` and every custom shader written before this
// existed working unchanged.
layout(set = 3, binding = 0) uniform Pass {
	mat4 ViewProjection;
	mat4 InverseViewProjection;
	vec4 Planes; // near, far, 1/near, 1/far
	vec4 Target; // width, height, 1/width, 1/height
	vec4 View;   // seconds, vertical field of view, aspect, unused
}
pass;

const int SAMPLES = 12;
const float RADIUS = 0.5;
const float BIAS = 0.02;

float Hash(vec2 at) {
	return fract(sin(dot(at, vec2(12.9898, 78.233))) * 43758.5453);
}

// Screen position and linear distance back to a world point. The same
// reconstruction `ssr.frag` makes, and for the same reason.
vec3 WorldAt(vec2 uv, float distance) {
	float near = pass.Planes.x;
	float far = pass.Planes.y;
	float raw = (far - (near * far) / max(distance, 1e-4)) / max(far - near, 1e-6);

	vec4 clip = vec4(uv * 2.0 - 1.0, raw, 1.0);
	vec4 world = pass.InverseViewProjection * clip;
	return world.xyz / max(world.w, 1e-6);
}

void main() {
	float centre = texture(depthImage, inUv).r;

	// **The far plane is not occluded by anything.** Sky pixels have nothing in
	// front of them and shading them dark puts a halo around every silhouette.
	if (centre >= pass.Planes.y) {
		outOcclusion = vec4(1.0);
		return;
	}

	vec3 normal = texture(normalImage, inUv).xyz * 2.0 - 1.0;
	if (dot(normal, normal) < 0.01) {
		outOcclusion = vec4(1.0);
		return;
	}
	normal = normalize(normal);

	vec3 origin = WorldAt(inUv, centre);

	float turn = Hash(inUv) * 6.2831853;
	float occluded = 0.0;

	for (int index = 0; index < SAMPLES; index++) {
		float along = (float(index) + 0.5) / float(SAMPLES);
		float angle = turn + along * 6.2831853 * 2.0;

		// Spiralling outward in screen space, so near samples are dense and far
		// ones sparse — occlusion falls off with distance and the samples should
		// follow it. Scaled by distance so the world radius stays constant:
		// without that, ambient occlusion grows as you walk away from a wall.
		vec2 offset = vec2(cos(angle), sin(angle)) * along * RADIUS / max(centre, 1.0);
		vec2 at = inUv + offset;

		float around = texture(depthImage, at).r;
		if (around >= pass.Planes.y) {
			continue;
		}

		vec3 towards = WorldAt(at, around) - origin;
		float span = length(towards);
		if (span < 1e-4) {
			continue;
		}

		// **The hemisphere test, which is what the normal is for.** A sample
		// behind the surface is on the far side of the wall and occludes
		// nothing; the bias is what stops a flat surface occluding itself out
		// of depth precision alone, which reads as dirt on a clean wall.
		float facing = max(dot(normal, towards / span) - BIAS, 0.0);

		// Range check: something far away is a different surface, not a crease.
		// Without it every silhouette gets a dark outline.
		occluded += facing * smoothstep(1.0, 0.0, span / RADIUS);
	}

	float visibility = 1.0 - (occluded / float(SAMPLES));
	outOcclusion = vec4(clamp(visibility, 0.0, 1.0));
}
