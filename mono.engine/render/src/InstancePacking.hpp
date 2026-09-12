#pragma once

// Private resident instance and joint layouts, shared with instance.glsl.
// Rotation keeps full float precision because oblique aperture coverage is
// sensitive to errors too small to notice on an isolated mesh.
// CPU tests compare upload reconstruction against independent double rotations.
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
#include <bit>
#include <cmath>
#include <cstdint>

namespace engine::render {

	// Unit quaternion as four IEEE float words in xyzw order.
	struct PackedRotation {
		uint32_t Words[4]{0u, 0u, 0u, 0x3F800000u};
	};

	// Normalize once before upload. No fixed-point step moves aperture edges.
	inline PackedRotation PackRotation(const glm::quat &rotation) {
		const double components[4]{rotation.x, rotation.y, rotation.z, rotation.w};
		double square = 0;
		for (const double component : components)
			square += component * component;
		if (!std::isfinite(square) || square <= 1e-12) return {};
		const double inverse = 1.0 / std::sqrt(square);
		PackedRotation packed;
		for (size_t index = 0; index < 4; ++index)
			packed.Words[index] = std::bit_cast<uint32_t>(static_cast<float>(components[index] * inverse));
		return packed;
	}

	inline glm::quat UnpackRotation(const PackedRotation &packed) {
		return {
			std::bit_cast<float>(packed.Words[3]),
			std::bit_cast<float>(packed.Words[0]),
			std::bit_cast<float>(packed.Words[1]),
			std::bit_cast<float>(packed.Words[2])
		};
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

	// Alpha mode in the low byte and cutoff as UNORM8 in the next one.
	inline uint32_t PackAppearance(
		scene::AlphaMode mode,
		float cutoff,
		scene::SurfaceResampleMode resample = scene::SurfaceResampleMode::Default
	) {
		const uint32_t threshold =
			static_cast<uint32_t>(std::lround(std::clamp(cutoff, 0.0f, 1.0f) * 255.0f));
		return static_cast<uint32_t>(mode) | (threshold << 8) | (static_cast<uint32_t>(resample) << 16);
	}

	inline scene::AlphaMode UnpackAlphaMode(uint32_t packed) {
		const uint32_t mode = packed & 0xFFu;
		return mode <= static_cast<uint32_t>(scene::AlphaMode::Opaque) ? static_cast<scene::AlphaMode>(mode)
																	   : scene::AlphaMode::Opaque;
	}

	inline float UnpackAlphaCutoff(uint32_t packed) {
		return static_cast<float>((packed >> 8) & 0xFFu) / 255.0f;
	}

	inline scene::SurfaceResampleMode UnpackResampleMode(uint32_t packed) {
		return ((packed >> 16u) & 0xFFu) == static_cast<uint32_t>(scene::SurfaceResampleMode::Pixelated)
				   ? scene::SurfaceResampleMode::Pixelated
				   : scene::SurfaceResampleMode::Default;
	}

	inline uint32_t PackEmission(const core::Color3 &tint, float strength) {
		return PackColour(glm::vec4{tint.R, tint.G, tint.B, std::clamp(strength, 0.0f, 16.0f) / 16.0f});
	}

	// Four aligned vectors match the shader storage row. Feature policy occupies
	// the final two words so every draw stage can resolve per-instance choices
	// without reading scene state back through the CPU.
	struct alignas(16) GpuInstance {
		glm::vec3 Position{0.0f, 0.0f, 0.0f};
		uint32_t Colour = 0xFFFFFFFFu;
		PackedRotation Rotation;
		glm::vec3 Scale{1.0f, 1.0f, 1.0f};
		uint32_t Appearance = PackAppearance(scene::AlphaMode::Opaque, 0.5f);
		uint32_t SurfaceColour = 0xFFFFFFFFu;
		uint32_t Emission = PackEmission(core::Color3{1.0f, 1.0f, 1.0f}, 1.0f);
		uint32_t FeatureEnable = 0;
		uint32_t FeatureDisable = 0;
	};

	// The resources build reads these strides for instance.glsl's layout guards.
	// Occlusion compacts slot indices; it does not copy these resident rows.
	inline constexpr size_t GPU_INSTANCE_WORDS = 16;
	inline constexpr size_t GPU_JOINT_WORDS = 7;

	// Both word strides are passed to the shader by the resources build.
	static_assert(
		sizeof(GpuInstance) == GPU_INSTANCE_WORDS * sizeof(uint32_t),
		"GpuInstance stride changed. Update GPU_INSTANCE_WORDS above; the build passes it to "
		"instance.glsl, whose layout guard must agree."
	);
	static_assert(alignof(GpuInstance) == 16, "GpuInstance must match the shader vector alignment.");

	// Rebuilds the model matrix a packed row draws with.
	//
	// **The matrix the *shader* builds, not the one that was packed.** It reads
	// the uploaded fields and reconstructs the rotation exactly as
	// `opaque.vert` does, so a caller measuring the world box of an instance -
	// `ViewRecording`'s occlusion candidates are the only one - bounds the
	// geometry that will actually be drawn rather than the geometry that was
	// asked for. A bound taken from the unnormalised authored transform would be
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
		// the same rotation or the correction misses by the rounding error.
		const glm::vec3 centre{mesh.Centre.X, mesh.Centre.Y, mesh.Centre.Z};
		gpu.Position =
			glm::vec3{instance.Frame.Position.X, instance.Frame.Position.Y, instance.Frame.Position.Z} -
			UnpackRotation(gpu.Rotation) * (scale * centre);

		// Convert author-facing transparency to shader alpha.
		gpu.Colour = PackColour(
			glm::vec4{instance.Tint.R, instance.Tint.G, instance.Tint.B, 1.0f - instance.Transparency}
		);
		gpu.Appearance = PackAppearance(instance.Alpha, instance.AlphaCutoff, instance.Resample);
		gpu.SurfaceColour = PackColour(
			glm::vec4{instance.SurfaceColour.R, instance.SurfaceColour.G, instance.SurfaceColour.B, 1.0f}
		);
		gpu.Emission = PackEmission(instance.EmissiveTint, instance.EmissiveStrength);
		gpu.FeatureEnable = instance.RenderFeatures.Enable & scene::ALL_RENDER_FEATURES;
		gpu.FeatureDisable = instance.RenderFeatures.Disable & scene::ALL_RENDER_FEATURES;
		return gpu;
	}
}
