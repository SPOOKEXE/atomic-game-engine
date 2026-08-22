#pragma once

// The per-instance row the vertex stream carries, and the quantisation that
// makes it thirty-six bytes instead of ninety-six.
//
// **A file of its own so the error is measurable without a device.** The whole
// argument for this layout is that a rotation survives eight bytes and a colour
// survives four, and "survives" is a number somebody has to be able to check.
// `RenderTypes.hpp` reaches `SDL3/SDL_gpu.h`; this reaches glm, `MeshTable` and
// `DrawInstance` and nothing else, so `render/tests/InstancePacking.cpp`
// includes it directly and compares a packed row against the matrix the old
// layout would have uploaded.
//
// **The decode lives twice, here and in `opaque.vert`.** That is the one
// duplication this file cannot remove: a vertex shader cannot call C++. The
// tests below pin the arithmetic so the two can be compared by reading, and
// `ModelMatrixOf` is the C++ side written to be read beside the GLSL rather
// than to be short.
//
// @tier L12 · client

#include <engine/core/types/CFrame.hpp>
#include <engine/render/MeshTable.hpp>
#include <engine/scene/DrawInstance.hpp>

#include <glm/gtc/quaternion.hpp>
#include <glm/mat3x3.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace engine::render {

	// The largest magnitude a signed-normalised sixteen-bit component encodes.
	//
	// 32767 rather than 32768 because that is the rule Vulkan, D3D and Metal all
	// decode by: the negative end has one more code than the positive one, and
	// -32768 decodes to -1 the same as -32767 does. Encoding against 32768 would
	// put every value one code hot and make `-1.0` the only input that failed to
	// round-trip.
	constexpr float SNORM16_SCALE = 32767.0f;

	// Rounds a value in [-1, 1] to its sixteen-bit signed-normalised code.
	inline uint16_t EncodeSnorm16(float value) {
		const float clamped = std::clamp(value, -1.0f, 1.0f);
		return static_cast<uint16_t>(static_cast<int16_t>(std::lround(clamped * SNORM16_SCALE)));
	}

	// Decodes one sixteen-bit signed-normalised code, exactly as GLSL's
	// `unpackSnorm2x16` does.
	inline float DecodeSnorm16(uint16_t bits) {
		return std::max(static_cast<float>(static_cast<int16_t>(bits)) / SNORM16_SCALE, -1.0f);
	}

	// A unit quaternion in eight bytes.
	//
	// **Four components at sixteen bits, not the "smallest three" trick.**
	// Dropping the largest component and spending ten bits on each of the rest
	// fits a rotation in four bytes, and its worst-case angular error is around
	// a thousandth of a radian - which is a millimetre of drift per metre of
	// lever arm, and a part twenty metres across would visibly shear. Sixteen
	// bits a component costs four more bytes and buys two orders of magnitude;
	// `render/tests/InstancePacking.cpp` measures what it actually lands at.
	//
	// Word zero holds x then y, word one holds z then w, each low half first -
	// which is what `unpackSnorm2x16` reads, so the shader is two calls.
	//
	// @since v0.19
	struct PackedRotation {
		// Identity by default: x, y and z at zero, w at one.
		uint32_t Words[2]{0u, 0x7FFF0000u};
	};

	// Packs a rotation, normalising it first.
	//
	// **Normalised here rather than trusted**, which is a change from the
	// matrix layout this replaced: that one passed `CFrame::Rotation()` straight
	// to `glm::toMat4`, which assumes a unit quaternion and silently scales the
	// geometry when handed something else. A quantised component is meaningless
	// unless the value is known to be in [-1, 1], so the normalise is not
	// optional here - and doing it turns a class of authoring bug into a
	// no-operation instead of into a stretched part.
	//
	// A quaternion with no length at all is a caller that never set one; it
	// packs as the identity rather than as NaN.
	inline PackedRotation PackRotation(const glm::quat &rotation) {
		const float square = rotation.x * rotation.x + rotation.y * rotation.y + rotation.z * rotation.z +
							 rotation.w * rotation.w;
		const glm::quat unit =
			square > 1e-12f ? rotation * (1.0f / std::sqrt(square)) : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);

		PackedRotation packed;
		packed.Words[0] = static_cast<uint32_t>(EncodeSnorm16(unit.x)) |
						  (static_cast<uint32_t>(EncodeSnorm16(unit.y)) << 16);
		packed.Words[1] = static_cast<uint32_t>(EncodeSnorm16(unit.z)) |
						  (static_cast<uint32_t>(EncodeSnorm16(unit.w)) << 16);
		return packed;
	}

	// Unpacks a rotation and renormalises it.
	//
	// **The renormalise is most of the accuracy.** Rounding four components
	// independently moves the quaternion off the unit sphere, and a quaternion
	// off the unit sphere is a rotation *and a scale* - so without this the
	// error would show up as geometry breathing by a few parts in a hundred
	// thousand rather than as a rotation being slightly wrong. One reciprocal
	// square root removes the radial half of the error outright; the shader
	// spends the same one.
	inline glm::quat UnpackRotation(const PackedRotation &packed) {
		const glm::quat raw{
			DecodeSnorm16(static_cast<uint16_t>(packed.Words[1] >> 16)),
			DecodeSnorm16(static_cast<uint16_t>(packed.Words[0] & 0xFFFFu)),
			DecodeSnorm16(static_cast<uint16_t>(packed.Words[0] >> 16)),
			DecodeSnorm16(static_cast<uint16_t>(packed.Words[1] & 0xFFFFu)),
		};
		const float square = raw.x * raw.x + raw.y * raw.y + raw.z * raw.z + raw.w * raw.w;
		return square > 1e-12f ? raw * (1.0f / std::sqrt(square)) : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
	}

	// Packs a linear colour and alpha as RGBA8, red in the low byte.
	//
	// **Eight bits a channel is not a compromise, for `ParticleInstance`'s
	// reason.** The value is a `Color3` an author picked and an alpha derived
	// from `Transparency`, and it reaches an eight-bit-per-channel swapchain
	// through the blend either way. Carrying sixteen bytes of float to feed a
	// target that keeps four was the old layout's largest single waste.
	inline uint32_t PackColour(const glm::vec4 &colour) {
		const auto channel = [](float value) {
			return static_cast<uint32_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
		};
		return channel(colour.r) | (channel(colour.g) << 8) | (channel(colour.b) << 16) |
			   (channel(colour.a) << 24);
	}

	// Unpacks an RGBA8 colour, exactly as GLSL's `unpackUnorm4x8` does.
	inline glm::vec4 UnpackColour(uint32_t packed) {
		const auto channel = [&](uint32_t shift) {
			return static_cast<float>((packed >> shift) & 0xFFu) / 255.0f;
		};
		return glm::vec4{channel(0), channel(8), channel(16), channel(24)};
	}

	// One instance as the vertex shader reads it.
	//
	// Private GPU layout; scene data must not expose device types.
	//
	// **Thirty-six bytes, down from ninety-six, and the shape is why.** The old
	// row carried a `mat4`, an RGBA float4 and a float4 of inverse squared
	// scales. Every one of those was derived from three things a `DrawInstance`
	// already holds separately - a `CFrame`, a half-extent and a tint - and the
	// derivation threw information away rather than adding any. A matrix that is
	// only ever `T * R * S` stores the rotation nine times over; the inverse
	// scale is the scale; and the fourth row is always `(0, 0, 0, 1)`.
	//
	// **Position stays at full float precision and that is deliberate.** It is
	// the one field here whose error is absolute rather than relative: a world
	// runs to thousands of metres from the origin, and a half float at two
	// thousand has an interval of two metres. Rotation and colour are bounded in
	// [-1, 1] and [0, 1], where a fixed-point code is exactly the right encoding;
	// scale is unbounded in the same way position is, so it stays float too. The
	// bytes that were saved were saved from the fields that could afford it.
	//
	// @since v0.19
	struct GpuInstance {
		// Where the mesh's origin sits in world space, with the mesh's own
		// centre offset already folded in. See `ToGpu`.
		glm::vec3 Position{0.0f, 0.0f, 0.0f};

		// The instance's orientation.
		PackedRotation Rotation;

		// How much to multiply each mesh axis by so its own box becomes the
		// part's. May be zero on an axis - the built-in plane has no thickness.
		glm::vec3 Scale{1.0f, 1.0f, 1.0f};

		// Colour and alpha, RGBA8 with red in the low byte.
		uint32_t Colour = 0xFFFFFFFFu;
	};

	// How many 32-bit words one row is.
	//
	// **Declared here and read by the build, so `occlusion-cull.comp` carries no
	// literal of its own.** That shader copies a survivor's row through
	// untouched, word by word, and it is the one consumer of this stride that no
	// C++ compiler can check: a row that grew would keep building and would copy
	// eight ninths of each survivor into the late buffer, which draws as geometry
	// smeared across the scene. `mono.engine/resources/CMakeLists.txt` reads this
	// declaration at configure time and passes it as `-DGPU_INSTANCE_WORDS`, exactly
	// the way the light cap already reaches the shaders - so the two cannot
	// disagree rather than merely being checked for agreement.
	//
	// The regex there matches this declaration's shape. Moving or rewriting it
	// stops the configure with a message saying so, rather than drifting.
	//
	// @since v0.19
	inline constexpr size_t GPU_INSTANCE_WORDS = 9;

	// **Thirty-six and not forty, which is worth pinning rather than leaving to
	// whatever the compiler laid out.** Every member is four-byte aligned and
	// there is no interior hole, so this is exactly the sum of the fields. The
	// stride reaches the device as a storage-buffer word count and as the C++
	// allocation and upload width.
	static_assert(
		sizeof(GpuInstance) == GPU_INSTANCE_WORDS * sizeof(uint32_t),
		"GpuInstance stride changed. Update GPU_INSTANCE_WORDS above; the build passes it to "
		"occlusion-cull.comp, so the shader follows on its own."
	);
	static_assert(alignof(GpuInstance) == 4, "GpuInstance must stay four-aligned for a vertex stream.");

	// Rebuilds the model matrix a packed row draws with.
	//
	// **The matrix the *shader* builds, not the one that was packed.** It reads
	// the quantised fields and renormalises the rotation exactly as
	// `opaque.vert` does, so a caller measuring the world box of an instance -
	// `ViewRecording`'s occlusion candidates are the only one - bounds the
	// geometry that will actually be drawn rather than the geometry that was
	// asked for. A bound taken from the pre-quantisation transform would be
	// tight by construction and wrong by a rounding error, which is the shape of
	// bug that shows up as one flickering part in a thousand.
	//
	// @param instance A packed row.
	// @return Its `T * R * S`, column-major, ready to multiply a mesh position.
	inline glm::mat4 ModelMatrixOf(const GpuInstance &instance) {
		glm::mat4 model = glm::mat4_cast(UnpackRotation(instance.Rotation));
		model[0] *= instance.Scale.x;
		model[1] *= instance.Scale.y;
		model[2] *= instance.Scale.z;
		model[3] = glm::vec4(instance.Position, 1.0f);
		return model;
	}

	// A draw instance in the layout the opaque pipeline binds.
	//
	// **`Size` is a box the mesh is stretched into, not a multiplier.** The
	// mesh's own bounding box is mapped exactly onto the part's, so
	// `MeshPart.Size` means metres for every mesh in the world regardless of
	// the scale it was authored at - Roblox's `MeshPart` semantic, and the
	// thing that makes `scene::Bounds` describe the geometry rather than
	// approximate it.
	//
	// **The culling depended on this and nothing enforced it.**
	// `graph::CullAndBound` tests `HalfExtent` against the frustum. While
	// `Size` merely multiplied mesh coordinates, a mesh authored twenty units
	// tall drew ten times outside the box describing it and was culled with
	// most of itself still on screen. Now the drawn geometry fills that box
	// exactly, so the cull is right by construction rather than right when
	// the content happened to be baked correctly.
	//
	// A built-in is a unit shape about its own origin, so its `Extent` is a
	// half on every axis and this is `HalfExtent * 2` - byte for byte what
	// this function did before.
	//
	// @param instance What to draw.
	// @param mesh     Its geometry, as `MeshTable::Resolve` gave it. Never
	//        null: an unknown name resolves to the fallback.
	inline GpuInstance ToGpu(const scene::DrawInstance &instance, const MeshEntry &mesh) {
		// How much to multiply one axis by so the mesh's own box becomes the
		// part's.
		//
		// **A degenerate axis keeps the old rule rather than dividing.** The
		// built-in plane is a quad with no thickness, so its Y extent is
		// exactly zero - and a flat mesh is an ordinary thing to author. The
		// fallback is `HalfExtent * 2`, which is what a zero-thickness mesh
		// got before and does nothing to geometry that has no extent on that
		// axis anyway.
		const auto stretch = [](float half, float extent) {
			return extent > 1e-6f ? half / extent : half * 2.0f;
		};

		const glm::vec3 scale{
			stretch(instance.HalfExtent.X, mesh.Extent.X),
			stretch(instance.HalfExtent.Y, mesh.Extent.Y),
			stretch(instance.HalfExtent.Z, mesh.Extent.Z),
		};

		GpuInstance gpu;
		gpu.Rotation = PackRotation(instance.Frame.Rotation());
		gpu.Scale = scale;

		// **Centred after scaling, in the part's own space.** A model
		// authored off its origin would otherwise hang away from the part by
		// however far its box is offset - and because the offset scales with
		// the part, it would grow as somebody resized it.
		//
		// Folded into the translation here rather than composed as a second
		// matrix, and folded against the *unpacked* rotation rather than the
		// authored one: the shader will rotate the mesh by what it reads, so
		// the offset that cancels the mesh's own centre has to be taken through
		// the same rotation or the correction misses by the quantisation error.
		const glm::vec3 centre{mesh.Centre.X, mesh.Centre.Y, mesh.Centre.Z};
		gpu.Position =
			glm::vec3{instance.Frame.Position.X, instance.Frame.Position.Y, instance.Frame.Position.Z} -
			UnpackRotation(gpu.Rotation) * (scale * centre);

		// Convert author-facing transparency to shader alpha.
		gpu.Colour = PackColour(
			glm::vec4{instance.Tint.R, instance.Tint.G, instance.Tint.B, 1.0f - instance.Transparency}
		);
		return gpu;
	}
}
