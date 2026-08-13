#include <engine/assets/Builtin.hpp>
#include <engine/assets/Resample.hpp>

#include <array>
#include <cmath>
#include <numbers>
#include <utility>

namespace engine::assets {

	namespace {
		// The names, indexed by the enum. A table rather than a switch so that
		// the name and the parse cannot disagree — the parse walks this.
		constexpr std::array<std::string_view, BUILTIN_MESH_COUNT> NAMES{
			"engine.Cube",
			"engine.Plane",
			"engine.Wedge",
			"engine.CornerWedge",
			"engine.Sphere",
			"engine.Cylinder",
		};

		// The texture names, indexed by the enum, for `NAMES`' reason.
		constexpr std::array<std::string_view, BUILTIN_TEXTURE_COUNT> TEXTURE_NAMES{
			"engine.Checker",
		};

		// How wide the built-in sheets are, in pixels, and how wide one check
		// is. Square, and the check divides the side, so the pattern repeats
		// across a tiled surface with no seam at the join.
		constexpr uint32_t TEXTURE_SIDE = 64;
		constexpr uint32_t CHECK = 8;

		// The two colours, as RGBA. A mid pink and a mid grey: light enough to
		// read the shading of the surface under them, far enough apart to count
		// the checks at a distance, and neither of them the full-saturation
		// magenta that means something has gone wrong.
		constexpr std::array<uint8_t, 4> PINK{0xE8, 0x8A, 0xB0, 0xFF};
		constexpr std::array<uint8_t, 4> GREY{0x96, 0x96, 0x9B, 0xFF};

		TextureData MakeChecker() {
			TextureData image;
			image.Width = TEXTURE_SIDE;
			image.Height = TEXTURE_SIDE;
			image.Format = TextureFormat::RGBA8;
			image.Pixels.resize(static_cast<size_t>(TEXTURE_SIDE) * TEXTURE_SIDE * 4);

			for (uint32_t y = 0; y < TEXTURE_SIDE; y++) {
				for (uint32_t x = 0; x < TEXTURE_SIDE; x++) {
					const bool pink = ((x / CHECK) + (y / CHECK)) % 2 == 0;
					const std::array<uint8_t, 4> &colour = pink ? PINK : GREY;

					const size_t at = (static_cast<size_t>(y) * TEXTURE_SIDE + x) * 4;
					for (size_t channel = 0; channel < colour.size(); channel++) {
						image.Pixels[at + channel] = static_cast<std::byte>(colour[channel]);
					}
				}
			}

			// **The chain is built here rather than left to whoever uploads it.**
			// A built-in is the one texture that reaches a sampler without passing
			// through `bake`, so there is no pipeline stage to put a `Mipmap` node
			// in — and the checker is exactly the sheet an author tiles across a
			// floor and then looks at from across the map, which is where a single
			// level shimmers worst. Cannot fail on an image this function just
			// built, and a failure would leave the sheet without levels rather
			// than invalid, so the answer is ignored deliberately.
			BuildMipChain(image);
			return image;
		}

		// How many segments a round built-in is cut into around its axis.
		//
		// **Fixed rather than a parameter**, because a built-in has to be the
		// same mesh in every process: a client that tessellated a sphere more
		// finely than the publisher would compute a different bounding box and
		// cull differently. Twenty-four reads as round at the sizes a part is
		// drawn at and costs 24 * 17 vertices for a sphere, which is under a
		// thousand.
		constexpr uint32_t SEGMENTS = 24;

		// How many bands a sphere is cut into from pole to pole. Two thirds of
		// the segment count, because a sphere spans half a turn vertically and
		// a full one horizontally, and equal spacing wants that ratio.
		constexpr uint32_t RINGS = 16;

		constexpr float PI = std::numbers::pi_v<float>;

		MeshVertex
		Made(float x, float y, float z, float normalX, float normalY, float normalZ, float u, float v) {
			MeshVertex vertex{};
			vertex.Position[0] = x;
			vertex.Position[1] = y;
			vertex.Position[2] = z;
			vertex.Normal[0] = normalX;
			vertex.Normal[1] = normalY;
			vertex.Normal[2] = normalZ;
			vertex.TexCoord[0] = u;
			vertex.TexCoord[1] = v;
			return vertex;
		}

		// Appends four vertices and the two triangles over them.
		//
		// The corners are given in winding order, so `0-1-2, 0-2-3` is
		// counter-clockwise seen from the side the normal points at. Every flat
		// built-in is built out of this call, which is what makes the winding
		// one decision rather than one per face.
		void AddQuad(
			MeshData &data,
			const std::array<float, 3> &a,
			const std::array<float, 3> &b,
			const std::array<float, 3> &c,
			const std::array<float, 3> &d,
			const std::array<float, 3> &normal
		) {
			const uint32_t base = static_cast<uint32_t>(data.Vertices.size());

			// Each face owns its four vertices so its normal stays flat rather
			// than being averaged across a shared corner — the reason the cube
			// has always been twenty-four vertices and not eight.
			data.Vertices.push_back(Made(a[0], a[1], a[2], normal[0], normal[1], normal[2], 0.0f, 1.0f));
			data.Vertices.push_back(Made(b[0], b[1], b[2], normal[0], normal[1], normal[2], 0.0f, 0.0f));
			data.Vertices.push_back(Made(c[0], c[1], c[2], normal[0], normal[1], normal[2], 1.0f, 0.0f));
			data.Vertices.push_back(Made(d[0], d[1], d[2], normal[0], normal[1], normal[2], 1.0f, 1.0f));

			data.Indices.insert(data.Indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
		}

		// Appends three vertices and the triangle over them, wound as given.
		void AddTriangle(
			MeshData &data,
			const std::array<float, 3> &a,
			const std::array<float, 3> &b,
			const std::array<float, 3> &c,
			const std::array<float, 3> &normal
		) {
			const uint32_t base = static_cast<uint32_t>(data.Vertices.size());

			data.Vertices.push_back(Made(a[0], a[1], a[2], normal[0], normal[1], normal[2], 0.0f, 1.0f));
			data.Vertices.push_back(Made(b[0], b[1], b[2], normal[0], normal[1], normal[2], 0.5f, 0.0f));
			data.Vertices.push_back(Made(c[0], c[1], c[2], normal[0], normal[1], normal[2], 1.0f, 1.0f));

			data.Indices.insert(data.Indices.end(), {base, base + 1, base + 2});
		}

		// The slope normal of a wedge, which is the one normal here that is not
		// an axis. Written as a constant rather than derived per face so the
		// two wedges cannot disagree about it.
		constexpr float DIAGONAL = 0.70710678f;

		MeshData MakeCube() {
			MeshData data;
			data.Vertices.reserve(24);
			data.Indices.reserve(36);

			AddQuad(
				data,
				{0.5f, -0.5f, -0.5f},
				{0.5f, 0.5f, -0.5f},
				{0.5f, 0.5f, 0.5f},
				{0.5f, -0.5f, 0.5f},
				{1.0f, 0.0f, 0.0f}
			);
			AddQuad(
				data,
				{-0.5f, -0.5f, 0.5f},
				{-0.5f, 0.5f, 0.5f},
				{-0.5f, 0.5f, -0.5f},
				{-0.5f, -0.5f, -0.5f},
				{-1.0f, 0.0f, 0.0f}
			);
			AddQuad(
				data,
				{-0.5f, 0.5f, -0.5f},
				{-0.5f, 0.5f, 0.5f},
				{0.5f, 0.5f, 0.5f},
				{0.5f, 0.5f, -0.5f},
				{0.0f, 1.0f, 0.0f}
			);
			AddQuad(
				data,
				{-0.5f, -0.5f, 0.5f},
				{-0.5f, -0.5f, -0.5f},
				{0.5f, -0.5f, -0.5f},
				{0.5f, -0.5f, 0.5f},
				{0.0f, -1.0f, 0.0f}
			);
			AddQuad(
				data,
				{-0.5f, -0.5f, 0.5f},
				{0.5f, -0.5f, 0.5f},
				{0.5f, 0.5f, 0.5f},
				{-0.5f, 0.5f, 0.5f},
				{0.0f, 0.0f, 1.0f}
			);
			AddQuad(
				data,
				{0.5f, -0.5f, -0.5f},
				{-0.5f, -0.5f, -0.5f},
				{-0.5f, 0.5f, -0.5f},
				{0.5f, 0.5f, -0.5f},
				{0.0f, 0.0f, -1.0f}
			);
			return data;
		}

		MeshData MakePlane() {
			MeshData data;
			AddQuad(
				data,
				{-0.5f, 0.0f, -0.5f},
				{-0.5f, 0.0f, 0.5f},
				{0.5f, 0.0f, 0.5f},
				{0.5f, 0.0f, -0.5f},
				{0.0f, 1.0f, 0.0f}
			);
			return data;
		}

		MeshData MakeWedge() {
			MeshData data;

			// The six corners. Named rather than repeated, because a wedge is
			// the built-in where an index typo produces a shape that still
			// looks like a wedge from three sides.
			constexpr std::array<float, 3> frontLeft{-0.5f, -0.5f, 0.5f};
			constexpr std::array<float, 3> frontRight{0.5f, -0.5f, 0.5f};
			constexpr std::array<float, 3> backRight{0.5f, -0.5f, -0.5f};
			constexpr std::array<float, 3> backLeft{-0.5f, -0.5f, -0.5f};
			constexpr std::array<float, 3> topLeft{-0.5f, 0.5f, -0.5f};
			constexpr std::array<float, 3> topRight{0.5f, 0.5f, -0.5f};

			AddQuad(data, frontLeft, backLeft, backRight, frontRight, {0.0f, -1.0f, 0.0f});
			AddQuad(data, backLeft, topLeft, topRight, backRight, {0.0f, 0.0f, -1.0f});
			AddQuad(data, topLeft, frontLeft, frontRight, topRight, {0.0f, DIAGONAL, DIAGONAL});
			AddTriangle(data, frontLeft, topLeft, backLeft, {-1.0f, 0.0f, 0.0f});
			AddTriangle(data, frontRight, backRight, topRight, {1.0f, 0.0f, 0.0f});
			return data;
		}

		MeshData MakeCornerWedge() {
			MeshData data;

			constexpr std::array<float, 3> frontLeft{-0.5f, -0.5f, 0.5f};
			constexpr std::array<float, 3> frontRight{0.5f, -0.5f, 0.5f};
			constexpr std::array<float, 3> backRight{0.5f, -0.5f, -0.5f};
			constexpr std::array<float, 3> backLeft{-0.5f, -0.5f, -0.5f};
			constexpr std::array<float, 3> apex{-0.5f, 0.5f, -0.5f};

			AddQuad(data, frontLeft, backLeft, backRight, frontRight, {0.0f, -1.0f, 0.0f});
			AddTriangle(data, backLeft, apex, backRight, {0.0f, 0.0f, -1.0f});
			AddTriangle(data, frontLeft, apex, backLeft, {-1.0f, 0.0f, 0.0f});
			AddTriangle(data, frontLeft, frontRight, apex, {0.0f, DIAGONAL, DIAGONAL});
			AddTriangle(data, frontRight, backRight, apex, {DIAGONAL, DIAGONAL, 0.0f});
			return data;
		}

		MeshData MakeSphere() {
			MeshData data;
			data.Vertices.reserve((RINGS + 1) * (SEGMENTS + 1));

			// **The seam column is duplicated**, which is why the loop runs to
			// `SEGMENTS` inclusive. The vertex at `u = 0` and the one at
			// `u = 1` are the same point in space and different points in
			// texture space, and sharing them wraps the whole texture backwards
			// across one column of triangles.
			for (uint32_t ring = 0; ring <= RINGS; ring++) {
				const float theta = PI * static_cast<float>(ring) / static_cast<float>(RINGS);
				const float y = std::cos(theta);
				const float radius = std::sin(theta);

				for (uint32_t segment = 0; segment <= SEGMENTS; segment++) {
					const float phi = 2.0f * PI * static_cast<float>(segment) / static_cast<float>(SEGMENTS);
					const float x = radius * std::sin(phi);
					const float z = radius * std::cos(phi);

					// The unit vector out of the origin *is* the normal for a
					// sphere, so the position is half of it rather than the
					// other way round.
					data.Vertices.push_back(Made(
						x * 0.5f,
						y * 0.5f,
						z * 0.5f,
						x,
						y,
						z,
						static_cast<float>(segment) / static_cast<float>(SEGMENTS),
						static_cast<float>(ring) / static_cast<float>(RINGS)
					));
				}
			}

			const uint32_t stride = SEGMENTS + 1;
			for (uint32_t ring = 0; ring < RINGS; ring++) {
				for (uint32_t segment = 0; segment < SEGMENTS; segment++) {
					const uint32_t topLeft = ring * stride + segment;
					const uint32_t topRight = topLeft + 1;
					const uint32_t bottomLeft = topLeft + stride;
					const uint32_t bottomRight = bottomLeft + 1;

					// **The pole rows emit one triangle rather than two.** At a
					// pole every vertex of the row is the same point, so one of
					// the two triangles has zero area — and a zero-area triangle
					// has no cross product, which means no winding, which means
					// the check that every face points outwards cannot be made
					// to cover the mesh.
					if (ring != 0) {
						data.Indices.insert(data.Indices.end(), {topLeft, bottomLeft, topRight});
					}
					if (ring + 1 != RINGS) {
						data.Indices.insert(data.Indices.end(), {topRight, bottomLeft, bottomRight});
					}
				}
			}
			return data;
		}

		MeshData MakeCylinder() {
			MeshData data;

			// The wall. Seam duplicated for `MakeSphere`'s reason.
			const uint32_t wallBase = static_cast<uint32_t>(data.Vertices.size());
			for (uint32_t segment = 0; segment <= SEGMENTS; segment++) {
				const float phi = 2.0f * PI * static_cast<float>(segment) / static_cast<float>(SEGMENTS);
				const float x = std::sin(phi);
				const float z = std::cos(phi);
				const float u = static_cast<float>(segment) / static_cast<float>(SEGMENTS);

				data.Vertices.push_back(Made(x * 0.5f, 0.5f, z * 0.5f, x, 0.0f, z, u, 0.0f));
				data.Vertices.push_back(Made(x * 0.5f, -0.5f, z * 0.5f, x, 0.0f, z, u, 1.0f));
			}

			for (uint32_t segment = 0; segment < SEGMENTS; segment++) {
				const uint32_t top = wallBase + segment * 2;
				const uint32_t bottom = top + 1;
				const uint32_t nextTop = top + 2;
				const uint32_t nextBottom = top + 3;

				data.Indices.insert(data.Indices.end(), {top, bottom, nextBottom});
				data.Indices.insert(data.Indices.end(), {top, nextBottom, nextTop});
			}

			// The caps. Their own vertices, because a rim vertex's normal points
			// sideways on the wall and up on the cap, and one vertex cannot
			// carry both — sharing them rounds the edge of the lid off into a
			// smear.
			for (int end = 0; end < 2; end++) {
				const float y = end == 0 ? 0.5f : -0.5f;
				const float normalY = end == 0 ? 1.0f : -1.0f;

				const uint32_t centre = static_cast<uint32_t>(data.Vertices.size());
				data.Vertices.push_back(Made(0.0f, y, 0.0f, 0.0f, normalY, 0.0f, 0.5f, 0.5f));

				for (uint32_t segment = 0; segment <= SEGMENTS; segment++) {
					const float phi = 2.0f * PI * static_cast<float>(segment) / static_cast<float>(SEGMENTS);
					const float x = std::sin(phi) * 0.5f;
					const float z = std::cos(phi) * 0.5f;
					data.Vertices.push_back(Made(x, y, z, 0.0f, normalY, 0.0f, x + 0.5f, z + 0.5f));
				}

				for (uint32_t segment = 0; segment < SEGMENTS; segment++) {
					const uint32_t first = centre + 1 + segment;
					const uint32_t second = first + 1;

					// The two ends wind opposite ways round, because "counter-
					// clockwise seen from outside" is a different direction at
					// the top of a cylinder than at the bottom.
					if (end == 0) {
						data.Indices.insert(data.Indices.end(), {centre, first, second});
					} else {
						data.Indices.insert(data.Indices.end(), {centre, second, first});
					}
				}
			}
			return data;
		}
	}

	std::string_view BuiltinName(BuiltinMesh mesh) {
		const uint8_t index = static_cast<uint8_t>(mesh);
		return index < BUILTIN_MESH_COUNT ? NAMES[index] : NAMES[0];
	}

	std::string_view BuiltinName(BuiltinTexture texture) {
		const auto index = static_cast<uint8_t>(texture);
		return index < BUILTIN_TEXTURE_COUNT ? TEXTURE_NAMES[index] : TEXTURE_NAMES[0];
	}

	bool BuiltinFromName(std::string_view name, BuiltinMesh &out) {
		for (uint8_t index = 0; index < BUILTIN_MESH_COUNT; index++) {
			if (NAMES[index] == name) {
				out = static_cast<BuiltinMesh>(index);
				return true;
			}
		}
		return false;
	}

	bool BuiltinFromName(std::string_view name, BuiltinTexture &out) {
		for (uint8_t index = 0; index < BUILTIN_TEXTURE_COUNT; index++) {
			if (TEXTURE_NAMES[index] == name) {
				out = static_cast<BuiltinTexture>(index);
				return true;
			}
		}
		return false;
	}

	MeshData MakeBuiltin(BuiltinMesh mesh) {
		MeshData data;
		switch (mesh) {
		case BuiltinMesh::Cube:
			data = MakeCube();
			break;
		case BuiltinMesh::Plane:
			data = MakePlane();
			break;
		case BuiltinMesh::Wedge:
			data = MakeWedge();
			break;
		case BuiltinMesh::CornerWedge:
			data = MakeCornerWedge();
			break;
		case BuiltinMesh::Sphere:
			data = MakeSphere();
			break;
		case BuiltinMesh::Cylinder:
			data = MakeCylinder();
			break;
		}

		data.ComputeBounds();
		return data;
	}

	TextureData MakeBuiltin(BuiltinTexture texture) {
		switch (texture) {
		case BuiltinTexture::Checker:
			return MakeChecker();
		}
		return MakeChecker();
	}
}
