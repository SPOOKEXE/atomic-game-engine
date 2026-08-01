#include <engine/core/Log.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/core/Random.hpp>
#include <engine/parallel/Jobs.hpp>

#include <algorithm>
#include <client/Demo.hpp>
#include <cmath>
#include <numbers>

namespace client {

	using engine::core::CFrame;
	using engine::core::Color3;
	using engine::core::Random;
	using engine::core::Vector3;
	using engine::ecs::Entity;
	using engine::ecs::Phase;
	using engine::ecs::Scheduler;
	using engine::ecs::Store;

	namespace {
		constexpr float TAU = 2.0f * std::numbers::pi_v<float>;

		// The deterministic sequence used to be an integer mixer written out
		// here, and the same one again in mono.server/src/Simulation.cpp. It is
		// engine::core::Random now — same reason, one copy, and a specified
		// algorithm rather than three constants nobody can check.

		// --- systems -------------------------------------------------------
		//
		// Every one of these is a plain function. None captures anything,
		// because there is nothing outside the world for it to capture — which
		// is what makes them registerable from bindings, replayable from a
		// recording, and reusable by a second world.

		// Before anything moves. Rendering interpolates from here, so it has to
		// be the position at the start of *this* tick — capturing it after a
		// system has run would interpolate from a place nothing was ever at.
		void CapturePrevious(Store &store) {
			store.EachParallel<PreviousTransform, const Transform>(
				[](Entity, PreviousTransform &previous, const Transform &transform) {
					previous.Frame = transform.Frame;
				}
			);
		}

		void MoveOrbits(Store &store) {
			// Simulated seconds, from the world's clock. Nothing accumulates
			// wall time here: the scene has to be in the same place after one
			// second whether that second took 30 frames or 600.
			const auto now = static_cast<float>(store.Time().Elapsed);

			store.Each<Transform, const Orbit>([now](Entity, Transform &transform, const Orbit &orbit) {
				const float angle = orbit.Phase + now * orbit.RadiansPerSecond;
				transform.Frame.Position = orbit.Centre + Vector3{
															  std::cos(angle) * orbit.Radius,
															  orbit.Height,
															  std::sin(angle) * orbit.Radius,
														  };
			});
		}

		void ApplySpin(Store &store) {
			// The tick delta, which is fixed. There is no way to reach the
			// frame delta from here by accident — it is a different field with
			// a different name.
			const float delta = store.Time().Delta;

			store.Each<Transform, const Spin>([delta](Entity, Transform &transform, const Spin &spin) {
				// Rotation composes on the right, so the spin is applied in
				// the cube's own space and the orbit position is untouched.
				transform.Frame =
					transform.Frame *
					CFrame::Angles(spin.Rate.X * delta, spin.Rate.Y * delta, spin.Rate.Z * delta);
			});
		}

		// The camera is part of the scene, so it moves in the simulation on
		// simulated time. Driving it from wall time would slide it past
		// everything it is looking at whenever the frame rate changed.
		void MoveCamera(Store &store) {
			const auto now = static_cast<float>(store.Time().Elapsed);
			const float extent = store.Resource<SceneBounds>()->Extent;

			// Far enough out that the whole scene fits, and drifting slowly so
			// that the depth buffer and the culling are visibly doing
			// something.
			const float distance = extent * 1.7f + 4.0f;
			const float angle = now * 0.12f;

			const Vector3 eye{
				std::cos(angle) * distance,
				5.0f + std::sin(now * 0.21f) * 3.5f,
				std::sin(angle) * distance,
			};

			auto *camera = store.ResourceMutable<ActiveCamera>();
			camera->Value.Frame = CFrame::LookAt(eye, Vector3::Zero);
			camera->Value.FarPlane = distance * 3.0f;
		}

		// The one phase that turns simulation state into something to draw. It
		// reads the simulation and writes only the draw list, which is what
		// "PreRender never mutates simulation state" means in practice.
		void CollectInstances(Store &store) {
			const float alpha = store.Time().Alpha;

			auto *drawList = store.ResourceMutable<DrawList>();

			// Split into spans that cost nothing to separate.
			//
			// The counting, the sizing and the arithmetic are three different
			// answers to "why is this system slow" — a cached query that is not
			// as cached as it looks, a vector reallocating every frame, or the
			// interpolation itself. One number covering all three cannot tell
			// them apart.
			//
			// It stops here. Going finer means a scope *inside* the row loop,
			// and a scope costs a clock read and a push — several times what a
			// quaternion multiply costs. That measurement would be mostly of
			// itself.
			size_t matching = 0;
			{
				ENGINE_PROFILE_CAT("count entities", engine::core::ProfileCategory::Simulation);
				matching = store.CountMatching<Transform, PreviousTransform, Visual>();
			}

			{
				// Sized once, then written by index. The vector is not cleared
				// first, so on a steady scene this is a no-op: the buffer is the
				// size it already was, and no element is value-initialised only
				// to be overwritten a moment later. A reading above zero here
				// means the scene changed size or the capacity is being lost.
				//
				// The count is a floor rather than a contract — it comes from a
				// different query than the one EachBatch walks, and this system
				// does not get to assume the two agree. The batches decide the
				// real size, and the shrink below settles it.
				ENGINE_PROFILE_CAT("size draw list", engine::core::ProfileCategory::Simulation);
				drawList->Instances.resize(matching);
			}

			size_t written = 0;
			{
				ENGINE_PROFILE_CAT("interpolate", engine::core::ProfileCategory::Simulation);
				store.EachBatch<const Transform, const PreviousTransform, const Visual>(
					[drawList, alpha, &written](
						size_t rows,
						const Transform *transforms,
						const PreviousTransform *previous,
						const Visual *visuals
					) {
						if (written + rows > drawList->Instances.size()) {
							drawList->Instances.resize(written + rows);
						}

						// Taken after the resize above, which may have moved it.
						engine::render::Instance *out = drawList->Instances.data() + written;

						for (size_t row = 0; row < rows; row++) {
							// Interpolated, not the tick position. At 300 fps against
							// a 60 Hz tick, drawing tick positions shows each one five
							// times and then jumps — which reads as a frame-rate
							// problem rather than as a tick-rate one.
							//
							// NLerp, not Lerp. The endpoints are one simulation tick
							// apart — a few degrees at most — and over an arc that
							// short the two agree to well inside a pixel. Lerp's
							// constant angular speed costs an acos and three sin
							// calls per entity, which on this loop was the single
							// most expensive thing in the frame.
							glm::mat4 model =
								previous[row].Frame.NLerp(transforms[row].Frame, alpha).ToMatrix();
							// Scale on the right, so it applies before the rotation
							// and stays a scale rather than a shear.
							model[0] *= visuals[row].Size;
							model[1] *= visuals[row].Size;
							model[2] *= visuals[row].Size;

							out[row] = engine::render::Instance{
								model,
								glm::vec4{
									visuals[row].Colour.R, visuals[row].Colour.G, visuals[row].Colour.B, 1.0f
								},
							};
						}

						written += rows;
					}
				);
			}

			{
				ENGINE_PROFILE_CAT("publish draw list", engine::core::ProfileCategory::Simulation);

				// Whatever the count said, this is how many there are. Shrinking
				// a vector writes nothing and keeps the capacity, so the frame
				// after an entity is destroyed still does not allocate.
				drawList->Instances.resize(written);

				engine::core::Metrics::Count(
					"render.instances", static_cast<double>(drawList->Instances.size())
				);
			}
		}
	}

	void BuildDemoWorld(Store &store, Scheduler &scheduler, uint32_t count) {
		// Rings rather than a cube of cubes: a ring shows depth, occlusion and
		// the shading model at a glance, and it makes the orbit motion legible.
		constexpr uint32_t PER_RING = 64;
		const uint32_t rings = std::max(1u, (count + PER_RING - 1) / PER_RING);

		// The rings share a fixed radius band instead of each one being a step
		// further out. Growing the scene with the entity count would push the
		// camera back to fit it, and every cube would shrink to a speck — so
		// `--entities 20000` would look *less* like a 3D scene than 500 does.
		constexpr float INNER_RADIUS = 3.0f;
		constexpr float OUTER_RADIUS = 12.0f;
		const float ringStep =
			rings > 1 ? (OUTER_RADIUS - INNER_RADIUS) / static_cast<float>(rings - 1) : 0.0f;

		float extent = 1.0f;

		for (uint32_t index = 0; index < count; index++) {
			const uint32_t ring = index / PER_RING;
			const float withinRing = static_cast<float>(index % PER_RING);

			const float radius = INNER_RADIUS + static_cast<float>(ring) * ringStep;
			const float height = Random::Range(index, 7u, -5.0f, 5.0f);

			const Entity entity = store.Create();

			store.Set<Transform>(entity, Transform{});
			store.Set<PreviousTransform>(entity, PreviousTransform{});
			store.Set<Orbit>(
				entity,
				Orbit{
					Vector3::Zero,
					radius,
					// Outer rings turn more slowly, which reads as depth without
					// any depth cue in the shading.
					0.45f / (1.0f + static_cast<float>(ring) * 0.35f),
					withinRing / static_cast<float>(PER_RING) * TAU,
					height,
				}
			);
			store.Set<Spin>(
				entity,
				Spin{Vector3{
					Random::Range(index, 11u, -1.2f, 1.2f),
					Random::Range(index, 13u, -1.2f, 1.2f),
					Random::Range(index, 17u, -1.2f, 1.2f),
				}}
			);
			store.Set<Visual>(
				entity,
				Visual{
					Color3::FromLinear(
						Random::Range(index, 19u, 0.15f, 0.90f),
						Random::Range(index, 23u, 0.20f, 0.80f),
						Random::Range(index, 29u, 0.35f, 0.95f)
					),
					Random::Range(index, 31u, 0.6f, 1.4f),
				}
			);

			extent = std::max(extent, radius);
		}

		// Every resource the systems below read, installed before any of them
		// can run. A system that has to check whether its resource exists yet
		// is a system with a branch for a state the world is never in.
		store.SetResource(SceneBounds{extent});
		store.SetResource(ActiveCamera{});
		store.SetResource(DrawList{});

		// Reserved once rather than grown: the count is known, and the first
		// frame is the one most likely to be looked at in a profile.
		store.ResourceMutable<DrawList>()->Instances.reserve(count);

		ENGINE_INFO("demo scene: {} entities across {} ring(s)", count, rings);

		scheduler.Add("capture-previous", Phase::PreSimulation, CapturePrevious);
		scheduler.Add("orbit", Phase::Simulation, MoveOrbits);
		scheduler.Add("spin", Phase::Simulation, ApplySpin);
		scheduler.Add("move-camera", Phase::Simulation, MoveCamera);
		scheduler.Add("collect-instances", Phase::PreRender, CollectInstances);
	}
}
