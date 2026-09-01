#version 450

// The ground grid, drawn as a plane rather than as lines.
//
// **A fullscreen triangle that finds the ground for itself.** Every pixel casts
// its own ray through the inverse view-projection, intersects `y = 0`, and asks
// whether that world point is on a grid line. There is no vertex buffer, no
// line list, and nothing to upload when the camera moves - which is the whole
// reason the grid can be depth-tested at all. The editor's grid was an imgui
// overlay drawn after the world, and an overlay has no depth buffer to test
// against, so it drew over the geometry it should have been under.
//
// **`gl_FragDepth` is what makes the hardware do the occluding.** The plane's
// own clip depth is written per pixel, so the depth test the pipeline was
// created with compares it against whatever the opaque pass left - a wall in
// front of the grid hides it, exactly as a wall hides anything else. Nothing is
// sampled and nothing is compared by hand.
//
// **The line width is a screen-space width, from `fwidth`.** A grid drawn as
// world-space quads is a pixel wide near the camera and a fifth of one at the
// horizon, which aliases into a shimmering mess. Dividing the distance to the
// nearest line by its own screen derivative gives a line that is the same
// weight everywhere and fades out on its own when the cells become smaller than
// a pixel - which is the analytic form of a mip chain and needs no texture.

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColour;

layout(set = 3, binding = 0) uniform Grid {
	// The pair, because the ray needs one and the depth needs the other.
	//@{
	mat4 ViewProjection;
	mat4 InverseViewProjection;
	//@}

	// Where the eye is, for the distance fade. `w` is unused.
	vec4 Eye;

	// x: studs between thin lines. y: how many cells to a heavy one.
	// z: studs at which the grid has faded out. w: overall strength.
	vec4 Params;

	// x/y: the authored X/Z origin for line and axis placement.
	vec4 Offset;

	// The line colour, with `a` the alpha a heavy line reaches.
	vec4 Colour;

	// The two axis colours, which are the one thing here that is not the grid:
	// X red and Z blue, the convention every editor uses.
	//@{
	vec4 AxisX;
	vec4 AxisZ;
	//@}
} grid;

// How close this pixel is to a line of the given spacing, as coverage in
// `0..1`. One at the centre of a line, zero a pixel away from it.
float Coverage(vec2 at, float spacing) {
	vec2 cell = at / spacing;
	vec2 width = fwidth(cell);

	// **Guarded, because `fwidth` is zero where the plane is edge-on.** A
	// derivative of zero divides to infinity, which reads as a solid sheet of
	// grid across the horizon rather than as a line.
	width = max(width, vec2(1e-6));

	const vec2 distance = abs(fract(cell - 0.5) - 0.5) / width;
	return 1.0 - min(min(distance.x, distance.y), 1.0);
}

void main() {
	// The uv the fullscreen triangle hands over is `0..1` with the origin at
	// the top left, and clip space is Y-up - `overlay.vert` says so where it
	// builds the position, and this is the same flip read backwards.
	const vec2 ndc = vec2(inUv.x * 2.0 - 1.0, 1.0 - inUv.y * 2.0);

	const vec4 nearPoint = grid.InverseViewProjection * vec4(ndc, 0.0, 1.0);
	const vec4 farPoint = grid.InverseViewProjection * vec4(ndc, 1.0, 1.0);
	if (nearPoint.w == 0.0 || farPoint.w == 0.0) {
		discard;
	}

	const vec3 from = nearPoint.xyz / nearPoint.w;
	const vec3 to = farPoint.xyz / farPoint.w;
	const vec3 along = to - from;

	// **Parallel to the ground is not "somewhere very far away", it is
	// nowhere.** A ray within a hair of the plane produces an intersection
	// millions of studs out, whose derivatives are meaningless and whose line
	// coverage is whatever the arithmetic happened to give.
	if (abs(along.y) < 1e-6) {
		discard;
	}

	// The plane is `y = 0`. Outside `0..1` the intersection is behind the near
	// plane or past the far one, which is a pixel the grid does not reach.
	const float step = -from.y / along.y;
	if (step < 0.0 || step > 1.0) {
		discard;
	}

	const vec3 ground = from + along * step;

	const vec4 clip = grid.ViewProjection * vec4(ground, 1.0);
	if (clip.w <= 0.0) {
		discard;
	}

	const float spacing = max(grid.Params.x, 1e-3);
	const vec2 gridPoint = ground.xz - grid.Offset.xy;
	const float heavy = Coverage(gridPoint, spacing * max(grid.Params.y, 1.0));
	const float thin = Coverage(gridPoint, spacing);

	// **Radial from the eye, and squared.** A grid that ended in a hard
	// rectangle is what a linear fade along one axis gives; the square falls
	// off faster near the edge, so the last cells disappear rather than stop.
	const float reach = max(grid.Params.z, 1.0);
	const float away = length(ground.xz - grid.Eye.xz);
	float fade = 1.0 - clamp(away / reach, 0.0, 1.0);
	fade *= fade;

	// A heavy line reaches the full alpha and a thin one half of it, which is
	// the weighting the editor's overlay used and the reason a grid reads as a
	// grid rather than as graph paper.
	float alpha = max(heavy, thin * 0.5) * fade * grid.Params.w * grid.Colour.a;

	vec3 colour = grid.Colour.rgb;

	// The two axes, over the top of the grid. Their own spacing is meaningless
	// - there is one of each - so the coverage is distance to zero over its own
	// derivative, which is the same line width by the same rule.
	const vec2 width = max(fwidth(gridPoint), vec2(1e-6));
	const float onAxisX = 1.0 - min(abs(gridPoint.y) / width.y, 1.0);
	const float onAxisZ = 1.0 - min(abs(gridPoint.x) / width.x, 1.0);

	if (onAxisX > 0.0) {
		colour = mix(colour, grid.AxisX.rgb, onAxisX);
		alpha = max(alpha, onAxisX * fade * grid.Params.w * grid.AxisX.a);
	}
	if (onAxisZ > 0.0) {
		colour = mix(colour, grid.AxisZ.rgb, onAxisZ);
		alpha = max(alpha, onAxisZ * fade * grid.Params.w * grid.AxisZ.a);
	}

	if (alpha <= 0.002) {
		discard;
	}

	// **Written, so the depth test can do the occluding.** Everything above is
	// about where the plane is; this is the one line that puts it in the scene
	// rather than over it.
	gl_FragDepth = clip.z / clip.w;

	outColour = vec4(colour, alpha);
}
