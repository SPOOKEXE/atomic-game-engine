// The particle pools and the ribbon stream.
//
// **Each world's pool lives on the device and is stepped there.** `particle-step.comp`
// writes the instance stream on its own submission before the frame's command
// buffer is recorded, which is why the transparent node uploads no particle
// data at all - see the note in `ViewRecording::RecordUploads`.

#include "DisplayColour.hpp"
#include "RenderTypes.hpp"
#include "RendererState.hpp"
#include "VulkanTimestamps.hpp"

#include <engine/core/Log.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/effects/Particles.hpp>

#include <SDL3/SDL_gpu.h>
#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <vector>

namespace engine::render {

	namespace {
		// --- the device layouts -----------------------------------------------
		//
		// **These numbers are also written in `particle-step.comp` and
		// `particle-scatter.comp`, and a disagreement between the two is a scene of
		// garbage rather than a compile error.** `vec3` takes sixteen-byte
		// alignment in std430 and none of these structs is laid out that way, so
		// both sides index a flat `uint` array by hand - the same arrangement
		// `occlusion-args.comp` has for its five-word draw command. The
		// `static_assert`s below are what makes a change to the C++ side fail to
		// build instead of failing to look right.
		constexpr uint32_t PARTICLE_STATE_WORDS = 15;
		constexpr uint32_t PARTICLE_BIRTH_WORDS = PARTICLE_STATE_WORDS + 1;
		constexpr uint32_t PARTICLE_SEAM_WORDS = 20;

		// The block's parameters, in a table indexed by block. The six tail words
		// carry the optional force modules without widening a particle row.
		constexpr uint32_t PARTICLE_PARAM_WORDS = 24;
		constexpr uint32_t PARTICLE_PARAM_ROTATION = 0;
		constexpr uint32_t PARTICLE_PARAM_POSITION = 4;
		constexpr uint32_t PARTICLE_PARAM_ACCELERATION = 7;
		constexpr uint32_t PARTICLE_PARAM_DRAG = 10;
		constexpr uint32_t PARTICLE_PARAM_FIRST = 11;
		constexpr uint32_t PARTICLE_PARAM_CAPACITY = 12;
		constexpr uint32_t PARTICLE_PARAM_FLAGS = 13;
		constexpr uint32_t PARTICLE_PARAM_CELLS = 14;
		constexpr uint32_t PARTICLE_PARAM_PLAYBACK = 15;
		constexpr uint32_t PARTICLE_PARAM_RATE = 16;
		constexpr uint32_t PARTICLE_PARAM_GENERATION = 17;
		constexpr uint32_t PARTICLE_PARAM_MAX_SPEED = 18;
		constexpr uint32_t PARTICLE_PARAM_NOISE_STRENGTH = 19;
		constexpr uint32_t PARTICLE_PARAM_NOISE_FREQUENCY = 20;
		constexpr uint32_t PARTICLE_PARAM_NOISE_SCROLL = 21;
		constexpr uint32_t PARTICLE_PARAM_RADIAL = 22;
		constexpr uint32_t PARTICLE_PARAM_TANGENTIAL = 23;

		// The four curves, in a second table indexed by block. Words rather than
		// floats because the colour curve is packed RGB and the other three are
		// floats; one table with one stride beats two of each.
		constexpr uint32_t PARTICLE_CURVE_WORDS = 64;
		constexpr uint32_t PARTICLE_CURVE_OF_SIZE = 0;
		constexpr uint32_t PARTICLE_CURVE_OF_ALPHA = 16;
		constexpr uint32_t PARTICLE_CURVE_OF_SQUASH = 32;
		constexpr uint32_t PARTICLE_CURVE_OF_COLOUR = 48;

		// One entry of the per-frame draw list: which block, and where its run of
		// the instance stream starts.
		//
		// **The only thing that crosses per emitter per frame, and getting it down
		// to two words is what makes the device-resident pool a win at the scale
		// it exists for.** The first version staged the whole three-hundred-and-
		// eighty-four-byte record for every emitter every frame: at the roadmap's
		// hundred thousand emitters that is thirty-nine megabytes of writes into a
		// mapped transfer buffer, *more* than the sixteen the entire pool cost
		// before it moved to the device. Measured at 102,400 emitters,
		// `prepare particles` was 2.81 ms of a 12.4 ms frame.
		//
		// Almost none of a record changes between frames. `EmitterBlock::Revision`
		// says when it has, `ParticlePool::ParamRevision` is what the device was
		// last told, and `particle-scatter.comp` sends the difference - nothing at
		// all for a static scene. What is left is this, because the sort that
		// decides where a block draws is genuinely per frame.
		constexpr uint32_t PARTICLE_DRAW_WORDS = 2;

		// How many block tables may be brought up to date in one frame.
		//
		// **A fixed budget rather than "however many changed", and the first
		// version without one was unusable.** The staging buffer is mapped with
		// cycling every frame, so its *size* is what a frame costs whether or not
		// anything is in it - and sizing it to the emitter count made it thirty-
		// four megabytes at a hundred thousand emitters, re-versioned sixty times
		// a second. That is the allocator, not the bus: 102,400 emitters ran at
		// **4 fps** and the process held hundreds of megabytes of transfer buffer
		// versions waiting on fences.
		//
		// Two thousand rows is half a megabyte, which is nothing to re-version.
		// In a settled scene almost nothing changes and the budget is never
		// reached; the one time it is, is the frame a scene loads, and a block
		// whose turn has not come is simply left out of the draw list until it
		// has - so a hundred thousand emitters fade in over a second rather than
		// drawing from a table that does not describe them yet.
		constexpr uint32_t PARTICLE_UPDATE_BUDGET = 2048;

		static_assert(
			sizeof(effects::ParticleState) == PARTICLE_STATE_WORDS * sizeof(uint32_t),
			"the state's width is the shader's stride"
		);
		static_assert(effects::CURVE_SAMPLES == 16, "the curve table reserves sixteen words a curve");
		static_assert(
			PARTICLE_CURVE_OF_COLOUR + effects::CURVE_SAMPLES <= PARTICLE_CURVE_WORDS,
			"the four curves must fit inside one row of the table"
		);
		static_assert(
			PARTICLE_PARAM_TANGENTIAL < PARTICLE_PARAM_WORDS,
			"the parameters must fit inside one row of the table"
		);

		// One float, into a word of a record.
		void PutFloat(uint32_t *words, uint32_t at, float value) {
			std::memcpy(words + at, &value, sizeof(float));
		}

		void PutVector(uint32_t *words, uint32_t at, const core::Vector3 &value) {
			PutFloat(words, at, value.X);
			PutFloat(words, at + 1, value.Y);
			PutFloat(words, at + 2, value.Z);
		}

		// Fills one row of the parameter table.
		void WriteParticleParams(uint32_t *words, const effects::EmitterBlock &block) {
			const glm::quat turn = block.Frame.Rotation();
			PutFloat(words, PARTICLE_PARAM_ROTATION, turn.x);
			PutFloat(words, PARTICLE_PARAM_ROTATION + 1, turn.y);
			PutFloat(words, PARTICLE_PARAM_ROTATION + 2, turn.z);
			PutFloat(words, PARTICLE_PARAM_ROTATION + 3, turn.w);
			PutVector(words, PARTICLE_PARAM_POSITION, block.Frame.Position);
			PutVector(words, PARTICLE_PARAM_ACCELERATION, block.Acceleration);
			PutFloat(words, PARTICLE_PARAM_DRAG, block.Drag);

			words[PARTICLE_PARAM_FIRST] = block.First;
			words[PARTICLE_PARAM_CAPACITY] = block.Capacity;
			words[PARTICLE_PARAM_GENERATION] = block.Generation;

			// **The random flipbook mode is decided at spawn and not here**, and
			// that is what keeps a sixty-four-bit hash out of a shader with no
			// sixty-four-bit integers: the mode picks a cell once and keeps it for
			// the particle's whole life, so the host writes it into the state's
			// rotation word and the step is told to leave the cell alone. Every
			// other mode is a function of age and the shader works it out.
			const uint32_t cells = std::min<uint32_t>(block.Frames, effects::FlipbookCells(block.Flipbook));
			const bool fixed = block.FlipbookPlayback == effects::FlipbookMode::Random;
			words[PARTICLE_PARAM_FLAGS] = (block.Locked ? 1u : 0u) | (fixed ? 2u : 0u);
			words[PARTICLE_PARAM_CELLS] = cells;
			words[PARTICLE_PARAM_PLAYBACK] = static_cast<uint32_t>(block.FlipbookPlayback);
			PutFloat(words, PARTICLE_PARAM_RATE, block.FlipbookRate);
			PutFloat(words, PARTICLE_PARAM_MAX_SPEED, block.MaxSpeed);
			PutFloat(words, PARTICLE_PARAM_NOISE_STRENGTH, block.NoiseStrength);
			PutFloat(words, PARTICLE_PARAM_NOISE_FREQUENCY, block.NoiseFrequency);
			PutFloat(words, PARTICLE_PARAM_NOISE_SCROLL, block.NoiseScrollSpeed);
			PutFloat(words, PARTICLE_PARAM_RADIAL, block.RadialAcceleration);
			PutFloat(words, PARTICLE_PARAM_TANGENTIAL, block.TangentialAcceleration);
		}

		// Fills one row of the curve table.
		void WriteParticleCurves(uint32_t *words, const effects::ParticleCurves &curves) {
			for (uint32_t at = 0; at < effects::CURVE_SAMPLES; at++) {
				PutFloat(words, PARTICLE_CURVE_OF_SIZE + at, curves.Size[at]);
				PutFloat(words, PARTICLE_CURVE_OF_ALPHA + at, curves.Alpha[at]);
				PutFloat(words, PARTICLE_CURVE_OF_SQUASH + at, curves.Squash[at]);
				words[PARTICLE_CURVE_OF_COLOUR + at] = curves.Colour[at];
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
		if (ActiveParticleWorld == nullptr) {
			return false;
		}
		ParticleWorld &world = *ActiveParticleWorld;
		if (count <= world.Capacity) {
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
		uint32_t capacity = world.Capacity == 0 ? 4096 : world.Capacity;
		while (capacity < count) {
			capacity *= 2;
		}

		if (world.Buffer != nullptr) {
			gpu::ReleaseBuffer(Device, world.Buffer);
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
		world.Buffer = gpu::CreateBuffer(Device, &bufferInfo);

		if (world.Buffer == nullptr) {
			ENGINE_ERROR("particle buffer of {} entries: {}", capacity, SDL_GetError());
			world.Capacity = 0;
			return false;
		}

		world.Capacity = capacity;
		return true;
	}

	bool Renderer::Impl::ReserveParticlePool(uint32_t slots) {
		ParticlePool &Particles = ActiveParticleWorld->Pool;
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
			gpu::ReleaseBuffer(Device, Particles.States);
		}

		SDL_GPUBufferCreateInfo info{};
		info.usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE;
		info.size = slots * static_cast<uint32_t>(sizeof(effects::ParticleState));
		Particles.States = gpu::CreateBuffer(Device, &info);
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
		SDL_GPUTransferBuffer *zeros = gpu::CreateTransferBuffer(Device, &blank);
		SDL_GPUCommandBuffer *command = zeros != nullptr ? SDL_AcquireGPUCommandBuffer(Device) : nullptr;
		if (zeros == nullptr || command == nullptr) {
			ENGINE_ERROR("particle state pool: could not clear {} bytes: {}", info.size, SDL_GetError());
			if (zeros != nullptr) {
				gpu::ReleaseTransferBuffer(Device, zeros);
			}
			gpu::ReleaseBuffer(Device, Particles.States);
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
		gpu::ReleaseTransferBuffer(Device, zeros);

		Particles.Slots = slots;
		return true;
	}

	bool Renderer::Impl::ReserveParticleTables(uint32_t blocks) {
		ParticlePool &Particles = ActiveParticleWorld->Pool;
		if (blocks <= Particles.TableRows) {
			return true;
		}

		// **Grown and never shrunk, and what is in them is kept.** These are what
		// the device knows about every block, written only where the host says it
		// has changed - so a grow that dropped the contents would leave every
		// unchanged block describing whatever was in that memory. The revisions
		// are reset alongside, which is what makes the next frame re-send
		// everything rather than trusting a buffer that no longer holds it.
		uint32_t rows = Particles.TableRows == 0 ? 1024 : Particles.TableRows;
		while (rows < blocks) {
			rows *= 2;
		}

		for (SDL_GPUBuffer **buffer : {&Particles.Params, &Particles.Curves}) {
			if (*buffer != nullptr) {
				gpu::ReleaseBuffer(Device, *buffer);
				*buffer = nullptr;
			}
		}

		SDL_GPUBufferCreateInfo info{};
		info.usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE;

		info.size = rows * PARTICLE_PARAM_WORDS * static_cast<uint32_t>(sizeof(uint32_t));
		Particles.Params = gpu::CreateBuffer(Device, &info);

		info.size = rows * PARTICLE_CURVE_WORDS * static_cast<uint32_t>(sizeof(uint32_t));
		Particles.Curves = gpu::CreateBuffer(Device, &info);

		if (Particles.Params == nullptr || Particles.Curves == nullptr) {
			ENGINE_ERROR("particle tables of {} blocks: {}", rows, SDL_GetError());
			Particles.TableRows = 0;
			return false;
		}

		Particles.TableRows = rows;
		Particles.ParamRevision.assign(rows, 0);
		Particles.CurveRevision.assign(rows, 0);
		return true;
	}

	bool Renderer::Impl::ReserveParticleStaging(uint32_t draws, uint32_t births, uint32_t seams) {
		ParticlePool &Particles = ActiveParticleWorld->Pool;
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
				gpu::ReleaseBuffer(Device, buffer);
			}
			if (staging != nullptr) {
				gpu::ReleaseTransferBuffer(Device, staging);
			}

			SDL_GPUBufferCreateInfo info{};
			info.usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ;
			info.size = size * stride;
			buffer = gpu::CreateBuffer(Device, &info);

			SDL_GPUTransferBufferCreateInfo transfer{};
			transfer.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
			transfer.size = size * stride;
			staging = gpu::CreateTransferBuffer(Device, &transfer);

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
		if (!grow(
				Particles.Draws,
				Particles.DrawStaging,
				Particles.DrawCapacity,
				std::max(draws, 1u),
				PARTICLE_DRAW_WORDS * word,
				"draw list"
			) ||
			!grow(
				Particles.Births,
				Particles.BirthStaging,
				Particles.BirthCapacity,
				std::max(births, 1u),
				PARTICLE_BIRTH_WORDS * word,
				"births"
			) ||
			!grow(
				Particles.Seams,
				Particles.SeamStaging,
				Particles.SeamCapacity,
				std::max(seams, 1u),
				PARTICLE_SEAM_WORDS * word,
				"seams"
			)) {
			return false;
		}

		// **Fixed sizes, and the only buffers here that never grow.** They are
		// mapped with cycling every frame, so what they cost is their size rather
		// than what is in them - see `PARTICLE_UPDATE_BUDGET`. A record is its
		// destination row and then the payload, so each has the stride of the
		// table it feeds.
		return grow(
				   Particles.ParamUpdateBuffer,
				   Particles.ParamStaging,
				   Particles.ParamStagingRows,
				   PARTICLE_UPDATE_BUDGET,
				   (PARTICLE_PARAM_WORDS + 1) * word,
				   "parameter updates"
			   ) &&
			   grow(
				   Particles.CurveUpdateBuffer,
				   Particles.CurveStaging,
				   Particles.CurveStagingRows,
				   PARTICLE_UPDATE_BUDGET,
				   (PARTICLE_CURVE_WORDS + 1) * word,
				   "curve updates"
			   );
	}

	void Renderer::Impl::ReleaseParticlePool() {
		for (ParticleWorld &world : ParticleWorlds) {
			ParticlePool &Particles = world.Pool;
			for (SDL_GPUBuffer **buffer :
				 {&world.Buffer,
				  &Particles.States,
				  &Particles.Draws,
				  &Particles.Params,
				  &Particles.Curves,
				  &Particles.ParamUpdateBuffer,
				  &Particles.CurveUpdateBuffer,
				  &Particles.Births,
				  &Particles.Seams}) {
				if (*buffer != nullptr) {
					gpu::ReleaseBuffer(Device, *buffer);
					*buffer = nullptr;
				}
			}
			for (SDL_GPUTransferBuffer **staging :
				 {&Particles.DrawStaging,
				  &Particles.ParamStaging,
				  &Particles.CurveStaging,
				  &Particles.BirthStaging,
				  &Particles.SeamStaging}) {
				if (*staging != nullptr) {
					gpu::ReleaseTransferBuffer(Device, *staging);
					*staging = nullptr;
				}
			}
		}
		for (SDL_GPUComputePipeline **pipeline : {&ParticleStep, &ParticleScatter}) {
			if (*pipeline != nullptr) {
				SDL_ReleaseGPUComputePipeline(Device, *pipeline);
				*pipeline = nullptr;
			}
		}
		ParticleWorlds.clear();
		ActiveParticleWorld = nullptr;
	}

	bool Renderer::Impl::DispatchParticles() {
		ParticlePool &Particles = ActiveParticleWorld->Pool;
		if (ParticleStep == nullptr || Particles.Records == 0) {
			return true;
		}

		ENGINE_PROFILE_CAT("particles.step", core::ProfileCategory::Render);

		SDL_GPUCommandBuffer *command = SDL_AcquireGPUCommandBuffer(Device);
		if (command == nullptr) {
			ENGINE_ERROR("particle step: SDL_AcquireGPUCommandBuffer: {}", SDL_GetError());
			return false;
		}

		const uint32_t word = static_cast<uint32_t>(sizeof(uint32_t));

		// The frame's uploads, in one copy pass. All are cycled: this is each
		// buffer's first touch of the frame, so the previous frame's dispatch
		// keeps the version it bound.
		//
		// **The table updates are two regions of one buffer, from its two ends.**
		// `PrepareParticles` fills parameters from the front and curves from the
		// back, so a frame that changed neither uploads nothing rather than a
		// zero-length copy the driver still has to look at.
		{
			SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(command);
			if (copy == nullptr) {
				ENGINE_ERROR("particle step: SDL_BeginGPUCopyPass: {}", SDL_GetError());
				SDL_CancelGPUCommandBuffer(command);
				return false;
			}

			const auto send = [copy](
								  SDL_GPUTransferBuffer *from,
								  SDL_GPUBuffer *to,
								  uint32_t offset,
								  uint32_t bytes,
								  bool cycle
							  ) {
				if (bytes == 0) {
					return;
				}
				const SDL_GPUTransferBufferLocation source{from, offset};
				const SDL_GPUBufferRegion destination{to, offset, bytes};
				SDL_UploadToGPUBuffer(copy, &source, &destination, cycle);
			};

			send(
				Particles.DrawStaging,
				Particles.Draws,
				0,
				Particles.Records * PARTICLE_DRAW_WORDS * word,
				true
			);
			send(
				Particles.BirthStaging,
				Particles.Births,
				0,
				Particles.BirthCount * PARTICLE_BIRTH_WORDS * word,
				true
			);
			send(
				Particles.SeamStaging,
				Particles.Seams,
				0,
				Particles.SeamCount * PARTICLE_SEAM_WORDS * word,
				true
			);

			// **Not cycled, because the two ends are two copies of one buffer**
			// and cycling the second would give it a fresh version with the first
			// copy's bytes missing.
			send(
				Particles.ParamStaging,
				Particles.ParamUpdateBuffer,
				0,
				Particles.ParamUpdates * (PARTICLE_PARAM_WORDS + 1) * word,
				true
			);
			send(
				Particles.CurveStaging,
				Particles.CurveUpdateBuffer,
				0,
				Particles.CurveUpdates * (PARTICLE_CURVE_WORDS + 1) * word,
				true
			);
			SDL_EndGPUCopyPass(copy);
		}

		// One pass per scatter, because there is no barrier inside a compute pass
		// and each writes a table the step then reads.
		const auto scatter = [&](SDL_GPUBuffer *table,
								 SDL_GPUBuffer *from,
								 uint32_t count,
								 uint32_t words,
								 uint32_t rows,
								 const char *what) {
			if (ParticleScatter == nullptr || count == 0) {
				return true;
			}

			SDL_GPUStorageBufferReadWriteBinding output{};
			output.buffer = table;

			SDL_GPUComputePass *pass = SDL_BeginGPUComputePass(command, nullptr, 0, &output, 1);
			if (pass == nullptr) {
				ENGINE_ERROR("particle {}: SDL_BeginGPUComputePass: {}", what, SDL_GetError());
				return false;
			}

			SDL_BindGPUComputePipeline(pass, ParticleScatter);
			SDL_GPUBuffer *const reads[1] = {from};
			SDL_BindGPUComputeStorageBuffers(pass, 0, reads, 1);

			// A record is its destination row and then `words` of payload, so the
			// width is both the stride and the amount to copy - see
			// `ParticlePool::ParamUpdateBuffer` for what one width for two
			// records cost.
			const uint32_t counts[4] = {count, words, rows, 0};
			SDL_PushGPUComputeUniformData(command, 0, counts, sizeof(counts));
			SDL_DispatchGPUCompute(pass, (count + 63) / 64, 1, 1);
			SDL_EndGPUComputePass(pass);
			return true;
		};

		// **The block tables first, then the births, then the step**, which is
		// the order the step reads them in: a block whose curves changed this
		// frame has to have them before anything is shaded with them, and a
		// newborn has to be in the pool before the pass that shades it runs or its
		// slot draws whatever the ring last left there.
		//
		// Ageing a newborn by one step on the tick it is born is the one place
		// this differs from the host-side pass, which spawns after ageing. It is
		// one frame of drift on a value that starts at zero.
		const bool staged = scatter(
								Particles.Params,
								Particles.ParamUpdateBuffer,
								Particles.ParamUpdates,
								PARTICLE_PARAM_WORDS,
								Particles.TableRows,
								"parameter update"
							) &&
							scatter(
								Particles.Curves,
								Particles.CurveUpdateBuffer,
								Particles.CurveUpdates,
								PARTICLE_CURVE_WORDS,
								Particles.TableRows,
								"curve update"
							) &&
							scatter(
								Particles.States,
								Particles.Births,
								Particles.BirthCount,
								PARTICLE_STATE_WORDS,
								Particles.Slots,
								"spawn"
							);
		if (!staged) {
			SDL_CancelGPUCommandBuffer(command);
			return false;
		}

		{
			SDL_GPUStorageBufferReadWriteBinding outputs[2]{};
			outputs[0].buffer = Particles.States;
			outputs[1].buffer = ActiveParticleWorld->Buffer;

			SDL_GPUComputePass *pass = SDL_BeginGPUComputePass(command, nullptr, 0, outputs, 2);
			if (pass == nullptr) {
				ENGINE_ERROR("particle step: SDL_BeginGPUComputePass: {}", SDL_GetError());
				SDL_CancelGPUCommandBuffer(command);
				return false;
			}

			SDL_BindGPUComputePipeline(pass, ParticleStep);
			SDL_GPUBuffer *const reads[4] = {
				Particles.Draws, Particles.Params, Particles.Curves, Particles.Seams
			};
			SDL_BindGPUComputeStorageBuffers(pass, 0, reads, 4);

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
		ActiveParticleWorld = &ParticleWorldFor(view.World, view.WorldName);
		ParticlePool &Particles = ActiveParticleWorld->Pool;
		ParticleGroups.clear();
		Particles.Records = 0;
		Particles.BirthCount = 0;
		Particles.SeamCount = 0;

		const std::span<const render::ParticleBatch> batches = view.Particles;
		if (ParticlePipeline == nullptr || ParticleStep == nullptr || batches.empty()) {
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
		if (!ReserveParticlePool(view.ParticlePool) || !ReserveParticleTables(view.ParticleBlocks)) {
			return 0;
		}
		if (!ReserveParticleStaging(
				static_cast<uint32_t>(batches.size()),
				static_cast<uint32_t>(view.ParticleBirths.size()),
				static_cast<uint32_t>(view.ParticleSeams.size())
			)) {
			return 0;
		}

		auto *draws = static_cast<uint32_t *>(SDL_MapGPUTransferBuffer(Device, Particles.DrawStaging, true));
		auto *changedParams =
			static_cast<uint32_t *>(SDL_MapGPUTransferBuffer(Device, Particles.ParamStaging, true));
		auto *changedCurves =
			static_cast<uint32_t *>(SDL_MapGPUTransferBuffer(Device, Particles.CurveStaging, true));
		if (draws == nullptr || changedParams == nullptr || changedCurves == nullptr) {
			for (auto [mapped, buffer] : {
					 std::pair{draws, Particles.DrawStaging},
					 std::pair{changedParams, Particles.ParamStaging},
					 std::pair{changedCurves, Particles.CurveStaging},
				 }) {
				if (mapped != nullptr) {
					SDL_UnmapGPUTransferBuffer(Device, buffer);
				}
			}
			return 0;
		}

		// One walk that stages the draw list, notices which blocks the device is
		// out of date about, and records where each group lands. After this the
		// order is a buffer offset and the batch it came from is gone, which is
		// the same arrangement `SlotMesh` has for the mesh path.
		//
		// **Two words a batch and nothing else, unless something changed.** The
		// workgroup index is a draw-list index, the entry says which block and
		// where its run lands, and the step looks the rest up by block. See
		// `PARTICLE_DRAW_WORDS` for what staging the whole record instead cost.
		//
		// The parameter and curve updates share one staging buffer, filled from
		// the front for parameters and from the back for curves, so a frame that
		// changed neither writes nothing at all.
		ParticleGroups.clear();

		uint32_t written = 0;
		uint32_t staged = 0;
		uint32_t params = 0;
		uint32_t curves = 0;
		for (const uint32_t index : ParticleOrder) {
			const render::ParticleBatch &batch = batches[index];
			if (batch.Block == nullptr || batch.Block->Capacity == 0 || batch.Index >= Particles.TableRows) {
				continue;
			}
			const effects::EmitterBlock &block = *batch.Block;

			// **Brought up to date first, and left out of the frame if it cannot
			// be.** The step reads a block's capacity out of the parameter table,
			// so a block the device has not been told about yet would return from
			// its workgroup without writing anything - while this walk had
			// already reserved its run of the instance stream. That run would
			// then draw whatever was in it, which is the previous frame's
			// particles at this frame's positions. Skipping it instead costs the
			// block a frame and leaves the stream exact.
			const bool needParams = Particles.ParamRevision[batch.Index] != block.Revision;
			const bool needCurves = Particles.CurveRevision[batch.Index] != block.CurveRevision;
			if (needParams || needCurves) {
				if ((needParams && params >= PARTICLE_UPDATE_BUDGET) ||
					(needCurves && curves >= PARTICLE_UPDATE_BUDGET)) {
					continue;
				}
				if (needParams) {
					uint32_t *const row = changedParams + params * (PARTICLE_PARAM_WORDS + 1);
					row[0] = batch.Index;
					WriteParticleParams(row + 1, block);
					Particles.ParamRevision[batch.Index] = block.Revision;
					params++;
				}
				if (needCurves) {
					uint32_t *const row = changedCurves + curves * (PARTICLE_CURVE_WORDS + 1);
					row[0] = batch.Index;
					WriteParticleCurves(row + 1, block.Curves);
					Particles.CurveRevision[batch.Index] = block.CurveRevision;
					curves++;
				}
			}

			if (ParticleGroups.empty() || !SameParticleState(batches[ParticleGroups.back().Batch], batch)) {
				ParticleGroups.push_back(ParticleGroup{index, written, 0});
			}

			uint32_t *const entry = draws + staged * PARTICLE_DRAW_WORDS;
			entry[0] = batch.Index;
			entry[1] = written;
			staged++;

			written += block.Capacity;
			ParticleGroups.back().Count += block.Capacity;
		}

		SDL_UnmapGPUTransferBuffer(Device, Particles.DrawStaging);
		SDL_UnmapGPUTransferBuffer(Device, Particles.ParamStaging);
		SDL_UnmapGPUTransferBuffer(Device, Particles.CurveStaging);
		Particles.Records = staged;
		Particles.ParamUpdates = params;
		Particles.CurveUpdates = curves;
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
					const effects::ParticleBirth &birth = view.ParticleBirths[at];
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
		if (ParticlePipeline == nullptr || ActiveParticleWorld == nullptr || ParticleGroups.empty()) {
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
		vertexBinding.buffer = ActiveParticleWorld->Buffer;
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
			gpu::ReleaseBuffer(Device, RibbonBuffer);
		}
		if (RibbonTransfer != nullptr) {
			gpu::ReleaseTransferBuffer(Device, RibbonTransfer);
		}

		const uint32_t bytes = capacity * static_cast<uint32_t>(sizeof(effects::RibbonVertex));

		SDL_GPUBufferCreateInfo bufferInfo{};
		bufferInfo.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
		bufferInfo.size = bytes;
		RibbonBuffer = gpu::CreateBuffer(Device, &bufferInfo);

		SDL_GPUTransferBufferCreateInfo transferInfo{};
		transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
		transferInfo.size = bytes;
		RibbonTransfer = gpu::CreateTransferBuffer(Device, &transferInfo);

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
}
