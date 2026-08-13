#pragma once

// Image import to `assets::TextureData`. Format detection uses bytes, and input
// lengths are checked before allocation.
// @tier L9 · shared

#include <engine/assets/Texture.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace engine::bake {

	// Image format.
	enum class ImageFormat : uint8_t {
		// Unsupported or unknown format.
		Unknown,

		// PNG, non-interlaced, 8 or 16 bits a channel.
		Png,

		// Windows BMP, uncompressed, 8, 24 or 32 bits a pixel.
		Bmp,

		// Baseline JPEG, greyscale or YCbCr.
		//
		// Baseline JPEG only.
		Jpeg,

		// GIF87a or GIF89a, decoded into **one flipbook sheet** rather than a
		// list of frames.
		//
		// **The one format here whose output is not a picture of its input**, and
		// that is deliberate: a GIF is a short looping animation and the thing
		// this engine draws animated is a flipbook, so the frames are laid out as
		// a square power-of-two grid and become an ordinary texture that every
		// existing path handles. `bake/src/Gif.cpp` carries the argument and
		// states what it costs — a 12-frame GIF wastes four cells of a 4x4, and
		// anything past 64 frames is truncated.
		//
		// @since v0.10
		Gif,

		// SVG, rasterised into RGBA8 at a size the caller names.
		//
		// **The only format here that is text and the only one with no pixels of
		// its own**, which is why it is the only one `ReadImage` cannot produce:
		// an SVG states a coordinate system, so the raster target is a decision
		// somebody has to take. `Graph`'s `Rasterize` node is where it is taken.
		//
		// A deliberately small subset — shapes, solid paint, `translate` and
		// `scale` — and everything outside it is refused by name rather than
		// approximated. `bake/src/Svg.cpp` carries the list and the argument.
		//
		// @since v0.14
		Svg,
	};

	// Identifies a format from the leading bytes.
	//
	// **Every format here except `Svg` has a signature**, which is what makes
	// this the answer to prefer: a file's own bytes cannot be renamed. SVG is
	// XML and has none, so it is `ImageFormatOfName`'s to identify.
	//
	// @param bytes The file, or as much of its front as is available.
	// @return The format, or `Unknown`.
	ImageFormat ImageFormatOfBytes(std::span<const std::byte> bytes);

	// Identifies a format from a file's name.
	//
	// **This exists for SVG and is spelled for all of them**, the way
	// `ModelFormatOfName` is. The reason it exists at all is that SVG carries no
	// signature: it is XML, so the only honest sniff is a `<svg` or `<?xml`
	// prefix, and a prefix over text claims every text file that starts that way
	// — the argument `Graph`'s import dispatch already makes about `.gltf` being
	// JSON. A `<?xml` sniff would take the next XML-shaped format this module
	// learns to read, whatever it turns out to be, and hand it to the
	// rasteriser.
	//
	// **The bytes are still asked first.** A name is a claim and a signature is
	// evidence, so this is consulted only where there is no evidence to be had.
	//
	// @param name A file name or path. The extension is what is read, and a dot
	//             inside a directory component is not one.
	// @return The format, or `Unknown`.
	// @since v0.14
	ImageFormat ImageFormatOfName(std::string_view name);

	// A name for a format, for a log line and a command line.
	//
	// @param format The format.
	// @return A view valid for the lifetime of the process.
	std::string_view Describe(ImageFormat format);

	// Decodes an image into RGBA8 texture data.
	//
	// **An SVG is refused here rather than given a default size**, because a
	// default would be a number this file invented appearing in somebody's
	// texture. `RasterizeSvg` is the entry that takes one.
	//
	// @param bytes   The file.
	// @param out     Filled on success, left alone on failure.
	// @param failure Set to why on failure, so a bake tool can name the file
	//                *and* the reason. Untouched on success.
	// @return `false` on anything this cannot read or will not trust.
	bool ReadImage(std::span<const std::byte> bytes, assets::TextureData &out, std::string &failure);

	// Rasterises an SVG into RGBA8 at a size.
	//
	// **A vector drawing has no pixels, so the target is a parameter and not a
	// resize afterwards.** Rasterising at the size wanted and rasterising large
	// then box-filtering down are different pictures: the second samples a
	// drawing that was never that sharp, and the edges it produces are the
	// resampler's rather than the shape's.
	//
	// The subset is small and everything outside it is refused by name — see
	// `ImageFormat::Svg` and `bake/src/Svg.cpp`. The document's own counts are
	// all bounded before they are used, and a DOCTYPE or entity declaration is
	// refused outright.
	//
	// @param bytes   The document.
	// @param width   The target width in pixels. Zero, with a zero `height`,
	//                means the size the document declares — which is the only
	//                size it can be said to have.
	// @param height  The target height, under the same rule.
	// @param out     Filled on success, left alone on failure.
	// @param failure Set to why on failure, naming what was refused.
	// @return `false` for a malformed document, an unsupported feature, or a
	//         target that is zero, mixed with a zero, or past
	//         `assets::Texture::MAXIMUM_DIMENSION`.
	// @since v0.14
	bool RasterizeSvg(
		std::span<const std::byte> bytes,
		uint32_t width,
		uint32_t height,
		assets::TextureData &out,
		std::string &failure
	);

}
