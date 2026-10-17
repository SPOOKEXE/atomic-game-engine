// Real ordinary-emitter recording cost, with GPU waits outside the CPU spans.
#include <engine/core/FrameGraph.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/effects/ParticleSystem.hpp>
#include <engine/effects/Registration.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/render/WorldView.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/testing/Bench.hpp>

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <vector>

TEST_SUITE_ID("engine.render.bench.particles")

namespace {
	using namespace engine;
	using Clock = std::chrono::steady_clock;
	constexpr uint32_t FRAMES = 16;

	struct ParticleFixture {
		ecs::Store World{"ordinary-particle-bench"};
		render::WorldViewFrame Frame;
		render::WorldCameraFrame Camera;
		render::WorldViewBinding Binding;
		render::View View;
		std::vector<ecs::Entity> Emitters;

		ParticleFixture(uint32_t count, float rate, float lifetime) {
			scene::RegisterSceneClasses();
			effects::RegisterEffectClasses();
			const auto workspace = scene::InstallServices(World);
			const auto slotsPerEmitter = static_cast<uint32_t>(std::ceil(rate * lifetime)) + 1;
			effects::InstallParticles(World, count * slotsPerEmitter);
			World.ResourceMutable<effects::ParticleSystem>()->DeviceStepped = true;
			const auto emitterClass = ecs::Classes::Find(core::Name("ParticleEmitter"));
			for (uint32_t index = 0; index < count; ++index) {
				scene::PartDesc host;
				host.Simulated = false;
				host.Frame.Position = {float(index % 320) * 2, 0, -float(index / 320) * 2};
				const auto part = scene::MakePart(World, host);
				if (!World.SetParent(part, workspace)) throw std::runtime_error("host parent refused");
				const auto emitter = World.CreateInstance(emitterClass);
				if (!World.SetParent(emitter, part)) throw std::runtime_error("emitter parent refused");
				auto *settings = World.GetMutable<effects::ParticleEmitter>(emitter);
				settings->Rate = rate;
				settings->Lifetime = {lifetime, lifetime};
				settings->Speed = {1, 3};
				settings->Acceleration = {0, -4, 0};
				Emitters.push_back(emitter);
			}
			effects::RefreshEmitters(World);
			Binding.Name = core::Name(World.Name());
			Binding.Identity = World.Identity();
			Binding.World = World.Identity();
			View.CameraFrame = core::CFrame::LookAt({320, 260, 450}, {320, 0, -150});
			View.Camera.FarPlane = 4000;
		}
		void Advance(uint32_t changed, uint32_t tick) {
			World.ClearChanges();
			for (uint32_t index = 0; index < changed; ++index) {
				auto *settings = World.GetMutable<effects::ParticleEmitter>(Emitters[index]);
				settings->Acceleration.X = (tick & 1u) == 0 ? .25f : -.25f;
			}
			effects::RefreshEmitters(World);
			effects::StepParticles(World, 1.0f / 60.0f);
			World.AdvanceTick(1.0f / 60.0f);
			render::CollectWorldView(World, Binding.Name, Frame);
			if (!render::BindWorldView(Frame, Camera, Binding, View))
				throw std::runtime_error("particle view binding refused");
		}
	};

	uint64_t Counter(std::string_view name) {
		const auto value = core::Metrics::Get(name);
		return value ? static_cast<uint64_t>(value->Value) : 0;
	}
	struct Traffic {
		uint64_t Work = Counter("render.particles.work_upload_bytes");
		uint64_t EmitWork = Counter("render.particles.emit_work_upload_bytes");
		uint64_t EmissionLanes = Counter("render.particles.emission_dispatch_lanes");
		uint64_t IntegrationLanes = Counter("render.particles.integration_dispatch_lanes");
		uint64_t Params = Counter("render.particles.parameter_upload_bytes");
		uint64_t Curves = Counter("render.particles.curve_upload_bytes");
		uint64_t Metadata = Counter("render.particles.metadata_copy_bytes");
	};

	void Measure(uint32_t count, uint32_t changed, float rate = 5, float lifetime = 1) {
		if (!SDL_Init(SDL_INIT_VIDEO)) throw std::runtime_error(SDL_GetError());
		struct VideoQuit {
			~VideoQuit() {
				SDL_QuitSubSystem(SDL_INIT_VIDEO);
			}
		} videoQuit;
		const bool profiling = core::FrameGraph::IsEnabled();
		struct ProfileRestore {
			bool Enabled;
			~ProfileRestore() {
				core::FrameGraph::SetEnabled(Enabled);
			}
		} restore{profiling};
		core::FrameGraph::SetEnabled(true);
		render::Renderer renderer;
		if (!renderer.Initialise(nullptr)) throw std::runtime_error("particle renderer unavailable");
		if (!renderer.Capabilities().HasCompute) throw std::runtime_error("particle compute unavailable");
		renderer.SetProfiling(render::ProfilingTier::Full);
		auto *device = static_cast<SDL_GPUDevice *>(renderer.Backend().Device);
		render::SceneTarget target{960, 540};
		render::OverlayImage overlay;
		ParticleFixture fixture(count, rate, lifetime);
		fixture.View.Target = &target;
		// The upload budget admits at most 2,048 emitter rows per frame. Finish
		// admission and fill each emission ring, then allow a second of steady
		// retirement before measuring. Keep the existing two-second minimum.
		const auto admissionFrames = (count + 2047) / 2048;
		const auto warmFrames =
			std::max(120u, static_cast<uint32_t>(std::ceil(lifetime * 60)) + admissionFrames + 60);
		for (uint32_t tick = 0; tick < warmFrames; ++tick) {
			fixture.Advance(0, tick);
			if (!SDL_WaitForGPUIdle(device)) throw std::runtime_error(SDL_GetError());
			const auto result = renderer.Render(std::span(&fixture.View, 1), overlay, nullptr, false);
			if (!result.Submitted) throw std::runtime_error("particle warm-up submission failed");
		}
		uint64_t recording = 0;
		double prepare = 0, prepareSelf = 0, gpuStep = 0, gpuTransparent = 0;
		uint32_t timestamps = 0, particles = 0, drawn = 0, dispatches = 0;
		const auto heapBefore = renderer.MemoryStatistics();
		const Traffic before;
		for (uint32_t tick = 0; tick < FRAMES; ++tick) {
			fixture.Advance(changed, tick);
			if (!SDL_WaitForGPUIdle(device)) throw std::runtime_error(SDL_GetError());
			core::FrameGraph::BeginFrame();
			const auto started = Clock::now();
			const auto result = renderer.Render(std::span(&fixture.View, 1), overlay, nullptr, false);
			recording += static_cast<uint64_t>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started).count()
			);
			core::FrameGraph::EndFrame();
			if (!result.Submitted) throw std::runtime_error("particle measured submission failed");
			particles = result.Particles;
			drawn = result.ParticlesDrawn;
			dispatches += result.ComputeDispatches;
			for (const auto &span : core::FrameGraph::Spans()) {
				if (span.Name != "prepare particles") continue;
				prepare += span.Milliseconds;
				prepareSelf += span.SelfMilliseconds;
			}
			const auto &timings = renderer.PassTimings();
			const auto step = timings.find(core::Name("particle-step").Id());
			const auto transparent = timings.find(core::Name("transparent").Id());
			if (step != timings.end() && transparent != timings.end()) {
				gpuStep += step->second;
				gpuTransparent += transparent->second;
				++timestamps;
			}
		}
		const auto heapAfter = renderer.MemoryStatistics();
		const Traffic after;
		if (std::getenv("MONO_ORDINARY_PARTICLE_REPORT") != nullptr) {
			std::cout << "ordinary-particle-report emitters=" << count << " changed=" << changed
					  << " rate=" << rate << " lifetime=" << lifetime << " warm_frames=" << warmFrames
					  << " slots_per_emitter=" << particles / count << " slots=" << particles
					  << " drawn=" << drawn << " frames=" << FRAMES
					  << " cameras=1 extent=960x540 cpu_record_ns=" << recording / FRAMES
					  << " cpu_prepare_us=" << prepare * 1000 / FRAMES
					  << " cpu_prepare_self_us=" << prepareSelf * 1000 / FRAMES
					  << " gpu_timing_reads=" << timestamps
					  << " gpu_step_us=" << (timestamps == 0 ? 0 : gpuStep / timestamps)
					  << " gpu_transparent_us=" << (timestamps == 0 ? 0 : gpuTransparent / timestamps)
					  << " dispatches=" << dispatches << " work_upload_bytes=" << after.Work - before.Work
					  << " emit_work_upload_bytes=" << after.EmitWork - before.EmitWork
					  << " emission_dispatch_lanes=" << after.EmissionLanes - before.EmissionLanes
					  << " integration_dispatch_lanes=" << after.IntegrationLanes - before.IntegrationLanes
					  << " parameter_upload_bytes=" << after.Params - before.Params
					  << " curve_upload_bytes=" << after.Curves - before.Curves
					  << " metadata_copy_bytes=" << after.Metadata - before.Metadata
					  << " gpu_allocated_bytes=" << heapAfter.AllocatedBytes - heapBefore.AllocatedBytes
					  << " gpu_live_bytes=" << heapAfter.LiveBytes << '\n';
		}
		testing::Consume(particles);
		renderer.Shutdown();
	}
}

BENCH("Ordinary particles | 1k emitters | unchanged", FRAMES) {
	Measure(1'000, 0);
}
BENCH("Ordinary particles | 10k emitters | unchanged", FRAMES) {
	Measure(10'000, 0);
}
BENCH("Ordinary particles | 100k emitters | unchanged", FRAMES) {
	Measure(100'000, 0);
}
BENCH("Ordinary particles | 10k emitters | one changed", FRAMES) {
	Measure(10'000, 1);
}
BENCH("Ordinary particles | 10k emitters | 101 slots | unchanged", FRAMES) {
	Measure(10'000, 0, 20, 5);
}
