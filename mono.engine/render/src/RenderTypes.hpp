#pragma once

// The device-side layouts and the frame-wide constants every part of the
// renderer shares.
//
// **A private header rather than an anonymous namespace, and that is the whole
// reason it exists.** These structs are what the pipelines bind, so every file
// that records a pass needs them - and while they lived inside `Renderer.cpp`
// every pass had to live there too. `render/AGENTS.md` states the rule they
// still obey: the device layout is private and stays private, so nothing here
// reaches a public header.

#include "InstancePacking.hpp"

#include <engine/graph/RenderGraph.hpp>
#include <engine/graph/Schedule.hpp>
#include <engine/render/GraphRunner.hpp>
#include <engine/render/MeshTable.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/scene/DrawInstance.hpp>
#include <engine/scene/Sunlight.hpp>

#include <SDL3/SDL_gpu.h>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <span>
#include <vector>

namespace engine::render {

	struct BackendNode {
		core::Name Kind;
		graph::NodeScope Scope;
		graph::ExecutionQueue Queue;
	};

	inline std::span<const BackendNode> BackendNodes() {
		static const std::array nodes{
			BackendNode{core::Name("world"), graph::NodeScope::World, graph::ExecutionQueue::Cpu},
			BackendNode{core::Name("shadow"), graph::NodeScope::World, graph::ExecutionQueue::Graphics},
			BackendNode{core::Name("camera"), graph::NodeScope::View, graph::ExecutionQueue::Cpu},
			BackendNode{core::Name("entities"), graph::NodeScope::View, graph::ExecutionQueue::Cpu},
			BackendNode{core::Name("cull-frustum"), graph::NodeScope::View, graph::ExecutionQueue::Cpu},
			BackendNode{core::Name("cull-distance"), graph::NodeScope::View, graph::ExecutionQueue::Cpu},
			BackendNode{core::Name("filter-tag"), graph::NodeScope::View, graph::ExecutionQueue::Cpu},
			BackendNode{core::Name("order-draw"), graph::NodeScope::View, graph::ExecutionQueue::Cpu},
			BackendNode{
				core::Name("upload-instances"), graph::NodeScope::View, graph::ExecutionQueue::Transfer
			},
			BackendNode{core::Name("last-frame"), graph::NodeScope::View, graph::ExecutionQueue::Graphics},
			BackendNode{
				core::Name("mirror-capture"), graph::NodeScope::View, graph::ExecutionQueue::Graphics
			},
			BackendNode{
				core::Name("portal-capture"), graph::NodeScope::View, graph::ExecutionQueue::Graphics
			},
			BackendNode{
				core::Name("portal-tonemap"), graph::NodeScope::View, graph::ExecutionQueue::Graphics
			},
			BackendNode{core::Name("gbuffer"), graph::NodeScope::View, graph::ExecutionQueue::Graphics},
			BackendNode{
				core::Name("depth-linearise"), graph::NodeScope::View, graph::ExecutionQueue::Graphics
			},
			BackendNode{core::Name("hzb"), graph::NodeScope::View, graph::ExecutionQueue::Compute},
			BackendNode{core::Name("ssao"), graph::NodeScope::View, graph::ExecutionQueue::Graphics},
			BackendNode{
				core::Name("deferred-lighting"), graph::NodeScope::View, graph::ExecutionQueue::Graphics
			},
			BackendNode{core::Name("tonemap"), graph::NodeScope::View, graph::ExecutionQueue::Graphics},
			BackendNode{
				core::Name("portal-overlay"), graph::NodeScope::View, graph::ExecutionQueue::Graphics
			},
			BackendNode{
				core::Name("mirror-overlay"), graph::NodeScope::View, graph::ExecutionQueue::Graphics
			},
			BackendNode{core::Name("transparent"), graph::NodeScope::View, graph::ExecutionQueue::Graphics},
			BackendNode{core::Name("raster"), graph::NodeScope::View, graph::ExecutionQueue::Graphics},
			BackendNode{core::Name("dispatch"), graph::NodeScope::View, graph::ExecutionQueue::Compute},
			BackendNode{core::Name("present"), graph::NodeScope::Frame, graph::ExecutionQueue::Graphics},
			BackendNode{core::Name("viewer"), graph::NodeScope::Frame, graph::ExecutionQueue::Transfer},
			BackendNode{core::Name("capture"), graph::NodeScope::Frame, graph::ExecutionQueue::Transfer},
			BackendNode{core::Name("overlay"), graph::NodeScope::Frame, graph::ExecutionQueue::Graphics},
			BackendNode{core::Name("interface"), graph::NodeScope::Frame, graph::ExecutionQueue::Graphics},
			BackendNode{core::Name("output-image"), graph::NodeScope::Frame, graph::ExecutionQueue::Transfer},
		};
		return nodes;
	}

	inline NodeTable BackendTable(const NodeHandler &handler) {
		NodeTable nodes;
		for (const BackendNode &node : BackendNodes()) {
			nodes.Set(node.Kind, handler);
		}
		return nodes;
	}

	struct FrameUniforms {
		glm::mat4 ViewProjection;

		// Matches the matrix used by the shadow pass.
		glm::mat4 LightViewProjection;

		// Surface-camera projection, or identity for ordinary geometry.
		glm::mat4 SurfaceViewProjection;
	};

	// The local light set, pushed once per pass rather than per draw.
	//
	// **Separate from `LightingUniforms` because the two change at different
	// rates.** That one carries the submesh's base colour and the surface
	// flags, so it is re-pushed on every draw call; this does not change
	// within a frame. Folding them would re-upload 784 bytes of light data on
	// every draw of a scene to say the same thing.
	struct LightUniforms {
		// xyz: position. w: range.
		glm::vec4 Position[MAX_SCENE_LIGHTS]{};

		// rgb: colour times brightness.
		glm::vec4 Colour[MAX_SCENE_LIGHTS]{};

		// xyz: spot direction. w: cone cosine, or -1 for a point light.
		glm::vec4 Direction[MAX_SCENE_LIGHTS]{};

		// x: how many are in use.
		glm::vec4 Count{0.0f, 0.0f, 0.0f, 0.0f};
	};

	// Packs a caller's lights into the buffer the shader reads.
	inline LightUniforms ToGpu(std::span<const SceneLight> lights) {
		LightUniforms out;
		const size_t count = std::min(lights.size(), MAX_SCENE_LIGHTS);

		for (size_t index = 0; index < count; index++) {
			const SceneLight &light = lights[index];
			out.Position[index] =
				glm::vec4{light.Position.X, light.Position.Y, light.Position.Z, light.Range};
			out.Colour[index] = glm::vec4{light.Colour.R, light.Colour.G, light.Colour.B, 0.0f};
			out.Direction[index] =
				glm::vec4{light.Direction.X, light.Direction.Y, light.Direction.Z, light.ConeCosine};
		}

		out.Count = glm::vec4{static_cast<float>(count), 0.0f, 0.0f, 0.0f};
		return out;
	}

	struct LightingUniforms {
		glm::vec4 Direction;
		glm::vec4 Ambient;
		glm::vec4 Direct;

		// x: whether a shadow map was rendered. y: one shadow texel.
		// z: whether this draw samples the surface texture. w: projected-image
		// opacity.
		glm::vec4 Flags;

		// The submesh's own colour, white for a draw with no material.
		glm::vec4 BaseColour{1.0f, 1.0f, 1.0f, 1.0f};

		// x: whether `colourMap` holds this draw's texture. y: the alpha
		// cutoff. z: whether height is present. w: parallax UV scale.
		glm::vec4 Surface{0.0f, 0.0f, 0.0f, 0.0f};
		glm::vec4 Material{0.0f, 0.0f, 0.0f, 0.0f};

		// Where the current animation cell sits: x the scale, yz the offset.
		//
		// **A transform rather than a cell index**, so a still image is the
		// identity and the shader needs no branch per fragment -
		// `render::FlipbookCell` carries the argument. A GIF is an ordinary
		// texture in every other respect, which is what makes one usable on
		// a part at all.
		glm::vec4 Flipbook{1.0f, 0.0f, 0.0f, 0.0f};

		// x: which `scene::SurfaceEffect` the projected image goes through.
		// y: the animation clock, for the effects that move.
		// zw: unused, and named so the struct's size is stated rather than
		//     implied.
		//
		// **A field of its own rather than the spare lanes in `Surface` or
		// `Flipbook`.** Both of those are rewritten wholesale per submesh by
		// `DrawSlots`, so anything parked in their unused components would
		// be silently zeroed by the next draw - a bug that presents as "the
		// effect only works on some parts".
		glm::vec4 Mirror{0.0f, 0.0f, 0.0f, 0.0f};

		// The face normal of the pane this draw is, for a portal, and a zero
		// vector for everything else.
		//
		// **Because a pane is a box and a hole is a rectangle.** The image is
		// chosen per draw and a draw is a whole instance, so a pane's four
		// edge faces read the sub-render as well - a hole comes out as wide
		// as the slab it is cut in, with a band of the far room running round
		// its rim at a parallax nothing else in the frame has. A thin pane
		// hides that in a fraction of a stud; a portal set in a wall does not.
		//
		// So the rim falls through to the pane's own material, which is what
		// a frame is, and the front and back faces show the picture - both of
		// them, because a hole is a hole from either side and the warp already
		// answers which.
		glm::vec4 PaneNormal{0.0f, 0.0f, 0.0f, 0.0f};

		// The half-space this draw keeps: xyz a unit world normal, w the offset
		// along it. A zero normal keeps everything, which is every draw in a
		// world with no portal in it.
		//
		// **Written by `DrawSlots` from the slot's own plane**, like the flipbook
		// cell and unlike the fields the caller sets, because the run it belongs
		// to is chosen by that plane - a caller passing one here would be
		// overwritten by the slot's.
		glm::vec4 SeamPlane{0.0f, 0.0f, 0.0f, 0.0f};

		// The sky term, fog colour and eye-relative fog interval. Appended so
		// built-in and user shaders that copy the established prefix keep every
		// earlier field at the same offset.
		glm::vec4 OutdoorAmbient{0.0f, 0.0f, 0.0f, 0.0f};
		glm::vec4 FogColour{0.0f, 0.0f, 0.0f, 0.0f};
		glm::vec4 Fog{100000.0f, 100001.0f, 0.0f, 0.0f};
		glm::vec4 Eye{0.0f, 0.0f, 0.0f, 0.0f};

		// x: whether a metalness map is present.
		glm::vec4 MaterialExtra{0.0f, 0.0f, 0.0f, 0.0f};
	};

	struct ShadowUniforms {
		glm::vec4 Plane{0.0f, 0.0f, 0.0f, 0.0f};
		glm::vec4 Material{1.0f, 0.0f, 0.0f, 0.0f};
		glm::vec4 Flipbook{1.0f, 0.0f, 0.0f, 0.0f};
	};

	// How many portal mouths may project their light field in one frame.
	//
	// **Two, which is one pair, and it is the prototype's budget.** Each is
	// a sampler binding and four vec4s in `PbrUniforms`, both spelled out in
	// `deferred-lighting.frag` - the three counts move together. The nearest
	// mouths win, so a corridor of pairs lights the one the viewer is at.
	constexpr size_t MAX_SEAM_LIGHTS = 2;

	// The side of one seam light-field capture, in texels.
	//
	// Small on purpose: the capture is read back as a pool of light on the
	// floor, low-frequency by the time the spread and the falloff have been
	// applied, and 128 keeps a pair of captures cheaper than one shadow
	// cascade.
	constexpr uint32_t SEAM_LIGHT_RESOLUTION = 128;

	// Shared by the default graph's fullscreen passes. One block keeps the
	// camera and authored Lighting state identical from depth reconstruction
	// through ambient occlusion and deferred shading.
	// What `grid.frag` reads. Its own block rather than a corner of
	// `PbrUniforms`, because the grid is an editor's furniture and the PBR
	// block is the lighting contract every authored post shader compiles
	// against - widening that to carry a grid colour would put a studio
	// preference in a file a game ships.
	struct GridUniforms {
		glm::mat4 ViewProjection{1.0f};
		glm::mat4 InverseViewProjection{1.0f};
		glm::vec4 Eye{};

		// x: studs between thin lines. y: cells to a heavy one. z: the
		// studs at which it has faded out. w: overall strength.
		glm::vec4 Params{};

		glm::vec4 Colour{};
		glm::vec4 AxisX{};
		glm::vec4 AxisZ{};
	};

	struct PbrUniforms {
		glm::mat4 InverseViewProjection{1.0f};
		glm::mat4 LightViewProjection{1.0f};
		glm::vec4 Planes{};
		glm::vec4 Target{};
		glm::vec4 Direction{};
		glm::vec4 Ambient{};
		glm::vec4 OutdoorAmbient{};
		glm::vec4 Direct{};
		glm::vec4 Eye{};
		glm::vec4 FogColour{};
		glm::vec4 Fog{};
		glm::vec4 Shadow{};

		// The seam light projectors - see the matching `SeamCentre` block in
		// `deferred-lighting.frag`. `Centre.w` is the live flag, `Outward.w`
		// the spill range; zeroed slots project nothing, so a frame with no
		// portals pays two dot products per pixel and no texture taps.
		//@{
		glm::vec4 SeamCentre[MAX_SEAM_LIGHTS]{};
		glm::vec4 SeamOutward[MAX_SEAM_LIGHTS]{};
		glm::vec4 SeamFirst[MAX_SEAM_LIGHTS]{};
		glm::vec4 SeamSecond[MAX_SEAM_LIGHTS]{};
		//@}
	};

	// Slot zero for an authored fullscreen fragment shader. The contract is
	// intentionally small and stable: target size, reciprocal size, frame
	// time, and the active camera matrices. Inputs remain sampler slots in the
	// order the node declares them.
	struct GraphPassUniforms {
		glm::mat4 ViewProjection{1.0f};
		glm::mat4 InverseViewProjection{1.0f};
		glm::vec4 Target{};
		glm::vec4 View{};
	};

	// How many holes may transport a shadow in one frame.
	//
	// **Four, in one 2x2 atlas, chosen by which holes are nearest the eye.**
	// Every fragment tests every beam, so the count is a cost per pixel and
	// not per hole; four is what a corridor needs and is two matrix products
	// and a tap each. Anything past it is logged rather than dropped
	// silently - a shadow that stops crossing when a fifth pane comes on
	// screen reads as the feature not working.
	constexpr uint32_t MAX_PORTAL_BEAMS = 4;

	// What `opaque.frag` needs to look a fragment up in one hole's beam.
	//
	// **Two matrices and a plane per beam, and the second matrix is the
	// surprising one.** The casters are left in the near room and the
	// *receiver* is mapped back to it - see `NON-EUCLIDEAN.md` Part V.3 -
	// so a far-side fragment goes through `Back` before it goes through
	// `Light`, and the plane is what says it was on the far side to begin
	// with.
	struct BeamUniforms {
		// The beam's own view-projection, in the near room's coordinates.
		glm::mat4 Light[MAX_PORTAL_BEAMS];

		// The far side back to the near one: the partner pane's own warp,
		// which is this pane's exact inverse.
		glm::mat4 Back[MAX_PORTAL_BEAMS];

		// The near pane's plane, as xyz normal and w offset. A mapped
		// fragment on the wrong side of it is in the near room already and
		// is shadowed by the world map rather than by this.
		glm::vec4 Plane[MAX_PORTAL_BEAMS];

		// Where this beam sits in the atlas: xy the scale, zw the offset.
		glm::vec4 Region[MAX_PORTAL_BEAMS];

		// x: how many of the four are in use.
		glm::vec4 Count{0.0f, 0.0f, 0.0f, 0.0f};
	};

	// How many indirect draw arguments one slot-run emits: one per non-empty
	// material range, or one for the whole mesh. `DrawSlots`' emit skips an
	// empty range without advancing its argument index, so anything walking
	// the arguments has to count the same way or drift one entry per empty
	// range and draw with a neighbour's geometry.
	inline uint32_t DrawArgumentCount(const MeshEntry &mesh) {
		if (mesh.Runs.empty()) {
			return mesh.Whole.IndexCount > 0 ? 1u : 0u;
		}
		uint32_t entries = 0;
		for (const MeshRange &range : mesh.Runs) {
			entries += range.IndexCount > 0 ? 1u : 0u;
		}
		return entries;
	}

	// Mix renderer-owned view data into the scene signature.
	inline uint64_t MixFloat(uint64_t hash, float value) {
		uint32_t bits = 0;
		std::memcpy(&bits, &value, sizeof(bits));
		return scene::MixSignature(hash, bits);
	}

	// Whether a surface's rate cap has elapsed.
	//
	// **Three ways to be uncapped, and all three are "yes" rather than
	// "no".** A rate of zero is the documented way to ask for every frame; a
	// negative one is a script that computed something silly, and answering
	// "never draw again" to that would be a surface that goes black for a
	// mistake nothing reports; and a clock that has not advanced - a host
	// that never calls `SetAnimationTime`, or a paused one - would otherwise
	// freeze every capped surface in the world after its first frame.
	//
	// The bias is the same one culling takes: when the answer is not
	// certain, do the work. A surface drawn too often costs a pass, and one
	// skipped wrongly is a frozen picture nobody can explain.
	//
	// @param drawn When the slot last drew, or negative for never.
	// @param fps   The cap, or zero for none.
	// @param now   The frame clock.
	inline bool DueToDraw(double drawn, float fps, double now) {
		if (!(fps > 0.0f) || drawn < 0.0 || !(now > drawn)) {
			return true;
		}
		return now - drawn >= 1.0 / static_cast<double>(fps);
	}

	inline uint64_t MixMatrix(uint64_t hash, const glm::mat4 &matrix) {
		for (int column = 0; column < 4; column++) {
			for (int row = 0; row < 4; row++) {
				hash = MixFloat(hash, matrix[column][row]);
			}
		}
		return hash;
	}

	// The one directional light this pipeline has.
	//
	// One directional light; shadow fitting and shading share its direction.
	// **`scene::SUN_DIRECTION`, converted rather than repeated.** The number
	// moved to `scene` when `CutAndCloneSeams` had to map it through a seam -
	// the far half of a body in a hole is lit by `R · L` - and a second
	// spelling here would be two suns that agree until somebody edits one.
	//
	// **The default, and no longer the answer.** `Renderer::SetSun` is what a
	// host calls with the world's `scene::Sun`, and a host that never calls it
	// draws with exactly what this engine has always drawn with.
	constexpr glm::vec3 SUN_DIRECTION{scene::SUN_DIRECTION.X, scene::SUN_DIRECTION.Y, scene::SUN_DIRECTION.Z};
	constexpr glm::vec4 SUN_AMBIENT{scene::SUN_AMBIENT.R, scene::SUN_AMBIENT.G, scene::SUN_AMBIENT.B, 1.0f};

	// Measured shadow-map resolution for the current scene scale.
	constexpr uint32_t SHADOW_RESOLUTION = 2048;

	// Measured resize threshold; keeps targets stable during viewport drags.
	constexpr uint32_t SCENE_TARGET_BLOCK = 64;

	// Rounds up to the next whole block, saturating rather than wrapping.
	inline uint32_t BlockUp(uint32_t value) {
		if (value > UINT32_MAX - (SCENE_TARGET_BLOCK - 1)) {
			return value;
		}
		return ((value + SCENE_TARGET_BLOCK - 1) / SCENE_TARGET_BLOCK) * SCENE_TARGET_BLOCK;
	}

	inline std::vector<uint8_t> ReadFile(const std::filesystem::path &path) {
		size_t size = 0;
		void *data = SDL_LoadFile(path.string().c_str(), &size);
		if (!data) {
			return {};
		}

		std::vector<uint8_t> bytes(
			static_cast<const uint8_t *>(data), static_cast<const uint8_t *>(data) + size
		);
		SDL_free(data);
		return bytes;
	}

	inline SDL_GPUTextureFormat DeviceFormat(graph::ResourceFormat format) {
		switch (format) {
		case graph::ResourceFormat::R8:
			return SDL_GPU_TEXTUREFORMAT_R8_UNORM;
		case graph::ResourceFormat::RG8:
			return SDL_GPU_TEXTUREFORMAT_R8G8_UNORM;
		case graph::ResourceFormat::RGBA8:
			return SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
		case graph::ResourceFormat::RGBA8_SRGB:
			return SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB;
		case graph::ResourceFormat::RGB10A2:
			return SDL_GPU_TEXTUREFORMAT_R10G10B10A2_UNORM;
		case graph::ResourceFormat::RG11B10F:
			return SDL_GPU_TEXTUREFORMAT_R11G11B10_UFLOAT;
		case graph::ResourceFormat::R16F:
			return SDL_GPU_TEXTUREFORMAT_R16_FLOAT;
		case graph::ResourceFormat::RG16F:
			return SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT;
		case graph::ResourceFormat::RGBA16F:
			return SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
		case graph::ResourceFormat::R32F:
			return SDL_GPU_TEXTUREFORMAT_R32_FLOAT;
		case graph::ResourceFormat::RG32F:
			return SDL_GPU_TEXTUREFORMAT_R32G32_FLOAT;
		case graph::ResourceFormat::D24S8:
			return SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT;
		case graph::ResourceFormat::D32F:
			return SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
		default:
			return SDL_GPU_TEXTUREFORMAT_INVALID;
		}
	}
}
