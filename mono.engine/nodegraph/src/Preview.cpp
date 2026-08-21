// Pictures: which one a payload makes, the software surface rasteriser, and the
// PNG encoder that lets a host write one out with nothing linked.

#include "Internal.hpp"

#include <engine/nodegraph/Preview.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>

namespace engine::nodegraph {

	bool PictureOf(
		const NodeType *type, const std::string &portType, const std::any &payload, PreviewImage &image
	) {
		// **The node's own first, the wire's second.** A type that wants a
		// different picture from everything else on its wire says so, and
		// everything that does not gets the wire's — which is one function
		// serving every node that carries the same thing.
		if (type != nullptr && type->Preview && type->Preview(payload, image)) {
			return true;
		}
		const DataType *carried = DataTypes::Find(portType);
		if (carried != nullptr && carried->Preview) {
			return carried->Preview(payload, image);
		}
		return false;
	}

	bool SurfaceOf(const std::string &portType, const std::any &payload, Surface &out) {
		const DataType *carried = DataTypes::Find(portType);
		if (carried == nullptr || !carried->Heights) {
			return false;
		}
		return carried->Heights(payload, out) && out.Valid();
	}

	namespace {
		// One projected corner of a cell.
		struct Vertex {
			float X = 0.0f;
			float Y = 0.0f;
			float Depth = 0.0f;
			float U = 0.0f;
			float V = 0.0f;
			float Height = 0.0f;
		};

		// How many cells a side the mesh is built at, at most.
		//
		// **A cap and not the field's resolution.** A 512² field is half a
		// million cells for a picture 190 pixels wide, where every triangle
		// lands inside one pixel — the extra work buys nothing a viewer can see.
		constexpr uint32_t MESH_CELLS = 96;

		// Where the light comes from, in view space, already normalised. Over
		// the viewer's left shoulder, which is the convention every terrain tool
		// uses and the one that makes a ridge read as a ridge.
		constexpr float LIGHT_X = -0.512f;
		constexpr float LIGHT_Y = -0.512f;
		constexpr float LIGHT_Z = 0.690f;

		void Shade(const PreviewImage *colour, float u, float v, float height, float &r, float &g, float &b) {
			if (colour != nullptr && colour->Valid()) {
				const auto x =
					static_cast<uint32_t>(std::clamp(u, 0.0f, 0.999f) * static_cast<float>(colour->Side));
				const auto y =
					static_cast<uint32_t>(std::clamp(v, 0.0f, 0.999f) * static_cast<float>(colour->Side));
				const size_t at = (static_cast<size_t>(y) * colour->Side + x) * 4;
				r = static_cast<float>(colour->Rgba[at]) / 255.0f;
				g = static_cast<float>(colour->Rgba[at + 1]) / 255.0f;
				b = static_cast<float>(colour->Rgba[at + 2]) / 255.0f;
				return;
			}

			// **A ramp rather than flat grey**, so a surface with no colour wired
			// into it still reads as terrain: dark low ground to pale peaks.
			const float shade = std::clamp(height, 0.0f, 1.0f);
			r = 0.22f + shade * 0.66f;
			g = 0.24f + shade * 0.64f;
			b = 0.28f + shade * 0.60f;
		}
	}

	bool RenderSurface(
		const Surface &surface,
		const PreviewImage *colour,
		float yaw,
		float pitch,
		float relief,
		uint32_t side,
		PreviewImage &out
	) {
		if (!surface.Valid() || side < 8) {
			return false;
		}

		out.Side = side;
		out.Rgba.assign(static_cast<size_t>(side) * side * 4, 0);

		// The ground the viewport is painted on, so an empty corner is a
		// viewport and not a hole.
		for (size_t at = 0; at < out.Rgba.size(); at += 4) {
			out.Rgba[at] = 10;
			out.Rgba[at + 1] = 11;
			out.Rgba[at + 2] = 14;
			out.Rgba[at + 3] = 255;
		}

		const uint32_t cells = std::min(MESH_CELLS, surface.Side - 1);
		const uint32_t across = cells + 1;

		const float cosYaw = std::cos(yaw);
		const float sinYaw = std::sin(yaw);
		const float clamped = std::clamp(pitch, -1.35f, 1.35f);
		const float cosPitch = std::cos(clamped);
		const float sinPitch = std::sin(clamped);

		// **Orthographic.** A perspective divide buys a little depth cueing and
		// costs a near plane, a field of view and a camera distance to get wrong
		// — none of which a thumbnail-sized viewport can show.
		const float span = static_cast<float>(side);

		// **Sized so the diagonal still fits.** The sheet is one unit square
		// about its middle, so a yaw near 45 degrees presents its corners and is
		// `sqrt(2)` wider than a yaw of zero — scaling to the flat-on width
		// would clip the corners off exactly half the time somebody orbits.
		constexpr float DIAGONAL = 1.41421356f;
		const float scale = span * 0.94f / DIAGONAL;
		const float middle = span * 0.5f;

		std::vector<Vertex> mesh(static_cast<size_t>(across) * across);
		for (uint32_t row = 0; row < across; row++) {
			for (uint32_t column = 0; column < across; column++) {
				const float u = static_cast<float>(column) / static_cast<float>(cells);
				const float v = static_cast<float>(row) / static_cast<float>(cells);

				const auto sx = static_cast<uint32_t>(u * static_cast<float>(surface.Side - 1));
				const auto sy = static_cast<uint32_t>(v * static_cast<float>(surface.Side - 1));
				const float height = surface.At(sx, sy);

				// Centred, so the yaw turns it about its middle rather than
				// swinging it round the corner.
				const float x = u - 0.5f;
				const float y = v - 0.5f;

				// **Elevation about its middle, not from zero.** Measured from
				// zero the whole landscape rises above the centre of the frame
				// and leaves the bottom third empty, which reads as a badly
				// aimed camera rather than as terrain.
				const float z = (height - 0.5f) * relief;

				const float rx = x * cosYaw - y * sinYaw;
				const float ry = x * sinYaw + y * cosYaw;

				// Tilt: the plane leans away and the height comes towards the
				// viewer, which is what makes it a landscape rather than a map.
				const float ty = ry * sinPitch - z * cosPitch;
				const float depth = ry * cosPitch + z * sinPitch;

				Vertex &vertex = mesh[static_cast<size_t>(row) * across + column];
				vertex.X = middle + rx * scale;
				vertex.Y = middle + ty * scale;
				vertex.Depth = depth;
				vertex.U = u;
				vertex.V = v;
				vertex.Height = height;
			}
		}

		// **Painted with a depth buffer rather than back to front.** A height
		// field folds over itself at a low pitch, and a painter's-algorithm sort
		// gets exactly those cells wrong — which is the only place anybody looks.
		std::vector<float> depths(static_cast<size_t>(side) * side, 1e30f);

		const auto triangle = [&](const Vertex &a, const Vertex &b, const Vertex &c) {
			const float area = (b.X - a.X) * (c.Y - a.Y) - (c.X - a.X) * (b.Y - a.Y);
			if (std::fabs(area) < 1e-6f) {
				return;
			}

			// The normal of the projected triangle stands in for the surface's,
			// which is exact for a flat facet and is what a flat-shaded mesh is.
			float nx = (b.Y - a.Y) * (c.Depth - a.Depth) - (b.Depth - a.Depth) * (c.Y - a.Y);
			float ny = (b.Depth - a.Depth) * (c.X - a.X) - (b.X - a.X) * (c.Depth - a.Depth);
			float nz = (b.X - a.X) * (c.Y - a.Y) - (b.Y - a.Y) * (c.X - a.X);
			const float length = std::sqrt(nx * nx + ny * ny + nz * nz);
			if (length > 1e-9f) {
				nx /= length;
				ny /= length;
				nz /= length;
			}

			// Two-sided, because the underside of a fold is still something to
			// look at rather than a black hole.
			float lit = std::fabs(nx * LIGHT_X + ny * LIGHT_Y + nz * LIGHT_Z);
			lit = 0.30f + lit * 0.85f;

			const auto low = [](float one, float two, float three) {
				return std::min(one, std::min(two, three));
			};
			const auto high = [](float one, float two, float three) {
				return std::max(one, std::max(two, three));
			};

			const int left = std::max(0, static_cast<int>(std::floor(low(a.X, b.X, c.X))));
			const int right =
				std::min(static_cast<int>(side) - 1, static_cast<int>(std::ceil(high(a.X, b.X, c.X))));
			const int top = std::max(0, static_cast<int>(std::floor(low(a.Y, b.Y, c.Y))));
			const int bottom =
				std::min(static_cast<int>(side) - 1, static_cast<int>(std::ceil(high(a.Y, b.Y, c.Y))));

			for (int y = top; y <= bottom; y++) {
				for (int x = left; x <= right; x++) {
					const float px = static_cast<float>(x) + 0.5f;
					const float py = static_cast<float>(y) + 0.5f;

					const float w0 = ((b.X - px) * (c.Y - py) - (c.X - px) * (b.Y - py)) / area;
					const float w1 = ((c.X - px) * (a.Y - py) - (a.X - px) * (c.Y - py)) / area;
					const float w2 = 1.0f - w0 - w1;
					if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) {
						continue;
					}

					const float depth = w0 * a.Depth + w1 * b.Depth + w2 * c.Depth;
					const size_t at = static_cast<size_t>(y) * side + static_cast<size_t>(x);
					if (depth >= depths[at]) {
						continue;
					}
					depths[at] = depth;

					float r = 0.0f;
					float g = 0.0f;
					float blue = 0.0f;
					Shade(
						colour,
						w0 * a.U + w1 * b.U + w2 * c.U,
						w0 * a.V + w1 * b.V + w2 * c.V,
						w0 * a.Height + w1 * b.Height + w2 * c.Height,
						r,
						g,
						blue
					);

					const size_t pixel = at * 4;
					out.Rgba[pixel] = static_cast<uint8_t>(std::clamp(r * lit, 0.0f, 1.0f) * 255.0f);
					out.Rgba[pixel + 1] = static_cast<uint8_t>(std::clamp(g * lit, 0.0f, 1.0f) * 255.0f);
					out.Rgba[pixel + 2] = static_cast<uint8_t>(std::clamp(blue * lit, 0.0f, 1.0f) * 255.0f);
					out.Rgba[pixel + 3] = 255;
				}
			}
		};

		for (uint32_t row = 0; row < cells; row++) {
			for (uint32_t column = 0; column < cells; column++) {
				const Vertex &a = mesh[static_cast<size_t>(row) * across + column];
				const Vertex &b = mesh[static_cast<size_t>(row) * across + column + 1];
				const Vertex &c = mesh[static_cast<size_t>(row + 1) * across + column];
				const Vertex &d = mesh[static_cast<size_t>(row + 1) * across + column + 1];
				triangle(a, b, c);
				triangle(b, d, c);
			}
		}

		return true;
	}

	namespace {
		// --- writing a PNG --------------------------------------------------
		//
		// **Stored deflate blocks, so nothing has to be linked.** A real PNG is
		// what somebody expects an exported picture to be, and a library that
		// pulled in zlib to write one would be a dependency in everybody's build
		// for a file dialog.
		//
		// Deflate's stored block is legal, universally understood and needs no
		// entropy coder: the cost is a file about a third larger than a
		// compressed one, for a picture somebody asked to look at rather than
		// ship. `assetc` is where a compressed one would belong.

		uint32_t Crc32(const uint8_t *bytes, size_t count, uint32_t running = 0xFFFFFFFFu) {
			// Built once, because it is 256 entries and this is called four
			// times per export.
			static const std::array<uint32_t, 256> TABLE = [] {
				std::array<uint32_t, 256> table{};
				for (uint32_t index = 0; index < 256; index++) {
					uint32_t value = index;
					for (int bit = 0; bit < 8; bit++) {
						value = (value & 1u) != 0u ? 0xEDB88320u ^ (value >> 1) : value >> 1;
					}
					table[index] = value;
				}
				return table;
			}();

			for (size_t at = 0; at < count; at++) {
				running = TABLE[(running ^ bytes[at]) & 0xFFu] ^ (running >> 8);
			}
			return running;
		}

		void PutBigEndian32(std::vector<uint8_t> &into, uint32_t value) {
			into.push_back(static_cast<uint8_t>((value >> 24) & 0xFFu));
			into.push_back(static_cast<uint8_t>((value >> 16) & 0xFFu));
			into.push_back(static_cast<uint8_t>((value >> 8) & 0xFFu));
			into.push_back(static_cast<uint8_t>(value & 0xFFu));
		}

		// One chunk: length, type, payload, and a CRC over the type and the
		// payload but not the length — which is the part everybody gets wrong.
		void PutChunk(std::vector<uint8_t> &into, const char (&type)[5], const std::vector<uint8_t> &body) {
			PutBigEndian32(into, static_cast<uint32_t>(body.size()));
			const size_t from = into.size();
			into.insert(into.end(), type, type + 4);
			into.insert(into.end(), body.begin(), body.end());
			PutBigEndian32(into, Crc32(into.data() + from, into.size() - from) ^ 0xFFFFFFFFu);
		}
	}

	std::vector<uint8_t> EncodePng(const PreviewImage &image) {
		// **Guarded, because the loop below trusts `Side` and reads from
		// `Rgba`.** An image whose buffer does not match the side it claims -
		// a `PreviewImage` that was default-constructed, or one whose maker
		// returned false and left it half-filled - makes the `insert` below
		// build iterators past the end of `Rgba` and read through them. That is
		// undefined behaviour rather than a short file, and it was a segfault.
		//
		// Empty rather than a partial PNG: a truncated image that some viewers
		// still open is worse than no file, because the failure moves to
		// whoever opens it later.
		if (!image.Valid()) {
			return {};
		}

		// The raw scanlines, each with its filter byte. Filter 0 — none —
		// because a stored block gains nothing from a filter that only helps
		// an entropy coder.
		std::vector<uint8_t> raw;
		raw.reserve((static_cast<size_t>(image.Side) * 4 + 1) * image.Side);
		for (uint32_t y = 0; y < image.Side; y++) {
			raw.push_back(0);
			const size_t row = static_cast<size_t>(y) * image.Side * 4;
			raw.insert(
				raw.end(),
				image.Rgba.begin() + static_cast<long>(row),
				image.Rgba.begin() + static_cast<long>(row + image.Side * 4)
			);
		}

		// Adler-32 over the uncompressed bytes, which is what closes a zlib
		// stream.
		uint32_t low = 1;
		uint32_t high = 0;
		for (const uint8_t byte : raw) {
			low = (low + byte) % 65521u;
			high = (high + low) % 65521u;
		}

		std::vector<uint8_t> zlib;
		zlib.push_back(0x78);
		zlib.push_back(0x01);

		// 65535 is the largest a stored block may carry, and its length is
		// written little-endian followed by its ones' complement.
		constexpr size_t BLOCK = 65535;
		for (size_t at = 0; at < raw.size(); at += BLOCK) {
			const size_t count = std::min(BLOCK, raw.size() - at);
			const bool last = at + count >= raw.size();
			zlib.push_back(last ? 1 : 0);
			zlib.push_back(static_cast<uint8_t>(count & 0xFFu));
			zlib.push_back(static_cast<uint8_t>((count >> 8) & 0xFFu));
			zlib.push_back(static_cast<uint8_t>(~count & 0xFFu));
			zlib.push_back(static_cast<uint8_t>((~count >> 8) & 0xFFu));
			zlib.insert(
				zlib.end(), raw.begin() + static_cast<long>(at), raw.begin() + static_cast<long>(at + count)
			);
		}
		PutBigEndian32(zlib, (high << 16) | low);

		std::vector<uint8_t> file{0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};

		std::vector<uint8_t> header;
		PutBigEndian32(header, image.Side);
		PutBigEndian32(header, image.Side);
		header.push_back(8);
		header.push_back(6);
		header.push_back(0);
		header.push_back(0);
		header.push_back(0);

		PutChunk(file, "IHDR", header);
		PutChunk(file, "IDAT", zlib);
		PutChunk(file, "IEND", {});
		return file;
	}

	uint64_t PictureKey(uint64_t ran, const std::string &port) {
		return MixText(Mix(SEED, &ran, sizeof(ran)), port);
	}

	std::string DescribeValue(const std::string &portType, const std::any &payload) {
		if (!payload.has_value()) {
			return "not evaluated";
		}
		const DataType *carried = DataTypes::Find(portType);
		if (carried != nullptr && carried->Describe) {
			return carried->Describe(payload);
		}

		// Nobody taught this type. A number is worth reading out anyway — it is
		// what most unlabelled payloads turn out to be — and everything else gets
		// the only true thing left, which is that something is there.
		if (const double *number = std::any_cast<double>(&payload); number != nullptr) {
			char text[32];
			std::snprintf(text, sizeof(text), "%.4f", *number);
			return text;
		}
		return "a value";
	}
}
