#include <engine/core/Log.hpp>
#include <engine/core/Paths.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/graph/Cull.hpp>
#include <engine/graph/Shadow.hpp>
#include <engine/render/MeshTable.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/render/TextureTable.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Tagging.hpp>

#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_video.h>
#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

#include <array>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace engine::render {

	namespace {

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
		// Built-in meshes are unit shapes; scale is folded into the model matrix.
		GpuInstance ToGpu(const scene::DrawInstance &instance) {
			GpuInstance gpu;
			gpu.Model = instance.Frame.ToMatrix();

			// Built-in geometry is one unit across.
			gpu.Model[0] *= instance.HalfExtent.X * 2.0f;
			gpu.Model[1] *= instance.HalfExtent.Y * 2.0f;
			gpu.Model[2] *= instance.HalfExtent.Z * 2.0f;

			// Convert author-facing transparency to shader alpha.
			gpu.Colour =
				glm::vec4{instance.Tint.R, instance.Tint.G, instance.Tint.B, 1.0f - instance.Transparency};

			// Avoid infinities for zero-scale geometry.
			const auto reciprocal = [](float extent) {
				const float scale = extent * 2.0f;
				return scale * scale > 1e-12f ? 1.0f / (scale * scale) : 1.0f;
			};
			gpu.InverseScaleSquared = glm::vec4{
				reciprocal(instance.HalfExtent.X),
				reciprocal(instance.HalfExtent.Y),
				reciprocal(instance.HalfExtent.Z),
				0.0f,
			};
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

			// x: whether a shadow map was rendered. y: one shadow texel.
			// z: whether this draw samples the surface texture. w: unused, and
			// named so the struct's size is stated rather than implied.
			glm::vec4 Flags;

			// The submesh's own colour, white for a draw with no material.
			glm::vec4 BaseColour{1.0f, 1.0f, 1.0f, 1.0f};

			// x: whether `colourMap` holds this draw's texture. y: the alpha
			// below which a fragment is discarded, or zero to discard none.
			glm::vec4 Surface{0.0f, 0.0f, 0.0f, 0.0f};
		};

		// Mix renderer-owned view data into the scene signature.
		uint64_t MixFloat(uint64_t hash, float value) {
			uint32_t bits = 0;
			std::memcpy(&bits, &value, sizeof(bits));
			return scene::MixSignature(hash, bits);
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
		constexpr glm::vec3 SUN_DIRECTION{-0.45f, -0.8f, -0.4f};
		constexpr glm::vec4 SUN_AMBIENT{0.26f, 0.28f, 0.34f, 1.0f};

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

		// Walks `PassOrder()` as the frame is submitted.
		//
		// Runtime guard for pass order; optional passes may be skipped.
		struct PassRecorder {
			uint8_t Ran = 0;
			uint8_t Furthest = 0;
			bool Complained = false;

			void Enter(Pass pass) {
				const auto index = static_cast<uint8_t>(pass);

				if (index < Furthest && !Complained) {
					ENGINE_ERROR(
						"render pass '{}' submitted after '{}', which PassOrder puts before it",
						PassOrder()[index].Text(),
						PassOrder()[Furthest].Text()
					);
					Complained = true;
				}

				if (index > Furthest) {
					Furthest = index;
				}

				Ran |= static_cast<uint8_t>(1u << index);
			}
		};
	}

	std::span<const core::Name> PassOrder() {
		// Function-local rather than a namespace-scope array, because a
		// `core::Name` interns on construction and interning before `main` is
		// how a static initialisation order bug is written.
		static const std::array<core::Name, static_cast<size_t>(Pass::Count)> order{
			core::Name("shadow"),
			core::Name("surface"),
			core::Name("opaque"),
			core::Name("transparent"),
			core::Name("overlay"),
			core::Name("interface"),
		};
		return order;
	}

	// -----------------------------------------------------------------------

	struct Renderer::Impl {
		SDL_Window *Window = nullptr;
		SDL_GPUDevice *Device = nullptr;

		SDL_GPUGraphicsPipeline *OpaquePipeline = nullptr;

		// The same geometry and the same shaders as the opaque pipeline, with
		// blending on and depth writes off. Two pipelines rather than one with
		// a uniform, because blend state is baked into a pipeline on every
		// modern API and cannot be changed by a draw call.
		SDL_GPUGraphicsPipeline *TransparentPipeline = nullptr;

		// The submission order, rebuilt each frame and kept so it is not
		// reallocated per frame. See `scene::OrderForDrawing`.
		std::vector<uint32_t> DrawOrder;

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
		std::vector<uint32_t> SceneOrder;
		SDL_GPUGraphicsPipeline *OverlayPipeline = nullptr;

		// Every mesh and texture available to the renderer.
		MeshTable Meshes;
		TextureTable Textures;

		// What is in each slot of the instance buffer, filled in the same loop
		// that fills the buffer itself.
		//
		// **Parallel arrays rather than a struct**, because the draw loop
		// compares consecutive entries to find its runs and does it far more
		// often than it reads them.
		std::vector<const MeshEntry *> SlotMesh;
		std::vector<core::Name> SlotTexture;

		// Each slot's tag mask, for the surface passes that filter by one.
		std::vector<uint32_t> SlotTags;

		SDL_GPUBuffer *InstanceBuffer = nullptr;
		SDL_GPUTransferBuffer *InstanceTransfer = nullptr;
		uint32_t InstanceCapacity = 0;

		// --- particles --------------------------------------------------------
		//
		// **Two pipelines and one buffer.** The two differ only in their blend
		// state — one mixes into the target and one adds to it — and blend state
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
		// Separate from `InstanceBuffer` because the strides differ — 28 bytes
		// against 96 — and a shared buffer would mean either padding a particle to
		// a mesh instance's width or rebinding the vertex layout mid-pass.
		SDL_GPUBuffer *ParticleBuffer = nullptr;
		SDL_GPUTransferBuffer *ParticleTransfer = nullptr;
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
		// where a test can reach it — `ribbon.vert` carries the argument.
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
		// that does not need saving, and it cannot merge two runs anyway — they
		// are separate strips, and a strip drawn as one primitive would connect
		// the end of one to the start of the next.
		//
		// @return How many draw calls were issued.
		uint32_t DrawRibbons(
			SDL_GPUCommandBuffer *command,
			SDL_GPURenderPass *pass,
			const glm::mat4 &viewProjection,
			const core::CFrame &eye,
			std::span<const effects::RibbonRun> runs
		);

		// This frame's groups, and the batch order they were built from.
		//
		// Members rather than locals, so the capacity survives the frame — the
		// same argument `DrawOrder` makes one screen up.
		std::vector<ParticleGroup> ParticleGroups;
		std::vector<uint32_t> ParticleOrder;

		// Grows the particle buffer to hold at least `count`, keeping what fits.
		//
		// **Grown and never shrunk**, exactly as the instance buffer is: an
		// explosion is a spike in the particle count and a frame that reallocated
		// on the way back down would pay for the spike twice.
		bool ReserveParticles(uint32_t count);

		// Groups the batches, packs them into the transfer buffer, and records
		// where each group lands.
		//
		// **Outside every render pass, beside the instance upload**, because the
		// copy that follows it is a copy pass and a copy pass cannot be started
		// while a render pass is open. The first version of this did the memcpy
		// inside the draw and never issued the copy at all, so the vertex buffer
		// held whatever the previous frame left — which draws *something*, which
		// is why it took a capture rather than a crash to find.
		//
		// @return How many particles were packed.
		uint32_t PrepareParticles(std::span<const ParticleBatch> batches);

		// Draws what `PrepareParticles` packed.
		//
		// @return How many draw calls were issued.
		uint32_t DrawParticles(
			SDL_GPUCommandBuffer *command,
			SDL_GPURenderPass *pass,
			const glm::mat4 &viewProjection,
			const core::CFrame &eye,
			std::span<const ParticleBatch> batches
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
		// own — a colour and a depth texture per frame, which is exactly the
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
		// @return `false` when there was nothing to acquire — minimised or
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
			// that true once the wait moved ahead of the interface — an editor
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
				// adding up to nothing is a frame that is waiting here — which
				// means the GPU is the limit, not the code above it.
				//
				// Idle, not Render. Nothing is being rendered here — the thread
				// is asleep until the display is ready for another image, and
				// counting that as rendering work makes the renderer look like
				// the most expensive thing in a frame it spent waiting.
				ENGINE_PROFILE_CAT("acquire swapchain", core::ProfileCategory::Idle);
				acquired =
					SDL_WaitAndAcquireGPUSwapchainTexture(command, Window, &swapchain, &width, &height);
			}

			if (!acquired || swapchain == nullptr) {
				// Minimised, or mid-resize. Not an error, and not a reason to
				// stop ticking — the simulation carries on and the next frame
				// presents.
				//
				// **Cancelled rather than submitted, which is what SDL's own
				// example does here.** No swapchain texture was acquired, so
				// there is nothing to present and nothing recorded worth
				// executing; submitting an empty buffer sends it through the
				// whole submit path and consumes a frame in flight for no work.
				// Cancel is only legal *because* the acquire failed —
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

		bool WriteCapture(SDL_GPUTransferBuffer *from, uint32_t width, uint32_t height) const;

		// --- the shadow map -------------------------------------------------
		//
		// A depth texture and the pipeline that fills it. The **same instance
		// buffer** the colour pass binds, which is what makes a shadow map one
		// more draw over data that is already on the device.
		SDL_GPUGraphicsPipeline *ShadowPipeline = nullptr;
		SDL_GPUTexture *ShadowTexture = nullptr;
		SDL_GPUSampler *ShadowSampler = nullptr;

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

			// World to this surface camera's clip space, for the frame just
			// written. What the screen pass projects with.
			glm::mat4 ViewProjection{1.0f};

			// The same, for the frame before — which is the one another surface
			// pass samples, and it must be projected with the matrix that
			// *rendered* it. Projecting last frame's texture with this frame's
			// camera is a reflection that slides as the viewer moves, and it
			// reads as a mis-aimed camera rather than as a stale matrix.
			glm::mat4 PreviousViewProjection{1.0f};

			// How solid this surface's image is, from the view that wrote it.
			float ImageOpacity = 1.0f;

			// What the scene looked like when this surface last rendered.
			//
			// **Compared rather than trusted to a dirty bit**, for
			// `SignatureOf`'s reason: the draw list is written in bulk and
			// announces nothing. A match means this pass would redraw the
			// texture it already holds, so it is not run.
			//
			// Meaningless while `Ready` is false — a slot that has never
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
		struct SurfaceBank {
			SurfaceSlotState Surfaces[scene::MAX_SURFACES];
		};

		// **One bank per viewport, and that is what makes a mirror a mirror when
		// a world is on screen twice.** A reflection is of the viewer: `scene::
		// AimSurfaceCameras` mirrors the world's `ActiveCamera` through each
		// pane, so two panels looking at one world want two different images out
		// of the same `SurfaceCamera`. With one shared bank they got one — the
		// panel that drew most recently wrote every surface texture, and the
		// other panel then composited its panes from a reflection computed for
		// somebody else's eye. Flying either camera moved the mirrors in both
		// windows, at half the frame rate, which reads as a projection fault
		// rather than as one texture with two authors.
		//
		// The aim is still world state and still per frame — one panel draws per
		// frame and re-aims before it does — so what has to be per viewport is
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
		bool EnsureSurface(size_t viewport, uint8_t index, uint32_t width, uint32_t height);

		// One opaque white texel, bound wherever a real texture is missing.
		//
		// **The pipelines declare two fragment samplers and a draw must bind
		// both.** An unbound sampler is undefined behaviour on several backends
		// where a wrongly bound one is merely ignored, and the uniform flag is
		// what stops the result being read — so any valid texture will do, and
		// what matters is that there is always one.
		//
		// This used to be `OverlayTexture`, which is created only when a debug
		// panel has something in it, standing in for `ShadowTexture`, which is
		// created only when something casts. Both are absent together in an
		// ordinary case — a scene of nothing but transparent geometry, with the
		// panels closed — and the screen pass then bound no samplers at all and
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

		SDL_GPUShader *LoadShader(
			std::string_view name, SDL_GPUShaderStage stage, uint32_t samplers, uint32_t uniformBuffers
		) const;

		bool CreatePipelines();
		// Issues the draws for one contiguous run of instance-buffer slots.
		//
		// **The loop v0.9 exists to add.** Every draw used to be one call over
		// one cube; now a run may hold several meshes and each mesh several
		// material submeshes, so this splits the run wherever the mesh or the
		// instance's texture override changes and issues a call per resulting
		// piece. The instances themselves never move — the split is entirely in
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
			uint64_t &triangles
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
		// ask, so it takes a fixed format — and the format has to be the *same*
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
		// collide on a common file name like fullscreen.vert.
		const auto path = core::Paths::Shaders("render") / (std::string(name) + ".spv");

		const auto code = ReadFile(path);
		if (code.empty()) {
			ENGINE_ERROR("shader not found or empty: {}", path.string());
			return nullptr;
		}

		SDL_GPUShaderCreateInfo info{};
		info.code = code.data();
		info.code_size = code.size();
		info.entrypoint = "main";
		info.format = SDL_GPU_SHADERFORMAT_SPIRV;
		info.stage = stage;
		info.num_samplers = samplers;
		info.num_uniform_buffers = uniformBuffers;

		SDL_GPUShader *shader = SDL_CreateGPUShader(Device, &info);
		if (!shader) {
			ENGINE_ERROR("SDL_CreateGPUShader failed for {}: {}", name, SDL_GetError());
		}
		return shader;
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
			// nowhere rather than a validation error — the same trap the sampler
			// count above records.
			"opaque.frag",
			SDL_GPU_SHADERSTAGE_FRAGMENT,
			3,
			2
		);

		SDL_GPUShader *shadowVertex = LoadShader("shadow.vert", SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
		SDL_GPUShader *shadowFragment = LoadShader("shadow.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 0);
		SDL_GPUShader *overlayVertex = LoadShader("overlay.vert", SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
		SDL_GPUShader *overlayFragment = LoadShader("overlay.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 0);

		if (!opaqueVertex || !opaqueFragment || !shadowVertex || !shadowFragment || !overlayVertex ||
			!overlayFragment) {
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
		//   goes. Leaving the write on is the classic version of this bug — the
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

		// --- particles ------------------------------------------------------
		//
		// **No vertex buffer for the quad and no index buffer at all.** The
		// billboard's four corners come out of `gl_VertexIndex` in the shader, and
		// the primitive is a triangle strip — so one particle is one instance of
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
			// driver convert bits that are not a float, which is not a slow path —
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
			particleTarget.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
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
			// eye, so which way it winds depends on where the camera is — and a
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
			// order-independent — which is why an additive emitter needs no sort.
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

		// --- overlay --------------------------------------------------------

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
		// outlive their creation.
		SDL_ReleaseGPUShader(Device, opaqueVertex);
		SDL_ReleaseGPUShader(Device, opaqueFragment);
		SDL_ReleaseGPUShader(Device, overlayVertex);
		SDL_ReleaseGPUShader(Device, overlayFragment);

		SDL_ReleaseGPUShader(Device, shadowVertex);
		SDL_ReleaseGPUShader(Device, shadowFragment);

		// **The particle pipelines are deliberately not in this conjunction.** A
		// build whose particle shaders failed to compile still draws a world, and
		// failing initialisation over an effect would take the whole client down
		// for something a scene may not even use. `DrawParticles` checks for null
		// and draws nothing, with the error already in the log above.
		return OpaquePipeline != nullptr && TransparentPipeline != nullptr && ShadowPipeline != nullptr &&
			   OverlayPipeline != nullptr;
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
		uint64_t &triangles
	) {
		if (count == 0 || first + count > SlotMesh.size()) {
			return 0;
		}

		uint32_t calls = 0;

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
				// nothing to draw — it is one made of the engine's default
				// material, which `DefaultTexture.hpp` says is a real sheet of
				// white plastic. So the slot always has content and the shader
				// always samples it; what used to be the "no texture" branch is
				// now the case where `Material = None` means something.
				SDL_GPUTexture *const found = Textures.Find(texture);
				SDL_GPUTexture *const sampled = found != nullptr ? found : Textures.Default();

				// **The fallback texel is bound rather than the binding being
				// skipped** for the other two, because a sampler a pipeline
				// declares must have something in it — an unbound one is
				// undefined behaviour on some backends and a validation error on
				// others. Those two are genuinely absent features rather than
				// defaulted ones, and their uniform flags still say so.
				const SDL_GPUTextureSamplerBinding samplers[] = {
					{shadow != nullptr ? shadow : FallbackTexture, shadowSampler},
					{surface != nullptr ? surface : FallbackTexture, surfaceSampler},
					{sampled != nullptr ? sampled : FallbackTexture, Textures.Sampler()},
				};
				SDL_BindGPUFragmentSamplers(pass, 0, samplers, 3);

				LightingUniforms uniforms = *lighting;
				uniforms.BaseColour = glm::vec4{colour[0], colour[1], colour[2], colour[3]};
				uniforms.Surface = glm::vec4{sampled != nullptr ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f};
				SDL_PushGPUFragmentUniformData(command, 0, &uniforms, sizeof(uniforms));
			}

			SDL_DrawGPUIndexedPrimitives(
				pass, range.IndexCount, run, range.FirstIndex, range.VertexOffset, slot
			);
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

			uint32_t run = 1;
			while (slot + run < first + count && SlotMesh[slot + run] == mesh &&
				   SlotTexture[slot + run] == texture &&
				   scene::MatchesTags(SlotTags[slot + run], tagFilter)) {
				run++;
			}

			if (mesh->Runs.empty()) {
				// A mesh with no materials of its own — every built-in.
				emit(mesh->Whole, texture, {1.0f, 1.0f, 1.0f, 1.0f}, slot, run);
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
						run
					);
				}
			}

			slot += run;
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

		// Opaque white for the fallback texel. Nothing reads it — the uniform
		// flag sees to that — but a texture whose contents were never written
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
		bufferInfo.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
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
		// emitting at all has hundreds of particles — the smallest useful scene is
		// already past the mesh path's starting size, so starting there would be
		// four reallocations on the first frame anything is drawn.
		uint32_t capacity = ParticleCapacity == 0 ? 4096 : ParticleCapacity;
		while (capacity < count) {
			capacity *= 2;
		}

		if (ParticleBuffer != nullptr) {
			SDL_ReleaseGPUBuffer(Device, ParticleBuffer);
		}
		if (ParticleTransfer != nullptr) {
			SDL_ReleaseGPUTransferBuffer(Device, ParticleTransfer);
		}

		const uint32_t bytes = capacity * static_cast<uint32_t>(sizeof(effects::ParticleInstance));

		SDL_GPUBufferCreateInfo bufferInfo{};
		bufferInfo.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
		bufferInfo.size = bytes;
		ParticleBuffer = SDL_CreateGPUBuffer(Device, &bufferInfo);

		SDL_GPUTransferBufferCreateInfo transferInfo{};
		transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
		transferInfo.size = bytes;
		ParticleTransfer = SDL_CreateGPUTransferBuffer(Device, &transferInfo);

		if (ParticleBuffer == nullptr || ParticleTransfer == nullptr) {
			ENGINE_ERROR("particle buffer of {} entries: {}", capacity, SDL_GetError());
			ParticleCapacity = 0;
			return false;
		}

		ParticleCapacity = capacity;
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
			// x: whether the sampler holds this group's texture. The rest is
			// named for `ParticleUniforms::Options`'s reason.
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
		// have been moved onto the instance instead — four more bytes a particle,
		// which is two megabytes a frame at the target count against a handful of
		// extra draw calls. The draw calls are cheaper.
		bool SameParticleState(const render::ParticleBatch &left, const render::ParticleBatch &right) {
			return left.Additive == right.Additive && left.WorldUp == right.WorldUp &&
				   left.Texture == right.Texture && left.FlipbookSide == right.FlipbookSide &&
				   left.ZOffset == right.ZOffset;
		}
	}

	uint32_t Renderer::Impl::PrepareParticles(std::span<const render::ParticleBatch> batches) {
		ParticleGroups.clear();
		if (ParticlePipeline == nullptr || batches.empty()) {
			return 0;
		}

		// **Grouped by state, and this is the difference between a scene that
		// draws and one that does not.** The first version issued one draw call
		// per emitter, which at the roadmap's hundred thousand emitters is a
		// hundred thousand draw calls a frame — an order of magnitude past what
		// any driver will do at sixty hertz. Measured at 1,600 emitters it was
		// 1,608 draw calls; grouped, the same scene is **three**, because every
		// emitter in the grid shares a texture and a blend mode.
		//
		// **A stable sort into an index list rather than sorting the batches**,
		// because the caller owns them — the same reason `scene::OrderForDrawing`
		// produces an order instead of reordering a draw list.
		ParticleOrder.resize(batches.size());
		for (size_t index = 0; index < batches.size(); index++) {
			ParticleOrder[index] = static_cast<uint32_t>(index);
		}

		// Blended before additive, so the pipeline is bound twice rather than
		// alternating. Within each half the key is arbitrary but must be *total*,
		// or equal states would not end up adjacent.
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
				return static_cast<int>(a.WorldUp) < static_cast<int>(b.WorldUp);
			}
		);

		uint32_t total = 0;
		for (const render::ParticleBatch &batch : batches) {
			total += static_cast<uint32_t>(batch.Particles.size());
		}
		if (total == 0 || !ReserveParticles(total)) {
			return 0;
		}

		auto *mapped = static_cast<effects::ParticleInstance *>(
			SDL_MapGPUTransferBuffer(Device, ParticleTransfer, true)
		);
		if (mapped == nullptr) {
			return 0;
		}

		// One walk that both packs the buffer and records where each group lands.
		// After this the order is a buffer offset and the batch it came from is
		// gone, which is the same arrangement `SlotMesh` has for the mesh path.
		uint32_t written = 0;
		for (const uint32_t index : ParticleOrder) {
			const render::ParticleBatch &batch = batches[index];
			if (batch.Particles.empty()) {
				continue;
			}

			if (ParticleGroups.empty() || !SameParticleState(batches[ParticleGroups.back().Batch], batch)) {
				ParticleGroups.push_back(ParticleGroup{index, written, 0});
			}

			const auto count = static_cast<uint32_t>(batch.Particles.size());
			std::memcpy(mapped + written, batch.Particles.data(), count * sizeof(effects::ParticleInstance));
			written += count;
			ParticleGroups.back().Count += count;
		}

		SDL_UnmapGPUTransferBuffer(Device, ParticleTransfer);
		return written;
	}

	uint32_t Renderer::Impl::DrawParticles(
		SDL_GPUCommandBuffer *command,
		SDL_GPURenderPass *pass,
		const glm::mat4 &viewProjection,
		const core::CFrame &eye,
		std::span<const render::ParticleBatch> batches
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
		// `CFrame`'s own forward is -Z — the engine's camera convention, which
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
					SDL_BindGPUGraphicsPipeline(pass, AdditiveParticlePipeline);
					additiveBound = true;
				}
			} else if (!blendedBound) {
				SDL_BindGPUGraphicsPipeline(pass, ParticlePipeline);
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
			material.Flags = glm::vec4{texture != nullptr ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f};
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
		// already an index into this stream — `BuildRibbons` packed the runs
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
		std::span<const effects::RibbonRun> runs
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
		// not group by blend mode — grouping there would be a shared module
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
						SDL_BindGPUGraphicsPipeline(pass, AdditiveRibbonPipeline);
						additiveBound = true;
					}
				} else if (!blendedBound) {
					SDL_BindGPUGraphicsPipeline(pass, RibbonPipeline);
					blendedBound = true;
				}

				uniforms.Options = glm::vec4{run.ZOffset, 0.0f, 0.0f, 0.0f};
				SDL_PushGPUVertexUniformData(command, 0, &uniforms, sizeof(uniforms));

				SDL_GPUTexture *const texture = Textures.Find(run.Texture);

				ParticleMaterial material{};
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
			}
		}

		return draws;
	}

	bool Renderer::Impl::EnsureScene(uint32_t width, uint32_t height) {
		SceneSlot &target = SlotAt(ActiveSlot);

		// **Recorded before anything decides whether to allocate.** This is the
		// rectangle the pass draws and the rectangle `SceneTextureExtent`
		// reports, and it is the panel's size whether or not the texture under
		// it changed — which is the whole point of keeping the two apart.
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
			// buffer — no interface hook can have recorded a bind of it — so the
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
		// big.** Growing is forced — a texture smaller than the rectangle would
		// clip the world — but shrinking is not, and refusing to shrink for a
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
			// recorded a bind of this texture for the frame in progress — that
			// is what "the image is last frame's texture" means — so freeing it
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
		info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
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

	bool Renderer::Impl::EnsureSurface(size_t viewport, uint8_t index, uint32_t width, uint32_t height) {
		if (index >= scene::MAX_SURFACES) {
			return false;
		}

		SurfaceSlotState &state = SurfacesAt(viewport).Surfaces[index];

		if (state.Texture[0] != nullptr && width == state.Width && height == state.Height) {
			return true;
		}

		for (SDL_GPUTexture *&texture : state.Texture) {
			if (texture) {
				SDL_ReleaseGPUTexture(Device, texture);
				texture = nullptr;
			}
		}
		if (state.Depth) {
			SDL_ReleaseGPUTexture(Device, state.Depth);
			state.Depth = nullptr;
		}

		// Resized, so whatever it held is gone. A mirror that showed the last
		// frame at the old resolution stretched across the new one would be a
		// visible artefact on exactly the frame a window was dragged.
		state.Ready = false;

		SDL_GPUTextureCreateInfo colour{};
		colour.type = SDL_GPU_TEXTURETYPE_2D;
		colour.format = ColourFormat();
		colour.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
		colour.width = width;
		colour.height = height;
		colour.layer_count_or_depth = 1;
		colour.num_levels = 1;
		colour.sample_count = SDL_GPU_SAMPLECOUNT_1;

		for (SDL_GPUTexture *&texture : state.Texture) {
			texture = SDL_CreateGPUTexture(Device, &colour);
			if (!texture) {
				ENGINE_ERROR(
					"viewport {} surface {} texture {}x{}: {}", viewport, index, width, height, SDL_GetError()
				);
				return false;
			}
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

		state.Depth = SDL_CreateGPUTexture(Device, &depth);
		if (!state.Depth) {
			ENGINE_ERROR(
				"viewport {} surface {} depth {}x{}: {}", viewport, index, width, height, SDL_GetError()
			);
			return false;
		}

		if (SurfaceSampler == nullptr) {
			SDL_GPUSamplerCreateInfo sampler{};
			sampler.min_filter = SDL_GPU_FILTER_LINEAR;
			sampler.mag_filter = SDL_GPU_FILTER_LINEAR;
			sampler.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
			sampler.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
			sampler.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
			sampler.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;

			SurfaceSampler = SDL_CreateGPUSampler(Device, &sampler);
			if (!SurfaceSampler) {
				ENGINE_ERROR("surface sampler: {}", SDL_GetError());
				return false;
			}
		}

		state.Width = width;
		state.Height = height;
		return true;
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

	// -----------------------------------------------------------------------

	Renderer::Renderer() : State(std::make_unique<Impl>()), Owner(std::this_thread::get_id()) {}

	Renderer::~Renderer() {
		Shutdown();
	}

	bool Renderer::IsInitialised() const {
		return State->Device != nullptr;
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
		// so check before asking for it — an unsupported mode would otherwise
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
		// thread is filling, so there is nothing left to decline — and the
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

	bool Renderer::Initialise(SDL_Window *window) {
		// **Re-bound here, and the constructor's claim is what makes the check
		// testable without a device.** A renderer is legitimately constructed by
		// whoever owns the object and initialised by whoever owns the window —
		// it is the device, not the C++ object, that the contract is about — so
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

		// SPIR-V only. Metal needs the cross-compile step the module's
		// CMakeLists does on Apple targets; D3D12 needs DXIL, which is not
		// built yet. Asking for formats we cannot supply would find a device
		// and then fail at pipeline creation, which is a worse error.
		State->Device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, false, nullptr);
		if (!State->Device) {
			ENGINE_ERROR("SDL_CreateGPUDevice: {}", SDL_GetError());
			return false;
		}

		if (window != nullptr && !SDL_ClaimWindowForGPUDevice(State->Device, window)) {
			ENGINE_ERROR("SDL_ClaimWindowForGPUDevice: {}", SDL_GetError());
			Shutdown();
			return false;
		}

		// **One frame queued rather than SDL's two, and the frame rate is not
		// what this buys.** The default lets the CPU submit a second frame
		// before the GPU has finished the first, so what a display is showing is
		// up to two frames behind the input that produced it — 33 ms at 60 Hz
		// before the compositor takes its turn. SDL's own wording is that higher
		// values "increase throughput at the expense of visual latency", and an
		// editor is the case where that trade is backwards: nobody drags a
		// splitter to reach a frame rate, and every millisecond between the
		// mouse and the picture is felt by the hand holding it.
		//
		// What it costs is the throughput it was buying. A frame that would have
		// overlapped now waits, so a GPU-bound scene loses some of its rate —
		// which is why the measurement that matters here is the one taken by
		// hand, not the one the profiler reports.
		if (window != nullptr && !SDL_SetGPUAllowedFramesInFlight(State->Device, 1)) {
			// Not fatal. The default is a working configuration and the only
			// thing lost is the latency this was trying to save.
			ENGINE_WARN("SDL_SetGPUAllowedFramesInFlight: {}", SDL_GetError());
		}

		const char *driver = SDL_GetGPUDeviceDriver(State->Device);
		State->Backend = driver ? driver : "unknown";

		// **Before the pipelines, because they name the format too.** A pipeline
		// built against one depth format and bound beside a texture in another
		// is a validation error at bind time. See `Impl::DepthFormat`.
		{
			// **Both usages, because one format serves both kinds of depth
			// texture.** The viewport's buffer is only ever a target, but the
			// shadow map is sampled as well — and a second format for the shadow
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
			// whole scene, so the extra bits are worth asking for — and
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
		// unreachable — pump, simulate, present, and only then test whether to
		// stop — but "usual" is not a guarantee, and a swapchain image held past
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

		if (State->OpaquePipeline) {
			SDL_ReleaseGPUGraphicsPipeline(device, State->OpaquePipeline);
		}
		if (State->TransparentPipeline) {
			SDL_ReleaseGPUGraphicsPipeline(device, State->TransparentPipeline);
		}
		if (State->ShadowPipeline) {
			SDL_ReleaseGPUGraphicsPipeline(device, State->ShadowPipeline);
		}
		if (State->ShadowTexture) {
			SDL_ReleaseGPUTexture(device, State->ShadowTexture);
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
		}

		// Anything a resize retired and no frame came along to free. Shutting
		// down is the one path where the next frame never arrives, so leaving
		// this to `DrainRetiredScenes` would leak a texture per resize on exit.
		State->DrainRetiredScenes();

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
		// `*State = Impl{}` no longer compiles — and it should not: an
		// assignment would have released their buffers a second time, after
		// `Shutdown` above already did.
		State = std::make_unique<Impl>();
	}

	bool Renderer::AddMesh(const core::Name &name, const assets::MeshData &mesh) {
		if (State == nullptr || State->Device == nullptr) {
			return false;
		}

		// Uploaded on the spot rather than at the next frame's barrier.
		// Registration happens when content arrives, which a caller has already
		// arranged to be a moment it controls — `delivery::Client::Pump` is the
		// whole design of that — so deferring would add a second barrier for a
		// caller that already had one.
		return State->Meshes.Add(name, mesh) && State->Meshes.Flush();
	}

	bool Renderer::AddTexture(const core::Name &name, const assets::TextureData &image) {
		if (State == nullptr || State->Device == nullptr) {
			return false;
		}
		return State->Textures.Add(name, image);
	}

	void *Renderer::TextureHandle(const core::Name &name) const {
		if (State == nullptr) {
			return nullptr;
		}
		return State->Textures.Find(name);
	}

	bool Renderer::DropTexture(const core::Name &name) {
		if (State == nullptr) {
			return false;
		}
		return State->Textures.Drop(name);
	}

	bool Renderer::HasMesh(const core::Name &name) const {
		return State != nullptr && State->Meshes.Has(name);
	}

	bool Renderer::HasTexture(const core::Name &name) const {
		return State != nullptr && State->Textures.Find(name) != nullptr;
	}

	bool Renderer::Impl::WriteCapture(SDL_GPUTransferBuffer *from, uint32_t width, uint32_t height) const {
		void *mapped = SDL_MapGPUTransferBuffer(Device, from, false);
		if (mapped == nullptr) {
			ENGINE_ERROR("SDL_MapGPUTransferBuffer: {}", SDL_GetError());
			return false;
		}

		// The swapchain's format decides the channel order, and getting it
		// wrong writes a picture with red and blue swapped — which looks like a
		// shader bug rather than like a file-writing bug, so it is worth
		// asking rather than assuming.
		const SDL_GPUTextureFormat format = ColourFormat();

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
			wrote = SDL_SaveBMP(surface, CapturePath.string().c_str());
			if (!wrote) {
				ENGINE_ERROR("SDL_SaveBMP: {}", SDL_GetError());
			}
			SDL_DestroySurface(surface);
		} else {
			// Every other failure here says so, and this one used to return
			// false in silence — a capture that produced no file and no reason
			// reads as the request having been ignored.
			ENGINE_ERROR("capture: SDL_CreateSurfaceFrom: {}", SDL_GetError());
		}

		SDL_UnmapGPUTransferBuffer(Device, from);
		return wrote;
	}

	void Renderer::RequestSceneCapture(std::filesystem::path path) {
		State->CapturePath = std::move(path);
	}

	bool Renderer::IsHeadless() const {
		return State->Headless();
	}

	void *Renderer::SceneTexture(size_t slot) const {
		return slot < State->SceneSlots.size() ? State->SceneSlots[slot].Texture : nullptr;
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
		};
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
		const core::CFrame &cameraFrame,
		const scene::Camera &camera,
		std::span<const scene::DrawInstance> instances,
		OverlayImage &overlay,
		std::span<const SurfaceView> surfaces,
		FrameOverlayHook *interfaceHook,
		const SceneTarget *sceneTarget,
		size_t targetSlot,
		std::span<const ParticleBatch> particles,
		std::span<const effects::RibbonVertex> ribbonVertices,
		std::span<const effects::RibbonRun> ribbonRuns,
		std::span<const SceneLight> lights
	) {
		ENGINE_PROFILE_CAT("Renderer::Render", core::ProfileCategory::Render);

		// **The single-threaded recording contract, checked rather than
		// described.** A studio draws one viewport after another; a second one
		// recording from another thread is the failure this refuses. See
		// `IsOnOwningThread` for why that is the design and not a limitation.
		RequireOwningThread("Render");

		// **Which target this frame draws into, read by `EnsureScene`.** Passed
		// through a member rather than an argument because `EnsureScene` is
		// called from two places and threading a slot through both would put
		// the same value in two signatures that must agree.
		State->ActiveSlot = targetSlot;

		FrameResult result;
		if (!State->Device) {
			return result;
		}

		// **Claimed here only if the caller did not claim it first.** `WaitForFrame`
		// is what a latency-sensitive loop calls before it reads its input; a
		// caller that does not is no worse off than before, because this is the
		// same acquisition at the same point in the frame. See `Impl::BeginFrame`.
		if (!State->BeginFrame()) {
			return result;
		}

		SDL_GPUCommandBuffer *command = nullptr;
		SDL_GPUTexture *swapchain = nullptr;
		uint32_t width = 0;
		uint32_t height = 0;
		State->TakeFrame(command, swapchain, width, height);

		// **Headless has no swapchain, so its size comes from the target** —
		// nothing else has an opinion about it.
		if (State->Headless()) {
			if (sceneTarget == nullptr || !sceneTarget->IsValid()) {
				// A headless renderer with nowhere to draw is a caller mistake
				// rather than a state to tolerate: every pass would run and its
				// result would be discarded.
				SDL_SubmitGPUCommandBuffer(command);
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
			SDL_SubmitGPUCommandBuffer(command);
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
			// beside, and the colour target is the block-rounded allocation —
			// not the rectangle the world is drawn into. Sizing this to the
			// world instead is a validation failure on the frames where the two
			// differ, which is nearly all of them.
			//
			// **The slot's own depth when drawing offscreen.** Two viewports of
			// different sizes sharing one depth texture made every frame
			// reallocate it twice — see `SceneSlot::Depth`.
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
				SDL_SubmitGPUCommandBuffer(command);
				return result;
			}
		}

		// --- uploads --------------------------------------------------------

		const auto totalCount = static_cast<uint32_t>(instances.size());
		bool haveInstances = false;
		bool haveOverlay = false;

		// **Culled, then ordered, then uploaded** — and the sequence is the
		// point. Culling first means the sort runs over what survives rather
		// than over the world, and the upload carries only what is drawn.
		//
		// The frustum comes from the same `ResolveCamera` the draw does, so it
		// cannot disagree with what was actually projected. A frustum built from
		// a field of view and an aspect ratio kept separately is the bug that
		// pops geometry at the screen edge on one machine and not another.

		// **Every surface camera's view, resolved before any pass runs.** Each
		// is used twice: to render into its own texture now, and — one frame
		// later, as `PreviousViewProjection` — to project that texture back onto
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
			// the texture the slot holds — so a surface that turns out to be
			// unchanged, and therefore does not re-render, must not take this
			// frame's matrix. See the refresh decision below.
			glm::mat4 ViewProjection{1.0f};

			float ImageOpacity = 1.0f;

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
			if (claimed[index]) {
				ENGINE_WARN("two surface cameras claim index {}; the second is ignored", view.Index);
				continue;
			}

			claimed[index] = true;

			const float aspect =
				static_cast<float>(view.Width) / static_cast<float>(std::max(view.Height, 1u));

			AcceptedView entry;
			entry.Index = index;
			entry.View = &view;
			entry.ViewProjection = scene::ResolveCamera(view.Frame, view.Lens, aspect).ViewProjection;
			entry.ImageOpacity = std::clamp(view.ImageOpacity, 0.0f, 1.0f);

			accepted[acceptedCount++] = entry;
		}

		size_t visibleCount = 0;

		// **What the light has to see, out of the walk the culler was making
		// anyway.** Deriving an instance's world box is three quaternion
		// rotations and nine abs-mul-adds, and this frame used to pay for it
		// twice over the same span — once to fit the light and once to test the
		// frustum. `graph::CullAndBound` is those two passes fused; the union is
		// six comparisons on a box that already exists. See its comment for why
		// this is not a cache.
		core::AABB sceneBounds;
		{
			ENGINE_PROFILE_CAT("cull instances", core::ProfileCategory::Render);

			const float aspect = static_cast<float>(sceneWidth) / static_cast<float>(sceneHeight);
			const graph::Frustum frustum = graph::Frustum::FromViewProjection(
				scene::ResolveCamera(cameraFrame, camera, aspect).ViewProjection
			);
			visibleCount = graph::CullAndBound(instances, frustum, State->Visible, sceneBounds);
		}

		// **Fitted to the whole draw list, not to what survived culling.** A
		// caster outside the camera's frustum still shadows into it, so the
		// light has to see everything — and fitting to the culled set is the
		// classic version of this bug: shadows that vanish as their casters
		// leave the screen. That is why `sceneBounds` is the union over
		// `instances` and not over `State->Visible`.
		//
		// Built here rather than before the cull only because the cull is now
		// what produces the bound. Nothing between the two reads it, and its
		// first use is the shadow pass.
		const glm::mat4 lightViewProjection = graph::FitDirectionalLight(
			sceneBounds, core::Vector3{SUN_DIRECTION.x, SUN_DIRECTION.y, SUN_DIRECTION.z}
		);

		// Ordered over the survivors, so the two passes are two ranges of one
		// buffer. Opaque first in whatever order the world produced, then the
		// transparent tail back to front from where the camera is.
		State->VisibleInstances.resize(visibleCount);
		for (size_t index = 0; index < visibleCount; index++) {
			State->VisibleInstances[index] = instances[State->Visible[index]];
		}

		size_t opaqueCount = 0;
		{
			ENGINE_PROFILE_CAT("order instances", core::ProfileCategory::Render);
			opaqueCount =
				scene::OrderForDrawing(State->VisibleInstances, cameraFrame.Position, State->DrawOrder);
		}
		const auto transparentCount = static_cast<uint32_t>(visibleCount - opaqueCount);

		// **Surface instances moved to the back of the opaque head**, so the
		// camera range is three contiguous runs — plain opaque, then mirrors,
		// then transparent — and each is one draw with one `first_instance`.
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
			// `scene` "where a headless suite can get at them" — and this file
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
		// per surface — and each has to be contiguous for that to be an offset
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
		// the moment its `Transparency` goes above zero — and the head is where
		// the mirror flag was set, so the reflection did not dim, it vanished.
		// That reads as the surface camera having stopped rather than as an
		// ordering rule, and it is the bug this run exists to fix.
		//
		// They go *last* of everything, so they draw over the blended geometry
		// as well as the opaque. Stable, so the back-to-front sort survives
		// inside each run — see `scene::ScenePlan::TransparentSurfaces` for what
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
		// into it, and a mirror shows what is behind the viewer — so culling to
		// the eye would give shadows that vanish as their casters leave the
		// screen and a mirror that reflects only what is already on screen.
		// Both are the classic version of this mistake.
		//
		// Ordered from the surface camera when there is one, because the surface
		// pass is the only consumer that needs an order at all — a depth-only
		// pass does not care.
		// **Allocated for every accepted view before anything is ordered**, so
		// a view whose texture cannot be made drops out of the frame here rather
		// than half way through the pass loop.
		size_t liveCount = 0;
		for (size_t index = 0; index < acceptedCount; index++) {
			const AcceptedView &view = accepted[index];
			if (State->EnsureSurface(targetSlot, view.Index, view.View->Width, view.View->Height)) {
				accepted[liveCount++] = view;
			}
		}
		acceptedCount = liveCount;

		// **Ordered from the first surface camera when there is one.** The
		// blended sort is per view and there is only one scene range, so several
		// surfaces cannot each have the tail sorted for them — the first is the
		// one that gets it, and every other surface draws that order. Blended
		// geometry inside a reflection of a reflection is therefore sorted for
		// the wrong eye, which is a compositing error confined to the second
		// bounce and cheaper than an ordering pass per surface.
		const bool wantSurface = acceptedCount > 0;
		const core::Vector3 sceneEye = wantSurface ? accepted[0].View->Frame.Position : cameraFrame.Position;

		// **One signature shared by every surface, and that is not a shortcut.**
		// Each camera's matrix is in it because a surface pass draws the *other*
		// mirrors, projecting each one's texture with the camera that rendered
		// it — so a camera that moves changes how it appears inside every other
		// one. Every input to any surface is therefore an input to all of them,
		// and computing several separately would only be several chances for
		// them to disagree.
		//
		// It is still stored per slot rather than once, because slots do not
		// refresh together: one that has never rendered has nothing to compare
		// against, and one that appeared this frame has to draw once before it
		// can be skipped.
		uint64_t surfaceSignature = 0;
		size_t refreshCount = 0;
		if (wantSurface) {
			ENGINE_PROFILE_CAT("surface signature", core::ProfileCategory::Render);

			surfaceSignature = scene::SignatureOf(instances);

			for (size_t index = 0; index < acceptedCount; index++) {
				surfaceSignature = scene::MixSignature(surfaceSignature, accepted[index].Index);
				surfaceSignature = MixMatrix(surfaceSignature, accepted[index].ViewProjection);
				surfaceSignature = MixFloat(surfaceSignature, accepted[index].ImageOpacity);
			}

			for (size_t index = 0; index < acceptedCount; index++) {
				Impl::SurfaceSlotState &state = bank.Surfaces[accepted[index].Index];

				// **Written whether or not the surface renders.** The opacity is
				// what the *screen* pass composites the pane with and it changes
				// no texel of the texture, so a mirror faded by a script fades
				// this frame rather than on whichever later frame something else
				// happens to move.
				state.ImageOpacity = accepted[index].ImageOpacity;

				accepted[index].Refresh = !state.Ready || state.Signature != surfaceSignature;
				refreshCount += accepted[index].Refresh ? 1u : 0u;
			}
		}

		State->SceneInstances.assign(instances.begin(), instances.end());

		// **Every range the three scene passes submit, from one call.** The
		// ordering, the mirror partition and the caster partition are arithmetic
		// over a `shared` type and they live in `scene::OrderScene` — where a
		// headless suite can get at them. A renderer is the one module a test
		// cannot exercise, so the counts it hands to a draw call are the last
		// place they should be computed. See `scene::ScenePlan` for the runs.
		scene::ScenePlan plan;
		{
			ENGINE_PROFILE_CAT("order scene", core::ProfileCategory::Render);
			plan = scene::OrderScene(State->SceneInstances, sceneEye, State->SceneOrder);
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
			// to it, so a frame that redraws nothing still has a panel to show —
			// which is the whole point of the image living on the GPU rather
			// than being pushed there again every frame.
			// **Headless first, because nothing headless can show it.** The
			// overlay pass is the window's, so a headless frame allocated a
			// texture, copied the panels into it and drew none of them — and
			// `MarkUploaded` then told the image the GPU matched it, which was
			// a claim about a texture nothing would ever sample. Now the whole
			// overlay is one question answered once.
			//
			// Safe to skip only because the screen pass no longer borrows this
			// texture when the shadow map is missing; see `FallbackTexture`.
			ENGINE_PROFILE_CAT("ensure overlay", core::ProfileCategory::Render);
			haveOverlay = !State->Headless() && overlay.HasContent() && !overlay.IsEmpty() &&
						  State->EnsureOverlay(overlay.GetWidth(), overlay.GetHeight());
		}

		const auto instanceCount = static_cast<uint32_t>(visibleCount);
		result.Culled = totalCount - instanceCount;

		// **One buffer, two ranges.** The scene range first so its offset is
		// zero and the camera range starts where it ends — which is what makes
		// each pass one `first_instance` rather than a second buffer and a
		// second bind.
		const uint32_t uploadCount = sceneCount + instanceCount;

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
					// staging copy would be that traffic paid a third time —
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
					// mapping exists is here — after this loop the order is a
					// buffer offset and the instance it came from is gone.
					State->SlotMesh.resize(uploadCount);
					State->SlotTexture.resize(uploadCount);
					State->SlotTags.resize(uploadCount);

					const auto record = [&](size_t slot, const scene::DrawInstance &instance) {
						State->SlotMesh[slot] = &State->Meshes.Resolve(instance.Mesh);
						State->SlotTexture[slot] = instance.Texture;
						State->SlotTags[slot] = instance.TagMask;
					};

					for (size_t index = 0; index < State->SceneOrder.size(); index++) {
						const scene::DrawInstance &instance = State->SceneInstances[State->SceneOrder[index]];
						out[index] = ToGpu(instance);
						record(index, instance);
					}

					const size_t cameraBase = State->SceneOrder.size();
					auto *camera = out + cameraBase;
					for (size_t index = 0; index < State->DrawOrder.size(); index++) {
						const scene::DrawInstance &instance =
							State->VisibleInstances[State->DrawOrder[index]];
						camera[index] = ToGpu(instance);
						record(cameraBase + index, instance);
					}
				}
				SDL_UnmapGPUTransferBuffer(State->Device, State->InstanceTransfer);
				haveInstances = true;
			}
		}

		// The particles, packed and grouped before any render pass opens.
		//
		// **Here rather than inside the draw**, because what follows is a copy
		// pass and a copy pass cannot be started while a render pass is open —
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
			particleCount = State->PrepareParticles(particles);
			result.Particles = particleCount;

			ribbonCount = State->PrepareRibbons(ribbonVertices);
			result.RibbonVertices = ribbonCount;
		}

		// Only when something is actually waiting to go across. A panel redrawn
		// ten times a second and presented a thousand times has nothing to
		// upload on nine hundred and ninety of those frames.
		const bool uploadOverlay =
			haveOverlay && (State->OverlayUninitialised || overlay.UploadRegion().Width > 0);

		if (haveInstances || uploadOverlay || particleCount > 0 || ribbonCount > 0) {
			ENGINE_PROFILE_CAT("copy pass", core::ProfileCategory::Render);

			SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(command);

			if (ribbonCount > 0) {
				const SDL_GPUTransferBufferLocation source{State->RibbonTransfer, 0};
				const SDL_GPUBufferRegion destination{
					State->RibbonBuffer,
					0,
					ribbonCount * static_cast<uint32_t>(sizeof(effects::RibbonVertex)),
				};
				SDL_UploadToGPUBuffer(copy, &source, &destination, true);
			}

			if (particleCount > 0) {
				const SDL_GPUTransferBufferLocation source{State->ParticleTransfer, 0};
				const SDL_GPUBufferRegion destination{
					State->ParticleBuffer,
					0,
					particleCount * static_cast<uint32_t>(sizeof(effects::ParticleInstance)),
				};
				// Cycled, for the instance buffer's reason: a fresh allocation
				// rather than a stall on the copy the previous frame may still be
				// reading.
				SDL_UploadToGPUBuffer(copy, &source, &destination, true);
			}

			if (haveInstances) {
				const SDL_GPUTransferBufferLocation source{State->InstanceTransfer, 0};
				const SDL_GPUBufferRegion destination{
					State->InstanceBuffer,
					0,
					uploadCount * static_cast<uint32_t>(sizeof(GpuInstance)),
				};
				// Cycling hands back a fresh allocation rather than stalling on
				// the copy the previous frame may still be reading.
				SDL_UploadToGPUBuffer(copy, &source, &destination, true);
			}

			if (uploadOverlay) {
				// Only the part of the overlay that anything has drawn on.
				//
				// The image is the size of the window and the panels are a
				// corner of it, so sending all of it meant megabytes of
				// transparent pixels per frame that had not changed since the
				// program started — measured as the largest single cost in the
				// frame. The region covers what was drawn this frame and what
				// was drawn last frame, the second being how the pixels a
				// shrinking panel vacates get told they are transparent now.
				ENGINE_PROFILE_CAT("upload overlay", core::ProfileCategory::Render);

				// The whole image on the frame the texture was created, so that
				// the parts no panel ever covers are transparent rather than
				// whatever the driver handed back.
				const auto region = State->OverlayUninitialised
										? OverlayImage::Region{0, 0, overlay.GetWidth(), overlay.GetHeight()}
										: overlay.UploadRegion();
				State->OverlayUninitialised = false;

				const auto rowBytes = static_cast<size_t>(region.Width) * OverlayImage::BYTES_PER_PIXEL;

				void *mapped = nullptr;
				{
					ENGINE_PROFILE_CAT("map overlay", core::ProfileCategory::Render);
					mapped = SDL_MapGPUTransferBuffer(State->Device, State->OverlayTransfer, true);
				}

				{
					// Row by row, because the region is narrower than the image
					// and its rows are not adjacent in it. Packed tightly on the
					// way out, which is what pixels_per_row below promises.
					ENGINE_PROFILE_CAT("copy overlay", core::ProfileCategory::Render);

					auto *destination = static_cast<uint8_t *>(mapped);
					const uint8_t *pixels = overlay.GetPixels();
					const auto stride =
						static_cast<size_t>(overlay.GetWidth()) * OverlayImage::BYTES_PER_PIXEL;

					for (int row = 0; row < region.Height; row++) {
						const size_t offset = static_cast<size_t>(region.Y + row) * stride +
											  static_cast<size_t>(region.X) * OverlayImage::BYTES_PER_PIXEL;
						std::memcpy(
							destination + static_cast<size_t>(row) * rowBytes, pixels + offset, rowBytes
						);
					}
				}

				SDL_UnmapGPUTransferBuffer(State->Device, State->OverlayTransfer);

				SDL_GPUTextureTransferInfo source{};
				source.transfer_buffer = State->OverlayTransfer;
				source.pixels_per_row = static_cast<uint32_t>(region.Width);
				source.rows_per_layer = static_cast<uint32_t>(region.Height);

				SDL_GPUTextureRegion destination{};
				destination.texture = State->OverlayTexture;
				destination.x = static_cast<uint32_t>(region.X);
				destination.y = static_cast<uint32_t>(region.Y);
				destination.w = static_cast<uint32_t>(region.Width);
				destination.h = static_cast<uint32_t>(region.Height);
				destination.d = 1;

				// False, not true. Cycling hands back a *fresh* texture, and a
				// fresh texture is uninitialised everywhere this upload does not
				// reach — which is now everywhere outside the panels.
				SDL_UploadToGPUTexture(copy, &source, &destination, false);

				// The GPU matches the image now, so nothing is pending until
				// something draws again.
				overlay.MarkUploaded();
			}

			SDL_EndGPUCopyPass(copy);
		}

		// The passes below, recorded as they are submitted. See `PassRecorder`.
		PassRecorder passes;

		// --- shadow pass ----------------------------------------------------
		//
		// **The scene range, not the camera's**, and no colour target at all.
		// Every caster casts, whether or not the eye can see it.
		//
		// A scene whose opaque geometry all opted out of casting skips the pass
		// rather than clearing a depth target nothing writes to — and the
		// colour pass then samples a shadow map that was never rendered, which
		// is what `FrameResult::Ran` exists to make visible.
		const bool haveShadow = haveInstances && sceneCount > 0 &&
								(reflectedCasters > 0 || surfaceCasters > 0) && State->EnsureShadow();

		if (haveShadow) {
			ENGINE_PROFILE_CAT("shadow pass", core::ProfileCategory::Render);
			passes.Enter(Pass::Shadow);

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
			SDL_BindGPUGraphicsPipeline(pass, State->ShadowPipeline);

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
			// Two draws because the two runs are not adjacent — the surface
			// partition sits between them. The second is empty in every scene
			// with no mirror in it, which is almost all of them.
			// Depth only, so no samplers and no fragment uniforms — the null
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

		// --- surface pass ----------------------------------------------------
		//
		// The same scene range, from each surface camera, into that surface's
		// own texture. What a mirror shows next frame — the one-frame staleness
		// `ViewChannel` already assumed, and what breaks the dependency cycle
		// between a mirror and what it reflects.
		//
		// **One pass per surface, and each one draws the other surfaces.** A
		// mirror still may not appear in its own reflection: it sits between its
		// camera and the world and would fill the texture with itself. Every
		// *other* mirror is drawn, from the half of its pair this frame is not
		// writing — so what you see in a mirror of a mirror is one frame old per
		// bounce. There is no order that would avoid that, because each surface
		// is being rendered for the others.
		//
		// **Only the surfaces whose signature moved.** A pass that would redraw
		// the texture its slot already holds is not run: its pair keeps the
		// frame it has, its matrices keep describing the camera that drew that
		// frame, and the screen pass samples it exactly as if it had just been
		// rendered. See `SignatureOf` for what counts as a change and, more to
		// the point, what deliberately does not.
		if (wantSurface && haveInstances && sceneCount > 0 && refreshCount > 0) {
			ENGINE_PROFILE_CAT("surface pass", core::ProfileCategory::Render);
			passes.Enter(Pass::Surface);

			// **Flipped for every refreshing surface before the first pass runs,
			// not inside the loop.** A surface pass samples the other surfaces'
			// read slots, so every slot has to have finished flipping before any
			// of them is read — flipping inside the loop would have the second
			// pass sample the first surface's *new* texture, which is this
			// frame's half-drawn image and the exact self-reference the pair
			// exists to make impossible.
			//
			// **A skipped surface does not flip, and its matrices do not move.**
			// Both halves of that are one fact: the slot still holds the frame it
			// held, so `ViewProjection` must still be the camera that drew it and
			// `PreviousViewProjection` the one before. Advancing either for a
			// surface that did not render would project a texture with a camera
			// that never took it — a reflection sliding across a pane that
			// nothing in the scene is moving, which is the hardest possible
			// version of this bug to attribute.
			for (size_t index = 0; index < acceptedCount; index++) {
				if (!accepted[index].Refresh) {
					continue;
				}

				Impl::SurfaceSlotState &state = bank.Surfaces[accepted[index].Index];
				state.PreviousViewProjection = state.ViewProjection;
				state.ViewProjection = accepted[index].ViewProjection;
				state.Slot ^= 1u;
			}

			for (size_t index = 0; index < acceptedCount; index++) {
				if (!accepted[index].Refresh) {
					continue;
				}

				const uint8_t self = accepted[index].Index;

				// **Taken here rather than at each draw**, because `drawMirrors`
				// below shadows `index` with its own loop over surfaces — so
				// `accepted[index]` inside it would name a different view
				// entirely, and the filter would silently be another surface's.
				const uint32_t surfaceFilter = accepted[index].View->TagFilter;
				Impl::SurfaceSlotState &state = bank.Surfaces[self];

				SDL_GPUColorTargetInfo surfaceColour{};
				surfaceColour.texture = state.Texture[state.Slot];
				surfaceColour.clear_color = SDL_FColor{0.05f, 0.06f, 0.09f, 1.0f};
				surfaceColour.load_op = SDL_GPU_LOADOP_CLEAR;
				surfaceColour.store_op = SDL_GPU_STOREOP_STORE;
				surfaceColour.cycle = true;

				SDL_GPUDepthStencilTargetInfo surfaceDepth{};
				surfaceDepth.texture = state.Depth;
				surfaceDepth.clear_depth = 1.0f;
				surfaceDepth.load_op = SDL_GPU_LOADOP_CLEAR;
				surfaceDepth.store_op = SDL_GPU_STOREOP_DONT_CARE;
				surfaceDepth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
				surfaceDepth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
				surfaceDepth.cycle = true;

				SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(command, &surfaceColour, 1, &surfaceDepth);
				// **The light set, pushed once for the whole pass.** Uniform state
				// on a command buffer persists until it is replaced, so one push
				// before the draws serves every one of them — which is the whole
				// reason this is a second buffer rather than fields on the
				// per-draw `LightingUniforms`.
				SDL_PushGPUFragmentUniformData(command, 1, &lightUniforms, sizeof(lightUniforms));
				SDL_BindGPUGraphicsPipeline(pass, State->OpaquePipeline);

				const SDL_GPUBufferBinding vertexBindings[] = {
					{State->Meshes.Vertices(), 0},
					{State->InstanceBuffer, 0},
				};
				SDL_BindGPUVertexBuffers(pass, 0, vertexBindings, 2);

				const SDL_GPUBufferBinding indexBinding{State->Meshes.Indices(), 0};
				SDL_BindGPUIndexBuffer(pass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);

				// **Shadowed, and pointedly not surfaced.** The mirror's own view
				// gets the shadow map, so what it reflects is lit the way the
				// screen lights it.
				//
				// **`Flags.z` is zero for the world, and it has to be.** It means
				// "this draw samples a surface texture instead of its own tint",
				// and it is set below for exactly the mirror runs. Setting it for
				// the whole pass is what made the floor sample the previous
				// frame's reflection and come out as the clear colour wherever
				// that projection landed on untouched texels — a black wedge in
				// the mirror that survived deleting every caster, the frame and
				// the near-plane hack, and moved when the camera was re-aimed but
				// not when the floor was.
				const LightingUniforms surfaceLighting{
					glm::vec4{SUN_DIRECTION, 0.0f},
					SUN_AMBIENT,
					glm::vec4{
						haveShadow ? 1.0f : 0.0f, 1.0f / static_cast<float>(SHADOW_RESOLUTION), 0.0f, 1.0f
					},
				};

				SDL_GPUTexture *const fallback =
					State->ShadowTexture != nullptr ? State->ShadowTexture : State->FallbackTexture;
				SDL_GPUSampler *const shadowSampler =
					State->ShadowSampler != nullptr ? State->ShadowSampler : State->SurfaceSampler;

				// **The samplers are bound per draw now rather than per run**,
				// because the third one — the colour map — changes with the
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

				// The world, minus every mirror. `plan.Reflected` is the
				// non-mirror opaque run.
				if (sceneReflected > 0) {
					result.DrawCalls += State->DrawSlots(
						command,
						pass,
						0,
						sceneReflected,
						&surfaceLighting,
						fallback,
						shadowSampler,
						surfaceTexture,
						State->SurfaceSampler,
						surfaceFilter,
						result.Triangles
					);
				}

				// **Every mirror except this one, one draw each.** `self` is
				// skipped because nothing sees itself in its own reflection —
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
				// texture being read — `PreviousViewProjection`, not the one
				// just resolved — because the image is a frame old and
				// projecting it with a fresh camera slides it across the pane.
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

						const Impl::SurfaceSlotState &shown = bank.Surfaces[index];
						if (!shown.Ready || !claimed[index]) {
							plainly();
							result.DrawCalls += State->DrawSlots(
								command,
								pass,
								first,
								count,
								&surfaceLighting,
								fallback,
								shadowSampler,
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
							shown.PreviousViewProjection,
						};
						const LightingUniforms mirrorLighting{
							glm::vec4{SUN_DIRECTION, 0.0f},
							SUN_AMBIENT,
							glm::vec4{
								haveShadow ? 1.0f : 0.0f,
								1.0f / static_cast<float>(SHADOW_RESOLUTION),
								1.0f,
								shown.ImageOpacity
							},
						};

						SDL_PushGPUVertexUniformData(command, 0, &mirrorFrame, sizeof(mirrorFrame));
						bindSurface(shown.Texture[shown.Slot ^ 1u]);

						result.DrawCalls += State->DrawSlots(
							command,
							pass,
							first,
							count,
							&mirrorLighting,
							fallback,
							shadowSampler,
							surfaceTexture,
							State->SurfaceSampler,
							surfaceFilter,
							result.Triangles
						);
					}
				};

				drawMirrors(false);

				// **The blended tail *minus* the mirrors in it.** The opaque head
				// already excludes them for the reason above; a pane that went
				// transparent moved from the head to the tail and stopped being
				// excluded, so a faded mirror reflected itself.
				const uint32_t blendedPlain = sceneTransparent - plan.TransparentSurfaces;
				if (blendedPlain > 0 || plan.TransparentSurfaces > 0) {
					SDL_BindGPUGraphicsPipeline(pass, State->TransparentPipeline);
				}

				if (blendedPlain > 0) {
					plainly();
					result.DrawCalls += State->DrawSlots(
						command,
						pass,
						static_cast<uint32_t>(sceneOpaque),
						blendedPlain,
						&surfaceLighting,
						fallback,
						shadowSampler,
						surfaceTexture,
						State->SurfaceSampler,
						surfaceFilter,
						result.Triangles
					);
				}

				// And the blended mirrors that are not this one, last of
				// everything drawn into this texture.
				if (plan.TransparentSurfaces > 0) {
					drawMirrors(true);
				}

				SDL_EndGPURenderPass(pass);
			}

			// **Marked ready only after every pass has run**, so a surface that
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
				result.SurfacePasses++;
			}
		}

		// --- the interface's uploads ----------------------------------------
		//
		// **Here, and not beside the pass that draws them.** Dear ImGui's
		// backend copies its vertex and index buffers through a copy pass, and
		// SDL refuses to open one while a render pass is in flight — so an
		// upload issued from `Record` works right up until the first frame with
		// enough widgets to grow a buffer, which is a bug that arrives months
		// after the code that caused it. The split is `FrameOverlayHook`'s
		// contract for exactly that reason.
		const bool drawInterface =
			!State->Headless() && interfaceHook != nullptr && interfaceHook->Prepare(command);

		// --- opaque pass ----------------------------------------------------

		// **The world's target, which is the offscreen texture or the window.**
		// Everything after this pass draws onto the *window* regardless — the
		// debug overlay is in window pixels and the editor's chrome is the
		// window — so this is the one target that moves.
		SDL_GPUColorTargetInfo colourTarget{};
		colourTarget.texture = offscreen ? State->SlotAt(targetSlot).Texture : swapchain;
		colourTarget.clear_color = SDL_FColor{0.05f, 0.06f, 0.09f, 1.0f};
		colourTarget.load_op = SDL_GPU_LOADOP_CLEAR;
		colourTarget.store_op = SDL_GPU_STOREOP_STORE;

		// What the overlay and the interface draw onto. When the world went
		// offscreen the window has never been touched this frame, so the first
		// pass to reach it clears — otherwise it is whatever the driver handed
		// back, which is last frame's image or uninitialised memory.
		SDL_GPUColorTargetInfo windowTarget{};
		windowTarget.texture = swapchain;
		windowTarget.clear_color = SDL_FColor{0.05f, 0.06f, 0.09f, 1.0f};
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

		{
			ENGINE_PROFILE_CAT("opaque pass", core::ProfileCategory::Render);

			// Entered unconditionally, and that is the honest reading rather
			// than a convenience: the stage clears colour and depth, so a frame
			// with nothing in it still ran this pass — the background is what it
			// drew. `Validate` sees the same thing, because the stage's writes
			// are marked `Clear`.
			passes.Enter(Pass::Opaque);

			SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(command, &colourTarget, 1, &depthTarget);
			// **The light set, pushed once for the whole pass.** Uniform state
			// on a command buffer persists until it is replaced, so one push
			// before the draws serves every one of them — which is the whole
			// reason this is a second buffer rather than fields on the
			// per-draw `LightingUniforms`.
			SDL_PushGPUFragmentUniformData(command, 1, &lightUniforms, sizeof(lightUniforms));

			// **The world's rectangle inside an attachment that is larger than
			// it.** Without this the pass inherits a viewport covering the whole
			// texture, and a block-rounded target would draw the world into
			// 1600x960 while the panel shows the 1600x900 corner — the image
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
				SDL_BindGPUGraphicsPipeline(pass, State->OpaquePipeline);

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
				// clip depth or the order of the product — a disagreement that
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
					scene::ResolveCamera(cameraFrame, camera, aspect).ViewProjection;

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
				const LightingUniforms lighting{
					glm::vec4{SUN_DIRECTION, 0.0f},
					SUN_AMBIENT,
					glm::vec4{
						haveShadow ? 1.0f : 0.0f, 1.0f / static_cast<float>(SHADOW_RESOLUTION), 0.0f, 0.0f
					},
				};
				SDL_PushGPUFragmentUniformData(command, 0, &lighting, sizeof(lighting));

				// The same, for the runs that sample the surface texture.
				//
				// **Declared beside the ordinary one because two runs push it
				// now**, not one: the opaque mirrors and, since a faded mirror
				// stopped vanishing, the blended ones at the very end of the
				// tail. `Flags.w` is the image's own opacity and was unused
				// until a mirror had to be legible on a pane that is itself
				// transparent — see `SurfaceView::ImageOpacity`.
				// Both samplers, every draw. A shadow map that was not rendered
				// binds another texture in its place rather than nothing: the
				// flag above is what stops it being read, and an unbound sampler
				// is undefined behaviour on several backends where a wrongly
				// bound one is merely ignored.
				//
				// **`FallbackTexture` rather than `OverlayTexture`**, which only
				// exists while a debug panel has something in it. A scene of
				// nothing but transparent geometry casts nothing, so the shadow
				// map is absent too — and with the panels closed both were null
				// and the guard below skipped the bind and drew anyway. See
				// `Impl::FallbackTexture`.
				SDL_GPUTexture *const shadow =
					State->ShadowTexture != nullptr ? State->ShadowTexture : State->FallbackTexture;
				SDL_GPUSampler *const shadowSampler =
					State->ShadowSampler != nullptr ? State->ShadowSampler : State->OverlaySampler;
				SDL_GPUSampler *const surfaceSampler =
					State->SurfaceSampler != nullptr ? State->SurfaceSampler : shadowSampler;

				// Remembered rather than bound, for the surface pass's reason:
				// `DrawSlots` binds all three samplers together because the
				// third changes per mesh.
				SDL_GPUTexture *screenSurface = nullptr;
				const auto bindScreen = [&](SDL_GPUTexture *texture) { screenSurface = texture; };

				bindScreen(nullptr);

				// **Two draws over one buffer, split at the boundary the
				// ordering produced.** `first_instance` is what makes that
				// possible without a second upload: the instance attributes are
				// per-instance vertex data, so the offset picks up where the
				// opaque range left off.
				if (plainOpaque > 0) {
					result.DrawCalls += State->DrawSlots(
						command,
						pass,
						sceneCount,
						plainOpaque,
						&lighting,
						shadow,
						shadowSampler,
						screenSurface,
						surfaceSampler,
						0,
						result.Triangles
					);
				}

				// **The surface draws, one per index rather than one for "the
				// mirrors".** Each index owns a texture and a projection, so
				// what used to be a single run is a run each — grouped by
				// `scene::GroupSurfaces` into `cameraRuns` so every one of them
				// is still an offset and a count rather than a per-instance
				// branch.
				//
				// **This frame's texture, not the previous one.** The surface
				// passes have already run, so the screen shows a reflection that
				// is current. Only what a mirror sees *of another mirror* is a
				// frame behind, and that is the staleness the pair exists for.
				LightingUniforms mirroredUniforms{};
				const auto drawScreenMirrors = [&](bool blended) {
					for (uint8_t index = 0; index < scene::MAX_SURFACES; index++) {
						const scene::SurfaceRun &run = cameraRuns[index];
						const uint32_t count = blended ? run.BlendedCount : run.OpaqueCount;
						const uint32_t first = blended ? run.BlendedFirst : run.OpaqueFirst;
						if (count == 0) {
							continue;
						}

						const Impl::SurfaceSlotState &shown = bank.Surfaces[index];

						// Its own tint until the surface has a frame, and for a
						// pane naming an index nothing renders. Skipping it
						// instead would leave a hole in the geometry, which reads
						// as a culling bug rather than as a mirror that has not
						// warmed up.
						const LightingUniforms *uniforms = &lighting;
						if (!shown.Ready) {
							bindScreen(nullptr);
						} else {
							const FrameUniforms mirrorFrame{
								viewProjection,
								lightViewProjection,
								shown.ViewProjection,
							};
							const LightingUniforms mirrored{
								glm::vec4{SUN_DIRECTION, 0.0f},
								SUN_AMBIENT,
								glm::vec4{
									haveShadow ? 1.0f : 0.0f,
									1.0f / static_cast<float>(SHADOW_RESOLUTION),
									1.0f,
									shown.ImageOpacity
								},
							};

							SDL_PushGPUVertexUniformData(command, 0, &mirrorFrame, sizeof(mirrorFrame));
							bindScreen(shown.Texture[shown.Slot]);

							// Held rather than pushed: `DrawSlots` pushes the
							// fragment uniforms itself, because it has to write
							// the submesh's colour into them.
							mirroredUniforms = mirrored;
							uniforms = &mirroredUniforms;

							result.SurfaceInstances += count;
						}

						result.DrawCalls += State->DrawSlots(
							command,
							pass,
							sceneCount + first,
							count,
							uniforms,
							shadow,
							shadowSampler,
							screenSurface,
							surfaceSampler,
							0,
							result.Triangles
						);

						// Back to the ordinary uniforms, so the next draw does
						// not inherit this mirror's projection.
						SDL_PushGPUVertexUniformData(command, 0, &frameUniforms, sizeof(frameUniforms));
					}
				};

				if (surfaceInCamera > 0) {
					drawScreenMirrors(false);
					bindScreen(nullptr);
				}

				if (transparentCount > 0) {
					// Same pass, same depth attachment, different pipeline —
					// blending on and depth writes off. A separate render pass
					// would have to reload the depth buffer, and the whole point
					// is that these fragments are tested against what the opaque
					// pass already wrote.
					//
					// Still its own stage, sharing a render pass. What the list
					// describes is what is drawn and in what order, not how many
					// times a target is bound.
					passes.Enter(Pass::Transparent);

					SDL_BindGPUGraphicsPipeline(pass, State->TransparentPipeline);

					if (plainTransparent > 0) {
						result.DrawCalls += State->DrawSlots(
							command,
							pass,
							sceneCount + static_cast<uint32_t>(opaqueCount),
							plainTransparent,
							&lighting,
							shadow,
							shadowSampler,
							screenSurface,
							surfaceSampler,
							0,
							result.Triangles
						);
					}

					// **The blended mirrors, last of everything.** Same pipeline
					// and same sort, different uniforms: these runs are the ones
					// that sample a surface texture, so a pane at any
					// transparency still shows its reflection at the image's own
					// opacity.
					if (transparentSurfaces > 0) {
						drawScreenMirrors(true);
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
				// **Not their own `Pass` member**, deliberately. `render::Pass` is
				// compared against `graph::StandardPipeline` by a test, and adding
				// a stage to one and not the other is exactly the drift D00016
				// records. Particles are part of what `Transparent` means until
				// the pipeline is a graph rather than a function body.
				if (particleCount > 0) {
					passes.Enter(Pass::Transparent);
					result.DrawCalls += State->DrawParticles(
						command, pass, frameUniforms.ViewProjection, cameraFrame, particles
					);
				}

				// The beams and trails, after the particles. See the header for
				// why the order is fixed rather than sorted.
				if (ribbonCount > 0) {
					passes.Enter(Pass::Transparent);
					result.DrawCalls += State->DrawRibbons(
						command, pass, frameUniforms.ViewProjection, cameraFrame, ribbonRuns
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
		}

		// --- overlay pass ---------------------------------------------------

		// `haveOverlay` already requires a window, so there is no second test.
		if (haveOverlay) {
			ENGINE_PROFILE_CAT("overlay pass", core::ProfileCategory::Render);
			passes.Enter(Pass::Overlay);

			SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(command, &windowTarget, 1, nullptr);

			// Whoever got here first has cleared it; everything after loads.
			windowTarget.load_op = SDL_GPU_LOADOP_LOAD;

			SDL_BindGPUGraphicsPipeline(pass, State->OverlayPipeline);

			const SDL_GPUTextureSamplerBinding binding{
				State->OverlayTexture,
				State->OverlaySampler,
			};
			SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);

			// Three vertices, no buffer: the vertex shader builds a fullscreen
			// triangle from gl_VertexIndex.
			SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
			SDL_EndGPURenderPass(pass);

			result.DrawCalls++;
		}

		// --- interface pass -------------------------------------------------

		// `drawInterface` already requires a window — the hook is not asked to
		// prepare anything headless — so there is no second test here.
		if (drawInterface) {
			ENGINE_PROFILE_CAT("interface pass", core::ProfileCategory::Render);
			passes.Enter(Pass::Interface);

			// No depth attachment. Panels are drawn in the order the interface
			// submitted them and testing them against the world's depth would
			// hide a window behind a wall.
			SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(command, &windowTarget, 1, nullptr);
			windowTarget.load_op = SDL_GPU_LOADOP_LOAD;
			interfaceHook->Record(command, pass);
			SDL_EndGPURenderPass(pass);

			// One, whatever the interface submitted. What this counter is for is
			// the frame's shape — the panel that reads it is trying to answer
			// "what did the engine draw", and a widget count is the editor's
			// business rather than the renderer's.
			result.DrawCalls++;
		}

		// --- the capture ----------------------------------------------------
		//
		// After the world's passes and before the window's, because what is
		// wanted is the scene as it was drawn rather than the scene with the
		// editor's panels over it. A copy pass, so it cannot be inside one of
		// the render passes above.
		SDL_GPUTransferBuffer *capture = nullptr;
		if (offscreen && !State->CapturePath.empty()) {
			SDL_GPUTransferBufferCreateInfo info{};
			info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
			info.size = sceneWidth * sceneHeight * 4;

			capture = SDL_CreateGPUTransferBuffer(State->Device, &info);
			if (capture != nullptr) {
				SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(command);

				SDL_GPUTextureRegion source{};
				source.texture = State->SlotAt(targetSlot).Texture;
				source.w = sceneWidth;
				source.h = sceneHeight;
				source.d = 1;

				SDL_GPUTextureTransferInfo destination{};
				destination.transfer_buffer = capture;
				destination.pixels_per_row = sceneWidth;
				destination.rows_per_layer = sceneHeight;

				SDL_DownloadFromGPUTexture(copy, &source, &destination);
				SDL_EndGPUCopyPass(copy);
			} else {
				ENGINE_ERROR("capture: SDL_CreateGPUTransferBuffer: {}", SDL_GetError());
			}
		}

		// **The window, when nothing else touched it.** With the world drawn
		// offscreen and neither the overlay nor the interface open, no pass has
		// reached the swapchain — and presenting a texture the driver handed
		// back without writing to it shows last frame's image or uninitialised
		// memory. One clear costs nothing and removes the whole case.
		if (!State->Headless() && windowTarget.load_op == SDL_GPU_LOADOP_CLEAR) {
			SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(command, &windowTarget, 1, nullptr);
			SDL_EndGPURenderPass(pass);
		}

		// Before the submit rather than after it, so a frame that fails to
		// submit still reports what it built.
		result.Passes = passes.Ran;

		{
			// Hands the whole buffer over and queues the present. The passes
			// above only *record* commands, so almost nothing that happens in
			// them is measured by their spans — this is where the driver gets
			// the work, and where any cost of building it lands.
			ENGINE_PROFILE_CAT("submit", core::ProfileCategory::Render);

			if (capture != nullptr) {
				// **A fence, and the stall is the point.** The pixels are not
				// there until the GPU has run the copy, so a capture has to wait
				// for it. That is a frame's worth of latency on the frames a
				// caller asked to capture and on no others.
				SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(command);
				if (fence == nullptr) {
					ENGINE_ERROR("SDL_SubmitGPUCommandBufferAndAcquireFence: {}", SDL_GetError());
					SDL_ReleaseGPUTransferBuffer(State->Device, capture);
					return result;
				}

				SDL_WaitForGPUFences(State->Device, true, &fence, 1);
				SDL_ReleaseGPUFence(State->Device, fence);

				if (State->WriteCapture(capture, sceneWidth, sceneHeight)) {
					ENGINE_INFO(
						"captured {} x {} to {}", sceneWidth, sceneHeight, State->CapturePath.string()
					);
				}

				SDL_ReleaseGPUTransferBuffer(State->Device, capture);

				// Once. A request that repeated would write a file every frame
				// and stall every one of them.
				State->CapturePath.clear();
			} else if (!SDL_SubmitGPUCommandBuffer(command)) {
				ENGINE_ERROR("SDL_SubmitGPUCommandBuffer: {}", SDL_GetError());
				return result;
			}
		}

		// **Not presented, because there is nowhere to present to.** A caller
		// counting presented frames gets zero from a headless renderer, which is
		// the honest answer — what it should count instead is captures, or its
		// own loop.
		result.Presented = !State->Headless();
		return result;
	}
}
