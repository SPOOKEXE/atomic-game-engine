#version 450

// Counts shading rather than doing any.
//
// **Fault 9 of `docs/PIPELINE_NODES.md` §1.5, which is the one a graph cannot
// answer.** The captured frame had no depth pre-pass, so 21% of its most
// expensive material area was shaded more than once; adding a partial pre-pass
// bought 7%, nearly a full millisecond. Nothing about the authored graph says
// that — it is a property of what the geometry happens to overlap, and the only
// way to see it is to count.
//
// **One over 255, added.** The target is `R8_UNORM` with additive blending, so
// each fragment that would have been shaded contributes exactly one step and the
// readback multiplies back up. 255 layers saturate, which is far past the point
// where the number stops being interesting.
//
// **The vertex stage is `opaque.vert` unchanged**, so what is counted is what
// that pass would actually shade — the same instancing, the same clip. A
// separate vertex shader would be a second description of where the geometry is,
// and the whole point is to measure the first one.
//
// The vertex outputs are deliberately not consumed. A fragment stage need not
// read every varying, and declaring them here to ignore them would be six lines
// that only exist to be discarded.

layout(location = 0) out vec4 outCount;

void main() {
	outCount = vec4(1.0 / 255.0, 0.0, 0.0, 0.0);
}
