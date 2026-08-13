#version 450

// Writes nothing. The depth attachment is the output.
//
// **A fragment shader is still required**, even for a depth-only pass: SDL's
// GPU API takes both stages when it creates a graphics pipeline, and a null
// fragment stage is not one of the things it accepts. An empty `main` is what
// every backend compiles this to anyway.

layout(location = 0) in vec3 inWorldPosition;

// The half-space this draw keeps, matching `opaque.frag`'s `SeamPlane`.
//
// **One `vec4` and no samplers**, which is why the shadow pipeline takes a
// fragment uniform buffer and nothing else. `Renderer::Impl::DrawSlots` pushes
// the slot's own plane here on the depth-only path, exactly as it pushes the
// whole lighting block on the colour one.
layout(set = 3, binding = 0) uniform Seam {
	vec4 Plane;
} seam;

void main() {
	if (dot(seam.Plane.xyz, seam.Plane.xyz) > 0.0 &&
		dot(inWorldPosition, seam.Plane.xyz) < seam.Plane.w) {
		discard;
	}
}
