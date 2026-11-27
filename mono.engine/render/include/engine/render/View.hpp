#pragma once

// The per-view scene description handed to Renderer::Render, with its particle and target inputs.

#include <engine/core/Name.hpp>
#include <engine/core/types/AABB.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Color3.hpp>
#include <engine/effects/ParticleSystem.hpp>
#include <engine/effects/Particles.hpp>
#include <engine/effects/Ribbon.hpp>
#include <engine/render/SceneLight.hpp>
#include <engine/render/SurfaceView.hpp>
#include <engine/render/capture/DataCapture.hpp>
#include <engine/render/presentation/PresentationDamage.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/DrawInstance.hpp>
#include <engine/scene/environment/CloudDensity.hpp>
#include <engine/scene/environment/GpuParticleField.hpp>
#include <engine/scene/environment/Sunlight.hpp>
#include <engine/scene/environment/VectorField.hpp>

#include <glm/mat4x4.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace engine::render {
	// One portal pane a particle can be drawn through.
	//
	// **Flattened out of `scene::PortalSeam` and `scene::SeamTransform` rather
	// than carrying them**, because the step shader needs exactly seven things
	// and those two types carry rather more - a pane's entities, the part its far
	// end is on, whether the viewer crosses it. A seam is uploaded per frame per
	// pane, so what it carries is what it costs.
	//
	// The rectangle test is `scene::SeamCarries` and the mapping is
	// `scene::SeamTransform::Point`; `particle-step.comp` states both again in
	// the form it applies them.
	//
	// @since v0.17
	struct ParticleSeam {
		// The pane's plane: a point on it and its face's unit normal.
		//@{
		core::Vector3 Centre;
		core::Vector3 Normal;
		//@}

		// The pane's half-axes in the world, so `Centre ± First ± Second` is the
		// rectangle.
		//@{
		core::Vector3 First;
		core::Vector3 Second;
		//@}

		// Where a point on this side lands on the far one, and what it is scaled
		// about - which is `Centre`.
		core::CFrame Mapping;

		// `scene::SeamTransform::Scale`: 1 for a mirror and for any matched pair.
		float Scale = 1.0f;
	};

	// One emitter's worth of particles, and the state they share.
	//
	// **A batch rather than a per-particle description, and that is the whole of
	// why `effects::ParticleInstance` is twenty-eight bytes.** Texture, blend mode
	// and flipbook layout are the same for every particle of one emitter, so they
	// travel once per emitter and the particles carry nothing but what varies.
	// Half a million particles times the four bytes a texture name would have cost
	// is two megabytes a frame.
	//
	// **A pointer to the block and not a copy of the particles**, and since v0.17
	// not a span of them either. The device owns the pool: `particle-step.comp`
	// integrates and shades from the block's own parameters and writes the
	// finished instances straight into the sorted draw stream, so what has to
	// cross is the block - its frame, its drag, its curves, its run of the pool -
	// and not half a million particles. The block is the caller's and is read
	// during the call. Batch state is copied only so later cameras in that same
	// call can draw the already-prepared pool; its block pointer is never read
	// after `Render` returns.
	//
	// @since v0.10
	struct ParticleBatch {
		// This emitter's block: where its run of the pool is and what the step
		// should do to it.
		//
		// Null draws nothing. The renderer reads `First`, `Capacity`, `Frame`,
		// `Acceleration`, `Drag`, `Locked`, the flipbook fields and all four
		// curves out of it, and reads nothing else.
		const effects::EmitterBlock *Block = nullptr;

		// Spawn configuration and device-owned rate counters for this block.
		// These are separate from EmitterBlock so the host fallback can stream its
		// compact runtime row without pulling curves and transforms into cache.
		//@{
		const effects::EmitterSpawnState *Spawn = nullptr;
		const effects::EmitterRuntime *Runtime = nullptr;
		//@}

		// Which block this is, from `effects::EmitterSlot::Index`.
		//
		// **The index and not only the pointer**, because the renderer keeps two
		// device tables indexed by block and a note of what it last told each
		// row. A pointer says where
		// the block is in this frame's list; the index says which block it *is*,
		// which is what survives a frame.
		uint32_t Index = 0;

		// Which texture, by name. Invalid draws an untextured quad, which is a
		// visible flat square rather than nothing.
		core::Name Texture;

		// Store namespace that owns generated images used by this emitter.
		// Missing local images fall back to the view's authored image namespace.
		core::Name SourceWorld;

		// How many cells the flipbook has on each side. One is not a flipbook.
		//
		// **A side rather than a layout enum**, because that is what the shader
		// divides by - converting an enum to a side in the renderer would be
		// doing per emitter what `effects::FlipbookSide` already does at compile
		// time on the other side of the boundary.
		float FlipbookSide = 1.0f;

		// How far towards the eye the quads are nudged, in metres.
		float ZOffset = 0.0f;

		// How blending moves from ordinary alpha to additive, and how much the
		// world's lighting modulates the particle colour.
		//@{
		float LightEmission = 0.0f;
		float LightInfluence = 0.0f;
		//@}

		// Whether the colour is added to the target rather than blended into it.
		//
		// **Selects a pipeline and not a uniform**, because blend state is baked
		// into a pipeline. Batches are drawn blended-first then additive, so the
		// pipeline is bound twice per frame rather than once per emitter.
		bool Additive = false;

		// Whether the quad keeps world up rather than the camera's.
		//
		// From `effects::ParticleOrientation::FacingCameraWorldUp`, which is what
		// stops a column of smoke rolling when the camera does.
		bool WorldUp = false;

		// Whether an untextured quad receives the analytic soft-disc mask.
		bool SoftParticles = false;

		// Explicit padding, for the reason every `Reserved` in the engine exists.
		uint8_t Reserved = 0;
	};

	// An offscreen colour target the world is drawn into instead of the window.
	//
	// **The editor's requirement, and the transparent-window trick it replaced
	// could not work.** A studio wants the world inside a dockable panel, and
	// the obvious cheap answer - draw the world to the swapchain and put a
	// background-less imgui window over it - fails the moment that window is
	// actually docked. `imgui.cpp`'s `central_node_hole` is only punched while
	// the central node is *empty*, so docking a panel into it makes the
	// dockspace fill its whole rectangle with `ImGuiCol_WindowBg` and paint over
	// the frame. The hole is a hole, not a panel.
	//
	// So the world goes into a texture and the panel shows it. That is what
	// every editor does, and it buys more than the fix: the viewport becomes an
	// ordinary window that docks, floats, resizes and closes like every other,
	// and several views become a matter of several targets rather than a second
	// arrangement of the window.
	//
	// The texture is the swapchain's format, because the pipelines that draw
	// into it were built against that format, and it is sampleable, because
	// something has to show it.
	//
	// @since v0.7
	struct SceneTarget {
		// How wide the texture is, in pixels. Zero draws to the window instead.
		uint32_t Width = 0;

		// How tall the texture is, in pixels. Zero draws to the window instead.
		uint32_t Height = 0;

		// Reports whether this asks for an offscreen target at all.
		//
		// @return `true` when both dimensions are positive.
		bool IsValid() const {
			return Width > 0 && Height > 0;
		}
	};

	// Render-local binding from an authored world to its admitted content session.
	struct WorldContentOwner {
		// Stable name of the world that authored the content references.
		core::Name World;
		// Content residency namespace admitted for that world.
		core::Name Owner;
	};

	// An owned copy of authored initial conditions and the generic force field.
	// No renderer path retains or dereferences a world row.
	struct GpuParticleFieldView {
		// The authored enable, layer, count and seed request.
		scene::GpuParticleField Field;
		// Transform mapping authored local samples into world coordinates.
		core::CFrame Frame;
		// Generic nearest-ancestor vector field retained without an ECS pointer.
		scene::VectorFieldSample ForceField;
		// Source identity and observed definition epoch, including sample/style writes.
		ecs::Entity Source = ecs::NULL_ENTITY;
		// Component version used to invalidate retained particle images.
		uint64_t DefinitionRevision = 0;
		// Source-world time keeps retained images current while device positions advance.
		double Seconds = 0;
	};

	// One camera invocation in a graph-owned frame.
	//
	// Spans are borrowed for the duration of `Renderer::Render`. A batch groups
	// views by `World` and `Pipeline`, runs world-scoped nodes once for each
	// group, records every view into one command buffer, then submits once.
	//
	// @since v0.17
	// A surface drawn into its mesh's UVs and kept as textures: the baked
	// prefix of a long material layer stack. The five layers are named
	// `Output` followed by `/0` to `/4` (see `SURFACE_BAKE` in
	// `engine-surface-main.glsl` for what each holds).
	struct SurfaceBake {
		// A `bake/` surface the shader library holds.
		core::Name Shader{};
		core::Name Mesh{};
		core::Name Output{};
		// The content namespace of the shader, mesh and textures.
		core::Name Owner{};
		uint32_t Size = 1024;
		core::Name MaterialTexture0{};
		core::Name MaterialTexture1{};
		std::array<float, 16> Parameters{};
		uint16_t ParameterMask = 0;
		// What it was baked from; a new value bakes again.
		uint64_t Revision = 0;
	};

	struct View {
		// Borrowed immutable solver checkpoint bytes, owned by the collected world packet.
		std::span<const std::byte> WeatherCheckpoint;
		// Trusted canonical full stream name copied from the accepted source definition.
		core::Name WeatherSimulationLayer;
		// Accepted terrain snapshot identity, checked before adopting physical checkpoint resources.
		uint32_t WeatherTerrainRevision = 1;
		// Changes only when that packet publishes or retires a checkpoint.
		uint64_t WeatherCheckpointVersion = 0;
		// Copied checkpoint identity; decoder validates these against the encoded payload.
		//@{
		uint64_t WeatherCheckpointTick = 0;
		uint64_t WeatherCheckpointEpoch = 0;
		uint32_t WeatherCheckpointRevision = 0;
		//@}
		// Immutable packed cloud density copied from the source world.
		std::optional<scene::CloudDensitySnapshot> CloudDensity;
		// Borrowed camera-packet precipitation preserves the authored particle field.
		// The owning WorldCameraFrame outlives this render invocation.
		std::span<const GpuParticleFieldView> WeatherParticles;
		// An HDR image owned by this world and viewport. The eye-image node defaults
		// to whole-eye projection; seam intermediates require its explicit projection option.
		// Zero denotes an unavailable image.
		uint64_t EyeImage = 0;
		// Nearest first, sharing EyeImageKey and the base image's capture identity.
		std::array<uint64_t, 2> EyeTransparentImages{};
		// Premultiplied world-space interface layer over the eye image.
		uint64_t EyeSpatialOverlayImage = 0;
		// Stable portal key shared by every eye-image layer.
		core::Name EyeImageKey;

		// Primary-eye body selection. Rig is a local root handle; Player is the
		// stable account identity used to resolve that root in another world.
		// Secondary views and shadow casters retain these rows.
		uint64_t EyeRig = 0;
		// Stable account identity of the first-person player, when known.
		std::optional<int64_t> EyePlayer;
		// Sorted indices into Instances. Import selection has no local ECS handle.
		std::span<const uint32_t> EyeHiddenRows;
		// Which retained parts of this view differ from the last completed
		// presentation. The conservative default preserves callers that do not
		// yet provide independent signatures.
		PresentationDamage Damage{
			.Scene = true,
			.GameInterface = true,
			.HostInterface = true,
			.Viewport = true,
			.Overlay = true,
			.Objects = true,
			.Particles = true,
			.Environment = true,
			.Portals = true,
		};

		// The eye transform and lens for this invocation.
		//@{
		core::CFrame CameraFrame;
		// The pose used for culling and camera-dependent collection. Empty keeps
		// ordinary views on CameraFrame. The free CameraFrame still supplies raster
		// projection, so callers can inspect from a different position while these
		// camera-dependent decisions stay anchored to VisibilityFrame.
		std::optional<core::CFrame> VisibilityFrame;
		// arch-waiver ecs-copy: each View is one render invocation, not retained world state.
		scene::Camera Camera;
		// Set by the product's completed world snapshot barrier. A queued data
		// capture only becomes Ready when this exact identity reaches its graph
		// capture node; an empty value is intentionally not a data-factory frame.
		std::string SnapshotId;
		// Identity and ordering supplied by the completed render-sample producer.
		// Motion capture refuses history when this is empty, a sequence is skipped,
		// or CameraCut marks a restore, rebinding, or projection discontinuity.
		std::string CameraTemporalId;
		uint64_t CameraTemporalSequence = 0;
		bool CameraCut = false;
		//@}

		// Returns the frame used for culling and camera-dependent collection.
		const core::CFrame &VisibilityCameraFrame() const {
			return VisibilityFrame.has_value() ? *VisibilityFrame : CameraFrame;
		}

		// Explicit clip-space projection for a fitted or portal capture. Uses
		// right-handed, Y-up, 0..1 depth coordinates for both culling and drawing.
		// Camera still supplies image limits and the background depth distance.
		std::optional<glm::mat4> Projection = {};

		// Copied lens captures keep their accepted time without changing world clocks.
		std::optional<float> LensTimeSeconds;
		// Residency namespace for copied lens shaders and textures.
		std::optional<core::Name> LensContentOwner;
		// A retained captured program group, independent of authored world content.
		uint64_t LensPrograms = 0;

		// Borrowed world data consumed by view-scoped nodes.
		//@{
		std::span<const scene::DrawInstance> Instances;
		// Caller-owned revision of the rows behind Instances. Nonzero promises
		// that the same span with the same revision holds the same rows, so
		// presentation signing hashes a list once and every camera sharing it
		// reuses that value until the revision moves. Zero hashes every call.
		uint64_t InstanceRevision = 0;
		// Studio may force distant meshes to a coarser level than the projected-area
		// selector asks for. Negative first distance leaves the area rule alone.
		std::array<float, 3> LodMinimumDistances{-1.0f, -1.0f, -1.0f};
		// Whether offscreen LOD clusters are dropped before indirect drawing.
		bool EnableLODCulling = true;
		std::span<const DataCaptureObjectLabel> ObjectLabels;
		std::span<const DataCaptureSemanticLabel> SemanticLabels;
		std::span<const DataCapturePartLabel> PartLabels;
		bool ObjectLabelsValid = true;
		bool SemanticLabelsValid = true;
		bool PartLabelsValid = true;
		std::span<const core::CFrame> JointFrames;
		std::span<const SurfaceView> Surfaces;
		//@}

		// The offscreen destination, or null for the presenting view.
		const SceneTarget *Target = nullptr;

		// Which persistent renderer target bank this view owns.
		size_t Slot = 0;
		// Nonzero only for one bridge-coordinated capture group. It keeps a
		// generated physical slot from admitting an unrelated logical-slot ticket.
		uint64_t CaptureGroup = 0;

		// A stable key shared only by views of the same logical world.
		uint64_t World = 0;

		// Store incarnation paired with the retained spatial input frame.
		uint64_t SpatialStoreIdentity = 0;
		// Host-owned GUI snapshot sequence paired with this view.
		uint64_t SpatialSnapshotSequence = 0;
		// Whether to retain scene depth for this snapshot's input routing.
		bool CaptureSpatialDepth = false;
		// Whether the following requirements came from the exact staged GUI frame.
		bool SnapshotLayersKnown = false;
		// Whether that frame can consume screen-space GUI input.
		bool SnapshotScreenExpected = false;
		// Whether that frame contains a visible spatial GUI collector.
		bool SnapshotSpatialExpected = false;
		// Whether the frozen frame contains screen-space drawing commands.
		bool SnapshotScreenCommands = false;
		// Whether the frozen frame contains world-space drawing commands.
		bool SnapshotSpatialCommands = false;

		// The persistent identity of that world for resident visual state. Entity
		// handles and particle block indices collide between stores, so both the
		// instance rows and particle pool use this with `World`.
		core::Name WorldName;
		// World-local generated image lookup context. Studio may draw authority
		// geometry with lighting and generated images from the displayed replica.
		core::Name ImageGraphWorldName;

		// Native rows use ContentOwner; copied rows select by SourceWorld.
		// Unbound worlds retain the shared namespace for callers without scoped content.
		// Built-in assets always use shared residency.
		core::Name ContentOwner;
		// Borrowed owner mappings for draw rows copied from other worlds.
		std::span<const WorldContentOwner> ForeignContentOwners;

		// Resolves a copied row's source world to its content residency owner.
		core::Name ContentOwnerOf(core::Name sourceWorld) const {
			if (!sourceWorld.IsValid() || sourceWorld == WorldName) return ContentOwner;
			for (const WorldContentOwner &binding : ForeignContentOwners) {
				if (binding.World == sourceWorld) return binding.Owner;
			}
			return {};
		}

		// A graph installed through `Renderer::SetPipeline`, or invalid for the default.
		core::Name Pipeline;

		// Optional request-owned limits for recursive image work. Pixels excludes
		// the primary view and is shared by portal and mirror captures.
		struct SurfaceCaptureBudget {
			// Remaining nested capture depth.
			uint32_t Depth = 0;
			// Remaining offscreen pixel allowance.
			uint64_t Pixels = 0;
		};
		// Limits shared by recursive portal and surface captures, if set.
		std::optional<SurfaceCaptureBudget> SurfaceBudget;

		// Borrowed transparent and lighting work for this view.
		//@{
		std::span<const ParticleBatch> Particles;

		// The panes a particle can be drawn through, already flattened.
		//
		// Empty in a scene with no portals, which is nearly all of them. See
		// `ParticleSeam`.
		std::span<const ParticleSeam> ParticleSeams;

		// Optional source-store identity for particle and instance residency.
		// A client can share authored image/program ownership with its authority
		// while its local emitter indices, device populations and resident
		// instance rows remain independent. An absent name uses World and WorldName.
		uint64_t ParticleWorld = 0;
		core::Name ParticleWorldName;

		// Which simulation revision produced the particle blocks and seams. An
		// unchanged value needs only another device step for the new
		// visual delta; ParticleLayoutRevision separately owns ordering.
		uint64_t ParticleRevision = 0;

		// Which emitter membership and material ordering `Particles` describes.
		// Unlike ParticleRevision, this stays constant while only simulation and
		// resident block values advance.
		uint64_t ParticleLayoutRevision = 0;

		// Which resident parameter and curve-table content `Particles` names.
		uint64_t ParticleResidentRevision = 0;

		// How far to advance the particle simulation, in seconds.
		//
		// **The frame's step and not the tick's.** The device steps once per
		// rendered frame, so this is time since the last one; spawning still
		// happens on the tick, and the two agree because both are rates per
		// second.
		//
		// **Consumed by the first view of this logical world in one `Render`
		// call.** A mirror, a surface camera and the room are several pictures of
		// the same particles, so the renderer prepares the pool once and later
		// cameras reuse it even when they repeat this value. Two different worlds
		// own different pools and each may advance once. Zero draws the last
		// prepared state, which is also what a paused view wants.
		float ParticleDelta = 0.0f;

		// How many blocks the world's pool has ever handed out, from
		// `effects::ParticleSystem::Blocks`.
		//
		// **The whole list and not this frame's batches**, because the tables the
		// renderer keeps are indexed by block: sizing them to the highest block
		// drawn would re-create them - and lose what every other row held - the
		// first time a further emitter claimed a later one.
		uint32_t ParticleBlocks = 0;

		// How many slots the device pool must hold, from
		// `effects::ParticleSystem::Capacity`.
		//
		// **Declared by the caller rather than inferred from the batches**,
		// because the pool is allocated once and the blocks index into it: a
		// buffer sized to this frame's highest block would be re-created the
		// first time a further emitter claimed a later one.
		uint32_t ParticlePool = 0;

		std::span<const effects::RibbonVertex> RibbonVertices;
		std::span<const effects::RibbonRun> RibbonRuns;
		std::optional<GpuParticleFieldView> GpuParticles;
		std::span<const SceneLight> Lights;
		//@}

		// Borrowed cross-world geometry and portal descriptions.
		//@{
		std::span<const scene::DrawInstance> Foreign;
		std::span<const core::CFrame> ForeignJointFrames;
		std::span<const PortalView> Portals;
		//@}

		// Surfaces to bake before this view draws; each runs once per revision.
		std::span<const SurfaceBake> Bakes;

		// Per-world lighting used when `OverrideLighting` is true.
		scene::WorldLighting Lighting;

		// Whether to replace the renderer's current lighting for this view.
		bool OverrideLighting = false;

		// Complete shadow domain in this world's coordinates. Source and body passes
		// share this bound to reproduce the native combined scene's raster grid.
		// Must be finite, ordered and contain every drawable row in this view.
		std::optional<core::AABB> DirectionalShadowBounds;
		// Renderer-local imported source depth, combined with this view's body casters.
		// Requires the exact imported domain in DirectionalShadowBounds.
		uint64_t ImportedDirectionalShadow = 0;

		// The editor's ground grid, drawn in this view or not drawn at all.
		//
		// **On the view rather than in the pipeline document, because the grid
		// is a property of who is looking.** A profile is chosen per world and
		// the studio shows one world in several panels - a replica panel beside
		// an authored one - so a grid wired into the graph would appear in the
		// panel that exists to show exactly what a player sees. `Renderer`
		// draws it as one triangle at the head of the transparent pass, which
		// already has the depth attached; `grid.frag` carries the rest.
		//
		// @since v0.19
		struct GroundGrid {
			// Whether to draw it at all. Off, so a client pays one branch.
			bool Enabled = false;

			// Studs between thin lines, and how many cells to a heavy one.
			//@{
			float Step = 4.0f;
			float Major = 8.0f;
			//@}

			// The studs at which it has faded out entirely.
			float Reach = 480.0f;

			// An overall multiplier on every line's alpha, so a host can dim
			// the whole grid without restating its colours.
			float Strength = 1.0f;

			// World-space X/Z origin used for line placement and the two axes.
			// The ground plane itself remains at Y zero.
			core::Vector3 Offset{};

			// The line colour, and the alpha a heavy line reaches.
			//@{
			core::Color3 Colour{0.6f, 0.65f, 0.75f};
			float Alpha = 0.5f;
			//@}

			// The two axes at the origin: X red, Z blue, the convention every
			// editor uses. `AxisAlpha` is theirs for the same reason `Alpha` is
			// the grid's.
			//@{
			core::Color3 AxisX{0.85f, 0.30f, 0.32f};
			core::Color3 AxisZ{0.32f, 0.50f, 0.90f};
			float AxisAlpha = 0.65f;
			//@}
		};

		// This view's grid. Off unless the view says otherwise, so a client that
		// never mentions it draws none.
		GroundGrid Grid;
	};

	// How much of a slot's texture the world was actually drawn into.
	//
	// **The texture is bigger than the panel on purpose, and this is how a
	// caller finds the part that is the picture.** A target allocated to the
	// panel's exact size has to be destroyed and created again on every frame
	// of a drag - and worse than the allocation, the *new* texture is not the
	// one the interface already recorded a bind of, so the panel spends the
	// whole drag showing the previous frame's image stretched to a rectangle it
	// was never drawn for. Rounding the allocation up to a block keeps one
	// texture alive across the whole drag, which means the image the panel
	// samples is the one this frame drew.
	//
	// What that costs is a border of pixels nothing draws into, and what it
	// needs is for whoever shows the texture to sample only the corner that is
	// the world. These are those coordinates: `(0, 0)` to `(U, V)`.
	//
	// @since v0.7
	struct SceneExtent {
		// The right edge of the drawn region, as a fraction of the texture.
		float U = 1.0f;

		// The bottom edge of the drawn region, as a fraction of the texture.
		float V = 1.0f;

		// The pixel rectangle represented by this image. A panel may already
		// have requested another size while its round-robin view still holds
		// this one, so the sampling fractions alone cannot say whether showing
		// it would stretch the camera projection.
		//@{
		uint32_t DrawnWidth = 0;
		uint32_t DrawnHeight = 0;
		//@}
	};
}
