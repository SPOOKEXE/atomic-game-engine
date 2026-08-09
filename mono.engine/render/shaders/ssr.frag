#version 450

// Screen-space reflections: the frame reflecting itself.
//
// **The other pass the G-buffer unblocked.** It needs a normal per pixel to
// know which way to reflect and linear depth to know what the ray hit, and
// before `gbuffer` neither existed. It reflects what is *on screen* and nothing
// else — that is the technique's defining limit, not a shortcoming of this
// implementation, and it is why a reflection fades out at the screen edge here
// rather than stopping dead.
//
// **Marched in screen space rather than in world space.** A world-space march
// has to project every stride to sample the depth buffer; stepping across the
// screen and interpolating depth does the projection once, at the ends. The
// view-projection and its inverse come from the pass block, which is what makes
// either possible at all.

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outReflection;

layout(set = 2, binding = 0) uniform sampler2D colourImage;
layout(set = 2, binding = 1) uniform sampler2D depthImage;
layout(set = 2, binding = 2) uniform sampler2D normalImage;

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

const int STEPS = 24;
const float THICKNESS = 0.5;
const float MAX_DISTANCE = 30.0;

// Screen position and linear depth back to a world point.
vec3 WorldAt(vec2 uv, float distance) {
	// The inverse projection wants a clip position, so the linear distance goes
	// back through the same formula `depth-linearise` reversed.
	float near = pass.Planes.x;
	float far = pass.Planes.y;
	float raw = (far - (near * far) / max(distance, 1e-4)) / max(far - near, 1e-6);

	vec4 clip = vec4(uv * 2.0 - 1.0, raw, 1.0);
	vec4 world = pass.InverseViewProjection * clip;
	return world.xyz / max(world.w, 1e-6);
}

void main() {
	vec4 packed = texture(normalImage, inUv);
	vec3 normal = packed.xyz * 2.0 - 1.0;

	float distance = texture(depthImage, inUv).r;

	// Nothing was drawn here, so there is no surface to reflect off — the sky
	// does not reflect the world. `deferred-lighting.frag` makes the same test
	// for the same reason.
	if (dot(normal, normal) < 0.01 || distance >= pass.Planes.y) {
		outReflection = vec4(0.0);
		return;
	}
	normal = normalize(normal);

	vec3 origin = WorldAt(inUv, distance);

	// The eye is where the inverse projection puts the near plane under this
	// pixel; the view ray is from there to the surface.
	vec3 eye = WorldAt(inUv, pass.Planes.x);
	vec3 towards = normalize(origin - eye);
	vec3 ray = reflect(towards, normal);

	// **A ray heading towards the camera cannot be traced on screen.** What it
	// would hit is behind the viewer, which no buffer holds.
	if (dot(ray, -towards) > 0.0) {
		outReflection = vec4(0.0);
		return;
	}

	vec4 target = pass.ViewProjection * vec4(origin + ray * MAX_DISTANCE, 1.0);
	if (target.w <= 0.0) {
		outReflection = vec4(0.0);
		return;
	}
	vec2 endUv = (target.xy / target.w) * 0.5 + 0.5;

	vec2 stride = (endUv - inUv) / float(STEPS);

	for (int index = 1; index <= STEPS; index++) {
		vec2 at = inUv + stride * float(index);
		if (at.x < 0.0 || at.x > 1.0 || at.y < 0.0 || at.y > 1.0) {
			break;
		}

		// Where the ray is at this stride, and what the depth buffer says is
		// actually there. The ray is behind the surface when it is further than
		// what was drawn — by more than a sliver, or every surface hits itself.
		vec3 along = origin + ray * (MAX_DISTANCE * float(index) / float(STEPS));
		vec4 projected = pass.ViewProjection * vec4(along, 1.0);
		float rayDistance = projected.w;
		float sceneDistance = texture(depthImage, at).r;

		if (rayDistance > sceneDistance && rayDistance - sceneDistance < THICKNESS) {
			// **Faded at the edges of the screen**, because the reflection
			// stops existing there rather than stopping being reflective. A
			// hard cut off is the artefact that makes screen-space reflection
			// obvious; this makes it merely absent.
			vec2 fade = smoothstep(vec2(0.0), vec2(0.15), at) *
						smoothstep(vec2(0.0), vec2(0.15), 1.0 - at);
			outReflection = vec4(texture(colourImage, at).rgb, fade.x * fade.y);
			return;
		}
	}

	outReflection = vec4(0.0);
}
