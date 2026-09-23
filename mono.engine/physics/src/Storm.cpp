#include "WorldResource.hpp"

#include <engine/core/Bytes.hpp>
#include <engine/core/Name.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/physics/Clock.hpp>
#include <engine/physics/Storm.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Constraints.hpp>
#include <engine/scene/Part.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

namespace engine::physics {

	namespace {
		constexpr float AIR_DENSITY = 1.225f;

		core::Name StormName() {
			static const core::Name name{"physics.Storm"};
			return name;
		}

		bool Finite(float value) {
			return std::isfinite(value);
		}

		bool Finite(const core::Vector3 &value) {
			return Finite(value.X) && Finite(value.Y) && Finite(value.Z);
		}

		float AreaOf(const scene::Collider &collider, float authoredArea) {
			if (Finite(authoredArea) && authoredArea > 0.0f) return authoredArea;
			const float x = std::max(collider.Extent.X, 0.0f) * 2.0f;
			const float y = std::max(collider.Extent.Y, 0.0f) * 2.0f;
			const float z = std::max(collider.Extent.Z, 0.0f) * 2.0f;
			return std::max({x * y, x * z, y * z});
		}

		core::Vector3 DragForce(
			const scene::StormSample &sample,
			const core::Vector3 &velocity,
			const scene::Collider &collider,
			const StormResponse &response
		) {
			if (!response.Enabled || !Finite(response.DragCoefficient) || !Finite(response.ForceScale) ||
				response.DragCoefficient <= 0.0f || response.ForceScale <= 0.0f) {
				return core::Vector3::Zero;
			}
			const float area = AreaOf(collider, response.ExposedArea);
			const core::Vector3 relative = sample.Velocity - velocity;
			const float speed = relative.Magnitude();
			if (!(area > 0.0f) || !Finite(area) || !(speed > 0.0f) || !Finite(speed))
				return core::Vector3::Zero;
			return relative *
				   (0.5f * AIR_DENSITY * response.DragCoefficient * area * speed * response.ForceScale);
		}

		bool ActiveBody(const scene::RigidBody &body, const StormResponse &response) {
			return body.Kind == scene::BodyKind::Dynamic && response.Enabled;
		}

		const scene::Motion *MotionOf(const ecs::Store &store, ecs::Entity entity) {
			return store.Get<scene::Motion>(entity);
		}

		float LinkLoad(
			const ecs::Store &store,
			const scene::PreparedTornadoField &field,
			const Storm &storm,
			ecs::Entity body
		) {
			const scene::Transform *transform = store.Get<scene::Transform>(body);
			const scene::Collider *collider = store.Get<scene::Collider>(body);
			const scene::RigidBody *rigidBody = store.Get<scene::RigidBody>(body);
			const StormResponse *response = store.Get<StormResponse>(body);
			if (transform == nullptr || collider == nullptr || rigidBody == nullptr || response == nullptr ||
				!ActiveBody(*rigidBody, *response)) {
				return 0.0f;
			}
			const scene::Motion *motion = MotionOf(store, body);
			const scene::StormSample sample = scene::SampleTornadoField(
				field, storm.State.Position, transform->Frame.Position, storm.State.ElapsedSeconds
			);
			return DragForce(
					   sample, motion == nullptr ? core::Vector3::Zero : motion->Linear, *collider, *response
			)
				.Magnitude();
		}

		bool Degrade(StormLink &link, float load, float delta) {
			if (!link.Enabled || !Finite(link.BreakForce) || !Finite(link.MaterialStrength) ||
				!Finite(link.Integrity) || !Finite(link.DamageRate) || link.BreakForce <= 0.0f ||
				link.MaterialStrength <= 0.0f || link.DamageRate < 0.0f) {
				return false;
			}
			const float capacity = link.BreakForce * link.MaterialStrength;
			const float overload = std::max(load / capacity - 0.55f, 0.0f);
			link.Integrity = std::clamp(link.Integrity - overload * link.DamageRate * delta, 0.0f, 1.0f);
			return link.Integrity <= 0.0f;
		}

		float WrapRadians(float radians) {
			constexpr float FULL_TURN = std::numbers::pi_v<float> * 2.0f;
			while (radians > std::numbers::pi_v<float>)
				radians -= FULL_TURN;
			while (radians < -std::numbers::pi_v<float>)
				radians += FULL_TURN;
			return radians;
		}

		void BendVegetation(
			ecs::Store &store, const scene::PreparedTornadoField &field, const Storm &storm, float delta
		) {
			store.Query<scene::Transform, StormVegetation>().Each([&](ecs::Entity entity,
																	  scene::Transform &transform,
																	  StormVegetation &vegetation) {
				if (store.Has<scene::Simulated>(entity) || !vegetation.Enabled ||
					!Finite(vegetation.MaximumBendRadians) || !Finite(vegetation.ResponsePerSecond) ||
					!Finite(vegetation.WindSpeedForMaximumBend) || vegetation.MaximumBendRadians < 0.0f ||
					vegetation.ResponsePerSecond <= 0.0f || vegetation.WindSpeedForMaximumBend <= 0.0f) {
					return;
				}

				const scene::StormSample sample = scene::SampleTornadoField(
					field, storm.State.Position, transform.Frame.Position, storm.State.ElapsedSeconds
				);
				core::Vector3 horizontal = sample.Velocity;
				horizontal.Y = 0.0f;
				const float horizontalSpeed = horizontal.Magnitude();
				const float targetBend = std::min(
					vegetation.MaximumBendRadians,
					horizontalSpeed / vegetation.WindSpeedForMaximumBend * vegetation.MaximumBendRadians
				);
				const float follow = std::min(delta * vegetation.ResponsePerSecond, 1.0f);
				vegetation.BendRadians += (targetBend - vegetation.BendRadians) * follow;

				if (horizontalSpeed > 1.0e-5f) {
					const float targetDirection = std::atan2(horizontal.Z, horizontal.X);
					vegetation.BendDirectionRadians +=
						WrapRadians(targetDirection - vegetation.BendDirectionRadians) * follow;
				}

				const core::Vector3 restUp =
					vegetation.RestFrame.VectorToWorldSpace(core::Vector3::YAxis).Unit();
				if (restUp == core::Vector3::Zero) return;
				const core::Vector3 bendDirection{
					std::cos(vegetation.BendDirectionRadians), 0.0f, std::sin(vegetation.BendDirectionRadians)
				};
				const core::Vector3 targetUp = (restUp * std::cos(vegetation.BendRadians) +
												bendDirection * std::sin(vegetation.BendRadians))
												   .Unit();
				const core::CFrame correction = core::CFrame::FromRotationBetweenVectors(restUp, targetUp);
				const core::CFrame rotation = correction * vegetation.RestFrame.RotationOnly();
				transform.Frame = core::CFrame(transform.Frame.Position, rotation.Rotation());
			});
		}

		void WriteStorms(core::ByteWriter &writer, const void *source, size_t count) {
			const auto *storms = static_cast<const Storm *>(source);
			for (size_t index = 0; index < count; index++) {
				const scene::StormState &state = storms[index].State;
				const scene::TornadoParameters &p = state.Parameters;
				for (const float value :
					 {p.Energy,
					  p.CoreRadius,
					  p.InfluenceRadius,
					  p.PeakTangentialSpeed,
					  p.PeakInflowSpeed,
					  p.PeakUpdraftSpeed,
					  p.PeakDowndraftSpeed,
					  p.SurfaceOutflowSpeed,
					  p.PressureDrop,
					  p.Humidity,
					  p.RainRate,
					  p.Turbulence,
					  p.GroundFriction,
					  p.DebrisDensity,
					  p.VortexTightness,
					  p.TopHeight})
					writer.WriteFloat(value);
				writer.WriteFloat(p.UpperWind.X);
				writer.WriteFloat(p.UpperWind.Y);
				writer.WriteFloat(p.UpperWind.Z);
				writer.WriteFloat(p.TranslationVelocity.X);
				writer.WriteFloat(p.TranslationVelocity.Y);
				writer.WriteFloat(p.TranslationVelocity.Z);
				writer.WriteBool(p.CounterClockwise);
				writer.WriteFloat(state.Position.X);
				writer.WriteFloat(state.Position.Y);
				writer.WriteFloat(state.Position.Z);
				writer.WriteFloat(state.ElapsedSeconds);
				writer.WriteBool(state.LifecycleEnabled);
				writer.WriteBool(storms[index].Enabled);
			}
		}

		void ReadStorms(core::ByteReader &reader, void *destination, size_t count) {
			auto *storms = static_cast<Storm *>(destination);
			for (size_t index = 0; index < count; index++) {
				Storm storm;
				auto &p = storm.State.Parameters;
				float *values[] = {
					&p.Energy,
					&p.CoreRadius,
					&p.InfluenceRadius,
					&p.PeakTangentialSpeed,
					&p.PeakInflowSpeed,
					&p.PeakUpdraftSpeed,
					&p.PeakDowndraftSpeed,
					&p.SurfaceOutflowSpeed,
					&p.PressureDrop,
					&p.Humidity,
					&p.RainRate,
					&p.Turbulence,
					&p.GroundFriction,
					&p.DebrisDensity,
					&p.VortexTightness,
					&p.TopHeight
				};
				for (float *value : values)
					*value = reader.ReadFloat();
				p.UpperWind = {reader.ReadFloat(), reader.ReadFloat(), reader.ReadFloat()};
				p.TranslationVelocity = {reader.ReadFloat(), reader.ReadFloat(), reader.ReadFloat()};
				p.CounterClockwise = reader.ReadBool();
				storm.State.Position = {reader.ReadFloat(), reader.ReadFloat(), reader.ReadFloat()};
				storm.State.ElapsedSeconds = reader.ReadFloat();
				storm.State.LifecycleEnabled = reader.ReadBool();
				storm.Enabled = reader.ReadBool();
				if (!Finite(storm.State.Position) || !Finite(storm.State.ElapsedSeconds) ||
					storm.State.ElapsedSeconds < 0.0f) {
					reader.Fail();
					return;
				}
				storm.State.Parameters = scene::SanitizeTornadoParameters(storm.State.Parameters);
				storms[index] = storm;
			}
		}
	}

	void RegisterStormComponents() {
		ecs::Components::Register<Storm>(StormName().Text(), WriteStorms, ReadStorms);
		ecs::Components::Register<StormResponse>("physics.StormResponse");
		ecs::Components::Register<StormLink>("physics.StormLink");
		ecs::Components::Register<StormVegetation>("physics.StormVegetation");
	}

	void SetStorm(ecs::Store &store, const Storm &storm) {
		RegisterStormComponents();
		Storm sanitized = storm;
		sanitized.State.Parameters = scene::SanitizeTornadoParameters(sanitized.State.Parameters);
		if (!Finite(sanitized.State.Position) || !Finite(sanitized.State.ElapsedSeconds) ||
			sanitized.State.ElapsedSeconds < 0.0f) {
			return;
		}
		store.SetResource(sanitized);
	}

	void ClearStorm(ecs::Store &store) {
		if (ecs::Components::Find(StormName()).IsValid()) store.RemoveResource<Storm>();
	}

	const Storm *StormOf(const ecs::Store &store) {
		if (!ecs::Components::Find(StormName()).IsValid()) return nullptr;
		return store.Resource<Storm>();
	}

	void ApplyStormForces(ecs::Store &store) {
		if (!ecs::Components::Find(StormName()).IsValid()) return;
		Storm *storm = store.ResourceMutable<Storm>();
		PhysicsWorld *world = PreparedWorldMutable(store);
		if (storm == nullptr || world == nullptr || !storm->Enabled) return;

		const float delta = PhysicsStepSeconds(store);
		if (!(delta > 0.0f) || !Finite(delta)) return;
		scene::AdvanceStorm(storm->State, delta);
		const scene::PreparedTornadoField field = scene::PrepareTornadoField(storm->State.Parameters);
		BendVegetation(store, field, *storm, delta);

		store.Query<scene::WeldConstraint, StormLink>().Each(
			[&](ecs::Entity, scene::WeldConstraint &joint, StormLink &link) {
				if (!joint.Enabled || Degrade(
										  link,
										  std::max(
											  LinkLoad(store, field, *storm, joint.Part0),
											  LinkLoad(store, field, *storm, joint.Part1)
										  ),
										  delta
									  ))
					joint.Enabled = false;
			}
		);
		store.Query<scene::JointInstance, StormLink>().Each(
			[&](ecs::Entity, scene::JointInstance &joint, StormLink &link) {
				if (!joint.Enabled || Degrade(
										  link,
										  std::max(
											  LinkLoad(store, field, *storm, joint.Part0),
											  LinkLoad(store, field, *storm, joint.Part1)
										  ),
										  delta
									  ))
					joint.Enabled = false;
			}
		);

		std::vector<ecs::Entity> sleepers;
		store
			.Query<
				const scene::Transform,
				const scene::Collider,
				const scene::RigidBody,
				const StormResponse>()
			.With<scene::Simulated>()
			.Each([&](ecs::Entity entity,
					  const scene::Transform &transform,
					  const scene::Collider &collider,
					  const scene::RigidBody &body,
					  const StormResponse &response) {
				if (!ActiveBody(body, response)) return;
				const scene::StormSample sample = scene::SampleTornadoField(
					field, storm->State.Position, transform.Frame.Position, storm->State.ElapsedSeconds
				);
				if (DragForce(sample, core::Vector3::Zero, collider, response) == core::Vector3::Zero) return;
				world->Wake(entity);
				if (!store.Has<scene::Motion>(entity)) sleepers.push_back(entity);
			});
		for (const ecs::Entity entity : sleepers)
			store.Set(entity, scene::Motion{});

		store
			.Query<
				scene::Motion,
				const scene::Transform,
				const scene::Collider,
				const scene::RigidBody,
				const StormResponse>()
			.With<scene::Simulated>()
			.Each([&](ecs::Entity entity,
					  scene::Motion &motion,
					  const scene::Transform &transform,
					  const scene::Collider &collider,
					  const scene::RigidBody &body,
					  const StormResponse &response) {
				if (!ActiveBody(body, response)) return;
				const float mass = scene::MassOf(collider, body, store.Get<scene::PhysicsProperties>(entity));
				if (!(mass > 0.0f) || !Finite(mass)) return;
				const scene::StormSample sample = scene::SampleTornadoField(
					field, storm->State.Position, transform.Frame.Position, storm->State.ElapsedSeconds
				);
				motion.Linear =
					motion.Linear + DragForce(sample, motion.Linear, collider, response) * (delta / mass);
			});
	}
}
