#pragma once

// Turning what a node produced into a picture, without a device.
//
// **The library never learns what a payload is**, so a picture comes from the
// wire's `DataType::Preview` or the node type's own. `PictureOf` is the one
// function both go through, which is what stops a canvas thumbnail and an
// inspector strip disagreeing about what a node looks like.
//
// Everything here produces a `PreviewImage`: pixels, on the calling thread, with
// no ImGui and no GPU in the file. What a host does with them (upload, cache,
// draw) is `ImageSink`, and that is the whole of the seam.

#include <any>
#include <cstdint>
#include <functional>
#include <nodegraph/Registry.hpp>
#include <string>
#include <vector>

namespace nodegraph {

	// Turns a preview into something the canvas can draw.
	//
	// **The library holds no device**, so the host says how a picture becomes a
	// texture. `key` is the hash the payload was computed at: a sink is expected
	// to keep what it made under that key and call `make` only on a miss, which
	// is what stops a thumbnail being rebuilt sixty times a second.
	//
	// @return Whatever the host's ImGui backend takes as a texture id, or null.
	using ImageSink = std::function<void *(uint64_t key, const std::function<bool(PreviewImage &)> &make)>;

	// The node type's own `Preview` first, then the port's `DataType::Preview`.
	// One function, so the canvas's thumbnail and the inspector's strip can
	// never disagree about what a node looks like.
	bool PictureOf(
		const NodeType *type, const std::string &portType, const std::any &payload, PreviewImage &image
	);

	// What a payload is, in one line. `DataType::Describe` where there is one,
	// and otherwise the only true thing left.
	std::string DescribeValue(const std::string &portType, const std::any &payload);

	// Reads a payload as elevation, through its wire's `DataType::Heights`.
	bool SurfaceOf(const std::string &portType, const std::any &payload, Surface &out);

	// Draws a lit surface into a square picture.
	//
	// **A software rasteriser, and deliberately.** The alternative is a render
	// target, a camera and a mesh upload inside a panel: a device dependency
	// for a 190-pixel square, and a second path for a picture that already has
	// one. This produces a `PreviewImage` like everything else here, so the
	// texture, the caching and the drawing are the code that was already there,
	// and it can be checked with no window.
	//
	// @param surface The heights to draw.
	// @param colour Optional albedo, sampled across the surface: the same
	//               square the 2-D view shows. Grey ramp without one.
	// @param yaw    Rotation about the vertical, radians.
	// @param pitch  Tilt towards the viewer, radians, clamped to a sane range.
	// @param relief How tall the height range stands, in units of the surface's
	//               width. 0 is flat.
	// @param side   The square picture's edge, in pixels.
	// @param out    The picture to draw into. Resized to `side`.
	bool RenderSurface(
		const Surface &surface,
		const PreviewImage *colour,
		float yaw,
		float pitch,
		float relief,
		uint32_t side,
		PreviewImage &out
	);

	// Writes a picture as a PNG.
	//
	// **Stored deflate blocks, so nothing has to be linked.** A real PNG is what
	// somebody expects an exported picture to be, and a library that pulled in
	// zlib to save one would be a dependency in everybody's build for a file
	// dialog. The cost is a file about a third larger than a compressed one, for
	// a picture somebody asked to look at rather than ship.
	std::vector<uint8_t> EncodePng(const PreviewImage &image);

	// The key a picture is held under: the hash its payload was computed at, and
	// which port it left by.
	//
	// **Both, because one run produces every output at once.** A key that was
	// only the hash would hand a node's second port whichever picture its first
	// one made, and the two would then differ for as long as the cache held.
	uint64_t PictureKey(uint64_t ran, const std::string &port);
}
