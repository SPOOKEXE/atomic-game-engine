#include "DisplayColour.hpp"
#include "ResourcePreview.hpp"
#include "ShaderBinary.hpp"
#include "SurfaceScale.hpp"
#include "VulkanTimestamps.hpp"

#include <engine/core/Log.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/graph/Cull.hpp>
#include <engine/graph/EntityFlow.hpp>
#include <engine/graph/ExecutionPlan.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/graph/Schedule.hpp>
#include <engine/graph/Shadow.hpp>
#include <engine/render/GraphRunner.hpp>
#include <engine/render/MeshTable.hpp>
#include <engine/render/MissingTexture.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/render/ShaderCompiler.hpp>
#include <engine/render/TextureTable.hpp>
#include <engine/resources/Shaders.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Sunlight.hpp>

// `scene::ReflectCamera` and `scene::SurfacePane`: where a mirror's camera goes
// for a viewer that is not the eye, which is what makes the levels below the
// first a recursion rather than a repeat.
#include <engine/scene/SurfaceCameras.hpp>
#include <engine/scene/Tagging.hpp>

#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_video.h>
#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iterator>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine::render {

	namespace {
		struct BackendNode {
			core::Name Kind;
			graph::NodeScope Scope;
			graph::ExecutionQueue Queue;
		};

		std::span<const BackendNode> BackendNodes() {
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
				BackendNode{
					core::Name("last-frame"), graph::NodeScope::View, graph::ExecutionQueue::Graphics
				},
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
				BackendNode{
					core::Name("transparent"), graph::NodeScope::View, graph::ExecutionQueue::Graphics
				},
				BackendNode{core::Name("raster"), graph::NodeScope::View, graph::ExecutionQueue::Graphics},
				BackendNode{core::Name("dispatch"), graph::NodeScope::View, graph::ExecutionQueue::Compute},
				BackendNode{core::Name("present"), graph::NodeScope::Frame, graph::ExecutionQueue::Graphics},
				BackendNode{core::Name("viewer"), graph::NodeScope::Frame, graph::ExecutionQueue::Transfer},
				BackendNode{core::Name("capture"), graph::NodeScope::Frame, graph::ExecutionQueue::Transfer},
				BackendNode{core::Name("overlay"), graph::NodeScope::Frame, graph::ExecutionQueue::Graphics},
				BackendNode{
					core::Name("interface"), graph::NodeScope::Frame, graph::ExecutionQueue::Graphics
				},
				BackendNode{
					core::Name("output-image"), graph::NodeScope::Frame, graph::ExecutionQueue::Transfer
				},
			};
			return nodes;
		}

		NodeTable BackendTable(const NodeHandler &handler) {
			NodeTable nodes;
			for (const BackendNode &node : BackendNodes()) {
				nodes.Set(node.Kind, handler);
			}
			return nodes;
		}

		// Keep the GPU vertex layout identical to the asset vertex layout.
		using Vertex = assets::MeshVertex;

		// One instance as the vertex shader reads it.
		//
		// Private GPU layout; scene data must not expose device types.
		struct GpuInstance {
			glm::mat4 Model{1.0f};
			glm::vec4 Colour{1.0f, 1.0f, 1.0f, 1.0f};

			// One over the square of each axis' scale, for the normal matrix.
			// The model includes scale, so its upper 3x3 is not a normal matrix.
			glm::vec4 InverseScaleSquared{1.0f, 1.0f, 1.0f, 0.0f};
		};

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
		GpuInstance ToGpu(const scene::DrawInstance &instance, const MeshEntry &mesh) {
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
			gpu.Model = instance.Frame.ToMatrix();

			gpu.Model[0] *= scale.x;
			gpu.Model[1] *= scale.y;
			gpu.Model[2] *= scale.z;

			// **Centred after scaling, in the part's own space.** A model
			// authored off its origin would otherwise hang away from the part by
			// however far its box is offset - and because the offset scales with
			// the part, it would grow as somebody resized it. Written into the
			// translation column rather than composed as a second matrix: this
			// runs once per instance per frame over the whole draw list.
			// The upper 3x3 already carries the rotation *and* the scale, so one
			// multiply moves the mesh's centre onto the part's origin.
			const glm::vec3 centre{mesh.Centre.X, mesh.Centre.Y, mesh.Centre.Z};
			gpu.Model[3] -= glm::vec4(glm::mat3(gpu.Model) * centre, 0.0f);

			// Convert author-facing transparency to shader alpha.
			gpu.Colour =
				glm::vec4{instance.Tint.R, instance.Tint.G, instance.Tint.B, 1.0f - instance.Transparency};

			// Avoid infinities for zero-scale geometry.
			const auto reciprocal = [](float axis) {
				return axis * axis > 1e-12f ? 1.0f / (axis * axis) : 1.0f;
			};
			gpu.InverseScaleSquared =
				glm::vec4{reciprocal(scale.x), reciprocal(scale.y), reciprocal(scale.z), 0.0f};
			return gpu;
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
		LightUniforms ToGpu(std::span<const SceneLight> lights) {
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

		// Mix renderer-owned view data into the scene signature.
		uint64_t MixFloat(uint64_t hash, float value) {
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
		bool DueToDraw(double drawn, float fps, double now) {
			if (!(fps > 0.0f) || drawn < 0.0 || !(now > drawn)) {
				return true;
			}
			return now - drawn >= 1.0 / static_cast<double>(fps);
		}

		uint64_t MixMatrix(uint64_t hash, const glm::mat4 &matrix) {
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
		constexpr glm::vec3 SUN_DIRECTION{
			scene::SUN_DIRECTION.X, scene::SUN_DIRECTION.Y, scene::SUN_DIRECTION.Z
		};
		constexpr glm::vec4 SUN_AMBIENT{
			scene::SUN_AMBIENT.R, scene::SUN_AMBIENT.G, scene::SUN_AMBIENT.B, 1.0f
		};

		// Measured shadow-map resolution for the current scene scale.
		constexpr uint32_t SHADOW_RESOLUTION = 2048;

		// Measured resize threshold; keeps targets stable during viewport drags.
		constexpr uint32_t SCENE_TARGET_BLOCK = 64;

		// Rounds up to the next whole block, saturating rather than wrapping.
		uint32_t BlockUp(uint32_t value) {
			if (value > UINT32_MAX - (SCENE_TARGET_BLOCK - 1)) {
				return value;
			}
			return ((value + SCENE_TARGET_BLOCK - 1) / SCENE_TARGET_BLOCK) * SCENE_TARGET_BLOCK;
		}

		std::vector<uint8_t> ReadFile(const std::filesystem::path &path) {
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

		SDL_GPUTextureFormat DeviceFormat(graph::ResourceFormat format) {
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

	namespace {
		bool CompileRenderPipeline(
			const graph::RenderGraph &pipeline,
			graph::CompiledGraph &compiled,
			graph::ExecutionSchedule &schedule,
			core::Name &offender,
			std::string &reason
		) {
			const graph::GraphStatus graphStatus = pipeline.Compile(compiled, offender);
			if (graphStatus != graph::GraphStatus::Ok) {
				reason = graph::Describe(graphStatus);
				return false;
			}
			const graph::ScheduleStatus status = graph::CompileSchedule(pipeline, schedule, offender);
			if (status != graph::ScheduleStatus::Ok) {
				reason = graph::Describe(status);
				return false;
			}

			// Resource edges, not canvas or declaration position, own execution.
			// SDL records each dependency wave serially, preserving the schedule on a
			// backend that exposes one portable command stream.
			compiled.Shared.clear();
			compiled.PerView.clear();
			compiled.Final.clear();
			for (const graph::ExecutionWave &wave : schedule.Waves) {
				for (const graph::ScheduledNode &scheduled : wave.Nodes) {
					const graph::Node *node = pipeline.Find(scheduled.Node);
					if (node == nullptr) {
						offender = {};
						reason = "the compiled schedule names no node";
						return false;
					}
					switch (node->Scope) {
					case graph::NodeScope::World:
						compiled.Shared.push_back(scheduled.Node);
						break;
					case graph::NodeScope::View:
						compiled.PerView.push_back(scheduled.Node);
						break;
					case graph::NodeScope::Frame:
						compiled.Final.push_back(scheduled.Node);
						break;
					}
				}
			}

			const NodeTable backend = BackendTable([](const graph::RunContext &) { return true; });
			const std::vector<core::Name> missing = backend.Missing(pipeline);
			if (!missing.empty()) {
				offender = missing.front();
				reason = "the renderer has no backend node for this kind";
				return false;
			}

			const std::span<const BackendNode> supported = BackendNodes();
			std::vector<bool> seen(supported.size(), false);
			for (const graph::ExecutionWave &wave : schedule.Waves) {
				for (const graph::ScheduledNode &scheduled : wave.Nodes) {
					const graph::Node *node = pipeline.Find(scheduled.Node);
					if (node == nullptr) {
						offender = {};
						reason = "the compiled schedule names no node";
						return false;
					}

					const auto found =
						std::find_if(supported.begin(), supported.end(), [node](const BackendNode &entry) {
							return entry.Kind == node->Kind;
						});
					if (found == supported.end()) {
						offender = node->Name;
						reason = "the renderer has no backend node for this kind";
						return false;
					}
					const size_t index = static_cast<size_t>(found - supported.begin());
					const bool repeatable =
						node->Kind == core::Name("cull-frustum") ||
						node->Kind == core::Name("cull-distance") || node->Kind == core::Name("filter-tag") ||
						node->Kind == core::Name("order-draw") || node->Kind == core::Name("raster") ||
						node->Kind == core::Name("dispatch") || node->Kind == core::Name("viewer") ||
						node->Kind == core::Name("capture");
					if (seen[index] && !repeatable) {
						offender = node->Name;
						reason = "a built-in render node kind may appear only once";
						return false;
					}
					const bool flexibleScope = node->Kind == core::Name("dispatch");
					if (node->Scope != found->Scope && !flexibleScope) {
						offender = node->Name;
						reason = "this backend node cannot run at the authored scope";
						return false;
					}
					seen[index] = true;
					if (const std::string *queue = node->Parameter(core::Name("queue"));
						queue != nullptr && *queue != "auto" && *queue != graph::Describe(found->Queue)) {
						offender = node->Name;
						reason = "this backend node requires the " +
								 std::string(graph::Describe(found->Queue)) + " queue";
						return false;
					}
					if (const std::string *culling = node->Parameter(core::Name("culling"));
						culling != nullptr) {
						if (*culling != "inherit" && node->Kind != core::Name("cull-frustum")) {
							offender = node->Name;
							reason = "culling belongs on an entity filter node, not this backend node";
							return false;
						}
						// Occlusion is accepted since the backend grew its depth
						// pyramid and indirect draw path, but it composes behind
						// the gbuffer pass: that pass's early phase is what
						// seeds the pyramid the cull tests against, so a
						// document without it authored a cull nothing can feed.
						if (*culling == "occlusion") {
							bool depthWriter = false;
							for (uint32_t value = 1; value <= pipeline.Count() && !depthWriter; value++) {
								const graph::Node *writer = pipeline.Find(graph::NodeId{value});
								depthWriter = writer != nullptr && writer->Enabled &&
											  writer->Kind == core::Name("gbuffer");
							}
							if (!depthWriter) {
								offender = node->Name;
								reason = "occlusion culling needs the gbuffer pass to seed its "
										 "depth pyramid";
								return false;
							}
						}
					}
				}
			}
			return true;
		}
	}

	// -----------------------------------------------------------------------

	struct Renderer::Impl {
		SDL_Window *Window = nullptr;
		SDL_GPUDevice *Device = nullptr;

		struct NamedPipeline {
			core::Name Name;
			graph::RenderGraph Graph;
			graph::CompiledGraph Compiled;
			graph::ExecutionSchedule Schedule;

			// The schedule's traffic plan, computed once at install. It decides
			// which command buffer class records each node, and its order is the
			// frame's submission order on SDL's one unified queue.
			std::vector<graph::PlannedCommandBuffer> Buffers;
		};

		std::optional<NamedPipeline> EngineDefault;
		std::vector<NamedPipeline> NamedPipelines;
		core::Name ActiveGraph;

		const NamedPipeline *PipelineFor(core::Name name) const {
			if (!name.IsValid()) {
				return EngineDefault ? &*EngineDefault : nullptr;
			}
			for (const NamedPipeline &pipeline : NamedPipelines) {
				if (pipeline.Name == name) {
					return &pipeline;
				}
			}
			return EngineDefault ? &*EngineDefault : nullptr;
		}

		enum class ResourceRole : uint8_t {
			Unknown,
			Scene,
			PreviousFrame,
			Depth,
			Shadow,
			Surface,
			PortalImage,
			PortalDisplay,
			PortalLight,
			Albedo,
			Normal,
			Material,
			Emissive,
			LinearDepth,
			Occlusion,
			Lit,
		};

		ResourceRole RoleFor(core::Name resource) const {
			const NamedPipeline *pipeline = PipelineFor(ActiveGraph);
			if (pipeline == nullptr) {
				return ResourceRole::Unknown;
			}
			for (uint32_t value = 1; value <= pipeline->Graph.Count(); value++) {
				const graph::Node *node = pipeline->Graph.Find(graph::NodeId{value});
				if (node == nullptr) {
					continue;
				}
				for (size_t output = 0; output < node->Writes.size(); output++) {
					const graph::ResourceDesc *desc = pipeline->Graph.FindResource(node->Writes[output]);
					if (desc == nullptr || desc->Name != resource) {
						continue;
					}
					if (node->Kind == core::Name("shadow")) {
						return ResourceRole::Shadow;
					}
					if (node->Kind == core::Name("last-frame")) {
						return ResourceRole::PreviousFrame;
					}
					if (node->Kind == core::Name("mirror-capture")) {
						return ResourceRole::Surface;
					}
					if (node->Kind == core::Name("portal-capture")) {
						// Output 0 is the recursion pool, output 1 the seam
						// light-field atlas - the default document's order.
						return output == 0 ? ResourceRole::PortalImage : ResourceRole::PortalLight;
					}
					if (node->Kind == core::Name("portal-tonemap")) {
						return ResourceRole::PortalDisplay;
					}
					if (node->Kind == core::Name("gbuffer")) {
						constexpr std::array roles{
							ResourceRole::Albedo,
							ResourceRole::Normal,
							ResourceRole::Material,
							ResourceRole::Emissive,
							ResourceRole::Depth,
						};
						return output < roles.size() ? roles[output] : ResourceRole::Unknown;
					}
					if (node->Kind == core::Name("depth-linearise")) {
						return ResourceRole::LinearDepth;
					}
					if (node->Kind == core::Name("ssao")) {
						return ResourceRole::Occlusion;
					}
					if (node->Kind == core::Name("deferred-lighting")) {
						return ResourceRole::Lit;
					}
				}
			}
			return ResourceRole::Unknown;
		}

		struct PreviewReadback {
			SDL_GPUTransferBuffer *Transfer = nullptr;
			uint32_t Capacity = 0;
			SDL_GPUFence *Fence = nullptr;
			core::Name Source;
			size_t Slot = Renderer::ANY_VIEWPORT;
			uint32_t Width = 0;
			uint32_t Height = 0;
			uint32_t BytesPerPixel = 0;
			bool Rgba = false;
			std::vector<uint32_t> Pixels;
			ImageHistogram Histogram;
			PendingReadback Pending;
		};

		PreviewReadback Preview;
		core::Name Inspected;
		size_t InspectedSlot = Renderer::ANY_VIEWPORT;
		uint64_t FrameCounter = 0;
		bool PreviewSubmitted = false;

		// A multi-view frame lends one command buffer to each view in turn. The
		// ordinary Render body still records a complete view, while these fields
		// keep acquisition, graph scope, timings, and submission frame-owned.
		bool BatchActive = false;
		bool BatchFirst = false;
		bool BatchFinal = false;
		bool BatchShared = false;
		bool BatchFailed = false;
		size_t BatchViewIndex = 0;
		size_t BatchWorldIndex = 0;
		SDL_GPUCommandBuffer *BatchCommand = nullptr;
		SDL_GPUTexture *BatchSwapchain = nullptr;
		uint32_t BatchWidth = 0;
		uint32_t BatchHeight = 0;
		uint32_t BatchTimingSlot = VulkanTimestamps::NO_SLOT;

		// The traffic plan's later-transfer command buffer: every download this
		// frame records - resource previews and captures - lands here rather
		// than in the main buffer, and it is submitted after the main buffer.
		// SDL's one unified queue executes submissions in order, so the copies
		// read the frame's finished images without a fence between the two.
		// Physical overlap is not available on that queue; the split is the
		// structural boundary `graph::PlanCommandBuffers` plans, in place for a
		// backend with an independent transfer queue.
		SDL_GPUCommandBuffer *DownloadCommand = nullptr;

		SDL_GPUCommandBuffer *DownloadBuffer() {
			if (DownloadCommand == nullptr) {
				DownloadCommand = SDL_AcquireGPUCommandBuffer(Device);
				if (DownloadCommand == nullptr) {
					ENGINE_ERROR("SDL_AcquireGPUCommandBuffer (downloads): {}", SDL_GetError());
				}
			}
			return DownloadCommand;
		}

		// A frame that failed after downloads were recorded: the buffer is
		// cancelled rather than submitted, and a pending preview stops waiting
		// for pixels that will never arrive.
		void DropDownloads() {
			if (DownloadCommand != nullptr) {
				SDL_CancelGPUCommandBuffer(DownloadCommand);
				DownloadCommand = nullptr;
			}
			if (PreviewSubmitted) {
				Preview.Pending.Poll(true);
				Preview.Pixels.clear();
				Preview.Histogram = ImageHistogram{};
				PreviewSubmitted = false;
			}
		}

		void CollectPreview();
		void PollPreview();
		bool RequestPreview(
			SDL_GPUCommandBuffer *command,
			SDL_GPUTexture *texture,
			uint32_t width,
			uint32_t height,
			core::Name source,
			size_t slot,
			SDL_GPUTextureFormat format
		);

		struct PassMarks {
			core::Name Name;
			uint32_t Opened = VulkanTimestamps::MARKS;
			uint32_t Closed = VulkanTimestamps::MARKS;
		};

		VulkanTimestamps Timestamps;
		std::array<std::vector<PassMarks>, VulkanTimestamps::SLOTS> PendingMarks;
		std::array<uint64_t, VulkanTimestamps::SLOTS> TimingSequence{};
		uint64_t NextTimingSequence = 1;
		uint64_t ResolvedTimingSequence = 0;
		std::unordered_map<uint32_t, double> GpuTimings;
		std::unordered_map<uint32_t, double> WallTimings;

		void CollectTimings();

		SDL_GPUGraphicsPipeline *OpaquePipeline = nullptr;

		// The two above, redrawn as lines. See where they are created for why
		// there are two objects and not a bindable state.
		SDL_GPUGraphicsPipeline *WireframeOpaquePipeline = nullptr;
		SDL_GPUGraphicsPipeline *WireframeTransparentPipeline = nullptr;

		// Whether `BindPipeline` should hand out the pair above instead of the
		// ordinary two. Off unless a caller has asked - `Renderer::
		// SetWireframe` is the only door.
		bool WireframeMode = false;

		// The default graph's opaque path. Geometry writes material properties,
		// then screen-sized passes consume those textures. Portal panes and the
		// blended tail stay on the forward family below because their projected
		// images and ordering are not representable by one G-buffer pixel.
		SDL_GPUGraphicsPipeline *GBufferPipeline = nullptr;
		SDL_GPUGraphicsPipeline *DepthLinearPipeline = nullptr;
		SDL_GPUGraphicsPipeline *SsaoPipeline = nullptr;
		SDL_GPUGraphicsPipeline *DeferredLightingPipeline = nullptr;
		SDL_GPUGraphicsPipeline *TonemapPipeline = nullptr;

		// `TonemapPipeline`'s replacement, when a world has asked for one -
		// see `Renderer::SetPostProcessShader`. Null is the ordinary state,
		// and the "tonemap" graph node falls back to `TonemapPipeline`
		// whenever it is.
		SDL_GPUGraphicsPipeline *PostProcessPipeline = nullptr;

		// Which name `PostProcessPipeline` was built for, so `Renderer::
		// PostProcessShaderName` can answer without a second field to keep
		// in step.
		core::Name PostProcessShaderName;

		struct PbrDimensions {
			uint32_t TargetWidth = 0;
			uint32_t TargetHeight = 0;
			uint32_t ViewWidth = 0;
			uint32_t ViewHeight = 0;
			uint32_t LinearWidth = 0;
			uint32_t LinearHeight = 0;
			uint32_t OcclusionWidth = 0;
			uint32_t OcclusionHeight = 0;
			uint32_t LitWidth = 0;
			uint32_t LitHeight = 0;

			bool operator==(const PbrDimensions &) const = default;
		};

		struct PbrSlot {
			SDL_GPUTexture *Albedo = nullptr;
			SDL_GPUTexture *Normal = nullptr;
			SDL_GPUTexture *Material = nullptr;
			SDL_GPUTexture *Emissive = nullptr;
			SDL_GPUTexture *LinearDepth = nullptr;
			SDL_GPUTexture *Occlusion = nullptr;
			SDL_GPUTexture *Lit = nullptr;
			PbrDimensions Dimensions;
		};

		std::vector<PbrSlot> PbrSlots;

		PbrSlot &PbrAt(size_t slot) {
			if (PbrSlots.size() <= slot) {
				PbrSlots.resize(slot + 1);
			}
			return PbrSlots[slot];
		}

		bool EnsurePbr(size_t slot, const PbrDimensions &dimensions);
		void ReleasePbr(PbrSlot &slot);

		struct NamedTexture {
			SDL_GPUTexture *Texture = nullptr;
			uint32_t Width = 0;
			uint32_t Height = 0;
			SDL_GPUTextureFormat Format = SDL_GPU_TEXTUREFORMAT_INVALID;

			bool IsValid() const {
				return Texture != nullptr && Width > 0 && Height > 0;
			}
		};

		// Graph targets are isolated by the pipeline that declared them and by
		// the scope that produced them. A view target belongs to a viewport slot,
		// while world work belongs to the caller's stable world key. This is the
		// storage rule that prevents two worlds with identically named resources
		// from sampling one another.
		struct GraphTarget {
			core::Name Pipeline;
			core::Name Resource;
			graph::NodeScope Scope = graph::NodeScope::Frame;
			uint64_t Owner = 0;
			SDL_GPUTexture *Texture = nullptr;
			SDL_GPUTextureFormat Format = SDL_GPU_TEXTUREFORMAT_INVALID;
			uint32_t Width = 0;
			uint32_t Height = 0;
		};

		std::vector<GraphTarget> GraphTargets;

		struct ResourcePreviewTarget {
			ResourcePreviewRoute Route;
			std::array<SDL_GPUTexture *, 2> Textures{};
			ResourcePreviewSlots Slots;
			uint32_t Width = 0;
			uint32_t Height = 0;
			bool ReverseSpectrum = false;
			bool Refresh = true;
		};

		std::vector<ResourcePreviewTarget> ResourcePreviews;

		graph::NodeScope ResourceScope(const NamedPipeline &pipeline, graph::ResourceId resource) const;
		NamedTexture FindGraphTarget(
			const NamedPipeline &pipeline, core::Name resource, graph::NodeScope scope, uint64_t owner
		) const;
		NamedTexture EnsureGraphTarget(
			const NamedPipeline &pipeline,
			graph::ResourceId resource,
			uint64_t owner,
			uint32_t viewWidth,
			uint32_t viewHeight
		);

		struct GraphRasterPipeline {
			core::Name Pipeline;
			core::Name Node;
			SDL_GPUTextureFormat Format = SDL_GPU_TEXTUREFORMAT_INVALID;
			uint32_t Samplers = 0;
			SDL_GPUGraphicsPipeline *Handle = nullptr;
		};

		struct GraphComputePipeline {
			core::Name Pipeline;
			core::Name Node;
			uint32_t Samplers = 0;
			uint32_t Storage = 0;
			uint32_t LocalX = 1;
			uint32_t LocalY = 1;
			uint32_t LocalZ = 1;
			SDL_GPUComputePipeline *Handle = nullptr;
		};

		std::vector<GraphRasterPipeline> GraphRasterPipelines;
		std::vector<GraphComputePipeline> GraphComputePipelines;
		struct GraphCaptureReceipt {
			core::Name Pipeline;
			core::Name Node;
			std::string Path;
		};
		std::vector<GraphCaptureReceipt> GraphCaptureReceipts;
		std::unordered_map<std::string, uint64_t> GraphCaptureFrames;

		bool GraphShaderCode(
			const graph::Node &node, ShaderStage stage, std::vector<uint8_t> &bytes, std::string &entryPoint
		) const;
		SDL_GPUGraphicsPipeline *GraphRasterFor(
			const NamedPipeline &pipeline,
			const graph::Node &node,
			SDL_GPUTextureFormat format,
			uint32_t samplers
		);
		SDL_GPUComputePipeline *GraphComputeFor(
			const NamedPipeline &pipeline,
			const graph::Node &node,
			uint32_t samplers,
			uint32_t storage,
			uint32_t localX,
			uint32_t localY,
			uint32_t localZ
		);
		void ReleaseGraphState(core::Name pipeline);
		void ReleaseAllGraphState();

		// The same geometry and the same shaders as the opaque pipeline, with
		// blending on and depth writes off. Two pipelines rather than one with
		// a uniform, because blend state is baked into a pipeline on every
		// modern API and cannot be changed by a draw call.
		SDL_GPUGraphicsPipeline *TransparentPipeline = nullptr;

		// --- shader variants --------------------------------------------------
		//
		// **A pipeline per named shader, per family, and no more general than
		// that.** `scene::MaterialRef::Shader` selects a *fragment* shader -
		// `scene::ShaderSource` says why only that stage - so a variant is the
		// opaque and transparent pipelines with one shader object swapped and
		// everything else identical. The vertex layout, the depth state and the
		// blend state are the renderer's and stay the renderer's.
		//
		// **Two pipelines and not one**, because blend state is baked into a
		// pipeline: a toon-shaded pane and a toon-shaded wall are two objects on
		// every modern API, exactly as `OpaquePipeline` and `TransparentPipeline`
		// already are.
		//
		// **The shadow pass has no variant and must not grow one.** It writes
		// depth and no colour, so a fragment shader that computed a colour would
		// cost a pass over the whole scene to produce nothing.
		//
		// This is a table of *substitutions*, not the beginning of a render
		// graph. When the node system arrives it is what says which shader a
		// pass wants, and this table becomes the backend that answers - so do
		// not grow a second way to describe a frame here in the meantime.
		struct ShaderVariant {
			SDL_GPUShader *Fragment = nullptr;
			SDL_GPUGraphicsPipeline *Opaque = nullptr;
			SDL_GPUGraphicsPipeline *Transparent = nullptr;
		};

		// Keyed by `core::Name::Id`, matching `MeshTable::Entries`.
		std::unordered_map<uint32_t, ShaderVariant> ShaderVariants;

		// The opaque vertex shader, kept rather than released.
		//
		// **Every other shader object is released once its pipeline holds it**,
		// and this one cannot be: a variant is built when a shader arrives,
		// which is any time after `CreatePipelines` ran, and it needs the same
		// vertex stage the opaque pipeline was built with. Building a second one
		// from the file would be two objects for one shader, free to disagree
		// the day `opaque.vert` changes shape.
		SDL_GPUShader *OpaqueVertexShader = nullptr;

		// The two descriptors a variant is derived from, kept whole.
		//
		// **Members rather than the locals `CreatePipelines` builds**, because a
		// `SDL_GPUGraphicsPipelineCreateInfo` is a struct of *pointers* into
		// arrays the caller owns - so keeping the info and letting the arrays go
		// out of scope is a dangling read at the next `AddShader`. The arrays
		// live here and the infos point at them.
		//@{
		SDL_GPUVertexBufferDescription VariantBuffers[2]{};
		SDL_GPUVertexAttribute VariantAttributes[9]{};
		SDL_GPUColorTargetDescription VariantOpaqueTarget{};
		SDL_GPUColorTargetDescription VariantBlendedTarget{};
		SDL_GPUGraphicsPipelineCreateInfo VariantOpaqueInfo{};
		SDL_GPUGraphicsPipelineCreateInfo VariantBlendedInfo{};
		bool VariantsReady = false;
		//@}

		// Which family the open pass last bound, so `DrawSlots` knows which
		// variant a slot's shader means and what to put back afterwards.
		enum class PipelineFamily : uint8_t {
			// Anything with no variants: the shadow, overlay, particle and
			// ribbon passes. A slot's shader is ignored while one is bound.
			Other,
			Opaque,
			Transparent,
		};

		PipelineFamily ActiveFamily = PipelineFamily::Other;
		SDL_GPUGraphicsPipeline *ActivePipeline = nullptr;

		// The submission order, rebuilt each frame and kept so it is not
		// reallocated per frame. See `scene::OrderForDrawing`.
		std::vector<uint32_t> DrawOrder;

		// Entity-list and viewpoint storage belongs to the renderer rather than one
		// `RenderView` call. Filter chains clear their contents between views while
		// retaining the capacity paid for by the largest view seen so far.
		graph::EntityFlow GraphEntities;
		graph::Viewpoints GraphViewpoints;

		// What survived culling: the indices, and the instances themselves.
		//
		// The copy makes scene and camera ranges contiguous in one buffer.
		std::vector<uint32_t> Visible;
		std::vector<scene::DrawInstance> VisibleInstances;

		// The whole draw list, ordered for the surface camera. What the shadow
		// pass and the surface pass draw, because neither is the eye's: a caster
		// off screen still shadows, and a mirror shows what is behind the
		// viewer.
		std::vector<scene::DrawInstance> SceneInstances;

		// The instances whose geometry has arrived, which is what every pass
		// below works from. See the filter in `Render` for why an instance
		// naming an absent mesh is dropped rather than drawn as a cube.
		//
		// Kept on the state rather than made per frame, so a steady scene stops
		// allocating after its first one - the rule every buffer here follows.
		std::vector<scene::DrawInstance> Drawable;

		// The same, for the rows belonging to *other* worlds - see `Render`'s
		// `foreign` argument. A separate buffer rather than a tail on `Drawable`
		// because every pass but the surface pass must not see these, and a
		// shared buffer is one `.size()` away from them all seeing them.
		std::vector<scene::DrawInstance> DrawableForeign;

		std::vector<uint32_t> SceneOrder;
		SDL_GPUGraphicsPipeline *ImagePipeline = nullptr;
		SDL_GPUGraphicsPipeline *OverlayPipeline = nullptr;

		// Every mesh and texture available to the renderer.
		MeshTable Meshes;
		TextureTable Textures;

		// How long animation has been running, as the caller measures it.
		//
		// **Given rather than read.** `render` holds no clock, which is the
		// standing rule `assets::Grant` and `cdn::Service::Pump` keep for the
		// same reason: a module with a notion of "now" of its own has one to
		// drift, and a recorded run could not replay.
		double AnimationSeconds = 0.0;

		// How many levels of mirror-in-mirror to resolve, or zero to measure it.
		//
		// **Zero by default, which is `scene::AUTOMATIC_SURFACE_BOUNCES`.** This
		// was welded at two for two versions and every scene resolved exactly
		// two levels whatever it was built from - a room with one mirror paying
		// for a level that could never show anything, and a corridor of facing
		// panes cut off one level into the effect. No constant is right for
		// both, and since the levels became a recursion the cost of guessing
		// high multiplies rather than adds: `panes × (panes - 1) ^ (levels - 1)`
		// where the old iteration went as `panes × levels`.
		//
		// So the ordinary case is that nobody states one and each viewport's
		// bank measures what the frame it just drew actually reached - see
		// `SurfaceBank::Bounces` and `scene::NextSurfaceBounces`. A world that
		// does state one (`workspace.SurfaceBounces`) or a run that does
		// (`--surface-bounces`) overrides the measurement outright, because an
		// author who has typed a number is not asking for a negotiation.
		uint32_t SurfaceBounces = 0;

		// How many levels the recursive portal pass goes to.
		//
		// **One, and it is a backend limit rather than a taste.** Two is what the
		// pass is for - a hole through a hole, resolved inside the frame from the
		// right viewpoint at each level - and it draws correctly. What it also
		// does, on SDL's Vulkan backend, is hang the device: at two levels a
		// level-zero target is rendered into once per level-one hole, so within
		// one command buffer the same texture is written, sampled, written again
		// and sampled again, and `SDL_WaitForGPUIdle` at shutdown then never
		// returns.
		//
		// Measured rather than suspected, on `Portals-1-world.luau` at twenty
		// frames: twenty of twenty runs hang at depth two and none of thirteen at
		// depth one. Forcing the nested pane to draw flat - the same recursion,
		// the same pass count, nothing sampled - is clean, which is what pins it
		// to the write-after-read rather than to the volume of work. Cycling the
		// colour target halves it and does not close it.
		//
		// **The knob is public and unclamped below `MAX_PORTAL_DEPTH`**, so this
		// is a default and not a wall; what it needs to be raised safely is a
		// command buffer per top-level hole, which is where each level-zero target
		// would be written once and read once.
		uint32_t PortalDepth = 2;

		// The world's directional light, as the shader wants it.
		//
		// **Held rather than taken per frame, for `SetSurfaceBounces`' reason**:
		// it is a property of what is being drawn rather than of this frame, and
		// threading it through `Render` would put it in a signature every host
		// calls to say the same thing on every frame. `client::Client::Frame`
		// writes it from `scene::LightingOf` before each world's render.
		glm::vec3 Sun{SUN_DIRECTION};
		glm::vec4 Ambient{SUN_AMBIENT};
		glm::vec4 OutdoorAmbient{0.0f, 0.0f, 0.0f, 1.0f};
		glm::vec4 Direct{1.0f, 1.0f, 1.0f, 1.0f};
		glm::vec4 FogColour{0.05f, 0.06f, 0.09f, 1.0f};
		float FogStart = 100000.0f;
		float FogEnd = 100001.0f;

		// What is in each slot of the instance buffer, filled in the same loop
		// that fills the buffer itself.
		//
		// **Parallel arrays rather than a struct**, because the draw loop
		// compares consecutive entries to find its runs and does it far more
		// often than it reads them.
		std::vector<const MeshEntry *> SlotMesh;
		std::vector<core::Name> SlotTexture;
		std::vector<core::Name> SlotNormalMap;
		std::vector<core::Name> SlotRoughnessMap;
		std::vector<core::Name> SlotOcclusionMap;
		std::vector<core::Name> SlotHeightMap;
		std::vector<core::Name> SlotEmissiveMap;

		// Which shader each slot asks for, or an invalid name for the engine's.
		//
		// **A name per slot rather than a resolved pipeline**, because the run
		// loop compares consecutive entries far more often than it uses one -
		// the same reason every array here is parallel rather than a struct. The
		// lookup happens once per run, where the pipeline is bound.
		std::vector<core::Name> SlotShader;

		// Each slot's tag mask, for the surface passes that filter by one.
		std::vector<uint32_t> SlotTags;

		// The half-space each slot keeps, as a world plane: xyz the unit normal,
		// w the offset, and a zero normal for "whole".
		//
		// **A per-slot plane rather than a run of its own in the plan.** A body
		// standing in a portal is cut at the pane and its far half is drawn in the
		// room beyond; the cut is per instance and every uniform here is per frame
		// or per draw, so it is carried the way the tag mask is - the run breaks
		// where the plane changes, exactly as it breaks where the mesh does. A
		// world with no hole in it holds one value throughout and pays one compare
		// per instance for it. See `scene::DrawInstance::SeamNormal`.
		std::vector<glm::vec4> SlotSeam;

		// Which way the sun comes from for each slot, or a zero vector for the
		// world's own. `scene::DrawInstance::SeamLight` carries the argument.
		std::vector<glm::vec4> SlotSeamLight;

		SDL_GPUBuffer *InstanceBuffer = nullptr;
		SDL_GPUTransferBuffer *InstanceTransfer = nullptr;
		uint32_t InstanceCapacity = 0;

		// --- occlusion culling ------------------------------------------------
		//
		// The machinery behind `culling = "occlusion"` on an authored entity
		// filter node: a depth pyramid seeded by the occluders the CPU picked,
		// a compute pass that tests every remaining opaque instance's box
		// against it, and indexed indirect draw arguments so the survivor
		// counts never make the round trip back to the CPU.
		//
		// **The pyramid is separate textures rather than one texture's mips.**
		// A compute pass cannot bind one mip of a texture for writing while
		// sampling another mip of the same texture, and SDL's read-only
		// storage-texture binding cannot name a mip at all. Twelve levels cover
		// a screen rectangle up to 2048 texels wide; a candidate wider than
		// that is declared visible, which is the conservative direction.
		static constexpr uint32_t PYRAMID_LEVEL_LIMIT = 12;

		struct OcclusionState {
			SDL_GPUComputePipeline *Seed = nullptr;
			SDL_GPUComputePipeline *Reduce = nullptr;
			SDL_GPUComputePipeline *Cull = nullptr;
			SDL_GPUComputePipeline *Args = nullptr;

			SDL_GPUTexture *Levels[PYRAMID_LEVEL_LIMIT] = {};
			uint32_t Width = 0;
			uint32_t Height = 0;
			uint32_t LevelCount = 0;

			// The per-frame buffers, all sized in whole entries and grown in
			// powers of two like `InstanceBuffer`. One transfer buffer stages
			// everything the CPU writes, packed back to back.
			SDL_GPUBuffer *Arguments = nullptr;	 // indexed indirect commands, early then late
			SDL_GPUBuffer *Candidates = nullptr; // two vec4 per candidate
			SDL_GPUBuffer *RunTable = nullptr;	 // per slot-run: first late slot
			SDL_GPUBuffer *ArgRuns = nullptr;	 // per late argument: its slot-run
			SDL_GPUBuffer *Counts = nullptr;	 // per slot-run: survivor count
			SDL_GPUBuffer *LateInstances = nullptr;
			SDL_GPUTransferBuffer *Transfer = nullptr;
			uint32_t ArgumentCapacity = 0;
			uint32_t CandidateCapacity = 0;
			uint32_t RunCapacity = 0;
			uint32_t LateCapacity = 0;
			uint32_t TransferCapacity = 0;
		};
		OcclusionState Occlusion;

		// What this frame's cull draws, decided while the instances convert.
		//
		// `RunEarly[r]` instances at the head of slot-run `r` are the occluders
		// the CPU picked; the `RunCandidates[r]` behind them wait on the GPU
		// test. Both phases walk the same runs `DrawSlots` walks, so the
		// argument order is the walk order and nothing stores a mapping.
		struct OcclusionPlan {
			bool Active = false;
			uint32_t RunCount = 0;
			uint32_t ArgCount = 0; // per phase: one per slot-run material range
			uint32_t CandidateCount = 0;
			uint32_t EarlyTotal = 0;
			std::vector<uint32_t> RunEarly;
			std::vector<uint32_t> RunCandidates;
			std::vector<uint32_t> RunFirstSlot;
			// Two vec4 per candidate, already in the layout the cull reads -
			// see occlusion-cull.comp.
			std::vector<glm::vec4> CandidatePairs;
		};
		OcclusionPlan OcclusionFrame;

		// Whether two slots may share one instanced draw. Everything a run
		// binds per draw has to agree, so this is the run-break rule - the one
		// `DrawSlots` walks with and the occlusion plan must walk with, or the
		// plan's argument order names the wrong runs.
		bool SlotsShareRun(uint32_t slot, uint32_t next) const {
			return SlotMesh[next] == SlotMesh[slot] && SlotTexture[next] == SlotTexture[slot] &&
				   SlotNormalMap[next] == SlotNormalMap[slot] &&
				   SlotRoughnessMap[next] == SlotRoughnessMap[slot] &&
				   SlotOcclusionMap[next] == SlotOcclusionMap[slot] &&
				   SlotHeightMap[next] == SlotHeightMap[slot] &&
				   SlotEmissiveMap[next] == SlotEmissiveMap[slot] && SlotShader[next] == SlotShader[slot] &&
				   SlotSeam[next] == SlotSeam[slot] && SlotSeamLight[next] == SlotSeamLight[slot];
		}

		// One phase of the occlusion-culled pass for `DrawSlots`: where its
		// indirect arguments start and how many instances each slot-run may
		// draw, which is what lets a run with nothing to draw skip its binds.
		struct IndirectPhase {
			SDL_GPUBuffer *Arguments = nullptr;
			uint32_t FirstArgument = 0;
			const std::vector<uint32_t> *RunDraws = nullptr;
		};

		bool EnsureOcclusionResources(
			uint32_t argCount, uint32_t candidateCount, uint32_t runCount, uint32_t lateCount
		);
		bool EnsurePyramid(uint32_t width, uint32_t height);
		void BuildPyramid(SDL_GPUCommandBuffer *command, SDL_GPUTexture *depth);
		void DispatchOcclusionCull(SDL_GPUCommandBuffer *command, const glm::mat4 &viewProjection);
		void ReleaseOcclusion();

		// --- particles --------------------------------------------------------
		//
		// **Two pipelines and one buffer.** The two differ only in their blend
		// state - one mixes into the target and one adds to it - and blend state
		// is baked into a pipeline on every modern API, which is the same reason
		// `TransparentPipeline` is a second object rather than a uniform on the
		// first.
		//
		// **Additive is not a cosmetic variant.** Adding is commutative, so an
		// additive emitter's particles need no back-to-front sort at all; at half a
		// million particles that is the difference between sorting and not.
		SDL_GPUGraphicsPipeline *ParticlePipeline = nullptr;
		SDL_GPUGraphicsPipeline *AdditiveParticlePipeline = nullptr;

		// One instance buffer for every particle in the frame.
		//
		// Separate from `InstanceBuffer` because the strides differ - 28 bytes
		// against 96 - and a shared buffer would mean either padding a particle to
		// a mesh instance's width or rebinding the vertex layout mid-pass.
		//
		// **No transfer buffer beside it since v0.17**, because nothing uploads
		// to it: `particle-step.comp` writes it as a storage buffer and the draw
		// reads it as a vertex stream, and the sixteen megabytes a frame that
		// used to cross to fill it do not.
		SDL_GPUBuffer *ParticleBuffer = nullptr;
		uint32_t ParticleCapacity = 0;

		// One run of particles that share every uniform and every binding.
		//
		// **What makes the target count drawable at all.** One draw call per
		// emitter is a hundred thousand of them at the roadmap's scale; grouping
		// by state takes a grid of identical emitters to a single call.
		struct ParticleGroup {
			// Which batch's state this group draws with. Every batch folded into
			// it compares equal under `SameParticleState`, so any of them would
			// do and the first is the one kept.
			uint32_t Batch = 0;

			// Where this group's particles start in the shared buffer.
			uint32_t First = 0;

			// How many there are.
			uint32_t Count = 0;
		};

		// --- ribbons ----------------------------------------------------------
		//
		// **The same two-pipeline shape the particles have**, and for the same
		// reason: blend state is baked into a pipeline, and an additive ribbon is
		// order-independent where a blended one is not.
		//
		// The primitive differs. A particle is a quad expanded from its vertex
		// index; a ribbon is a real vertex stream, because its geometry is a
		// function of where its endpoints are and that is resolved on the CPU
		// where a test can reach it - `ribbon.vert` carries the argument.
		SDL_GPUGraphicsPipeline *RibbonPipeline = nullptr;
		SDL_GPUGraphicsPipeline *AdditiveRibbonPipeline = nullptr;

		SDL_GPUBuffer *RibbonBuffer = nullptr;
		SDL_GPUTransferBuffer *RibbonTransfer = nullptr;
		uint32_t RibbonCapacity = 0;

		// Grows the ribbon buffer, on `ReserveParticles`'s terms.
		bool ReserveRibbons(uint32_t count);

		// Uploads this frame's ribbon vertices, outside every render pass.
		//
		// @return How many vertices were packed.
		uint32_t PrepareRibbons(std::span<const effects::RibbonVertex> vertices);

		// Draws the runs `effects::BuildRibbons` produced.
		//
		// **One draw call per run and no grouping**, which is the opposite of the
		// particle path and is right for the opposite reason: a run is already a
		// whole beam or a whole trail, so a scene has tens of them where it has
		// tens of thousands of emitters. Grouping would save a bind on a count
		// that does not need saving, and it cannot merge two runs anyway - they
		// are separate strips, and a strip drawn as one primitive would connect
		// the end of one to the start of the next.
		//
		// @param triangles Added to, rather than set - `DrawSlots` takes the
		//        frame's running total the same way.
		// @return How many draw calls were issued.
		uint32_t DrawRibbons(
			SDL_GPUCommandBuffer *command,
			SDL_GPURenderPass *pass,
			const glm::mat4 &viewProjection,
			const core::CFrame &eye,
			std::span<const effects::RibbonRun> runs,
			uint64_t &triangles
		);

		// This frame's groups, and the batch order they were built from.
		//
		// Members rather than locals, so the capacity survives the frame - the
		// same argument `DrawOrder` makes one screen up.
		std::vector<ParticleGroup> ParticleGroups;
		std::vector<uint32_t> ParticleOrder;

		// --- the device-resident pool -----------------------------------------
		//
		// **The particles live here and are simulated here.** Until v0.17 the
		// host stepped the pool into its own array and this class copied the
		// result across every frame: at half a million particles that is sixteen
		// megabytes, and `particles.pack` was the single largest thing a
		// particle frame did - larger than the simulation and two orders of
		// magnitude larger than recording the draw.
		//
		// What crosses now is `Blocks` - one 384-byte record per emitter, which
		// is a five-thousand-emitter scene's two megabytes - and `Births`, which
		// is whatever the tick spawned. `particle-step.comp` does the rest and
		// writes its output straight into `ParticleBuffer` at the offsets
		// `PrepareParticles` worked out, so the draw grouping is exactly what it
		// was and there is no gather pass.
		struct ParticlePool {
			// One state per slot, `STATE_WORDS` wide, read and written by the
			// step and written by the spawn. Never read by the host.
			SDL_GPUBuffer *States = nullptr;
			uint32_t Slots = 0;

			// This frame's block records, in the sorted batch order, so a
			// workgroup index is a record index and no live list is needed.
			//@{
			SDL_GPUBuffer *Blocks = nullptr;
			SDL_GPUTransferBuffer *BlockStaging = nullptr;
			uint32_t BlockCapacity = 0;
			//@}

			// This frame's births, each a slot and the state to put in it.
			//@{
			SDL_GPUBuffer *Births = nullptr;
			SDL_GPUTransferBuffer *BirthStaging = nullptr;
			uint32_t BirthCapacity = 0;
			//@}

			// The portal panes, if the scene has any.
			//@{
			SDL_GPUBuffer *Seams = nullptr;
			SDL_GPUTransferBuffer *SeamStaging = nullptr;
			uint32_t SeamCapacity = 0;
			//@}

			SDL_GPUComputePipeline *Step = nullptr;
			SDL_GPUComputePipeline *Spawn = nullptr;

			// What was staged this frame, for the dispatch that follows.
			//@{
			uint32_t Records = 0;
			uint32_t BirthCount = 0;
			uint32_t SeamCount = 0;
			float Delta = 0.0f;
			//@}

			// Which batch list the instance stream currently holds.
			//
			// **A frame has several views and one pool.** A mirror and the room
			// it reflects are two views of the same particles, and stepping for
			// each would age them twice a frame. Every caller in the engine gives
			// its views the same batch list - the client collects once and hands
			// the span to each - so the second view's draw ranges are the ones
			// already in the buffer and there is nothing to redo.
			//
			// That is what makes `View::ParticleDelta` the caller's declaration
			// of which view advances time: a repeat view passes zero and is
			// skipped, and a view that genuinely brought its own emitters is
			// caught here and re-staged whatever it passed.
			//@{
			const ParticleBatch *StagedFrom = nullptr;
			size_t StagedCount = 0;
			//@}
		} Particles;

		// Grows the pool's state buffer to `slots`, and the three staging pairs
		// to what this frame needs.
		//
		// **The state buffer is never re-created once it is big enough**, and it
		// must not be: it is the simulation. Re-creating it would empty every
		// live particle in the scene, which is a visible pop rather than a slow
		// frame.
		//@{
		bool ReserveParticlePool(uint32_t slots);
		bool ReserveParticleStaging(uint32_t records, uint32_t births, uint32_t seams);
		//@}

		// Runs the spawn scatter and then the step, on their own command buffer.
		//
		// **Its own submission, and once per frame rather than once per view.**
		// The pool is the world's and not a camera's, so stepping it inside a
		// view's command buffer would age every particle again for a mirror.
		// Submission order is what makes the draws that follow see it.
		bool DispatchParticles();

		void ReleaseParticlePool();

		// Grows the particle buffer to hold at least `count`, keeping what fits.
		//
		// **Grown and never shrunk**, exactly as the instance buffer is: an
		// explosion is a spike in the particle count and a frame that reallocated
		// on the way back down would pay for the spike twice.
		bool ReserveParticles(uint32_t count);

		// Groups the batches, decides where each block's run lands in the draw
		// stream, and stages the records, births and seams the step will read.
		//
		// **Outside every render pass**, because the copy that follows it is a
		// copy pass and a copy pass cannot be started while a render pass is
		// open. The first version of this did the memcpy inside the draw and
		// never issued the copy at all, so the vertex buffer held whatever the
		// previous frame left - which draws *something*, which is why it took a
		// capture rather than a crash to find.
		//
		// @return How many instance slots the frame will draw.
		uint32_t PrepareParticles(const View &view);

		// Draws what the step wrote.
		//
		// @param triangles Added to, rather than set - `DrawSlots` takes the
		//        frame's running total the same way.
		// @return How many draw calls were issued.
		uint32_t DrawParticles(
			SDL_GPUCommandBuffer *command,
			SDL_GPURenderPass *pass,
			const glm::mat4 &viewProjection,
			const core::CFrame &eye,
			std::span<const ParticleBatch> batches,
			uint64_t &triangles
		);

		// Chosen once so pipelines and depth textures use one supported format.
		SDL_GPUTextureFormat DepthFormat = SDL_GPU_TEXTUREFORMAT_D16_UNORM;

		SDL_GPUTexture *DepthTexture = nullptr;
		uint32_t DepthWidth = 0;
		uint32_t DepthHeight = 0;

		// --- the offscreen scene target ---------------------------------------
		//
		// Where the world goes when a caller asks for a texture instead of the
		// window. See `render::SceneTarget` for why an editor needs one.
		// One offscreen target per viewport asking for one.
		//
		// **A vector rather than a single texture, and the editor is why.** Two
		// viewports are two different sizes, so one shared target would be
		// destroyed and recreated twice a frame as each panel asked for its
		// own - a colour and a depth texture per frame, which is exactly the
		// cost `RetiredScenes` exists to avoid paying even once.
		struct SceneSlot {
			SDL_GPUTexture *Texture = nullptr;

			// What was allocated, which is the panel's size rounded up to a
			// block. See `SCENE_TARGET_BLOCK`.
			uint32_t Width = 0;
			uint32_t Height = 0;

			// The rectangle inside it the world is drawn into, which is the
			// panel's size exactly. What the pass sets its viewport to and what
			// `SceneTextureExtent` reports. See `render::SceneExtent`.
			uint32_t DrawnWidth = 0;
			uint32_t DrawnHeight = 0;

			// Per-slot depth matches the block-rounded colour target dimensions.
			SDL_GPUTexture *Depth = nullptr;
			uint32_t DepthWidth = 0;
			uint32_t DepthHeight = 0;

			// The previous completed graph output. A swapchain image is neither
			// owned by the renderer nor guaranteed to support sampling, so the
			// last-frame graph resource must never alias it.
			SDL_GPUTexture *History = nullptr;
			uint32_t HistoryWidth = 0;
			uint32_t HistoryHeight = 0;
			bool HistoryReady = false;
		};

		std::vector<SceneSlot> SceneSlots;

		// The slot the frame in progress is drawing into.
		size_t ActiveSlot = 0;

		SceneSlot &SlotAt(size_t slot) {
			if (SceneSlots.size() <= slot) {
				SceneSlots.resize(slot + 1);
			}
			return SceneSlots[slot];
		}

		// Scene targets that have been replaced but may still be referenced.
		//
		// Keep replaced targets alive until interface draw lists finish this frame.
		std::vector<SDL_GPUTexture *> RetiredScenes;

		// Frees what the previous frame retired. Called once at the top of a
		// frame, which is the only point at which no draw list can still name
		// one of them.
		void DrainRetiredScenes() {
			for (SDL_GPUTexture *texture : RetiredScenes) {
				SDL_ReleaseGPUTexture(Device, texture);
			}
			RetiredScenes.clear();
		}

		// --- the frame that has been waited for but not yet recorded ----------
		//
		// Hold the claimed frame between WaitForFrame and Render.
		SDL_GPUCommandBuffer *PendingCommand = nullptr;
		SDL_GPUTexture *PendingSwapchain = nullptr;
		uint32_t PendingWidth = 0;
		uint32_t PendingHeight = 0;

		// Whether `BeginFrame` has claimed this frame and `Render` has not yet
		// consumed it.
		bool FrameClaimed = false;

		// A present mode asked for, waiting for a legal moment to be set.
		//
		// Changing present mode recreates the swapchain; apply only before acquire.
		SDL_GPUPresentMode PendingPresentMode = SDL_GPU_PRESENTMODE_VSYNC;
		bool PresentModePending = false;

		// Sets it, if one was asked for. Safe only while no frame is claimed.
		void ApplyPresentMode() {
			if (!PresentModePending) {
				return;
			}
			PresentModePending = false;

			if (Device == nullptr || Window == nullptr) {
				return;
			}

			if (!SDL_SetGPUSwapchainParameters(
					Device, Window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, PendingPresentMode
				)) {
				// **Warned rather than reported back.** The caller was told
				// whether the mode was *supported* when it asked, which is the
				// question a checkbox can act on; a driver failing the set a frame
				// later is not something there is anybody left to tell. The mode
				// is unchanged, which is the safe outcome.
				ENGINE_WARN("SDL_SetGPUSwapchainParameters: {}", SDL_GetError());
			}
		}

		// Claims this frame: drains what the last one retired, takes a command
		// buffer, and waits for a swapchain image unless there is no window.
		//
		// **Idempotent within a frame**, so a caller that waits explicitly and a
		// `Render` that would have waited for itself cannot acquire twice. That
		// matters more than it looks: two swapchain acquisitions in one frame is
		// two frames in flight consumed for one presented, which reads as the
		// frame rate halving for no reason a profile can show.
		//
		// @return `false` when there was nothing to acquire - minimised or
		//         mid-resize, which is not an error.
		bool BeginFrame() {
			if (FrameClaimed) {
				return true;
			}
			if (Device == nullptr) {
				return false;
			}

			// **Before the acquire, which is the only moment it is legal.** See
			// `PendingPresentMode`.
			ApplyPresentMode();

			// **Before anything this frame records or binds.** Whatever a
			// previous frame retired is unreferenced now: its draw lists have
			// been replayed and thrown away, and nothing has yet recorded a bind
			// for this frame. Doing it here rather than in `Render` is what keeps
			// that true once the wait moved ahead of the interface - an editor
			// records its draw lists between the two calls.
			DrainRetiredScenes();

			SDL_GPUCommandBuffer *command = nullptr;
			{
				ENGINE_PROFILE_CAT("acquire command buffer", core::ProfileCategory::Render);
				command = SDL_AcquireGPUCommandBuffer(Device);
			}
			if (command == nullptr) {
				ENGINE_ERROR("SDL_AcquireGPUCommandBuffer: {}", SDL_GetError());
				return false;
			}

			// **Headless waits for nothing and is not a failure.** There is no
			// swapchain to acquire and nothing to present; the frame is finished
			// when the world has been drawn into its target.
			if (Headless()) {
				PendingCommand = command;
				PendingSwapchain = nullptr;
				PendingWidth = 0;
				PendingHeight = 0;
				FrameClaimed = true;
				return true;
			}

			SDL_GPUTexture *swapchain = nullptr;
			uint32_t width = 0;
			uint32_t height = 0;
			bool acquired = false;
			{
				// Where the frame waits, and the reason this one has a span of
				// its own. "WaitAnd" is not decoration: with vertical sync on
				// this blocks until the display is ready, and with it off it
				// blocks until the GPU hands back a swapchain image. Either way
				// the time is real, the CPU is idle for it, and it is not a cost
				// anything above this can do anything about.
				//
				// A frame that looks slow with everything else on the panel
				// adding up to nothing is a frame that is waiting here - which
				// means the GPU is the limit, not the code above it.
				//
				// Idle, not Render. Nothing is being rendered here - the thread
				// is asleep until the display is ready for another image, and
				// counting that as rendering work makes the renderer look like
				// the most expensive thing in a frame it spent waiting.
				ENGINE_PROFILE_CAT("acquire swapchain", core::ProfileCategory::Idle);
				acquired =
					SDL_WaitAndAcquireGPUSwapchainTexture(command, Window, &swapchain, &width, &height);
			}

			if (!acquired || swapchain == nullptr) {
				// Minimised, or mid-resize. Not an error, and not a reason to
				// stop ticking - the simulation carries on and the next frame
				// presents.
				//
				// **Cancelled rather than submitted, which is what SDL's own
				// example does here.** No swapchain texture was acquired, so
				// there is nothing to present and nothing recorded worth
				// executing; submitting an empty buffer sends it through the
				// whole submit path and consumes a frame in flight for no work.
				// Cancel is only legal *because* the acquire failed -
				// `SDL_CancelGPUCommandBuffer` is documented as an error once a
				// swapchain texture has been acquired, which is why every later
				// bail-out submits instead.
				SDL_CancelGPUCommandBuffer(command);
				return false;
			}

			PendingCommand = command;
			PendingSwapchain = swapchain;
			PendingWidth = width;
			PendingHeight = height;
			FrameClaimed = true;
			return true;
		}

		// Hands the claimed frame to whoever is about to record it.
		void TakeFrame(
			SDL_GPUCommandBuffer *&command, SDL_GPUTexture *&swapchain, uint32_t &width, uint32_t &height
		) {
			command = PendingCommand;
			swapchain = PendingSwapchain;
			width = PendingWidth;
			height = PendingHeight;

			PendingCommand = nullptr;
			PendingSwapchain = nullptr;
			PendingWidth = 0;
			PendingHeight = 0;
			FrameClaimed = false;
		}

		// Gets rid of a frame that was claimed and will never be recorded.
		//
		// **Submitted and not cancelled**, because a swapchain texture has been
		// acquired by the time this can be reached and SDL documents cancelling
		// after that as an error. An empty submit presents nothing and costs one
		// trip through the submit path, which is the correct price for a caller
		// that waited for a frame and then decided to quit.
		void AbandonFrame() {
			if (!FrameClaimed) {
				return;
			}
			if (PendingCommand != nullptr) {
				SDL_SubmitGPUCommandBuffer(PendingCommand);
			}

			PendingCommand = nullptr;
			PendingSwapchain = nullptr;
			PendingWidth = 0;
			PendingHeight = 0;
			FrameClaimed = false;
		}

		// Where the next capture goes, or empty for none. See
		// `Renderer::RequestSceneCapture`.
		std::filesystem::path CapturePath;

		// Which viewport's scene the pending capture wants, or `ANY_VIEWPORT`.
		//
		// **A panel per scene means the next `Render` is usually the wrong
		// one.** See `Renderer::RequestSceneCapture`: the request outlives the
		// call that made it, and honouring it in whichever call comes next
		// photographs whatever that panel happens to be showing.
		size_t CaptureSlot = Renderer::ANY_VIEWPORT;

		bool WriteCapture(
			SDL_GPUTransferBuffer *from,
			uint32_t width,
			uint32_t height,
			SDL_GPUTextureFormat format,
			const std::filesystem::path &path
		) const;

		// --- the shadow map -------------------------------------------------
		//
		// A depth texture and the pipeline that fills it. The **same instance
		// buffer** the colour pass binds, which is what makes a shadow map one
		// more draw over data that is already on the device.
		SDL_GPUGraphicsPipeline *ShadowPipeline = nullptr;
		SDL_GPUTexture *ShadowTexture = nullptr;
		SDL_GPUSampler *ShadowSampler = nullptr;

		// The beams: up to four holes' worth of shadow, in one 2x2 atlas.
		//
		// **One texture rather than four, because a fragment binds samplers and
		// not maps.** Every fragment tests every live beam, so four textures
		// would be four more samplers on every draw in the frame to serve a
		// handful of pixels near a doorway. The atlas costs one sub-rectangle per
		// beam in the uniform and one viewport per beam in the pass.
		SDL_GPUTexture *BeamTexture = nullptr;

		// What every pass pushes, whether or not anything crossed. A count of
		// zero is the ordinary case and the shader's loop ends immediately.
		BeamUniforms Beams;

		// The largest overflow already reported. Portal counts are stable for
		// most scenes, so logging the same capacity decision every frame only
		// hides later rendering diagnostics in thousands of duplicate lines.
		size_t MaximumBeamCandidatesWarned = MAX_PORTAL_BEAMS;

		// --- the surface target ----------------------------------------------
		//
		// Surface targets use colour and depth, with ping-pong textures to prevent
		// render-target self-sampling. The surface flag is per draw.
		struct SurfaceSlotState {
			SDL_GPUTexture *Texture[2] = {nullptr, nullptr};
			SDL_GPUTexture *Depth = nullptr;
			uint32_t Width = 0;
			uint32_t Height = 0;

			// Which of the pair this frame wrote. The other is what a surface
			// pass samples, and it holds the frame before.
			uint32_t Slot = 0;

			// Whether either texture holds a frame yet.
			//
			// The first frame has nothing to show, so a mirror draws as its own
			// tint rather than sampling whatever the driver handed back.
			bool Ready = false;

			// The frame clock when this slot last drew, for
			// `SurfaceView::FPS`.
			//
			// **Per slot rather than one stamp for the pass**, because the
			// surfaces do not refresh together and never have: one that has
			// never rendered has nothing to compare against, and one whose
			// content has not moved is skipped for a different reason entirely.
			// A shared stamp would let a mirror that redrew for its own reasons
			// reset the interval of every other mirror in the room.
			double Drawn = -1.0;

			// World to this surface camera's clip space, for the frame just
			// written. What the surface pass renders with.
			glm::mat4 ViewProjection{1.0f};

			// The same, with the pane's own map folded in. **What a pane
			// projects with, which is not the matrix the texture was rendered
			// with.** A mirror's map is the identity and the two are equal; a
			// portal's takes the pane to where its camera was fitted, and
			// without it every fragment projects outside the image. See
			// `scene::SurfaceLens::Mapping`.
			glm::mat4 Sampling{1.0f};

			// The pair again, for the frame before - which is the one another
			// surface pass samples, and it must be projected with the matrix
			// that *rendered* it. Projecting last frame's texture with this
			// frame's camera is a reflection that slides as the viewer moves,
			// and it reads as a mis-aimed camera rather than as a stale matrix.
			//@{
			glm::mat4 PreviousViewProjection{1.0f};
			glm::mat4 PreviousSampling{1.0f};
			//@}

			// How solid this surface's image is, from the view that wrote it.
			float ImageOpacity = 1.0f;

			// What the pane puts the image through when it samples it.
			//
			// **Not part of the signature**, unlike the matrix beside it: the
			// effect changes no texel of the texture, only how the screen pass
			// reads it. A mirror switched to thermal has to change this frame
			// rather than on whichever later frame something happens to move -
			// the same argument `ImageOpacity` makes one line up.
			scene::SurfaceEffect Effect = scene::SurfaceEffect::None;

			// What the scene looked like when this surface last rendered.
			//
			// **Compared rather than trusted to a dirty bit**, for
			// `SignatureOf`'s reason: the draw list is written in bulk and
			// announces nothing. A match means this pass would redraw the
			// texture it already holds, so it is not run.
			//
			// Meaningless while `Ready` is false - a slot that has never
			// rendered refreshes on the signature it happens to hold, which is
			// why the two are always tested together.
			uint64_t Signature = 0;
		};

		// Every surface a viewport owns.
		//
		// **Indexed by surface number, and never released short of shutdown.**
		// A slot is allocated the first time an index is rendered and then kept,
		// which is deliberate rather than lax: the studio round-robins its
		// viewports, so one frame draws a world full of mirrors and the next
		// draws one with none. Releasing on absence would destroy and recreate
		// every surface texture on alternate frames, which is the same
		// reallocation `SCENE_TARGET_BLOCK` exists to avoid one layer up. The
		// high-water mark is bounded by `scene::MAX_SURFACES`.
		// One level of one portal's recursion: the picture seen through that hole
		// from the camera the level above stands at.
		//
		// **No ping-pong pair, unlike a surface slot, and the difference is what
		// makes the pass a recursion rather than an iteration.** A surface reads
		// its neighbours' textures while they read its, so a pair is the only way
		// to stop a pass sampling what another pass is writing in the same
		// bounce. A portal level is written by the recursion and read exactly
		// once, by the level above it, after that write has finished - depth-first
		// order is the ordering, so there is nothing to alternate between.
		//
		// **No signature, no rate cap and no `Ready` either.** All three are ways
		// of keeping a texture across frames, and a level's contents are only
		// meaningful for the camera that produced them - which is this frame's.
		struct PortalTarget {
			SDL_GPUTexture *Colour = nullptr;
			SDL_GPUTexture *Display = nullptr;
			SDL_GPUTexture *Depth = nullptr;
			uint32_t Width = 0;
			uint32_t Height = 0;
		};

		// The pool, indexed by level and then by slot.
		//
		// **Per level per slot, and both indices are needed.** Per level alone
		// would be enough if a level were consumed the instant it was written -
		// which it is not: level `L` renders the scene and *then* draws every
		// hole visible from it, so all of level `L-1` has to survive until the
		// last of them is drawn. Per slot alone would have two levels of one hole
		// writing the same texture.
		//
		// Allocated on the frame a level is first reached and kept after, for
		// `SurfaceBank`'s reason: a viewer who walks away from a corridor of
		// holes and back again should not pay two full-screen allocations for it.
		struct PortalLevel {
			PortalTarget Targets[scene::MAX_SURFACES];
		};

		// One level of one *mirror's* recursion: the pane's picture as seen from
		// the camera the level above stands at.
		//
		// **A pane-shaped `PortalTarget`, and the two are separate for the reason
		// their passes are.** A hole's sub-render is the screen's own frustum
		// skewed, so its target is the screen's size and the pane reads the texel
		// it is standing on. A mirror's is fitted to its pane, so its target is
		// the pane's authored size and the pane reads it by projecting its own
		// world position - which is why this carries a matrix and a portal level
		// does not.
		//
		// **`Sampling` is the whole point of the type.** It is the matrix that
		// *rendered* this texture, and the level above binds it to project the
		// pane with. Before the recursion existed the pass had only the matrix
		// fitted to the eye available to it - so a pane inside another pane's
		// picture was projected from a viewpoint nobody was looking from, the
		// coordinate left 0..1, and `opaque.frag` fell back to the flat lit pane.
		// That flat slab in a mirror's reflection is the defect this exists to
		// remove.
		//
		// **No ping-pong pair and no signature**, for `PortalTarget`'s reasons: a
		// level is written by the recursion and read exactly once, by the level
		// above it, after that write has finished. Depth-first order *is* the
		// ordering, so there is nothing to alternate between and nothing worth
		// keeping across a frame - the contents are only meaningful for the
		// camera that produced them, which is this frame's.
		struct MirrorTarget {
			SDL_GPUTexture *Colour = nullptr;
			SDL_GPUTexture *Depth = nullptr;
			uint32_t Width = 0;
			uint32_t Height = 0;

			// World to this level's camera clip space. What the pane above
			// projects its own world position through.
			//
			// A mirror's map is the identity - a reflection fixes every point of
			// the plane it reflects through - so this is the camera's
			// `ViewProjection` and nothing else. `scene::SurfaceLens::Mapping`
			// carries the general case for the pane path.
			glm::mat4 Sampling{1.0f};

			// Whether this frame's recursion reached this slot at this level.
			//
			// **Cleared at the top of every frame rather than trusted.** A pane
			// that went off screen, or edge-on, or whose texture could not be
			// made, leaves a target holding last frame's picture taken from a
			// camera that no longer exists - and sampling it would slide a
			// reflection across a pane nothing in the scene is moving, which is
			// the hardest possible version of this bug to attribute.
			bool Ready = false;
		};

		struct MirrorLevel {
			MirrorTarget Targets[scene::MAX_SURFACES];
		};

		// One portal mouth's light-field capture: the room its seam opens onto,
		// rendered against a lit void from a stand-in eye at the mouth.
		//
		// **The seam's geometry travels with the texture**, because the capture
		// and the projection are two passes reading one record - a projector fed
		// a rectangle the capture was not taken at throws another room's light
		// onto this one's floor, at an angle nothing authored.
		//
		// `Ready` is cleared at the top of every portal pass rather than
		// trusted, for `MirrorTarget::Ready`'s reason: a mouth that was disabled
		// or walked away from must not go on projecting last frame's rooms.
		struct SeamLightTarget {
			SDL_GPUTexture *Colour = nullptr;
			SDL_GPUTexture *Depth = nullptr;
			uint32_t Width = 0;
			uint32_t Height = 0;
			bool Ready = false;

			// The projector, in world space: xyz centre / w unused, xyz unit
			// normal toward the lit room / w spill range, and the two half axes.
			glm::vec4 Centre{};
			glm::vec4 Outward{};
			glm::vec4 First{};
			glm::vec4 Second{};
		};

		struct SurfaceBank {
			SurfaceSlotState Surfaces[scene::MAX_SURFACES];

			// **Grown to the depth actually reached**, so a world with no holes
			// in it costs one empty vector per viewport and nothing else.
			std::vector<PortalLevel> Portals;

			// The same, for mirrors. Two pools rather than one, because the two
			// passes size their targets differently - see `MirrorTarget`.
			std::vector<MirrorLevel> Mirrors;

			// The seam light-field captures, one per mouth slot. Fixed at
			// `SEAM_LIGHT_RESOLUTION` rather than pooled by level: a mouth
			// captures its far room once per frame however deep the picture
			// recursion goes.
			SeamLightTarget SeamLights[scene::MAX_SURFACES];

			// What the last frame drawn into this bank reached, which is what an
			// automatic depth reads.
			//
			// **Per viewport and not per renderer, for the reason the textures
			// are.** The studio's four panels look at one world from four
			// places, and how deep a corridor telescopes is a fact about where
			// somebody is standing - one shared number would let the panel with
			// the deepest view pay for every other panel's frame, and would
			// oscillate as the panels took turns.
			scene::SurfaceBounceProbe Bounces;
		};

		// **One bank per viewport, and that is what makes a mirror a mirror when
		// a world is on screen twice.** A reflection is of the viewer: `scene::
		// AimSurfaceCameras` mirrors the world's `ActiveCamera` through each
		// pane, so two panels looking at one world want two different images out
		// of the same `SurfaceCamera`. With one shared bank they got one - the
		// panel that drew most recently wrote every surface texture, and the
		// other panel then composited its panes from a reflection computed for
		// somebody else's eye. Flying either camera moved the mirrors in both
		// windows, at half the frame rate, which reads as a projection fault
		// rather than as one texture with two authors.
		//
		// The aim is still world state and still per frame - one panel draws per
		// frame and re-aims before it does - so what has to be per viewport is
		// the *texture*, which outlives the frame that drew it. A panel that is
		// not this frame's shows its own last image rather than another panel's
		// current one.
		//
		// **Grown on demand rather than sized to a maximum.** A client has one
		// viewport and a scene with no mirrors has no banks at all; the studio's
		// four allocate as their panels first draw a surface, and each costs a
		// texture pair per live surface index and nothing per unused one.
		std::vector<SurfaceBank> SurfaceBanks;

		SurfaceBank &SurfacesAt(size_t viewport) {
			if (SurfaceBanks.size() <= viewport) {
				SurfaceBanks.resize(viewport + 1);
			}
			return SurfaceBanks[viewport];
		}

		SDL_GPUSampler *SurfaceSampler = nullptr;

		bool EnsureShadow();

		// The beam atlas, made on the frame a hole first transports a shadow.
		//
		// **Shares `ShadowSampler`**, because it is the same kind of map read the
		// same way - a depth texture clamped at the edge, with the fragment
		// shader range-checking anyway.
		bool EnsureBeams();

		// The one sampler every offscreen scene texture is read through.
		//
		// **Its own call because two passes need it and only one of them used to
		// create it.** It was made inside `EnsureSurface`, which a world of
		// nothing but portals never reaches - so the portal pass captured a null
		// sampler and handed it to `SDL_BindGPUFragmentSamplers`, which
		// dereferences it. A scene with a single mirror in it hid that
		// completely.
		//
		// Linear and clamped: the clamp is what stops a fragment at the very edge
		// of a pane wrapping to the far side of the picture.
		bool EnsureSurfaceSampler();

		bool EnsureSurface(size_t viewport, uint8_t index, uint32_t width, uint32_t height);

		// One portal level's colour and depth, at the size of the attachment the
		// level above draws into.
		//
		// **The attachment's size and not the viewport's**, which is what makes
		// `opaque.frag`'s screen-position lookup exact: the pane divides its own
		// `gl_FragCoord` by `textureSize`, so the texel it reads is the pixel it
		// is standing on only while the two rectangles are the same. A level
		// rendered at anything else would need a scale factor pushed to the
		// shader, which is a uniform that can be wrong.
		//
		// @return `false` when either texture could not be made, which drops that
		//         hole to a flat pane for the frame rather than the frame.
		PortalTarget *
		EnsurePortal(size_t viewport, uint32_t level, uint8_t index, uint32_t width, uint32_t height);

		// One mirror level's colour and depth, at the pane's own size.
		//
		// **The pane's size and not the viewport's**, which is the one line of
		// difference from `EnsurePortal` and follows from how the two are read.
		// A hole's picture is sampled by screen position, so it has to be the
		// screen's shape; a mirror's is sampled by projecting the pane's world
		// position through the matrix that drew it, so it can be any size at all
		// and the authored one is what the scene asked for.
		//
		// @return `null` when either texture could not be made, which drops that
		//         level to a flat pane for the frame rather than the frame.
		MirrorTarget *
		EnsureMirror(size_t viewport, uint32_t level, uint8_t index, uint32_t width, uint32_t height);

		// One mouth's light-field capture pair, at `SEAM_LIGHT_RESOLUTION`.
		//
		// @return `null` when either texture could not be made, which loses the
		//         mouth's spill for the frame rather than the frame.
		SeamLightTarget *EnsureSeamLight(size_t viewport, uint8_t index);

		// One opaque white texel, bound wherever a real texture is missing.
		//
		// **The pipelines declare two fragment samplers and a draw must bind
		// both.** An unbound sampler is undefined behaviour on several backends
		// where a wrongly bound one is merely ignored, and the uniform flag is
		// what stops the result being read - so any valid texture will do, and
		// what matters is that there is always one.
		//
		// This used to be `OverlayTexture`, which is created only when a debug
		// panel has something in it, standing in for `ShadowTexture`, which is
		// created only when something casts. Both are absent together in an
		// ordinary case - a scene of nothing but transparent geometry, with the
		// panels closed - and the screen pass then bound no samplers at all and
		// drew anyway. Owning a texture for the job costs four bytes of device
		// memory and removes the case rather than making it rarer.
		SDL_GPUTexture *FallbackTexture = nullptr;

		SDL_GPUTexture *OverlayTexture = nullptr;
		SDL_GPUTransferBuffer *OverlayTransfer = nullptr;
		SDL_GPUSampler *OverlaySampler = nullptr;
		int OverlayWidth = 0;
		int OverlayHeight = 0;

		// Set when the overlay texture is created and cleared by the first
		// upload after it, which is made to cover the whole image rather than
		// only the region that changed.
		bool OverlayUninitialised = false;

		std::string Backend;

		// What this device takes, asked once at initialisation. Every shader
		// created below reads it rather than naming a format of its own.
		ShaderBinary Binary;

		SDL_GPUShader *LoadShader(
			std::string_view name, SDL_GPUShaderStage stage, uint32_t samplers, uint32_t uniformBuffers
		) const;

		// A built-in compute pipeline from the same staged directory
		// `LoadShader` reads. The counts are the shader's declared bindings in
		// SDL's order; the thread counts must match its `local_size`.
		SDL_GPUComputePipeline *LoadComputePipeline(
			std::string_view name,
			uint32_t samplers,
			uint32_t readStorageBuffers,
			uint32_t writeStorageTextures,
			uint32_t writeStorageBuffers,
			uint32_t threadsX,
			uint32_t threadsY
		) const;

		bool CreatePipelines();

		// Binds a pipeline and records which family it belongs to.
		//
		// **Every opaque and transparent bind goes through this**, because
		// `DrawSlots` may substitute a variant for a run and has to know what to
		// put back. A pass that called `SDL_BindGPUGraphicsPipeline` directly
		// would leave the record saying something false, and the next run would
		// restore the wrong pipeline - a draw with somebody else's blend state,
		// which reads as a sorting bug.
		void BindPipeline(SDL_GPURenderPass *pass, SDL_GPUGraphicsPipeline *pipeline, PipelineFamily family);

		// Builds the two pipelines a named fragment shader draws through.
		//
		// Replaces whatever was registered under the name, releasing it first.
		//
		// @param name  What a material names it.
		// @param spirv The module. Must declare the sampler and uniform slots
		//              `opaque.frag` does - see `Renderer::AddShader`.
		// @return `false` when the shader or either pipeline could not be built.
		bool AddShaderVariant(const core::Name &name, std::span<const uint32_t> spirv);

		// Releases one variant's shader and pipelines.
		void DropShaderVariant(const core::Name &name);

		// Releases every variant. Called from `Shutdown`.
		void ReleaseShaderVariants();

		// The variant a slot's shader means in the family currently bound.
		//
		// @return The pipeline, or null for no shader, an unknown one, or a
		//         family with no variants.
		SDL_GPUGraphicsPipeline *VariantFor(const core::Name &shader) const;

		// Issues the draws for one contiguous run of instance-buffer slots.
		//
		// **The loop v0.9 exists to add.** Every draw used to be one call over
		// one cube; now a run may hold several meshes and each mesh several
		// material submeshes, so this splits the run wherever the mesh or the
		// instance's texture override changes and issues a call per resulting
		// piece. The instances themselves never move - the split is entirely in
		// `first_instance` and a count.
		//
		// **Consecutive-run splitting rather than grouping.** Sorting the run by
		// mesh would produce fewer draw calls and is exactly what the blended
		// pass may not have: that order is back-to-front from the eye and
		// reordering it is the transparency bug the sort exists to prevent. One
		// rule for both passes is worth more here than a draw call.
		//
		// @param command    The frame's command buffer, for the uniform pushes.
		// @param pass       The open render pass.
		// @param first      The first slot.
		// @param count      How many slots.
		// @param lighting   The pass's uniforms, or null for a depth-only pass
		//                   that binds no samplers and pushes nothing.
		// @param shadow     The shadow map to bind, or null.
		// @param shadowSampler   Its sampler.
		// @param surface    The surface texture to bind, or null.
		// @param surfaceSampler  Its sampler.
		// @param triangles  Incremented by what was actually drawn.
		// @return How many draw calls were issued.
		uint32_t DrawSlots(
			SDL_GPUCommandBuffer *command,
			SDL_GPURenderPass *pass,
			uint32_t first,
			uint32_t count,
			const LightingUniforms *lighting,
			SDL_GPUTexture *shadow,
			SDL_GPUSampler *shadowSampler,
			SDL_GPUTexture *surface,
			SDL_GPUSampler *surfaceSampler,
			uint32_t tagFilter,
			uint64_t &triangles,
			const IndirectPhase *indirect = nullptr
		);

		bool CreateGeometry();
		bool EnsureInstanceCapacity(uint32_t count);
		bool EnsureDepth(uint32_t width, uint32_t height);

		// The same, into whichever depth texture the caller owns. See
		// `SceneSlot::Depth` for why a viewport keeps its own.
		bool EnsureDepthIn(
			SDL_GPUTexture *&texture,
			uint32_t &haveWidth,
			uint32_t &haveHeight,
			uint32_t width,
			uint32_t height
		);
		bool EnsureScene(uint32_t width, uint32_t height);
		bool EnsureHistory(size_t slot, uint32_t width, uint32_t height);

		// Whether this renderer has a window at all.
		//
		// **Headless is a device with nothing claimed**, not a hidden window. A
		// hidden window still owns a swapchain, and whether one can be acquired
		// for a window nobody can see is a per-platform answer nobody should
		// have to know. With no window there is no swapchain and no question.
		bool Headless() const {
			return Window == nullptr;
		}

		// The colour format every pipeline and the scene target are built
		// against.
		//
		// **One answer, asked in five places.** Headless has no swapchain to
		// ask, so it takes a fixed format - and the format has to be the *same*
		// fixed one everywhere, or a pipeline is built for one target and bound
		// to another. That is the whole reason this is a function rather than a
		// call to SDL at each use.
		SDL_GPUTextureFormat ColourFormat() const {
			return Headless() ? SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM
							  : SDL_GetGPUSwapchainTextureFormat(Device, Window);
		}
		bool EnsureOverlay(int width, int height);
	};

	SDL_GPUShader *Renderer::Impl::LoadShader(
		std::string_view name, SDL_GPUShaderStage stage, uint32_t samplers, uint32_t uniformBuffers
	) const {
		// Staged under the owning module's name, so that two modules cannot
		// collide on a common file name like fullscreen.vert. The built-in GLSL
		// is `Engine::resources`, which is what that name is.
		//
		// The form comes from the device: the build stages a `.spv` and a `.msl`
		// for every shader, and which of them SDL will accept is what
		// `SDL_GetGPUShaderFormats` answered.
		const auto path = resources::Shader(name, Binary.Form);

		const auto code = ReadFile(path);
		if (code.empty()) {
			ENGINE_ERROR("shader not found or empty: {}", path.string());
			return nullptr;
		}

		SDL_GPUShaderCreateInfo info{};
		info.code = code.data();
		info.code_size = code.size();
		info.entrypoint = Binary.EntryPoint;
		info.format = Binary.Format;
		info.stage = stage;
		info.num_samplers = samplers;
		info.num_uniform_buffers = uniformBuffers;

		SDL_GPUShader *shader = SDL_CreateGPUShader(Device, &info);
		if (!shader) {
			ENGINE_ERROR("SDL_CreateGPUShader failed for {}: {}", name, SDL_GetError());
		}
		return shader;
	}

	SDL_GPUComputePipeline *Renderer::Impl::LoadComputePipeline(
		std::string_view name,
		uint32_t samplers,
		uint32_t readStorageBuffers,
		uint32_t writeStorageTextures,
		uint32_t writeStorageBuffers,
		uint32_t threadsX,
		uint32_t threadsY
	) const {
		const auto path = resources::Shader(name, Binary.Form);
		const auto code = ReadFile(path);
		if (code.empty()) {
			ENGINE_ERROR("shader not found or empty: {}", path.string());
			return nullptr;
		}

		SDL_GPUComputePipelineCreateInfo info{};
		info.code = code.data();
		info.code_size = code.size();
		info.entrypoint = Binary.EntryPoint;
		info.format = Binary.Format;
		info.num_samplers = samplers;
		info.num_readonly_storage_buffers = readStorageBuffers;
		info.num_readwrite_storage_textures = writeStorageTextures;
		info.num_readwrite_storage_buffers = writeStorageBuffers;
		info.num_uniform_buffers = 1;
		info.threadcount_x = threadsX;
		info.threadcount_y = threadsY;
		info.threadcount_z = 1;

		SDL_GPUComputePipeline *pipeline = SDL_CreateGPUComputePipeline(Device, &info);
		if (!pipeline) {
			ENGINE_ERROR("SDL_CreateGPUComputePipeline failed for {}: {}", name, SDL_GetError());
		}
		return pipeline;
	}

	bool Renderer::Impl::CreatePipelines() {
		SDL_GPUShader *opaqueVertex = LoadShader("opaque.vert", SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);

		// **Two samplers now: the shadow map and the surface.** The count is
		// part of the shader object rather than of the pipeline, so a mismatch
		// with the `layout(set = 2, binding = n)` declarations is a bind that
		// silently reads nothing rather than a validation error.
		SDL_GPUShader *opaqueFragment = LoadShader(
			// **Two uniform buffers now, not one.** The count is part of the
			// shader object rather than of the pipeline, so a mismatch with the
			// `layout(set = 3, binding = n)` declarations is a push that lands
			// nowhere rather than a validation error - the same trap the sampler
			// count above records.
			"opaque.frag",
			SDL_GPU_SHADERSTAGE_FRAGMENT,
			9,
			3
		);

		SDL_GPUShader *shadowVertex = LoadShader("shadow.vert", SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
		SDL_GPUShader *shadowFragment = LoadShader("shadow.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 1);
		SDL_GPUShader *overlayVertex = LoadShader("overlay.vert", SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
		SDL_GPUShader *imageFragment = LoadShader("image.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
		SDL_GPUShader *overlayFragment = LoadShader("overlay.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 0);
		SDL_GPUShader *gbufferFragment = LoadShader("gbuffer.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 9, 1);
		SDL_GPUShader *depthLinearFragment =
			LoadShader("depth-linearise.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
		SDL_GPUShader *ssaoFragment = LoadShader("ssao.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 2, 1);
		// Nine samplers: the seven G-buffer and shadow inputs plus the two seam
		// light-field captures - `MAX_SEAM_LIGHTS`, bound last.
		SDL_GPUShader *deferredLightingFragment =
			LoadShader("deferred-lighting.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 9, 2);
		SDL_GPUShader *tonemapFragment = LoadShader("tonemap.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 0);

		if (!opaqueVertex || !opaqueFragment || !shadowVertex || !shadowFragment || !overlayVertex ||
			!imageFragment || !overlayFragment || !gbufferFragment || !depthLinearFragment || !ssaoFragment ||
			!deferredLightingFragment || !tonemapFragment) {
			return false;
		}

		const SDL_GPUTextureFormat swapchainFormat = ColourFormat();

		// --- opaque ---------------------------------------------------------

		const SDL_GPUVertexBufferDescription vertexBuffers[] = {
			{0, sizeof(Vertex), SDL_GPU_VERTEXINPUTRATE_VERTEX, 0},
			// One step per instance: the same 36 indices are replayed for every
			// entity, and only the matrix and colour change.
			{1, sizeof(GpuInstance), SDL_GPU_VERTEXINPUTRATE_INSTANCE, 0},
		};

		const SDL_GPUVertexAttribute attributes[] = {
			{0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(Vertex, Position)},
			{1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(Vertex, Normal)},
			{2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(Vertex, TexCoord)},
			// A mat4 attribute is four float4 locations; there is no matrix
			// vertex format.
			{3, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 0},
			{4, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, sizeof(float) * 4},
			{5, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, sizeof(float) * 8},
			{6, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, sizeof(float) * 12},
			{7, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, offsetof(GpuInstance, Colour)},
			{8, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, offsetof(GpuInstance, InverseScaleSquared)},
		};

		SDL_GPUColorTargetDescription opaqueTarget{};
		opaqueTarget.format = swapchainFormat;

		SDL_GPUGraphicsPipelineCreateInfo opaque{};
		opaque.vertex_shader = opaqueVertex;
		opaque.fragment_shader = opaqueFragment;
		opaque.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
		opaque.vertex_input_state.vertex_buffer_descriptions = vertexBuffers;
		opaque.vertex_input_state.num_vertex_buffers = 2;
		opaque.vertex_input_state.vertex_attributes = attributes;
		opaque.vertex_input_state.num_vertex_attributes = 9;
		opaque.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
		opaque.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_BACK;
		// The cube winds counter-clockwise when seen from outside.
		opaque.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;

		// **Clip against the near plane rather than clamping to it, which a
		// portal depends on and everything else quietly wanted.** SDL zeroes
		// this field, and false means *clamp*: a fragment in front of the near
		// plane is not discarded, it is pushed to depth zero and drawn. For an
		// ordinary camera that is a wall you have walked into filling the screen
		// instead of vanishing, which reads as a bug nobody files.
		//
		// For a portal it is the whole feature failing. The oblique clip works
		// by skewing the near plane onto the destination's pane, so *everything
		// the hole should not show is behind the near plane* - the wall the
		// destination is set into, its back face, and the room behind it.
		// Clamped, all of it draws at depth zero and fills the hole with the
		// wall it leads through. The matrix was right, the placement was right,
		// and the picture was a flat grey wall. See `scene::SurfaceLens`.
		opaque.rasterizer_state.enable_depth_clip = true;
		opaque.depth_stencil_state.enable_depth_test = true;
		opaque.depth_stencil_state.enable_depth_write = true;
		opaque.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS;
		opaque.target_info.color_target_descriptions = &opaqueTarget;
		opaque.target_info.num_color_targets = 1;
		opaque.target_info.depth_stencil_format = DepthFormat;
		opaque.target_info.has_depth_stencil_target = true;

		OpaquePipeline = SDL_CreateGPUGraphicsPipeline(Device, &opaque);
		if (!OpaquePipeline) {
			ENGINE_ERROR("opaque pipeline: {}", SDL_GetError());
		}

		// --- wireframe view mode ----------------------------------------------
		//
		// **A second pipeline rather than a bindable state, because SDL_GPU has
		// no bindable state for it.** `rasterizer_state.fill_mode` is baked into
		// the pipeline object at creation, the same way `cull_mode` is for
		// `TransparentPipeline` above - there is no `SDL_SetGPU...FillMode` to
		// call between draws. So a toggle here means a second object with
		// everything else unchanged, exactly the shape `TransparentPipeline`
		// already is relative to `OpaquePipeline`.
		//
		// **Culling off, so a wireframe view shows every edge and not only the
		// ones facing the eye.** A wireframe box with back-face culling on
		// draws as three visible faces and reads as broken geometry; a
		// developer reaching for this view wants the far side of the mesh
		// too.
		//
		// **Bound in `BindPipeline`, not at each of the dozens of call sites
		// that ask for `OpaquePipeline` or `TransparentPipeline`.** Every one
		// of them already funnels through that one function to keep
		// `ActivePipeline` and `ActiveFamily` correct for `DrawSlots`'
		// restore - see its own header - so a family-keyed substitution there
		// reaches the screen pass, the surface pass and every mirror without
		// a second line anywhere else. A part with its own `ShaderScript`
		// keeps its own shader even so: `DrawSlots` binds a variant by name
		// over whatever `BindPipeline` left active, and a debug view is not
		// the place to override an author's own material.
		//
		// **Failure here is a diagnostic and a feature quietly unavailable,
		// never a reason `CreatePipelines` itself fails.** `fillModeNonSolid`
		// is an optional device feature on some backends; a machine without it
		// still renders every ordinary frame correctly and only loses a debug
		// toggle, which is not worth the whole renderer refusing to start
		// over.
		SDL_GPUGraphicsPipelineCreateInfo wireframeOpaque = opaque;
		wireframeOpaque.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_LINE;
		wireframeOpaque.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
		WireframeOpaquePipeline = SDL_CreateGPUGraphicsPipeline(Device, &wireframeOpaque);
		if (!WireframeOpaquePipeline) {
			ENGINE_WARN("wireframe opaque pipeline unavailable: {}", SDL_GetError());
		}

		// --- default PBR graph -----------------------------------------------

		SDL_GPUColorTargetDescription gbufferTargets[4]{};
		gbufferTargets[0].format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB;
		gbufferTargets[1].format = SDL_GPU_TEXTUREFORMAT_R10G10B10A2_UNORM;
		gbufferTargets[2].format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
		gbufferTargets[3].format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;

		SDL_GPUGraphicsPipelineCreateInfo gbuffer = opaque;
		gbuffer.fragment_shader = gbufferFragment;
		gbuffer.target_info.color_target_descriptions = gbufferTargets;
		gbuffer.target_info.num_color_targets = 4;
		GBufferPipeline = SDL_CreateGPUGraphicsPipeline(Device, &gbuffer);
		if (GBufferPipeline == nullptr) {
			ENGINE_ERROR("gbuffer pipeline: {}", SDL_GetError());
		}

		const auto fullscreen = [&](SDL_GPUShader *fragment, SDL_GPUTextureFormat format) {
			SDL_GPUColorTargetDescription target{};
			target.format = format;

			SDL_GPUGraphicsPipelineCreateInfo info{};
			info.vertex_shader = overlayVertex;
			info.fragment_shader = fragment;
			info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
			info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
			info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
			info.target_info.color_target_descriptions = &target;
			info.target_info.num_color_targets = 1;
			return SDL_CreateGPUGraphicsPipeline(Device, &info);
		};

		DepthLinearPipeline = fullscreen(depthLinearFragment, SDL_GPU_TEXTUREFORMAT_R32_FLOAT);
		SsaoPipeline = fullscreen(ssaoFragment, SDL_GPU_TEXTUREFORMAT_R8_UNORM);
		DeferredLightingPipeline =
			fullscreen(deferredLightingFragment, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT);
		TonemapPipeline = fullscreen(tonemapFragment, swapchainFormat);
		if (DepthLinearPipeline == nullptr || SsaoPipeline == nullptr ||
			DeferredLightingPipeline == nullptr || TonemapPipeline == nullptr) {
			ENGINE_ERROR("default PBR fullscreen pipeline: {}", SDL_GetError());
		}

		// --- shadow ---------------------------------------------------------
		//
		// The opaque pipeline with **no colour target at all** and the other
		// face culled. Front-face culling is the classic trick: rendering back
		// faces into the map moves the recorded depth to the far side of each
		// object, which pushes self-shadowing acne behind the surface that would
		// have shown it. It costs a little peter-panning on thin geometry, which
		// the slope-scaled bias in the fragment shader is sized against.
		SDL_GPUGraphicsPipelineCreateInfo shadow = opaque;
		shadow.vertex_shader = shadowVertex;
		shadow.fragment_shader = shadowFragment;
		shadow.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_FRONT;
		shadow.target_info.color_target_descriptions = nullptr;
		shadow.target_info.num_color_targets = 0;

		ShadowPipeline = SDL_CreateGPUGraphicsPipeline(Device, &shadow);
		if (!ShadowPipeline) {
			ENGINE_ERROR("shadow pipeline: {}", SDL_GetError());
		}

		// --- transparent ----------------------------------------------------
		//
		// The opaque pipeline with two changes, and each one is the whole
		// reason a second pipeline exists:
		//
		// - **Blending on**, source alpha over one-minus-source-alpha. Not
		//   premultiplied, unlike the overlay below: these instances carry a
		//   straight `Color3` from `Visual::Tint`, and premultiplying it in the
		//   producer would make the same colour mean two things depending on
		//   which pass read it.
		// - **Depth writes off, depth *test* on.** A transparent pane must be
		//   hidden by an opaque wall in front of it, so the test stays; but it
		//   must not stop the pane behind it from being drawn, so the write
		//   goes. Leaving the write on is the classic version of this bug - the
		//   nearest pane silently erases everything behind it and the scene
		//   looks like the sort failed.
		SDL_GPUColorTargetDescription blendedTarget{};
		blendedTarget.format = swapchainFormat;
		blendedTarget.blend_state.enable_blend = true;
		blendedTarget.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
		blendedTarget.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
		blendedTarget.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
		blendedTarget.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
		blendedTarget.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
		blendedTarget.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;

		SDL_GPUGraphicsPipelineCreateInfo transparent = opaque;
		transparent.depth_stencil_state.enable_depth_write = false;
		transparent.target_info.color_target_descriptions = &blendedTarget;

		// **Back faces are drawn too.** A cube with see-through walls shows its
		// own far side, and culling it leaves a shape that reads as hollow
		// rather than as glass.
		transparent.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;

		TransparentPipeline = SDL_CreateGPUGraphicsPipeline(Device, &transparent);
		if (!TransparentPipeline) {
			ENGINE_ERROR("transparent pipeline: {}", SDL_GetError());
		}

		// The wireframe twin, for `WireframeOpaquePipeline`'s own reason.
		// Culling is already off on `transparent`, so only the fill mode
		// changes.
		SDL_GPUGraphicsPipelineCreateInfo wireframeTransparent = transparent;
		wireframeTransparent.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_LINE;
		WireframeTransparentPipeline = SDL_CreateGPUGraphicsPipeline(Device, &wireframeTransparent);
		if (!WireframeTransparentPipeline) {
			ENGINE_WARN("wireframe transparent pipeline unavailable: {}", SDL_GetError());
		}

		// --- what a variant is derived from ---------------------------------
		//
		// The two descriptors above, copied whole with their arrays, so a shader
		// arriving later builds a pipeline that differs from these in exactly
		// one field. Copied rather than rebuilt: a second description of the
		// vertex layout is a second thing to keep in step, and the one that
		// would be missed is this one - it is only read when somebody selects a
		// custom shader.
		std::memcpy(VariantBuffers, vertexBuffers, sizeof(VariantBuffers));
		std::memcpy(VariantAttributes, attributes, sizeof(VariantAttributes));
		VariantOpaqueTarget = opaqueTarget;
		VariantBlendedTarget = blendedTarget;

		VariantOpaqueInfo = opaque;
		VariantOpaqueInfo.vertex_input_state.vertex_buffer_descriptions = VariantBuffers;
		VariantOpaqueInfo.vertex_input_state.vertex_attributes = VariantAttributes;
		VariantOpaqueInfo.target_info.color_target_descriptions = &VariantOpaqueTarget;

		VariantBlendedInfo = transparent;
		VariantBlendedInfo.vertex_input_state.vertex_buffer_descriptions = VariantBuffers;
		VariantBlendedInfo.vertex_input_state.vertex_attributes = VariantAttributes;
		VariantBlendedInfo.target_info.color_target_descriptions = &VariantBlendedTarget;

		// The vertex stage is kept rather than released with the others below,
		// because a variant built after this function has run needs it.
		OpaqueVertexShader = opaqueVertex;
		VariantsReady = true;

		// --- particles ------------------------------------------------------
		//
		// **No vertex buffer for the quad and no index buffer at all.** The
		// billboard's four corners come out of `gl_VertexIndex` in the shader, and
		// the primitive is a triangle strip - so one particle is one instance of
		// four vertices with nothing fetched. At half a million particles that is
		// half a million cache lines never read.
		//
		// Slot 0 is the per-instance particle, which is why the descriptions below
		// name one buffer where the opaque pipeline names two.

		SDL_GPUShader *particleVertex = LoadShader("particle.vert", SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
		SDL_GPUShader *particleFragment = LoadShader("particle.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);

		if (particleVertex != nullptr && particleFragment != nullptr) {
			const SDL_GPUVertexBufferDescription particleBuffers[] = {
				{0, sizeof(effects::ParticleInstance), SDL_GPU_VERTEXINPUTRATE_INSTANCE, 0},
			};

			// **Three of the four are unsigned integers rather than floats**,
			// because that is what they are: a packed size, a packed rotation and
			// cell, and an RGBA8 colour. Declaring them as floats would make the
			// driver convert bits that are not a float, which is not a slow path -
			// it is a different number.
			const SDL_GPUVertexAttribute particleAttributes[] = {
				{0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(effects::ParticleInstance, Position)},
				{1, 0, SDL_GPU_VERTEXELEMENTFORMAT_UINT, offsetof(effects::ParticleInstance, Size)},
				{2,
				 0,
				 SDL_GPU_VERTEXELEMENTFORMAT_UINT,
				 offsetof(effects::ParticleInstance, RotationAndCell)},
				{3, 0, SDL_GPU_VERTEXELEMENTFORMAT_UINT, offsetof(effects::ParticleInstance, Colour)},
				{4, 0, SDL_GPU_VERTEXELEMENTFORMAT_UINT, offsetof(effects::ParticleInstance, Slot)},
			};

			SDL_GPUColorTargetDescription particleTarget{};
			particleTarget.format = swapchainFormat;
			particleTarget.blend_state.enable_blend = true;
			particleTarget.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
			particleTarget.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
			particleTarget.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
			particleTarget.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
			particleTarget.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
			particleTarget.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;

			SDL_GPUGraphicsPipelineCreateInfo particle{};
			particle.vertex_shader = particleVertex;
			particle.fragment_shader = particleFragment;
			particle.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP;
			particle.vertex_input_state.vertex_buffer_descriptions = particleBuffers;
			particle.vertex_input_state.num_vertex_buffers = 1;
			particle.vertex_input_state.vertex_attributes = particleAttributes;
			particle.vertex_input_state.num_vertex_attributes = 5;
			particle.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;

			// **Nothing is culled.** A billboard is a flat quad turned to face the
			// eye, so which way it winds depends on where the camera is - and a
			// cull mode would make half the particles in a scene vanish depending
			// on which side of the emitter you stood.
			particle.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;

			// **Depth tested and not depth written**, which is the same pair the
			// transparent pipeline has and for the same reason: a particle must be
			// hidden by the wall in front of it, and must not hide the particle
			// behind it.
			particle.depth_stencil_state.enable_depth_test = true;
			particle.depth_stencil_state.enable_depth_write = false;
			particle.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS;
			particle.target_info.color_target_descriptions = &particleTarget;
			particle.target_info.num_color_targets = 1;
			particle.target_info.depth_stencil_format = DepthFormat;
			particle.target_info.has_depth_stencil_target = true;

			ParticlePipeline = SDL_CreateGPUGraphicsPipeline(Device, &particle);
			if (ParticlePipeline == nullptr) {
				ENGINE_ERROR("particle pipeline: {}", SDL_GetError());
			}

			// The additive twin. **One destination factor apart**, and that one
			// factor is what makes the blend commutative and therefore
			// order-independent - which is why an additive emitter needs no sort.
			SDL_GPUColorTargetDescription additiveTarget = particleTarget;
			additiveTarget.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
			additiveTarget.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;

			SDL_GPUGraphicsPipelineCreateInfo additive = particle;
			additive.target_info.color_target_descriptions = &additiveTarget;

			AdditiveParticlePipeline = SDL_CreateGPUGraphicsPipeline(Device, &additive);
			if (AdditiveParticlePipeline == nullptr) {
				ENGINE_ERROR("additive particle pipeline: {}", SDL_GetError());
			}

			SDL_ReleaseGPUShader(Device, particleVertex);
			SDL_ReleaseGPUShader(Device, particleFragment);
		}

		// --- ribbons --------------------------------------------------------
		//
		// A triangle strip over a real vertex buffer, where the particle pass has
		// no buffer at all. `ribbon.vert` says why the geometry is built on the
		// CPU rather than expanded here.

		SDL_GPUShader *ribbonVertex = LoadShader("ribbon.vert", SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
		SDL_GPUShader *ribbonFragment = LoadShader("ribbon.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);

		if (ribbonVertex != nullptr && ribbonFragment != nullptr) {
			const SDL_GPUVertexBufferDescription ribbonBuffers[] = {
				{0, sizeof(effects::RibbonVertex), SDL_GPU_VERTEXINPUTRATE_VERTEX, 0},
			};

			const SDL_GPUVertexAttribute ribbonAttributes[] = {
				{0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(effects::RibbonVertex, Position)},
				{1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(effects::RibbonVertex, Coordinate)},
				{2, 0, SDL_GPU_VERTEXELEMENTFORMAT_UINT, offsetof(effects::RibbonVertex, Colour)},
			};

			SDL_GPUColorTargetDescription ribbonTarget{};
			ribbonTarget.format = swapchainFormat;
			ribbonTarget.blend_state.enable_blend = true;
			ribbonTarget.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
			ribbonTarget.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
			ribbonTarget.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
			ribbonTarget.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
			ribbonTarget.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
			ribbonTarget.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;

			SDL_GPUGraphicsPipelineCreateInfo ribbon{};
			ribbon.vertex_shader = ribbonVertex;
			ribbon.fragment_shader = ribbonFragment;
			ribbon.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP;
			ribbon.vertex_input_state.vertex_buffer_descriptions = ribbonBuffers;
			ribbon.vertex_input_state.num_vertex_buffers = 1;
			ribbon.vertex_input_state.vertex_attributes = ribbonAttributes;
			ribbon.vertex_input_state.num_vertex_attributes = 3;
			ribbon.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;

			// **Nothing is culled**, for the billboard's reason: a camera-facing
			// strip's winding depends on where the camera is, so a cull mode
			// would make a beam vanish from one side.
			ribbon.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
			ribbon.depth_stencil_state.enable_depth_test = true;
			ribbon.depth_stencil_state.enable_depth_write = false;
			ribbon.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS;
			ribbon.target_info.color_target_descriptions = &ribbonTarget;
			ribbon.target_info.num_color_targets = 1;
			ribbon.target_info.depth_stencil_format = DepthFormat;
			ribbon.target_info.has_depth_stencil_target = true;

			RibbonPipeline = SDL_CreateGPUGraphicsPipeline(Device, &ribbon);
			if (RibbonPipeline == nullptr) {
				ENGINE_ERROR("ribbon pipeline: {}", SDL_GetError());
			}

			SDL_GPUColorTargetDescription additiveRibbon = ribbonTarget;
			additiveRibbon.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
			additiveRibbon.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;

			SDL_GPUGraphicsPipelineCreateInfo additive = ribbon;
			additive.target_info.color_target_descriptions = &additiveRibbon;

			AdditiveRibbonPipeline = SDL_CreateGPUGraphicsPipeline(Device, &additive);
			if (AdditiveRibbonPipeline == nullptr) {
				ENGINE_ERROR("additive ribbon pipeline: {}", SDL_GetError());
			}

			SDL_ReleaseGPUShader(Device, ribbonVertex);
			SDL_ReleaseGPUShader(Device, ribbonFragment);
		}

		// --- image and overlay ----------------------------------------------

		SDL_GPUColorTargetDescription imageTarget{};
		imageTarget.format = swapchainFormat;

		SDL_GPUGraphicsPipelineCreateInfo image{};
		image.vertex_shader = overlayVertex;
		image.fragment_shader = imageFragment;
		image.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
		image.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
		image.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
		image.target_info.color_target_descriptions = &imageTarget;
		image.target_info.num_color_targets = 1;

		ImagePipeline = SDL_CreateGPUGraphicsPipeline(Device, &image);
		if (!ImagePipeline) {
			ENGINE_ERROR("image pipeline: {}", SDL_GetError());
		}

		SDL_GPUColorTargetDescription overlayTarget{};
		overlayTarget.format = swapchainFormat;
		overlayTarget.blend_state.enable_blend = true;
		overlayTarget.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
		overlayTarget.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
		// OverlayImage stores premultiplied RGB, so multiplying by source alpha
		// here would apply alpha twice and darken every translucent panel pixel.
		overlayTarget.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
		overlayTarget.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
		overlayTarget.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
		overlayTarget.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;

		SDL_GPUGraphicsPipelineCreateInfo overlay{};
		overlay.vertex_shader = overlayVertex;
		overlay.fragment_shader = overlayFragment;
		overlay.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
		overlay.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
		overlay.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
		// No depth: the overlay is on top of everything by definition.
		overlay.target_info.color_target_descriptions = &overlayTarget;
		overlay.target_info.num_color_targets = 1;

		OverlayPipeline = SDL_CreateGPUGraphicsPipeline(Device, &overlay);
		if (!OverlayPipeline) {
			ENGINE_ERROR("overlay pipeline: {}", SDL_GetError());
		}

		// The pipelines hold what they need; the shader objects do not have to
		// outlive their creation. **`opaqueVertex` is the exception and is
		// released in `Shutdown` instead** - see `OpaqueVertexShader`.
		SDL_ReleaseGPUShader(Device, opaqueFragment);
		SDL_ReleaseGPUShader(Device, gbufferFragment);
		SDL_ReleaseGPUShader(Device, depthLinearFragment);
		SDL_ReleaseGPUShader(Device, ssaoFragment);
		SDL_ReleaseGPUShader(Device, deferredLightingFragment);
		SDL_ReleaseGPUShader(Device, tonemapFragment);
		SDL_ReleaseGPUShader(Device, overlayVertex);
		SDL_ReleaseGPUShader(Device, imageFragment);
		SDL_ReleaseGPUShader(Device, overlayFragment);

		SDL_ReleaseGPUShader(Device, shadowVertex);
		SDL_ReleaseGPUShader(Device, shadowFragment);

		// The occlusion culling compute set. Not in the conjunction below for
		// the particle pipelines' reason: a build these failed on still draws a
		// world, and the validation path refuses `culling = "occlusion"`
		// documents with the failure named rather than the client dying here.
		Occlusion.Seed = LoadComputePipeline("hzb-seed.comp", 1, 0, 1, 0, 8, 8);
		Occlusion.Reduce = LoadComputePipeline("hzb-reduce.comp", 1, 0, 1, 0, 8, 8);
		Occlusion.Cull = LoadComputePipeline("occlusion-cull.comp", PYRAMID_LEVEL_LIMIT, 3, 0, 2, 64, 1);
		Occlusion.Args = LoadComputePipeline("occlusion-args.comp", 0, 2, 0, 1, 64, 1);

		// The particle simulation. Two read-only buffers (the block records and
		// the panes) and two read-write ones (the state pool and the instance
		// stream it writes the frame into); the spawn scatter reads one and
		// writes one. Sixty-four threads a group, which for the step is the
		// stride it walks a block's capacity in.
		Particles.Step = LoadComputePipeline("particle-step.comp", 0, 2, 0, 2, 64, 1);
		Particles.Spawn = LoadComputePipeline("particle-spawn.comp", 0, 1, 0, 1, 64, 1);

		// **The particle pipelines are deliberately not in this conjunction.** A
		// build whose particle shaders failed to compile still draws a world, and
		// failing initialisation over an effect would take the whole client down
		// for something a scene may not even use. `DrawParticles` checks for null
		// and draws nothing, with the error already in the log above.
		return OpaquePipeline != nullptr && TransparentPipeline != nullptr && ShadowPipeline != nullptr &&
			   ImagePipeline != nullptr && OverlayPipeline != nullptr && GBufferPipeline != nullptr &&
			   DepthLinearPipeline != nullptr && SsaoPipeline != nullptr &&
			   DeferredLightingPipeline != nullptr && TonemapPipeline != nullptr;
	}

	void Renderer::Impl::BindPipeline(
		SDL_GPURenderPass *pass, SDL_GPUGraphicsPipeline *pipeline, PipelineFamily family
	) {
		// **The one substitution point for the whole renderer.** Every caller
		// still names `OpaquePipeline` or `TransparentPipeline`, exactly as
		// before wireframe existed - this is where that request becomes a
		// line-drawing one, so the screen pass, the surface pass and every
		// mirror get it with nothing edited at their own call sites. See
		// `WireframeOpaquePipeline`'s own header for the rest of the argument.
		if (WireframeMode) {
			if (family == PipelineFamily::Opaque && WireframeOpaquePipeline != nullptr) {
				pipeline = WireframeOpaquePipeline;
			} else if (family == PipelineFamily::Transparent && WireframeTransparentPipeline != nullptr) {
				pipeline = WireframeTransparentPipeline;
			}
		}

		SDL_BindGPUGraphicsPipeline(pass, pipeline);
		ActivePipeline = pipeline;
		ActiveFamily = family;
	}

	bool Renderer::Impl::AddShaderVariant(const core::Name &name, std::span<const uint32_t> spirv) {
		if (!name.IsValid() || spirv.empty() || !VariantsReady || OpaqueVertexShader == nullptr) {
			return false;
		}

		// **The same counts `opaque.frag` declares, and they are the interface.**
		// A shader object carries its sampler and uniform-buffer counts rather
		// than the pipeline doing so, so a module declaring different ones binds
		// and silently reads nothing - the trap `CreatePipelines` already records
		// for the built-in fragment shader. That is why a `ShaderScript` is a
		// fragment shader written against `opaque.frag`'s slots and not against
		// a blank page.
		// **The runtime half of the translation, and it is the half a build step
		// cannot do.** A `ShaderScript` does not exist when the shaders are
		// compiled, so a device that takes MSL gets nothing at all unless the
		// engine can translate one while it runs. Same function as
		// `mono.tools/shadercross` calls, so a script and a built-in land on the
		// same Metal indices.
		//
		// A failure is a log line and a refused variant rather than a fatal, for
		// the reason `ShaderLibrary` gives about a compile failure: the part
		// keeps the engine's own shader and the world keeps running.
		const bool toMsl = Binary.Form == resources::ShaderForm::Msl;

		std::string translated;
		if (toMsl) {
			msl::Translation result = msl::Translate(spirv);
			if (result.Failed) {
				ENGINE_ERROR("shader '{}' cannot be translated to MSL: {}", name.Text(), result.Error);
				return false;
			}
			translated = std::move(result.Source);
		}

		SDL_GPUShaderCreateInfo info{};
		// Metal's backend reads the source with `initWithBytes:length:`, so the
		// length is the text and not the text plus its terminator.
		info.code = toMsl ? reinterpret_cast<const Uint8 *>(translated.data())
						  : reinterpret_cast<const Uint8 *>(spirv.data());
		info.code_size = toMsl ? translated.size() : spirv.size() * sizeof(uint32_t);
		info.entrypoint = Binary.EntryPoint;
		info.format = Binary.Format;
		info.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
		info.num_samplers = 9;
		info.num_uniform_buffers = 3;

		SDL_GPUShader *fragment = SDL_CreateGPUShader(Device, &info);
		if (fragment == nullptr) {
			ENGINE_ERROR("shader '{}': {}", name.Text(), SDL_GetError());
			return false;
		}

		SDL_GPUGraphicsPipelineCreateInfo opaque = VariantOpaqueInfo;
		opaque.vertex_shader = OpaqueVertexShader;
		opaque.fragment_shader = fragment;

		SDL_GPUGraphicsPipelineCreateInfo blended = VariantBlendedInfo;
		blended.vertex_shader = OpaqueVertexShader;
		blended.fragment_shader = fragment;

		ShaderVariant variant;
		variant.Fragment = fragment;
		variant.Opaque = SDL_CreateGPUGraphicsPipeline(Device, &opaque);
		variant.Transparent = SDL_CreateGPUGraphicsPipeline(Device, &blended);

		// **Both or neither.** A variant with one pipeline would draw a wall
		// toon-shaded and the pane in front of it with the engine's shader,
		// which reads as the material not applying to glass rather than as a
		// pipeline that failed to build.
		if (variant.Opaque == nullptr || variant.Transparent == nullptr) {
			ENGINE_ERROR("shader '{}' pipeline: {}", name.Text(), SDL_GetError());
			if (variant.Opaque != nullptr) {
				SDL_ReleaseGPUGraphicsPipeline(Device, variant.Opaque);
			}
			if (variant.Transparent != nullptr) {
				SDL_ReleaseGPUGraphicsPipeline(Device, variant.Transparent);
			}
			SDL_ReleaseGPUShader(Device, fragment);
			return false;
		}

		// Replacing is the ordinary case: an author editing a `ShaderScript`
		// bumps its revision every keystroke that lands, and the library hands
		// the new words straight back here.
		DropShaderVariant(name);
		ShaderVariants[name.Id()] = variant;
		return true;
	}

	void Renderer::Impl::DropShaderVariant(const core::Name &name) {
		const auto found = ShaderVariants.find(name.Id());
		if (found == ShaderVariants.end()) {
			return;
		}

		// **Ordered after the last frame that could have bound it**, which is
		// what `WaitForFrame` in `Renderer::DropShader` is for: releasing a
		// pipeline a command buffer in flight still references is a use after
		// free inside the driver.
		SDL_ReleaseGPUGraphicsPipeline(Device, found->second.Opaque);
		SDL_ReleaseGPUGraphicsPipeline(Device, found->second.Transparent);
		SDL_ReleaseGPUShader(Device, found->second.Fragment);
		ShaderVariants.erase(found);
	}

	void Renderer::Impl::ReleaseShaderVariants() {
		for (auto &entry : ShaderVariants) {
			SDL_ReleaseGPUGraphicsPipeline(Device, entry.second.Opaque);
			SDL_ReleaseGPUGraphicsPipeline(Device, entry.second.Transparent);
			SDL_ReleaseGPUShader(Device, entry.second.Fragment);
		}
		ShaderVariants.clear();

		if (OpaqueVertexShader != nullptr) {
			SDL_ReleaseGPUShader(Device, OpaqueVertexShader);
			OpaqueVertexShader = nullptr;
		}
		VariantsReady = false;
	}

	SDL_GPUGraphicsPipeline *Renderer::Impl::VariantFor(const core::Name &shader) const {
		if (!shader.IsValid() || ShaderVariants.empty()) {
			return nullptr;
		}

		// **An unknown name draws with the engine's shader rather than not at
		// all**, which is `MeshTable::Resolve`'s rule: a shader that has not
		// arrived is the ordinary state of a world still loading, and a part
		// that vanished until it did would be a worse symptom than one drawn
		// plainly. The name being wrong rather than late is reported by
		// `ShaderLibrary`, where the world can still be asked about it.
		const auto found = ShaderVariants.find(shader.Id());
		if (found == ShaderVariants.end()) {
			return nullptr;
		}

		switch (ActiveFamily) {
		case PipelineFamily::Opaque:
			return found->second.Opaque;
		case PipelineFamily::Transparent:
			return found->second.Transparent;
		case PipelineFamily::Other:
			break;
		}
		return nullptr;
	}

	namespace {
		// How many indirect draw arguments one slot-run emits: one per non-empty
		// material range, or one for the whole mesh. `DrawSlots`' emit skips an
		// empty range without advancing its argument index, so anything walking
		// the arguments has to count the same way or drift one entry per empty
		// range and draw with a neighbour's geometry.
		uint32_t DrawArgumentCount(const MeshEntry &mesh) {
			if (mesh.Runs.empty()) {
				return mesh.Whole.IndexCount > 0 ? 1u : 0u;
			}
			uint32_t entries = 0;
			for (const MeshRange &range : mesh.Runs) {
				entries += range.IndexCount > 0 ? 1u : 0u;
			}
			return entries;
		}
	}

	uint32_t Renderer::Impl::DrawSlots(
		SDL_GPUCommandBuffer *command,
		SDL_GPURenderPass *pass,
		uint32_t first,
		uint32_t count,
		const LightingUniforms *lighting,
		SDL_GPUTexture *shadow,
		SDL_GPUSampler *shadowSampler,
		SDL_GPUTexture *surface,
		SDL_GPUSampler *surfaceSampler,
		uint32_t tagFilter,
		uint64_t &triangles,
		const IndirectPhase *indirect
	) {
		if (count == 0 || first + count > SlotMesh.size()) {
			return 0;
		}

		uint32_t calls = 0;

		// Which slot-run and which draw argument the walk is at, for the
		// indirect phases. Argument order **is** walk order - the builder in
		// the upload path walks the same slots with the same predicate, so an
		// index here names the entry it wrote there.
		uint32_t slotRun = 0;
		uint32_t argument = 0;

		// What the pass bound before this call, so it can be put back.
		//
		// **Restored rather than left**, because a caller issues several
		// `DrawSlots` in one pass and every one of them assumes the pass's own
		// pipeline is bound. A run that ended on a variant would hand the next
		// call somebody else's fragment shader.
		SDL_GPUGraphicsPipeline *const base = ActivePipeline;
		SDL_GPUGraphicsPipeline *bound = base;

		// One draw for one range of one mesh, over `run` consecutive instances.
		const auto emit = [&](const MeshRange &range,
							  const core::Name &texture,
							  const std::array<float, 4> &colour,
							  uint32_t slot,
							  uint32_t run) {
			if (range.IndexCount == 0) {
				return;
			}

			if (lighting != nullptr) {
				// **The default, not the fallback texel, and not "do not
				// sample".** A drawable naming no texture is not a drawable with
				// nothing to draw - it is one made of the engine's default
				// material, which `DefaultTexture.hpp` says is a real sheet of
				// white plastic. So the slot always has content and the shader
				// always samples it; what used to be the "no texture" branch is
				// now the case where `Material = None` means something.
				//
				// **And naming a texture that is not here is a third case, not
				// the first one again.** A part that asked for a sheet the table
				// does not hold gets the purple checkerboard, because "nobody
				// textured this" and "this asked for something that never
				// arrived" are different facts and only one of them is finished.
				// The same split `scene::KeepLoaded` makes for geometry.
				//
				// **A sheet still on its way is the first case, not the third**,
				// which is `D00107` closed: the marker now means *nothing is
				// coming*. `ChooseTexture` is the rule and carries the argument;
				// it is a free function so a suite can state it without a
				// device.
				SDL_GPUTexture *const found = Textures.Find(texture);
				const TextureChoice choice =
					ChooseTexture(found != nullptr, texture.IsValid(), Textures.Expecting(texture));

				SDL_GPUTexture *const sampled = choice == TextureChoice::Named	   ? found
												: choice == TextureChoice::Missing ? Textures.Missing()
																				   : Textures.Default();
				const bool absent = choice == TextureChoice::Missing;

				const auto dataMap = [&](core::Name name) {
					SDL_GPUTexture *foundMap = Textures.Find(name);
					if (foundMap != nullptr) {
						return foundMap;
					}
					if (name.IsValid() && !Textures.Expecting(name)) {
						return Textures.Missing();
					}
					return static_cast<SDL_GPUTexture *>(nullptr);
				};

				SDL_GPUTexture *const normal = dataMap(SlotNormalMap[slot]);
				SDL_GPUTexture *const roughness = dataMap(SlotRoughnessMap[slot]);
				SDL_GPUTexture *const occlusion = dataMap(SlotOcclusionMap[slot]);
				SDL_GPUTexture *const height = dataMap(SlotHeightMap[slot]);
				SDL_GPUTexture *const emissive = dataMap(SlotEmissiveMap[slot]);
				SDL_GPUSampler *const fallbackSampler =
					surfaceSampler != nullptr ? surfaceSampler : Textures.Sampler();
				SDL_GPUSampler *const sampledShadowSampler =
					shadowSampler != nullptr ? shadowSampler : fallbackSampler;
				SDL_GPUSampler *const sampledBeamSampler =
					ShadowSampler != nullptr ? ShadowSampler : sampledShadowSampler;

				// **The fallback texel is bound rather than the binding being
				// skipped** for data maps, because a sampler a pipeline
				// declares must have something in it - an unbound one is
				// undefined behaviour on some backends and a validation error on
				// others. Those two are genuinely absent features rather than
				// defaulted ones, and their uniform flags still say so.
				// **A fourth sampler for the beams**, bound from the member
				// rather than passed in: which holes transport a shadow is a
				// property of the frame and not of one draw, and threading it
				// through would put it in a signature five passes call.
				const SDL_GPUTextureSamplerBinding samplers[] = {
					{shadow != nullptr ? shadow : FallbackTexture, sampledShadowSampler},
					{surface != nullptr ? surface : FallbackTexture, fallbackSampler},
					{sampled != nullptr ? sampled : FallbackTexture, Textures.Sampler()},
					{BeamTexture != nullptr ? BeamTexture : FallbackTexture, sampledBeamSampler},
					{normal != nullptr ? normal : FallbackTexture, Textures.Sampler()},
					{roughness != nullptr ? roughness : FallbackTexture, Textures.Sampler()},
					{occlusion != nullptr ? occlusion : FallbackTexture, Textures.Sampler()},
					{emissive != nullptr ? emissive : FallbackTexture, Textures.Sampler()},
					{height != nullptr ? height : FallbackTexture, Textures.Sampler()},
				};
				SDL_BindGPUFragmentSamplers(pass, 0, samplers, 9);

				LightingUniforms uniforms = *lighting;

				// **The marker arrives as itself, so the base colour stops
				// applying to it.** Every other texture here is modulated by the
				// material's colour, which is what makes one grey sheet serve a
				// whole palette - but a magenta check multiplied by a dark red
				// part is a dark pattern that reads as somebody's intent. A
				// marker that can be tinted into looking deliberate is not a
				// marker.
				const std::array<float, 4> tint =
					absent ? std::array<float, 4>{1.0f, 1.0f, 1.0f, 1.0f} : colour;
				uniforms.BaseColour = glm::vec4{tint[0], tint[1], tint[2], tint[3]};
				uniforms.Surface = glm::vec4{
					sampled != nullptr ? 1.0f : 0.0f,
					0.0f,
					height != nullptr ? 1.0f : 0.0f,
					0.04f,
				};
				uniforms.Material = glm::vec4{
					normal != nullptr ? 1.0f : 0.0f,
					roughness != nullptr ? 1.0f : 0.0f,
					occlusion != nullptr ? 1.0f : 0.0f,
					emissive != nullptr ? 1.0f : 0.0f,
				};

				// **The cell is per draw, not per instance**, which is the whole
				// simplification: a sheet plays on the clock rather than on
				// anything an entity carries, so every part showing one GIF shows
				// the same frame. A per-instance phase is a real feature and is a
				// different one - `effects::FlipbookLayout` already has it for
				// particles, where the cell is a function of a particle's age.
				const FlipbookCell cell = Textures.CellOf(texture, AnimationSeconds);
				uniforms.Flipbook = glm::vec4{cell.Scale, cell.OffsetU, cell.OffsetV, 0.0f};

				// **The clock is stamped here and the effect arrives from the
				// caller.** Which effect is a fact about the surface being
				// sampled; what time it is, is a fact about the frame, and this
				// is the one place that already holds it.
				//
				// Wrapped rather than passed whole: a `float` a session old has
				// lost the precision these effects step by, and a scan line that
				// coarsens over an afternoon is a bug nobody would reproduce.
				uniforms.Mirror.y = static_cast<float>(std::fmod(AnimationSeconds, 1000.0));

				// **The slot's own plane, and the run above guarantees they agree.**
				// A run is broken wherever the plane changes, so every instance in
				// this draw is cut the same way - which is what makes a per-instance
				// fact expressible in a per-draw uniform.
				uniforms.SeamPlane = SlotSeam[slot];

				// **The far half of a body in a hole is lit by the sun turned the way
				// the body was.** Its normals are the near half's rotated by the
				// seam, so the world's own direction would shade one body with two
				// suns a quarter apart. Set per draw for the same reason the plane is,
				// and only where the row asked for it - a zero vector is every
				// instance that is not half of something.
				const glm::vec3 mapped{SlotSeamLight[slot]};
				if (glm::dot(mapped, mapped) > 0.0f) {
					uniforms.Direction = glm::vec4{mapped, 0.0f};
				}

				SDL_PushGPUFragmentUniformData(command, 0, &uniforms, sizeof(uniforms));
			} else {
				// **Depth only, and it still has to know where the body is cut.** A
				// half drawn whole into the shadow map casts a whole body's shadow,
				// so the near half of somebody in a doorway would darken the floor as
				// if none of them had gone through. `shadow.frag` takes the plane and
				// discards on the same test `opaque.frag` makes; it is one `vec4`
				// per draw and no samplers, which is why this is a push of its own
				// rather than the whole lighting block.
				const glm::vec4 plane = SlotSeam[slot];
				SDL_PushGPUFragmentUniformData(command, 0, &plane, sizeof(plane));
			}

			if (indirect != nullptr) {
				// The counts live on the GPU. `run` here is the phase's upper
				// bound, so the triangle tally can only overcount what the
				// cull discarded - an estimate is honest for a number whose
				// exact value never comes back to the CPU.
				SDL_DrawGPUIndexedPrimitivesIndirect(
					pass,
					indirect->Arguments,
					(indirect->FirstArgument + argument) *
						static_cast<uint32_t>(sizeof(SDL_GPUIndexedIndirectDrawCommand)),
					1
				);
				argument++;
			} else {
				SDL_DrawGPUIndexedPrimitives(
					pass, range.IndexCount, run, range.FirstIndex, range.VertexOffset, slot
				);
			}
			calls++;
			triangles += static_cast<uint64_t>(range.IndexCount / 3) * run;
		};

		uint32_t slot = first;
		while (slot < first + count) {
			// **A filtered-out slot ends the run and is stepped over.** The draw
			// list is not re-ordered for it: the order is shared by every view
			// and a filter is per view, so partitioning for one surface would
			// partition for all of them and the screen pass would draw the group
			// instead of the world. The cost is a run break wherever an excluded
			// instance sits between two included ones, which is a draw call and
			// not a wrong picture.
			if (!scene::MatchesTags(SlotTags[slot], tagFilter)) {
				slot++;
				continue;
			}

			const MeshEntry *const mesh = SlotMesh[slot];
			const core::Name texture = SlotTexture[slot];

			// **The shader joins what ends a run, because it is a pipeline.** A
			// pipeline bind is per draw at best, so two instances drawn through
			// different fragment shaders cannot share one - the same reason the
			// seam plane breaks a run, one level up from a uniform. A world
			// where nothing selects a shader holds one invalid name throughout
			// and never breaks on it. `SlotsShareRun` is the whole rule: the
			// data maps, the shader, the seam plane and its light all join the
			// mesh and the texture in what ends a run.
			const core::Name shader = SlotShader[slot];

			uint32_t run = 1;
			while (slot + run < first + count && SlotsShareRun(slot, slot + run) &&
				   scene::MatchesTags(SlotTags[slot + run], tagFilter)) {
				run++;
			}

			// **An indirect phase with nothing in this run skips its binds but
			// not its argument entries.** The argument buffer holds every run's
			// draws in walk order, so a run stepped over still advances the
			// index by the draws it would have emitted.
			const uint32_t phaseInstances =
				indirect != nullptr
					? (slotRun < indirect->RunDraws->size() ? (*indirect->RunDraws)[slotRun] : 0u)
					: run;
			if (indirect != nullptr && phaseInstances == 0) {
				argument += DrawArgumentCount(*mesh);
				slotRun++;
				slot += run;
				continue;
			}

			// **Bound per run and only where it changes.** A scene with no
			// custom shaders never enters this branch, and one where every part
			// wears the same one binds twice: once here and once on the way out.
			SDL_GPUGraphicsPipeline *const wanted = VariantFor(shader);
			SDL_GPUGraphicsPipeline *const want = wanted != nullptr ? wanted : base;
			if (want != bound && want != nullptr) {
				SDL_BindGPUGraphicsPipeline(pass, want);
				bound = want;
			}

			if (mesh->Runs.empty()) {
				// A mesh with no materials of its own - every built-in.
				emit(mesh->Whole, texture, {1.0f, 1.0f, 1.0f, 1.0f}, slot, phaseInstances);
			} else {
				for (size_t index = 0; index < mesh->Runs.size(); index++) {
					// **The instance's texture wins when it has one.** That is
					// what `MeshPart.TextureID` means: an imported mesh's own
					// per-material sheets are the default, and a part may
					// replace all of them at once.
					emit(
						mesh->Runs[index],
						texture.IsValid() ? texture : mesh->Textures[index],
						mesh->Colours[index],
						slot,
						phaseInstances
					);
				}
			}

			slotRun++;
			slot += run;
		}

		// Put the pass's own pipeline back - see `base` above.
		if (bound != base && base != nullptr) {
			SDL_BindGPUGraphicsPipeline(pass, base);
		}

		return calls;
	}

	bool Renderer::Impl::CreateGeometry() {
		// The built-in meshes and the sampler every texture shares. What used
		// to be an unconditional upload of one cube is now a table that starts
		// with six shapes and grows as content arrives.
		if (!Meshes.Initialise(Device) || !Textures.Initialise(Device)) {
			return false;
		}

		// The sampler stand-in, created here because this function already owns
		// a command buffer and a copy pass to fill it. See `FallbackTexture`.
		{
			SDL_GPUTextureCreateInfo info{};
			info.type = SDL_GPU_TEXTURETYPE_2D;
			info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
			info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
			info.width = 1;
			info.height = 1;
			info.layer_count_or_depth = 1;
			info.num_levels = 1;
			info.sample_count = SDL_GPU_SAMPLECOUNT_1;

			FallbackTexture = SDL_CreateGPUTexture(Device, &info);
			if (!FallbackTexture) {
				ENGINE_ERROR("fallback texture: {}", SDL_GetError());
				return false;
			}
		}

		// One texel, and the transfer buffer is temporary because nothing
		// rewrites it.
		SDL_GPUTransferBufferCreateInfo transferInfo{};
		transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
		transferInfo.size = OverlayImage::BYTES_PER_PIXEL;

		SDL_GPUTransferBuffer *transfer = SDL_CreateGPUTransferBuffer(Device, &transferInfo);
		if (!transfer) {
			ENGINE_ERROR("cube transfer buffer: {}", SDL_GetError());
			return false;
		}

		// Opaque white for the fallback texel. Nothing reads it - the uniform
		// flag sees to that - but a texture whose contents were never written
		// is uninitialised device memory, and "nothing reads it" is a claim
		// about the shaders of the day rather than a property of the resource.
		constexpr uint8_t FALLBACK_TEXEL[OverlayImage::BYTES_PER_PIXEL] = {255, 255, 255, 255};

		auto *mapped = static_cast<uint8_t *>(SDL_MapGPUTransferBuffer(Device, transfer, false));
		std::memcpy(mapped, FALLBACK_TEXEL, sizeof(FALLBACK_TEXEL));
		SDL_UnmapGPUTransferBuffer(Device, transfer);

		SDL_GPUCommandBuffer *command = SDL_AcquireGPUCommandBuffer(Device);
		SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(command);

		SDL_GPUTextureTransferInfo texel{};
		texel.transfer_buffer = transfer;
		texel.offset = 0;
		texel.pixels_per_row = 1;
		texel.rows_per_layer = 1;

		SDL_GPUTextureRegion texelTarget{};
		texelTarget.texture = FallbackTexture;
		texelTarget.w = 1;
		texelTarget.h = 1;
		texelTarget.d = 1;
		SDL_UploadToGPUTexture(copy, &texel, &texelTarget, false);

		SDL_EndGPUCopyPass(copy);
		SDL_SubmitGPUCommandBuffer(command);
		SDL_ReleaseGPUTransferBuffer(Device, transfer);

		return true;
	}

	bool Renderer::Impl::EnsureInstanceCapacity(uint32_t count) {
		if (count <= InstanceCapacity) {
			return true;
		}

		// Grow in powers of two. A scene that gains one entity per frame would
		// otherwise reallocate every frame.
		uint32_t capacity = InstanceCapacity == 0 ? 256 : InstanceCapacity;
		while (capacity < count) {
			capacity *= 2;
		}

		if (InstanceBuffer) {
			SDL_ReleaseGPUBuffer(Device, InstanceBuffer);
		}
		if (InstanceTransfer) {
			SDL_ReleaseGPUTransferBuffer(Device, InstanceTransfer);
		}

		const uint32_t bytes = capacity * static_cast<uint32_t>(sizeof(GpuInstance));

		SDL_GPUBufferCreateInfo bufferInfo{};
		// Compute-readable as well as a vertex stream: the occlusion cull reads
		// candidate rows out of this buffer to compact its survivors.
		bufferInfo.usage = SDL_GPU_BUFFERUSAGE_VERTEX | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ;
		bufferInfo.size = bytes;
		InstanceBuffer = SDL_CreateGPUBuffer(Device, &bufferInfo);

		SDL_GPUTransferBufferCreateInfo transferInfo{};
		transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
		transferInfo.size = bytes;
		InstanceTransfer = SDL_CreateGPUTransferBuffer(Device, &transferInfo);

		if (!InstanceBuffer || !InstanceTransfer) {
			ENGINE_ERROR("instance buffer of {} entries: {}", capacity, SDL_GetError());
			InstanceCapacity = 0;
			return false;
		}

		InstanceCapacity = capacity;
		return true;
	}

	bool Renderer::Impl::EnsureOcclusionResources(
		uint32_t argCount, uint32_t candidateCount, uint32_t runCount, uint32_t lateCount
	) {
		// Powers of two, for `EnsureInstanceCapacity`'s reason. One helper
		// because six buffers grown six ways is six chances to grow five.
		const auto grown = [](uint32_t have, uint32_t need) {
			uint32_t capacity = have == 0 ? 64 : have;
			while (capacity < need) {
				capacity *= 2;
			}
			return capacity;
		};
		const auto ensure = [&](SDL_GPUBuffer *&buffer,
								uint32_t &capacity,
								uint32_t need,
								uint32_t stride,
								SDL_GPUBufferUsageFlags usage,
								const char *what) {
			if (need <= capacity && buffer != nullptr) {
				return true;
			}
			const uint32_t entries = grown(capacity, need);
			if (buffer != nullptr) {
				SDL_ReleaseGPUBuffer(Device, buffer);
			}
			SDL_GPUBufferCreateInfo info{};
			info.usage = usage;
			info.size = entries * stride;
			buffer = SDL_CreateGPUBuffer(Device, &info);
			if (buffer == nullptr) {
				ENGINE_ERROR("occlusion {} buffer of {} entries: {}", what, entries, SDL_GetError());
				capacity = 0;
				return false;
			}
			capacity = entries;
			return true;
		};

		constexpr uint32_t COMMAND_BYTES = sizeof(SDL_GPUIndexedIndirectDrawCommand);
		constexpr uint32_t CANDIDATE_BYTES = 32; // two vec4 - see occlusion-cull.comp

		// `ArgRuns` shares the argument capacity and `Counts` the run capacity,
		// because each pair grows for the same reason on the same frame. The
		// paired buffer is recreated whenever its partner was, which the null
		// check after a release makes true by construction.
		bool ready = true;
		if (argCount * 2 > Occlusion.ArgumentCapacity || Occlusion.Arguments == nullptr ||
			Occlusion.ArgRuns == nullptr) {
			if (Occlusion.ArgRuns != nullptr) {
				SDL_ReleaseGPUBuffer(Device, Occlusion.ArgRuns);
				Occlusion.ArgRuns = nullptr;
			}
			ready = ensure(
						Occlusion.Arguments,
						Occlusion.ArgumentCapacity,
						argCount * 2,
						COMMAND_BYTES,
						SDL_GPU_BUFFERUSAGE_INDIRECT | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE,
						"argument"
					) &&
					ready;
			if (ready) {
				SDL_GPUBufferCreateInfo info{};
				info.usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ;
				info.size = Occlusion.ArgumentCapacity * static_cast<uint32_t>(sizeof(uint32_t));
				Occlusion.ArgRuns = SDL_CreateGPUBuffer(Device, &info);
				ready = Occlusion.ArgRuns != nullptr;
				if (!ready) {
					ENGINE_ERROR("occlusion argument-run buffer: {}", SDL_GetError());
				}
			}
		}

		if (runCount > Occlusion.RunCapacity || Occlusion.RunTable == nullptr ||
			Occlusion.Counts == nullptr) {
			if (Occlusion.Counts != nullptr) {
				SDL_ReleaseGPUBuffer(Device, Occlusion.Counts);
				Occlusion.Counts = nullptr;
			}
			ready = ensure(
						Occlusion.RunTable,
						Occlusion.RunCapacity,
						runCount,
						sizeof(uint32_t),
						SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ,
						"run table"
					) &&
					ready;
			if (ready) {
				SDL_GPUBufferCreateInfo info{};
				info.usage =
					SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE;
				info.size = Occlusion.RunCapacity * static_cast<uint32_t>(sizeof(uint32_t));
				Occlusion.Counts = SDL_CreateGPUBuffer(Device, &info);
				ready = Occlusion.Counts != nullptr;
				if (!ready) {
					ENGINE_ERROR("occlusion count buffer: {}", SDL_GetError());
				}
			}
		}

		ready = ensure(
					Occlusion.Candidates,
					Occlusion.CandidateCapacity,
					candidateCount,
					CANDIDATE_BYTES,
					SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ,
					"candidate"
				) &&
				ready;
		ready = ensure(
					Occlusion.LateInstances,
					Occlusion.LateCapacity,
					lateCount,
					sizeof(GpuInstance),
					SDL_GPU_BUFFERUSAGE_VERTEX | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE,
					"late instance"
				) &&
				ready;

		// One transfer stages everything the CPU writes, packed back to back in
		// the order `submitUploads` copies it out: arguments, candidates, run
		// table, argument runs, count zeros.
		const uint32_t staged = argCount * 2 * COMMAND_BYTES + candidateCount * CANDIDATE_BYTES +
								runCount * static_cast<uint32_t>(sizeof(uint32_t)) +
								argCount * static_cast<uint32_t>(sizeof(uint32_t)) +
								runCount * static_cast<uint32_t>(sizeof(uint32_t));
		if (staged > Occlusion.TransferCapacity || Occlusion.Transfer == nullptr) {
			if (Occlusion.Transfer != nullptr) {
				SDL_ReleaseGPUTransferBuffer(Device, Occlusion.Transfer);
			}
			const uint32_t bytes = grown(Occlusion.TransferCapacity, staged);
			SDL_GPUTransferBufferCreateInfo info{};
			info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
			info.size = bytes;
			Occlusion.Transfer = SDL_CreateGPUTransferBuffer(Device, &info);
			if (Occlusion.Transfer == nullptr) {
				ENGINE_ERROR("occlusion transfer buffer of {} bytes: {}", bytes, SDL_GetError());
				Occlusion.TransferCapacity = 0;
				ready = false;
			} else {
				Occlusion.TransferCapacity = bytes;
			}
		}
		return ready;
	}

	bool Renderer::Impl::EnsurePyramid(uint32_t width, uint32_t height) {
		if (Occlusion.Width == width && Occlusion.Height == height && Occlusion.Levels[0] != nullptr) {
			return true;
		}
		for (SDL_GPUTexture *&level : Occlusion.Levels) {
			if (level != nullptr) {
				SDL_ReleaseGPUTexture(Device, level);
				level = nullptr;
			}
		}
		Occlusion.Width = 0;
		Occlusion.Height = 0;
		Occlusion.LevelCount = 0;

		uint32_t levelWidth = width;
		uint32_t levelHeight = height;
		uint32_t count = 0;
		while (count < PYRAMID_LEVEL_LIMIT) {
			SDL_GPUTextureCreateInfo info{};
			info.type = SDL_GPU_TEXTURETYPE_2D;
			info.format = SDL_GPU_TEXTUREFORMAT_R32_FLOAT;
			info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
			info.width = levelWidth;
			info.height = levelHeight;
			info.layer_count_or_depth = 1;
			info.num_levels = 1;
			info.sample_count = SDL_GPU_SAMPLECOUNT_1;
			Occlusion.Levels[count] = SDL_CreateGPUTexture(Device, &info);
			if (Occlusion.Levels[count] == nullptr) {
				ENGINE_ERROR("depth pyramid level {}: {}", count, SDL_GetError());
				return false;
			}
			count++;
			if (levelWidth == 1 && levelHeight == 1) {
				break;
			}
			levelWidth = std::max(levelWidth / 2, 1u);
			levelHeight = std::max(levelHeight / 2, 1u);
		}

		Occlusion.Width = width;
		Occlusion.Height = height;
		Occlusion.LevelCount = count;
		return true;
	}

	void Renderer::Impl::BuildPyramid(SDL_GPUCommandBuffer *command, SDL_GPUTexture *depth) {
		ENGINE_PROFILE_CAT("depth pyramid", core::ProfileCategory::Render);

		// The shaders reproduce this halving, so the two must stay one rule:
		// level n is `max(size >> n, 1)` of level zero.
		uint32_t sourceWidth = Occlusion.Width;
		uint32_t sourceHeight = Occlusion.Height;
		for (uint32_t level = 0; level < Occlusion.LevelCount; level++) {
			const uint32_t destinationWidth = level == 0 ? sourceWidth : std::max(sourceWidth / 2, 1u);
			const uint32_t destinationHeight = level == 0 ? sourceHeight : std::max(sourceHeight / 2, 1u);

			SDL_GPUStorageTextureReadWriteBinding destination{};
			destination.texture = Occlusion.Levels[level];
			// A fresh version per view: another view's pyramid may still be in
			// flight, and its cull already bound the version it reads.
			destination.cycle = true;

			SDL_GPUComputePass *pass = SDL_BeginGPUComputePass(command, &destination, 1, nullptr, 0);
			if (pass == nullptr) {
				ENGINE_ERROR("depth pyramid: SDL_BeginGPUComputePass: {}", SDL_GetError());
				return;
			}
			SDL_BindGPUComputePipeline(pass, level == 0 ? Occlusion.Seed : Occlusion.Reduce);

			const SDL_GPUTextureSamplerBinding source{
				level == 0 ? depth : Occlusion.Levels[level - 1], Textures.Sampler()
			};
			SDL_BindGPUComputeSamplers(pass, 0, &source, 1);

			// The seed reads xy as its own size; the reduce reads source then
			// destination.
			const int32_t sizes[4] = {
				static_cast<int32_t>(level == 0 ? destinationWidth : sourceWidth),
				static_cast<int32_t>(level == 0 ? destinationHeight : sourceHeight),
				static_cast<int32_t>(destinationWidth),
				static_cast<int32_t>(destinationHeight),
			};
			SDL_PushGPUComputeUniformData(command, 0, sizes, sizeof(sizes));
			SDL_DispatchGPUCompute(pass, (destinationWidth + 7) / 8, (destinationHeight + 7) / 8, 1);
			SDL_EndGPUComputePass(pass);

			sourceWidth = destinationWidth;
			sourceHeight = destinationHeight;
		}
	}

	void
	Renderer::Impl::DispatchOcclusionCull(SDL_GPUCommandBuffer *command, const glm::mat4 &viewProjection) {
		ENGINE_PROFILE_CAT("occlusion cull", core::ProfileCategory::Render);

		// Pass one: test every candidate and compact the survivors.
		{
			SDL_GPUStorageBufferReadWriteBinding outputs[2]{};
			// `Counts` is not cycled: the upload wrote this frame's zeros into
			// the version the atomics must land in. The late buffer took no
			// upload, so this write is its first touch and may cycle.
			outputs[0].buffer = Occlusion.Counts;
			outputs[1].buffer = Occlusion.LateInstances;
			outputs[1].cycle = true;

			SDL_GPUComputePass *pass = SDL_BeginGPUComputePass(command, nullptr, 0, outputs, 2);
			if (pass == nullptr) {
				ENGINE_ERROR("occlusion cull: SDL_BeginGPUComputePass: {}", SDL_GetError());
				return;
			}
			SDL_BindGPUComputePipeline(pass, Occlusion.Cull);

			SDL_GPUTextureSamplerBinding levels[PYRAMID_LEVEL_LIMIT];
			for (uint32_t level = 0; level < PYRAMID_LEVEL_LIMIT; level++) {
				// The tail past `LevelCount` is never selected; level zero fills
				// it because a declared sampler must have something bound.
				levels[level] = SDL_GPUTextureSamplerBinding{
					Occlusion.Levels[std::min(level, Occlusion.LevelCount - 1)], Textures.Sampler()
				};
			}
			SDL_BindGPUComputeSamplers(pass, 0, levels, PYRAMID_LEVEL_LIMIT);

			SDL_GPUBuffer *const reads[3] = {Occlusion.Candidates, InstanceBuffer, Occlusion.RunTable};
			SDL_BindGPUComputeStorageBuffers(pass, 0, reads, 3);

			struct CullUniform {
				glm::mat4 ViewProjection;
				uint32_t Counts[4];
				float Level0[4];
			} uniform{
				viewProjection,
				{OcclusionFrame.CandidateCount, Occlusion.LevelCount, 0, 0},
				{static_cast<float>(Occlusion.Width), static_cast<float>(Occlusion.Height), 0.0f, 0.0f},
			};
			SDL_PushGPUComputeUniformData(command, 0, &uniform, sizeof(uniform));
			SDL_DispatchGPUCompute(pass, (OcclusionFrame.CandidateCount + 63) / 64, 1, 1);
			SDL_EndGPUComputePass(pass);
		}

		// Pass two: copy each run's survivor count into every indirect draw
		// argument that run emits. Its own pass, so the counts are complete
		// before anything reads them.
		{
			SDL_GPUStorageBufferReadWriteBinding output{};
			output.buffer = Occlusion.Arguments;

			SDL_GPUComputePass *pass = SDL_BeginGPUComputePass(command, nullptr, 0, &output, 1);
			if (pass == nullptr) {
				ENGINE_ERROR("occlusion arguments: SDL_BeginGPUComputePass: {}", SDL_GetError());
				return;
			}
			SDL_BindGPUComputePipeline(pass, Occlusion.Args);

			SDL_GPUBuffer *const reads[2] = {Occlusion.ArgRuns, Occlusion.Counts};
			SDL_BindGPUComputeStorageBuffers(pass, 0, reads, 2);

			const uint32_t range[4] = {OcclusionFrame.ArgCount, OcclusionFrame.ArgCount, 0, 0};
			SDL_PushGPUComputeUniformData(command, 0, range, sizeof(range));
			SDL_DispatchGPUCompute(pass, (OcclusionFrame.ArgCount + 63) / 64, 1, 1);
			SDL_EndGPUComputePass(pass);
		}
	}

	void Renderer::Impl::ReleaseOcclusion() {
		for (SDL_GPUTexture *&level : Occlusion.Levels) {
			if (level != nullptr) {
				SDL_ReleaseGPUTexture(Device, level);
				level = nullptr;
			}
		}
		const auto releaseBuffer = [&](SDL_GPUBuffer *&buffer) {
			if (buffer != nullptr) {
				SDL_ReleaseGPUBuffer(Device, buffer);
				buffer = nullptr;
			}
		};
		releaseBuffer(Occlusion.Arguments);
		releaseBuffer(Occlusion.Candidates);
		releaseBuffer(Occlusion.RunTable);
		releaseBuffer(Occlusion.ArgRuns);
		releaseBuffer(Occlusion.Counts);
		releaseBuffer(Occlusion.LateInstances);
		if (Occlusion.Transfer != nullptr) {
			SDL_ReleaseGPUTransferBuffer(Device, Occlusion.Transfer);
			Occlusion.Transfer = nullptr;
		}
		const auto releasePipeline = [&](SDL_GPUComputePipeline *&pipeline) {
			if (pipeline != nullptr) {
				SDL_ReleaseGPUComputePipeline(Device, pipeline);
				pipeline = nullptr;
			}
		};
		releasePipeline(Occlusion.Seed);
		releasePipeline(Occlusion.Reduce);
		releasePipeline(Occlusion.Cull);
		releasePipeline(Occlusion.Args);
		Occlusion = OcclusionState{};
	}

	namespace {
		// --- the device layouts -----------------------------------------------
		//
		// **These numbers are also written in `particle-step.comp` and
		// `particle-spawn.comp`, and a disagreement between the two is a scene of
		// garbage rather than a compile error.** `vec3` takes sixteen-byte
		// alignment in std430 and none of these structs is laid out that way, so
		// both sides index a flat `uint` array by hand - the same arrangement
		// `occlusion-args.comp` has for its five-word draw command. The
		// `static_assert`s below are what makes a change to the C++ side fail to
		// build instead of failing to look right.
		constexpr uint32_t PARTICLE_STATE_WORDS = 15;
		constexpr uint32_t PARTICLE_BIRTH_WORDS = PARTICLE_STATE_WORDS + 1;
		constexpr uint32_t PARTICLE_SEAM_WORDS = 20;

		// The block record: eighteen words of parameters, fourteen spare, then
		// the four sixteen-sample curves. The spare run is deliberate - a field
		// added in front of the curves would otherwise shift all sixty-four of
		// them and every one would be silently reinterpreted.
		constexpr uint32_t PARTICLE_BLOCK_WORDS = 96;
		constexpr uint32_t PARTICLE_BLOCK_ROTATION = 0;
		constexpr uint32_t PARTICLE_BLOCK_POSITION = 4;
		constexpr uint32_t PARTICLE_BLOCK_ACCELERATION = 7;
		constexpr uint32_t PARTICLE_BLOCK_DRAG = 10;
		constexpr uint32_t PARTICLE_BLOCK_FIRST = 11;
		constexpr uint32_t PARTICLE_BLOCK_CAPACITY = 12;
		constexpr uint32_t PARTICLE_BLOCK_FLAGS = 13;
		constexpr uint32_t PARTICLE_BLOCK_CELLS = 14;
		constexpr uint32_t PARTICLE_BLOCK_PLAYBACK = 15;
		constexpr uint32_t PARTICLE_BLOCK_RATE = 16;
		constexpr uint32_t PARTICLE_BLOCK_DESTINATION = 17;
		constexpr uint32_t PARTICLE_BLOCK_GENERATION = 18;
		constexpr uint32_t PARTICLE_BLOCK_CURVE_SIZE = 32;
		constexpr uint32_t PARTICLE_BLOCK_CURVE_ALPHA = 48;
		constexpr uint32_t PARTICLE_BLOCK_CURVE_SQUASH = 64;
		constexpr uint32_t PARTICLE_BLOCK_CURVE_COLOUR = 80;

		static_assert(
			sizeof(effects::ParticleState) == PARTICLE_STATE_WORDS * sizeof(uint32_t),
			"the state's width is the shader's stride"
		);
		static_assert(effects::CURVE_SAMPLES == 16, "the block record reserves sixteen words a curve");
		static_assert(
			PARTICLE_BLOCK_CURVE_COLOUR + effects::CURVE_SAMPLES <= PARTICLE_BLOCK_WORDS,
			"the curves must fit inside the record"
		);

		// One float, into a word of the record.
		void PutFloat(uint32_t *words, uint32_t at, float value) {
			std::memcpy(words + at, &value, sizeof(float));
		}

		void PutVector(uint32_t *words, uint32_t at, const core::Vector3 &value) {
			PutFloat(words, at, value.X);
			PutFloat(words, at + 1, value.Y);
			PutFloat(words, at + 2, value.Z);
		}

		// Fills one block record for `particle-step.comp`.
		//
		// @param destination Where this block's run starts in the draw stream.
		void WriteParticleBlock(uint32_t *words, const effects::EmitterBlock &block, uint32_t destination) {
			const glm::quat turn = block.Frame.Rotation();
			PutFloat(words, PARTICLE_BLOCK_ROTATION, turn.x);
			PutFloat(words, PARTICLE_BLOCK_ROTATION + 1, turn.y);
			PutFloat(words, PARTICLE_BLOCK_ROTATION + 2, turn.z);
			PutFloat(words, PARTICLE_BLOCK_ROTATION + 3, turn.w);
			PutVector(words, PARTICLE_BLOCK_POSITION, block.Frame.Position);
			PutVector(words, PARTICLE_BLOCK_ACCELERATION, block.Acceleration);
			PutFloat(words, PARTICLE_BLOCK_DRAG, block.Drag);

			words[PARTICLE_BLOCK_FIRST] = block.First;
			words[PARTICLE_BLOCK_CAPACITY] = block.Capacity;
			words[PARTICLE_BLOCK_DESTINATION] = destination;
			words[PARTICLE_BLOCK_GENERATION] = block.Generation;

			// **The random flipbook mode is decided at spawn and not here**, and
			// that is what keeps a sixty-four-bit hash out of a shader with no
			// sixty-four-bit integers: the mode picks a cell once and keeps it for
			// the particle's whole life, so the host writes it into the state's
			// rotation word and the step is told to leave the cell alone. Every
			// other mode is a function of age and the shader works it out.
			const uint32_t cells = std::min<uint32_t>(block.Frames, effects::FlipbookCells(block.Flipbook));
			const bool fixed = block.FlipbookPlayback == effects::FlipbookMode::Random;
			words[PARTICLE_BLOCK_FLAGS] = (block.Locked ? 1u : 0u) | (fixed ? 2u : 0u);
			words[PARTICLE_BLOCK_CELLS] = cells;
			words[PARTICLE_BLOCK_PLAYBACK] = static_cast<uint32_t>(block.FlipbookPlayback);
			PutFloat(words, PARTICLE_BLOCK_RATE, block.FlipbookRate);

			for (uint32_t at = 0; at < effects::CURVE_SAMPLES; at++) {
				PutFloat(words, PARTICLE_BLOCK_CURVE_SIZE + at, block.Curves.Size[at]);
				PutFloat(words, PARTICLE_BLOCK_CURVE_ALPHA + at, block.Curves.Alpha[at]);
				PutFloat(words, PARTICLE_BLOCK_CURVE_SQUASH + at, block.Curves.Squash[at]);
				words[PARTICLE_BLOCK_CURVE_COLOUR + at] = block.Curves.Colour[at];
			}
		}

		// Fills one pane record. Laid out as floats throughout, so unlike a block
		// it needs no word-by-word punning.
		void WriteParticleSeam(float *words, const render::ParticleSeam &seam) {
			const auto put = [words](uint32_t at, const core::Vector3 &value) {
				words[at] = value.X;
				words[at + 1] = value.Y;
				words[at + 2] = value.Z;
			};
			put(0, seam.Centre);
			put(3, seam.Normal);
			put(6, seam.First);
			put(9, seam.Second);

			const glm::quat turn = seam.Mapping.Rotation();
			words[12] = turn.x;
			words[13] = turn.y;
			words[14] = turn.z;
			words[15] = turn.w;
			put(16, seam.Mapping.Position);
			words[19] = seam.Scale;
		}
	}

	bool Renderer::Impl::ReserveParticles(uint32_t count) {
		if (count <= ParticleCapacity) {
			return true;
		}

		// Powers of two, for `EnsureInstanceCapacity`'s reason and with more
		// force: an explosion is a spike in the particle count, and a buffer that
		// grew by exactly what was asked would reallocate on every frame of the
		// ramp.
		//
		// **Starting at 4096 rather than at 256**, because an emitter that is
		// emitting at all has hundreds of particles - the smallest useful scene is
		// already past the mesh path's starting size, so starting there would be
		// four reallocations on the first frame anything is drawn.
		uint32_t capacity = ParticleCapacity == 0 ? 4096 : ParticleCapacity;
		while (capacity < count) {
			capacity *= 2;
		}

		if (ParticleBuffer != nullptr) {
			SDL_ReleaseGPUBuffer(Device, ParticleBuffer);
		}

		const uint32_t bytes = capacity * static_cast<uint32_t>(sizeof(effects::ParticleInstance));

		// **Written by the step and read by the draw, and never uploaded to.**
		// There is no transfer buffer beside it any more: the instances are not
		// host data that has to cross, they are what `particle-step.comp`
		// produces. Losing what it holds on a grow costs one frame of particles
		// in a scene that just got bigger, which is why nothing tries to keep it.
		SDL_GPUBufferCreateInfo bufferInfo{};
		bufferInfo.usage = SDL_GPU_BUFFERUSAGE_VERTEX | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE;
		bufferInfo.size = bytes;
		ParticleBuffer = SDL_CreateGPUBuffer(Device, &bufferInfo);

		if (ParticleBuffer == nullptr) {
			ENGINE_ERROR("particle buffer of {} entries: {}", capacity, SDL_GetError());
			ParticleCapacity = 0;
			return false;
		}

		ParticleCapacity = capacity;
		return true;
	}

	bool Renderer::Impl::ReserveParticlePool(uint32_t slots) {
		if (slots == 0) {
			return false;
		}
		if (slots <= Particles.Slots) {
			return true;
		}

		// **Not grown in powers of two, and not grown by much.** The pool is
		// `InstallParticles`' declared capacity and it does not move: the host
		// allocates blocks inside it and their `First` indices are absolute, so a
		// pool sized to anything but the declared number would put a block's run
		// off the end. It is asked for once and answered once.
		if (Particles.States != nullptr) {
			SDL_ReleaseGPUBuffer(Device, Particles.States);
		}

		SDL_GPUBufferCreateInfo info{};
		info.usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE;
		info.size = slots * static_cast<uint32_t>(sizeof(effects::ParticleState));
		Particles.States = SDL_CreateGPUBuffer(Device, &info);
		if (Particles.States == nullptr) {
			ENGINE_ERROR("particle state pool of {} slots: {}", slots, SDL_GetError());
			Particles.Slots = 0;
			return false;
		}

		// **Zeroed, and this is not defensive tidiness.** A fresh GPU buffer holds
		// whatever was in that memory, and the step reads a slot's `Lifetime` to
		// decide whether it holds a particle - so a pool that was never cleared
		// would come up as half a million particles of garbage, at garbage
		// positions and sizes, in the frame before the ring got round to
		// overwriting them. Some drivers hand back zeroed pages and the first
		// version of this looked correct on the machine it was written on.
		//
		// Once per pool rather than per frame, and the pool is created once.
		SDL_GPUTransferBufferCreateInfo blank{};
		blank.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
		blank.size = info.size;
		SDL_GPUTransferBuffer *zeros = SDL_CreateGPUTransferBuffer(Device, &blank);
		SDL_GPUCommandBuffer *command = zeros != nullptr ? SDL_AcquireGPUCommandBuffer(Device) : nullptr;
		if (zeros == nullptr || command == nullptr) {
			ENGINE_ERROR("particle state pool: could not clear {} bytes: {}", info.size, SDL_GetError());
			if (zeros != nullptr) {
				SDL_ReleaseGPUTransferBuffer(Device, zeros);
			}
			SDL_ReleaseGPUBuffer(Device, Particles.States);
			Particles.States = nullptr;
			Particles.Slots = 0;
			return false;
		}

		if (void *mapped = SDL_MapGPUTransferBuffer(Device, zeros, true)) {
			std::memset(mapped, 0, info.size);
			SDL_UnmapGPUTransferBuffer(Device, zeros);
		}

		if (SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(command)) {
			const SDL_GPUTransferBufferLocation source{zeros, 0};
			const SDL_GPUBufferRegion destination{Particles.States, 0, info.size};
			SDL_UploadToGPUBuffer(copy, &source, &destination, false);
			SDL_EndGPUCopyPass(copy);
			(void)SDL_SubmitGPUCommandBuffer(command);
		} else {
			SDL_CancelGPUCommandBuffer(command);
		}

		// **Waited for before the transfer buffer is let go.** The copy reads it,
		// and releasing it while a submitted command buffer still has work
		// against it is a use after free the driver will not warn about.
		SDL_WaitForGPUIdle(Device);
		SDL_ReleaseGPUTransferBuffer(Device, zeros);

		Particles.Slots = slots;
		return true;
	}

	bool Renderer::Impl::ReserveParticleStaging(uint32_t records, uint32_t births, uint32_t seams) {
		// One shape three times over: a read-only storage buffer and the upload
		// transfer beside it, grown in powers of two and never shrunk.
		const auto grow = [this](
							  SDL_GPUBuffer *&buffer,
							  SDL_GPUTransferBuffer *&staging,
							  uint32_t &capacity,
							  uint32_t wanted,
							  uint32_t stride,
							  const char *what
						  ) {
			if (wanted <= capacity) {
				return true;
			}

			uint32_t size = capacity == 0 ? 64 : capacity;
			while (size < wanted) {
				size *= 2;
			}

			if (buffer != nullptr) {
				SDL_ReleaseGPUBuffer(Device, buffer);
			}
			if (staging != nullptr) {
				SDL_ReleaseGPUTransferBuffer(Device, staging);
			}

			SDL_GPUBufferCreateInfo info{};
			info.usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ;
			info.size = size * stride;
			buffer = SDL_CreateGPUBuffer(Device, &info);

			SDL_GPUTransferBufferCreateInfo transfer{};
			transfer.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
			transfer.size = size * stride;
			staging = SDL_CreateGPUTransferBuffer(Device, &transfer);

			if (buffer == nullptr || staging == nullptr) {
				ENGINE_ERROR("particle {} of {} entries: {}", what, size, SDL_GetError());
				capacity = 0;
				return false;
			}

			capacity = size;
			return true;
		};

		const uint32_t word = static_cast<uint32_t>(sizeof(uint32_t));

		// **Always at least one of each, even when the frame has none.** A
		// compute pipeline declares how many storage buffers it reads and every
		// one of them has to be bound; leaving a declared binding empty is a
		// validation error on some drivers and a read of whatever was there on
		// others - the same rule `DrawParticles` follows for its sampler.
		return grow(
				   Particles.Blocks,
				   Particles.BlockStaging,
				   Particles.BlockCapacity,
				   std::max(records, 1u),
				   PARTICLE_BLOCK_WORDS * word,
				   "block records"
			   ) &&
			   grow(
				   Particles.Births,
				   Particles.BirthStaging,
				   Particles.BirthCapacity,
				   std::max(births, 1u),
				   PARTICLE_BIRTH_WORDS * word,
				   "births"
			   ) &&
			   grow(
				   Particles.Seams,
				   Particles.SeamStaging,
				   Particles.SeamCapacity,
				   std::max(seams, 1u),
				   PARTICLE_SEAM_WORDS * word,
				   "seams"
			   );
	}

	void Renderer::Impl::ReleaseParticlePool() {
		for (SDL_GPUBuffer **buffer :
			 {&Particles.States, &Particles.Blocks, &Particles.Births, &Particles.Seams}) {
			if (*buffer != nullptr) {
				SDL_ReleaseGPUBuffer(Device, *buffer);
				*buffer = nullptr;
			}
		}
		for (SDL_GPUTransferBuffer **staging :
			 {&Particles.BlockStaging, &Particles.BirthStaging, &Particles.SeamStaging}) {
			if (*staging != nullptr) {
				SDL_ReleaseGPUTransferBuffer(Device, *staging);
				*staging = nullptr;
			}
		}
		if (Particles.Step != nullptr) {
			SDL_ReleaseGPUComputePipeline(Device, Particles.Step);
			Particles.Step = nullptr;
		}
		if (Particles.Spawn != nullptr) {
			SDL_ReleaseGPUComputePipeline(Device, Particles.Spawn);
			Particles.Spawn = nullptr;
		}
		Particles.Slots = 0;
		Particles.BlockCapacity = 0;
		Particles.BirthCapacity = 0;
		Particles.SeamCapacity = 0;
	}

	bool Renderer::Impl::DispatchParticles() {
		if (Particles.Step == nullptr || Particles.Records == 0) {
			return true;
		}

		ENGINE_PROFILE_CAT("particles.step", core::ProfileCategory::Render);

		SDL_GPUCommandBuffer *command = SDL_AcquireGPUCommandBuffer(Device);
		if (command == nullptr) {
			ENGINE_ERROR("particle step: SDL_AcquireGPUCommandBuffer: {}", SDL_GetError());
			return false;
		}

		// The frame's three uploads, in one copy pass. All three are cycled: this
		// is each buffer's first touch of the frame, so the previous frame's
		// dispatch keeps the version it bound.
		{
			SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(command);
			if (copy == nullptr) {
				ENGINE_ERROR("particle step: SDL_BeginGPUCopyPass: {}", SDL_GetError());
				SDL_CancelGPUCommandBuffer(command);
				return false;
			}

			const auto send = [copy](SDL_GPUTransferBuffer *from, SDL_GPUBuffer *to, uint32_t bytes) {
				if (bytes == 0) {
					return;
				}
				const SDL_GPUTransferBufferLocation source{from, 0};
				const SDL_GPUBufferRegion destination{to, 0, bytes};
				SDL_UploadToGPUBuffer(copy, &source, &destination, true);
			};

			const uint32_t word = static_cast<uint32_t>(sizeof(uint32_t));
			send(Particles.BlockStaging, Particles.Blocks, Particles.Records * PARTICLE_BLOCK_WORDS * word);
			send(
				Particles.BirthStaging, Particles.Births, Particles.BirthCount * PARTICLE_BIRTH_WORDS * word
			);
			send(Particles.SeamStaging, Particles.Seams, Particles.SeamCount * PARTICLE_SEAM_WORDS * word);
			SDL_EndGPUCopyPass(copy);
		}

		// **Spawn first, then step, and in two passes rather than one.** A
		// newborn has to be in the pool before the pass that shades it runs, or
		// its slot draws whatever the ring last left there and the particle is
		// invisible for a frame. Two passes because there is no barrier inside
		// one: the step reads the states the spawn writes.
		//
		// Ageing a newborn by one step on the tick it is born is the one place
		// this differs from the host-side pass, which spawns after ageing. It is
		// one frame of drift on a value that starts at zero.
		if (Particles.Spawn != nullptr && Particles.BirthCount > 0) {
			SDL_GPUStorageBufferReadWriteBinding output{};
			output.buffer = Particles.States;

			SDL_GPUComputePass *pass = SDL_BeginGPUComputePass(command, nullptr, 0, &output, 1);
			if (pass == nullptr) {
				ENGINE_ERROR("particle spawn: SDL_BeginGPUComputePass: {}", SDL_GetError());
				SDL_CancelGPUCommandBuffer(command);
				return false;
			}

			SDL_BindGPUComputePipeline(pass, Particles.Spawn);
			SDL_GPUBuffer *const reads[1] = {Particles.Births};
			SDL_BindGPUComputeStorageBuffers(pass, 0, reads, 1);

			const uint32_t counts[4] = {Particles.BirthCount, Particles.Slots, 0, 0};
			SDL_PushGPUComputeUniformData(command, 0, counts, sizeof(counts));
			SDL_DispatchGPUCompute(pass, (Particles.BirthCount + 63) / 64, 1, 1);
			SDL_EndGPUComputePass(pass);
		}

		{
			SDL_GPUStorageBufferReadWriteBinding outputs[2]{};
			outputs[0].buffer = Particles.States;
			outputs[1].buffer = ParticleBuffer;

			SDL_GPUComputePass *pass = SDL_BeginGPUComputePass(command, nullptr, 0, outputs, 2);
			if (pass == nullptr) {
				ENGINE_ERROR("particle step: SDL_BeginGPUComputePass: {}", SDL_GetError());
				SDL_CancelGPUCommandBuffer(command);
				return false;
			}

			SDL_BindGPUComputePipeline(pass, Particles.Step);
			SDL_GPUBuffer *const reads[2] = {Particles.Blocks, Particles.Seams};
			SDL_BindGPUComputeStorageBuffers(pass, 0, reads, 2);

			const float step[4] = {
				Particles.Delta,
				static_cast<float>(Particles.Records),
				static_cast<float>(Particles.SeamCount),
				0.0f,
			};
			SDL_PushGPUComputeUniformData(command, 0, step, sizeof(step));

			// **One workgroup per block, not per particle.** A slot cannot find
			// its block without a table as large as the pool or a prefix sum over
			// the blocks, and a workgroup already knows which block it is; each
			// one strides through its own capacity.
			SDL_DispatchGPUCompute(pass, Particles.Records, 1, 1);
			SDL_EndGPUComputePass(pass);
		}

		if (!SDL_SubmitGPUCommandBuffer(command)) {
			ENGINE_ERROR("particle step: SDL_SubmitGPUCommandBuffer: {}", SDL_GetError());
			return false;
		}
		return true;
	}

	// The uniforms the particle shaders read. Private, like every other GPU
	// layout in this file.
	namespace {
		struct ParticleUniforms {
			glm::mat4 ViewProjection;
			glm::vec4 CameraRight;
			glm::vec4 CameraUp;
			glm::vec4 CameraForward;

			// x: flipbook side. y: Z offset. z: whether world up is kept.
			// w: unused, named so the struct's size is stated rather than implied.
			glm::vec4 Options;
		};

		struct ParticleMaterial {
			// x: whether the sampler holds this group's texture. y: the blend from
			// alpha to additive. z: environmental-light influence.
			glm::vec4 Flags;

			// The orientation-free light approximation, followed by fog state.
			glm::vec4 Illumination;
			glm::vec4 FogColour;
			glm::vec4 Fog;
			glm::vec4 Eye;
		};

		// The ribbon fragment shader deliberately keeps the older one-vector
		// block. Reusing the larger particle block would push bytes past what that
		// shader declares on backends that validate uniform ranges.
		struct RibbonMaterial {
			glm::vec4 Flags;
		};

		// Whether two batches can be drawn as one call.
		//
		// **Everything that is a uniform or a binding, and nothing that is a
		// vertex attribute.** Two emitters differing only in their particles are
		// one draw; two differing in their texture are two, because a texture is
		// a binding and a binding cannot change inside a draw.
		//
		// `ZOffset` and `FlipbookSide` are uniforms rather than bindings and could
		// have been moved onto the instance instead - four more bytes a particle,
		// which is two megabytes a frame at the target count against a handful of
		// extra draw calls. The draw calls are cheaper.
		bool SameParticleState(const render::ParticleBatch &left, const render::ParticleBatch &right) {
			return left.Additive == right.Additive && left.WorldUp == right.WorldUp &&
				   left.Texture == right.Texture && left.FlipbookSide == right.FlipbookSide &&
				   left.ZOffset == right.ZOffset && left.LightEmission == right.LightEmission &&
				   left.LightInfluence == right.LightInfluence;
		}
	}

	uint32_t Renderer::Impl::PrepareParticles(const render::View &view) {
		ParticleGroups.clear();
		Particles.Records = 0;
		Particles.BirthCount = 0;
		Particles.SeamCount = 0;

		const std::span<const render::ParticleBatch> batches = view.Particles;
		if (ParticlePipeline == nullptr || Particles.Step == nullptr || batches.empty()) {
			return 0;
		}

		// **Grouped by state, and this is the difference between a scene that
		// draws and one that does not.** The first version issued one draw call
		// per emitter, which at the roadmap's hundred thousand emitters is a
		// hundred thousand draw calls a frame - an order of magnitude past what
		// any driver will do at sixty hertz. Measured at 1,600 emitters it was
		// 1,608 draw calls; grouped, the same scene is **three**, because every
		// emitter in the grid shares a texture and a blend mode.
		//
		// **A stable sort into an index list rather than sorting the batches**,
		// because the caller owns them - the same reason `scene::OrderForDrawing`
		// produces an order instead of reordering a draw list.
		ParticleOrder.resize(batches.size());
		for (size_t index = 0; index < batches.size(); index++) {
			ParticleOrder[index] = static_cast<uint32_t>(index);
		}

		// Blended before additive, so the pipeline is bound twice rather than
		// alternating. Within each half the key is arbitrary but must be *total*,
		// or equal states would not end up adjacent.
		//
		// **Timed apart from the copy below**, because the two scale with
		// different things and one bar could not say which had grown: this is
		// proportional to the number of *emitters* and the copy is proportional
		// to the number of *particles*, and a scene can move a long way in one
		// without moving in the other.
		{
			ENGINE_PROFILE_CAT("particles.sort", core::ProfileCategory::Render);
			std::stable_sort(
				ParticleOrder.begin(), ParticleOrder.end(), [batches](uint32_t left, uint32_t right) {
					const render::ParticleBatch &a = batches[left];
					const render::ParticleBatch &b = batches[right];
					if (a.Additive != b.Additive) {
						return !a.Additive;
					}
					if (a.Texture.Id() != b.Texture.Id()) {
						return a.Texture.Id() < b.Texture.Id();
					}
					if (a.FlipbookSide != b.FlipbookSide) {
						return a.FlipbookSide < b.FlipbookSide;
					}
					if (a.ZOffset != b.ZOffset) {
						return a.ZOffset < b.ZOffset;
					}
					if (a.LightEmission != b.LightEmission) {
						return a.LightEmission < b.LightEmission;
					}
					if (a.LightInfluence != b.LightInfluence) {
						return a.LightInfluence < b.LightInfluence;
					}
					return static_cast<int>(a.WorldUp) < static_cast<int>(b.WorldUp);
				}
			);
		}

		// **The whole capacity of every block, not the live count.** The device
		// does not tell the host what died, so there is no live count here to
		// draw - and there is almost nothing to gain from one: `BlockSizeFor` is
		// `Rate * maxLifetime + 1`, so a steady block is full but for a slot.
		// A dead slot's instance is written with a zero `Size`, which
		// `particle.vert` expands into a quad with no extent.
		uint32_t total = 0;
		for (const render::ParticleBatch &batch : batches) {
			if (batch.Block != nullptr) {
				total += batch.Block->Capacity;
			}
		}
		if (total == 0 || !ReserveParticles(total)) {
			return 0;
		}
		if (!ReserveParticlePool(view.ParticlePool)) {
			return 0;
		}
		if (!ReserveParticleStaging(
				static_cast<uint32_t>(batches.size()),
				static_cast<uint32_t>(view.ParticleBirths.size()),
				static_cast<uint32_t>(view.ParticleSeams.size())
			)) {
			return 0;
		}

		auto *records =
			static_cast<uint32_t *>(SDL_MapGPUTransferBuffer(Device, Particles.BlockStaging, true));
		if (records == nullptr) {
			return 0;
		}

		// One walk that both stages the block records and records where each
		// group lands. After this the order is a buffer offset and the batch it
		// came from is gone, which is the same arrangement `SlotMesh` has for the
		// mesh path.
		//
		// **A record per batch in the sorted order**, which is what lets the step
		// dispatch one workgroup per record with no live list: the workgroup
		// index *is* the record index, and the record carries both where its
		// block sits in the pool and where its run lands in the draw stream.
		ParticleGroups.clear();

		uint32_t written = 0;
		uint32_t staged = 0;
		for (const uint32_t index : ParticleOrder) {
			const render::ParticleBatch &batch = batches[index];
			if (batch.Block == nullptr || batch.Block->Capacity == 0) {
				continue;
			}
			const effects::EmitterBlock &block = *batch.Block;

			if (ParticleGroups.empty() || !SameParticleState(batches[ParticleGroups.back().Batch], batch)) {
				ParticleGroups.push_back(ParticleGroup{index, written, 0});
			}

			WriteParticleBlock(records + staged * PARTICLE_BLOCK_WORDS, block, written);
			staged++;

			written += block.Capacity;
			ParticleGroups.back().Count += block.Capacity;
		}

		SDL_UnmapGPUTransferBuffer(Device, Particles.BlockStaging);
		Particles.Records = staged;
		Particles.Delta = view.ParticleDelta;

		// The births and the panes, both small and both straight copies - a birth
		// is sixty bytes and a scene has thousands of them against half a million
		// particles, and a pane is eighty and a scene has none.
		Particles.BirthCount = 0;
		if (!view.ParticleBirths.empty()) {
			auto *births =
				static_cast<uint32_t *>(SDL_MapGPUTransferBuffer(Device, Particles.BirthStaging, true));
			if (births != nullptr) {
				for (size_t at = 0; at < view.ParticleBirths.size(); at++) {
					const render::ParticleBirth &birth = view.ParticleBirths[at];
					uint32_t *const row = births + at * PARTICLE_BIRTH_WORDS;
					row[0] = birth.Row;
					std::memcpy(row + 1, &birth.State, sizeof(effects::ParticleState));
				}
				SDL_UnmapGPUTransferBuffer(Device, Particles.BirthStaging);
				Particles.BirthCount = static_cast<uint32_t>(view.ParticleBirths.size());
			}
		}

		Particles.SeamCount = 0;
		if (!view.ParticleSeams.empty()) {
			auto *seams = static_cast<float *>(SDL_MapGPUTransferBuffer(Device, Particles.SeamStaging, true));
			if (seams != nullptr) {
				for (size_t at = 0; at < view.ParticleSeams.size(); at++) {
					WriteParticleSeam(seams + at * PARTICLE_SEAM_WORDS, view.ParticleSeams[at]);
				}
				SDL_UnmapGPUTransferBuffer(Device, Particles.SeamStaging);
				Particles.SeamCount = static_cast<uint32_t>(view.ParticleSeams.size());
			}
		}

		// **Stepped here rather than by the caller**, because the destinations
		// this walk just decided are what the step writes to: the two cannot be
		// separated without the block records crossing twice.
		const bool restage =
			batches.data() != Particles.StagedFrom || batches.size() != Particles.StagedCount;
		if (restage || view.ParticleDelta > 0.0f) {
			(void)DispatchParticles();
			Particles.StagedFrom = batches.data();
			Particles.StagedCount = batches.size();
		}

		return written;
	}

	uint32_t Renderer::Impl::DrawParticles(
		SDL_GPUCommandBuffer *command,
		SDL_GPURenderPass *pass,
		const glm::mat4 &viewProjection,
		const core::CFrame &eye,
		std::span<const render::ParticleBatch> batches,
		uint64_t &triangles
	) {
		if (ParticlePipeline == nullptr || ParticleGroups.empty()) {
			return 0;
		}

		// The camera's axes, once for the frame rather than once per group: a
		// billboard is turned by the same three vectors whatever emitter it came
		// from.
		const auto axis = [&eye](float x, float y, float z) {
			const core::Vector3 world = eye.VectorToWorldSpace(core::Vector3{x, y, z});
			return glm::vec4{world.X, world.Y, world.Z, 0.0f};
		};

		ParticleUniforms uniforms{};
		uniforms.ViewProjection = viewProjection;
		uniforms.CameraRight = axis(1.0f, 0.0f, 0.0f);
		uniforms.CameraUp = axis(0.0f, 1.0f, 0.0f);

		// **The forward the shader wants points from the eye into the scene**, and
		// `CFrame`'s own forward is -Z - the engine's camera convention, which
		// `scene::NormalOf` states. Taken here rather than negated in the shader,
		// so the convention lives in one place.
		uniforms.CameraForward = axis(0.0f, 0.0f, -1.0f);

		SDL_GPUBufferBinding vertexBinding{};
		vertexBinding.buffer = ParticleBuffer;
		vertexBinding.offset = 0;
		SDL_BindGPUVertexBuffers(pass, 0, &vertexBinding, 1);

		uint32_t draws = 0;
		bool additiveBound = false;
		bool blendedBound = false;

		for (const ParticleGroup &group : ParticleGroups) {
			const render::ParticleBatch &state = batches[group.Batch];

			// **Bound once per pipeline rather than once per group**, which the
			// sort is what makes possible: every blended group precedes every
			// additive one, so each pipeline is bound the first time it is
			// reached and never again.
			if (state.Additive) {
				if (AdditiveParticlePipeline == nullptr) {
					continue;
				}
				if (!additiveBound) {
					BindPipeline(pass, AdditiveParticlePipeline, PipelineFamily::Other);
					additiveBound = true;
				}
			} else if (!blendedBound) {
				BindPipeline(pass, ParticlePipeline, PipelineFamily::Other);
				blendedBound = true;
			}

			uniforms.Options = glm::vec4{
				state.FlipbookSide <= 0.0f ? 1.0f : state.FlipbookSide,
				state.ZOffset,
				state.WorldUp ? 1.0f : 0.0f,
				0.0f,
			};
			SDL_PushGPUVertexUniformData(command, 0, &uniforms, sizeof(uniforms));

			SDL_GPUTexture *const texture = Textures.Find(state.Texture);

			ParticleMaterial material{};
			material.Flags = glm::vec4{
				texture != nullptr ? 1.0f : 0.0f,
				state.Additive ? 1.0f : std::clamp(state.LightEmission, 0.0f, 1.0f),
				std::clamp(state.LightInfluence, 0.0f, 1.0f),
				0.0f,
			};
			material.Illumination = Ambient + OutdoorAmbient * 0.5f + Direct * 0.5f;
			material.FogColour = FogColour;
			material.Fog = glm::vec4{FogStart, FogEnd, 0.0f, 0.0f};
			material.Eye = glm::vec4{eye.Position.X, eye.Position.Y, eye.Position.Z, 0.0f};
			SDL_PushGPUFragmentUniformData(command, 0, &material, sizeof(material));

			// **The fallback is bound rather than the sampler left unbound**,
			// which is the rule `DrawSlots` follows: a shader declares a sampler
			// whether or not a draw has a texture for it, and leaving it unbound
			// is a validation error on some drivers and a read of whatever was
			// there on others. The uniform above decides whether it is used.
			SDL_GPUTextureSamplerBinding binding{};
			binding.texture = texture != nullptr ? texture : FallbackTexture;
			binding.sampler = Textures.Sampler();
			SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);

			// Four vertices a particle, as a strip. `first_instance` selects this
			// group's slice of the shared buffer.
			SDL_DrawGPUPrimitives(pass, 4, group.Count, 0, group.First);
			draws++;

			// **Two triangles a particle, counted here.** A four-vertex strip is
			// two triangles and `group.Count` of them are drawn, so this is the
			// whole arithmetic - but it was not being done at all, and the
			// omission read as a broken renderer rather than as a missing sum. A
			// frame drawing half a million particle quads reported "108
			// triangle(s)", which is a number small enough to look like nothing
			// had been submitted; the particles were on screen the whole time.
			triangles += static_cast<uint64_t>(group.Count) * 2;
		}

		return draws;
	}

	bool Renderer::Impl::ReserveRibbons(uint32_t count) {
		if (count <= RibbonCapacity) {
			return true;
		}

		// Powers of two from 1024, which is about sixty beams' worth. Smaller
		// than the particle buffer's floor because a ribbon count is bounded by
		// how many beams an author placed rather than by a rate.
		uint32_t capacity = RibbonCapacity == 0 ? 1024 : RibbonCapacity;
		while (capacity < count) {
			capacity *= 2;
		}

		if (RibbonBuffer != nullptr) {
			SDL_ReleaseGPUBuffer(Device, RibbonBuffer);
		}
		if (RibbonTransfer != nullptr) {
			SDL_ReleaseGPUTransferBuffer(Device, RibbonTransfer);
		}

		const uint32_t bytes = capacity * static_cast<uint32_t>(sizeof(effects::RibbonVertex));

		SDL_GPUBufferCreateInfo bufferInfo{};
		bufferInfo.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
		bufferInfo.size = bytes;
		RibbonBuffer = SDL_CreateGPUBuffer(Device, &bufferInfo);

		SDL_GPUTransferBufferCreateInfo transferInfo{};
		transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
		transferInfo.size = bytes;
		RibbonTransfer = SDL_CreateGPUTransferBuffer(Device, &transferInfo);

		if (RibbonBuffer == nullptr || RibbonTransfer == nullptr) {
			ENGINE_ERROR("ribbon buffer of {} vertices: {}", capacity, SDL_GetError());
			RibbonCapacity = 0;
			return false;
		}

		RibbonCapacity = capacity;
		return true;
	}

	uint32_t Renderer::Impl::PrepareRibbons(std::span<const effects::RibbonVertex> vertices) {
		if (RibbonPipeline == nullptr || vertices.empty()) {
			return 0;
		}

		const auto count = static_cast<uint32_t>(vertices.size());
		if (!ReserveRibbons(count)) {
			return 0;
		}

		// **Copied whole rather than run by run**, because `RibbonRun::First` is
		// already an index into this stream - `BuildRibbons` packed the runs
		// contiguously in the order it produced them, so the buffer and the runs
		// agree with no repacking.
		auto *mapped =
			static_cast<effects::RibbonVertex *>(SDL_MapGPUTransferBuffer(Device, RibbonTransfer, true));
		if (mapped == nullptr) {
			return 0;
		}
		std::memcpy(mapped, vertices.data(), count * sizeof(effects::RibbonVertex));
		SDL_UnmapGPUTransferBuffer(Device, RibbonTransfer);

		return count;
	}

	namespace {
		struct RibbonUniforms {
			glm::mat4 ViewProjection;
			glm::vec4 CameraForward;

			// x: Z offset. The rest is named so the struct's size is stated.
			glm::vec4 Options;
		};
	}

	uint32_t Renderer::Impl::DrawRibbons(
		SDL_GPUCommandBuffer *command,
		SDL_GPURenderPass *pass,
		const glm::mat4 &viewProjection,
		const core::CFrame &eye,
		std::span<const effects::RibbonRun> runs,
		uint64_t &triangles
	) {
		if (RibbonPipeline == nullptr || runs.empty()) {
			return 0;
		}

		const core::Vector3 forward = eye.VectorToWorldSpace(core::Vector3{0.0f, 0.0f, -1.0f});

		RibbonUniforms uniforms{};
		uniforms.ViewProjection = viewProjection;
		uniforms.CameraForward = glm::vec4{forward.X, forward.Y, forward.Z, 0.0f};

		SDL_GPUBufferBinding vertexBinding{};
		vertexBinding.buffer = RibbonBuffer;
		vertexBinding.offset = 0;
		SDL_BindGPUVertexBuffers(pass, 0, &vertexBinding, 1);

		uint32_t draws = 0;
		bool blendedBound = false;
		bool additiveBound = false;

		// **Two passes over the runs rather than one**, so each pipeline is bound
		// once. `BuildRibbons` produces beams then trails in world order and does
		// not group by blend mode - grouping there would be a shared module
		// ordering work for a pipeline it cannot name.
		for (int additive = 0; additive < 2; additive++) {
			for (const effects::RibbonRun &run : runs) {
				if (run.Additive != (additive == 1) || run.Count < 4) {
					continue;
				}

				if (run.Additive) {
					if (AdditiveRibbonPipeline == nullptr) {
						continue;
					}
					if (!additiveBound) {
						BindPipeline(pass, AdditiveRibbonPipeline, PipelineFamily::Other);
						additiveBound = true;
					}
				} else if (!blendedBound) {
					BindPipeline(pass, RibbonPipeline, PipelineFamily::Other);
					blendedBound = true;
				}

				uniforms.Options = glm::vec4{run.ZOffset, 0.0f, 0.0f, 0.0f};
				SDL_PushGPUVertexUniformData(command, 0, &uniforms, sizeof(uniforms));

				SDL_GPUTexture *const texture = Textures.Find(run.Texture);

				RibbonMaterial material{};
				material.Flags = glm::vec4{texture != nullptr ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f};
				SDL_PushGPUFragmentUniformData(command, 0, &material, sizeof(material));

				// The fallback is bound rather than the sampler left unbound, for
				// `DrawParticles`'s reason.
				SDL_GPUTextureSamplerBinding binding{};
				binding.texture = texture != nullptr ? texture : FallbackTexture;
				binding.sampler = Textures.Sampler();
				SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);

				// A strip, so the vertex count is the run's own and `first_vertex`
				// selects its slice. No instancing: one run is one ribbon.
				SDL_DrawGPUPrimitives(pass, run.Count, 1, run.First, 0);
				draws++;

				// A strip of `n` vertices is `n - 2` triangles, and the `< 4`
				// guard above means this is never negative. Counted for
				// `DrawParticles`' reason.
				triangles += static_cast<uint64_t>(run.Count) - 2;
			}
		}

		return draws;
	}

	bool Renderer::Impl::EnsureScene(uint32_t width, uint32_t height) {
		SceneSlot &target = SlotAt(ActiveSlot);

		// **Recorded before anything decides whether to allocate.** This is the
		// rectangle the pass draws and the rectangle `SceneTextureExtent`
		// reports, and it is the panel's size whether or not the texture under
		// it changed - which is the whole point of keeping the two apart.
		target.DrawnWidth = width;
		target.DrawnHeight = height;

		if (width == 0 || height == 0) {
			// Nobody wants a picture. Retired rather than released for the
			// reason below: an interface hook may already have recorded a bind
			// of it for the frame in progress.
			if (target.Texture) {
				RetiredScenes.push_back(target.Texture);
				target.Texture = nullptr;
			}

			// **Released outright rather than retired.** Nothing samples a depth
			// buffer - no interface hook can have recorded a bind of it - so the
			// grace period the colour target needs does not apply, and a closed
			// panel should not go on holding a megabyte of it.
			if (target.Depth) {
				SDL_ReleaseGPUTexture(Device, target.Depth);
				target.Depth = nullptr;
			}

			target.Width = 0;
			target.Height = 0;
			target.DepthWidth = 0;
			target.DepthHeight = 0;
			return false;
		}

		const uint32_t wantWidth = BlockUp(width);
		const uint32_t wantHeight = BlockUp(height);

		// **Kept when it is big enough, and only replaced when it is far too
		// big.** Growing is forced - a texture smaller than the rectangle would
		// clip the world - but shrinking is not, and refusing to shrink for a
		// factor of two is what stops a drag from reallocating on the way back
		// down as well as on the way up. See `SCENE_TARGET_BLOCK`.
		if (target.Texture && wantWidth <= target.Width && wantHeight <= target.Height) {
			const bool wasteful = target.Width >= wantWidth * 2 || target.Height >= wantHeight * 2;
			if (!wasteful) {
				return true;
			}
		}

		if (target.Texture) {
			// **Retired rather than released.** An interface hook has already
			// recorded a bind of this texture for the frame in progress - that
			// is what "the image is last frame's texture" means - so freeing it
			// here is a use-after-free that lands inside SDL's Vulkan backend.
			// `DrainRetiredScenes` frees it at the top of the next frame.
			RetiredScenes.push_back(target.Texture);
			target.Texture = nullptr;
		}

		target.Width = wantWidth;
		target.Height = wantHeight;

		SDL_GPUTextureCreateInfo info{};
		info.type = SDL_GPU_TEXTURETYPE_2D;

		// **The swapchain's format, and that is a requirement rather than a
		// convenience.** The opaque and transparent pipelines were built with
		// the swapchain's colour format; a target in another format is a
		// validation error at bind time on the backends that check and a
		// corrupt image on the ones that do not.
		info.format = ColourFormat();

		// Sampled as well as drawn into, because the whole point is that
		// something shows it afterwards.
		info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;

		info.width = wantWidth;
		info.height = wantHeight;
		info.layer_count_or_depth = 1;
		info.num_levels = 1;

		target.Texture = SDL_CreateGPUTexture(Device, &info);
		if (!target.Texture) {
			ENGINE_ERROR("SDL_CreateGPUTexture (scene target): {}", SDL_GetError());
			target.Width = 0;
			target.Height = 0;

			// The drawn rectangle goes with it. Leaving it set would have
			// `SceneTextureExtent` divide by a texture that does not exist.
			target.DrawnWidth = 0;
			target.DrawnHeight = 0;
			return false;
		}
		return true;
	}

	bool Renderer::Impl::EnsureHistory(size_t slot, uint32_t width, uint32_t height) {
		SceneSlot &history = SlotAt(slot);
		if (history.History != nullptr && history.HistoryWidth == width && history.HistoryHeight == height) {
			return true;
		}

		if (history.History != nullptr) {
			// The texture may still be sampled by work submitted for the previous
			// frame. Retiring it follows the same frame-boundary ownership rule as
			// a resized viewport target.
			RetiredScenes.push_back(history.History);
			history.History = nullptr;
		}
		history.HistoryWidth = 0;
		history.HistoryHeight = 0;
		history.HistoryReady = false;
		if (width == 0 || height == 0) {
			return false;
		}

		SDL_GPUTextureCreateInfo info{};
		info.type = SDL_GPU_TEXTURETYPE_2D;
		info.format = ColourFormat();
		info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
		info.width = width;
		info.height = height;
		info.layer_count_or_depth = 1;
		info.num_levels = 1;
		info.sample_count = SDL_GPU_SAMPLECOUNT_1;
		history.History = SDL_CreateGPUTexture(Device, &info);
		if (history.History == nullptr) {
			ENGINE_ERROR("SDL_CreateGPUTexture (frame history): {}", SDL_GetError());
			return false;
		}
		history.HistoryWidth = width;
		history.HistoryHeight = height;
		return true;
	}

	bool Renderer::Impl::EnsureDepthIn(
		SDL_GPUTexture *&texture, uint32_t &haveWidth, uint32_t &haveHeight, uint32_t width, uint32_t height
	) {
		if (texture && width == haveWidth && height == haveHeight) {
			return true;
		}

		if (texture) {
			SDL_ReleaseGPUTexture(Device, texture);
			texture = nullptr;
		}

		SDL_GPUTextureCreateInfo info{};
		info.type = SDL_GPU_TEXTURETYPE_2D;
		info.format = DepthFormat;
		// Deferred depth-linearisation samples the same depth the geometry pass
		// writes. The chosen format was probed for both usages at initialisation.
		info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
		info.width = width;
		info.height = height;
		info.layer_count_or_depth = 1;
		info.num_levels = 1;
		info.sample_count = SDL_GPU_SAMPLECOUNT_1;

		texture = SDL_CreateGPUTexture(Device, &info);
		if (!texture) {
			ENGINE_ERROR("depth texture {}x{}: {}", width, height, SDL_GetError());
			haveWidth = 0;
			haveHeight = 0;
			return false;
		}

		haveWidth = width;
		haveHeight = height;
		return true;
	}

	bool Renderer::Impl::EnsureDepth(uint32_t width, uint32_t height) {
		return EnsureDepthIn(DepthTexture, DepthWidth, DepthHeight, width, height);
	}

	void Renderer::Impl::ReleasePbr(PbrSlot &slot) {
		for (SDL_GPUTexture *texture :
			 {slot.Albedo,
			  slot.Normal,
			  slot.Material,
			  slot.Emissive,
			  slot.LinearDepth,
			  slot.Occlusion,
			  slot.Lit}) {
			if (texture != nullptr) {
				SDL_ReleaseGPUTexture(Device, texture);
			}
		}
		slot = {};
	}

	bool Renderer::Impl::EnsurePbr(size_t index, const PbrDimensions &dimensions) {
		PbrSlot &slot = PbrAt(index);
		if (slot.Albedo != nullptr && slot.Dimensions == dimensions) {
			return true;
		}
		if (dimensions.TargetWidth == 0 || dimensions.TargetHeight == 0 || dimensions.ViewWidth == 0 ||
			dimensions.ViewHeight == 0 || dimensions.LinearWidth == 0 || dimensions.LinearHeight == 0 ||
			dimensions.OcclusionWidth == 0 || dimensions.OcclusionHeight == 0 || dimensions.LitWidth == 0 ||
			dimensions.LitHeight == 0 || !EnsureSurfaceSampler()) {
			return false;
		}

		PbrSlot made;
		made.Dimensions = dimensions;
		const auto texture = [&](SDL_GPUTextureFormat format, uint32_t textureWidth, uint32_t textureHeight) {
			SDL_GPUTextureCreateInfo info{};
			info.type = SDL_GPU_TEXTURETYPE_2D;
			info.format = format;
			info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
			info.width = textureWidth;
			info.height = textureHeight;
			info.layer_count_or_depth = 1;
			info.num_levels = 1;
			info.sample_count = SDL_GPU_SAMPLECOUNT_1;
			return SDL_CreateGPUTexture(Device, &info);
		};

		made.Albedo = texture(
			SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB, dimensions.TargetWidth, dimensions.TargetHeight
		);
		made.Normal =
			texture(SDL_GPU_TEXTUREFORMAT_R10G10B10A2_UNORM, dimensions.TargetWidth, dimensions.TargetHeight);
		made.Material =
			texture(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, dimensions.TargetWidth, dimensions.TargetHeight);
		made.Emissive = texture(
			SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, dimensions.TargetWidth, dimensions.TargetHeight
		);
		made.LinearDepth =
			texture(SDL_GPU_TEXTUREFORMAT_R32_FLOAT, dimensions.LinearWidth, dimensions.LinearHeight);
		made.Occlusion =
			texture(SDL_GPU_TEXTUREFORMAT_R8_UNORM, dimensions.OcclusionWidth, dimensions.OcclusionHeight);
		made.Lit =
			texture(SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, dimensions.LitWidth, dimensions.LitHeight);

		if (made.Albedo == nullptr || made.Normal == nullptr || made.Material == nullptr ||
			made.Emissive == nullptr || made.LinearDepth == nullptr || made.Occlusion == nullptr ||
			made.Lit == nullptr) {
			ENGINE_ERROR(
				"render graph targets for {}x{} view: {}",
				dimensions.ViewWidth,
				dimensions.ViewHeight,
				SDL_GetError()
			);
			ReleasePbr(made);
			return slot.Albedo != nullptr;
		}

		ReleasePbr(slot);
		slot = made;
		return true;
	}

	graph::NodeScope
	Renderer::Impl::ResourceScope(const NamedPipeline &pipeline, graph::ResourceId resource) const {
		graph::NodeScope found = graph::NodeScope::Frame;
		for (uint32_t value = 1; value <= pipeline.Graph.Count(); value++) {
			const graph::Node *node = pipeline.Graph.Find(graph::NodeId{value});
			if (node == nullptr ||
				std::find(node->Writes.begin(), node->Writes.end(), resource) == node->Writes.end()) {
				continue;
			}
			if (node->Scope == graph::NodeScope::View) {
				return graph::NodeScope::View;
			}
			if (node->Scope == graph::NodeScope::World) {
				found = graph::NodeScope::World;
			}
		}
		return found;
	}

	Renderer::Impl::NamedTexture Renderer::Impl::FindGraphTarget(
		const NamedPipeline &pipeline, core::Name resource, graph::NodeScope scope, uint64_t owner
	) const {
		for (const GraphTarget &target : GraphTargets) {
			if (target.Pipeline == pipeline.Name && target.Resource == resource && target.Scope == scope &&
				target.Owner == owner) {
				return NamedTexture{target.Texture, target.Width, target.Height, target.Format};
			}
		}
		return {};
	}

	Renderer::Impl::NamedTexture Renderer::Impl::EnsureGraphTarget(
		const NamedPipeline &pipeline,
		graph::ResourceId resource,
		uint64_t owner,
		uint32_t viewWidth,
		uint32_t viewHeight
	) {
		const graph::ResourceDesc *desc = pipeline.Graph.FindResource(resource);
		if (desc == nullptr || desc->Kind == graph::ResourceKind::Texture ||
			desc->Kind == graph::ResourceKind::Buffer || desc->Kind == graph::ResourceKind::Camera ||
			desc->Kind == graph::ResourceKind::Entities) {
			return {};
		}

		bool presentationImage = false;
		for (uint32_t value = 1; value <= pipeline.Graph.Count(); value++) {
			const graph::Node *writer = pipeline.Graph.Find(graph::NodeId{value});
			if (writer == nullptr ||
				std::find(writer->Writes.begin(), writer->Writes.end(), resource) == writer->Writes.end()) {
				continue;
			}
			presentationImage = writer->Kind == core::Name("present") ||
								writer->Kind == core::Name("interface") ||
								writer->Kind == core::Name("overlay");
			break;
		}
		if (desc->External && !presentationImage) {
			return {};
		}
		const SDL_GPUTextureFormat format = presentationImage ? ColourFormat() : DeviceFormat(desc->Format);
		if (format == SDL_GPU_TEXTUREFORMAT_INVALID) {
			ENGINE_WARN("graph resource '{}' uses a format the renderer cannot allocate", desc->Name.Text());
			return {};
		}

		uint32_t width = 0;
		uint32_t height = 0;
		desc->Resolve(viewWidth, viewHeight, width, height);
		const graph::NodeScope scope = ResourceScope(pipeline, resource);
		for (GraphTarget &target : GraphTargets) {
			if (target.Pipeline != pipeline.Name || target.Resource != desc->Name || target.Scope != scope ||
				target.Owner != owner) {
				continue;
			}
			if (target.Texture != nullptr && target.Format == format && target.Width == width &&
				target.Height == height) {
				return NamedTexture{target.Texture, width, height, format};
			}
			if (target.Texture != nullptr) {
				SDL_ReleaseGPUTexture(Device, target.Texture);
				target.Texture = nullptr;
			}

			SDL_GPUTextureCreateInfo info{};
			info.type = SDL_GPU_TEXTURETYPE_2D;
			info.format = format;
			info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
			if (desc->Kind == graph::ResourceKind::Storage) {
				info.usage |= SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
			} else if (desc->Kind == graph::ResourceKind::Depth) {
				info.usage |= SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
			} else {
				info.usage |= SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
			}
			info.width = width;
			info.height = height;
			info.layer_count_or_depth = 1;
			info.num_levels = 1;
			info.sample_count = SDL_GPU_SAMPLECOUNT_1;
			target.Texture = SDL_CreateGPUTexture(Device, &info);
			target.Format = format;
			target.Width = width;
			target.Height = height;
			if (target.Texture == nullptr) {
				ENGINE_ERROR("graph target '{}': {}", desc->Name.Text(), SDL_GetError());
				return {};
			}
			return NamedTexture{target.Texture, width, height, format};
		}

		GraphTargets.push_back(GraphTarget{pipeline.Name, desc->Name, scope, owner});
		return EnsureGraphTarget(pipeline, resource, owner, viewWidth, viewHeight);
	}

	bool Renderer::Impl::GraphShaderCode(
		const graph::Node &node, ShaderStage stage, std::vector<uint8_t> &bytes, std::string &entryPoint
	) const {
		bytes.clear();
		entryPoint = Binary.EntryPoint;
		const std::string *source = node.Parameter(core::Name("source"));
		if (source == nullptr || source->empty()) {
			const std::string *shader = node.Parameter(core::Name("shader"));
			if (shader == nullptr || shader->empty()) {
				ENGINE_WARN("'{}' has no shader or GLSL source, so it records no work", node.Name.Text());
				return false;
			}
			bytes = ReadFile(resources::Shader(*shader, Binary.Form));
			if (bytes.empty()) {
				ENGINE_WARN("'{}' names shader '{}' but no staged module exists", node.Name.Text(), *shader);
				return false;
			}
			return true;
		}

		ShaderCompiler compiler;
		const ShaderCompilation compiled = compiler.Compile(*source, stage, node.Name.Text());
		if (compiled.Failed) {
			ENGINE_WARN("shader for '{}': {}", node.Name.Text(), compiled.Error);
			return false;
		}

		if (Binary.Form == resources::ShaderForm::Msl) {
			msl::Translation translated = msl::Translate(compiled.SpirV);
			if (translated.Failed) {
				ENGINE_WARN(
					"shader for '{}' cannot be translated to MSL: {}", node.Name.Text(), translated.Error
				);
				return false;
			}
			bytes.assign(translated.Source.begin(), translated.Source.end());
			return true;
		}

		const auto *first = reinterpret_cast<const uint8_t *>(compiled.SpirV.data());
		bytes.assign(first, first + compiled.SpirV.size() * sizeof(uint32_t));
		return true;
	}

	SDL_GPUGraphicsPipeline *Renderer::Impl::GraphRasterFor(
		const NamedPipeline &pipeline, const graph::Node &node, SDL_GPUTextureFormat format, uint32_t samplers
	) {
		for (const GraphRasterPipeline &entry : GraphRasterPipelines) {
			if (entry.Pipeline == pipeline.Name && entry.Node == node.Name && entry.Format == format &&
				entry.Samplers == samplers) {
				return entry.Handle;
			}
		}

		SDL_GPUShader *vertex = LoadShader("overlay.vert", SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
		std::vector<uint8_t> bytes;
		std::string entryPoint;
		SDL_GPUShader *fragment = nullptr;
		if (GraphShaderCode(node, ShaderStage::Fragment, bytes, entryPoint)) {
			SDL_GPUShaderCreateInfo shader{};
			shader.code = bytes.data();
			shader.code_size = bytes.size();
			shader.entrypoint = entryPoint.c_str();
			shader.format = Binary.Format;
			shader.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
			shader.num_samplers = samplers;
			shader.num_uniform_buffers = 1;
			fragment = SDL_CreateGPUShader(Device, &shader);
			if (fragment == nullptr) {
				ENGINE_WARN("fragment shader for '{}': {}", node.Name.Text(), SDL_GetError());
			}
		}

		SDL_GPUGraphicsPipeline *built = nullptr;
		if (vertex != nullptr && fragment != nullptr) {
			SDL_GPUColorTargetDescription target{};
			target.format = format;
			SDL_GPUGraphicsPipelineCreateInfo info{};
			info.vertex_shader = vertex;
			info.fragment_shader = fragment;
			info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
			info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
			info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
			info.target_info.color_target_descriptions = &target;
			info.target_info.num_color_targets = 1;
			built = SDL_CreateGPUGraphicsPipeline(Device, &info);
			if (built == nullptr) {
				ENGINE_WARN("raster pipeline for '{}': {}", node.Name.Text(), SDL_GetError());
			}
		}
		if (vertex != nullptr) {
			SDL_ReleaseGPUShader(Device, vertex);
		}
		if (fragment != nullptr) {
			SDL_ReleaseGPUShader(Device, fragment);
		}
		GraphRasterPipelines.push_back({pipeline.Name, node.Name, format, samplers, built});
		return built;
	}

	SDL_GPUComputePipeline *Renderer::Impl::GraphComputeFor(
		const NamedPipeline &pipeline,
		const graph::Node &node,
		uint32_t samplers,
		uint32_t storage,
		uint32_t localX,
		uint32_t localY,
		uint32_t localZ
	) {
		for (const GraphComputePipeline &entry : GraphComputePipelines) {
			if (entry.Pipeline == pipeline.Name && entry.Node == node.Name && entry.Samplers == samplers &&
				entry.Storage == storage && entry.LocalX == localX && entry.LocalY == localY &&
				entry.LocalZ == localZ) {
				return entry.Handle;
			}
		}

		std::vector<uint8_t> bytes;
		std::string entryPoint;
		SDL_GPUComputePipeline *built = nullptr;
		if (GraphShaderCode(node, ShaderStage::Compute, bytes, entryPoint)) {
			SDL_GPUComputePipelineCreateInfo info{};
			info.code = bytes.data();
			info.code_size = bytes.size();
			info.entrypoint = entryPoint.c_str();
			info.format = Binary.Format;
			info.num_samplers = samplers;
			info.num_readwrite_storage_textures = storage;
			info.threadcount_x = localX;
			info.threadcount_y = localY;
			info.threadcount_z = localZ;
			built = SDL_CreateGPUComputePipeline(Device, &info);
			if (built == nullptr) {
				ENGINE_WARN("compute pipeline for '{}': {}", node.Name.Text(), SDL_GetError());
			}
		}
		GraphComputePipelines.push_back(
			{pipeline.Name, node.Name, samplers, storage, localX, localY, localZ, built}
		);
		return built;
	}

	void Renderer::Impl::ReleaseGraphState(core::Name pipeline) {
		std::erase_if(GraphCaptureReceipts, [pipeline](const GraphCaptureReceipt &receipt) {
			return receipt.Pipeline == pipeline;
		});
		for (size_t index = GraphTargets.size(); index > 0; index--) {
			GraphTarget &target = GraphTargets[index - 1];
			if (target.Pipeline != pipeline) {
				continue;
			}
			if (target.Texture != nullptr && Device != nullptr) {
				SDL_ReleaseGPUTexture(Device, target.Texture);
			}
			GraphTargets.erase(GraphTargets.begin() + static_cast<ptrdiff_t>(index - 1));
		}
		for (size_t index = GraphRasterPipelines.size(); index > 0; index--) {
			GraphRasterPipeline &entry = GraphRasterPipelines[index - 1];
			if (entry.Pipeline != pipeline) {
				continue;
			}
			if (entry.Handle != nullptr && Device != nullptr) {
				SDL_ReleaseGPUGraphicsPipeline(Device, entry.Handle);
			}
			GraphRasterPipelines.erase(GraphRasterPipelines.begin() + static_cast<ptrdiff_t>(index - 1));
		}
		for (size_t index = GraphComputePipelines.size(); index > 0; index--) {
			GraphComputePipeline &entry = GraphComputePipelines[index - 1];
			if (entry.Pipeline != pipeline) {
				continue;
			}
			if (entry.Handle != nullptr && Device != nullptr) {
				SDL_ReleaseGPUComputePipeline(Device, entry.Handle);
			}
			GraphComputePipelines.erase(GraphComputePipelines.begin() + static_cast<ptrdiff_t>(index - 1));
		}
	}

	void Renderer::Impl::ReleaseAllGraphState() {
		while (!GraphTargets.empty()) {
			ReleaseGraphState(GraphTargets.back().Pipeline);
		}
		while (!GraphRasterPipelines.empty()) {
			ReleaseGraphState(GraphRasterPipelines.back().Pipeline);
		}
		while (!GraphComputePipelines.empty()) {
			ReleaseGraphState(GraphComputePipelines.back().Pipeline);
		}
		GraphCaptureReceipts.clear();
		GraphCaptureFrames.clear();
	}

	bool Renderer::Impl::EnsureBeams() {
		if (BeamTexture != nullptr) {
			return true;
		}

		// The shadow map's own sampler and format, so the two are read the same
		// way - and `EnsureShadow` is what makes both.
		if (!EnsureShadow()) {
			return false;
		}

		SDL_GPUTextureCreateInfo info{};
		info.type = SDL_GPU_TEXTURETYPE_2D;
		info.format = DepthFormat;
		info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;

		// **The world map's resolution for the whole atlas**, so one beam gets a
		// quarter of it in each direction. A beam covers one doorway rather than
		// a scene, so a quarter of the texels over a hundredth of the area is
		// several times the density the world map has.
		info.width = SHADOW_RESOLUTION;
		info.height = SHADOW_RESOLUTION;
		info.layer_count_or_depth = 1;
		info.num_levels = 1;
		info.sample_count = SDL_GPU_SAMPLECOUNT_1;

		BeamTexture = SDL_CreateGPUTexture(Device, &info);
		if (!BeamTexture) {
			ENGINE_ERROR("portal beam texture: {}", SDL_GetError());
			return false;
		}
		return true;
	}

	bool Renderer::Impl::EnsureShadow() {
		if (ShadowTexture != nullptr) {
			return true;
		}

		SDL_GPUTextureCreateInfo info{};
		info.type = SDL_GPU_TEXTURETYPE_2D;
		info.format = DepthFormat;

		// **Both usages, and the sampler one is the point.** A depth attachment
		// that is only a target cannot be read, and a shadow map that cannot be
		// read is a pass that costs a draw and changes nothing.
		info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
		info.width = SHADOW_RESOLUTION;
		info.height = SHADOW_RESOLUTION;
		info.layer_count_or_depth = 1;
		info.num_levels = 1;
		info.sample_count = SDL_GPU_SAMPLECOUNT_1;

		ShadowTexture = SDL_CreateGPUTexture(Device, &info);
		if (!ShadowTexture) {
			ENGINE_ERROR("shadow texture: {}", SDL_GetError());
			return false;
		}

		SDL_GPUSamplerCreateInfo sampler{};
		sampler.min_filter = SDL_GPU_FILTER_LINEAR;
		sampler.mag_filter = SDL_GPU_FILTER_LINEAR;
		sampler.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;

		// **Clamped to the edge, and the fragment shader also range-checks.**
		// Either alone would do; both, because a wrap mode would tile the map
		// across the world and the range check is what makes "outside the map is
		// lit" a stated rule rather than a property of a sampler setting.
		sampler.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
		sampler.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
		sampler.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;

		ShadowSampler = SDL_CreateGPUSampler(Device, &sampler);
		if (!ShadowSampler) {
			ENGINE_ERROR("shadow sampler: {}", SDL_GetError());
			return false;
		}
		return true;
	}

	bool Renderer::Impl::EnsureSurfaceSampler() {
		if (SurfaceSampler != nullptr) {
			return true;
		}

		SDL_GPUSamplerCreateInfo sampler{};
		sampler.min_filter = SDL_GPU_FILTER_LINEAR;
		sampler.mag_filter = SDL_GPU_FILTER_LINEAR;
		sampler.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
		sampler.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
		sampler.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
		sampler.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;

		SurfaceSampler = SDL_CreateGPUSampler(Device, &sampler);
		if (SurfaceSampler == nullptr) {
			ENGINE_ERROR("surface sampler: {}", SDL_GetError());
			return false;
		}
		return true;
	}

	bool Renderer::Impl::EnsureSurface(size_t viewport, uint8_t index, uint32_t width, uint32_t height) {
		if (index >= scene::MAX_SURFACES) {
			return false;
		}

		SurfaceSlotState &state = SurfacesAt(viewport).Surfaces[index];

		if (state.Texture[0] != nullptr && width == state.Width && height == state.Height) {
			return true;
		}

		if (!EnsureSurfaceSampler()) {
			return false;
		}

		// **Built beside what the slot already holds, and swapped in only once
		// all four exist.** Releasing first is what turned a device that could
		// not honour the new size into a pane with no texture at all: the slot
		// kept its old `Width`, so every later frame asked for the same size,
		// failed the same way, and the pane drew its own flat tint for the rest
		// of the run. A resize that cannot be made is a resize that does not
		// happen, and the picture the slot has is still a picture.
		SDL_GPUTextureCreateInfo colour{};
		colour.type = SDL_GPU_TEXTURETYPE_2D;
		colour.format = ColourFormat();
		colour.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
		colour.width = width;
		colour.height = height;
		colour.layer_count_or_depth = 1;
		colour.num_levels = 1;
		colour.sample_count = SDL_GPU_SAMPLECOUNT_1;

		SDL_GPUTextureCreateInfo depth{};
		depth.type = SDL_GPU_TEXTURETYPE_2D;
		depth.format = DepthFormat;
		depth.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
		depth.width = width;
		depth.height = height;
		depth.layer_count_or_depth = 1;
		depth.num_levels = 1;
		depth.sample_count = SDL_GPU_SAMPLECOUNT_1;

		SDL_GPUTexture *made[2] = {nullptr, nullptr};
		SDL_GPUTexture *madeDepth = nullptr;

		const auto abandon = [&]() {
			for (SDL_GPUTexture *texture : made) {
				if (texture != nullptr) {
					SDL_ReleaseGPUTexture(Device, texture);
				}
			}
			if (madeDepth != nullptr) {
				SDL_ReleaseGPUTexture(Device, madeDepth);
			}
			// **True when the slot still has its old pair**, because the caller's
			// question is "may this surface be rendered", not "was it resized".
			return state.Texture[0] != nullptr;
		};

		for (SDL_GPUTexture *&texture : made) {
			texture = SDL_CreateGPUTexture(Device, &colour);
			if (texture == nullptr) {
				ENGINE_ERROR(
					"viewport {} surface {} texture {}x{}: {}", viewport, index, width, height, SDL_GetError()
				);
				return abandon();
			}
		}

		madeDepth = SDL_CreateGPUTexture(Device, &depth);
		if (madeDepth == nullptr) {
			ENGINE_ERROR(
				"viewport {} surface {} depth {}x{}: {}", viewport, index, width, height, SDL_GetError()
			);
			return abandon();
		}

		for (SDL_GPUTexture *&texture : state.Texture) {
			if (texture != nullptr) {
				SDL_ReleaseGPUTexture(Device, texture);
			}
		}
		if (state.Depth != nullptr) {
			SDL_ReleaseGPUTexture(Device, state.Depth);
		}

		state.Texture[0] = made[0];
		state.Texture[1] = made[1];
		state.Depth = madeDepth;

		// Resized, so whatever it held is gone. A mirror that showed the last
		// frame at the old resolution stretched across the new one would be a
		// visible artefact on exactly the frame a window was dragged.
		state.Ready = false;

		// And it owes a draw immediately rather than at the end of its next
		// interval, which is what a resized slot with a live stamp would do:
		// hold an empty texture for up to a frame's worth of cap while the pane
		// showed its own tint.
		state.Drawn = -1.0;

		state.Width = width;
		state.Height = height;
		return true;
	}

	Renderer::Impl::PortalTarget *Renderer::Impl::EnsurePortal(
		size_t viewport, uint32_t level, uint8_t index, uint32_t width, uint32_t height
	) {
		if (index >= scene::MAX_SURFACES || level >= MAX_PORTAL_DEPTH || width == 0 || height == 0) {
			return nullptr;
		}

		SurfaceBank &bank = SurfacesAt(viewport);
		if (bank.Portals.size() <= level) {
			bank.Portals.resize(level + 1);
		}

		PortalTarget &target = bank.Portals[level].Targets[index];
		if (target.Colour != nullptr && target.Display != nullptr && target.Depth != nullptr &&
			width == target.Width && height == target.Height) {
			return &target;
		}

		if (target.Colour != nullptr) {
			SDL_ReleaseGPUTexture(Device, target.Colour);
			target.Colour = nullptr;
		}
		if (target.Display != nullptr) {
			SDL_ReleaseGPUTexture(Device, target.Display);
			target.Display = nullptr;
		}
		if (target.Depth != nullptr) {
			SDL_ReleaseGPUTexture(Device, target.Depth);
			target.Depth = nullptr;
		}
		target.Width = 0;
		target.Height = 0;

		SDL_GPUTextureCreateInfo colour{};
		colour.type = SDL_GPU_TEXTURETYPE_2D;
		colour.format = ColourFormat();
		colour.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
		colour.width = width;
		colour.height = height;
		colour.layer_count_or_depth = 1;
		colour.num_levels = 1;
		colour.sample_count = SDL_GPU_SAMPLECOUNT_1;

		target.Colour = SDL_CreateGPUTexture(Device, &colour);
		if (target.Colour == nullptr) {
			ENGINE_ERROR(
				"viewport {} portal level {} slot {} colour {}x{}: {}",
				viewport,
				level,
				index,
				width,
				height,
				SDL_GetError()
			);
			return nullptr;
		}
		target.Display = SDL_CreateGPUTexture(Device, &colour);
		if (target.Display == nullptr) {
			ENGINE_ERROR(
				"viewport {} portal level {} slot {} display {}x{}: {}",
				viewport,
				level,
				index,
				width,
				height,
				SDL_GetError()
			);
			SDL_ReleaseGPUTexture(Device, target.Colour);
			target.Colour = nullptr;
			target.Display = nullptr;
			return nullptr;
		}

		SDL_GPUTextureCreateInfo depth{};
		depth.type = SDL_GPU_TEXTURETYPE_2D;
		depth.format = DepthFormat;
		depth.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
		depth.width = width;
		depth.height = height;
		depth.layer_count_or_depth = 1;
		depth.num_levels = 1;
		depth.sample_count = SDL_GPU_SAMPLECOUNT_1;

		target.Depth = SDL_CreateGPUTexture(Device, &depth);
		if (target.Depth == nullptr) {
			ENGINE_ERROR(
				"viewport {} portal level {} slot {} depth {}x{}: {}",
				viewport,
				level,
				index,
				width,
				height,
				SDL_GetError()
			);
			SDL_ReleaseGPUTexture(Device, target.Colour);
			SDL_ReleaseGPUTexture(Device, target.Display);
			target.Colour = nullptr;
			target.Display = nullptr;
			return nullptr;
		}

		// **Shared with the surface path rather than a second one.** A portal
		// level is sampled exactly as a mirror's texture is.
		if (!EnsureSurfaceSampler()) {
			SDL_ReleaseGPUTexture(Device, target.Colour);
			SDL_ReleaseGPUTexture(Device, target.Display);
			SDL_ReleaseGPUTexture(Device, target.Depth);
			target.Colour = nullptr;
			target.Display = nullptr;
			target.Depth = nullptr;
			return nullptr;
		}

		target.Width = width;
		target.Height = height;
		return &target;
	}

	Renderer::Impl::MirrorTarget *Renderer::Impl::EnsureMirror(
		size_t viewport, uint32_t level, uint8_t index, uint32_t width, uint32_t height
	) {
		if (index >= scene::MAX_SURFACES || level >= MAX_SURFACE_DEPTH || width == 0 || height == 0) {
			return nullptr;
		}

		SurfaceBank &bank = SurfacesAt(viewport);
		if (bank.Mirrors.size() <= level) {
			bank.Mirrors.resize(level + 1);
		}

		MirrorTarget &target = bank.Mirrors[level].Targets[index];
		if (target.Colour != nullptr && width == target.Width && height == target.Height) {
			return &target;
		}

		if (target.Colour != nullptr) {
			SDL_ReleaseGPUTexture(Device, target.Colour);
			target.Colour = nullptr;
		}
		if (target.Depth != nullptr) {
			SDL_ReleaseGPUTexture(Device, target.Depth);
			target.Depth = nullptr;
		}
		target.Width = 0;
		target.Height = 0;

		SDL_GPUTextureCreateInfo colour{};
		colour.type = SDL_GPU_TEXTURETYPE_2D;
		colour.format = ColourFormat();
		colour.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
		colour.width = width;
		colour.height = height;
		colour.layer_count_or_depth = 1;
		colour.num_levels = 1;
		colour.sample_count = SDL_GPU_SAMPLECOUNT_1;

		target.Colour = SDL_CreateGPUTexture(Device, &colour);
		if (target.Colour == nullptr) {
			ENGINE_ERROR(
				"viewport {} mirror level {} slot {} colour {}x{}: {}",
				viewport,
				level,
				index,
				width,
				height,
				SDL_GetError()
			);
			return nullptr;
		}

		SDL_GPUTextureCreateInfo depth{};
		depth.type = SDL_GPU_TEXTURETYPE_2D;
		depth.format = DepthFormat;
		depth.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
		depth.width = width;
		depth.height = height;
		depth.layer_count_or_depth = 1;
		depth.num_levels = 1;
		depth.sample_count = SDL_GPU_SAMPLECOUNT_1;

		target.Depth = SDL_CreateGPUTexture(Device, &depth);
		if (target.Depth == nullptr) {
			ENGINE_ERROR(
				"viewport {} mirror level {} slot {} depth {}x{}: {}",
				viewport,
				level,
				index,
				width,
				height,
				SDL_GetError()
			);
			SDL_ReleaseGPUTexture(Device, target.Colour);
			target.Colour = nullptr;
			return nullptr;
		}

		// The one sampler every projected image is read through, for
		// `EnsurePortal`'s reason: a scene of nothing but mirror levels would
		// otherwise take a null one into `SDL_BindGPUFragmentSamplers`.
		if (!EnsureSurfaceSampler()) {
			return nullptr;
		}

		target.Width = width;
		target.Height = height;
		return &target;
	}

	Renderer::Impl::SeamLightTarget *Renderer::Impl::EnsureSeamLight(size_t viewport, uint8_t index) {
		if (index >= scene::MAX_SURFACES) {
			return nullptr;
		}

		SeamLightTarget &target = SurfacesAt(viewport).SeamLights[index];
		if (target.Colour != nullptr && target.Depth != nullptr) {
			return &target;
		}

		SDL_GPUTextureCreateInfo colour{};
		colour.type = SDL_GPU_TEXTURETYPE_2D;
		colour.format = ColourFormat();
		colour.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
		colour.width = SEAM_LIGHT_RESOLUTION;
		colour.height = SEAM_LIGHT_RESOLUTION;
		colour.layer_count_or_depth = 1;
		colour.num_levels = 1;
		colour.sample_count = SDL_GPU_SAMPLECOUNT_1;

		target.Colour = SDL_CreateGPUTexture(Device, &colour);
		if (target.Colour == nullptr) {
			ENGINE_ERROR("viewport {} seam light {} colour: {}", viewport, index, SDL_GetError());
			return nullptr;
		}

		SDL_GPUTextureCreateInfo depth{};
		depth.type = SDL_GPU_TEXTURETYPE_2D;
		depth.format = DepthFormat;
		depth.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
		depth.width = SEAM_LIGHT_RESOLUTION;
		depth.height = SEAM_LIGHT_RESOLUTION;
		depth.layer_count_or_depth = 1;
		depth.num_levels = 1;
		depth.sample_count = SDL_GPU_SAMPLECOUNT_1;

		target.Depth = SDL_CreateGPUTexture(Device, &depth);
		if (target.Depth == nullptr) {
			ENGINE_ERROR("viewport {} seam light {} depth: {}", viewport, index, SDL_GetError());
			SDL_ReleaseGPUTexture(Device, target.Colour);
			target.Colour = nullptr;
			return nullptr;
		}

		target.Width = SEAM_LIGHT_RESOLUTION;
		target.Height = SEAM_LIGHT_RESOLUTION;
		return &target;
	}

	bool Renderer::Impl::EnsureOverlay(int width, int height) {
		if (OverlayTexture && width == OverlayWidth && height == OverlayHeight) {
			return true;
		}

		if (OverlayTexture) {
			SDL_ReleaseGPUTexture(Device, OverlayTexture);
			OverlayTexture = nullptr;
		}
		if (OverlayTransfer) {
			SDL_ReleaseGPUTransferBuffer(Device, OverlayTransfer);
			OverlayTransfer = nullptr;
		}

		SDL_GPUTextureCreateInfo info{};
		info.type = SDL_GPU_TEXTURETYPE_2D;
		info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
		info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
		info.width = static_cast<uint32_t>(width);
		info.height = static_cast<uint32_t>(height);
		info.layer_count_or_depth = 1;
		info.num_levels = 1;
		info.sample_count = SDL_GPU_SAMPLECOUNT_1;

		OverlayTexture = SDL_CreateGPUTexture(Device, &info);

		SDL_GPUTransferBufferCreateInfo transferInfo{};
		transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
		transferInfo.size =
			static_cast<uint32_t>(width) * static_cast<uint32_t>(height) * OverlayImage::BYTES_PER_PIXEL;
		OverlayTransfer = SDL_CreateGPUTransferBuffer(Device, &transferInfo);

		if (!OverlayTexture || !OverlayTransfer) {
			ENGINE_ERROR("overlay texture {}x{}: {}", width, height, SDL_GetError());
			return false;
		}

		OverlayWidth = width;
		OverlayHeight = height;

		// A new texture holds whatever the driver had lying around. That did not
		// matter while every frame uploaded the whole image; now that a frame
		// uploads only the panels, everything outside them would be garbage
		// until something happened to draw over it. The next upload covers the
		// whole texture once to settle it.
		OverlayUninitialised = true;
		return true;
	}

	void Renderer::Impl::CollectTimings() {
		for (uint32_t slot = 0; slot < VulkanTimestamps::SLOTS; slot++) {
			if (!Timestamps.Pending(slot)) {
				continue;
			}
			double times[VulkanTimestamps::MARKS]{};
			uint32_t count = 0;
			if (!Timestamps.Collect(slot, times, count)) {
				continue;
			}

			if (TimingSequence[slot] >= ResolvedTimingSequence) {
				GpuTimings.clear();
				for (const PassMarks &marks : PendingMarks[slot]) {
					if (marks.Opened >= count || marks.Closed >= count) {
						continue;
					}
					GpuTimings[marks.Name.Id()] +=
						VulkanTimestamps::Between(times, marks.Opened, marks.Closed) / 1000.0;
				}
				ResolvedTimingSequence = TimingSequence[slot];
			}
			PendingMarks[slot].clear();
			TimingSequence[slot] = 0;
		}
	}

	void Renderer::Impl::CollectPreview() {
		void *mapped = SDL_MapGPUTransferBuffer(Device, Preview.Transfer, false);
		if (mapped == nullptr) {
			ENGINE_ERROR("resource preview: SDL_MapGPUTransferBuffer: {}", SDL_GetError());
			return;
		}

		const size_t count = static_cast<size_t>(Preview.Width) * Preview.Height;
		const auto *bytes = static_cast<const uint8_t *>(mapped);
		Preview.Pixels.resize(count);
		if (Preview.BytesPerPixel == 4) {
			std::memcpy(Preview.Pixels.data(), bytes, count * sizeof(uint32_t));
		} else if (Preview.BytesPerPixel == 2) {
			for (size_t index = 0; index < count; index++) {
				Preview.Pixels[index] = static_cast<uint32_t>(bytes[index * 2]) |
										(static_cast<uint32_t>(bytes[index * 2 + 1]) << 8) | 0xFF000000u;
			}
		} else {
			for (size_t index = 0; index < count; index++) {
				const uint32_t value = bytes[index];
				Preview.Pixels[index] = value | (value << 8) | (value << 16) | 0xFF000000u;
			}
		}
		SDL_UnmapGPUTransferBuffer(Device, Preview.Transfer);
		Preview.Histogram =
			Preview.Rgba ? render::HistogramRgba(Preview.Pixels) : render::Histogram(Preview.Pixels);
	}

	void Renderer::Impl::PollPreview() {
		if (Preview.Fence == nullptr || !SDL_QueryGPUFence(Device, Preview.Fence)) {
			return;
		}
		SDL_ReleaseGPUFence(Device, Preview.Fence);
		Preview.Fence = nullptr;
		if (Preview.Pending.Poll(true)) {
			CollectPreview();
		}
	}

	bool Renderer::Impl::RequestPreview(
		SDL_GPUCommandBuffer *command,
		SDL_GPUTexture *texture,
		uint32_t width,
		uint32_t height,
		core::Name source,
		size_t slot,
		SDL_GPUTextureFormat format
	) {
		if (!Preview.Pending.CanRequest() || command == nullptr || texture == nullptr || width == 0 ||
			height == 0 || !source.IsValid()) {
			return false;
		}

		uint32_t bytesPerPixel = 0;
		bool rgba = true;
		switch (format) {
		case SDL_GPU_TEXTUREFORMAT_R8_UNORM:
			bytesPerPixel = 1;
			break;
		case SDL_GPU_TEXTUREFORMAT_R8G8_UNORM:
			bytesPerPixel = 2;
			break;
		case SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM:
		case SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB:
			bytesPerPixel = 4;
			break;
		case SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM:
		case SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB:
			bytesPerPixel = 4;
			rgba = false;
			break;
		default:
			return false;
		}

		const uint64_t bytes = static_cast<uint64_t>(width) * height * bytesPerPixel;
		if (bytes > std::numeric_limits<uint32_t>::max()) {
			ENGINE_ERROR("resource preview: {}x{} is too large for one transfer", width, height);
			return false;
		}
		if (Preview.Capacity < bytes) {
			if (Preview.Transfer != nullptr) {
				SDL_ReleaseGPUTransferBuffer(Device, Preview.Transfer);
				Preview.Transfer = nullptr;
			}

			SDL_GPUTransferBufferCreateInfo info{};
			info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
			info.size = static_cast<uint32_t>(bytes);
			Preview.Transfer = SDL_CreateGPUTransferBuffer(Device, &info);
			if (Preview.Transfer == nullptr) {
				ENGINE_ERROR("resource preview: SDL_CreateGPUTransferBuffer: {}", SDL_GetError());
				Preview.Capacity = 0;
				return false;
			}
			Preview.Capacity = static_cast<uint32_t>(bytes);
		}

		SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(command);
		if (copy == nullptr) {
			ENGINE_ERROR("resource preview: SDL_BeginGPUCopyPass: {}", SDL_GetError());
			return false;
		}
		SDL_GPUTextureRegion region{};
		region.texture = texture;
		region.w = width;
		region.h = height;
		region.d = 1;
		SDL_GPUTextureTransferInfo destination{};
		destination.transfer_buffer = Preview.Transfer;
		destination.pixels_per_row = width;
		destination.rows_per_layer = height;
		SDL_DownloadFromGPUTexture(copy, &region, &destination);
		SDL_EndGPUCopyPass(copy);

		Preview.Source = source;
		Preview.Slot = slot;
		Preview.Width = width;
		Preview.Height = height;
		Preview.BytesPerPixel = bytesPerPixel;
		Preview.Rgba = rgba;
		Preview.Pending.Submitted(FrameCounter);
		PreviewSubmitted = true;
		return true;
	}

	// -----------------------------------------------------------------------

	void FrameResult::Accumulate(const FrameResult &view) {
		Presented = Presented || view.Presented;
		DrawCalls += view.DrawCalls;
		Triangles += view.Triangles;
		SurfaceInstances += view.SurfaceInstances;
		SurfacePasses += view.SurfacePasses;
		PortalPasses += view.PortalPasses;
		RibbonVertices += view.RibbonVertices;
		Particles += view.Particles;
		Culled += view.Culled;
		ScheduledReadBytes += view.ScheduledReadBytes;
		ScheduledWriteBytes += view.ScheduledWriteBytes;
		QueueTransferBytes += view.QueueTransferBytes;
		UploadedBytes += view.UploadedBytes;
		UploadCommandBuffers += view.UploadCommandBuffers;
		ComputeDispatches += view.ComputeDispatches;
		AsyncComputeCommandBuffers += view.AsyncComputeCommandBuffers;
		DownloadCommandBuffers += view.DownloadCommandBuffers;
		TrafficCommandBuffers += view.TrafficCommandBuffers;
		ConcurrentWaves += view.ConcurrentWaves;
		for (const core::Name node : view.Nodes) {
			if (!Ran(node)) {
				Nodes.push_back(node);
			}
		}
	}

	Renderer::Renderer() : State(std::make_unique<Impl>()), Owner(std::this_thread::get_id()) {
		graph::RenderGraph pipeline;
		core::Name offender;
		if (graph::Build(graph::DefaultPbrDocument(), pipeline, offender) !=
			graph::PipelineDocumentStatus::Ok) {
			ENGINE_ERROR("engine default render graph did not build at '{}'", offender.Text());
			return;
		}

		graph::CompiledGraph compiled;
		graph::ExecutionSchedule schedule;
		std::string reason;
		if (!CompileRenderPipeline(pipeline, compiled, schedule, offender, reason)) {
			ENGINE_ERROR("engine default render graph refused: {} at '{}'", reason, offender.Text());
			return;
		}
		std::vector<graph::PlannedCommandBuffer> buffers = graph::PlanCommandBuffers(schedule);
		State->EngineDefault = Impl::NamedPipeline{
			core::Name("Engine Default"),
			pipeline,
			std::move(compiled),
			std::move(schedule),
			std::move(buffers)
		};
	}

	Renderer::~Renderer() {
		Shutdown();
	}

	bool Renderer::SetPipeline(core::Name name, const graph::RenderGraph &pipeline) {
		RequireOwningThread("SetPipeline");
		if (!name.IsValid()) {
			ENGINE_ERROR("a render pipeline needs a name a view can select");
			return false;
		}

		graph::CompiledGraph compiled;
		graph::ExecutionSchedule schedule;
		core::Name offender;
		std::string reason;
		if (!CompileRenderPipeline(pipeline, compiled, schedule, offender, reason)) {
			ENGINE_ERROR("pipeline '{}' refused: {} at '{}'", name.Text(), reason, offender.Text());
			return false;
		}

		for (Impl::NamedPipeline &installed : State->NamedPipelines) {
			if (installed.Name != name) {
				continue;
			}
			if (State->Device != nullptr) {
				(void)WaitForFrame();
			}
			State->ReleaseGraphState(name);
			installed.Graph = pipeline;
			installed.Compiled = std::move(compiled);
			installed.Buffers = graph::PlanCommandBuffers(schedule);
			installed.Schedule = std::move(schedule);
			return true;
		}

		std::vector<graph::PlannedCommandBuffer> buffers = graph::PlanCommandBuffers(schedule);
		State->NamedPipelines.push_back(
			Impl::NamedPipeline{name, pipeline, std::move(compiled), std::move(schedule), std::move(buffers)}
		);
		return true;
	}

	bool Renderer::RemovePipeline(core::Name name) {
		RequireOwningThread("RemovePipeline");
		for (size_t index = 0; index < State->NamedPipelines.size(); index++) {
			if (State->NamedPipelines[index].Name != name) {
				continue;
			}
			if (State->Device != nullptr) {
				(void)WaitForFrame();
			}
			State->ReleaseGraphState(name);
			State->NamedPipelines.erase(State->NamedPipelines.begin() + static_cast<ptrdiff_t>(index));
			return true;
		}
		return false;
	}

	std::vector<core::Name> Renderer::Pipelines() const {
		std::vector<core::Name> names;
		names.reserve(State->NamedPipelines.size());
		for (const Impl::NamedPipeline &pipeline : State->NamedPipelines) {
			names.push_back(pipeline.Name);
		}
		std::sort(names.begin(), names.end(), [](core::Name first, core::Name second) {
			return first.Text() < second.Text();
		});
		return names;
	}

	void Renderer::ResetPipelines() {
		RequireOwningThread("ResetPipelines");
		if (State->Device != nullptr && !State->NamedPipelines.empty()) {
			(void)WaitForFrame();
		}
		for (const Impl::NamedPipeline &pipeline : State->NamedPipelines) {
			State->ReleaseGraphState(pipeline.Name);
		}
		State->NamedPipelines.clear();
	}

	void Renderer::Inspect(core::Name resource, size_t slot) {
		RequireOwningThread("Inspect");
		State->Inspected = resource;
		State->InspectedSlot = slot;
	}

	core::Name Renderer::Inspecting() const {
		return State == nullptr ? core::Name{} : State->Inspected;
	}

	Renderer::ReadbackImage Renderer::Readback() const {
		ReadbackImage image;
		if (State == nullptr || !State->Preview.Pending.HasImage()) {
			return image;
		}
		image.Source = State->Preview.Source;
		image.Slot = State->Preview.Slot;
		image.Width = State->Preview.Width;
		image.Height = State->Preview.Height;
		image.Pixels = State->Preview.Pixels;
		image.Histogram = State->Preview.Histogram;
		image.Age = State->Preview.Pending.Age(State->FrameCounter);
		return image;
	}

	const std::unordered_map<uint32_t, double> &Renderer::PassTimings() const {
		return State->GpuTimings;
	}

	const std::unordered_map<uint32_t, double> &Renderer::PassWallTimes() const {
		return State->WallTimings;
	}

	bool Renderer::Timed() const {
		return State != nullptr && State->Timestamps.Ready();
	}

	std::string_view Renderer::BackendName() const {
		return State->Backend;
	}

	bool Renderer::SetVerticalSync(bool enabled) {
		if (!State->Device) {
			return false;
		}

		const SDL_GPUPresentMode mode = enabled ? SDL_GPU_PRESENTMODE_VSYNC : SDL_GPU_PRESENTMODE_IMMEDIATE;

		// VSYNC is the only mode required to exist. Asking for IMMEDIATE on a
		// backend without it fails rather than silently staying synchronised,
		// so check before asking for it - an unsupported mode would otherwise
		// leave the swapchain in whatever state the failed call left it.
		//
		// **The query is the half that is safe to do now**, which is what lets
		// this keep answering the caller straight away. It reads the surface's
		// capabilities and touches nothing; setting the mode is what recreates
		// the swapchain, and that is deferred.
		if (!SDL_WindowSupportsGPUPresentMode(State->Device, State->Window, mode)) {
			ENGINE_WARN("present mode unsupported on {}", State->Backend);
			return false;
		}

		// **Queued rather than set, and see `Impl::PendingPresentMode` for why.**
		// Setting it destroys and rebuilds every swapchain image, and this is
		// called from the middle of a frame that is already holding one of them.
		State->PendingPresentMode = mode;
		State->PresentModePending = true;
		return true;
	}

	bool Renderer::WaitForFrame() {
		RequireOwningThread("WaitForFrame");

		if (State->Device == nullptr) {
			return false;
		}

		return State->BeginFrame();
	}

	bool Renderer::IsOnOwningThread() const {
		return Owner == std::this_thread::get_id();
	}

	void Renderer::RequireOwningThread(const char *what) const {
		if (IsOnOwningThread()) {
			return;
		}

		// Abort rather than return, for `ecs::Store::RequireOwningThread`'s
		// reason and one of this module's own. By the time a second thread is
		// inside here it has already recorded into a command buffer another
		// thread is filling, so there is nothing left to decline - and the
		// symptom on the far side is a driver validation error or a frame of
		// somebody else's geometry, neither of which points back here. The stack
		// at the violation is the whole value.
		ENGINE_ERROR(
			"renderer: {} called from a thread that does not own it. "
			"Passes share one command buffer and one device, so a frame is "
			"recorded by the thread that initialised the renderer and by no "
			"other. Draw viewports one after another.",
			what
		);
		std::abort();
	}

	bool Renderer::Initialise(SDL_Window *window, uint32_t framesInFlight) {
		// **Re-bound here, and the constructor's claim is what makes the check
		// testable without a device.** A renderer is legitimately constructed by
		// whoever owns the object and initialised by whoever owns the window -
		// it is the device, not the C++ object, that the contract is about - so
		// this is the authoritative claim and the constructor's is a default
		// that costs nothing to be wrong about, because being wrong about it
		// means nothing has been created yet.
		Owner = std::this_thread::get_id();

		// **A null window is headless and is not an error.** A renderer with
		// nowhere to present still draws: into a `SceneTarget`, which is what a
		// capture, a CI comparison and an automated editor all read. Refusing it
		// was right while an offscreen target did not exist and stopped being
		// right at v0.7.
		State->Window = window;

		// **Every format the build produces, which is now two.** `glslc`
		// compiles the built-in GLSL to SPIR-V and `mono.tools/shadercross`
		// translates each module to MSL beside it, so a Metal device has
		// something to be given. D3D12 needs DXIL, which is still not built -
		// asking for a format we cannot supply would find a device and then fail
		// at pipeline creation, which is a worse error than being refused here.
		State->Device = SDL_CreateGPUDevice(SUPPORTED_SHADER_FORMATS, false, nullptr);
		if (!State->Device) {
			ENGINE_ERROR("SDL_CreateGPUDevice: {}", SDL_GetError());
			return false;
		}

		// Asked once, here, rather than assumed at each of the three places a
		// shader is created. `docs/DEFERRED.md` D00001's guess at what would
		// break first on a Mac was this line naming SPIR-V and nothing else.
		State->Binary = ShaderBinaryFor(State->Device);

		if (window != nullptr && !SDL_ClaimWindowForGPUDevice(State->Device, window)) {
			ENGINE_ERROR("SDL_ClaimWindowForGPUDevice: {}", SDL_GetError());
			Shutdown();
			return false;
		}

		// **One frame queued rather than SDL's two, and the frame rate is not
		// what this buys.** The default lets the CPU submit a second frame
		// before the GPU has finished the first, so what a display is showing is
		// up to two frames behind the input that produced it - 33 ms at 60 Hz
		// before the compositor takes its turn. SDL's own wording is that higher
		// values "increase throughput at the expense of visual latency", and an
		// editor is the case where that trade is backwards: nobody drags a
		// splitter to reach a frame rate, and every millisecond between the
		// mouse and the picture is felt by the hand holding it.
		//
		// What it costs is the throughput it was buying. A frame that would have
		// overlapped now waits, so a GPU-bound scene loses some of its rate -
		// which is why the measurement that matters here is the one taken by
		// hand, not the one the profiler reports.
		// **Clamped rather than trusted.** SDL takes 1 to 3 and answers false
		// for anything else, which would leave the device at its default with
		// only a warning to say so - a number nobody chose deciding the feel of
		// the editor.
		const uint32_t queued = std::clamp<uint32_t>(framesInFlight, 1, 3);

		if (window != nullptr && !SDL_SetGPUAllowedFramesInFlight(State->Device, queued)) {
			// Not fatal. The default is a working configuration and the only
			// thing lost is the latency this was trying to save.
			ENGINE_WARN("SDL_SetGPUAllowedFramesInFlight: {}", SDL_GetError());
		}

		const char *driver = SDL_GetGPUDeviceDriver(State->Device);
		State->Backend = driver ? driver : "unknown";
		(void)State->Timestamps.Probe(State->Device);

		// **Before the pipelines, because they name the format too.** A pipeline
		// built against one depth format and bound beside a texture in another
		// is a validation error at bind time. See `Impl::DepthFormat`.
		{
			// **Both usages, because one format serves both kinds of depth
			// texture.** The viewport's buffer is only ever a target, but the
			// shadow map is sampled as well - and a second format for the shadow
			// map would be a second thing that has to agree with the shadow
			// pipeline. Asking for the intersection once is cheaper than keeping
			// two in step.
			const auto supports = [&](SDL_GPUTextureFormat format) {
				return SDL_GPUTextureSupportsFormat(
					State->Device,
					format,
					SDL_GPU_TEXTURETYPE_2D,
					SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER
				);
			};

			// Most precision first. The shadow pass compares depths across a
			// whole scene, so the extra bits are worth asking for - and
			// D16_UNORM is the fallback rather than the preference because a
			// sixteen-bit shadow map stair-steps on a large world.
			if (supports(SDL_GPU_TEXTUREFORMAT_D32_FLOAT)) {
				State->DepthFormat = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
			} else if (supports(SDL_GPU_TEXTUREFORMAT_D24_UNORM)) {
				State->DepthFormat = SDL_GPU_TEXTUREFORMAT_D24_UNORM;
			} else {
				// Guaranteed by SDL, so there is no third case to handle.
				State->DepthFormat = SDL_GPU_TEXTUREFORMAT_D16_UNORM;
			}
		}

		SDL_GPUSamplerCreateInfo sampler{};
		// Nearest, because the overlay is pixel art at exactly one texel per
		// pixel. Linear would blur the 3x5 font into illegibility.
		sampler.min_filter = SDL_GPU_FILTER_NEAREST;
		sampler.mag_filter = SDL_GPU_FILTER_NEAREST;
		sampler.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
		sampler.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
		sampler.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
		State->OverlaySampler = SDL_CreateGPUSampler(State->Device, &sampler);

		if (!State->CreatePipelines() || !State->CreateGeometry()) {
			Shutdown();
			return false;
		}

		ENGINE_INFO("renderer ready on {}", State->Backend);
		return true;
	}

	void Renderer::Shutdown() {
		// Checked here as well as in `Render`, because releasing a device while
		// another thread holds a command buffer against it is the same violation
		// arriving at the end of the frame instead of the middle.
		RequireOwningThread("Shutdown");

		// **A frame waited for and never drawn, which is what quitting during
		// the event pump produces.** The loop's usual shape makes this
		// unreachable - pump, simulate, present, and only then test whether to
		// stop - but "usual" is not a guarantee, and a swapchain image held past
		// the device's destruction is a crash inside the backend rather than an
		// error here. See `Impl::AbandonFrame` for why it submits.
		State->AbandonFrame();

		auto *device = State->Device;
		if (!device) {
			return;
		}

		// Everything below is still referenced by frames that may not have
		// finished. Waiting once here is simpler and no slower than tracking
		// per-resource fences for a shutdown path.
		SDL_WaitForGPUIdle(device);
		State->Timestamps.Shutdown();
		if (State->Preview.Fence != nullptr) {
			SDL_ReleaseGPUFence(device, State->Preview.Fence);
			State->Preview.Fence = nullptr;
		}
		if (State->Preview.Transfer != nullptr) {
			SDL_ReleaseGPUTransferBuffer(device, State->Preview.Transfer);
			State->Preview.Transfer = nullptr;
		}

		// **Before the pipelines they were derived from**, which costs nothing
		// and reads in the order the objects were built. `SDL_WaitForGPUIdle`
		// above is what makes releasing any of this safe.
		State->ReleaseShaderVariants();
		State->ReleaseAllGraphState();
		State->ReleaseOcclusion();
		State->ReleaseParticlePool();

		if (State->OpaquePipeline) {
			SDL_ReleaseGPUGraphicsPipeline(device, State->OpaquePipeline);
		}
		if (State->WireframeOpaquePipeline) {
			SDL_ReleaseGPUGraphicsPipeline(device, State->WireframeOpaquePipeline);
		}
		if (State->WireframeTransparentPipeline) {
			SDL_ReleaseGPUGraphicsPipeline(device, State->WireframeTransparentPipeline);
		}
		for (SDL_GPUGraphicsPipeline *pipeline :
			 {State->GBufferPipeline,
			  State->DepthLinearPipeline,
			  State->SsaoPipeline,
			  State->DeferredLightingPipeline,
			  State->TonemapPipeline,
			  State->PostProcessPipeline}) {
			if (pipeline != nullptr) {
				SDL_ReleaseGPUGraphicsPipeline(device, pipeline);
			}
		}
		State->PostProcessPipeline = nullptr;
		State->PostProcessShaderName = core::Name{};
		if (State->TransparentPipeline) {
			SDL_ReleaseGPUGraphicsPipeline(device, State->TransparentPipeline);
		}
		if (State->ShadowPipeline) {
			SDL_ReleaseGPUGraphicsPipeline(device, State->ShadowPipeline);
		}
		if (State->ShadowTexture) {
			SDL_ReleaseGPUTexture(device, State->ShadowTexture);
		}
		if (State->BeamTexture) {
			SDL_ReleaseGPUTexture(device, State->BeamTexture);
		}
		if (State->ShadowSampler) {
			SDL_ReleaseGPUSampler(device, State->ShadowSampler);
		}
		for (Impl::SurfaceBank &bank : State->SurfaceBanks) {
			for (Impl::SurfaceSlotState &surface : bank.Surfaces) {
				for (SDL_GPUTexture *texture : surface.Texture) {
					if (texture) {
						SDL_ReleaseGPUTexture(device, texture);
					}
				}
				if (surface.Depth) {
					SDL_ReleaseGPUTexture(device, surface.Depth);
				}
			}

			for (Impl::PortalLevel &level : bank.Portals) {
				for (Impl::PortalTarget &target : level.Targets) {
					if (target.Colour) {
						SDL_ReleaseGPUTexture(device, target.Colour);
					}
					if (target.Display) {
						SDL_ReleaseGPUTexture(device, target.Display);
					}
					if (target.Depth) {
						SDL_ReleaseGPUTexture(device, target.Depth);
					}
				}
			}

			for (Impl::MirrorLevel &level : bank.Mirrors) {
				for (Impl::MirrorTarget &target : level.Targets) {
					if (target.Colour) {
						SDL_ReleaseGPUTexture(device, target.Colour);
					}
					if (target.Depth) {
						SDL_ReleaseGPUTexture(device, target.Depth);
					}
				}
			}

			for (Impl::SeamLightTarget &target : bank.SeamLights) {
				if (target.Colour) {
					SDL_ReleaseGPUTexture(device, target.Colour);
				}
				if (target.Depth) {
					SDL_ReleaseGPUTexture(device, target.Depth);
				}
			}
		}
		State->SurfaceBanks.clear();
		if (State->SurfaceSampler) {
			SDL_ReleaseGPUSampler(device, State->SurfaceSampler);
		}
		for (Impl::SceneSlot &slot : State->SceneSlots) {
			if (slot.Texture) {
				SDL_ReleaseGPUTexture(device, slot.Texture);
				slot.Texture = nullptr;
			}
			if (slot.Depth) {
				SDL_ReleaseGPUTexture(device, slot.Depth);
				slot.Depth = nullptr;
			}
			if (slot.History) {
				SDL_ReleaseGPUTexture(device, slot.History);
				slot.History = nullptr;
			}
		}
		for (Impl::PbrSlot &slot : State->PbrSlots) {
			State->ReleasePbr(slot);
		}
		State->PbrSlots.clear();
		for (Impl::ResourcePreviewTarget &preview : State->ResourcePreviews) {
			for (SDL_GPUTexture *texture : preview.Textures) {
				if (texture != nullptr) {
					SDL_ReleaseGPUTexture(device, texture);
				}
			}
		}
		State->ResourcePreviews.clear();

		// Anything a resize retired and no frame came along to free. Shutting
		// down is the one path where the next frame never arrives, so leaving
		// this to `DrainRetiredScenes` would leak a texture per resize on exit.
		State->DrainRetiredScenes();

		if (State->ImagePipeline) {
			SDL_ReleaseGPUGraphicsPipeline(device, State->ImagePipeline);
		}
		if (State->OverlayPipeline) {
			SDL_ReleaseGPUGraphicsPipeline(device, State->OverlayPipeline);
		}
		State->Meshes.Shutdown();
		State->Textures.Shutdown();
		if (State->InstanceBuffer) {
			SDL_ReleaseGPUBuffer(device, State->InstanceBuffer);
		}
		if (State->InstanceTransfer) {
			SDL_ReleaseGPUTransferBuffer(device, State->InstanceTransfer);
		}
		if (State->DepthTexture) {
			SDL_ReleaseGPUTexture(device, State->DepthTexture);
		}
		if (State->FallbackTexture) {
			SDL_ReleaseGPUTexture(device, State->FallbackTexture);
		}
		if (State->OverlayTexture) {
			SDL_ReleaseGPUTexture(device, State->OverlayTexture);
		}
		if (State->OverlayTransfer) {
			SDL_ReleaseGPUTransferBuffer(device, State->OverlayTransfer);
		}
		if (State->OverlaySampler) {
			SDL_ReleaseGPUSampler(device, State->OverlaySampler);
		}

		if (State->Window) {
			SDL_ReleaseWindowFromGPUDevice(device, State->Window);
		}
		SDL_DestroyGPUDevice(device);

		// **Rebuilt in place rather than assigned from a fresh one.** The two
		// tables own device resources and are deliberately not copyable, so
		// `*State = Impl{}` no longer compiles - and it should not: an
		// assignment would have released their buffers a second time, after
		// `Shutdown` above already did.
		State = std::make_unique<Impl>();
	}

	bool Renderer::MeshExtentOf(const core::Name &name, core::Vector3 &out) const {
		if (State == nullptr || !State->Meshes.Has(name)) {
			return false;
		}
		out = State->Meshes.Resolve(name).Extent;
		return true;
	}

	bool Renderer::AddMesh(const core::Name &name, const assets::MeshData &mesh) {
		if (State == nullptr || State->Device == nullptr) {
			return false;
		}

		// Uploaded on the spot rather than at the next frame's barrier.
		// Registration happens when content arrives, which a caller has already
		// arranged to be a moment it controls - `delivery::Client::Pump` is the
		// whole design of that - so deferring would add a second barrier for a
		// caller that already had one.
		return State->Meshes.Add(name, mesh) && State->Meshes.Flush();
	}

	bool Renderer::AddTexture(const core::Name &name, const assets::TextureData &image) {
		if (State == nullptr || State->Device == nullptr) {
			return false;
		}
		return State->Textures.Add(name, image);
	}

	void Renderer::ExpectTexture(const core::Name &name) {
		if (State != nullptr) {
			State->Textures.Expect(name);
		}
	}

	void Renderer::StopExpectingTexture(const core::Name &name) {
		if (State != nullptr) {
			State->Textures.StopExpecting(name);
		}
	}

	bool Renderer::ExpectingTexture(const core::Name &name) const {
		return State != nullptr && State->Textures.Expecting(name);
	}

	void Renderer::SetAnimationTime(double seconds) {
		if (State != nullptr) {
			State->AnimationSeconds = seconds;
		}
	}

	void Renderer::SetSurfaceBounces(uint32_t bounces) {
		if (State != nullptr) {
			// **Zero is kept rather than floored, and that is the change.** It
			// used to mean "no surface pass at all", which nobody wanted and
			// which has a clearer spelling; it is
			// `scene::AUTOMATIC_SURFACE_BOUNCES` now, and it is the default. A
			// stated number still floors at one for the original reason.
			//
			// **Capped**, which it did not need to be while this counted
			// iterations of one pass. It counts levels of a recursion - see
			// `MAX_SURFACE_DEPTH` for why that turns a large number from
			// wasteful into unfinishable.
			State->SurfaceBounces = bounces == 0 ? 0u : std::clamp<uint32_t>(bounces, 1u, MAX_SURFACE_DEPTH);
		}
	}

	uint32_t Renderer::SurfaceBounces() const {
		return State == nullptr ? 0u : State->SurfaceBounces;
	}

	void Renderer::SetWireframe(bool enabled) {
		if (State != nullptr) {
			State->WireframeMode = enabled;
		}
	}

	bool Renderer::Wireframe() const {
		return State != nullptr && State->WireframeMode;
	}

	void Renderer::SetPortalDepth(uint32_t depth) {
		if (State != nullptr) {
			// **Clamped rather than floored**, which is the opposite of the
			// bounce knob above and is right for the opposite reason: zero here
			// is a meaningful setting - every hole draws flat and nothing
			// recurses - while the ceiling exists because the pool is full-screen
			// targets per level per slot.
			State->PortalDepth = std::min(depth, MAX_PORTAL_DEPTH);
		}
	}

	void Renderer::SetSun(
		const core::Vector3 &direction, const core::Color3 &ambient, const core::Color3 &direct
	) {
		if (State == nullptr) {
			return;
		}

		// **Normalised by the caller and checked here.** `scene::SunOf` already
		// does it, and a direction of zero arriving from anywhere else would
		// shade every surface by `dot(n, 0)` - a world that goes flat grey with
		// nothing in the log to say why.
		const float length = direction.Magnitude();
		if (length > 0.0f) {
			State->Sun = glm::vec3{direction.X / length, direction.Y / length, direction.Z / length};
		}

		State->Ambient = glm::vec4{ambient.R, ambient.G, ambient.B, 1.0f};
		State->Direct = glm::vec4{direct.R, direct.G, direct.B, 1.0f};
	}

	void Renderer::SetLighting(const scene::WorldLighting &lighting) {
		if (State == nullptr) {
			return;
		}

		SetSun(lighting.Direction, lighting.Ambient, lighting.Direct);
		State->OutdoorAmbient = glm::vec4{
			lighting.OutdoorAmbient.R,
			lighting.OutdoorAmbient.G,
			lighting.OutdoorAmbient.B,
			1.0f,
		};
		State->FogColour = glm::vec4{lighting.FogColor.R, lighting.FogColor.G, lighting.FogColor.B, 1.0f};
		State->FogStart = std::max(lighting.FogStart, 0.0f);
		State->FogEnd = std::max(lighting.FogEnd, State->FogStart);
	}

	scene::WorldLighting Renderer::CurrentLighting() const {
		if (State == nullptr) {
			return {};
		}

		scene::WorldLighting lighting;
		lighting.Direction = core::Vector3{State->Sun.x, State->Sun.y, State->Sun.z};
		lighting.Ambient = core::Color3{State->Ambient.x, State->Ambient.y, State->Ambient.z};
		lighting.OutdoorAmbient = core::Color3{
			State->OutdoorAmbient.x,
			State->OutdoorAmbient.y,
			State->OutdoorAmbient.z,
		};
		lighting.Direct = core::Color3{State->Direct.x, State->Direct.y, State->Direct.z};
		lighting.FogColor = core::Color3{State->FogColour.x, State->FogColour.y, State->FogColour.z};
		lighting.FogStart = State->FogStart;
		lighting.FogEnd = State->FogEnd;
		return lighting;
	}

	core::Vector3 Renderer::SunDirection() const {
		return core::Vector3{State->Sun.x, State->Sun.y, State->Sun.z};
	}

	core::Color3 Renderer::SunAmbient() const {
		return core::Color3{State->Ambient.x, State->Ambient.y, State->Ambient.z};
	}

	core::Color3 Renderer::SunColor() const {
		return core::Color3{State->Direct.x, State->Direct.y, State->Direct.z};
	}

	uint32_t Renderer::PortalDepth() const {
		return State == nullptr ? 0u : State->PortalDepth;
	}

	FlipbookCell Renderer::TextureCell(const core::Name &name, double seconds) const {
		if (State == nullptr) {
			return {};
		}
		return State->Textures.CellOf(name, seconds);
	}

	void *Renderer::TextureHandle(const core::Name &name) const {
		if (State == nullptr) {
			return nullptr;
		}
		return State->Textures.Find(name);
	}

	bool Renderer::TextureSize(const core::Name &name, uint32_t &width, uint32_t &height) const {
		if (State == nullptr) {
			return false;
		}
		return State->Textures.SizeOf(name, width, height);
	}

	bool Renderer::DropTexture(const core::Name &name) {
		if (State == nullptr) {
			return false;
		}
		return State->Textures.Drop(name);
	}

	bool Renderer::AddShader(const core::Name &name, std::span<const uint32_t> spirv) {
		if (State == nullptr || State->Device == nullptr) {
			return false;
		}

		// **Built on the spot rather than at the next frame's barrier**, which
		// is `AddMesh`'s rule and its reason: a caller has already arranged for
		// this to be a moment it controls - a content pump or a
		// `ShaderLibrary::Refresh` - so deferring would add a second
		// synchronisation point for a caller that already had one.
		//
		// **A frame is waited for first, because replacing releases.** An author
		// editing a `ShaderScript` replaces a pipeline that the frame in flight
		// may still be drawing through, and releasing that is a use after free
		// inside the driver rather than an error here.
		if (State->ShaderVariants.contains(name.Id())) {
			(void)WaitForFrame();
		}
		return State->AddShaderVariant(name, spirv);
	}

	bool Renderer::DropShader(const core::Name &name) {
		if (State == nullptr || State->Device == nullptr) {
			return false;
		}
		if (!State->ShaderVariants.contains(name.Id())) {
			return false;
		}

		(void)WaitForFrame();
		State->DropShaderVariant(name);
		return true;
	}

	bool Renderer::HasShader(const core::Name &name) const {
		return State != nullptr && name.IsValid() && State->ShaderVariants.contains(name.Id());
	}

	bool Renderer::SetPostProcessShader(const core::Name &name, std::span<const uint32_t> spirv) {
		if (State == nullptr || State->Device == nullptr || spirv.empty()) {
			return false;
		}

		const bool toMsl = State->Binary.Form == resources::ShaderForm::Msl;
		std::string translated;
		if (toMsl) {
			msl::Translation result = msl::Translate(spirv);
			if (result.Failed) {
				ENGINE_ERROR(
					"postprocess shader '{}' cannot be translated to MSL: {}", name.Text(), result.Error
				);
				return false;
			}
			translated = std::move(result.Source);
		}

		SDL_GPUShaderCreateInfo fragmentInfo{};
		fragmentInfo.code = toMsl ? reinterpret_cast<const Uint8 *>(translated.data())
								  : reinterpret_cast<const Uint8 *>(spirv.data());
		fragmentInfo.code_size = toMsl ? translated.size() : spirv.size() * sizeof(uint32_t);
		fragmentInfo.entrypoint = State->Binary.EntryPoint;
		fragmentInfo.format = State->Binary.Format;
		fragmentInfo.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;

		// **One sampler and no bound uniform buffer - `tonemap.frag`'s own
		// shape**, matching what `CreatePipelines` declares for it. See
		// `SetPostProcessShader`'s own header for the contract a
		// `ShaderScript` used here is written against.
		fragmentInfo.num_samplers = 1;
		fragmentInfo.num_uniform_buffers = 0;

		SDL_GPUShader *fragment = SDL_CreateGPUShader(State->Device, &fragmentInfo);
		if (fragment == nullptr) {
			ENGINE_ERROR("postprocess shader '{}': {}", name.Text(), SDL_GetError());
			return false;
		}

		// Reloaded rather than kept - SDL_GPU pipelines own what they need
		// from the shader objects that built them, `InterfacePass::
		// AddShaderVariant`'s own reason.
		SDL_GPUShader *vertex = State->LoadShader("overlay.vert", SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
		if (vertex == nullptr) {
			SDL_ReleaseGPUShader(State->Device, fragment);
			return false;
		}

		SDL_GPUColorTargetDescription target{};
		target.format = State->ColourFormat();

		SDL_GPUGraphicsPipelineCreateInfo info{};
		info.vertex_shader = vertex;
		info.fragment_shader = fragment;
		info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
		info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
		info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
		info.target_info.color_target_descriptions = &target;
		info.target_info.num_color_targets = 1;

		SDL_GPUGraphicsPipeline *pipeline = SDL_CreateGPUGraphicsPipeline(State->Device, &info);

		SDL_ReleaseGPUShader(State->Device, vertex);
		SDL_ReleaseGPUShader(State->Device, fragment);

		if (pipeline == nullptr) {
			ENGINE_ERROR("postprocess shader '{}' pipeline: {}", name.Text(), SDL_GetError());
			return false;
		}

		// **Waited for before replacing**, `AddShader`'s own reason: the
		// frame in flight may still be reading through whatever pipeline
		// this is about to release.
		if (State->PostProcessPipeline != nullptr) {
			(void)WaitForFrame();
			SDL_ReleaseGPUGraphicsPipeline(State->Device, State->PostProcessPipeline);
		}
		State->PostProcessPipeline = pipeline;
		State->PostProcessShaderName = name;
		return true;
	}

	void Renderer::ClearPostProcessShader() {
		if (State == nullptr || State->PostProcessPipeline == nullptr) {
			return;
		}
		if (State->Device != nullptr) {
			(void)WaitForFrame();
			SDL_ReleaseGPUGraphicsPipeline(State->Device, State->PostProcessPipeline);
		}
		State->PostProcessPipeline = nullptr;
		State->PostProcessShaderName = core::Name{};
	}

	core::Name Renderer::PostProcessShaderName() const {
		return State == nullptr ? core::Name{} : State->PostProcessShaderName;
	}

	bool Renderer::Impl::WriteCapture(
		SDL_GPUTransferBuffer *from,
		uint32_t width,
		uint32_t height,
		SDL_GPUTextureFormat format,
		const std::filesystem::path &path
	) const {
		void *mapped = SDL_MapGPUTransferBuffer(Device, from, false);
		if (mapped == nullptr) {
			ENGINE_ERROR("SDL_MapGPUTransferBuffer: {}", SDL_GetError());
			return false;
		}

		// The swapchain's format decides the channel order, and getting it
		// wrong writes a picture with red and blue swapped - which looks like a
		// shader bug rather than like a file-writing bug, so it is worth
		// asking rather than assuming.
		SDL_PixelFormat pixels = SDL_PIXELFORMAT_UNKNOWN;
		switch (format) {
		case SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM:
		case SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB:
			pixels = SDL_PIXELFORMAT_BGRA32;
			break;
		case SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM:
		case SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB:
			pixels = SDL_PIXELFORMAT_RGBA32;
			break;
		default:
			break;
		}

		if (pixels == SDL_PIXELFORMAT_UNKNOWN) {
			ENGINE_ERROR("capture: swapchain format {} has no BMP mapping", static_cast<int>(format));
			SDL_UnmapGPUTransferBuffer(Device, from);
			return false;
		}

		SDL_Surface *surface = SDL_CreateSurfaceFrom(
			static_cast<int>(width), static_cast<int>(height), pixels, mapped, static_cast<int>(width * 4)
		);

		bool wrote = false;
		if (surface != nullptr) {
			wrote = SDL_SaveBMP(surface, path.string().c_str());
			if (!wrote) {
				ENGINE_ERROR("SDL_SaveBMP: {}", SDL_GetError());
			}
			SDL_DestroySurface(surface);
		} else {
			// Every other failure here says so, and this one used to return
			// false in silence - a capture that produced no file and no reason
			// reads as the request having been ignored.
			ENGINE_ERROR("capture: SDL_CreateSurfaceFrom: {}", SDL_GetError());
		}

		SDL_UnmapGPUTransferBuffer(Device, from);
		return wrote;
	}

	void Renderer::RequestSceneCapture(std::filesystem::path path, size_t slot) {
		State->CapturePath = std::move(path);
		State->CaptureSlot = slot;
	}

	bool Renderer::IsHeadless() const {
		return State->Headless();
	}

	void *Renderer::SceneTexture(size_t slot) const {
		return slot < State->SceneSlots.size() ? State->SceneSlots[slot].Texture : nullptr;
	}

	void *Renderer::ResourceTexture(core::Name resource, size_t slot) const {
		if (State == nullptr || !resource.IsValid()) {
			return nullptr;
		}
		const Impl::NamedPipeline *pipeline = State->PipelineFor(State->ActiveGraph);
		if (pipeline != nullptr) {
			for (const Impl::GraphTarget &target : State->GraphTargets) {
				if (target.Pipeline != pipeline->Name || target.Resource != resource ||
					target.Texture == nullptr) {
					continue;
				}
				if (target.Scope != graph::NodeScope::View || target.Owner == slot) {
					return target.Texture;
				}
			}
		}
		const Impl::ResourceRole role = State->RoleFor(resource);
		if (role == Impl::ResourceRole::PreviousFrame) {
			if (slot >= State->SceneSlots.size()) {
				return nullptr;
			}
			const Impl::SceneSlot &history = State->SceneSlots[slot];
			return history.HistoryReady ? history.History : nullptr;
		}
		if (role == Impl::ResourceRole::Scene) {
			return SceneTexture(slot);
		}
		if (role == Impl::ResourceRole::Depth) {
			return slot < State->SceneSlots.size() ? State->SceneSlots[slot].Depth : nullptr;
		}
		if (role == Impl::ResourceRole::Shadow) {
			return State->ShadowTexture;
		}
		if (role == Impl::ResourceRole::Surface && slot < State->SurfaceBanks.size()) {
			const Impl::SurfaceSlotState &surface = State->SurfaceBanks[slot].Surfaces[0];
			return surface.Ready ? surface.Texture[surface.Slot] : nullptr;
		}
		if ((role == Impl::ResourceRole::PortalImage || role == Impl::ResourceRole::PortalDisplay) &&
			slot < State->SurfaceBanks.size()) {
			const std::vector<Impl::PortalLevel> &levels = State->SurfaceBanks[slot].Portals;
			for (auto level = levels.rbegin(); level != levels.rend(); ++level) {
				for (const Impl::PortalTarget &portal : level->Targets) {
					SDL_GPUTexture *texture =
						role == Impl::ResourceRole::PortalImage ? portal.Colour : portal.Display;
					if (texture != nullptr) {
						return texture;
					}
				}
			}
			return nullptr;
		}
		if (role == Impl::ResourceRole::PortalLight && slot < State->SurfaceBanks.size()) {
			for (const Impl::SeamLightTarget &seamLight : State->SurfaceBanks[slot].SeamLights) {
				if (seamLight.Ready && seamLight.Colour != nullptr) {
					return seamLight.Colour;
				}
			}
			return nullptr;
		}
		if (slot >= State->PbrSlots.size()) {
			return nullptr;
		}

		const Impl::PbrSlot &pbr = State->PbrSlots[slot];
		if (role == Impl::ResourceRole::Albedo) {
			return pbr.Albedo;
		}
		if (role == Impl::ResourceRole::Normal) {
			return pbr.Normal;
		}
		if (role == Impl::ResourceRole::Material) {
			return pbr.Material;
		}
		if (role == Impl::ResourceRole::Emissive) {
			return pbr.Emissive;
		}
		if (role == Impl::ResourceRole::LinearDepth) {
			return pbr.LinearDepth;
		}
		if (role == Impl::ResourceRole::Occlusion) {
			return pbr.Occlusion;
		}
		if (role == Impl::ResourceRole::Lit) {
			return pbr.Lit;
		}
		return nullptr;
	}

	void Renderer::RefreshResourcePreview(
		core::Name pipeline, core::Name resource, size_t slot, bool reverseSpectrum
	) {
		if (State == nullptr || !pipeline.IsValid() || !resource.IsValid()) {
			return;
		}
		const ResourcePreviewRoute route{pipeline, resource, slot};
		for (Impl::ResourcePreviewTarget &preview : State->ResourcePreviews) {
			if (preview.Route == route) {
				preview.ReverseSpectrum = reverseSpectrum;
				preview.Refresh = true;
				return;
			}
		}
		Impl::ResourcePreviewTarget preview;
		preview.Route = route;
		preview.ReverseSpectrum = reverseSpectrum;
		State->ResourcePreviews.push_back(std::move(preview));
	}

	void *Renderer::ResourcePreviewTexture(core::Name pipeline, core::Name resource, size_t slot) const {
		if (State == nullptr || !pipeline.IsValid() || !resource.IsValid()) {
			return nullptr;
		}
		const ResourcePreviewRoute route{pipeline, resource, slot};
		for (const Impl::ResourcePreviewTarget &preview : State->ResourcePreviews) {
			if (preview.Route == route) {
				return preview.Slots.Ready ? preview.Textures[preview.Slots.Visible] : nullptr;
			}
		}
		return nullptr;
	}

	bool Renderer::CaptureSceneTexture(size_t slot, const core::Name &name) {
		if (State == nullptr || State->Device == nullptr || !name.IsValid()) {
			return false;
		}
		if (slot >= State->SceneSlots.size()) {
			return false;
		}

		const Impl::SceneSlot &source = State->SceneSlots[slot];
		if (source.Texture == nullptr || source.DrawnWidth == 0 || source.DrawnHeight == 0) {
			return false;
		}

		// **The drawn rectangle and not the whole target.** A scene target is
		// allocated in 64-pixel blocks with hysteresis, so most of it is border
		// the pass never wrote - `SceneTextureExtent` exists because of exactly
		// that. Copying the allocation would keep a picture with an unwritten
		// margin down two edges, and every consumer would then need the extent
		// as well as the handle, which is the coupling this call is meant to end.
		SDL_GPUTextureCreateInfo info{};
		info.type = SDL_GPU_TEXTURETYPE_2D;

		// The source's format, for `EnsureSceneTarget`'s reason one level up: a
		// blit between mismatched formats is a validation error on the backends
		// that check and a corrupt image on the ones that do not.
		info.format = State->ColourFormat();

		// Sampled, and a colour target because `SDL_BlitGPUTexture` renders into
		// its destination rather than copying into it.
		info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
		info.width = source.DrawnWidth;
		info.height = source.DrawnHeight;
		info.layer_count_or_depth = 1;
		info.num_levels = 1;

		SDL_GPUTexture *copy = SDL_CreateGPUTexture(State->Device, &info);
		if (copy == nullptr) {
			ENGINE_ERROR("SDL_CreateGPUTexture (scene capture): {}", SDL_GetError());
			return false;
		}

		SDL_GPUCommandBuffer *command = SDL_AcquireGPUCommandBuffer(State->Device);
		if (command == nullptr) {
			ENGINE_ERROR("SDL_AcquireGPUCommandBuffer (scene capture): {}", SDL_GetError());
			SDL_ReleaseGPUTexture(State->Device, copy);
			return false;
		}

		// **A blit rather than a copy pass, because the source region is a
		// sub-rectangle.** `SDL_CopyGPUTextureToTexture` would work only if the
		// two agreed on size, which they deliberately do not.
		SDL_GPUBlitInfo blit{};
		blit.source.texture = source.Texture;
		blit.source.w = source.DrawnWidth;
		blit.source.h = source.DrawnHeight;
		blit.destination.texture = copy;
		blit.destination.w = source.DrawnWidth;
		blit.destination.h = source.DrawnHeight;

		// Nothing is preserved, because every texel of the destination is
		// written - saying so lets a tiler skip loading it.
		blit.load_op = SDL_GPU_LOADOP_DONT_CARE;
		blit.filter = SDL_GPU_FILTER_NEAREST;

		SDL_BlitGPUTexture(command, &blit);
		SDL_SubmitGPUCommandBuffer(command);

		// **Four bytes a texel, which is the honest figure for every format this
		// swapchain uses.** Guessing low here would let the table's ceiling be
		// walked past by whoever captures the most.
		const size_t bytes = static_cast<size_t>(source.DrawnWidth) * source.DrawnHeight * 4;

		if (!State->Textures.Adopt(name, copy, source.DrawnWidth, source.DrawnHeight, bytes)) {
			// Refused, so the texture is still ours to release - `Adopt` says so.
			SDL_ReleaseGPUTexture(State->Device, copy);
			return false;
		}

		return true;
	}

	SceneExtent Renderer::SceneTextureExtent(size_t slot) const {
		if (slot >= State->SceneSlots.size()) {
			return {};
		}

		const Impl::SceneSlot &target = State->SceneSlots[slot];

		// **The whole texture when nothing has been drawn yet**, which is the
		// honest answer rather than a safe one: a caller sampling a texture no
		// pass has written is showing uninitialised memory whatever the
		// coordinates say, and a fraction of it is not better than all of it.
		if (target.Width == 0 || target.Height == 0) {
			return {};
		}

		return SceneExtent{
			static_cast<float>(target.DrawnWidth) / static_cast<float>(target.Width),
			static_cast<float>(target.DrawnHeight) / static_cast<float>(target.Height),
			target.DrawnWidth,
			target.DrawnHeight,
		};
	}

	SceneExtent Renderer::ResourceTextureExtent(core::Name resource, size_t slot) const {
		if (State == nullptr) {
			return {};
		}
		const Impl::NamedPipeline *pipeline = State->PipelineFor(State->ActiveGraph);
		if (pipeline != nullptr) {
			for (const Impl::GraphTarget &target : State->GraphTargets) {
				if (target.Pipeline != pipeline->Name || target.Resource != resource ||
					target.Texture == nullptr ||
					(target.Scope == graph::NodeScope::View && target.Owner != slot)) {
					continue;
				}
				return SceneExtent{1.0f, 1.0f};
			}
		}
		const Impl::ResourceRole role = State->RoleFor(resource);
		if (role == Impl::ResourceRole::PreviousFrame || role == Impl::ResourceRole::Shadow ||
			role == Impl::ResourceRole::Surface || role == Impl::ResourceRole::PortalImage ||
			role == Impl::ResourceRole::PortalDisplay) {
			return SceneExtent{1.0f, 1.0f};
		}
		if (role == Impl::ResourceRole::Scene || role == Impl::ResourceRole::Depth) {
			return SceneTextureExtent(slot);
		}
		if (slot >= State->PbrSlots.size()) {
			return {};
		}

		const Impl::PbrSlot &pbr = State->PbrSlots[slot];
		const Impl::PbrDimensions &dimensions = pbr.Dimensions;
		if (dimensions.TargetWidth == 0 || dimensions.TargetHeight == 0 || dimensions.ViewWidth == 0 ||
			dimensions.ViewHeight == 0) {
			return {};
		}

		if (role == Impl::ResourceRole::Albedo || role == Impl::ResourceRole::Normal ||
			role == Impl::ResourceRole::Material || role == Impl::ResourceRole::Emissive) {
			return SceneExtent{
				static_cast<float>(dimensions.ViewWidth) / static_cast<float>(dimensions.TargetWidth),
				static_cast<float>(dimensions.ViewHeight) / static_cast<float>(dimensions.TargetHeight),
			};
		}
		return SceneExtent{1.0f, 1.0f};
	}

	BackendHandles Renderer::Backend() const {
		BackendHandles handles;
		if (State->Device != nullptr) {
			handles.Device = State->Device;
			handles.ColourFormat = static_cast<uint32_t>(State->ColourFormat());
		}
		return handles;
	}

	FrameResult Renderer::Render(
		std::span<const View> views,
		OverlayImage &overlay,
		FrameOverlayHook *gameInterfaceHook,
		bool present,
		FrameOverlayHook *hostOverlayHook
	) {
		ENGINE_PROFILE_CAT("Renderer::Render views", core::ProfileCategory::Render);
		RequireOwningThread("Render views");

		FrameResult frame;
		if (State == nullptr || State->Device == nullptr || views.empty() || State->BatchActive) {
			return frame;
		}

		struct ViewGroup {
			uint64_t World = 0;
			core::Name Pipeline;
			std::vector<size_t> Views;
		};
		std::vector<ViewGroup> groups;
		groups.reserve(views.size());
		for (size_t index = 0; index < views.size(); index++) {
			const View &view = views[index];
			auto found = std::find_if(groups.begin(), groups.end(), [&](const ViewGroup &group) {
				return group.World == view.World && group.Pipeline == view.Pipeline;
			});
			if (found == groups.end()) {
				groups.push_back({view.World, view.Pipeline, {index}});
			} else {
				found->Views.push_back(index);
			}
		}

		// Presentation belongs to the caller's last view. Keep that group last so
		// another world's shared targets cannot overwrite it before its view runs.
		if (present) {
			const size_t finalView = views.size() - 1;
			const auto finalGroup = std::find_if(groups.begin(), groups.end(), [&](const ViewGroup &group) {
				return std::find(group.Views.begin(), group.Views.end(), finalView) != group.Views.end();
			});
			if (finalGroup != groups.end() && std::next(finalGroup) != groups.end()) {
				std::rotate(finalGroup, std::next(finalGroup), groups.end());
			}
		}

		std::vector<size_t> order;
		order.reserve(views.size());
		for (const ViewGroup &group : groups) {
			order.insert(order.end(), group.Views.begin(), group.Views.end());
		}
		for (size_t position = 0; position + 1 < order.size(); position++) {
			const SceneTarget *target = views[order[position]].Target;
			if (target == nullptr || !target->IsValid()) {
				ENGINE_ERROR("render batch view {} has no offscreen target", order[position]);
				return frame;
			}
		}

		SDL_GPUCommandBuffer *command = nullptr;
		SDL_GPUTexture *swapchain = nullptr;
		uint32_t width = 0;
		uint32_t height = 0;
		if (present) {
			if (!State->BeginFrame()) {
				return frame;
			}
			State->TakeFrame(command, swapchain, width, height);
		} else {
			command = SDL_AcquireGPUCommandBuffer(State->Device);
			if (command == nullptr) {
				ENGINE_ERROR("SDL_AcquireGPUCommandBuffer (view batch): {}", SDL_GetError());
				return frame;
			}
		}

		const SceneTarget *finalTarget = views[order.back()].Target;
		if (swapchain == nullptr && (finalTarget == nullptr || !finalTarget->IsValid())) {
			// A headless Studio has no swapchain and its viewport has no extent
			// until the first interface layout. That frame has nowhere to draw by
			// design. A windowed caller reaching the same state lost its target.
			if (!State->Headless()) {
				ENGINE_ERROR("render batch final view has neither a swapchain nor an offscreen target");
			}
			SDL_SubmitGPUCommandBuffer(command);
			return frame;
		}

		const scene::WorldLighting previousLighting = CurrentLighting();
		State->BatchActive = true;
		State->BatchFailed = false;
		State->BatchCommand = command;
		State->BatchSwapchain = swapchain;
		State->BatchWidth = width;
		State->BatchHeight = height;
		State->BatchTimingSlot = VulkanTimestamps::NO_SLOT;

		size_t position = 0;
		for (size_t groupIndex = 0; groupIndex < groups.size() && !State->BatchFailed; groupIndex++) {
			const ViewGroup &group = groups[groupIndex];
			for (size_t member = 0; member < group.Views.size(); member++, position++) {
				const size_t viewIndex = group.Views[member];
				const View &view = views[viewIndex];
				SetLighting(view.OverrideLighting ? view.Lighting : previousLighting);

				State->BatchFirst = position == 0;
				State->BatchFinal = position + 1 == order.size();
				State->BatchShared = member == 0;
				State->BatchViewIndex = viewIndex;
				State->BatchWorldIndex = groupIndex;

				frame.Accumulate(RenderView(
					view.CameraFrame,
					view.Camera,
					view.Instances,
					overlay,
					view.Surfaces,
					gameInterfaceHook,
					State->BatchFinal ? hostOverlayHook : nullptr,
					view.Target,
					view.Slot,
					view,
					view.Particles,
					view.RibbonVertices,
					view.RibbonRuns,
					view.Lights,
					view.Foreign,
					view.Portals,
					State->BatchFinal && present,
					view.Pipeline,
					view.World
				));
				if (State->BatchFailed) {
					break;
				}
			}
		}

		for (const ViewGroup &group : groups) {
			const Impl::NamedPipeline *named = State->PipelineFor(group.Pipeline);
			if (named == nullptr) {
				continue;
			}
			uint32_t planWidth = width;
			uint32_t planHeight = height;
			std::vector<uint64_t> worlds;
			worlds.reserve(group.Views.size());
			for (const size_t viewIndex : group.Views) {
				const View &view = views[viewIndex];
				worlds.push_back(view.World);
				if (view.Target != nullptr && view.Target->IsValid()) {
					planWidth = std::max(planWidth, view.Target->Width);
					planHeight = std::max(planHeight, view.Target->Height);
				}
			}
			planWidth = std::max(planWidth, 1u);
			planHeight = std::max(planHeight, 1u);

			graph::FrameExecutionPlan plan;
			core::Name offender;
			if (graph::PlanFrame(
					named->Graph, named->Schedule, worlds, planWidth, planHeight, plan, offender
				) == graph::ExecutionPlanStatus::Ok) {
				frame.ScheduledReadBytes += plan.ReadBytes;
				frame.ScheduledWriteBytes += plan.WriteBytes;
				frame.QueueTransferBytes += plan.QueueTransferBytes;
				frame.ConcurrentWaves += static_cast<uint32_t>(
					std::count_if(plan.Waves.begin(), plan.Waves.end(), [](const graph::PlannedWave &wave) {
						return wave.ConcurrentQueues;
					})
				);
				frame.TrafficCommandBuffers += static_cast<uint32_t>(named->Buffers.size());
			}
		}

		SetLighting(previousLighting);
		if (State->BatchCommand != nullptr) {
			State->Timestamps.Abandon(State->BatchTimingSlot);
			if (State->BatchTimingSlot < VulkanTimestamps::SLOTS) {
				State->PendingMarks[State->BatchTimingSlot].clear();
			}
			SDL_SubmitGPUCommandBuffer(State->BatchCommand);
			State->BatchCommand = nullptr;

			// A failed batch never reached the final view's submit, so any
			// downloads an earlier view recorded still hold their buffer.
			State->DropDownloads();
		}
		State->BatchActive = false;
		State->BatchFirst = false;
		State->BatchFinal = false;
		State->BatchShared = false;
		State->BatchFailed = false;
		State->BatchSwapchain = nullptr;
		State->BatchWidth = 0;
		State->BatchHeight = 0;
		State->BatchTimingSlot = VulkanTimestamps::NO_SLOT;
		return frame;
	}

	FrameResult Renderer::RenderView(
		const core::CFrame &cameraFrame,
		const scene::Camera &camera,
		std::span<const scene::DrawInstance> instances,
		OverlayImage &overlay,
		std::span<const SurfaceView> surfaces,
		FrameOverlayHook *gameInterfaceHook,
		FrameOverlayHook *hostOverlayHook,
		const SceneTarget *sceneTarget,
		size_t targetSlot,
		const View &source,
		std::span<const ParticleBatch> particles,
		std::span<const effects::RibbonVertex> ribbonVertices,
		std::span<const effects::RibbonRun> ribbonRuns,
		std::span<const SceneLight> lights,
		std::span<const scene::DrawInstance> foreign,
		std::span<const PortalView> portals,
		bool present,
		core::Name pipeline,
		uint64_t world
	) {
		ENGINE_PROFILE_CAT("Renderer::RenderView", core::ProfileCategory::Render);

		// **The single-threaded recording contract, checked rather than
		// described.** A studio draws one viewport after another; a second one
		// recording from another thread is the failure this refuses. See
		// `IsOnOwningThread` for why that is the design and not a limitation.
		RequireOwningThread("RenderView");

		// **Which target this frame draws into, read by `EnsureScene`.** Passed
		// through a member rather than an argument because `EnsureScene` is
		// called from two places and threading a slot through both would put
		// the same value in two signatures that must agree.
		State->ActiveSlot = targetSlot;

		FrameResult result;
		if (!State->Device) {
			return result;
		}

		State->ActiveGraph = pipeline;
		const Impl::NamedPipeline *selectedPipeline = State->PipelineFor(pipeline);
		if (selectedPipeline == nullptr) {
			ENGINE_ERROR("render graph '{}' is not installed", pipeline.Text());
			State->BatchFailed = State->BatchActive;
			return result;
		}
		NodeTable frameNodes = BackendTable([](const graph::RunContext &) { return true; });
		const auto graphNode = [&](core::Name kind) -> const graph::Node * {
			for (size_t index = 0; index < selectedPipeline->Graph.Count(); index++) {
				const graph::Node *node =
					selectedPipeline->Graph.Find(graph::NodeId{static_cast<uint32_t>(index + 1)});
				if (node != nullptr && node->Enabled && node->Kind == kind) {
					return node;
				}
			}
			return nullptr;
		};
		const auto graphEnabled = [&](core::Name kind) { return graphNode(kind) != nullptr; };
		const auto scheduledFor = [&](graph::NodeId id) -> const graph::ScheduledNode * {
			for (const graph::ExecutionWave &wave : selectedPipeline->Schedule.Waves) {
				for (const graph::ScheduledNode &scheduled : wave.Nodes) {
					if (scheduled.Node == id) {
						return &scheduled;
					}
				}
			}
			return nullptr;
		};

		// How close the nearest hole is, and the near plane that follows from it.
		//
		// **The near plane a portal needs is not the one a scene authors.** Walk
		// up to a doorway with a hole in it and the last hand's width of the
		// approach is the whole illusion: an authored near plane slices the pane
		// open there and the wall beside it disappears. `scene::PortalNearPlane`
		// trades depth precision for that, and only while a hole is close enough
		// to need it - CodeParade's `GH_CLAMP(NearestPortalDist() * 0.5f, ...)`,
		// which the demo applies to its one and only camera.
		//
		// **Measured off the panes this frame was handed rather than off the
		// world**, because that is the same set the pass below draws through and
		// the renderer has no store to ask. `scene::ResolveActiveCamera` answers
		// the same question from the seams for culling, from the same functions.
		float nearestPane = std::numeric_limits<float>::infinity();
		for (const PortalView &portal : portals) {
			nearestPane = std::min(
				nearestPane,
				scene::RectangleDistance(portal.Centre, portal.First, portal.Second, cameraFrame.Position)
			);
		}

		// **One adapted copy used by every projection this frame builds**, so the
		// cull, the portal recursion and the opaque draw cannot disagree about
		// where the near plane is. A cull run against a larger near plane than
		// the draw uses throws away exactly the geometry the smaller one exists
		// to keep.
		scene::Camera drawCamera = camera;
		drawCamera.NearPlane = scene::PortalNearPlane(camera.NearPlane, nearestPane);

		// **Claimed here only if the caller did not claim it first.** `WaitForFrame`
		// is what a latency-sensitive loop calls before it reads its input; a
		// caller that does not is no worse off than before, because this is the
		// same acquisition at the same point in the frame. See `Impl::BeginFrame`.
		SDL_GPUCommandBuffer *command = nullptr;
		SDL_GPUTexture *swapchain = nullptr;
		uint32_t width = 0;
		uint32_t height = 0;
		if (State->BatchActive) {
			command = State->BatchCommand;
			if (State->BatchFinal) {
				swapchain = State->BatchSwapchain;
				width = State->BatchWidth;
				height = State->BatchHeight;
			}
		} else if (present) {
			if (!State->BeginFrame()) {
				return result;
			}
			State->TakeFrame(command, swapchain, width, height);
		} else {
			command = SDL_AcquireGPUCommandBuffer(State->Device);
			if (command == nullptr) {
				ENGINE_ERROR("SDL_AcquireGPUCommandBuffer (texture-only): {}", SDL_GetError());
				return result;
			}
		}
		if (State->BatchFirst || !State->BatchActive) {
			State->FrameCounter++;
			State->PreviewSubmitted = false;
			State->PollPreview();
		}
		const auto endIncompleteView = [&] {
			if (State->BatchActive) {
				// The batch owner drops any recorded downloads with the frame.
				State->BatchFailed = true;
			} else {
				SDL_SubmitGPUCommandBuffer(command);
				State->DropDownloads();
			}
		};

		// **Headless has no swapchain, so its size comes from the target** -
		// nothing else has an opinion about it.
		if (swapchain == nullptr) {
			if (sceneTarget == nullptr || !sceneTarget->IsValid()) {
				// A headless renderer with nowhere to draw is a caller mistake
				// rather than a state to tolerate: every pass would run and its
				// result would be discarded.
				endIncompleteView();
				return result;
			}

			width = sceneTarget->Width;
			height = sceneTarget->Height;
		}

		// --- where the world goes -------------------------------------------
		//
		// Resolved once, here, and everything downstream reads `sceneWidth` and
		// `sceneHeight` rather than the swapchain's. That is the whole reason
		// this is a few lines in one place instead of a conditional at each use:
		// the cull frustum, the projection and the depth buffer all have to
		// agree about how big the image is, and they are decided hundreds of
		// lines apart.
		//
		// A target that cannot be allocated falls back to the window rather than
		// dropping the frame. A caller asking for a texture and getting a frame
		// it did not expect can see that something is wrong; one that gets no
		// frame at all sees a frozen editor.
		const bool offscreen = sceneTarget != nullptr && sceneTarget->IsValid() &&
							   State->EnsureScene(sceneTarget->Width, sceneTarget->Height);

		if (State->Headless() && !offscreen) {
			// The target could not be allocated. Headless has no window to fall
			// back to, so the frame ends here rather than drawing into nothing.
			endIncompleteView();
			return result;
		}

		if (!offscreen && State->SlotAt(targetSlot).Texture != nullptr) {
			// Nothing asked for a texture this frame, so last frame's is
			// released rather than kept against a caller who might come back. A
			// viewport panel that was closed should not go on costing its
			// pixels.
			State->EnsureScene(0, 0);
		}

		const uint32_t sceneWidth = offscreen ? sceneTarget->Width : width;
		const uint32_t sceneHeight = offscreen ? sceneTarget->Height : height;

		// **What the pass is drawing *onto*, which is not what it draws.** An
		// offscreen target is allocated in blocks, so the attachment is at least
		// as big as the world's rectangle and usually bigger; the world fills
		// the corner and the viewport below is what confines it there. See
		// `SCENE_TARGET_BLOCK`.
		const uint32_t targetWidth = offscreen ? State->SlotAt(targetSlot).Width : width;
		const uint32_t targetHeight = offscreen ? State->SlotAt(targetSlot).Height : height;

		{
			// Nothing at all on a steady window, and a texture allocation on the
			// frame after a resize. Worth telling apart from the pass that uses
			// it, because one is every frame and the other is one frame.
			//
			// **Sized to the attachment rather than to the world.** SDL wants a
			// depth target whose dimensions match the colour target it is bound
			// beside, and the colour target is the block-rounded allocation -
			// not the rectangle the world is drawn into. Sizing this to the
			// world instead is a validation failure on the frames where the two
			// differ, which is nearly all of them.
			//
			// **The slot's own depth when drawing offscreen.** Two viewports of
			// different sizes sharing one depth texture made every frame
			// reallocate it twice - see `SceneSlot::Depth`.
			ENGINE_PROFILE_CAT("ensure depth", core::ProfileCategory::Render);

			bool depthReady = false;
			if (offscreen) {
				Impl::SceneSlot &slot = State->SlotAt(targetSlot);
				depthReady = State->EnsureDepthIn(
					slot.Depth, slot.DepthWidth, slot.DepthHeight, targetWidth, targetHeight
				);
			} else {
				depthReady = State->EnsureDepth(targetWidth, targetHeight);
			}

			if (!depthReady) {
				endIncompleteView();
				return result;
			}
		}

		// --- what is ready to be drawn ---------------------------------------
		//
		// **An instance naming a mesh this table does not hold is not drawn at
		// all**, and the distinction from an instance naming *no* mesh is the
		// whole of it:
		//
		//   - no mesh named - an ordinary `Part` - draws the default cube, which
		//     is what a part is.
		//   - a mesh named and not resident - a `MeshPart` whose geometry has
		//     not arrived - draws nothing.
		//
		// Without the second, `MeshTable::Resolve` hands back the default and a
		// scene of mesh parts comes up as a field of cubes that turn into models
		// one by one as the content lands. That is worse than an empty space: an
		// empty space reads as "still loading" and a wrong cube reads as the
		// asset being broken.
		//
		// **Filtered once here rather than inside the cull and the scene gather
		// separately.** Both read this span, and a test written into each would
		// be two places to keep in step - the exact duplication that made the
		// mirror pass and the camera pass disagree about `Transparency` before
		// `OrderScene` was one function.
		//
		// A frame where everything named is loaded copies the span and does one
		// hash lookup per instance, which is nothing beside the hundred and
		// fifty bytes of traffic per instance the collector already pays.
		{
			ENGINE_PROFILE_CAT("filter unloaded", core::ProfileCategory::Render);

			scene::KeepLoaded(
				instances, [this](const core::Name &mesh) { return State->Meshes.Has(mesh); }, State->Drawable
			);

			// **The other worlds pay the same toll.** A destination whose meshes
			// have not arrived here would otherwise come up as the field of
			// cubes described above - seen through a portal, which is the one
			// place a viewer cannot walk over and check.
			scene::KeepLoaded(
				foreign,
				[this](const core::Name &mesh) { return State->Meshes.Has(mesh); },
				State->DrawableForeign
			);
		}
		instances = State->Drawable;
		foreign = State->DrawableForeign;

		// --- uploads --------------------------------------------------------

		const auto totalCount = static_cast<uint32_t>(instances.size());
		bool haveInstances = false;
		bool haveOverlay = false;

		// **Culled, then ordered, then uploaded** - and the sequence is the
		// point. Culling first means the sort runs over what survives rather
		// than over the world, and the upload carries only what is drawn.
		//
		// The frustum comes from the same `ResolveCamera` the draw does, so it
		// cannot disagree with what was actually projected. A frustum built from
		// a field of view and an aspect ratio kept separately is the bug that
		// pops geometry at the screen edge on one machine and not another.

		// **Every surface camera's view, resolved before any pass runs.** Each
		// is used twice: to render into its own texture now, and - one frame
		// later, as `PreviousViewProjection` - to project that texture back onto
		// whatever samples it, including another mirror.
		//
		// The accepted views are gathered here rather than filtered at each use,
		// so the two passes that draw mirrors iterate the same list and cannot
		// disagree about which indices are live. A duplicate index is the one
		// case worth refusing outright: two views writing one texture would race
		// for the pair and neither would be the frame the screen then samples.
		struct AcceptedView {
			uint8_t Index = 0;
			const SurfaceView *View = nullptr;

			// **Held here rather than written straight to the slot**, because
			// whether it may be written is not known yet. A slot's
			// `ViewProjection` has to keep describing the camera that rendered
			// the texture the slot holds - so a surface that turns out to be
			// unchanged, and therefore does not re-render, must not take this
			// frame's matrix. See the refresh decision below.
			glm::mat4 ViewProjection{1.0f};

			// The same with the pane's map folded in, which is what the pane
			// reads the image back through. `SurfaceView::Mapping` says why they
			// are two matrices rather than one.
			glm::mat4 Sampling{1.0f};

			float ImageOpacity = 1.0f;

			// What the pane puts the image through. Carried for the same reason
			// the opacity is: it is composited with, not rendered with.
			scene::SurfaceEffect Effect = scene::SurfaceEffect::None;

			// Whether this surface renders this frame. False when its signature
			// matches the one its texture was drawn with.
			bool Refresh = true;
		};
		AcceptedView accepted[scene::MAX_SURFACES];
		size_t acceptedCount = 0;
		bool claimed[scene::MAX_SURFACES] = {};

		// **This viewport's surfaces, and not the renderer's.** A reflection is
		// of the viewer, so a world drawn from two panels wants two images per
		// pane. Resolved once here and read everywhere below, so no pass can
		// reach the wrong bank. See `Impl::SurfaceBanks`.
		Impl::SurfaceBank &bank = State->SurfacesAt(targetSlot);

		// **The holes, claimed before the surfaces are**, so that a slot named by
		// both goes to the recursive pass. That order is the answer rather than a
		// tie-break: a same-world portal handed to the surface path draws from the
		// wrong viewpoint the moment it is seen through another hole, which is the
		// whole reason this pass exists - whereas a portal drawn recursively and
		// *also* given a surface camera merely wastes a scene pass on a texture
		// nothing samples.
		const PortalView *portalOf[scene::MAX_SURFACES] = {};
		bool havePortals = false;
		for (const PortalView &portal : portals) {
			if (portal.Index < 0 || static_cast<uint8_t>(portal.Index) >= scene::MAX_SURFACES) {
				ENGINE_WARN(
					"portal index {} is outside 0..{}, so it draws flat",
					portal.Index,
					scene::MAX_SURFACES - 1
				);
				continue;
			}

			const auto index = static_cast<uint8_t>(portal.Index);
			if (portalOf[index] != nullptr) {
				ENGINE_WARN("two portals claim index {}; the second is ignored", portal.Index);
				continue;
			}

			portalOf[index] = &portal;
			claimed[index] = true;
			havePortals = true;
		}

		// **Read once here, so the pass that fills the pool and the pass that
		// samples it cannot disagree about how many levels there are.** The top
		// level is `portalLevels - 1`, which is the index the transparent surface
		// composition reads.
		const uint32_t portalLevels = std::min(State->PortalDepth, MAX_PORTAL_DEPTH);

		for (const SurfaceView &view : surfaces) {
			if (view.Index < 0 || static_cast<uint8_t>(view.Index) >= scene::MAX_SURFACES) {
				ENGINE_WARN(
					"surface camera index {} is outside 0..{}, so it renders nothing",
					view.Index,
					scene::MAX_SURFACES - 1
				);
				continue;
			}

			const auto index = static_cast<uint8_t>(view.Index);
			if (portalOf[index] != nullptr) {
				ENGINE_WARN(
					"surface camera {} names a slot a portal already draws; the recursive pass keeps it",
					view.Index
				);
				continue;
			}
			if (claimed[index]) {
				ENGINE_WARN("two surface cameras claim index {}; the second is ignored", view.Index);
				continue;
			}

			claimed[index] = true;

			// **No aspect ratio, and its absence is load-bearing.** A surface
			// frustum is fitted to the pane's four corners, so the texture's
			// shape is already inside the extents that produced this
			// projection; widening by the aspect again here would apply it
			// twice and stretch every mirror in the scene.
			AcceptedView entry;
			entry.Index = index;
			entry.View = &view;
			entry.ViewProjection = scene::ResolveSurfaceCamera(view.Frame, view.Projection).ViewProjection;
			entry.Sampling = entry.ViewProjection * view.Mapping;
			entry.ImageOpacity = std::clamp(view.ImageOpacity, 0.0f, 1.0f);
			entry.Effect = view.Effect;

			accepted[acceptedCount++] = entry;
		}

		// **The panes, by slot, for the levels below the first.** A surface
		// camera arrives here already placed - from the eye - and that is the one
		// viewpoint a recursion cannot use: a pane appearing inside another pane's
		// picture is looked at from *that* pane's camera. `scene::ReflectCamera`
		// will place it for any viewer, and what it needs is the rectangle, which
		// is what `SurfaceView::PaneNormal` carries.
		//
		// **Zero normal means "do not descend", and it is the ordinary case for
		// two kinds of view.** A camera parented to the world has no face to
		// reflect through, and a cross-world pane's picture is another simulation
		// rather than this one's geometry. Both keep the single eye-derived image
		// they have always had.
		scene::SurfacePane panes[scene::MAX_SURFACES];
		bool havePanes[scene::MAX_SURFACES] = {};
		bool anyPane = false;

		// The authored texture size of each pane, which is what the levels below
		// the first are rendered at. See the allocation in the recursion for why
		// they do not take the top level's screen-coverage scaling.
		uint32_t paneWidth[scene::MAX_SURFACES] = {};
		uint32_t paneHeight[scene::MAX_SURFACES] = {};

		for (size_t index = 0; index < acceptedCount; index++) {
			const SurfaceView &view = *accepted[index].View;
			if (view.PaneNormal.Magnitude() <= 0.0f || view.InstanceCount > 0) {
				continue;
			}

			scene::SurfacePane &pane = panes[accepted[index].Index];
			pane.Centre = view.PaneCentre;
			pane.Normal = view.PaneNormal;
			pane.First = view.PaneFirst;
			pane.Second = view.PaneSecond;
			pane.Surface = static_cast<int8_t>(accepted[index].Index);
			pane.TagFilter = view.TagFilter;
			pane.NearPlane = view.PaneNear;
			pane.FarPlane = view.PaneFar;

			paneWidth[accepted[index].Index] = view.Width;
			paneHeight[accepted[index].Index] = view.Height;

			havePanes[accepted[index].Index] = true;
			anyPane = true;
		}

		const float cameraAspect = static_cast<float>(sceneWidth) / static_cast<float>(sceneHeight);
		const glm::mat4 cameraMatrix =
			scene::ResolveCamera(cameraFrame, drawCamera, cameraAspect).ViewProjection;
		const core::AABB sceneBounds = graph::BoundsOfAll(instances);

		graph::EntityFlow &entityFlow = State->GraphEntities;
		graph::Viewpoints &viewpoints = State->GraphViewpoints;
		entityFlow.Clear();
		viewpoints.Clear();
		std::unordered_map<uint32_t, double> cpuNodeWall;
		graph::Viewpoint fallbackViewpoint;
		fallbackViewpoint.Frame = cameraFrame;
		fallbackViewpoint.Lens = drawCamera;

		size_t visibleCount = instances.size();
		size_t opaqueCount = 0;
		core::Name orderedEntities;
		for (const graph::NodeId id : selectedPipeline->Compiled.PerView) {
			const graph::Node *node = selectedPipeline->Graph.Find(id);
			if (node == nullptr) {
				continue;
			}
			const auto started = std::chrono::steady_clock::now();
			const graph::EntityNodeRun run = graph::RunEntityNode(
				selectedPipeline->Graph,
				*node,
				instances,
				fallbackViewpoint,
				cameraAspect,
				entityFlow,
				viewpoints
			);
			if (!run.Handled) {
				continue;
			}
			visibleCount = run.Output.IsValid() ? run.Count : visibleCount;
			if (run.Ordered) {
				opaqueCount = run.Opaque;
				orderedEntities = run.Output;
			}
			const auto ended = std::chrono::steady_clock::now();
			cpuNodeWall[node->Name.Id()] +=
				std::chrono::duration<double, std::micro>(ended - started).count();
		}

		const std::span<const uint32_t> ordered = entityFlow.Get(orderedEntities);
		State->VisibleInstances.assign(instances.begin(), instances.end());
		State->DrawOrder.assign(ordered.begin(), ordered.end());
		visibleCount = ordered.size();

		// **Fitted to the whole draw list, not to what survived culling.** A
		// caster outside the camera's frustum still shadows into it, so the
		// light has to see everything - and fitting to the culled set is the
		// classic version of this bug: shadows that vanish as their casters
		// leave the screen. That is why `sceneBounds` is the union over
		// `instances` and not over `State->Visible`.
		//
		// The graph cull only changes what the view draws. Shadows still fit the
		// whole world so an off-screen caster cannot disappear from the map.
		const glm::mat4 lightViewProjection =
			graph::FitDirectionalLight(sceneBounds, core::Vector3{State->Sun.x, State->Sun.y, State->Sun.z});
		const auto transparentCount = static_cast<uint32_t>(visibleCount - opaqueCount);

		// **Surface instances moved to the back of the opaque head**, so the
		// camera range is three contiguous runs - plain opaque, then mirrors,
		// then transparent - and each is one draw with one `first_instance`.
		// Whether an instance samples the surface is per instance and the
		// uniform that says so is per draw, so the alternative is a branch on
		// data the fragment shader does not have.
		//
		// Stable, for the reason the ordering itself is: an opaque scene with no
		// mirrors must come out of this exactly as it went in.
		uint32_t surfaceInCamera = 0;
		if (opaqueCount > 0) {
			ENGINE_PROFILE_CAT("partition surfaces", core::ProfileCategory::Render);

			// **`scene::PartitionSurfaces`, not a fourth copy of it.** The
			// comment forty lines down insists the mirror partition lives in
			// `scene` "where a headless suite can get at them" - and this file
			// had two hand-rolled copies of it, which is what that sentence
			// exists to prevent. They are one function now, and it is the tested
			// one.
			surfaceInCamera = static_cast<uint32_t>(scene::PartitionSurfaces(
				State->VisibleInstances, std::span<uint32_t>(State->DrawOrder.data(), opaqueCount)
			));
		}
		const auto plainOpaque = static_cast<uint32_t>(opaqueCount) - surfaceInCamera;

		// **Grouped by index within that run, because each index owns a
		// texture.** The screen pass binds a sampler and pushes a projection per
		// surface, so what used to be one draw over "the mirrors" is one draw
		// per surface - and each has to be contiguous for that to be an offset
		// and a count rather than a per-instance branch.
		//
		// `scene::GroupSurfaces`, not a second copy of it: the scene range is
		// grouped by `OrderScene` using the same function, and two groupings
		// that disagreed would put a pane's reflection on another pane's pass.
		scene::SurfaceRun cameraRuns[scene::MAX_SURFACES];
		if (surfaceInCamera > 0) {
			scene::GroupSurfaces(
				State->VisibleInstances,
				std::span<uint32_t>(State->DrawOrder.data() + plainOpaque, surfaceInCamera),
				plainOpaque,
				true,
				cameraRuns
			);
		}

		// **And the same split at the end of the blended tail, which is what
		// makes a faded mirror still a mirror.** A part leaves the opaque head
		// the moment its `Transparency` goes above zero - and the head is where
		// the mirror flag was set, so the reflection did not dim, it vanished.
		// That reads as the surface camera having stopped rather than as an
		// ordering rule, and it is the bug this run exists to fix.
		//
		// They go *last* of everything, so they draw over the blended geometry
		// as well as the opaque. Stable, so the back-to-front sort survives
		// inside each run - see `scene::ScenePlan::TransparentSurfaces` for what
		// is given up across the two.
		uint32_t transparentSurfaces = 0;
		if (transparentCount > 0) {
			ENGINE_PROFILE_CAT("partition blended surfaces", core::ProfileCategory::Render);

			transparentSurfaces = static_cast<uint32_t>(scene::PartitionSurfaces(
				State->VisibleInstances,
				std::span<uint32_t>(State->DrawOrder.data() + opaqueCount, transparentCount)
			));
		}
		const uint32_t plainTransparent = transparentCount - transparentSurfaces;

		if (transparentSurfaces > 0) {
			scene::GroupSurfaces(
				State->VisibleInstances,
				std::span<uint32_t>(
					State->DrawOrder.data() + opaqueCount + plainTransparent, transparentSurfaces
				),
				static_cast<uint32_t>(opaqueCount) + plainTransparent,
				false,
				cameraRuns
			);
		}

		// The flip from transparency to opacity happened where each view was
		// accepted, once per surface, rather than in a shader nobody can put a
		// breakpoint in. `Impl::SurfaceSlotState::ImageOpacity` holds it.

		// **A second range holding everything, for the two passes that are not
		// the camera's.** A caster outside the camera's frustum still shadows
		// into it, and a mirror shows what is behind the viewer - so culling to
		// the eye would give shadows that vanish as their casters leave the
		// screen and a mirror that reflects only what is already on screen.
		// Both are the classic version of this mistake.
		//
		// Ordered from the surface camera when there is one, because the surface
		// pass is the only consumer that needs an order at all - a depth-only
		// pass does not care.
		// **Allocated for every accepted view before anything is ordered**, so
		// a view whose texture cannot be made drops out of the frame here rather
		// than half way through the pass loop.
		// **Whether anything can see each pane, which is the other half of the
		// refresh decision and the half this pass did not have.** The signature
		// answers "did the image change"; nothing answered "is the image
		// looked at", so a room of mirrors redrew every one of them on every
		// frame anything moved - including the ones behind the viewer and the
		// ones a wall stands in front of. That cost is per pane and the scenes
		// this exists for are the ones with several.
		//
		// **Two sweeps and deliberately not a fixed point.** A pane visible only
		// *inside another mirror* is a real case - two facing panes, or a portal
		// seen through a portal - so main-camera visibility alone would freeze
		// it. The union with the other surfaces' own frusta covers it in one
		// pass: a pane seen inside a surface that is itself on screen refreshes
		// now, and one buried two bounces deep refreshes a frame later. That is
		// the same one-frame budget the whole surface pass already runs on, and
		// iterating to closure here would spend the saving this exists for.
		// **How deep the mirrors go this frame, decided once and read twice.**
		// The cull below marks that many levels visible and the pass below draws
		// that many; a level drawn and not marked is a level culled, which is the
		// defect the two expressions were allowed to drift into once already. One
		// name is what stops it happening again.
		//
		// **A stated number wins outright and the measurement is the default.** A
		// world's `workspace.SurfaceBounces` and `--surface-bounces` both arrive
		// through `SetSurfaceBounces`, so by here the choice is only whether one
		// was made. Nothing is stated: `SurfaceBank::Bounces` is what the frame
		// before this one reached, and `scene::NextSurfaceBounces` turns it into
		// what to draw now.
		//
		// **And a frame with no pane rectangle keeps the old constant**, because
		// the iterating path is not the thing being measured - a cross-world pane
		// shows a second simulation and a camera parented to the world has no
		// face, so neither can be descended into and neither reports a depth.
		const graph::Node *mirrorCapture = graphNode(core::Name("mirror-capture"));
		const uint32_t mirrorDepthLimit = std::clamp(
			mirrorCapture != nullptr ? mirrorCapture->Integer(core::Name("max-recursion"), MAX_SURFACE_DEPTH)
									 : MAX_SURFACE_DEPTH,
			1u,
			MAX_SURFACE_DEPTH
		);
		const bool mirrorHistory = mirrorCapture == nullptr ||
								   mirrorCapture->Parameter(core::Name("feedback")) == nullptr ||
								   *mirrorCapture->Parameter(core::Name("feedback")) != "flat";
		const uint32_t surfaceBounces =
			State->SurfaceBounces > 0
				? std::min(State->SurfaceBounces, mirrorDepthLimit)
				: (anyPane ? scene::NextSurfaceBounces(bank.Bounces, mirrorDepthLimit)
						   : std::min(scene::DEFAULT_SURFACE_BOUNCES, mirrorDepthLimit));

		// What this frame reaches, which the next one reads back out of the bank.
		//
		// **Rebuilt every frame rather than accumulated.** A viewer who turns
		// away from a corridor has to come back down as fast as they went up, and
		// a running maximum would hold the deepest thing anybody ever saw for the
		// rest of the session.
		scene::SurfaceBounceProbe surfaceDepth;

		bool surfaceVisible[scene::MAX_SURFACES] = {};
		float surfaceCoverage[scene::MAX_SURFACES] = {};
		if (acceptedCount > 0) {
			graph::SurfaceEye eyes[scene::MAX_SURFACES];
			for (size_t index = 0; index < acceptedCount; index++) {
				eyes[index].ViewProjection = accepted[index].ViewProjection;
				eyes[index].Index = static_cast<int8_t>(accepted[index].Index);
			}

			// **In `graph` rather than here, and that is the seam rather than
			// tidiness.** The decision is boxes against frusta over a draw list,
			// which is what that module is, and a rule this pass held privately
			// would be a rule no suite could reach - `Renderer::Render` needs a
			// device and the answer does not.
			// **`surfaceBounces` and not a constant.** The pass below resolves
			// that many levels of surface-seen-in-surface, so a level this marks
			// invisible is a level that is *culled* rather than one frame late.
			// The two numbers were allowed to disagree when `D00112` made the
			// pass recursive, and the result was a mirror's deeper reflections
			// vanishing as the viewer turned - the moment a pane left the
			// frustum, everything it had been revealing dropped past the single
			// level this used to follow.
			//
			// **One name rather than the same expression written twice**, which
			// is what that drift cost and is why the depth is decided above
			// this block instead of inside it.
			const uint32_t cullRounds = acceptedCount > 1 ? std::max(surfaceBounces, 1u) : 1u;

			(void)graph::VisibleSurfaces(
				instances,
				cameraMatrix,
				std::span<const graph::SurfaceEye>(eyes, acceptedCount),
				std::span<bool>(surfaceVisible, scene::MAX_SURFACES),
				std::span<float>(surfaceCoverage, scene::MAX_SURFACES),
				cullRounds
			);
		}

		size_t liveCount = 0;
		for (size_t index = 0; index < acceptedCount; index++) {
			const AcceptedView &view = accepted[index];

			// **Sized to what the pane covers, not to what was authored.** The
			// authored size is a floor and the screen is a ceiling; between them
			// the target doubles as the pane grows on screen, which is what stops
			// a portal going coarse when you walk into it. See `SurfaceScale`.
			const uint32_t authored = std::max<uint32_t>(view.View->Width, view.View->Height);
			const Impl::SurfaceSlotState &sized = bank.Surfaces[view.Index];
			const uint32_t held = std::max<uint32_t>(sized.Width, sized.Height);
			const uint32_t current = authored > 0 && held >= authored ? held / authored : 1u;

			const uint32_t scale = SurfaceScale(
				authored, surfaceCoverage[view.Index], std::max<uint32_t>(sceneWidth, sceneHeight), current
			);

			if (State->EnsureSurface(
					targetSlot, view.Index, view.View->Width * scale, view.View->Height * scale
				)) {
				accepted[liveCount++] = view;
			}
		}
		acceptedCount = liveCount;

		// **Ordered from the first surface camera when there is one.** The
		// blended sort is per view and there is only one scene range, so several
		// surfaces cannot each have the tail sorted for them - the first is the
		// one that gets it, and every other surface draws that order. Blended
		// geometry inside a reflection of a reflection is therefore sorted for
		// the wrong eye, which is a compositing error confined to the second
		// bounce and cheaper than an ordering pass per surface.
		const bool wantSurface = acceptedCount > 0;
		const core::Vector3 sceneEye = wantSurface ? accepted[0].View->Frame.Position : cameraFrame.Position;

		// **One signature shared by every surface, and that is not a shortcut.**
		// Each camera's matrix is in it because a surface pass draws the *other*
		// mirrors, projecting each one's texture with the camera that rendered
		// it - so a camera that moves changes how it appears inside every other
		// one. Every input to any surface is therefore an input to all of them,
		// and computing several separately would only be several chances for
		// them to disagree.
		//
		// It is still stored per slot rather than once, because slots do not
		// refresh together: one that has never rendered has nothing to compare
		// against, and one that appeared this frame has to draw once before it
		// can be skipped.
		// The frame clock, which is what a rate cap is measured against.
		//
		// **`SetAnimationTime`'s, because the renderer already has exactly one
		// idea of what time it is** and a second clock read here would let a
		// surface's interval drift against the flipbooks in it.
		const double frameSeconds = State->AnimationSeconds;

		uint64_t surfaceSignature = 0;
		size_t refreshCount = 0;
		if (wantSurface) {
			ENGINE_PROFILE_CAT("surface signature", core::ProfileCategory::Render);

			surfaceSignature = scene::SignatureOf(instances);

			// **And how deep the mirrors are being drawn, which is an input to
			// every one of them.** A surface pass draws the *other* panes, so a
			// level added or taken away changes what is inside each picture as
			// surely as moving something does - and without this the automatic
			// depth could not climb at all in a still scene. It measures one
			// deeper, nothing else in the frame moves, no surface refreshes, the
			// deeper level is never drawn, and the next frame measures the same
			// shallow answer again. Found exactly that way, on
			// `MirrorCorridor.luau`, which is static on purpose.
			surfaceSignature = scene::MixSignature(surfaceSignature, surfaceBounces);

			// **And the other worlds, whose whole purpose is to be moving.** A
			// destination that changed while this world sat still is the case a
			// live portal exists for, and it is invisible to the line above.
			surfaceSignature = scene::MixSignature(surfaceSignature, scene::SignatureOf(foreign));

			for (size_t index = 0; index < acceptedCount; index++) {
				surfaceSignature = scene::MixSignature(surfaceSignature, accepted[index].Index);
				surfaceSignature = MixMatrix(surfaceSignature, accepted[index].ViewProjection);
				surfaceSignature = MixFloat(surfaceSignature, accepted[index].ImageOpacity);

				// **The foreign range, because moving it is a change nothing
				// else here would notice.** `SignatureOf(instances)` already
				// covers the *contents* of the appended tail - the far world
				// moving redraws the pane, which is the whole point of a live
				// destination - but a host that reordered two foreign ranges
				// without changing either world would leave the signature
				// identical while each surface now names the other's instances.
				surfaceSignature = scene::MixSignature(surfaceSignature, accepted[index].View->InstanceFirst);
				surfaceSignature = scene::MixSignature(surfaceSignature, accepted[index].View->InstanceCount);
			}

			for (size_t index = 0; index < acceptedCount; index++) {
				Impl::SurfaceSlotState &state = bank.Surfaces[accepted[index].Index];

				// **Written whether or not the surface renders.** The opacity is
				// what the *screen* pass composites the pane with and it changes
				// no texel of the texture, so a mirror faded by a script fades
				// this frame rather than on whichever later frame something else
				// happens to move.
				state.ImageOpacity = accepted[index].ImageOpacity;
				state.Effect = accepted[index].Effect;

				// **Three independent reasons to skip, and they are not
				// interchangeable.** The signature says the image has not
				// changed; visibility says nothing is looking at it; the rate
				// says it drew recently enough. A surface has to clear all
				// three, and each of them alone leaves the slot holding the
				// frame it has along with the matrices that drew it.
				//
				// **Visibility overrides the never-rendered case.** A slot that
				// has never drawn has nothing to compare a signature against,
				// but it also has nothing looking at it - and `Ready` staying
				// false is exactly right for that: the pane draws as its own
				// tint until the frame something can see it.
				//
				// **The rate does not**, and that asymmetry is deliberate: a
				// surface must draw *once* as soon as something can see it, or
				// a pane walked up to shows its own tint for up to an interval
				// before the picture appears.
				const bool changed = !state.Ready || state.Signature != surfaceSignature;
				const bool due = DueToDraw(state.Drawn, accepted[index].View->FPS, frameSeconds);

				accepted[index].Refresh = surfaceVisible[accepted[index].Index] && changed && due;

				refreshCount += accepted[index].Refresh ? 1u : 0u;
			}
		}

		// **This world's rows, then the other worlds' - one buffer, two halves
		// that never mix.** The head is what every pass in this frame partitions
		// and submits; the tail exists only so a surface can name a range of it.
		// Joining them before the plan is what drew two rooms on top of each
		// other until v0.14, and is why `foreign` is its own argument.
		State->SceneInstances.assign(instances.begin(), instances.end());
		const auto ownCount = static_cast<uint32_t>(State->SceneInstances.size());
		State->SceneInstances.insert(State->SceneInstances.end(), foreign.begin(), foreign.end());

		// **Every range the three scene passes submit, from one call.** The
		// ordering, the mirror partition and the caster partition are arithmetic
		// over a `shared` type and they live in `scene::OrderScene` - where a
		// headless suite can get at them. A renderer is the one module a test
		// cannot exercise, so the counts it hands to a draw call are the last
		// place they should be computed. See `scene::ScenePlan` for the runs.
		//
		// **Given the head alone.** A plan over the tail as well would sort
		// another world's parts into this one's opaque run, its mirror partition
		// and its shadow casters - and the ordering it returns is a permutation,
		// so it would also move the very rows a surface has already named.
		scene::ScenePlan plan;
		{
			ENGINE_PROFILE_CAT("order scene", core::ProfileCategory::Render);
			const auto started = std::chrono::steady_clock::now();
			plan = scene::OrderScene(
				std::span<const scene::DrawInstance>(State->SceneInstances).first(ownCount),
				sceneEye,
				State->SceneOrder
			);
			if (const graph::Node *worldNode = graphNode(core::Name("world")); worldNode != nullptr) {
				cpuNodeWall[worldNode->Name.Id()] +=
					std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - started)
						.count();
			}
		}

		const auto sceneCount = static_cast<uint32_t>(State->SceneInstances.size());
		const auto sceneOpaque = static_cast<size_t>(plan.Opaque);
		const uint32_t sceneTransparent = plan.Transparent;
		const uint32_t sceneReflected = plan.Reflected;
		const uint32_t reflectedCasters = plan.ReflectedCasters;
		const uint32_t surfaceCasters = plan.SurfaceCasters;

		{
			// Allocation, on the frame an overlay first appears or changes size.
			// Zero on every other frame, which is what makes a reading here
			// worth looking at rather than background noise.
			// HasContent, not IsDirty. The texture keeps the last thing uploaded
			// to it, so a frame that redraws nothing still has a panel to show -
			// which is the whole point of the image living on the GPU rather
			// than being pushed there again every frame.
			// **Headless first, because nothing headless can show it.** The
			// overlay pass is the window's, so a headless frame allocated a
			// texture, copied the panels into it and drew none of them - and
			// `MarkUploaded` then told the image the GPU matched it, which was
			// a claim about a texture nothing would ever sample. Now the whole
			// overlay is one question answered once.
			//
			// Safe to skip only because the screen pass no longer borrows this
			// texture when the shadow map is missing; see `FallbackTexture`.
			ENGINE_PROFILE_CAT("ensure overlay", core::ProfileCategory::Render);
			haveOverlay = graphEnabled(core::Name("overlay")) && !State->Headless() && overlay.HasContent() &&
						  !overlay.IsEmpty() && State->EnsureOverlay(overlay.GetWidth(), overlay.GetHeight());
		}

		const auto instanceCount = static_cast<uint32_t>(visibleCount);
		result.Culled = totalCount - instanceCount;

		// **One buffer, two ranges.** The scene range first so its offset is
		// zero and the camera range starts where it ends - which is what makes
		// each pass one `first_instance` rather than a second buffer and a
		// second bind.
		const uint32_t uploadCount = sceneCount + instanceCount;

		// Whether this view's pipeline authored `culling = "occlusion"` on its
		// entity filter, and the backend can serve it this frame. The CPU
		// frustum cull already ran - occlusion composes behind it rather than
		// replacing it.
		bool occlusionCulling = false;
		if (const Impl::NamedPipeline *active = State->PipelineFor(State->ActiveGraph);
			active != nullptr && State->Occlusion.Seed != nullptr && State->Occlusion.Reduce != nullptr &&
			State->Occlusion.Cull != nullptr && State->Occlusion.Args != nullptr) {
			for (uint32_t value = 1; value <= active->Graph.Count() && !occlusionCulling; value++) {
				const graph::Node *node = active->Graph.Find(graph::NodeId{value});
				if (node == nullptr || !node->Enabled || node->Kind != core::Name("cull-frustum")) {
					continue;
				}
				const std::string *culling = node->Parameter(core::Name("culling"));
				occlusionCulling = culling != nullptr && *culling == "occlusion";
			}
		}

		if (uploadCount > 0) {
			bool capacity = false;
			{
				// Grows the device buffer when the scene does. Separate from the
				// copy below because one is a GPU allocation and the other is a
				// memcpy, and a spike in either means something different.
				ENGINE_PROFILE_CAT("ensure instance capacity", core::ProfileCategory::Render);
				capacity = State->EnsureInstanceCapacity(uploadCount);
			}

			if (capacity) {
				ENGINE_PROFILE_CAT("upload instances", core::ProfileCategory::Render);

				void *mapped = nullptr;
				{
					// Mapping can stall: the driver hands back memory the GPU may
					// still be reading unless it cycles, and this asks it to.
					ENGINE_PROFILE_CAT("map instances", core::ProfileCategory::Render);
					mapped = SDL_MapGPUTransferBuffer(State->Device, State->InstanceTransfer, true);
				}
				{
					// Converted straight into the mapped buffer rather than
					// into a vector that is then memcpy'd. Eighty bytes an
					// entity go into write-combined memory either way, and the
					// staging copy would be that traffic paid a third time -
					// once by the world filling its draw list, once here, and
					// once again on the way out.
					//
					// This is where a `CFrame` and a `Color3` become a `mat4`
					// and an RGBA, and it is the only place in the engine that
					// happens. A world produces scene data; a device layout is
					// this module's business.
					ENGINE_PROFILE_CAT("convert instances", core::ProfileCategory::Render);

					auto *out = static_cast<GpuInstance *>(mapped);

					// **What is in each slot, recorded in the same pass that
					// fills it.** The draw loop needs the mesh and the texture
					// of every slot to find its runs, and the only place that
					// mapping exists is here - after this loop the order is a
					// buffer offset and the instance it came from is gone.
					State->SlotMesh.resize(uploadCount);
					State->SlotTexture.resize(uploadCount);
					State->SlotNormalMap.resize(uploadCount);
					State->SlotRoughnessMap.resize(uploadCount);
					State->SlotOcclusionMap.resize(uploadCount);
					State->SlotHeightMap.resize(uploadCount);
					State->SlotEmissiveMap.resize(uploadCount);
					State->SlotShader.resize(uploadCount);
					State->SlotTags.resize(uploadCount);
					State->SlotSeam.resize(uploadCount);
					State->SlotSeamLight.resize(uploadCount);

					// **The mesh is resolved once and used twice.** `ToGpu` needs
					// it to stretch the instance into its `Size` box and the draw
					// loop needs it to find its runs, and resolving twice would
					// be a second hash of the same name per instance per frame.
					const auto record = [&](size_t slot, const scene::DrawInstance &instance) {
						const MeshEntry &mesh = State->Meshes.Resolve(instance.Mesh);
						State->SlotMesh[slot] = &mesh;
						State->SlotTexture[slot] = instance.Texture;
						State->SlotNormalMap[slot] = instance.NormalMap;
						State->SlotRoughnessMap[slot] = instance.RoughnessMap;
						State->SlotOcclusionMap[slot] = instance.OcclusionMap;
						State->SlotHeightMap[slot] = instance.HeightMap;
						State->SlotEmissiveMap[slot] = instance.EmissiveMap;
						State->SlotShader[slot] = instance.Shader;
						State->SlotTags[slot] = instance.TagMask;
						State->SlotSeam[slot] = glm::vec4{
							instance.SeamNormal.X,
							instance.SeamNormal.Y,
							instance.SeamNormal.Z,
							instance.SeamOffset
						};
						State->SlotSeamLight[slot] =
							glm::vec4{instance.SeamLight.X, instance.SeamLight.Y, instance.SeamLight.Z, 0.0f};
						return &mesh;
					};

					for (size_t index = 0; index < State->SceneOrder.size(); index++) {
						const scene::DrawInstance &instance = State->SceneInstances[State->SceneOrder[index]];
						out[index] = ToGpu(instance, *record(index, instance));
					}

					// **The other worlds, in the order they were handed over.**
					// Nothing sorts them: they are drawn as one plain run by one
					// surface, so there is no partition to build and no eye to
					// sort towards - the pane's camera is not this frame's.
					//
					// Slot `ownCount + k` therefore holds `foreign[k]`, which is
					// what lets `SurfaceView::InstanceFirst` survive a plan that
					// permuted everything before it.
					for (size_t index = ownCount; index < sceneCount; index++) {
						const scene::DrawInstance &instance = State->SceneInstances[index];
						out[index] = ToGpu(instance, *record(index, instance));
					}

					const size_t cameraBase = sceneCount;
					auto *camera = out + cameraBase;
					for (size_t index = 0; index < State->DrawOrder.size(); index++) {
						const scene::DrawInstance &instance =
							State->VisibleInstances[State->DrawOrder[index]];
						camera[index] = ToGpu(instance, *record(cameraBase + index, instance));
					}

					// --- the occlusion plan --------------------------------
					//
					// Each opaque slot-run is partitioned in place: the
					// instances big and near enough to occlude move to its
					// head and draw in the early phase; the tail waits on the
					// GPU test against the pyramid the early phase's depth
					// seeds. **Only the instance rows move.** Every per-slot
					// array is constant across a run by `SlotsShareRun`'s
					// definition, so swapping rows inside one changes no run
					// boundary and no other pass's picture - an opaque draw
					// is order-independent under the depth test.
					State->OcclusionFrame = Impl::OcclusionPlan{};
					if (occlusionCulling && plainOpaque > 0) {
						ENGINE_PROFILE_CAT("occlusion plan", core::ProfileCategory::Render);
						Impl::OcclusionPlan &occlusionPlan = State->OcclusionFrame;

						// Drawn as an occluder when its widest world extent
						// subtends at least this fraction of its distance -
						// roughly six degrees. Lower drafts more occluders and
						// costs early-phase overdraw; higher seeds the pyramid
						// with too little to cull against. Not measured
						// finely; revisit with a real scene if the cull rate
						// disappoints.
						constexpr float OCCLUDER_SCORE = 0.1f;

						const glm::vec3 eye{
							cameraFrame.Position.X, cameraFrame.Position.Y, cameraFrame.Position.Z
						};
						const auto base = static_cast<uint32_t>(cameraBase);
						const uint32_t opaqueEnd = base + plainOpaque;

						std::vector<GpuInstance> earlyRows;
						std::vector<GpuInstance> lateRows;
						uint32_t slot = base;
						while (slot < opaqueEnd) {
							uint32_t run = 1;
							while (slot + run < opaqueEnd && State->SlotsShareRun(slot, slot + run)) {
								run++;
							}

							const MeshEntry &runMesh = *State->SlotMesh[slot];
							const glm::vec3 meshCentre{runMesh.Centre.X, runMesh.Centre.Y, runMesh.Centre.Z};
							const glm::vec3 meshExtent{runMesh.Extent.X, runMesh.Extent.Y, runMesh.Extent.Z};

							const auto runIndex = static_cast<uint32_t>(occlusionPlan.RunFirstSlot.size());
							const size_t pairFirst = occlusionPlan.CandidatePairs.size();
							earlyRows.clear();
							lateRows.clear();
							for (uint32_t at = slot; at < slot + run; at++) {
								const GpuInstance &row = out[at];

								// The world box, from the same matrix the
								// vertex shader draws with: the mesh's own box
								// mapped through the model. `ToGpu` built the
								// model to fill the part's box exactly, so
								// this bound is tight rather than approximate.
								const glm::mat3 basis{row.Model};
								const glm::vec3 centre = glm::vec3(row.Model * glm::vec4(meshCentre, 1.0f));
								const glm::vec3 extent = glm::abs(basis[0]) * meshExtent.x +
														 glm::abs(basis[1]) * meshExtent.y +
														 glm::abs(basis[2]) * meshExtent.z;

								const float widest = std::max(extent.x, std::max(extent.y, extent.z));
								const float away = std::max(glm::distance(centre, eye), 0.01f);
								if (widest >= away * OCCLUDER_SCORE) {
									earlyRows.push_back(row);
								} else {
									lateRows.push_back(row);
									occlusionPlan.CandidatePairs.emplace_back(
										centre, std::bit_cast<float>(runIndex)
									);
									occlusionPlan.CandidatePairs.emplace_back(extent, 0.0f);
								}
							}

							const auto earlyCount = static_cast<uint32_t>(earlyRows.size());
							std::copy(earlyRows.begin(), earlyRows.end(), out + slot);
							std::copy(lateRows.begin(), lateRows.end(), out + slot + earlyCount);

							// A candidate names the instance row it became
							// after the partition, which is only known now.
							for (size_t pair = pairFirst; pair < occlusionPlan.CandidatePairs.size();
								 pair += 2) {
								const auto order = static_cast<uint32_t>((pair - pairFirst) / 2);
								occlusionPlan.CandidatePairs[pair + 1].w =
									std::bit_cast<float>(slot + earlyCount + order);
							}

							occlusionPlan.RunFirstSlot.push_back(slot);
							occlusionPlan.RunEarly.push_back(earlyCount);
							occlusionPlan.RunCandidates.push_back(static_cast<uint32_t>(lateRows.size()));
							occlusionPlan.EarlyTotal += earlyCount;
							occlusionPlan.ArgCount += DrawArgumentCount(runMesh);
							slot += run;
						}

						occlusionPlan.RunCount = static_cast<uint32_t>(occlusionPlan.RunFirstSlot.size());
						occlusionPlan.CandidateCount =
							static_cast<uint32_t>(occlusionPlan.CandidatePairs.size() / 2);

						// Nothing big enough to seed the pyramid means nothing
						// can be culled, and nothing to test means nothing to
						// cull either way - both fall back to the plain draw,
						// which is what "conservative" costs in the worst case.
						occlusionPlan.Active = occlusionPlan.EarlyTotal > 0 &&
											   occlusionPlan.CandidateCount > 0 && occlusionPlan.ArgCount > 0;
					}
				}
				SDL_UnmapGPUTransferBuffer(State->Device, State->InstanceTransfer);
				haveInstances = true;

				// Stage what the cull reads and the indirect draws consume. Its
				// own transfer rather than a tail on the instance one, so the
				// plain path pays nothing for a feature it never authored.
				if (State->OcclusionFrame.Active) {
					Impl::OcclusionPlan &occlusionPlan = State->OcclusionFrame;
					if (!State->EnsureOcclusionResources(
							occlusionPlan.ArgCount,
							occlusionPlan.CandidateCount,
							occlusionPlan.RunCount,
							plainOpaque
						)) {
						occlusionPlan.Active = false;
					} else {
						void *staged =
							SDL_MapGPUTransferBuffer(State->Device, State->Occlusion.Transfer, true);
						if (staged == nullptr) {
							ENGINE_ERROR("occlusion staging: SDL_MapGPUTransferBuffer: {}", SDL_GetError());
							occlusionPlan.Active = false;
						} else {
							const auto cameraBase = static_cast<uint32_t>(sceneCount);
							auto *commands = static_cast<SDL_GPUIndexedIndirectDrawCommand *>(staged);
							auto *lateArgRuns = reinterpret_cast<uint32_t *>(
								reinterpret_cast<uint8_t *>(staged) +
								static_cast<size_t>(occlusionPlan.ArgCount) * 2 *
									sizeof(SDL_GPUIndexedIndirectDrawCommand) +
								static_cast<size_t>(occlusionPlan.CandidateCount) * 2 * sizeof(glm::vec4) +
								static_cast<size_t>(occlusionPlan.RunCount) * sizeof(uint32_t)
							);

							// The early and late commands for one draw differ
							// only in what fills their instance count and where
							// their instances start: the early phase draws the
							// run's occluder head out of the instance buffer,
							// the late phase draws the compacted survivors out
							// of the late buffer, whose slots drop `cameraBase`
							// so a view's opaque head starts it at zero.
							uint32_t argument = 0;
							for (uint32_t runIndex = 0; runIndex < occlusionPlan.RunCount; runIndex++) {
								const MeshEntry &runMesh =
									*State->SlotMesh[occlusionPlan.RunFirstSlot[runIndex]];
								const uint32_t firstSlot = occlusionPlan.RunFirstSlot[runIndex];
								const uint32_t lateFirst =
									firstSlot - cameraBase + occlusionPlan.RunEarly[runIndex];
								const auto emitArgs = [&](const MeshRange &range) {
									if (range.IndexCount == 0) {
										return;
									}
									commands[argument] = SDL_GPUIndexedIndirectDrawCommand{
										range.IndexCount,
										occlusionPlan.RunEarly[runIndex],
										range.FirstIndex,
										range.VertexOffset,
										firstSlot,
									};
									commands[occlusionPlan.ArgCount + argument] =
										SDL_GPUIndexedIndirectDrawCommand{
											range.IndexCount,
											0,
											range.FirstIndex,
											range.VertexOffset,
											lateFirst,
										};
									lateArgRuns[argument] = runIndex;
									argument++;
								};
								if (runMesh.Runs.empty()) {
									emitArgs(runMesh.Whole);
								} else {
									for (const MeshRange &range : runMesh.Runs) {
										emitArgs(range);
									}
								}
							}

							auto *cursor = reinterpret_cast<uint8_t *>(staged) +
										   static_cast<size_t>(occlusionPlan.ArgCount) * 2 *
											   sizeof(SDL_GPUIndexedIndirectDrawCommand);
							std::memcpy(
								cursor,
								occlusionPlan.CandidatePairs.data(),
								occlusionPlan.CandidatePairs.size() * sizeof(glm::vec4)
							);
							cursor += occlusionPlan.CandidatePairs.size() * sizeof(glm::vec4);

							auto *runTable = reinterpret_cast<uint32_t *>(cursor);
							for (uint32_t runIndex = 0; runIndex < occlusionPlan.RunCount; runIndex++) {
								runTable[runIndex] = occlusionPlan.RunFirstSlot[runIndex] - cameraBase +
													 occlusionPlan.RunEarly[runIndex];
							}
							cursor += static_cast<size_t>(occlusionPlan.RunCount) * sizeof(uint32_t) +
									  static_cast<size_t>(occlusionPlan.ArgCount) * sizeof(uint32_t);

							// The zeros the cull's atomics count up from.
							std::memset(
								cursor, 0, static_cast<size_t>(occlusionPlan.RunCount) * sizeof(uint32_t)
							);

							SDL_UnmapGPUTransferBuffer(State->Device, State->Occlusion.Transfer);
						}
					}
				}
			}
		}

		// The particles, packed and grouped before any render pass opens.
		//
		// **Here rather than inside the draw**, because what follows is a copy
		// pass and a copy pass cannot be started while a render pass is open -
		// the same constraint `FrameOverlayHook`'s `Prepare`/`Record` split
		// exists for, stated in that header in the same words.
		// The local lights, packed once for the frame.
		//
		// **Every pass gets the same set**, including a mirror's: a lamp lights
		// what a reflection shows exactly as it lights the world, and giving the
		// surface pass a different set would make a mirror disagree with the room
		// it is in.
		const LightUniforms lightUniforms = ToGpu(lights);

		uint32_t particleCount = 0;
		uint32_t ribbonCount = 0;
		{
			ENGINE_PROFILE_CAT("prepare particles", core::ProfileCategory::Render);
			particleCount = graphEnabled(core::Name("transparent")) ? State->PrepareParticles(source) : 0;
			result.Particles = particleCount;

			ribbonCount = graphEnabled(core::Name("transparent")) ? State->PrepareRibbons(ribbonVertices) : 0;
			result.RibbonVertices = ribbonCount;
		}

		// Only when something is actually waiting to go across. A panel redrawn
		// ten times a second and presented a thousand times has nothing to
		// upload on nine hundred and ninety of those frames.
		const bool uploadOverlay =
			haveOverlay && (State->OverlayUninitialised || overlay.UploadRegion().Width > 0);

		// Timings and the frame result use authored node names. This is the
		// execution record of the selected graph, not a second fixed pass list.
		if (State->BatchFirst || !State->BatchActive) {
			State->CollectTimings();
			State->WallTimings.clear();
		}
		uint32_t timingSlot = State->BatchActive ? (State->BatchFirst ? State->Timestamps.Begin(command)
																	  : State->BatchTimingSlot)
												 : State->Timestamps.Begin(command);
		if (State->BatchActive && State->BatchFirst) {
			State->BatchTimingSlot = timingSlot;
		}
		if ((State->BatchFirst || !State->BatchActive) && timingSlot < VulkanTimestamps::SLOTS) {
			State->PendingMarks[timingSlot].clear();
		}
		core::Name timedName;
		uint32_t openedMark = VulkanTimestamps::MARKS;
		SDL_GPUCommandBuffer *timedCommand = command;
		auto openedWall = std::chrono::steady_clock::now();
		bool mainGpuWorkRecorded = State->BatchActive && !State->BatchFirst;
		bool dedicatedComputeSubmitted = false;
		const auto closePass = [&] {
			if (!timedName.IsValid()) {
				return;
			}
			const auto now = std::chrono::steady_clock::now();
			State->WallTimings[timedName.Id()] +=
				std::chrono::duration<double, std::micro>(now - openedWall).count();
			if (timingSlot < VulkanTimestamps::SLOTS) {
				const uint32_t closedMark = State->Timestamps.Mark(timedCommand);
				if (openedMark < VulkanTimestamps::MARKS && closedMark < VulkanTimestamps::MARKS) {
					State->PendingMarks[timingSlot].push_back({timedName, openedMark, closedMark});
				}
			}
			timedName = {};
		};
		const auto enterNamedPass = [&](core::Name name, SDL_GPUCommandBuffer *recordedCommand = nullptr) {
			closePass();
			timedName = name;
			timedCommand = recordedCommand != nullptr ? recordedCommand : command;
			const graph::Node *node = nullptr;
			const graph::ScheduledNode *scheduled = nullptr;
			for (const graph::ExecutionWave &wave : selectedPipeline->Schedule.Waves) {
				for (const graph::ScheduledNode &candidate : wave.Nodes) {
					const graph::Node *candidateNode = selectedPipeline->Graph.Find(candidate.Node);
					if (candidateNode != nullptr && candidateNode->Name == name) {
						node = candidateNode;
						scheduled = &candidate;
						break;
					}
				}
				if (scheduled != nullptr) {
					break;
				}
			}
			if (scheduled != nullptr &&
				(scheduled->Queue == graph::ExecutionQueue::Graphics ||
				 scheduled->Queue == graph::ExecutionQueue::Transfer) &&
				node->Kind != core::Name("upload-instances")) {
				mainGpuWorkRecorded = true;
			}
			if (std::find(result.Nodes.begin(), result.Nodes.end(), name) == result.Nodes.end()) {
				result.Nodes.push_back(name);
			}
			openedWall = std::chrono::steady_clock::now();
			openedMark = timingSlot < VulkanTimestamps::SLOTS ? State->Timestamps.Mark(timedCommand)
															  : VulkanTimestamps::MARKS;
		};
		const auto finishCpuNode = [&](const graph::RunContext &context) {
			closePass();
			if (std::find(result.Nodes.begin(), result.Nodes.end(), context.Name) == result.Nodes.end()) {
				result.Nodes.push_back(context.Name);
			}
			if (const auto found = cpuNodeWall.find(context.Name.Id()); found != cpuNodeWall.end()) {
				State->WallTimings[context.Name.Id()] += found->second;
			}
			return true;
		};
		for (const char *kind :
			 {"world", "camera", "entities", "cull-frustum", "cull-distance", "filter-tag", "order-draw"}) {
			frameNodes.Set(core::Name(kind), finishCpuNode);
		}
		bool uploadsSubmitted = false;
		const auto submitUploads = [&] {
			if (uploadsSubmitted) {
				return true;
			}

			const bool uploadInstances = haveInstances;
			if (!uploadInstances && !uploadOverlay && particleCount == 0 && ribbonCount == 0) {
				uploadsSubmitted = true;
				return true;
			}

			OverlayImage::Region overlayRegion;
			if (uploadOverlay) {
				ENGINE_PROFILE_CAT("stage overlay", core::ProfileCategory::Render);
				overlayRegion = State->OverlayUninitialised
									? OverlayImage::Region{0, 0, overlay.GetWidth(), overlay.GetHeight()}
									: overlay.UploadRegion();
				const auto rowBytes =
					static_cast<size_t>(overlayRegion.Width) * OverlayImage::BYTES_PER_PIXEL;
				void *mapped = SDL_MapGPUTransferBuffer(State->Device, State->OverlayTransfer, true);
				if (mapped == nullptr) {
					ENGINE_ERROR("upload overlay: SDL_MapGPUTransferBuffer: {}", SDL_GetError());
					return false;
				}

				auto *destination = static_cast<uint8_t *>(mapped);
				const uint8_t *pixels = overlay.GetPixels();
				const auto stride = static_cast<size_t>(overlay.GetWidth()) * OverlayImage::BYTES_PER_PIXEL;
				for (int row = 0; row < overlayRegion.Height; row++) {
					const size_t offset =
						static_cast<size_t>(overlayRegion.Y + row) * stride +
						static_cast<size_t>(overlayRegion.X) * OverlayImage::BYTES_PER_PIXEL;
					std::memcpy(destination + static_cast<size_t>(row) * rowBytes, pixels + offset, rowBytes);
				}
				SDL_UnmapGPUTransferBuffer(State->Device, State->OverlayTransfer);
			}

			SDL_GPUCommandBuffer *uploadCommand = SDL_AcquireGPUCommandBuffer(State->Device);
			if (uploadCommand == nullptr) {
				ENGINE_ERROR("upload: SDL_AcquireGPUCommandBuffer: {}", SDL_GetError());
				return false;
			}
			SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(uploadCommand);
			if (copy == nullptr) {
				ENGINE_ERROR("upload instances: SDL_BeginGPUCopyPass: {}", SDL_GetError());
				SDL_CancelGPUCommandBuffer(uploadCommand);
				return false;
			}

			uint64_t uploadedBytes = 0;
			if (uploadInstances) {
				const SDL_GPUTransferBufferLocation source{State->InstanceTransfer, 0};
				const SDL_GPUBufferRegion destination{
					State->InstanceBuffer,
					0,
					uploadCount * static_cast<uint32_t>(sizeof(GpuInstance)),
				};
				SDL_UploadToGPUBuffer(copy, &source, &destination, true);
				uploadedBytes += destination.size;
			}

			// The occlusion plan's five buffers, in the order its staging wrote
			// them. Cycled like the instances: this copy is each buffer's first
			// touch of the frame, so a later view gets a fresh version while
			// the previous view's dispatches keep the one they bound. The cull
			// pass itself must *not* cycle `Counts` again - its atomics count
			// up from the zeros this copy delivers.
			if (uploadInstances && State->OcclusionFrame.Active) {
				const Impl::OcclusionPlan &occlusionPlan = State->OcclusionFrame;
				uint32_t offset = 0;
				const auto stage = [&](SDL_GPUBuffer *buffer, uint32_t bytes) {
					const SDL_GPUTransferBufferLocation source{State->Occlusion.Transfer, offset};
					const SDL_GPUBufferRegion destination{buffer, 0, bytes};
					SDL_UploadToGPUBuffer(copy, &source, &destination, true);
					offset += bytes;
					uploadedBytes += bytes;
				};
				stage(
					State->Occlusion.Arguments,
					occlusionPlan.ArgCount * 2 *
						static_cast<uint32_t>(sizeof(SDL_GPUIndexedIndirectDrawCommand))
				);
				stage(
					State->Occlusion.Candidates,
					occlusionPlan.CandidateCount * 2 * static_cast<uint32_t>(sizeof(glm::vec4))
				);
				stage(State->Occlusion.RunTable, occlusionPlan.RunCount * sizeof(uint32_t));
				stage(State->Occlusion.ArgRuns, occlusionPlan.ArgCount * sizeof(uint32_t));
				stage(State->Occlusion.Counts, occlusionPlan.RunCount * sizeof(uint32_t));
			}

			if (ribbonCount > 0) {
				const SDL_GPUTransferBufferLocation source{State->RibbonTransfer, 0};
				const SDL_GPUBufferRegion destination{
					State->RibbonBuffer,
					0,
					ribbonCount * static_cast<uint32_t>(sizeof(effects::RibbonVertex)),
				};
				SDL_UploadToGPUBuffer(copy, &source, &destination, true);
				uploadedBytes += destination.size;
			}

			// **No particle upload here any more.** The instance stream is not
			// host data that has to cross - it is what `particle-step.comp`
			// wrote, on its own submission, before this command buffer was
			// recorded. See `Impl::ParticlePool`.

			if (uploadOverlay) {
				SDL_GPUTextureTransferInfo source{};
				source.transfer_buffer = State->OverlayTransfer;
				source.pixels_per_row = static_cast<uint32_t>(overlayRegion.Width);
				source.rows_per_layer = static_cast<uint32_t>(overlayRegion.Height);

				SDL_GPUTextureRegion destination{};
				destination.texture = State->OverlayTexture;
				destination.x = static_cast<uint32_t>(overlayRegion.X);
				destination.y = static_cast<uint32_t>(overlayRegion.Y);
				destination.w = static_cast<uint32_t>(overlayRegion.Width);
				destination.h = static_cast<uint32_t>(overlayRegion.Height);
				destination.d = 1;

				// Keep the texture allocation because a partial upload must preserve
				// every pixel outside the dirty rectangle.
				SDL_UploadToGPUTexture(copy, &source, &destination, false);
				uploadedBytes += static_cast<uint64_t>(overlayRegion.Width) * overlayRegion.Height *
								 OverlayImage::BYTES_PER_PIXEL;
			}

			SDL_EndGPUCopyPass(copy);
			if (!SDL_SubmitGPUCommandBuffer(uploadCommand)) {
				ENGINE_ERROR("upload: SDL_SubmitGPUCommandBuffer: {}", SDL_GetError());
				return false;
			}

			// No fence here. Resource cycling gives each later view a fresh
			// destination while its draw commands retain the version they bound.
			// The GPU can consume this copy while the CPU records the main command
			// buffer, and queue submission order makes every draw see its upload.
			if (uploadOverlay) {
				State->OverlayUninitialised = false;
				overlay.MarkUploaded();
			}
			result.UploadedBytes += uploadedBytes;
			result.UploadCommandBuffers++;
			uploadsSubmitted = true;
			return true;
		};
		frameNodes.Set(core::Name("upload-instances"), [&](const graph::RunContext &context) {
			enterNamedPass(context.Name);
			return submitUploads();
		});
		// --- shadow pass ----------------------------------------------------
		//
		// **The scene range, not the camera's**, and no colour target at all.
		// Every caster casts, whether or not the eye can see it.
		//
		// A scene whose opaque geometry all opted out of casting skips the pass
		// rather than clearing a depth target nothing writes to - and the
		// colour pass then samples a shadow map that was never rendered, which
		// is what `FrameResult::Ran` exists to make visible.
		const bool haveShadow = graphEnabled(core::Name("shadow")) && haveInstances && sceneCount > 0 &&
								(reflectedCasters > 0 || surfaceCasters > 0) && State->EnsureShadow();

		// Builds the per-draw block from world lighting and the camera used by
		// this pass. Fog is eye-relative, so a reflected or portal sub-view must
		// not reuse the screen eye even though every other authored term is shared.
		const scene::WorldLighting currentLighting = CurrentLighting();
		const auto lightingFrom = [&](const scene::WorldLighting &worldLighting,
									  const core::Vector3 &eye,
									  float surfaceMode,
									  float imageOpacity) {
			LightingUniforms lighting;
			lighting.Direction = glm::vec4{
				worldLighting.Direction.X,
				worldLighting.Direction.Y,
				worldLighting.Direction.Z,
				0.0f,
			};
			lighting.Ambient = glm::vec4{
				worldLighting.Ambient.R,
				worldLighting.Ambient.G,
				worldLighting.Ambient.B,
				1.0f,
			};
			lighting.Direct = glm::vec4{
				worldLighting.Direct.R,
				worldLighting.Direct.G,
				worldLighting.Direct.B,
				1.0f,
			};
			lighting.Flags = glm::vec4{
				haveShadow ? 1.0f : 0.0f,
				1.0f / static_cast<float>(SHADOW_RESOLUTION),
				surfaceMode,
				imageOpacity,
			};
			lighting.OutdoorAmbient = glm::vec4{
				worldLighting.OutdoorAmbient.R,
				worldLighting.OutdoorAmbient.G,
				worldLighting.OutdoorAmbient.B,
				1.0f,
			};
			lighting.FogColour = glm::vec4{
				worldLighting.FogColor.R,
				worldLighting.FogColor.G,
				worldLighting.FogColor.B,
				1.0f,
			};
			lighting.Fog = glm::vec4{worldLighting.FogStart, worldLighting.FogEnd, 0.0f, 0.0f};
			lighting.Eye = glm::vec4{eye.X, eye.Y, eye.Z, 0.0f};
			return lighting;
		};
		const auto lightingAt = [&](const core::Vector3 &eye, float surfaceMode, float imageOpacity) {
			return lightingFrom(currentLighting, eye, surfaceMode, imageOpacity);
		};

		State->Beams = BeamUniforms{};
		frameNodes.Set(core::Name("shadow"), [&](const graph::RunContext &context) {
			enterNamedPass(context.Name);
			if (!haveShadow) {
				return true;
			}
			if (!submitUploads()) {
				return false;
			}

			{
				ENGINE_PROFILE_CAT("shadow pass", core::ProfileCategory::Render);

				SDL_GPUDepthStencilTargetInfo shadowTarget{};
				shadowTarget.texture = State->ShadowTexture;
				shadowTarget.clear_depth = 1.0f;
				shadowTarget.load_op = SDL_GPU_LOADOP_CLEAR;

				// **Stored, unlike the colour pass's depth.** This one is read by
				// the next pass, which is the entire point of rendering it.
				shadowTarget.store_op = SDL_GPU_STOREOP_STORE;
				shadowTarget.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
				shadowTarget.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
				shadowTarget.cycle = true;

				SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(command, nullptr, 0, &shadowTarget);
				State->BindPipeline(pass, State->ShadowPipeline, Impl::PipelineFamily::Other);

				const SDL_GPUBufferBinding vertexBindings[] = {
					{State->Meshes.Vertices(), 0},
					{State->InstanceBuffer, 0},
				};
				SDL_BindGPUVertexBuffers(pass, 0, vertexBindings, 2);

				const SDL_GPUBufferBinding indexBinding{State->Meshes.Indices(), 0};
				SDL_BindGPUIndexBuffer(pass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);

				SDL_PushGPUVertexUniformData(command, 0, &lightViewProjection, sizeof(lightViewProjection));

				// **Only the opaque part of the scene casts**, and of that only what
				// `Visual::CastShadow` left switched on. A transparent pane writing
				// full depth into the shadow map would cast a solid shadow, which is
				// the most obviously wrong thing glass can do; an opaque thing that
				// should not occlude is the case the author decides, and it arrives
				// here as the caster runs `partition casters` produced.
				//
				// Two draws because the two runs are not adjacent - the surface
				// partition sits between them. The second is empty in every scene
				// with no mirror in it, which is almost all of them.
				// Depth only, so no samplers and no fragment uniforms - the null
				// lighting pointer is what says so.
				uint64_t shadowTriangles = 0;
				if (reflectedCasters > 0) {
					result.DrawCalls += State->DrawSlots(
						command,
						pass,
						0,
						reflectedCasters,
						nullptr,
						nullptr,
						nullptr,
						nullptr,
						nullptr,
						0,
						shadowTriangles
					);
				}
				if (surfaceCasters > 0) {
					result.DrawCalls += State->DrawSlots(
						command,
						pass,
						sceneReflected,
						surfaceCasters,
						nullptr,
						nullptr,
						nullptr,
						nullptr,
						nullptr,
						0,
						shadowTriangles
					);
				}

				SDL_EndGPURenderPass(pass);
			}

			// --- portal beams ----------------------------------------------------
			//
			// **A hole carries occlusion as well as a picture.** Both rooms already
			// have the world's sun, so what a portal transports is not light but the
			// *absence* of it: a caster standing in front of a hole darkens the floor
			// beyond it, and a body cut at the seam is shadowed by whatever shadows
			// its other half. Adding a second contribution instead would double-light
			// every floor near a doorway.
			//
			// **The casters are left where they are and the receiver is mapped
			// back**, which is the whole reason this is affordable. The obvious
			// arrangement renders the near room's casters *transformed* into the far
			// room, and that needs a second instance buffer holding a mapped copy of
			// the world. Mapping the other way needs none: a far-side fragment goes
			// through `Back` into the near room and is looked up there, where the
			// casters already are. `NON-EUCLIDEAN.md` Part V.3 is the derivation.
			//
			// **The frustum is the aperture.** `graph::FitPortalLight` fits the sides
			// of the box to the pane's own rectangle, so a fragment the beam does not
			// reach projects outside `0..1` and the lookup already reads that as lit.
			// There is no rectangle test in the shader because the matrix is one.
			if (havePortals && haveShadow && State->EnsureBeams()) {
				ENGINE_PROFILE_CAT("portal beams", core::ProfileCategory::Render);

				// The receiver holes nearest the eye, because every fragment tests
				// every live beam and four is what a corridor needs. A directional
				// beam starts at `Pane` and arrives at `Partner`; ranking the source
				// drops the incoming beam for the room the eye is actually in when
				// several pairs compete for the budget.
				struct Beam {
					const PortalView *Pane = nullptr;
					const PortalView *Partner = nullptr;
					float Distance = 0.0f;
				};

				Beam ordered[scene::MAX_SURFACES];
				size_t candidates = 0;

				for (uint8_t slot = 0; slot < scene::MAX_SURFACES; slot++) {
					const PortalView *const portal = portalOf[slot];
					if (portal == nullptr || portal->Partner < 0) {
						continue;
					}

					// **A pane with no partner in this frame's set carries nothing**,
					// because the map back is the partner's own warp - one map per
					// pane, and a pair's two are each other's inverse. Deriving an
					// inverse here would be a second arithmetic to get wrong.
					const PortalView *const partner = portalOf[static_cast<uint8_t>(portal->Partner)];
					if (partner == nullptr) {
						continue;
					}

					ordered[candidates++] = Beam{
						portal,
						partner,
						scene::RectangleDistance(
							partner->Centre, partner->First, partner->Second, cameraFrame.Position
						)
					};
				}

				std::sort(ordered, ordered + candidates, [](const Beam &left, const Beam &right) {
					return left.Distance < right.Distance;
				});

				if (candidates > State->MaximumBeamCandidatesWarned) {
					// **Logged rather than dropped quietly.** A shadow that stops
					// crossing when a fifth pane comes on screen reads as the feature
					// not working at all, which is a much harder thing to look for
					// than a line saying which holes were left out.
					ENGINE_WARN(
						"{} holes could carry a shadow and only {} may; the farther ones do not",
						candidates,
						MAX_PORTAL_BEAMS
					);
					State->MaximumBeamCandidatesWarned = candidates;
				}

				const auto live = static_cast<uint32_t>(std::min<size_t>(candidates, MAX_PORTAL_BEAMS));

				const auto half = static_cast<float>(SHADOW_RESOLUTION / 2);

				for (uint32_t index = 0; index < live; index++) {
					const Beam &beam = ordered[index];
					const core::Vector3 sun{State->Sun.x, State->Sun.y, State->Sun.z};

					// The receiver is carried from the far room back into this
					// pane's chart by the partner's warp. Its light ray has to take
					// that same rotation. Mapping only the position makes a turned
					// portal cast the right silhouette in the wrong direction.
					const core::Vector3 beamDirection = beam.Partner->Warp.Rotate(sun);

					State->Beams.Light[index] = graph::FitPortalLight(
						sceneBounds, beam.Pane->Centre, beam.Pane->First, beam.Pane->Second, beamDirection
					);

					State->Beams.Back[index] = scene::SeamMatrix(beam.Partner->Warp);

					State->Beams.Plane[index] = glm::vec4{
						beam.Pane->Normal.X,
						beam.Pane->Normal.Y,
						beam.Pane->Normal.Z,
						beam.Pane->Normal.Dot(beam.Pane->Centre)
					};

					// Half the atlas on each axis, in the reading order the viewport
					// below uses.
					State->Beams.Region[index] = glm::vec4{
						0.5f, 0.5f, static_cast<float>(index % 2) * 0.5f, static_cast<float>(index / 2) * 0.5f
					};

					SDL_GPUDepthStencilTargetInfo beamTarget{};
					beamTarget.texture = State->BeamTexture;
					beamTarget.clear_depth = 1.0f;

					// **The first beam clears the whole atlas and the rest load it.**
					// A clear is not confined by the viewport, so clearing per beam
					// would wipe the ones already drawn - and a quadrant nobody wrote
					// stays at the far plane, which the lookup reads as lit.
					beamTarget.load_op = index == 0 ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
					beamTarget.store_op = SDL_GPU_STOREOP_STORE;
					beamTarget.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
					beamTarget.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
					beamTarget.cycle = false;

					SDL_GPURenderPass *beamPass = SDL_BeginGPURenderPass(command, nullptr, 0, &beamTarget);
					State->BindPipeline(beamPass, State->ShadowPipeline, Impl::PipelineFamily::Other);

					const SDL_GPUViewport beamViewport{
						static_cast<float>(index % 2) * half,
						static_cast<float>(index / 2) * half,
						half,
						half,
						0.0f,
						1.0f
					};
					SDL_SetGPUViewport(beamPass, &beamViewport);

					const SDL_Rect beamScissor{
						static_cast<int>(index % 2) * static_cast<int>(half),
						static_cast<int>(index / 2) * static_cast<int>(half),
						static_cast<int>(half),
						static_cast<int>(half)
					};
					SDL_SetGPUScissor(beamPass, &beamScissor);

					const SDL_GPUBufferBinding beamBindings[] = {
						{State->Meshes.Vertices(), 0},
						{State->InstanceBuffer, 0},
					};
					SDL_BindGPUVertexBuffers(beamPass, 0, beamBindings, 2);

					const SDL_GPUBufferBinding beamIndices{State->Meshes.Indices(), 0};
					SDL_BindGPUIndexBuffer(beamPass, &beamIndices, SDL_GPU_INDEXELEMENTSIZE_32BIT);

					SDL_PushGPUVertexUniformData(command, 0, &State->Beams.Light[index], sizeof(glm::mat4));

					// The same caster runs the world's own shadow map draws, for the
					// same reason: a caster outside the beam is culled by the matrix
					// rather than by a list.
					uint64_t beamTriangles = 0;
					if (reflectedCasters > 0) {
						result.DrawCalls += State->DrawSlots(
							command,
							beamPass,
							0,
							reflectedCasters,
							nullptr,
							nullptr,
							nullptr,
							nullptr,
							nullptr,
							0,
							beamTriangles
						);
					}
					if (surfaceCasters > 0) {
						result.DrawCalls += State->DrawSlots(
							command,
							beamPass,
							sceneReflected,
							surfaceCasters,
							nullptr,
							nullptr,
							nullptr,
							nullptr,
							nullptr,
							0,
							beamTriangles
						);
					}

					SDL_EndGPURenderPass(beamPass);
				}

				State->Beams.Count.x = static_cast<float>(live);
			}
			return true;
		});

		// --- what a scene pass is, wherever it is opened ----------------------
		//
		// The three passes below stay separate on purpose - a mirror reflects its
		// viewer through one plane and a hole carries it onto a second pane, and
		// the two panes read their pictures back by different lookups, which is
		// the whole of `PortalView`'s header comment. What was never a difference
		// between them is how a scene pass is opened and what the world's own
		// draws look like once it is, and those were written out twice side by
		// side, so every bug in one was available to the other.

		// The shadow map and the sampler that reads it, or the stand-ins bound
		// where the real ones do not exist. The pipelines declare both slots
		// and a draw must bind both - an unbound sampler is undefined behaviour
		// on several backends where a wrongly bound one is merely ignored.
		struct ShadowBinding {
			SDL_GPUTexture *Texture;
			SDL_GPUSampler *Sampler;
		};

		// **Asked at each pass rather than resolved once for the frame**, and
		// that is not a style choice: `State->SurfaceSampler` is made by
		// whichever pass first needs it, and a world of nothing but holes never
		// reaches `EnsureSurface`. The portal pass makes it itself for exactly
		// that case, so a pair resolved before the surface pass would hand the
		// portal pass the null it goes out of its way to avoid.
		const auto shadowBinding = [&] {
			return ShadowBinding{
				State->ShadowTexture != nullptr ? State->ShadowTexture : State->FallbackTexture,
				State->ShadowSampler != nullptr ? State->ShadowSampler : State->SurfaceSampler,
			};
		};

		// Opens a scene pass onto a colour/depth pair and primes it with the
		// state every scene pass shares: the clear, the light set, the beams,
		// the opaque pipeline and the one buffer pair every mesh lives in.
		//
		// **`cycle` is the one thing the callers disagree about.** A surface
		// slot is written and sampled inside the same frame, so it cycles; a
		// portal target is written once and sampled once, by the pass above it,
		// in that order - and asking for a fresh allocation there made the
		// device hang more often rather than less.
		//
		// `viewport` is the rectangle of the target the world fills, or null for
		// the whole of it: a surface texture is made the size of the pass that
		// writes it and a portal target is not. The scissor follows the
		// viewport, because nothing here draws outside the rectangle it set.
		// `clearColour` overrides the fog clear when given: the seam light
		// captures clear to the world's ambient - the lit void - so their
		// background carries the room's light level rather than a sky wash.
		const auto openScenePass = [&](SDL_GPUTexture *colour,
									   SDL_GPUTexture *depth,
									   bool cycle,
									   const SDL_GPUViewport *viewport,
									   const LightUniforms &passLights,
									   const SDL_FColor *clearColour = nullptr) -> SDL_GPURenderPass * {
			SDL_GPUColorTargetInfo colourInfo{};
			colourInfo.texture = colour;
			colourInfo.clear_color = clearColour != nullptr ? *clearColour
															: SDL_FColor{
																  State->FogColour.r,
																  State->FogColour.g,
																  State->FogColour.b,
																  1.0f,
															  };
			colourInfo.load_op = SDL_GPU_LOADOP_CLEAR;
			colourInfo.store_op = SDL_GPU_STOREOP_STORE;
			colourInfo.cycle = cycle;

			SDL_GPUDepthStencilTargetInfo depthInfo{};
			depthInfo.texture = depth;
			depthInfo.clear_depth = 1.0f;
			depthInfo.load_op = SDL_GPU_LOADOP_CLEAR;
			depthInfo.store_op = SDL_GPU_STOREOP_DONT_CARE;
			depthInfo.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
			depthInfo.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
			depthInfo.cycle = cycle;

			SDL_GPURenderPass *const pass = SDL_BeginGPURenderPass(command, &colourInfo, 1, &depthInfo);

			if (viewport != nullptr) {
				SDL_SetGPUViewport(pass, viewport);

				const SDL_Rect scissor{
					static_cast<int>(viewport->x),
					static_cast<int>(viewport->y),
					static_cast<int>(viewport->w),
					static_cast<int>(viewport->h)
				};
				SDL_SetGPUScissor(pass, &scissor);
			}

			// **The light set, pushed once for the whole pass.** Uniform state
			// on a command buffer persists until it is replaced, so one push
			// before the draws serves every one of them - which is the whole
			// reason this is a second buffer rather than fields on the per-draw
			// `LightingUniforms`.
			SDL_PushGPUFragmentUniformData(command, 1, &passLights, sizeof(passLights));

			// **The beams, beside the lights and for the same reason.** Which
			// holes carry a shadow is a fact about the frame, so it is pushed
			// once per pass rather than per draw - and it is pushed even when
			// there are none, because a stale block from a previous frame would
			// shadow through a hole that is no longer there.
			SDL_PushGPUFragmentUniformData(command, 2, &State->Beams, sizeof(State->Beams));
			State->BindPipeline(pass, State->OpaquePipeline, Impl::PipelineFamily::Opaque);

			const SDL_GPUBufferBinding vertexBindings[] = {
				{State->Meshes.Vertices(), 0},
				{State->InstanceBuffer, 0},
			};
			SDL_BindGPUVertexBuffers(pass, 0, vertexBindings, 2);

			const SDL_GPUBufferBinding indexBinding{State->Meshes.Indices(), 0};
			SDL_BindGPUIndexBuffer(pass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);

			return pass;
		};

		// The world minus every pane, drawn into whatever pass is open.
		//
		// **`plan.Reflected` is exactly that run** - the opaque head with every
		// instance that samples a slot taken out of it, and a mirror and a hole
		// are the same exclusion. The panes are put back by the caller, each
		// with its own texture: a mirror may not appear in its own reflection
		// and a portal's pane shows the level below it.
		const auto drawWorldInto =
			[&](SDL_GPURenderPass *pass, const LightingUniforms &plainLighting, uint32_t filter) {
				if (sceneReflected > 0) {
					const ShadowBinding shadow = shadowBinding();
					result.DrawCalls += State->DrawSlots(
						command,
						pass,
						0,
						sceneReflected,
						&plainLighting,
						shadow.Texture,
						shadow.Sampler,
						nullptr,
						State->SurfaceSampler,
						filter,
						result.Triangles
					);
				}
			};

		// **The blended tail, minus the panes in it.** The opaque head already
		// excludes them; a pane that went transparent moved from the head to the
		// tail and stopped being excluded, so a faded mirror reflected itself
		// and a faded hole was drawn as ordinary glass on top of the picture it
		// was already showing.
		//
		// **`panesFollow` binds the transparent pipeline for draws this does not
		// make.** The surface pass submits its blended mirrors itself, straight
		// after this and onto the same pipeline, so a tail of nothing but
		// mirrors still has to have it bound. The portal pass drew its panes
		// with the opaque head and has nothing left to bind for.
		const auto drawBlendedInto = [&](SDL_GPURenderPass *pass,
										 const FrameUniforms &frame,
										 const LightingUniforms &plainLighting,
										 uint32_t filter,
										 bool panesFollow) {
			const uint32_t blendedPlain = sceneTransparent - plan.TransparentSurfaces;
			if (blendedPlain > 0 || (panesFollow && plan.TransparentSurfaces > 0)) {
				State->BindPipeline(pass, State->TransparentPipeline, Impl::PipelineFamily::Transparent);
			}

			if (blendedPlain == 0) {
				return;
			}

			SDL_PushGPUVertexUniformData(command, 0, &frame, sizeof(FrameUniforms));

			const ShadowBinding shadow = shadowBinding();
			result.DrawCalls += State->DrawSlots(
				command,
				pass,
				static_cast<uint32_t>(sceneOpaque),
				blendedPlain,
				&plainLighting,
				shadow.Texture,
				shadow.Sampler,
				nullptr,
				State->SurfaceSampler,
				filter,
				result.Triangles
			);
		};

		// The interface upload must precede every scene pass that can draw a
		// spatial collector. `Prepare` opens a copy pass and therefore cannot run
		// once a mirror, surface or portal render pass is open. Headless frames
		// still draw into capture targets; a hook that has no backend declines in
		// `Prepare` itself.
		const bool drawInterface = graphEnabled(core::Name("interface")) && gameInterfaceHook != nullptr &&
								   gameInterfaceHook->Prepare(command);
		const bool drawHostOverlay =
			swapchain != nullptr && hostOverlayHook != nullptr && hostOverlayHook->Prepare(command);

		// --- the mirror recursion --------------------------------------------
		//
		// **What a pane shows a viewer that is not the eye.**
		//
		// `scene::AimSurfaceCameras` places every surface camera by reflecting the
		// world's *active* camera through its pane. That answer is right for the
		// screen and wrong everywhere else: a pane appearing inside another pane's
		// picture is being looked at from that pane's camera, several studs and a
		// reflection away from the eye. Projecting it with the eye's matrix put
		// the coordinate outside the texture's 0..1 rectangle, and `opaque.frag`
		// falls back to the plain lit pane there - which is the flat slab a mirror
		// seen in a mirror used to be, and which reads as culling rather than as a
		// projection fault.
		//
		// **Running the pass again cannot fix it, and that is worth being precise
		// about.** Iterating refreshes textures; it never moves a camera. Each
		// bounce redrew the same eye-derived viewpoints with fresher contents, so
		// the chain got newer and stayed wrong.
		//
		// So the levels below the first are a recursion, exactly as a hole's are:
		// each level's camera is `scene::ReflectCamera` applied to the camera of
		// the level above, and reflections therefore compose by construction. The
		// two passes now differ in what a level's camera *is* and in nothing else,
		// which is why they share `openScenePass`, `drawWorldInto` and
		// `drawBlendedInto` and keep their own entry points.
		//
		// **Depth first, and every level's targets survive until the level above
		// has drawn all of its panes** - `Impl::MirrorLevel` is indexed by level
		// and slot for that reason, which is `Impl::PortalLevel`'s reason.
		//
		// **One fewer than the bounce count, because the surface pass is the top
		// level.** `SetSurfaceBounces(2)` has always meant "two levels of
		// mirror-in-mirror", and it still does: the pane's own texture is one and
		// the recursion supplies the rest. Zero here is one bounce - no
		// recursion, every inner pane drawn flat - which is exactly what one
		// bounce drew before and is the honest floor rather than a special case.
		//
		// **The subtraction cannot underflow because every path to
		// `surfaceBounces` floors at one**, which is why that is stated at each
		// of them rather than defended again here.
		const uint32_t mirrorLevels = surfaceBounces - 1u;

		// **Cleared for the whole bank before anything descends.** A target holds
		// last frame's picture from a camera that no longer exists, and a level
		// that is not reached this frame must read as absent rather than as stale
		// - see `Impl::MirrorTarget::Ready`.
		for (Impl::MirrorLevel &level : bank.Mirrors) {
			for (Impl::MirrorTarget &target : level.Targets) {
				target.Ready = false;
			}
		}

		// Whether one more level of the recursion would have drawn anything.
		//
		// **The whole of the automatic depth's measurement, and it asks the
		// descent's own two questions rather than an approximation of them.** A
		// pane in the draw list is not enough - most of them are behind this
		// level's camera - and a pane in the frustum is not enough either, since
		// one seen edge-on has no continuous orientation to reflect through and
		// renders nothing. Answering either of those looser questions would ask
		// for a level that comes back empty, measure one shallower for it, and
		// ask again: the depth would sit oscillating between two values for as
		// long as the viewer stood still.
		//
		// Up to sixteen frustum tests and a reflection, once per pane at the
		// bottom level only, beside a scene render each.
		const auto wouldDescend = [&](const glm::mat4 &from, const core::CFrame &frame, int8_t skip) {
			for (uint8_t slot = 0; slot < scene::MAX_SURFACES; slot++) {
				if (!havePanes[slot] || static_cast<int8_t>(slot) == skip) {
					continue;
				}

				const scene::SurfacePane &pane = panes[slot];
				if (!graph::VisiblePane(from, pane.Centre, pane.First, pane.Second)) {
					continue;
				}

				if (scene::ReflectCamera(pane, frame, {}).Renders) {
					return true;
				}
			}

			return false;
		};

		// One level: fill `bank.Mirrors[level][i]` for every mirror `i` this
		// camera can see, then leave them for the caller to sample.
		//
		// A `std::function` because it calls itself and captures the frame, which
		// is `fillLevel`'s arrangement below and is paid once per pane per level
		// beside a whole scene render.
		//
		// @param from    The viewer's matrices, for the per-level pane cull.
		// @param frame   Where the viewer stands, which is what gets reflected.
		// @param level   Which level of the pool this call fills.
		// @param skip    The pane this camera is *on*. Nothing sees itself in its
		//                own reflection, and descending into it would put the pane
		//                in front of its own camera.
		std::function<void(const glm::mat4 &, const core::CFrame &, uint32_t, int8_t)> fillMirror;

		fillMirror = [&](const glm::mat4 &from, const core::CFrame &frame, uint32_t level, int8_t skip) {
			for (uint8_t slot = 0; slot < scene::MAX_SURFACES; slot++) {
				if (!havePanes[slot] || static_cast<int8_t>(slot) == skip) {
					continue;
				}

				const scene::SurfacePane &pane = panes[slot];

				// **Per pane per level, which is what stops the cost being
				// `panes ^ depth`.** A mirror behind this level's camera costs
				// nothing, and in a room most of them are. `graph::VisiblePane` is
				// the portal pass's test unchanged - the question is the same one.
				if (!graph::VisiblePane(from, pane.Centre, pane.First, pane.Second)) {
					continue;
				}

				// **The one statement of what a mirror does to a camera**, applied
				// to this level's viewer rather than to the eye. `scene::
				// AimSurfaceCameras` calls the same function for the top level, so
				// a chain cannot drift from the screen by a sign.
				//
				// **No frustum corners handed down.** The clamp they buy is a
				// sharpness optimisation for a pane the viewer is close to, and
				// the viewer at these levels is a fitted off-axis camera whose
				// lens this pass does not carry. Unclamped is the correct image at
				// a coarser resolution, which is the right trade for a reflection
				// that is already a reflection of a reflection.
				const scene::MirrorEye eye = scene::ReflectCamera(pane, frame, {});
				if (!eye.Renders) {
					continue;
				}

				const scene::CameraMatrices matrices =
					scene::ResolveSurfaceCamera(eye.Frame, scene::SurfaceProjection(eye.Lens, eye.Frame));

				// **Deeper first**, so this level's own draws can sample what the
				// level below just wrote. The pool is per level, so the targets
				// filled here survive exactly until this loop has finished with
				// them.
				//
				// **At the bottom the same question is asked and not acted on**,
				// which is what tells the next frame whether the budget was the
				// thing that stopped it. This pane is about to be drawn flat
				// inside its own picture; whether that is the end of the chain or
				// the end of the allowance is exactly `wouldDescend`.
				if (level > 0) {
					fillMirror(matrices.ViewProjection, eye.Frame, level - 1, static_cast<int8_t>(slot));
				} else {
					surfaceDepth.Deeper =
						surfaceDepth.Deeper ||
						wouldDescend(matrices.ViewProjection, eye.Frame, static_cast<int8_t>(slot));
				}

				// **The authored size, with no screen-coverage scaling.** A pane's
				// footprint on screen decides how sharp the *top* level has to be
				// - `SurfaceScale` - and these are levels inside that one, where a
				// pane covers a fraction of a fraction. Scaling them by the top
				// pane's coverage would allocate the deepest, smallest images at
				// the highest resolution in the frame.
				Impl::MirrorTarget *target =
					State->EnsureMirror(targetSlot, level, slot, paneWidth[slot], paneHeight[slot]);
				if (target == nullptr) {
					continue;
				}

				// **Not cycled, unlike a surface slot.** A level is written once
				// and sampled once, by the pass above it, in that order - see
				// `Impl::PortalTarget`, where cycling anyway made the device hang
				// more often rather than less.
				SDL_GPURenderPass *const pass =
					openScenePass(target->Colour, target->Depth, false, nullptr, lightUniforms);

				const LightingUniforms levelLighting = lightingAt(eye.Frame.Position, 0.0f, 1.0f);

				const FrameUniforms levelFrame{
					matrices.ViewProjection,
					lightViewProjection,
					glm::mat4{1.0f},
				};
				SDL_PushGPUVertexUniformData(command, 0, &levelFrame, sizeof(levelFrame));

				drawWorldInto(pass, levelLighting, pane.TagFilter);

				// The other panes, put back one at a time, each sampling the level
				// below. At the recursion bound it samples that pane's completed
				// image from the prior frame. This preserves a continuing corridor
				// without adding another same-frame scene draw. A pane with no
				// history yet draws as its own lit material for the first frame.
				//
				// **The opaque runs only, exactly as the portal pass does it.** A
				// pane that went transparent left the opaque head, and
				// `drawBlendedInto` excludes the panes in the tail - so a *faded*
				// mirror inside a deep reflection shows as glass rather than as a
				// reflection. The top level still composites it, which is where
				// anybody would notice; paying a level of recursion for the second
				// bounce of a pane somebody has faded is not a trade this makes.
				const ShadowBinding shadow = shadowBinding();

				for (uint8_t seen = 0; seen < scene::MAX_SURFACES; seen++) {
					if (seen == slot) {
						continue;
					}

					const scene::SurfaceRun &run = plan.Runs[seen];
					if (run.OpaqueCount == 0) {
						continue;
					}

					const Impl::MirrorTarget *below = level > 0 && bank.Mirrors.size() >= level
														  ? &bank.Mirrors[level - 1].Targets[seen]
														  : nullptr;

					LightingUniforms paneLighting = levelLighting;
					SDL_GPUTexture *paneTexture = nullptr;

					if (below != nullptr && below->Ready) {
						// 1 is the projected-image branch - see `opaque.frag`.
						paneLighting.Flags.z = 1.0f;
						paneLighting.Flags.w = 1.0f;

						const FrameUniforms seenFrame{
							matrices.ViewProjection,
							lightViewProjection,
							below->Sampling,
						};
						SDL_PushGPUVertexUniformData(command, 0, &seenFrame, sizeof(seenFrame));
						paneTexture = below->Colour;
					} else if (mirrorHistory && bank.Surfaces[seen].Ready) {
						const Impl::SurfaceSlotState &history = bank.Surfaces[seen];
						paneLighting = lightingAt(eye.Frame.Position, 1.0f, history.ImageOpacity);
						paneLighting.Mirror.x = static_cast<float>(history.Effect);
						const FrameUniforms historyFrame{
							matrices.ViewProjection,
							lightViewProjection,
							history.Sampling,
						};
						SDL_PushGPUVertexUniformData(command, 0, &historyFrame, sizeof(historyFrame));
						paneTexture = history.Texture[history.Slot];
					} else {
						SDL_PushGPUVertexUniformData(command, 0, &levelFrame, sizeof(levelFrame));
					}

					result.DrawCalls += State->DrawSlots(
						command,
						pass,
						run.OpaqueFirst,
						run.OpaqueCount,
						&paneLighting,
						shadow.Texture,
						shadow.Sampler,
						paneTexture,
						State->SurfaceSampler,
						pane.TagFilter,
						result.Triangles
					);
				}

				if (drawInterface) {
					result.DrawCalls += gameInterfaceHook->RecordWorld(
						command,
						pass,
						matrices.ViewProjection,
						eye.Frame,
						core::Color3{State->Ambient.x, State->Ambient.y, State->Ambient.z},
						core::Vector3{State->Sun.x, State->Sun.y, State->Sun.z},
						paneWidth[slot],
						paneHeight[slot],
						false
					);
				}

				drawBlendedInto(pass, levelFrame, levelLighting, pane.TagFilter, false);

				if (drawInterface) {
					result.DrawCalls += gameInterfaceHook->RecordWorld(
						command,
						pass,
						matrices.ViewProjection,
						eye.Frame,
						core::Color3{State->Ambient.x, State->Ambient.y, State->Ambient.z},
						core::Vector3{State->Sun.x, State->Sun.y, State->Sun.z},
						paneWidth[slot],
						paneHeight[slot],
						true
					);
				}

				SDL_EndGPURenderPass(pass);

				// **Written after the pass has ended**, which is the invariant the
				// whole ordering rests on: a level is readable only once the draw
				// that filled it has been submitted.
				target->Sampling = matrices.ViewProjection;
				target->Ready = true;
				result.SurfacePasses++;

				// **Counted from the eye rather than from the pool.** The pool
				// index runs the other way - `mirrorLevels - 1` is the level
				// nearest the screen and zero is the deepest - and what the next
				// frame's arithmetic needs is a depth, with the surface pass
				// itself as one.
				surfaceDepth.Resolved = std::max(surfaceDepth.Resolved, mirrorLevels + 1u - level);
			}
		};

		// --- surface pass ----------------------------------------------------
		//
		// The same scene range, from each surface camera, into that surface's
		// own texture. What a mirror shows next frame - the one-frame staleness
		// `ViewChannel` already assumed, and what breaks the dependency cycle
		// between a mirror and what it reflects.
		//
		// **One pass per surface, and each one draws the other surfaces.** A
		// mirror still may not appear in its own reflection: it sits between its
		// camera and the world and would fill the texture with itself. Every
		// *other* mirror is drawn, from the half of its pair this frame is not
		// writing - so what you see in a mirror of a mirror is one frame old per
		// bounce. There is no order that would avoid that, because each surface
		// is being rendered for the others.
		//
		// **Only the surfaces whose signature moved.** A pass that would redraw
		// the texture its slot already holds is not run: its pair keeps the
		// frame it has, its matrices keep describing the camera that drew that
		// frame, and the screen pass samples it exactly as if it had just been
		// rendered. See `SignatureOf` for what counts as a change and, more to
		// the point, what deliberately does not.
		frameNodes.Set(core::Name("mirror-capture"), [&](const graph::RunContext &context) {
			enterNamedPass(context.Name);
			if (wantSurface && haveInstances && sceneCount > 0 && refreshCount > 0) {
				ENGINE_PROFILE_CAT("surface pass", core::ProfileCategory::Render);

				// **The whole pass, run once per bounce, which is how depth used to be
				// had - and is not how it is had any more where a pane carries its
				// rectangle.** `D00112`'s remaining half was that a surface samples the
				// *other* surfaces from the textures they had last frame, so a chain
				// resolved over frames rather than within one. Iterating fixed that:
				// bounce zero draws every surface sampling last frame's neighbours, the
				// flip makes bounce zero's output the read side, and bounce one
				// therefore samples *this* frame's neighbours.
				//
				// **What it never fixed is the viewpoint, and no number of runs
				// could.** Every bounce redrew the same eye-derived cameras. The
				// pictures got fresher and stayed taken from where nobody was standing,
				// which is the flat slab `fillMirror` above exists to remove.
				//
				// So: **one run when the panes carry rectangles**, because the depth is
				// the recursion's now and running the whole pass again would only
				// redraw the same answer at the same viewpoints. A view with no
				// rectangle - a camera parented to the world, or a cross-world pane -
				// still resolves its chain by iterating, which is what it always did
				// and is still the best available for it: nothing here can reflect a
				// camera through a pane it was never told about.
				//
				// **The ping-pong stays either way.** With the recursion nothing
				// samples a slot being written, so the pair is not load-bearing for
				// mirrors - but the screen pass and the iterating path both still read
				// `Slot ^ 1`, and a surface skipped this frame must keep the matrices
				// that drew what it holds.
				//
				// **`graph::VisibleSurfaces` is given `surfaceBounces` regardless**,
				// above, where `cullRounds` is computed - and it must be. That decides
				// how many levels of surface-seen-in-surface are *marked visible*, and
				// a level the recursion draws without being marked is a level culled,
				// which is what made a mirror's deeper reflections vanish as the viewer
				// turned. The two numbers describe the same depth by two routes.
				const uint32_t bounces =
					anyPane ? 1u : (acceptedCount > 1 ? std::max(surfaceBounces, 1u) : 1u);

				for (uint32_t bounce = 0; bounce < bounces; bounce++) {

					// **Flipped for every refreshing surface before the first pass runs,
					// not inside the loop.** A surface pass samples the other surfaces'
					// read slots, so every slot has to have finished flipping before any
					// of them is read - flipping inside the loop would have the second
					// pass sample the first surface's *new* texture, which is this
					// frame's half-drawn image and the exact self-reference the pair
					// exists to make impossible.
					//
					// **A skipped surface does not flip, and its matrices do not move.**
					// Both halves of that are one fact: the slot still holds the frame it
					// held, so `ViewProjection` must still be the camera that drew it and
					// `PreviousViewProjection` the one before. Advancing either for a
					// surface that did not render would project a texture with a camera
					// that never took it - a reflection sliding across a pane that
					// nothing in the scene is moving, which is the hardest possible
					// version of this bug to attribute.
					for (size_t index = 0; index < acceptedCount; index++) {
						if (!accepted[index].Refresh) {
							continue;
						}

						Impl::SurfaceSlotState &state = bank.Surfaces[accepted[index].Index];
						state.PreviousViewProjection = state.ViewProjection;
						state.PreviousSampling = state.Sampling;
						state.ViewProjection = accepted[index].ViewProjection;
						state.Sampling = accepted[index].Sampling;
						state.Slot ^= 1u;
					}

					for (size_t index = 0; index < acceptedCount; index++) {
						if (!accepted[index].Refresh) {
							continue;
						}

						const uint8_t self = accepted[index].Index;

						// **Taken here rather than at each draw**, because `drawMirrors`
						// below shadows `index` with its own loop over surfaces - so
						// `accepted[index]` inside it would name a different view
						// entirely, and the filter would silently be another surface's.
						const uint32_t surfaceFilter = accepted[index].View->TagFilter;
						Impl::SurfaceSlotState &state = bank.Surfaces[self];

						// **The levels below this one, filled from *this* camera and
						// not from the eye.** That is the whole of the fix: every pane
						// this pass is about to draw needs a picture taken from where
						// this camera stands, and `fillMirror` is what takes it.
						//
						// **Here rather than once for the frame**, because the pool is
						// per level and not per level per parent: the targets filled
						// now are consumed by the draws a few lines down and then
						// overwritten by the next pane's descent. Hoisting this out of
						// the loop would give every pane the last one's reflections.
						//
						// **And with no levels below it, the same question is asked
						// here instead**, because one bounce is a depth the automatic
						// rule has to be able to climb out of: a corridor at one level
						// draws every inner pane flat, and nothing else in the frame
						// would say that a second level had anything to show.
						if (mirrorLevels > 0 && havePanes[self]) {
							fillMirror(
								accepted[index].ViewProjection,
								accepted[index].View->Frame,
								mirrorLevels - 1,
								static_cast<int8_t>(self)
							);
						} else if (havePanes[self]) {
							surfaceDepth.Deeper = surfaceDepth.Deeper || wouldDescend(
																			 accepted[index].ViewProjection,
																			 accepted[index].View->Frame,
																			 static_cast<int8_t>(self)
																		 );
						}

						// **Cycled**, because this half of the pair is written here and
						// read by the screen pass in the same frame - see
						// `openScenePass` for what the other caller does instead. The
						// whole target is the pass, so there is no viewport to set.
						const SurfaceView &capturedView = *accepted[index].View;
						const scene::WorldLighting &surfaceWorldLighting =
							capturedView.OverrideLighting ? capturedView.Lighting : currentLighting;
						const LightUniforms surfaceLights =
							capturedView.OverrideLighting
								? ToGpu(std::span<const SceneLight>(capturedView.Lights))
								: lightUniforms;
						SDL_GPURenderPass *const pass = openScenePass(
							state.Texture[state.Slot], state.Depth, true, nullptr, surfaceLights
						);

						// **Shadowed, and pointedly not surfaced.** The mirror's own view
						// gets the shadow map, so what it reflects is lit the way the
						// screen lights it.
						//
						// **`Flags.z` is zero for the world, and it has to be.** It means
						// "this draw samples a surface texture instead of its own tint",
						// and it is set below for exactly the mirror runs. Setting it for
						// the whole pass is what made the floor sample the previous
						// frame's reflection and come out as the clear colour wherever
						// that projection landed on untouched texels - a black wedge in
						// the mirror that survived deleting every caster, the frame and
						// the near-plane hack, and moved when the camera was re-aimed but
						// not when the floor was.
						const core::Vector3 surfaceEye = capturedView.Frame.Position;
						const LightingUniforms surfaceLighting =
							lightingFrom(surfaceWorldLighting, surfaceEye, 0.0f, 1.0f);

						const ShadowBinding shadow = shadowBinding();

						// **The samplers are bound per draw now rather than per run**,
						// because the third one - the colour map - changes with the
						// mesh being drawn and the other two do not. `DrawSlots` binds
						// all three together; what is left here is remembering which
						// surface texture the next draws should sample.
						SDL_GPUTexture *surfaceTexture = nullptr;
						const auto bindSurface = [&](SDL_GPUTexture *texture) { surfaceTexture = texture; };

						const FrameUniforms worldFrame{
							state.ViewProjection,
							lightViewProjection,
							glm::mat4{1.0f},
						};

						const auto plainly = [&]() {
							SDL_PushGPUVertexUniformData(command, 0, &worldFrame, sizeof(worldFrame));
							bindSurface(nullptr);
						};

						plainly();

						// **Another world's instances, when the host handed some over.**
						// `SurfaceView::InstanceCount` says why this bypasses the plan:
						// the plan partitions *this* world's draw list and knows nothing
						// about the tail behind it, so a foreign surface is one plain run
						// and no mirror runs at all. That is what lets a portal in one
						// world show a live second world.
						//
						// **`ownCount` is what turns the host's index into a slot.** The
						// host counts from zero within its own `foreign` list, because
						// nothing outside this call knows where this world's rows end or
						// that the plan reordered them.
						//
						// **`continue`, so the plan-driven draws below are skipped
						// entirely.** Drawing both would put this world's floor into the
						// far world's picture, which reads as the two rooms bleeding
						// into each other.
						if (accepted[index].View->InstanceCount > 0) {
							result.DrawCalls += State->DrawSlots(
								command,
								pass,
								ownCount + accepted[index].View->InstanceFirst,
								accepted[index].View->InstanceCount,
								&surfaceLighting,
								shadow.Texture,
								shadow.Sampler,
								surfaceTexture,
								State->SurfaceSampler,
								surfaceFilter,
								result.Triangles
							);

							// **Ended and left for the sweep below to mark ready**, which
							// is the invariant this must not shortcut: a surface written
							// this frame may not be sampled as another surface's
							// "previous" within the same frame.
							SDL_EndGPURenderPass(pass);
							continue;
						}

						drawWorldInto(pass, surfaceLighting, surfaceFilter);

						// **Every mirror except this one, one draw each.** `self` is
						// skipped because nothing sees itself in its own reflection -
						// drawing it would fill this texture with the pane it belongs
						// to, and the mirror would show itself rather than the room.
						//
						// A surface that has no frame yet, or that no camera is
						// rendering this frame, is drawn **plainly** rather than skipped.
						// A pane that vanishes until its mirror warms up is worse than
						// one that is briefly its own colour, and a pane naming an index
						// nothing renders is a scene mistake that should be visible as a
						// flat pane rather than as a hole in the geometry.
						//
						// Sampled draws project with the matrix that *rendered* the
						// texture being read - `PreviousViewProjection`, not the one
						// just resolved - because the image is a frame old and
						// projecting it with a fresh camera slides it across the pane.
						//
						// **And the recursion's level is preferred to the slot's own
						// texture whenever there is one.** The slot holds the pane as
						// the *eye* sees it; the level holds it as this camera sees it,
						// which is the only one of the two that belongs in this
						// picture. The slot is still the fallback for a pane the
						// recursion could not reach - off screen from here, edge-on, or
						// a camera with no rectangle at all - because a stale
						// reflection of a reflection is a better answer than a blank
						// one.
						const auto drawMirrors = [&](bool blended) {
							for (uint8_t index = 0; index < scene::MAX_SURFACES; index++) {
								if (index == self) {
									continue;
								}

								const scene::SurfaceRun &run = plan.Runs[index];
								const uint32_t count = blended ? run.BlendedCount : run.OpaqueCount;
								const uint32_t first = blended ? run.BlendedFirst : run.OpaqueFirst;
								if (count == 0) {
									continue;
								}

								const Impl::MirrorTarget *const level =
									mirrorLevels > 0 && bank.Mirrors.size() >= mirrorLevels
										? &bank.Mirrors[mirrorLevels - 1].Targets[index]
										: nullptr;

								if (level != nullptr && level->Ready) {
									const Impl::SurfaceSlotState &shown = bank.Surfaces[index];

									const FrameUniforms levelFrame{
										state.ViewProjection,
										lightViewProjection,
										level->Sampling,
									};
									LightingUniforms levelLighting =
										lightingAt(surfaceEye, 1.0f, shown.ImageOpacity);
									levelLighting.Mirror.x = static_cast<float>(shown.Effect);

									SDL_PushGPUVertexUniformData(command, 0, &levelFrame, sizeof(levelFrame));
									bindSurface(level->Colour);

									result.DrawCalls += State->DrawSlots(
										command,
										pass,
										first,
										count,
										&levelLighting,
										shadow.Texture,
										shadow.Sampler,
										surfaceTexture,
										State->SurfaceSampler,
										surfaceFilter,
										result.Triangles
									);
									continue;
								}

								const Impl::SurfaceSlotState &shown = bank.Surfaces[index];
								if (!shown.Ready || !claimed[index]) {
									plainly();
									result.DrawCalls += State->DrawSlots(
										command,
										pass,
										first,
										count,
										&surfaceLighting,
										shadow.Texture,
										shadow.Sampler,
										surfaceTexture,
										State->SurfaceSampler,
										surfaceFilter,
										result.Triangles
									);
									continue;
								}

								const FrameUniforms mirrorFrame{
									state.ViewProjection,
									lightViewProjection,
									shown.PreviousSampling,
								};
								LightingUniforms mirrorLighting =
									lightingAt(surfaceEye, 1.0f, shown.ImageOpacity);

								// Which grade this surface's image goes through. On the
								// composite rather than on the render, so switching one
								// costs no redraw of the texture.
								mirrorLighting.Mirror.x = static_cast<float>(shown.Effect);

								SDL_PushGPUVertexUniformData(command, 0, &mirrorFrame, sizeof(mirrorFrame));
								bindSurface(shown.Texture[shown.Slot ^ 1u]);

								result.DrawCalls += State->DrawSlots(
									command,
									pass,
									first,
									count,
									&mirrorLighting,
									shadow.Texture,
									shadow.Sampler,
									surfaceTexture,
									State->SurfaceSampler,
									surfaceFilter,
									result.Triangles
								);
							}
						};

						drawMirrors(false);

						if (drawInterface && accepted[index].View->InstanceCount == 0) {
							result.DrawCalls += gameInterfaceHook->RecordWorld(
								command,
								pass,
								state.ViewProjection,
								accepted[index].View->Frame,
								surfaceWorldLighting.Ambient,
								surfaceWorldLighting.Direction,
								state.Width,
								state.Height,
								false
							);
						}

						// **`panesFollow` is true here**, because the blended mirrors
						// below go onto the same pipeline - so a tail of nothing but
						// mirrors still has to have it bound.
						drawBlendedInto(pass, worldFrame, surfaceLighting, surfaceFilter, true);

						// And the blended mirrors that are not this one, last of
						// everything drawn into this texture.
						if (plan.TransparentSurfaces > 0) {
							drawMirrors(true);
						}

						if (drawInterface && accepted[index].View->InstanceCount == 0) {
							result.DrawCalls += gameInterfaceHook->RecordWorld(
								command,
								pass,
								state.ViewProjection,
								accepted[index].View->Frame,
								surfaceWorldLighting.Ambient,
								surfaceWorldLighting.Direction,
								state.Width,
								state.Height,
								true
							);
						}

						SDL_EndGPURenderPass(pass);
					}
				}

				// **Marked ready only after every bounce has run**, so a surface that
				// was written this frame cannot be sampled as another surface's
				// "previous" within the same frame. From here the screen pass may
				// sample what was just written and the next frame's surface passes
				// may sample it as their previous.
				//
				// **The signature is recorded here and not where it was computed**,
				// which is what makes a surface that failed to render try again. A
				// slot only claims to be drawn with this signature once a pass has
				// actually drawn it; storing it up front would mark a skipped or
				// abandoned surface as current and leave it holding the wrong image
				// until something else in the scene moved.
				for (size_t index = 0; index < acceptedCount; index++) {
					if (!accepted[index].Refresh) {
						continue;
					}

					Impl::SurfaceSlotState &state = bank.Surfaces[accepted[index].Index];
					state.Ready = true;
					state.Signature = surfaceSignature;

					// **Stamped here for the same reason the signature is.** A slot
					// only claims to have drawn at a time once a pass has actually
					// drawn it; stamping up front would start the interval for a
					// surface that was then skipped or abandoned, and the picture
					// would end up an interval staler than the cap promises.
					state.Drawn = frameSeconds;
					result.SurfacePasses++;

					// The surface pass is level one, so a frame that drew any
					// surface at all has resolved at least that much.
					surfaceDepth.Resolved = std::max(surfaceDepth.Resolved, 1u);
				}

				// **Written back only where the pass actually ran**, which is the
				// half that is easy to get wrong and was. A surface whose signature
				// has not moved is deliberately not redrawn, so on a still scene most
				// frames draw no surface at all - and a measurement written on those
				// frames says "nothing was resolved", which reads back as one level
				// and throws away a depth the frames that did draw had worked out. A
				// skipped pass has measured nothing rather than measured zero.
				//
				// **A render fact, and it stays one.** Nothing here reaches an
				// `ecs::Store`: next frame's depth is derived from last frame's
				// picture, which is exactly the sort of thing `AGENTS.md` rule 5
				// refuses to let into a tick. A recorded run replays byte-identically
				// whatever this measured, because the simulation never sees it.
				bank.Bounces = surfaceDepth;
			}
			return true;
		});

		frameNodes.Set(core::Name("portal-capture"), [&](const graph::RunContext &context) {
			enterNamedPass(context.Name);

			// Last frame's light fields are for mouths that may be gone - a
			// disabled `Portal` reaches here as no `PortalView` at all, and its
			// spill has to go out with it. See `SeamLightTarget::Ready`.
			for (Impl::SeamLightTarget &seamLight : bank.SeamLights) {
				seamLight.Ready = false;
			}

			// --- the portal capture ----------------------------------------------
			//
			// **The same recursion as `fillMirror`, by a different map.** Both derive
			// each level's camera from the level above - that is what makes either one
			// compose, and the mirror pass was an iteration until v0.15 and wrong at
			// every level past the first for exactly the want of it.
			//
			// Here the derivation is the warp applied to *that* camera's frame, and
			// that camera's own projection skewed onto the mapped pane - exactly as
			// `Portal::Draw` composes `portalCam.worldView *= warp->delta`.
			// `NON-EUCLIDEAN.md`'s Part III is the whole argument.
			//
			// **What stays separate is the map and the lookup**, which is why the two
			// share `openScenePass`, `drawWorldInto` and `drawBlendedInto` and nothing
			// above them. A hole's sub-render is the screen's own frustum, so its pane
			// reads the texel it is standing on; a mirror's is fitted to its own
			// rectangle, so its pane reads by projecting its world position. Neither
			// lookup is expressible in the other's target.
			//
			// **Depth first, and every level's targets survive until the level above
			// has drawn all of its panes.** That is why the pool is indexed by level
			// *and* slot: level `L` renders the world and then draws every hole it can
			// see, so all of level `L-1` is live at once.
			if (havePortals && haveInstances && sceneCount > 0 && portalLevels > 0) {
				ENGINE_PROFILE_CAT("portal pass", core::ProfileCategory::Render);

				// **The unskewed screen projection, kept and re-skewed at every
				// level.** `scene::ObliqueProjection` substitutes the whole depth row,
				// reading two of the entries it is about to overwrite - so skewing an
				// already-skewed matrix is not the same as skewing the original
				// against the new plane, which is the arrangement `Camera::ClipOblique`
				// gets for free by writing the row from the untouched half of the
				// matrix. Starting from this every time is what makes each level's
				// frustum the screen's own, which is what makes the screen-position
				// lookup in `opaque.frag` exact.
				const float portalAspect = static_cast<float>(sceneWidth) / static_cast<float>(sceneHeight);
				const glm::mat4 screenProjection =
					scene::ResolveCamera(cameraFrame, drawCamera, portalAspect).Projection;

				// **Made before anything is captured, because a world of nothing but
				// holes never reaches `EnsureSurface`.** The sampler used to be
				// created there, so a portal-only scene took a null one into
				// `SDL_BindGPUFragmentSamplers` and died inside the backend - and a
				// scene with one mirror in it hid that completely.
				(void)State->EnsureSurfaceSampler();

				const ShadowBinding shadow = shadowBinding();

				// Where a hole's sub-camera stands, and what it looks through.
				//
				// **Which warp is a question about this level's camera, asked again at
				// every level.** A pane is a hole from either side, and one map serves
				// both: it carries the pane's front hemisphere to the far pane's back
				// one and its back to the far pane's front, so a sub-camera that has
				// stepped through and is now on the other side of something is carried
				// by the same matrix, the other way, for free. CodeParade's
				// `Portal::Connect` writes the same `delta` into both warps.
				//
				// Which *side* still has to be asked, because the clip plane's normal
				// is the way this camera is looking and that does flip.
				struct SubCamera {
					core::CFrame Frame;
					scene::CameraMatrices Matrices;
				};

				const auto subCameraFor = [&](const PortalView &portal, const core::CFrame &from) {
					const float side = (from.Position - portal.Centre).Dot(portal.Normal);
					const scene::SeamTransform &warp = portal.Warp;

					const core::CFrame placed = warp.Place(from);

					// **The clip normal points back through the hole**, which is the
					// one sign here worth deriving rather than trying. The map sends
					// the eye's side of the source pane to the *opposite* side of the
					// far one, so a sub-camera placed from an eye at `+outward` lands
					// behind the mapped pane looking back along `outward`'s image.
					// What has to survive clipping is everything beyond the mapped
					// pane, so the normal is the way this camera is looking and not
					// the way the pane faces.
					const core::Vector3 outward = portal.Normal * (side >= 0.0f ? 1.0f : -1.0f);
					const core::Vector3 clipNormal = warp.Rotate(outward) * -1.0f;

					// **Moved back towards this camera by a sliver, so the plane
					// keeps a little more rather than a little less.** The oblique
					// substitution makes this plane the near plane, so everything
					// between the sub-camera and it is thrown away - and the far
					// room's own geometry meets the mapped pane exactly, which after
					// two matrix products means some of it lands a float either side.
					// The half that lands short is clipped, and what that looks like
					// is a hairline of background around the inside of every hole,
					// with parts poking through it.
					//
					// **The sign is the whole of it and it is worth deriving rather
					// than trying.** `clipNormal` is the way this camera looks, so
					// adding along it pushes the plane deeper into the far room and
					// removes a slab of whatever is standing in the hole - a body
					// straddling the seam loses its far half and reads as a character
					// cut in two. CodeParade's `extra_clip` subtracts for this
					// reason: `pos - normal*extra_clip` with `normal` pointing away
					// from the camera is the pane moved *towards* it.
					const core::Vector3 clipPoint =
						warp.Point(portal.Centre) - clipNormal * scene::PortalClipBias(nearestPane);

					return SubCamera{
						placed,
						scene::ResolveSurfaceCamera(
							placed,
							scene::ObliqueProjection(
								screenProjection, placed, clipNormal, clipNormal.Dot(clipPoint)
							)
						),
					};
				};

				// One level: fill `bank.Portals[level][i]` for every hole `i` this
				// camera can see, then leave them for the caller to sample.
				//
				// A `std::function` because it calls itself and captures the frame.
				// The depth is bounded by `MAX_PORTAL_DEPTH`, so the recursion is four
				// deep at worst and the indirection is paid once per hole per level
				// beside a whole scene render.
				std::function<void(const scene::CameraMatrices &, const core::CFrame &, uint32_t, int8_t)>
					fillLevel;

				fillLevel = [&](const scene::CameraMatrices &from,
								const core::CFrame &fromFrame,
								uint32_t level,
								int8_t skip) {
					for (uint8_t slot = 0; slot < scene::MAX_SURFACES; slot++) {
						if (portalOf[slot] == nullptr) {
							continue;
						}

						const PortalView &portal = *portalOf[slot];

						// **The hole this camera just came out of**, which is at this
						// level's own clip plane and would render a scene that is then
						// entirely clipped away. CodeParade's `skipPortal` argument.
						if (portal.Index == skip) {
							continue;
						}

						// **Per portal per level, which is what stops the cost being
						// `holes ^ depth`.** A hole behind this level's camera costs
						// nothing, and most of them are.
						if (!graph::VisiblePane(
								from.ViewProjection, portal.Centre, portal.First, portal.Second
							)) {
							continue;
						}

						const SubCamera sub = subCameraFor(portal, fromFrame);

						if (level > 0) {
							fillLevel(sub.Matrices, sub.Frame, level - 1, portal.Partner);
						}

						Impl::PortalTarget *target =
							State->EnsurePortal(targetSlot, level, slot, targetWidth, targetHeight);
						if (target == nullptr) {
							continue;
						}

						// **The same rectangle as the level above draws into.** The
						// target is the attachment's size, and the world fills the
						// viewport's corner of it - so a pane in the level above reads
						// the texel it is standing on. Setting a different one here is
						// the whole of what would make the picture slide.
						const SDL_GPUViewport portalViewport{
							0.0f,
							0.0f,
							static_cast<float>(sceneWidth),
							static_cast<float>(sceneHeight),
							0.0f,
							1.0f
						};

						// **Not cycled, unlike a surface slot, and it was measured
						// rather than reasoned.** Cycling hands back a fresh allocation
						// per write, which is the right answer when two passes in one
						// frame share a texture - and at one level nothing does: a
						// target is written once and sampled once, by the pass above it,
						// in that order. Asking for a fresh allocation anyway made the
						// device hang more often rather than less. `Impl::PortalDepth`
						// carries what happens above one level, which is where the same
						// target *is* written twice.
						SDL_GPURenderPass *const pass = openScenePass(
							target->Colour, target->Depth, false, &portalViewport, lightUniforms
						);

						const FrameUniforms subFrameUniforms{
							sub.Matrices.ViewProjection,
							lightViewProjection,
							glm::mat4{1.0f},
						};
						SDL_PushGPUVertexUniformData(command, 0, &subFrameUniforms, sizeof(subFrameUniforms));

						const LightingUniforms subLighting = lightingAt(sub.Frame.Position, 0.0f, 1.0f);

						drawWorldInto(pass, subLighting, portal.TagFilter);

						// The holes this level can see, put back one at a time. Their
						// targets are `level - 1`, filled by the call above and still
						// untouched - which is why the pool is per level per slot.
						for (uint8_t seenSlot = 0; seenSlot < scene::MAX_SURFACES; seenSlot++) {
							if (portalOf[seenSlot] == nullptr ||
								portalOf[seenSlot]->Index == portal.Partner) {
								continue;
							}

							const PortalView &inner = *portalOf[seenSlot];
							const scene::SurfaceRun &run = plan.Runs[seenSlot];
							if (run.OpaqueCount == 0) {
								continue;
							}

							if (!graph::VisiblePane(
									sub.Matrices.ViewProjection, inner.Centre, inner.First, inner.Second
								)) {
								continue;
							}

							const Impl::PortalTarget *seen = level > 0 && bank.Portals.size() >= level
																 ? &bank.Portals[level - 1].Targets[seenSlot]
																 : nullptr;

							LightingUniforms paneLighting = subLighting;
							SDL_GPUTexture *paneTexture = nullptr;

							if (seen != nullptr && seen->Colour != nullptr) {
								// 2 is the screen-position lookup - see `opaque.frag`.
								paneLighting.Flags.z = 2.0f;
								paneTexture = seen->Colour;
								paneLighting.PaneNormal =
									glm::vec4{inner.Normal.X, inner.Normal.Y, inner.Normal.Z, 0.0f};
							} else {
								// **The terminus, and it is a shade rather than the
								// pane's own material.** This is the deepest level the
								// recursion goes to, so a hole seen here has nothing
								// behind it - and a lit grey slab at the end of a
								// corridor of holes reads as a wall somebody built,
								// which is the one thing the corridor is trying not to
								// look like. CodeParade draws pink here, deliberately
								// wrong, because their demo is about the mechanism; a
								// shipped world wants the chain to fade.
								//
								// The ambient is what it fades to, which is the far
								// room's own unlit tone and needs no second uniform to
								// say - 3 is the flat branch in `opaque.frag`.
								paneLighting.Flags.z = 3.0f;
							}

							result.DrawCalls += State->DrawSlots(
								command,
								pass,
								run.OpaqueFirst,
								run.OpaqueCount,
								&paneLighting,
								shadow.Texture,
								shadow.Sampler,
								paneTexture,
								State->SurfaceSampler,
								portal.TagFilter,
								result.Triangles
							);
						}

						// **`panesFollow` is false here**, because this level's panes
						// were drawn with the opaque head above - nothing follows that
						// needs the transparent pipeline bound for it.
						if (drawInterface) {
							result.DrawCalls += gameInterfaceHook->RecordWorld(
								command,
								pass,
								sub.Matrices.ViewProjection,
								sub.Frame,
								core::Color3{State->Ambient.x, State->Ambient.y, State->Ambient.z},
								core::Vector3{State->Sun.x, State->Sun.y, State->Sun.z},
								sceneWidth,
								sceneHeight,
								false
							);
						}

						drawBlendedInto(pass, subFrameUniforms, subLighting, portal.TagFilter, false);

						if (drawInterface) {
							result.DrawCalls += gameInterfaceHook->RecordWorld(
								command,
								pass,
								sub.Matrices.ViewProjection,
								sub.Frame,
								core::Color3{State->Ambient.x, State->Ambient.y, State->Ambient.z},
								core::Vector3{State->Sun.x, State->Sun.y, State->Sun.z},
								sceneWidth,
								sceneHeight,
								true
							);
						}

						SDL_EndGPURenderPass(pass);
						result.PortalPasses++;
					}
				};

				fillLevel(
					scene::CameraMatrices{
						glm::inverse(cameraFrame.ToMatrix()), screenProjection, cameraMatrix
					},
					cameraFrame,
					portalLevels - 1,
					-1
				);

				// --- the seam light-field captures -------------------------------
				//
				// **Each mouth's far room, rendered against a lit void.** A
				// stand-in eye at the mouth's centre looks through the hole and
				// is carried by the same warp a body crosses by, so what it sees
				// is the light arriving at the seam. The clear is the world's
				// ambient rather than the fog - a lit void - and the fog is
				// pushed out of reach, so the capture holds room lighting and
				// nothing atmospheric. `deferred-lighting.frag`'s `SeamSpill`
				// projects the matching capture back out of the entrance.
				//
				// **Viewer-independent, unlike the recursion above.** Light
				// spills out of a doorway whether or not anybody is looking at
				// the pane, so this does not test `VisiblePane` - a pair costs
				// two 128x128 forward passes per frame while its mouths are
				// enabled, and a disabled mouth never reaches this loop.
				for (uint8_t slot = 0; slot < scene::MAX_SURFACES; slot++) {
					if (portalOf[slot] == nullptr) {
						continue;
					}
					const PortalView &portal = *portalOf[slot];

					// The lit side is the viewer's side of this mouth: spill
					// onto the room the camera is in, and the partner mouth
					// serves the far room the same way.
					const float side = (cameraFrame.Position - portal.Centre).Dot(portal.Normal);
					const core::Vector3 outward = portal.Normal * (side >= 0.0f ? 1.0f : -1.0f);

					// Far enough off the plane that the oblique clip below stays
					// in front of the eye: the bias is derived from this same
					// distance, and a plane that lands behind the camera inverts
					// the frustum and captures nothing.
					constexpr float STAND_OFF = 0.5f;
					const core::Vector3 standPosition = portal.Centre + outward * STAND_OFF;
					const core::Vector3 upAxis = std::abs(outward.Y) > 0.99f
													 ? core::Vector3{0.0f, 0.0f, 1.0f}
													 : core::Vector3{0.0f, 1.0f, 0.0f};
					const core::CFrame stand =
						core::CFrame::LookAt(standPosition, standPosition - outward, upAxis);
					const core::CFrame placed = portal.Warp.Place(stand);

					// Wide and square: the capture is a light probe of a room,
					// not a picture, and a narrow lens would miss the lamps
					// standing beside the doorway.
					scene::Camera captureCamera = drawCamera;
					captureCamera.FieldOfViewRadians = 1.9f;
					captureCamera.NearPlane = 0.05f;
					const glm::mat4 captureProjection =
						scene::ResolveCamera(placed, captureCamera, 1.0f).Projection;

					// The same backward-pointing clip as `subCameraFor`, so the
					// wall the far mouth is set into does not fill the capture.
					// The bias is the stand-in eye's own seam distance rather
					// than the viewer's - `PortalClipBias` halves it, keeping
					// the plane in front of an eye the viewer's bias could put
					// it behind.
					const core::Vector3 clipNormal = portal.Warp.Rotate(outward) * -1.0f;
					const core::Vector3 clipPoint =
						portal.Warp.Point(portal.Centre) - clipNormal * scene::PortalClipBias(STAND_OFF);
					const scene::CameraMatrices captureMatrices = scene::ResolveSurfaceCamera(
						placed,
						scene::ObliqueProjection(
							captureProjection, placed, clipNormal, clipNormal.Dot(clipPoint)
						)
					);

					Impl::SeamLightTarget *seamLight = State->EnsureSeamLight(targetSlot, slot);
					if (seamLight == nullptr) {
						continue;
					}

					const SDL_GPUViewport seamViewport{
						0.0f,
						0.0f,
						static_cast<float>(seamLight->Width),
						static_cast<float>(seamLight->Height),
						0.0f,
						1.0f
					};

					// The lit void. `Ambient` is already in the linear working
					// space the pass writes, which is the space a clear on an
					// sRGB target is given in.
					const SDL_FColor voidColour{
						State->Ambient.x,
						State->Ambient.y,
						State->Ambient.z,
						1.0f,
					};

					SDL_GPURenderPass *const pass = openScenePass(
						seamLight->Colour, seamLight->Depth, false, &seamViewport, lightUniforms, &voidColour
					);

					const FrameUniforms captureUniforms{
						captureMatrices.ViewProjection,
						lightViewProjection,
						glm::mat4{1.0f},
					};
					SDL_PushGPUVertexUniformData(command, 0, &captureUniforms, sizeof(captureUniforms));

					LightingUniforms voidLighting = lightingAt(placed.Position, 0.0f, 1.0f);
					// No fog in a light probe: what falls to distance falls to
					// the void the clear already painted.
					voidLighting.Fog = glm::vec4{1.0e6f, 1.0e6f + 1.0f, 0.0f, 0.0f};

					drawWorldInto(pass, voidLighting, portal.TagFilter);
					drawBlendedInto(pass, captureUniforms, voidLighting, portal.TagFilter, false);

					SDL_EndGPURenderPass(pass);

					seamLight->Centre = glm::vec4{portal.Centre.X, portal.Centre.Y, portal.Centre.Z, 1.0f};

					// The spill reaches about a doorway's span into the room:
					// past that the window falloff has taken it below anything
					// the ambient does not already cover.
					const float reach =
						2.0f * std::max(portal.First.Magnitude() + portal.Second.Magnitude(), 1.0f);
					seamLight->Outward = glm::vec4{outward.X, outward.Y, outward.Z, reach};
					seamLight->First = glm::vec4{portal.First.X, portal.First.Y, portal.First.Z, 0.0f};
					seamLight->Second = glm::vec4{portal.Second.X, portal.Second.Y, portal.Second.Z, 0.0f};
					seamLight->Ready = true;
				}
			}
			return true;
		});

		// --- view targets ---------------------------------------------------

		// **The world's target, which is the offscreen texture or the window.**
		// Graph passes continue through this target. Host chrome is recorded only
		// after `output-image`, so it cannot leak into graph previews or captures.
		SDL_GPUColorTargetInfo colourTarget{};
		colourTarget.texture = offscreen ? State->SlotAt(targetSlot).Texture : swapchain;
		colourTarget.clear_color = SDL_FColor{
			State->FogColour.r,
			State->FogColour.g,
			State->FogColour.b,
			1.0f,
		};
		colourTarget.load_op = SDL_GPU_LOADOP_CLEAR;
		colourTarget.store_op = SDL_GPU_STOREOP_STORE;

		// What the overlay and the interface draw onto. When the world went
		// offscreen the window has never been touched this frame, so the first
		// pass to reach it clears - otherwise it is whatever the driver handed
		// back, which is last frame's image or uninitialised memory.
		SDL_GPUColorTargetInfo windowTarget{};
		windowTarget.texture = swapchain;
		windowTarget.clear_color = colourTarget.clear_color;
		windowTarget.load_op = offscreen ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
		windowTarget.store_op = SDL_GPU_STOREOP_STORE;

		SDL_GPUDepthStencilTargetInfo depthTarget{};
		// The one `EnsureDepth` above filled: this slot's when the world is going
		// into a texture, the shared window one when it is going to the
		// swapchain. See `SceneSlot::Depth`.
		depthTarget.texture = offscreen ? State->SlotAt(targetSlot).Depth : State->DepthTexture;
		depthTarget.clear_depth = 1.0f;
		depthTarget.load_op = SDL_GPU_LOADOP_CLEAR;
		// Nothing reads depth after the pass, so there is no reason to write it
		// back out to memory.
		depthTarget.store_op = SDL_GPU_STOREOP_DONT_CARE;
		depthTarget.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
		depthTarget.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
		depthTarget.cycle = true;

		// --- default PBR graph ---------------------------------------------
		//
		// The material-producing head and its explicit consumers. Projected
		// surfaces and blended geometry are submitted by the graph's transparent
		// node over the finished image.
		const auto outputDimensions =
			[&](core::Name kind, size_t output, uint32_t &outWidth, uint32_t &outHeight) {
				outWidth = sceneWidth;
				outHeight = sceneHeight;
				const graph::Node *node = nullptr;
				for (uint32_t value = 1; value <= selectedPipeline->Graph.Count(); value++) {
					const graph::Node *candidate = selectedPipeline->Graph.Find(graph::NodeId{value});
					if (candidate != nullptr && candidate->Kind == kind) {
						node = candidate;
						break;
					}
				}
				if (node == nullptr || output >= node->Writes.size()) {
					return;
				}
				const graph::ResourceDesc *resource =
					selectedPipeline->Graph.FindResource(node->Writes[output]);
				if (resource != nullptr) {
					resource->Resolve(sceneWidth, sceneHeight, outWidth, outHeight);
				}
			};
		Impl::PbrDimensions pbrDimensions;
		pbrDimensions.TargetWidth = targetWidth;
		pbrDimensions.TargetHeight = targetHeight;
		pbrDimensions.ViewWidth = sceneWidth;
		pbrDimensions.ViewHeight = sceneHeight;
		pbrDimensions.LinearWidth = sceneWidth;
		pbrDimensions.LinearHeight = sceneHeight;
		pbrDimensions.OcclusionWidth = sceneWidth;
		pbrDimensions.OcclusionHeight = sceneHeight;
		pbrDimensions.LitWidth = sceneWidth;
		pbrDimensions.LitHeight = sceneHeight;
		outputDimensions(
			core::Name("depth-linearise"), 0, pbrDimensions.LinearWidth, pbrDimensions.LinearHeight
		);
		outputDimensions(core::Name("ssao"), 0, pbrDimensions.OcclusionWidth, pbrDimensions.OcclusionHeight);
		outputDimensions(core::Name("deferred-lighting"), 0, pbrDimensions.LitWidth, pbrDimensions.LitHeight);
		const bool needsPbrTargets =
			graphEnabled(core::Name("gbuffer")) || graphEnabled(core::Name("depth-linearise")) ||
			graphEnabled(core::Name("ssao")) || graphEnabled(core::Name("deferred-lighting")) ||
			graphEnabled(core::Name("tonemap")) || graphEnabled(core::Name("transparent"));
		const bool graphTargetsReady = !needsPbrTargets || State->EnsurePbr(targetSlot, pbrDimensions);
		if (!graphTargetsReady) {
			closePass();
			State->Timestamps.Abandon(timingSlot);
			if (timingSlot < VulkanTimestamps::SLOTS) {
				State->PendingMarks[timingSlot].clear();
			}
			endIncompleteView();
			return result;
		}
		Impl::PbrSlot &pbr = State->PbrAt(targetSlot);
		SDL_GPUTexture *const viewTarget = colourTarget.texture;
		const float aspect = static_cast<float>(sceneWidth) / static_cast<float>(sceneHeight);
		const scene::CameraMatrices matrices = scene::ResolveCamera(cameraFrame, drawCamera, aspect);
		const FrameUniforms frameUniforms{
			matrices.ViewProjection,
			lightViewProjection,
			glm::mat4{1.0f},
		};
		const LightingUniforms lighting = lightingAt(cameraFrame.Position, 0.0f, 0.0f);

		PbrUniforms uniforms;
		uniforms.InverseViewProjection = glm::inverse(matrices.ViewProjection);
		uniforms.LightViewProjection = lightViewProjection;
		uniforms.Planes = glm::vec4{
			drawCamera.NearPlane,
			drawCamera.FarPlane,
			drawCamera.NearPlane > 0.0f ? 1.0f / drawCamera.NearPlane : 0.0f,
			drawCamera.FarPlane > 0.0f ? 1.0f / drawCamera.FarPlane : 0.0f,
		};
		uniforms.Target = glm::vec4{
			static_cast<float>(sceneWidth),
			static_cast<float>(sceneHeight),
			static_cast<float>(sceneWidth) / static_cast<float>(targetWidth),
			static_cast<float>(sceneHeight) / static_cast<float>(targetHeight),
		};
		uniforms.Direction = glm::vec4{State->Sun, 0.0f};
		uniforms.Ambient = State->Ambient;
		uniforms.OutdoorAmbient = State->OutdoorAmbient;
		uniforms.Direct = State->Direct;
		uniforms.Eye =
			glm::vec4{cameraFrame.Position.X, cameraFrame.Position.Y, cameraFrame.Position.Z, 1.0f};
		uniforms.FogColour = glm::vec4{
			WorkingFromDisplay(State->FogColour.r),
			WorkingFromDisplay(State->FogColour.g),
			WorkingFromDisplay(State->FogColour.b),
			1.0f,
		};
		uniforms.Fog = glm::vec4{State->FogStart, State->FogEnd, 0.0f, 0.0f};
		uniforms.Shadow = glm::vec4{
			haveShadow ? 1.0f : 0.0f,
			1.0f / static_cast<float>(SHADOW_RESOLUTION),
			0.0f,
			0.0f,
		};

		const SDL_GPUViewport sceneViewport{
			0.0f,
			0.0f,
			static_cast<float>(sceneWidth),
			static_cast<float>(sceneHeight),
			0.0f,
			1.0f,
		};
		const SDL_Rect sceneScissor{0, 0, static_cast<int>(sceneWidth), static_cast<int>(sceneHeight)};

		frameNodes.Set(core::Name("gbuffer"), [&](const graph::RunContext &context) {
			enterNamedPass(context.Name);

			// One pass plainly, or two around the occlusion cull. The second
			// begins where the first ended - every target loads - so the two
			// together paint exactly the frame one pass would have, minus the
			// pixels the cull proved covered.
			const bool occluded = State->OcclusionFrame.Active && haveInstances && plainOpaque > 0 &&
								  State->EnsurePyramid(sceneWidth, sceneHeight);

			const auto beginGBuffer = [&](bool clear) {
				SDL_GPUColorTargetInfo gbufferTargets[4]{};
				for (size_t target = 0; target < 4; target++) {
					gbufferTargets[target].clear_color = SDL_FColor{0.0f, 0.0f, 0.0f, 0.0f};
					gbufferTargets[target].load_op = clear ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
					gbufferTargets[target].store_op = SDL_GPU_STOREOP_STORE;
					// Cycling is for the frame's first touch; the late pass
					// must draw over the early pass's pixels, not fresh memory.
					gbufferTargets[target].cycle = clear;
				}
				gbufferTargets[0].texture = pbr.Albedo;
				gbufferTargets[1].texture = pbr.Normal;
				gbufferTargets[2].texture = pbr.Material;
				gbufferTargets[3].texture = pbr.Emissive;

				depthTarget.load_op = clear ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
				depthTarget.store_op = SDL_GPU_STOREOP_STORE;
				depthTarget.cycle = clear;

				SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(command, gbufferTargets, 4, &depthTarget);
				if (pass == nullptr) {
					return pass;
				}
				State->BindPipeline(pass, State->GBufferPipeline, Impl::PipelineFamily::Other);
				SDL_SetGPUViewport(pass, &sceneViewport);
				SDL_SetGPUScissor(pass, &sceneScissor);
				SDL_PushGPUVertexUniformData(command, 0, &frameUniforms, sizeof(frameUniforms));
				return pass;
			};
			const auto drawOpaque =
				[&](SDL_GPURenderPass *pass, SDL_GPUBuffer *instances, const Impl::IndirectPhase *phase) {
					const SDL_GPUBufferBinding vertexBindings[] = {
						{State->Meshes.Vertices(), 0},
						{instances, 0},
					};
					SDL_BindGPUVertexBuffers(pass, 0, vertexBindings, 2);
					const SDL_GPUBufferBinding indexBinding{State->Meshes.Indices(), 0};
					SDL_BindGPUIndexBuffer(pass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);
					result.DrawCalls += State->DrawSlots(
						command,
						pass,
						sceneCount,
						plainOpaque,
						&lighting,
						State->ShadowTexture,
						State->ShadowSampler,
						nullptr,
						State->SurfaceSampler,
						0,
						result.Triangles,
						phase
					);
				};

			if (!occluded) {
				SDL_GPURenderPass *gbuffer = beginGBuffer(true);
				if (gbuffer == nullptr) {
					ENGINE_ERROR("gbuffer: SDL_BeginGPURenderPass: {}", SDL_GetError());
					return false;
				}
				if (haveInstances && plainOpaque > 0) {
					drawOpaque(gbuffer, State->InstanceBuffer, nullptr);
				}
				SDL_EndGPURenderPass(gbuffer);
				return true;
			}

			// Trace rather than a counter in `FrameResult`: the survivor count
			// lives on the GPU and never comes back, so the honest numbers are
			// the two the CPU decided.
			ENGINE_TRACE(
				"gbuffer: occlusion cull of {} candidate(s) behind {} occluder(s) in {} run(s)",
				State->OcclusionFrame.CandidateCount,
				State->OcclusionFrame.EarlyTotal,
				State->OcclusionFrame.RunCount
			);

			// Early phase: the CPU-picked occluders, by indirect arguments so
			// both phases drive their draws the same way.
			const Impl::IndirectPhase early{State->Occlusion.Arguments, 0, &State->OcclusionFrame.RunEarly};
			SDL_GPURenderPass *earlyPass = beginGBuffer(true);
			if (earlyPass == nullptr) {
				ENGINE_ERROR("gbuffer early: SDL_BeginGPURenderPass: {}", SDL_GetError());
				return false;
			}
			drawOpaque(earlyPass, State->InstanceBuffer, &early);
			SDL_EndGPURenderPass(earlyPass);

			// The pyramid over what the occluders wrote, then the cull that
			// compacts the survivors and fills the late arguments.
			State->BuildPyramid(command, depthTarget.texture);
			State->DispatchOcclusionCull(command, frameUniforms.ViewProjection);

			// Late phase: the survivors, loading everything the early phase
			// stored.
			const Impl::IndirectPhase late{
				State->Occlusion.Arguments,
				State->OcclusionFrame.ArgCount,
				&State->OcclusionFrame.RunCandidates
			};
			SDL_GPURenderPass *latePass = beginGBuffer(false);
			if (latePass == nullptr) {
				ENGINE_ERROR("gbuffer late: SDL_BeginGPURenderPass: {}", SDL_GetError());
				return false;
			}
			drawOpaque(latePass, State->Occlusion.LateInstances, &late);
			SDL_EndGPURenderPass(latePass);
			return true;
		});

		const auto fullscreen = [&](core::Name name,
									SDL_GPUGraphicsPipeline *pipeline,
									SDL_GPUTexture *target,
									uint32_t passWidth,
									uint32_t passHeight,
									std::span<const SDL_GPUTextureSamplerBinding> bindings,
									const PbrUniforms *passUniforms,
									const LightUniforms *passLights,
									SDL_FColor clear) {
			enterNamedPass(name);
			SDL_GPUColorTargetInfo colour{};
			colour.texture = target;
			colour.clear_color = clear;
			colour.load_op = SDL_GPU_LOADOP_CLEAR;
			colour.store_op = SDL_GPU_STOREOP_STORE;
			colour.cycle = true;
			SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(command, &colour, 1, nullptr);
			SDL_BindGPUGraphicsPipeline(pass, pipeline);
			if (!bindings.empty()) {
				SDL_BindGPUFragmentSamplers(pass, 0, bindings.data(), static_cast<uint32_t>(bindings.size()));
			}
			if (passUniforms != nullptr) {
				SDL_PushGPUFragmentUniformData(command, 0, passUniforms, sizeof(*passUniforms));
			}
			if (passLights != nullptr) {
				SDL_PushGPUFragmentUniformData(command, 1, passLights, sizeof(*passLights));
			}
			const SDL_GPUViewport viewport{
				0.0f, 0.0f, static_cast<float>(passWidth), static_cast<float>(passHeight), 0.0f, 1.0f
			};
			const SDL_Rect scissor{0, 0, static_cast<int>(passWidth), static_cast<int>(passHeight)};
			SDL_SetGPUViewport(pass, &viewport);
			SDL_SetGPUScissor(pass, &scissor);
			SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
			SDL_EndGPURenderPass(pass);
			result.DrawCalls++;
		};

		const auto fixedTexture = [&](core::Name resource, size_t slot) {
			Impl::NamedTexture texture;
			const Impl::ResourceRole role = State->RoleFor(resource);
			if (role == Impl::ResourceRole::PreviousFrame) {
				if (slot < State->SceneSlots.size()) {
					const Impl::SceneSlot &history = State->SceneSlots[slot];
					return Impl::NamedTexture{
						history.HistoryReady ? history.History : nullptr,
						history.HistoryWidth,
						history.HistoryHeight,
						State->ColourFormat(),
					};
				}
				return texture;
			}
			if (role == Impl::ResourceRole::Scene) {
				if (slot == targetSlot) {
					return Impl::NamedTexture{viewTarget, sceneWidth, sceneHeight, State->ColourFormat()};
				}
				if (slot < State->SceneSlots.size()) {
					const Impl::SceneSlot &scene = State->SceneSlots[slot];
					return Impl::NamedTexture{
						scene.Texture, scene.DrawnWidth, scene.DrawnHeight, State->ColourFormat()
					};
				}
				return texture;
			}
			if (role == Impl::ResourceRole::Depth) {
				if (slot < State->SceneSlots.size()) {
					const Impl::SceneSlot &scene = State->SceneSlots[slot];
					return Impl::NamedTexture{
						scene.Depth, scene.DepthWidth, scene.DepthHeight, State->DepthFormat
					};
				}
				return texture;
			}
			if (role == Impl::ResourceRole::Shadow) {
				return Impl::NamedTexture{
					State->ShadowTexture,
					SHADOW_RESOLUTION,
					SHADOW_RESOLUTION,
					State->DepthFormat,
				};
			}
			if (role == Impl::ResourceRole::Surface && slot < State->SurfaceBanks.size()) {
				const Impl::SurfaceSlotState &surface = State->SurfaceBanks[slot].Surfaces[0];
				return Impl::NamedTexture{
					surface.Ready ? surface.Texture[surface.Slot] : nullptr,
					surface.Width,
					surface.Height,
					State->ColourFormat(),
				};
			}
			if (role == Impl::ResourceRole::PortalImage && slot < State->SurfaceBanks.size()) {
				const std::vector<Impl::PortalLevel> &levels = State->SurfaceBanks[slot].Portals;
				for (auto level = levels.rbegin(); level != levels.rend(); ++level) {
					for (const Impl::PortalTarget &portal : level->Targets) {
						SDL_GPUTexture *texture = portal.Colour;
						if (texture == nullptr) {
							continue;
						}
						return Impl::NamedTexture{
							texture,
							portal.Width,
							portal.Height,
							State->ColourFormat(),
						};
					}
				}
			}
			if (role == Impl::ResourceRole::PortalDisplay && slot < State->SurfaceBanks.size()) {
				const std::vector<Impl::PortalLevel> &levels = State->SurfaceBanks[slot].Portals;
				for (auto level = levels.rbegin(); level != levels.rend(); ++level) {
					for (const Impl::PortalTarget &portal : level->Targets) {
						if (portal.Display == nullptr) {
							continue;
						}
						return Impl::NamedTexture{
							portal.Display,
							portal.Width,
							portal.Height,
							State->ColourFormat(),
						};
					}
				}
			}
			if (role == Impl::ResourceRole::PortalLight && slot < State->SurfaceBanks.size()) {
				for (const Impl::SeamLightTarget &seamLight : State->SurfaceBanks[slot].SeamLights) {
					if (seamLight.Ready && seamLight.Colour != nullptr) {
						return Impl::NamedTexture{
							seamLight.Colour,
							seamLight.Width,
							seamLight.Height,
							State->ColourFormat(),
						};
					}
				}
			}
			if (slot >= State->PbrSlots.size()) {
				return texture;
			}
			const Impl::PbrSlot &slotPbr = State->PbrSlots[slot];
			if (role == Impl::ResourceRole::Albedo) {
				return Impl::NamedTexture{
					slotPbr.Albedo,
					slotPbr.Dimensions.TargetWidth,
					slotPbr.Dimensions.TargetHeight,
					SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB,
				};
			}
			if (role == Impl::ResourceRole::Normal) {
				return Impl::NamedTexture{
					slotPbr.Normal,
					slotPbr.Dimensions.TargetWidth,
					slotPbr.Dimensions.TargetHeight,
					SDL_GPU_TEXTUREFORMAT_R10G10B10A2_UNORM,
				};
			}
			if (role == Impl::ResourceRole::Material) {
				return Impl::NamedTexture{
					slotPbr.Material,
					slotPbr.Dimensions.TargetWidth,
					slotPbr.Dimensions.TargetHeight,
					SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
				};
			}
			if (role == Impl::ResourceRole::Emissive) {
				return Impl::NamedTexture{
					slotPbr.Emissive,
					slotPbr.Dimensions.TargetWidth,
					slotPbr.Dimensions.TargetHeight,
					SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT,
				};
			}
			if (role == Impl::ResourceRole::LinearDepth) {
				return Impl::NamedTexture{
					slotPbr.LinearDepth,
					slotPbr.Dimensions.LinearWidth,
					slotPbr.Dimensions.LinearHeight,
					SDL_GPU_TEXTUREFORMAT_R32_FLOAT,
				};
			}
			if (role == Impl::ResourceRole::Occlusion) {
				return Impl::NamedTexture{
					slotPbr.Occlusion,
					slotPbr.Dimensions.OcclusionWidth,
					slotPbr.Dimensions.OcclusionHeight,
					SDL_GPU_TEXTUREFORMAT_R8_UNORM,
				};
			}
			if (role == Impl::ResourceRole::Lit) {
				return Impl::NamedTexture{
					slotPbr.Lit,
					slotPbr.Dimensions.LitWidth,
					slotPbr.Dimensions.LitHeight,
					SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT,
				};
			}
			return texture;
		};

		const auto resourceTexture = [&](graph::ResourceId resource, size_t selectedSlot, bool make) {
			const graph::ResourceDesc *desc = selectedPipeline->Graph.FindResource(resource);
			if (desc == nullptr) {
				return Impl::NamedTexture{};
			}
			Impl::NamedTexture fixed = fixedTexture(desc->Name, selectedSlot);
			if (fixed.IsValid()) {
				return fixed;
			}
			const graph::NodeScope scope = State->ResourceScope(*selectedPipeline, resource);
			const uint64_t owner = scope == graph::NodeScope::View	  ? static_cast<uint64_t>(selectedSlot)
								   : scope == graph::NodeScope::World ? world
																	  : 0;
			if (desc->External) {
				if (desc->Name == core::Name("window")) {
					return Impl::NamedTexture{swapchain, width, height, State->ColourFormat()};
				}
				if (!make) {
					return State->FindGraphTarget(*selectedPipeline, desc->Name, scope, owner);
				}
			}

			if (make) {
				// Graph images ultimately feed this view's output target. Using the
				// Studio swapchain here makes an offscreen interface draw in one
				// coordinate space while its pixel scissors are applied in another.
				const uint32_t resourceWidth = sceneWidth;
				const uint32_t resourceHeight = sceneHeight;
				return State->EnsureGraphTarget(
					*selectedPipeline, resource, owner, resourceWidth, resourceHeight
				);
			}
			return State->FindGraphTarget(*selectedPipeline, desc->Name, scope, owner);
		};
		const auto graphTexture =
			[&](graph::ResourceId resource, const graph::RunContext &context, bool make) {
				const graph::Node *node = selectedPipeline->Graph.Find(context.Node);
				const bool selectsView = context.View == graph::RunContext::WHOLE_FRAME && node != nullptr &&
										 node->Parameter(core::Name("view")) != nullptr;
				const size_t selectedSlot = selectsView ? node->Integer(core::Name("view"), 0) : targetSlot;
				return resourceTexture(resource, selectedSlot, make);
			};

		const auto textureBindings = [&](const graph::RunContext &context) {
			std::vector<SDL_GPUTextureSamplerBinding> bindings;
			for (const graph::ResourceId resource : context.Reads) {
				const graph::ResourceDesc *desc = selectedPipeline->Graph.FindResource(resource);
				if (desc == nullptr || desc->Kind == graph::ResourceKind::Buffer ||
					desc->Kind == graph::ResourceKind::Camera ||
					desc->Kind == graph::ResourceKind::Entities) {
					continue;
				}
				Impl::NamedTexture source = graphTexture(resource, context, false);
				bindings.push_back(
					SDL_GPUTextureSamplerBinding{
						source.IsValid() ? source.Texture : State->FallbackTexture,
						State->SurfaceSampler,
					}
				);
			}
			return bindings;
		};

		const auto singleChannel = [](const Impl::NamedTexture &source) {
			switch (source.Format) {
			case SDL_GPU_TEXTUREFORMAT_R8_UNORM:
			case SDL_GPU_TEXTUREFORMAT_R32_FLOAT:
			case SDL_GPU_TEXTUREFORMAT_D16_UNORM:
			case SDL_GPU_TEXTUREFORMAT_D24_UNORM:
			case SDL_GPU_TEXTUREFORMAT_D32_FLOAT:
			case SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT:
			case SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT:
				return true;
			default:
				return false;
			}
		};
		const auto drawImage = [&](const Impl::NamedTexture &source,
								   const Impl::NamedTexture &target,
								   SDL_GPULoadOp load,
								   bool reverseSpectrum = false) {
			if (!source.IsValid() || !target.IsValid()) {
				return false;
			}
			SDL_GPUColorTargetInfo colour{};
			colour.texture = target.Texture;
			colour.clear_color = SDL_FColor{0.0f, 0.0f, 0.0f, 1.0f};
			colour.load_op = load;
			colour.store_op = SDL_GPU_STOREOP_STORE;
			colour.cycle = load == SDL_GPU_LOADOP_CLEAR;
			SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(command, &colour, 1, nullptr);
			State->BindPipeline(pass, State->ImagePipeline, Impl::PipelineFamily::Other);
			const SDL_GPUTextureSamplerBinding binding{source.Texture, State->SurfaceSampler};
			SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
			const ImageUniformMode mode = ImageMode(singleChannel(source), reverseSpectrum);
			SDL_PushGPUFragmentUniformData(command, 0, &mode, sizeof(mode));
			SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
			SDL_EndGPURenderPass(pass);
			result.DrawCalls++;
			return true;
		};
		const auto drawOverlayImage =
			[&](SDL_GPUTexture *source, const Impl::NamedTexture &target, SDL_GPULoadOp load) {
				if (source == nullptr || !target.IsValid()) {
					return false;
				}
				SDL_GPUColorTargetInfo colour{};
				colour.texture = target.Texture;
				colour.load_op = load;
				colour.store_op = SDL_GPU_STOREOP_STORE;
				SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(command, &colour, 1, nullptr);
				State->BindPipeline(pass, State->OverlayPipeline, Impl::PipelineFamily::Other);
				const SDL_GPUTextureSamplerBinding binding{source, State->OverlaySampler};
				SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
				SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
				SDL_EndGPURenderPass(pass);
				result.DrawCalls++;
				return true;
			};

		frameNodes.Set(core::Name("last-frame"), [&](const graph::RunContext &context) {
			// `output-image` copies the completed graph output into renderer-owned
			// history. This node is the dependency and profile boundary at which the
			// following frame may sample that image.
			enterNamedPass(context.Name);
			return true;
		});

		frameNodes.Set(core::Name("raster"), [&](const graph::RunContext &context) {
			enterNamedPass(context.Name);
			const graph::Node *node = selectedPipeline->Graph.Find(context.Node);
			if (node == nullptr) {
				return false;
			}
			Impl::NamedTexture target;
			for (const graph::ResourceId resource : context.Writes) {
				const graph::ResourceDesc *desc = selectedPipeline->Graph.FindResource(resource);
				if (desc != nullptr && desc->Kind == graph::ResourceKind::Colour) {
					target = graphTexture(resource, context, true);
					break;
				}
			}
			if (!target.IsValid()) {
				ENGINE_WARN("'{}' has no colour target to draw into", context.Name.Text());
				return true;
			}
			const std::vector<SDL_GPUTextureSamplerBinding> bindings = textureBindings(context);
			SDL_GPUGraphicsPipeline *raster =
				State->GraphRasterFor(*selectedPipeline, *node, target.Format, bindings.size());
			if (raster == nullptr) {
				return true;
			}

			GraphPassUniforms passUniforms;
			passUniforms.ViewProjection = matrices.ViewProjection;
			passUniforms.InverseViewProjection = glm::inverse(matrices.ViewProjection);
			passUniforms.Target = glm::vec4{
				static_cast<float>(target.Width),
				static_cast<float>(target.Height),
				1.0f / static_cast<float>(target.Width),
				1.0f / static_cast<float>(target.Height),
			};
			passUniforms.View = glm::vec4{
				static_cast<float>(State->AnimationSeconds),
				drawCamera.FieldOfViewRadians,
				static_cast<float>(target.Width) / static_cast<float>(target.Height),
				0.0f,
			};

			SDL_GPUColorTargetInfo colour{};
			colour.texture = target.Texture;
			colour.clear_color = SDL_FColor{
				node->Number(core::Name("clear.r"), 0.0f),
				node->Number(core::Name("clear.g"), 0.0f),
				node->Number(core::Name("clear.b"), 0.0f),
				node->Number(core::Name("clear.a"), 1.0f),
			};
			const std::string *load = node->Parameter(core::Name("load"));
			colour.load_op = load != nullptr && *load == "load" ? SDL_GPU_LOADOP_LOAD : SDL_GPU_LOADOP_CLEAR;
			colour.store_op = SDL_GPU_STOREOP_STORE;
			colour.cycle = true;
			SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(command, &colour, 1, nullptr);
			SDL_BindGPUGraphicsPipeline(pass, raster);
			if (!bindings.empty()) {
				SDL_BindGPUFragmentSamplers(pass, 0, bindings.data(), static_cast<uint32_t>(bindings.size()));
			}
			SDL_PushGPUFragmentUniformData(command, 0, &passUniforms, sizeof(passUniforms));
			SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
			SDL_EndGPURenderPass(pass);
			result.DrawCalls++;
			return true;
		});

		frameNodes.Set(core::Name("dispatch"), [&](const graph::RunContext &context) {
			const graph::Node *node = selectedPipeline->Graph.Find(context.Node);
			if (node == nullptr) {
				return false;
			}
			std::vector<SDL_GPUStorageTextureReadWriteBinding> writes;
			Impl::NamedTexture firstTarget;
			for (const graph::ResourceId resource : context.Writes) {
				const graph::ResourceDesc *desc = selectedPipeline->Graph.FindResource(resource);
				if (desc == nullptr || desc->Kind != graph::ResourceKind::Storage) {
					continue;
				}
				Impl::NamedTexture target = graphTexture(resource, context, true);
				if (!target.IsValid()) {
					continue;
				}
				if (!firstTarget.IsValid()) {
					firstTarget = target;
				}
				SDL_GPUStorageTextureReadWriteBinding binding{};
				binding.texture = target.Texture;
				binding.cycle = true;
				writes.push_back(binding);
			}
			if (writes.empty()) {
				ENGINE_WARN("'{}' has no storage target to dispatch into", context.Name.Text());
				return true;
			}
			const std::vector<SDL_GPUTextureSamplerBinding> bindings = textureBindings(context);
			const uint32_t localX = node->Integer(core::Name("local.x"), 8);
			const uint32_t localY = node->Integer(core::Name("local.y"), 8);
			const uint32_t localZ = node->Integer(core::Name("local.z"), 1);
			if (localX == 0 || localY == 0 || localZ == 0) {
				ENGINE_WARN("'{}' asks for a zero-sized compute thread group", context.Name.Text());
				return true;
			}
			SDL_GPUComputePipeline *compute = State->GraphComputeFor(
				*selectedPipeline, *node, bindings.size(), writes.size(), localX, localY, localZ
			);
			if (compute == nullptr) {
				return true;
			}

			const graph::ScheduledNode *scheduled = scheduledFor(context.Node);

			// The traffic plan decides which command buffer this dispatch
			// belongs to. A compute buffer ahead of the plan's first graphics
			// buffer may submit on its own before the main stream; the runtime
			// guards below keep that promise when a batch or execution order
			// has already put work in the main buffer.
			//
			// Dependency-bound compute - a compute buffer the plan places
			// between graphics buffers - stays in the main stream on SDL: one
			// unified queue offers no overlap to win, the present is bound to
			// the buffer that acquired the swapchain so the graphics stream
			// cannot be cut around it, and a pass recorded for later submission
			// could read textures a later main-stream pass cycles. The plan
			// still carries the boundary, so a backend with an independent
			// compute queue can lift it without re-planning.
			const auto planLeadsGraphics = [&](graph::NodeId node) {
				for (const graph::PlannedCommandBuffer &buffer : selectedPipeline->Buffers) {
					if (buffer.Class == graph::CommandBufferClass::Graphics) {
						return false;
					}
					if (buffer.Class == graph::CommandBufferClass::Compute &&
						std::find(buffer.Nodes.begin(), buffer.Nodes.end(), node) != buffer.Nodes.end()) {
						return true;
					}
				}
				return false;
			};
			const bool separateCommand = scheduled != nullptr && scheduled->AsyncEligible &&
										 planLeadsGraphics(context.Node) && !mainGpuWorkRecorded &&
										 !dedicatedComputeSubmitted &&
										 (!State->BatchActive || State->BatchFirst);
			SDL_GPUCommandBuffer *dispatchCommand = command;
			if (separateCommand) {
				dispatchCommand = SDL_AcquireGPUCommandBuffer(State->Device);
				if (dispatchCommand == nullptr) {
					ENGINE_ERROR(
						"'{}': SDL_AcquireGPUCommandBuffer: {}", context.Name.Text(), SDL_GetError()
					);
					return false;
				}

				// `Begin` put the query reset in the main command buffer, which is
				// submitted after this prefix. Use another slot whose reset and marks
				// travel together on the command buffer that reaches the queue first.
				const uint32_t laterReset = timingSlot;
				State->Timestamps.Abandon(laterReset);
				if (laterReset < VulkanTimestamps::SLOTS) {
					State->PendingMarks[laterReset].clear();
					State->TimingSequence[laterReset] = 0;
				}
				timingSlot = State->Timestamps.Begin(dispatchCommand, laterReset);
				if (State->BatchActive) {
					State->BatchTimingSlot = timingSlot;
				}
			}
			enterNamedPass(context.Name, dispatchCommand);
			SDL_GPUComputePass *pass = SDL_BeginGPUComputePass(
				dispatchCommand, writes.data(), static_cast<uint32_t>(writes.size()), nullptr, 0
			);
			if (pass == nullptr) {
				ENGINE_ERROR("'{}': SDL_BeginGPUComputePass: {}", context.Name.Text(), SDL_GetError());
				closePass();
				if (separateCommand) {
					State->Timestamps.Abandon(timingSlot);
					if (timingSlot < VulkanTimestamps::SLOTS) {
						State->PendingMarks[timingSlot].clear();
					}
					timingSlot = VulkanTimestamps::NO_SLOT;
					if (State->BatchActive) {
						State->BatchTimingSlot = timingSlot;
					}
					SDL_CancelGPUCommandBuffer(dispatchCommand);
				}
				return false;
			}
			SDL_BindGPUComputePipeline(pass, compute);
			if (!bindings.empty()) {
				SDL_BindGPUComputeSamplers(pass, 0, bindings.data(), static_cast<uint32_t>(bindings.size()));
			}
			const std::string *mode = node->Parameter(core::Name("dispatch.mode"));
			const bool coverTarget = mode == nullptr || *mode != "groups";
			const uint32_t groupsX = coverTarget ? (firstTarget.Width + localX - 1) / localX
												 : node->Integer(core::Name("dispatch.x"), 1);
			const uint32_t groupsY = coverTarget ? (firstTarget.Height + localY - 1) / localY
												 : node->Integer(core::Name("dispatch.y"), 1);
			const uint32_t groupsZ = coverTarget ? 1 : node->Integer(core::Name("dispatch.z"), 1);
			SDL_DispatchGPUCompute(pass, groupsX, groupsY, groupsZ);
			SDL_EndGPUComputePass(pass);
			result.ComputeDispatches++;
			if (separateCommand) {
				closePass();
				if (!SDL_SubmitGPUCommandBuffer(dispatchCommand)) {
					ENGINE_ERROR("'{}': SDL_SubmitGPUCommandBuffer: {}", context.Name.Text(), SDL_GetError());
					State->Timestamps.Abandon(timingSlot);
					if (timingSlot < VulkanTimestamps::SLOTS) {
						State->PendingMarks[timingSlot].clear();
					}
					return false;
				}
				dedicatedComputeSubmitted = true;
				result.AsyncComputeCommandBuffers++;
			} else {
				mainGpuWorkRecorded = true;
			}
			return true;
		});

		SDL_GPUSampler *const sampler = State->SurfaceSampler;
		const std::array depthBindings = {SDL_GPUTextureSamplerBinding{depthTarget.texture, sampler}};
		frameNodes.Set(core::Name("depth-linearise"), [&](const graph::RunContext &context) {
			fullscreen(
				context.Name,
				State->DepthLinearPipeline,
				pbr.LinearDepth,
				pbrDimensions.LinearWidth,
				pbrDimensions.LinearHeight,
				depthBindings,
				&uniforms,
				nullptr,
				SDL_FColor{drawCamera.FarPlane, 0.0f, 0.0f, 0.0f}
			);
			return true;
		});

		// The authored depth hierarchy: the same pyramid the occlusion cull
		// seeds mid-gbuffer, rebuilt here over the *finished* depth so
		// screen-space consumers walk a pyramid that saw every opaque draw.
		// When the cull also ran this frame, this is a rebuild rather than a
		// duplicate resource - the levels are reused, and the cull already
		// consumed the version it made.
		frameNodes.Set(core::Name("hzb"), [&](const graph::RunContext &context) {
			enterNamedPass(context.Name);
			if (State->Occlusion.Seed == nullptr || State->Occlusion.Reduce == nullptr) {
				// The compute shaders failed at startup; the log already
				// carries the reason, and a missing pyramid only disables what
				// reads it.
				return true;
			}
			if (!State->EnsurePyramid(sceneWidth, sceneHeight)) {
				return false;
			}
			State->BuildPyramid(command, depthTarget.texture);
			return true;
		});

		frameNodes.Set(core::Name("ssao"), [&](const graph::RunContext &context) {
			const std::array aoBindings = {
				SDL_GPUTextureSamplerBinding{pbr.LinearDepth, sampler},
				SDL_GPUTextureSamplerBinding{pbr.Normal, sampler},
			};
			fullscreen(
				context.Name,
				State->SsaoPipeline,
				pbr.Occlusion,
				pbrDimensions.OcclusionWidth,
				pbrDimensions.OcclusionHeight,
				aoBindings,
				&uniforms,
				nullptr,
				SDL_FColor{1.0f, 1.0f, 1.0f, 1.0f}
			);
			return true;
		});

		const auto clearOcclusion = [&] {
			SDL_GPUColorTargetInfo clearAo{};
			clearAo.texture = pbr.Occlusion;
			clearAo.clear_color = SDL_FColor{1.0f, 1.0f, 1.0f, 1.0f};
			clearAo.load_op = SDL_GPU_LOADOP_CLEAR;
			clearAo.store_op = SDL_GPU_STOREOP_STORE;
			clearAo.cycle = true;
			SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(command, &clearAo, 1, nullptr);
			SDL_EndGPURenderPass(pass);
		};

		const std::array lightingBindings = {
			SDL_GPUTextureSamplerBinding{pbr.Albedo, sampler},
			SDL_GPUTextureSamplerBinding{pbr.Normal, sampler},
			SDL_GPUTextureSamplerBinding{pbr.Material, sampler},
			SDL_GPUTextureSamplerBinding{pbr.Emissive, sampler},
			SDL_GPUTextureSamplerBinding{pbr.LinearDepth, sampler},
			SDL_GPUTextureSamplerBinding{pbr.Occlusion, sampler},
			SDL_GPUTextureSamplerBinding{
				State->ShadowTexture != nullptr ? State->ShadowTexture : State->FallbackTexture,
				State->ShadowSampler != nullptr ? State->ShadowSampler : sampler,
			},
		};
		frameNodes.Set(core::Name("deferred-lighting"), [&](const graph::RunContext &context) {
			if (!graphEnabled(core::Name("ssao"))) {
				clearOcclusion();
			}

			// The seam light projectors, chosen and bound here rather than with
			// `lightingBindings`, because the capture textures are made inside
			// the portal-capture node this same frame - the graph's
			// portal-light edge is what guarantees that node has already run.
			// The nearest ready mouths win the two slots; empty slots stay
			// zeroed in `uniforms` and bind the fallback texel.
			std::array<SDL_GPUTextureSamplerBinding, lightingBindings.size() + MAX_SEAM_LIGHTS>
				spillBindings{};
			std::copy(lightingBindings.begin(), lightingBindings.end(), spillBindings.begin());

			std::array<const Impl::SeamLightTarget *, scene::MAX_SURFACES> ready{};
			size_t readyCount = 0;
			if (graphEnabled(core::Name("portal-capture"))) {
				for (const Impl::SeamLightTarget &seamLight : bank.SeamLights) {
					if (seamLight.Ready) {
						ready[readyCount++] = &seamLight;
					}
				}
			}
			const auto distanceTo = [&](const Impl::SeamLightTarget *candidate) {
				const core::Vector3 offset{
					candidate->Centre.x - cameraFrame.Position.X,
					candidate->Centre.y - cameraFrame.Position.Y,
					candidate->Centre.z - cameraFrame.Position.Z,
				};
				return offset.Dot(offset);
			};
			std::sort(
				ready.begin(),
				ready.begin() + static_cast<std::ptrdiff_t>(readyCount),
				[&](const Impl::SeamLightTarget *left, const Impl::SeamLightTarget *right) {
					return distanceTo(left) < distanceTo(right);
				}
			);

			std::array<const Impl::SeamLightTarget *, MAX_SEAM_LIGHTS> chosen{};
			for (size_t slot = 0; slot < chosen.size() && slot < readyCount; slot++) {
				chosen[slot] = ready[slot];
			}

			for (size_t slot = 0; slot < chosen.size(); slot++) {
				SDL_GPUTextureSamplerBinding &binding = spillBindings[lightingBindings.size() + slot];
				if (chosen[slot] != nullptr) {
					uniforms.SeamCentre[slot] = chosen[slot]->Centre;
					uniforms.SeamOutward[slot] = chosen[slot]->Outward;
					uniforms.SeamFirst[slot] = chosen[slot]->First;
					uniforms.SeamSecond[slot] = chosen[slot]->Second;
					binding = SDL_GPUTextureSamplerBinding{chosen[slot]->Colour, sampler};
				} else {
					uniforms.SeamCentre[slot] = glm::vec4{};
					uniforms.SeamOutward[slot] = glm::vec4{};
					uniforms.SeamFirst[slot] = glm::vec4{};
					uniforms.SeamSecond[slot] = glm::vec4{};
					binding = SDL_GPUTextureSamplerBinding{State->FallbackTexture, sampler};
				}
			}

			fullscreen(
				context.Name,
				State->DeferredLightingPipeline,
				pbr.Lit,
				pbrDimensions.LitWidth,
				pbrDimensions.LitHeight,
				spillBindings,
				&uniforms,
				&lightUniforms,
				SDL_FColor{State->FogColour.r, State->FogColour.g, State->FogColour.b, 1.0f}
			);
			return true;
		});

		const std::array tonemapBindings = {SDL_GPUTextureSamplerBinding{pbr.Lit, sampler}};
		frameNodes.Set(core::Name("portal-tonemap"), [&](const graph::RunContext &context) {
			enterNamedPass(context.Name);
			if (!havePortals || portalLevels == 0 || bank.Portals.size() < portalLevels) {
				return true;
			}

			Impl::PortalLevel &top = bank.Portals[portalLevels - 1];
			for (uint8_t index = 0; index < scene::MAX_SURFACES; index++) {
				Impl::PortalTarget &portal = top.Targets[index];
				if (portalOf[index] == nullptr || portal.Colour == nullptr || portal.Display == nullptr) {
					continue;
				}
				const std::array bindings = {
					SDL_GPUTextureSamplerBinding{portal.Colour, State->SurfaceSampler}
				};
				fullscreen(
					context.Name,
					State->TonemapPipeline,
					portal.Display,
					portal.Width,
					portal.Height,
					bindings,
					nullptr,
					nullptr,
					colourTarget.clear_color
				);
			}
			return true;
		});
		frameNodes.Set(core::Name("tonemap"), [&](const graph::RunContext &context) {
			Impl::NamedTexture target;
			for (const graph::ResourceId resource : context.Writes) {
				target = graphTexture(resource, context, true);
				if (target.IsValid()) {
					break;
				}
			}
			// **The one place `PostProcessPipeline` is read.** The portal
			// preview above always draws with the engine's own tonemap - see
			// `Renderer::SetPostProcessShader`'s own header for why a custom
			// grade on the main view must not also recolour every mirror and
			// portal in it.
			fullscreen(
				context.Name,
				State->PostProcessPipeline != nullptr ? State->PostProcessPipeline : State->TonemapPipeline,
				target.Texture,
				target.Width,
				target.Height,
				tonemapBindings,
				nullptr,
				nullptr,
				colourTarget.clear_color
			);

			// The forward tail consumes both completed attachments.
			colourTarget.load_op = SDL_GPU_LOADOP_LOAD;
			depthTarget.load_op = SDL_GPU_LOADOP_LOAD;
			// An offscreen slot exposes this attachment through `ResourceTexture`
			// after the graph finishes, including the profiler's stage thumbnails.
			// Discarding it here returned a valid texture handle holding undefined
			// pixels. A window render has no later depth consumer and may still skip
			// the final store.
			depthTarget.store_op = offscreen ? SDL_GPU_STOREOP_STORE : SDL_GPU_STOREOP_DONT_CARE;
			// Cycling selects fresh backing storage, which cannot contain the depth
			// this pass explicitly loads from the G-buffer pass.
			depthTarget.cycle = false;
			return true;
		});

		frameNodes.Set(core::Name("portal-overlay"), [&](const graph::RunContext &context) {
			enterNamedPass(context.Name);
			if (!submitUploads()) {
				return false;
			}

			Impl::NamedTexture source;
			Impl::NamedTexture target;
			if (!context.Reads.empty()) {
				source = graphTexture(context.Reads.front(), context, false);
			}
			for (const graph::ResourceId resource : context.Writes) {
				target = graphTexture(resource, context, true);
				if (target.IsValid()) {
					break;
				}
			}
			if (!drawImage(source, target, SDL_GPU_LOADOP_CLEAR)) {
				ENGINE_WARN("'{}' needs a scene image and an output image", context.Name.Text());
				return true;
			}

			SDL_GPUColorTargetInfo portalTarget{};
			portalTarget.texture = target.Texture;
			portalTarget.load_op = SDL_GPU_LOADOP_LOAD;
			portalTarget.store_op = SDL_GPU_STOREOP_STORE;
			portalTarget.cycle = false;
			depthTarget.load_op = SDL_GPU_LOADOP_LOAD;
			depthTarget.store_op = SDL_GPU_STOREOP_STORE;
			depthTarget.cycle = false;
			SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(command, &portalTarget, 1, &depthTarget);
			SDL_PushGPUFragmentUniformData(command, 1, &lightUniforms, sizeof(lightUniforms));
			SDL_PushGPUFragmentUniformData(command, 2, &State->Beams, sizeof(State->Beams));
			SDL_SetGPUViewport(pass, &sceneViewport);
			SDL_SetGPUScissor(pass, &sceneScissor);

			if (haveInstances) {
				State->BindPipeline(pass, State->OpaquePipeline, Impl::PipelineFamily::Opaque);
				const SDL_GPUBufferBinding vertexBindings[] = {
					{State->Meshes.Vertices(), 0},
					{State->InstanceBuffer, 0},
				};
				SDL_BindGPUVertexBuffers(pass, 0, vertexBindings, 2);
				const SDL_GPUBufferBinding indexBinding{State->Meshes.Indices(), 0};
				SDL_BindGPUIndexBuffer(pass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);
				SDL_PushGPUVertexUniformData(command, 0, &frameUniforms, sizeof(frameUniforms));

				const LightingUniforms portalLighting = lightingAt(cameraFrame.Position, 0.0f, 0.0f);
				const ShadowBinding shadow = shadowBinding();
				const auto drawPortals = [&](bool blended) {
					if (blended) {
						State->BindPipeline(
							pass, State->TransparentPipeline, Impl::PipelineFamily::Transparent
						);
					}
					for (uint8_t index = 0; index < scene::MAX_SURFACES; index++) {
						if (portalOf[index] == nullptr) {
							continue;
						}
						const scene::SurfaceRun &run = cameraRuns[index];
						const uint32_t count = blended ? run.BlendedCount : run.OpaqueCount;
						const uint32_t first = blended ? run.BlendedFirst : run.OpaqueFirst;
						if (count == 0) {
							continue;
						}

						const Impl::PortalTarget *captured =
							graphEnabled(core::Name("portal-capture")) && portalLevels > 0 &&
									bank.Portals.size() >= portalLevels
								? &bank.Portals[portalLevels - 1].Targets[index]
								: nullptr;
						LightingUniforms paneLighting = portalLighting;
						SDL_GPUTexture *image = nullptr;
						if (captured != nullptr && captured->Display != nullptr) {
							paneLighting.Flags.z = 2.0f;
							paneLighting.Flags.w = 1.0f;
							paneLighting.PaneNormal = glm::vec4{
								portalOf[index]->Normal.X,
								portalOf[index]->Normal.Y,
								portalOf[index]->Normal.Z,
								0.0f,
							};
							image = captured->Display;
							result.SurfaceInstances += count;
						}

						result.DrawCalls += State->DrawSlots(
							command,
							pass,
							sceneCount + first,
							count,
							&paneLighting,
							shadow.Texture,
							shadow.Sampler,
							image,
							State->SurfaceSampler,
							0,
							result.Triangles
						);
					}
				};

				drawPortals(false);
				drawPortals(true);
			}
			SDL_EndGPURenderPass(pass);
			return true;
		});

		frameNodes.Set(core::Name("mirror-overlay"), [&](const graph::RunContext &context) {
			ENGINE_PROFILE_CAT("mirror overlay pass", core::ProfileCategory::Render);
			if (!submitUploads()) {
				return false;
			}
			enterNamedPass(context.Name);

			Impl::NamedTexture source;
			Impl::NamedTexture target;
			if (!context.Reads.empty()) {
				source = graphTexture(context.Reads.front(), context, false);
			}
			for (const graph::ResourceId resource : context.Writes) {
				target = graphTexture(resource, context, true);
				if (target.IsValid()) {
					break;
				}
			}
			if (!drawImage(source, target, SDL_GPU_LOADOP_CLEAR)) {
				ENGINE_WARN("'{}' needs a scene image and an output image", context.Name.Text());
				return true;
			}

			colourTarget.texture = target.Texture;
			colourTarget.load_op = SDL_GPU_LOADOP_LOAD;
			colourTarget.store_op = SDL_GPU_STOREOP_STORE;
			colourTarget.cycle = false;
			depthTarget.load_op = SDL_GPU_LOADOP_LOAD;
			depthTarget.store_op = SDL_GPU_STOREOP_STORE;
			depthTarget.cycle = false;

			SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(command, &colourTarget, 1, &depthTarget);
			SDL_PushGPUFragmentUniformData(command, 1, &lightUniforms, sizeof(lightUniforms));
			SDL_PushGPUFragmentUniformData(command, 2, &State->Beams, sizeof(State->Beams));
			SDL_SetGPUViewport(pass, &sceneViewport);
			SDL_SetGPUScissor(pass, &sceneScissor);

			if (haveInstances && (surfaceInCamera > 0 || transparentSurfaces > 0)) {
				State->BindPipeline(pass, State->OpaquePipeline, Impl::PipelineFamily::Opaque);
				const SDL_GPUBufferBinding vertexBindings[] = {
					{State->Meshes.Vertices(), 0},
					{State->InstanceBuffer, 0},
				};
				SDL_BindGPUVertexBuffers(pass, 0, vertexBindings, 2);
				const SDL_GPUBufferBinding indexBinding{State->Meshes.Indices(), 0};
				SDL_BindGPUIndexBuffer(pass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);

				const float aspect = static_cast<float>(sceneWidth) / static_cast<float>(sceneHeight);
				const glm::mat4 viewProjection =
					scene::ResolveCamera(cameraFrame, drawCamera, aspect).ViewProjection;
				const FrameUniforms frameUniforms{
					viewProjection,
					lightViewProjection,
					glm::mat4{1.0f},
				};
				SDL_PushGPUVertexUniformData(command, 0, &frameUniforms, sizeof(frameUniforms));

				const LightingUniforms plainLighting = lightingAt(cameraFrame.Position, 0.0f, 0.0f);
				const ShadowBinding shadow = shadowBinding();
				const bool surfaceImagesEnabled = graphEnabled(core::Name("mirror-capture"));
				LightingUniforms mirroredUniforms{};

				const auto drawMirrors = [&](bool blended) {
					for (uint8_t index = 0; index < scene::MAX_SURFACES; index++) {
						if (portalOf[index] != nullptr) {
							continue;
						}
						const scene::SurfaceRun &run = cameraRuns[index];
						const uint32_t count = blended ? run.BlendedCount : run.OpaqueCount;
						const uint32_t first = blended ? run.BlendedFirst : run.OpaqueFirst;
						if (count == 0) {
							continue;
						}

						const Impl::SurfaceSlotState &shown = bank.Surfaces[index];
						const LightingUniforms *paneLighting = &plainLighting;
						SDL_GPUTexture *image = nullptr;
						if (surfaceImagesEnabled && shown.Ready) {
							const FrameUniforms mirrorFrame{
								viewProjection,
								lightViewProjection,
								shown.Sampling,
							};
							mirroredUniforms = lightingAt(cameraFrame.Position, 1.0f, shown.ImageOpacity);
							mirroredUniforms.Mirror.x = static_cast<float>(shown.Effect);
							SDL_PushGPUVertexUniformData(command, 0, &mirrorFrame, sizeof(mirrorFrame));
							paneLighting = &mirroredUniforms;
							image = shown.Texture[shown.Slot];
							result.SurfaceInstances += count;
						}

						result.DrawCalls += State->DrawSlots(
							command,
							pass,
							sceneCount + first,
							count,
							paneLighting,
							shadow.Texture,
							shadow.Sampler,
							image,
							State->SurfaceSampler,
							0,
							result.Triangles
						);
						SDL_PushGPUVertexUniformData(command, 0, &frameUniforms, sizeof(frameUniforms));
					}
				};

				if (surfaceInCamera > 0) {
					drawMirrors(false);
				}
				if (transparentSurfaces > 0) {
					State->BindPipeline(pass, State->TransparentPipeline, Impl::PipelineFamily::Transparent);
					drawMirrors(true);
				}
			}

			SDL_EndGPURenderPass(pass);
			return true;
		});

		frameNodes.Set(core::Name("transparent"), [&](const graph::RunContext &context) {
			ENGINE_PROFILE_CAT("transparent pass", core::ProfileCategory::Render);
			if (!submitUploads()) {
				return false;
			}

			// Entered unconditionally, and that is the honest reading rather
			// than a convenience: the stage clears colour and depth, so a frame
			// with nothing in it still ran this pass - the background is what it
			// drew. `Validate` sees the same thing, because the stage's writes
			// are marked `Clear`.
			enterNamedPass(context.Name);

			Impl::NamedTexture source;
			Impl::NamedTexture target;
			if (!context.Reads.empty()) {
				source = graphTexture(context.Reads.front(), context, false);
			}
			for (const graph::ResourceId resource : context.Writes) {
				target = graphTexture(resource, context, true);
				if (target.IsValid()) {
					break;
				}
			}
			if (!drawImage(source, target, SDL_GPU_LOADOP_CLEAR)) {
				ENGINE_WARN("'{}' needs a scene image and an output image", context.Name.Text());
				return true;
			}
			colourTarget.texture = target.Texture;
			colourTarget.load_op = SDL_GPU_LOADOP_LOAD;
			colourTarget.store_op = SDL_GPU_STOREOP_STORE;
			colourTarget.cycle = false;

			SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(command, &colourTarget, 1, &depthTarget);
			// **The light set, pushed once for the whole pass.** Uniform state
			// on a command buffer persists until it is replaced, so one push
			// before the draws serves every one of them - which is the whole
			// reason this is a second buffer rather than fields on the
			// per-draw `LightingUniforms`.
			SDL_PushGPUFragmentUniformData(command, 1, &lightUniforms, sizeof(lightUniforms));

			// **The beams, beside the lights and for the same reason.**
			// Which holes carry a shadow is a fact about the frame, so it
			// is pushed once per pass rather than per draw - and it is
			// pushed even when there are none, because a stale block from
			// a previous frame would shadow through a hole that is no
			// longer there.
			SDL_PushGPUFragmentUniformData(command, 2, &State->Beams, sizeof(State->Beams));

			// **The world's rectangle inside an attachment that is larger than
			// it.** Without this the pass inherits a viewport covering the whole
			// texture, and a block-rounded target would draw the world into
			// 1600x960 while the panel shows the 1600x900 corner - the image
			// squashed by the rounding. Set once here and inherited by the
			// transparent draws in the same pass. See `SCENE_TARGET_BLOCK`.
			//
			// Correct on the window path too, where the two sizes are equal and
			// this restates the default rather than changing it.
			const SDL_GPUViewport view{
				0.0f, 0.0f, static_cast<float>(sceneWidth), static_cast<float>(sceneHeight), 0.0f, 1.0f
			};
			SDL_SetGPUViewport(pass, &view);

			// The scissor goes with it. A viewport shrinks what is drawn but
			// does not clip what a pipeline with no depth test could still
			// scribble outside it, and the border is memory nothing owns.
			const SDL_Rect scissor{0, 0, static_cast<int>(sceneWidth), static_cast<int>(sceneHeight)};
			SDL_SetGPUScissor(pass, &scissor);

			if (haveInstances) {
				State->BindPipeline(pass, State->OpaquePipeline, Impl::PipelineFamily::Opaque);

				const SDL_GPUBufferBinding vertexBindings[] = {
					{State->Meshes.Vertices(), 0},
					{State->InstanceBuffer, 0},
				};
				SDL_BindGPUVertexBuffers(pass, 0, vertexBindings, 2);

				const SDL_GPUBufferBinding indexBinding{State->Meshes.Indices(), 0};
				SDL_BindGPUIndexBuffer(pass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);

				const float aspect = static_cast<float>(sceneWidth) / static_cast<float>(sceneHeight);

				// `scene::ResolveCamera`, not a projection built here. It is the
				// one place the engine decides what a camera's matrices are, and
				// a second copy is a second chance to disagree about handedness,
				// clip depth or the order of the product - a disagreement that
				// reads as z-fighting rather than as a matrix mistake.
				//
				// The Y convention that used to need a comment here lives there
				// too: no flip, because SDL's Vulkan backend already submits a
				// negative-height viewport "for consistency with other
				// backends".
				//
				// The aspect ratio is the rectangle the world is drawn into
				// rather than anything a caller computed, so a frame taken
				// mid-resize is projected for the image it actually lands in.
				// Without a `Viewport` that rectangle is the swapchain, which is
				// what every non-editor caller gets and what this used to say.
				// **Identity for the surface projection, because this draw is
				// not a mirror's.** Every draw that samples a surface pushes its
				// own matrix below, one per index; leaving a live one here would
				// give the plain geometry a projection it must never use, which
				// is the shape of the black-wedge bug the surface pass records.
				const glm::mat4 viewProjection =
					scene::ResolveCamera(cameraFrame, drawCamera, aspect).ViewProjection;

				const FrameUniforms frameUniforms{
					viewProjection,
					lightViewProjection,
					glm::mat4{1.0f},
				};
				SDL_PushGPUVertexUniformData(command, 0, &frameUniforms, sizeof(frameUniforms));

				// **The surface flag is off for the opaque range and on for a
				// second draw over the instances that carry one.** Whether an
				// instance samples the surface is per instance and the uniform
				// is per draw, so the split is a third draw rather than a
				// per-fragment branch on data the shader does not have.
				const LightingUniforms lighting = lightingAt(cameraFrame.Position, 0.0f, 0.0f);
				SDL_PushGPUFragmentUniformData(command, 0, &lighting, sizeof(lighting));

				// Both samplers, every draw. A shadow map that was not rendered
				// binds another texture in its place rather than nothing: the
				// flag above is what stops it being read, and an unbound sampler
				// is undefined behaviour on several backends where a wrongly
				// bound one is merely ignored.
				//
				// **`FallbackTexture` rather than `OverlayTexture`**, which only
				// exists while a debug panel has something in it. A scene of
				// nothing but transparent geometry casts nothing, so the shadow
				// map is absent too - and with the panels closed both were null
				// and the guard below skipped the bind and drew anyway. See
				// `Impl::FallbackTexture`.
				SDL_GPUTexture *const shadow =
					State->ShadowTexture != nullptr ? State->ShadowTexture : State->FallbackTexture;
				SDL_GPUSampler *const shadowSampler =
					State->ShadowSampler != nullptr ? State->ShadowSampler : State->OverlaySampler;
				SDL_GPUSampler *const surfaceSampler =
					State->SurfaceSampler != nullptr ? State->SurfaceSampler : shadowSampler;

				if (drawInterface) {
					result.DrawCalls += gameInterfaceHook->RecordWorld(
						command,
						pass,
						viewProjection,
						cameraFrame,
						core::Color3{State->Ambient.x, State->Ambient.y, State->Ambient.z},
						core::Vector3{State->Sun.x, State->Sun.y, State->Sun.z},
						sceneWidth,
						sceneHeight,
						false
					);
				}

				if (transparentCount > 0) {
					// Same pass, same depth attachment, different pipeline -
					// blending on and depth writes off. A separate render pass
					// would have to reload the depth buffer, and the whole point
					// is that these fragments are tested against what the opaque
					// pass already wrote.
					//
					// Still its own stage, sharing a render pass. What the list
					// describes is what is drawn and in what order, not how many
					// times a target is bound.
					State->BindPipeline(pass, State->TransparentPipeline, Impl::PipelineFamily::Transparent);

					if (plainTransparent > 0) {
						result.DrawCalls += State->DrawSlots(
							command,
							pass,
							sceneCount + static_cast<uint32_t>(opaqueCount),
							plainTransparent,
							&lighting,
							shadow,
							shadowSampler,
							nullptr,
							surfaceSampler,
							0,
							result.Triangles
						);
					}
				}

				// --- particles ---------------------------------------------
				//
				// **After every blended run and inside the same pass**, which is
				// the arrangement the header states: a particle is depth-tested
				// against the world and drawn over the glass. Sorting half a
				// million particles into the geometry's own order would cost more
				// than the artefact of not doing it.
				//
				// **Not their own node**, deliberately. Particles share the ordered
				// transparent target and depth state, so the graph's `transparent`
				// node owns them along with blended geometry. Splitting that node
				// would require a resource edge and an independently executable
				// backend operation, not another fixed pass label in this body.
				if (particleCount > 0) {
					result.DrawCalls += State->DrawParticles(
						command, pass, frameUniforms.ViewProjection, cameraFrame, particles, result.Triangles
					);
				}

				// The beams and trails, after the particles. See the header for
				// why the order is fixed rather than sorted.
				if (ribbonCount > 0) {
					result.DrawCalls += State->DrawRibbons(
						command, pass, frameUniforms.ViewProjection, cameraFrame, ribbonRuns, result.Triangles
					);
				}

				if (drawInterface) {
					result.DrawCalls += gameInterfaceHook->RecordWorld(
						command,
						pass,
						viewProjection,
						cameraFrame,
						core::Color3{State->Ambient.x, State->Ambient.y, State->Ambient.z},
						core::Vector3{State->Sun.x, State->Sun.y, State->Sun.z},
						sceneWidth,
						sceneHeight,
						true
					);
				}

				// **Counted as it is drawn rather than derived from the instance
				// count.** While everything was a cube, triangles were thirty-six
				// indices times however many instances; with a mesh per instance
				// there is no such multiplier, and the honest number is the one
				// `DrawSlots` accumulated. `instanceCount` is still what the
				// instance counter reports.
				(void)instanceCount;
			}

			SDL_EndGPURenderPass(pass);
			return true;
		});
		Impl::NamedTexture authoredCapture;
		std::filesystem::path authoredCapturePath;
		core::Name authoredCaptureNode;
		bool authoredCaptureOnce = false;
		frameNodes.Set(core::Name("viewer"), [&](const graph::RunContext &context) {
			enterNamedPass(context.Name);
			for (const graph::ResourceId resource : context.Reads) {
				const graph::ResourceDesc *desc = selectedPipeline->Graph.FindResource(resource);
				const Impl::NamedTexture source = graphTexture(resource, context, false);
				if (desc == nullptr || !source.IsValid()) {
					continue;
				}
				const graph::Node *node = selectedPipeline->Graph.Find(context.Node);
				const size_t slot = node != nullptr ? node->Integer(core::Name("view"), 0) : targetSlot;
				// Onto the later-transfer buffer, per the traffic plan: this node
				// reads finished images, and the main buffer is submitted first.
				(void)State->RequestPreview(
					State->DownloadBuffer(),
					source.Texture,
					source.Width,
					source.Height,
					desc->Name,
					slot,
					source.Format
				);
				break;
			}
			return true;
		});
		frameNodes.Set(core::Name("capture"), [&](const graph::RunContext &context) {
			enterNamedPass(context.Name);
			const graph::Node *node = selectedPipeline->Graph.Find(context.Node);
			const std::string *path = node != nullptr ? node->Parameter(core::Name("path")) : nullptr;
			if (node == nullptr || path == nullptr || path->empty() || authoredCapture.IsValid()) {
				return true;
			}
			const std::string *mode = node->Parameter(core::Name("capture.mode"));
			authoredCaptureOnce = mode == nullptr || *mode != "every-frame";
			if (authoredCaptureOnce) {
				const auto done = std::find_if(
					State->GraphCaptureReceipts.begin(),
					State->GraphCaptureReceipts.end(),
					[&](const Impl::GraphCaptureReceipt &receipt) {
						// A file path is process-wide even when the same authored
						// pipeline is installed once per world. Claiming by path keeps
						// those instances from overwriting one another in the same frame.
						return receipt.Node == context.Name && receipt.Path == *path;
					}
				);
				if (done != State->GraphCaptureReceipts.end()) {
					return true;
				}
			} else {
				const auto claimed = State->GraphCaptureFrames.find(*path);
				if (claimed != State->GraphCaptureFrames.end() && claimed->second == State->FrameCounter) {
					return true;
				}
				State->GraphCaptureFrames[*path] = State->FrameCounter;
			}
			for (const graph::ResourceId resource : context.Reads) {
				const Impl::NamedTexture source = graphTexture(resource, context, false);
				if (!source.IsValid()) {
					continue;
				}
				if (source.Format != SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM &&
					source.Format != SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB &&
					source.Format != SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM &&
					source.Format != SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB) {
					ENGINE_WARN("capture node '{}' needs a four-byte display target", context.Name.Text());
					return true;
				}
				authoredCapture = source;
				authoredCapturePath = *path;
				authoredCaptureNode = context.Name;
				if (authoredCaptureOnce) {
					State->GraphCaptureReceipts.push_back(
						{selectedPipeline->Name, authoredCaptureNode, authoredCapturePath.string()}
					);
				}
				break;
			}
			return true;
		});
		frameNodes.Set(core::Name("present"), [&](const graph::RunContext &context) {
			enterNamedPass(context.Name);
			Impl::NamedTexture source;
			Impl::NamedTexture target;
			for (const graph::ResourceId resource : context.Reads) {
				source = graphTexture(resource, context, false);
				if (source.IsValid()) {
					break;
				}
			}
			for (const graph::ResourceId resource : context.Writes) {
				target = graphTexture(resource, context, true);
				if (target.IsValid()) {
					break;
				}
			}
			if (!drawImage(source, target, SDL_GPU_LOADOP_CLEAR)) {
				ENGINE_WARN("'{}' needs one readable image and one writable image", context.Name.Text());
			}
			return true;
		});

		// --- interface image ------------------------------------------------

		frameNodes.Set(core::Name("interface"), [&](const graph::RunContext &context) {
			enterNamedPass(context.Name);
			Impl::NamedTexture target;
			for (const graph::ResourceId resource : context.Writes) {
				target = graphTexture(resource, context, true);
				if (target.IsValid()) {
					break;
				}
			}
			if (!target.IsValid()) {
				ENGINE_WARN("'{}' has no image target", context.Name.Text());
				return true;
			}

			SDL_GPUColorTargetInfo interfaceTarget{};
			interfaceTarget.texture = target.Texture;
			interfaceTarget.clear_color = SDL_FColor{0.0f, 0.0f, 0.0f, 0.0f};
			interfaceTarget.load_op = SDL_GPU_LOADOP_CLEAR;
			interfaceTarget.store_op = SDL_GPU_STOREOP_STORE;
			interfaceTarget.cycle = true;
			SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(command, &interfaceTarget, 1, nullptr);
			if (!drawInterface) {
				SDL_EndGPURenderPass(pass);
				return true;
			}
			ENGINE_PROFILE_CAT("interface pass", core::ProfileCategory::Render);
			gameInterfaceHook->Record(command, pass);
			SDL_EndGPURenderPass(pass);
			result.DrawCalls++;
			return true;
		});

		// --- image composition ----------------------------------------------

		frameNodes.Set(core::Name("overlay"), [&](const graph::RunContext &context) {
			enterNamedPass(context.Name);
			Impl::NamedTexture sceneImage;
			Impl::NamedTexture interfaceImage;
			Impl::NamedTexture target;
			if (!context.Reads.empty()) {
				sceneImage = graphTexture(context.Reads[0], context, false);
			}
			if (context.Reads.size() > 1) {
				interfaceImage = graphTexture(context.Reads[1], context, false);
			}
			for (const graph::ResourceId resource : context.Writes) {
				target = graphTexture(resource, context, true);
				if (target.IsValid()) {
					break;
				}
			}
			if (!drawImage(sceneImage, target, SDL_GPU_LOADOP_CLEAR)) {
				ENGINE_WARN("'{}' has no scene image to compose", context.Name.Text());
				return true;
			}

			if (haveOverlay) {
				if (!submitUploads()) {
					return false;
				}
				ENGINE_PROFILE_CAT("debug image overlay", core::ProfileCategory::Render);
				drawOverlayImage(State->OverlayTexture, target, SDL_GPU_LOADOP_LOAD);
			}
			if (graphEnabled(core::Name("interface")) && interfaceImage.IsValid()) {
				drawOverlayImage(interfaceImage.Texture, target, SDL_GPU_LOADOP_LOAD);
			}
			return true;
		});

		frameNodes.Set(core::Name("output-image"), [&](const graph::RunContext &context) {
			enterNamedPass(context.Name);
			for (Impl::ResourcePreviewTarget &preview : State->ResourcePreviews) {
				if (!preview.Refresh || preview.Route.Pipeline != selectedPipeline->Name ||
					preview.Route.Slot != targetSlot) {
					continue;
				}
				graph::ResourceId resource;
				for (uint32_t value = 1; value <= selectedPipeline->Graph.ResourceCount(); value++) {
					const graph::ResourceId candidate{value};
					const graph::ResourceDesc *desc = selectedPipeline->Graph.FindResource(candidate);
					if (desc != nullptr && desc->Name == preview.Route.Resource) {
						resource = candidate;
						break;
					}
				}
				const Impl::NamedTexture source = resource.IsValid()
													  ? resourceTexture(resource, preview.Route.Slot, false)
													  : Impl::NamedTexture{};
				if (!source.IsValid()) {
					continue;
				}
				constexpr uint32_t PREVIEW_SIDE = 256;
				uint32_t previewWidth = source.Width;
				uint32_t previewHeight = source.Height;
				if (std::max(previewWidth, previewHeight) > PREVIEW_SIDE) {
					if (previewWidth >= previewHeight) {
						previewHeight = std::max(1u, previewHeight * PREVIEW_SIDE / previewWidth);
						previewWidth = PREVIEW_SIDE;
					} else {
						previewWidth = std::max(1u, previewWidth * PREVIEW_SIDE / previewHeight);
						previewHeight = PREVIEW_SIDE;
					}
				}
				if (preview.Width != previewWidth || preview.Height != previewHeight) {
					// The interface draw list was recorded before this graph node and
					// can still name the visible image. Retire both at the next frame
					// boundary instead of releasing either under that draw list.
					for (SDL_GPUTexture *&texture : preview.Textures) {
						if (texture != nullptr) {
							State->RetiredScenes.push_back(texture);
							texture = nullptr;
						}
					}
					preview.Slots.Reset();
					preview.Width = 0;
					preview.Height = 0;
				}

				const uint8_t writeSlot = preview.Slots.Writable();
				SDL_GPUTexture *&writeTexture = preview.Textures[writeSlot];
				if (writeTexture == nullptr) {
					SDL_GPUTextureCreateInfo info{};
					info.type = SDL_GPU_TEXTURETYPE_2D;
					info.format = State->ColourFormat();
					info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
					info.width = previewWidth;
					info.height = previewHeight;
					info.layer_count_or_depth = 1;
					info.num_levels = 1;
					info.sample_count = SDL_GPU_SAMPLECOUNT_1;
					writeTexture = SDL_CreateGPUTexture(State->Device, &info);
					preview.Width = writeTexture != nullptr ? previewWidth : 0;
					preview.Height = writeTexture != nullptr ? previewHeight : 0;
				}
				const Impl::NamedTexture target{
					writeTexture, preview.Width, preview.Height, State->ColourFormat()
				};
				if (drawImage(source, target, SDL_GPU_LOADOP_CLEAR, preview.ReverseSpectrum)) {
					preview.Slots.Publish(writeSlot);
					preview.Refresh = false;
				}
			}
			if (swapchain == nullptr && !offscreen) {
				return true;
			}
			Impl::NamedTexture source;
			for (const graph::ResourceId resource : context.Reads) {
				source = graphTexture(resource, context, false);
				if (source.IsValid()) {
					break;
				}
			}
			const Impl::NamedTexture target =
				offscreen ? Impl::NamedTexture{viewTarget, sceneWidth, sceneHeight, State->ColourFormat()}
						  : Impl::NamedTexture{swapchain, width, height, State->ColourFormat()};
			if (!drawImage(source, target, SDL_GPU_LOADOP_CLEAR)) {
				ENGINE_WARN("'{}' has no image wired into it", context.Name.Text());
				return true;
			}
			if (State->EnsureHistory(targetSlot, sceneWidth, sceneHeight)) {
				Impl::SceneSlot &history = State->SlotAt(targetSlot);
				const Impl::NamedTexture historyTarget{
					history.History,
					history.HistoryWidth,
					history.HistoryHeight,
					State->ColourFormat(),
				};
				if (source.Texture == historyTarget.Texture ||
					drawImage(source, historyTarget, SDL_GPU_LOADOP_CLEAR)) {
					history.HistoryReady = true;
				}
			}
			windowTarget.load_op = SDL_GPU_LOADOP_LOAD;
			return true;
		});

		GraphRunner frameRunner(frameNodes);
		bool dispatched = false;
		if (State->BatchActive) {
			dispatched = selectedPipeline->Graph.ExecuteView(
				selectedPipeline->Compiled,
				frameRunner,
				State->BatchViewIndex,
				State->BatchWorldIndex,
				State->BatchShared
			);
			if (dispatched && State->BatchFinal) {
				dispatched = selectedPipeline->Graph.ExecuteFinal(selectedPipeline->Compiled, frameRunner);
			}
		} else {
			const uint64_t worlds[] = {world};
			dispatched = selectedPipeline->Graph.Execute(selectedPipeline->Compiled, frameRunner, worlds);
		}
		if (!dispatched) {
			ENGINE_ERROR(
				"render graph '{}' refused while executing '{}'",
				selectedPipeline->Name.Text(),
				frameRunner.Unhandled().Text()
			);
			closePass();
			State->BatchFailed = State->BatchActive;
			endIncompleteView();
			return result;
		}

		closePass();

		// Byte and two-byte masks are expanded to RGBA by the readback path. Float
		// targets remain directly inspectable on the GPU without pretending their
		// bytes are display colour.
		if (offscreen && State->Inspected.IsValid() &&
			(State->InspectedSlot == Renderer::ANY_VIEWPORT || State->InspectedSlot == targetSlot)) {
			Impl::NamedTexture inspected = fixedTexture(State->Inspected, targetSlot);
			if (!inspected.IsValid()) {
				for (const Impl::GraphTarget &target : State->GraphTargets) {
					if (target.Pipeline == selectedPipeline->Name && target.Resource == State->Inspected &&
						target.Texture != nullptr &&
						(target.Scope != graph::NodeScope::View || target.Owner == targetSlot)) {
						inspected = {target.Texture, target.Width, target.Height, target.Format};
						break;
					}
				}
			}
			if (inspected.IsValid()) {
				(void)State->RequestPreview(
					State->DownloadBuffer(),
					inspected.Texture,
					inspected.Width,
					inspected.Height,
					State->Inspected,
					targetSlot,
					inspected.Format
				);
			}
		}

		// --- the capture ----------------------------------------------------
		//
		// After the world's passes and before the window's, because what is
		// wanted is the scene as it was drawn rather than the scene with the
		// editor's panels over it. A copy pass, so it cannot be inside one of
		// the render passes above.
		SDL_GPUTransferBuffer *capture = nullptr;
		// **And only this viewport's, when one was named.** A request for a
		// panel that is not drawing this call is left pending rather than
		// served with the wrong picture - its turn comes round within as many
		// frames as there are panels.
		const bool captureWantsThis =
			State->CaptureSlot == Renderer::ANY_VIEWPORT || State->CaptureSlot == targetSlot;
		const bool explicitCapture = present && offscreen && !State->CapturePath.empty() && captureWantsThis;
		const bool capturingAuthored = authoredCapture.IsValid();
		Impl::NamedTexture captureSource = authoredCapture;
		std::filesystem::path capturePath = authoredCapturePath;
		if (!captureSource.IsValid() && explicitCapture) {
			const Impl::SceneSlot &scene = State->SlotAt(targetSlot);
			captureSource = {scene.Texture, sceneWidth, sceneHeight, State->ColourFormat()};
			capturePath = State->CapturePath;
		}

		if (captureSource.IsValid() && !capturePath.empty()) {
			SDL_GPUTransferBufferCreateInfo info{};
			info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
			const uint64_t captureBytes =
				static_cast<uint64_t>(captureSource.Width) * captureSource.Height * sizeof(uint32_t);
			if (captureBytes > std::numeric_limits<uint32_t>::max()) {
				ENGINE_ERROR("capture: {}x{} is too large", captureSource.Width, captureSource.Height);
			} else {
				info.size = static_cast<uint32_t>(captureBytes);
			}

			capture = info.size > 0 ? SDL_CreateGPUTransferBuffer(State->Device, &info) : nullptr;
			if (capture == nullptr) {
				ENGINE_ERROR("capture: SDL_CreateGPUTransferBuffer: {}", SDL_GetError());
			}

			// The copy records on the later-transfer buffer, which is submitted
			// after the main buffer - so the picture is the frame as drawn, on
			// the same one-queue ordering the previews rely on.
			SDL_GPUCommandBuffer *downloads = capture != nullptr ? State->DownloadBuffer() : nullptr;
			if (capture != nullptr && downloads == nullptr) {
				SDL_ReleaseGPUTransferBuffer(State->Device, capture);
				capture = nullptr;
			}
			if (capture != nullptr) {
				SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(downloads);

				SDL_GPUTextureRegion source{};
				source.texture = captureSource.Texture;
				source.w = captureSource.Width;
				source.h = captureSource.Height;
				source.d = 1;

				SDL_GPUTextureTransferInfo destination{};
				destination.transfer_buffer = capture;
				destination.pixels_per_row = captureSource.Width;
				destination.rows_per_layer = captureSource.Height;

				SDL_DownloadFromGPUTexture(copy, &source, &destination);
				SDL_EndGPUCopyPass(copy);
			}
		}

		// Studio chrome is a host concern, not a universe render-graph stage. It
		// is recorded after `output-image`, so graph previews, authored captures,
		// and rendering profiles contain only the game image and game interface.
		if (drawHostOverlay) {
			ENGINE_PROFILE_CAT("host overlay pass", core::ProfileCategory::Render);
			SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(command, &windowTarget, 1, nullptr);
			hostOverlayHook->Record(command, pass);
			SDL_EndGPURenderPass(pass);
			windowTarget.load_op = SDL_GPU_LOADOP_LOAD;
			result.DrawCalls++;
		}

		// **The window, when nothing else touched it.** With the world drawn
		// offscreen and neither the overlay nor the interface open, no pass has
		// reached the swapchain - and presenting a texture the driver handed
		// back without writing to it shows last frame's image or uninitialised
		// memory. One clear costs nothing and removes the whole case.
		if (swapchain != nullptr && windowTarget.load_op == SDL_GPU_LOADOP_CLEAR) {
			SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(command, &windowTarget, 1, nullptr);
			SDL_EndGPURenderPass(pass);
		}

		// Every earlier view has finished recording, but the command buffer still
		// belongs to the batch. The last view alone transfers ownership to SDL.
		if (State->BatchActive && !State->BatchFinal) {
			return result;
		}
		if (State->BatchActive) {
			State->BatchCommand = nullptr;
		}

		// Before the submit rather than after it, so a frame that fails to
		// submit still reports what it built.

		{
			// Hands the whole buffer over and queues the present. The passes
			// above only *record* commands, so almost nothing that happens in
			// them is measured by their spans - this is where the driver gets
			// the work, and where any cost of building it lands.
			ENGINE_PROFILE_CAT("submit", core::ProfileCategory::Render);

			// The main buffer goes first: its submit queues the present, and on
			// SDL's one unified queue it is what guarantees every pass has
			// executed before the later-transfer buffer's downloads read their
			// textures. The fences below therefore move to that second buffer.
			if (!SDL_SubmitGPUCommandBuffer(command)) {
				ENGINE_ERROR("SDL_SubmitGPUCommandBuffer: {}", SDL_GetError());
				State->Timestamps.Abandon(timingSlot);
				if (timingSlot < VulkanTimestamps::SLOTS) {
					State->PendingMarks[timingSlot].clear();
				}
				if (capture != nullptr) {
					SDL_ReleaseGPUTransferBuffer(State->Device, capture);
				}
				State->DropDownloads();
				return result;
			}

			if (SDL_GPUCommandBuffer *downloads = State->DownloadCommand; downloads != nullptr) {
				State->DownloadCommand = nullptr;
				result.DownloadCommandBuffers++;
				if (capture != nullptr) {
					// **A fence, and the stall is the point.** The pixels are
					// not there until the GPU has run the copy, so a capture has
					// to wait for it. That is a frame's worth of latency on the
					// frames a caller asked to capture and on no others. The
					// main buffer is already on the queue, so a failure here
					// loses this frame's downloads, never the frame itself.
					SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(downloads);
					if (fence == nullptr) {
						ENGINE_ERROR("SDL_SubmitGPUCommandBufferAndAcquireFence: {}", SDL_GetError());
						SDL_ReleaseGPUTransferBuffer(State->Device, capture);
						if (State->PreviewSubmitted) {
							State->Preview.Pending.Poll(true);
							State->Preview.Pixels.clear();
							State->Preview.Histogram = ImageHistogram{};
						}
					} else {
						SDL_WaitForGPUFences(State->Device, true, &fence, 1);
						SDL_ReleaseGPUFence(State->Device, fence);
						if (State->PreviewSubmitted) {
							State->Preview.Pending.Poll(true);
							State->CollectPreview();
						}

						if (State->WriteCapture(
								capture,
								captureSource.Width,
								captureSource.Height,
								captureSource.Format,
								capturePath
							)) {
							ENGINE_INFO(
								"captured {} x {} to {}",
								captureSource.Width,
								captureSource.Height,
								capturePath.string()
							);
						}

						SDL_ReleaseGPUTransferBuffer(State->Device, capture);

						// Once. A request that repeated would write a file
						// every frame and stall every one of them.
						if (explicitCapture && !capturingAuthored) {
							State->CapturePath.clear();
							State->CaptureSlot = Renderer::ANY_VIEWPORT;
						}
					}
				} else if (State->PreviewSubmitted) {
					State->Preview.Fence = SDL_SubmitGPUCommandBufferAndAcquireFence(downloads);
					if (State->Preview.Fence == nullptr) {
						ENGINE_ERROR(
							"resource preview: SDL_SubmitGPUCommandBufferAndAcquireFence: {}", SDL_GetError()
						);
						State->Preview.Pending.Poll(true);
						State->Preview.Pixels.clear();
						State->Preview.Histogram = ImageHistogram{};
					}
				} else if (!SDL_SubmitGPUCommandBuffer(downloads)) {
					ENGINE_ERROR("SDL_SubmitGPUCommandBuffer (downloads): {}", SDL_GetError());
				}
			}
		}
		if (timingSlot < VulkanTimestamps::SLOTS) {
			State->Timestamps.Submitted(timingSlot);
			if (!State->PendingMarks[timingSlot].empty()) {
				State->TimingSequence[timingSlot] = State->NextTimingSequence++;
			} else {
				State->TimingSequence[timingSlot] = 0;
			}
		}

		// **Not presented, because there is nowhere to present to.** A caller
		// counting presented frames gets zero from a headless renderer, which is
		// the honest answer - what it should count instead is captures, or its
		// own loop.
		result.Presented = swapchain != nullptr;
		return result;
	}
}
