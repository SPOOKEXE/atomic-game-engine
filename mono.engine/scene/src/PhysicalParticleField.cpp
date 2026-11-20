#include <engine/core/Name.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/DrawInstance.hpp>
#include <engine/scene/PhysicalParticleField.hpp>

#include <algorithm>
#include <cmath>

namespace engine::scene {
	namespace {
		constexpr double Q16 = 65536.0;

		float Finite(float value, float fallback) {
			return std::isfinite(value) ? value : fallback;
		}

		core::Vector3 FiniteVector(core::Vector3 value) {
			return {Finite(value.X, 0.0f), Finite(value.Y, 0.0f), Finite(value.Z, 0.0f)};
		}
	}

	PhysicalParticleField SanitizePhysicalParticleField(const PhysicalParticleField &field) {
		PhysicalParticleField out = field;
		out.HalfExtent = {
			std::clamp(Finite(field.HalfExtent.X, 0.0f), 0.0f, 4096.0f),
			std::clamp(Finite(field.HalfExtent.Y, 0.0f), 0.0f, 4096.0f),
			std::clamp(Finite(field.HalfExtent.Z, 0.0f), 0.0f, 4096.0f),
		};
		out.Velocity = FiniteVector(field.Velocity);
		out.Acceleration = FiniteVector(field.Acceleration);
		out.Rate = std::clamp(Finite(field.Rate, 0.0f), 0.0f, 100000.0f);
		out.Lifetime = std::clamp(Finite(field.Lifetime, 0.0f), 0.0f, MAX_PHYSICAL_PARTICLE_LIFETIME);
		out.Radius = std::clamp(Finite(field.Radius, 0.0f), 0.001f, 4.0f);
		out.Restitution = std::clamp(Finite(field.Restitution, 0.0f), 0.0f, 1.0f);
		out.Jitter = std::clamp(Finite(field.Jitter, 0.0f), 0.0f, 1000.0f);
		out.MaxParticles = std::min(field.MaxParticles, MAX_PHYSICAL_PARTICLES);
		out.MaxEventsPerStep = std::min(field.MaxEventsPerStep, MAX_PARTICLE_EVENTS_PER_STEP);
		if (static_cast<uint8_t>(field.Response) > static_cast<uint8_t>(ParticleResponse::Die))
			out.Response = ParticleResponse::Bounce;
		return out;
	}

	void AppendPhysicalParticleRows(const ecs::Store &store, std::vector<DrawInstance> &out) {
		const auto *world = store.Resource<PhysicalParticleWorld>();
		if (!world) return;
		static const core::Name SPHERE("engine.Sphere");
		for (const PhysicalParticleRecord &record : world->Records) {
			const float alpha = std::clamp(record.Alpha, 0.0f, 1.0f);
			const float half = record.Settings.Radius;
			for (const PhysicalParticle &particle : record.Particles) {
				const auto lerp = [&](int axis) {
					const double from = static_cast<double>(particle.Previous[axis]) / Q16;
					const double to = static_cast<double>(particle.Position[axis]) / Q16;
					return static_cast<float>(from + (to - from) * alpha);
				};
				const core::Vector3 at{lerp(0), lerp(1), lerp(2)};
				DrawInstance row;
				row.Frame = core::CFrame(at);
				row.HalfExtent = {half, half, half};
				row.Tint = record.Settings.Colour;
				row.SurfaceColour = record.Settings.Colour;
				row.Mesh = SPHERE;
				row.CastShadow = false;
				// The field names the slot and the particle its form, so each
				// particle keeps one resident row while it lives.
				row.Source = record.Field.Id;
				row.Variant = particle.Id + 1;
				out.push_back(row);
			}
		}
	}

	void LogParticleCollision(ecs::Store &store, ParticleCollision collision) {
		if (!store.HasResource<ParticleCollisionLog>()) store.SetResource(ParticleCollisionLog{});
		auto &log = *store.ResourceMutable<ParticleCollisionLog>();
		collision.Sequence = log.NextSequence++;
		log.Records.push_back(collision);
		while (log.Records.size() > ParticleCollisionLog::CAPACITY)
			log.Records.pop_front();
	}
}
