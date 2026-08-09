#version 450

// Shades the G-buffer: one pass over the screen, however much geometry went in.
//
// **This is what the G-buffer was for.** A forward pass shades once per
// fragment, so overlapping geometry pays for light it never shows — `§1.5`
// fault 9 measured 21% of the most expensive material area shaded twice in a
// real frame. Deferred shades once per pixel, because by the time this runs the
// depth test has already decided which surface won.
//
// **Reconstructing the normal rather than the position.** The albedo, normal and
// material targets are all this needs for a directional light; a position would
// need the inverse view-projection and a depth fetch, and the sun does not care
// where a pixel is — only which way it faces. A point light would, and that is
// the uniform this pass gains when there is one.
//
// Shipped by the engine as `deferred-lighting`'s default.

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColour;

layout(set = 2, binding = 0) uniform sampler2D albedoImage;
layout(set = 2, binding = 1) uniform sampler2D normalImage;
layout(set = 2, binding = 2) uniform sampler2D materialImage;

// **Depth, which this shades nothing without.** The catalogue declares it
// required and the renderer binds every declared read in order, so a node
// wiring what it was told to wire hands this pass four textures — a shader
// declaring three would be built for a binding count it does not have. It earns
// its place as well as filling it: a zero-length normal says "nothing here" for
// geometry that was never drawn, and depth says the same for the sky, which is
// the case a G-buffer cleared to zero cannot otherwise distinguish from a
// surface facing exactly away.
layout(set = 2, binding = 3) uniform sampler2D depthImage;

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

// **The same sun the forward pass uses**, and it is a constant here for the same
// reason it is a constant there: one directional light, declared once. A light
// list is a `Buffer` input and a different pass.
const vec3 SUN_DIRECTION = normalize(vec3(-0.45, -0.85, -0.28));
const vec3 SUN_AMBIENT = vec3(0.28, 0.30, 0.36);

void main() {
	vec4 albedo = texture(albedoImage, inUv);
	vec4 packed = texture(normalImage, inUv);
	vec4 material = texture(materialImage, inUv);

	// **Nothing was written here.** The G-buffer clears to zero and a pixel no
	// geometry covered has a zero-length normal — shading it would light the
	// background as if it faced the camera. Left transparent so whatever drew
	// the sky shows through.
	vec3 normal = packed.xyz * 2.0 - 1.0;
	// **Against the far plane and not against 1.0**, because the catalogue
	// declares this input as `R32` linear depth — metres, not the raw buffer.
	// Testing it as if it were normalised would call everything past one metre
	// sky and leave the frame black.
	if (dot(normal, normal) < 0.01 || texture(depthImage, inUv).r >= pass.Planes.y) {
		outColour = vec4(0.0);
		return;
	}
	normal = normalize(normal);

	float facing = max(dot(normal, -SUN_DIRECTION), 0.0);

	// Roughness is `material.r`. A rough surface scatters, so its highlight
	// spreads and dims; this is the cheapest expression of that which is not
	// simply wrong, and it is what the real PBR term replaces once the material
	// maps are read rather than written as constants.
	float roughness = clamp(material.r, 0.04, 1.0);
	float gloss = pow(facing, mix(64.0, 2.0, roughness)) * (1.0 - roughness);

	// **Emission is added after shading and is not scaled by the light**, which
	// is the whole point of the term: a sign is as bright at midnight as at
	// noon. Its hue is the albedo's, because `gbuffer.frag` stores strength
	// rather than colour — see the note there for why that is not a shortcut.
	//
	// **Occlusion multiplies the ambient only.** Ambient occlusion describes how
	// much of the sky a point can see; applying it to the sun as well would
	// darken surfaces the sun is directly hitting, which is the mistake that
	// makes an occlusion pass read as dirt.
	vec3 lit = albedo.rgb * (SUN_AMBIENT * material.b + facing) + vec3(gloss) * 0.25;
	lit += albedo.rgb * material.a;

	outColour = vec4(lit, albedo.a);
}
