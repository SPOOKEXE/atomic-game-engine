#version 450

// The same interface mesh as `interface.vert`, placed onto a plane in the
// world. Positions remain collector pixels in the vertex buffer. One uniform
// maps those pixels onto the collector's full-span world axes, which preserves
// perspective without tessellating the UI differently for every camera.

layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec2 inUv;
layout(location = 2) in vec4 inColour;

layout(location = 0) out vec2 outUv;
layout(location = 1) out vec4 outColour;
layout(location = 2) out vec2 outCanvasPosition;

layout(set = 1, binding = 0) uniform Spatial {
	mat4 ViewProjection;
	vec4 Origin;
	vec4 AxisX;
	vec4 AxisY;
	vec4 Tint;
	vec4 Canvas;
} spatial;

void main() {
	outUv = inUv;
	outColour = inColour * spatial.Tint;
	outCanvasPosition = inPosition;

	const vec2 size = max(spatial.Canvas.xy, vec2(1.0));
	const vec2 local = inPosition / size;
	const vec3 world = spatial.Origin.xyz + spatial.AxisX.xyz * local.x + spatial.AxisY.xyz * local.y;
	gl_Position = spatial.ViewProjection * vec4(world, 1.0);
}
