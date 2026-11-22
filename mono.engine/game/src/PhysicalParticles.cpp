#include <engine/core/Profiling.hpp>
#include <engine/core/types/Ray.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/game/PhysicalParticles.hpp>
#include <engine/physics/Query.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/environment/PhysicalParticleField.hpp>
#include <engine/scene/environment/WeatherClock.hpp>
#include <engine/weather/Runtime.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

namespace engine::game {
	namespace {
		using scene::PhysicalParticle;
		using scene::PhysicalParticleField;
		using scene::PhysicalParticleRecord;
		using Q3 = std::array<int64_t, 3>;

		constexpr int64_t ONE = 65536;
		// Below this speed after a bounce, in Q16 metres per second, a particle rests.
		constexpr int64_t REST_SPEED = ONE / 2;
		// Most particles one step may spawn; the low 12 bits of an id.
		constexpr uint32_t MAX_SPAWNS_PER_STEP = 4095;

		int64_t ToQ(double value) {
			return std::llround(value * static_cast<double>(ONE));
		}
		double FromQ(int64_t value) {
			return static_cast<double>(value) / static_cast<double>(ONE);
		}
		Q3 ToQ(core::Vector3 value) {
			return {ToQ(value.X), ToQ(value.Y), ToQ(value.Z)};
		}
		core::Vector3 FromQ(const Q3 &value) {
			return {
				static_cast<float>(FromQ(value[0])),
				static_cast<float>(FromQ(value[1])),
				static_cast<float>(FromQ(value[2]))
			};
		}

		uint64_t Mix(uint64_t value) {
			value += 0x9E3779B97F4A7C15ull;
			value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
			value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
			return value ^ (value >> 31);
		}

		// A signed fraction of `scale` from 16 hash bits, in [-scale, scale].
		int64_t Spread(uint64_t bits, int64_t scale) {
			return (static_cast<int64_t>(bits & 0xFFFF) - 32768) * scale / 32768;
		}

		// The clock a record steps on this frame.
		struct Clock {
			uint64_t Now = 0;
			uint64_t Epoch = 0;
			uint32_t Rate = 20;
			float Alpha = 1.0f;
			bool Shared = false;
		};

		Clock ClockOf(const ecs::Store &store, ecs::Entity source) {
			if (const auto *clock = store.Get<scene::WeatherClock>(source);
				clock && scene::ValidWeatherClock(*clock)) {
				return {
					clock->Tick,
					clock->Epoch,
					clock->TickRate,
					static_cast<float>(std::clamp(clock->Fraction, 0.0, 1.0)),
					true
				};
			}
			const auto time = store.Time();
			const float delta = std::isfinite(time.Delta) && time.Delta > 0 ? time.Delta : 1.0f / 60.0f;
			const auto rate = static_cast<uint32_t>(std::clamp(std::lround(1.0f / delta), 1l, 1000l));
			return {time.Tick, 0, rate, std::clamp(time.Alpha, 0.0f, 1.0f), false};
		}

		uint32_t LifetimeTicks(const PhysicalParticleField &field, uint32_t rate) {
			return std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(field.Lifetime * rate)));
		}

		// Particles per second the field spawns at `tick`. A sourced field
		// samples its weather once per clock second, at that second's first
		// tick, so a replay reads the same samples the live run did.
		double SpawnRate(ecs::Store &store, const PhysicalParticleRecord &record, uint64_t tick) {
			const auto *link = store.Get<scene::PhysicalParticleSource>(record.Field);
			if (!link || record.Source == ecs::NULL_ENTITY) return record.Settings.Rate;
			const uint64_t sampled = tick - tick % record.TickRate;
			const float ground = record.Origin.Y - record.Settings.HalfExtent.Y;
			const auto sample = weather::QueryWeatherAt(store, record.Source, record.Origin, ground, sampled);
			if (!sample) return 0.0;
			const auto &rates = sample->Precipitation;
			double value = 0.0;
			switch (link->Channel) {
			case scene::ParticleChannel::Rain:
				value = rates.LiquidMillimetresPerHour;
				break;
			case scene::ParticleChannel::Snow:
				value = rates.SnowMillimetresPerHour;
				break;
			case scene::ParticleChannel::Hail:
				value = rates.HailMillimetresPerHour;
				break;
			case scene::ParticleChannel::All:
				value = rates.LiquidMillimetresPerHour + rates.SnowMillimetresPerHour +
						rates.HailMillimetresPerHour;
				break;
			}
			return std::max(0.0, static_cast<double>(record.Settings.Rate) * link->Scale * value);
		}

		void Spawn(ecs::Store &store, PhysicalParticleRecord &record, uint64_t tick) {
			const PhysicalParticleField &field = record.Settings;
			if (!field.Enabled || record.Dying) return;
			// A pure function of the tick, so a replay spawns what the live run
			// did: the whole part of the per-step rate, plus one more when this
			// tick's hash falls under the fraction. The cap is per step, sized so
			// a full lifetime of spawns stays within `MaxParticles`.
			const double perStep = SpawnRate(store, record, tick) / record.TickRate;
			const auto whole = static_cast<uint64_t>(std::floor(perStep));
			const auto fraction = static_cast<uint64_t>(std::llround((perStep - std::floor(perStep)) * ONE));
			const uint64_t roll = Mix(field.Seed ^ Mix(tick)) & (ONE - 1);
			const uint64_t lifetime = LifetimeTicks(field, record.TickRate);
			const uint64_t cap =
				field.MaxParticles == 0 ? 0 : std::max<uint64_t>(1, field.MaxParticles / lifetime);
			const uint64_t count =
				std::min<uint64_t>({whole + (roll < fraction ? 1 : 0), cap, MAX_SPAWNS_PER_STEP});

			const Q3 origin = ToQ(record.Origin);
			const Q3 half = ToQ(field.HalfExtent);
			const Q3 velocity = ToQ(field.Velocity);
			const int64_t jitter = ToQ(field.Jitter);
			for (uint32_t index = 0; index < count; ++index) {
				const uint64_t key = Mix(Mix(field.Seed ^ (tick << 20)) ^ index);
				const uint64_t other = Mix(key);
				PhysicalParticle particle;
				particle.Id = (tick << 12) | index;
				particle.Position = {
					origin[0] + Spread(key, half[0]),
					origin[1] + half[1],
					origin[2] + Spread(key >> 16, half[2]),
				};
				particle.Previous = particle.Position;
				particle.Velocity = {
					velocity[0] + Spread(other, jitter),
					velocity[1] + Spread(other >> 16, jitter),
					velocity[2] + Spread(other >> 32, jitter),
				};
				record.Particles.push_back(particle);
			}
		}

		// The nearest anchored part along a segment. A moving body can be in a
		// different place on another machine at the same tick, so it is
		// skipped, once; a second moving body in the way hides the hit.
		std::optional<physics::ColliderHit>
		StaticHit(const ecs::Store &store, core::Vector3 from, core::Vector3 to) {
			const core::Vector3 segment = to - from;
			const float length = segment.Magnitude();
			if (!(length > 0.0f)) return std::nullopt;
			const core::Ray ray{from, segment / length};
			ecs::Entity ignore = ecs::NULL_ENTITY;
			for (int attempt = 0; attempt < 2; ++attempt) {
				auto hit = physics::Raycast(store, ray, length, spatial::LayerMask::All(), ignore, false);
				if (!hit) return std::nullopt;
				if (!store.Has<scene::Simulated>(hit->Owner)) return hit;
				ignore = hit->Owner;
			}
			return std::nullopt;
		}

		int64_t Dot(const Q3 &a, const Q3 &b) {
			return (a[0] * b[0] + a[1] * b[1] + a[2] * b[2]) / ONE;
		}

		void Step(ecs::Store &store, PhysicalParticleRecord &record, uint64_t tick, bool report) {
			const PhysicalParticleField &field = record.Settings;
			const int64_t rate = record.TickRate;
			const Q3 acceleration = ToQ(field.Acceleration);
			const int64_t radius = ToQ(field.Radius);
			const int64_t restitution = ToQ(field.Restitution);
			const uint32_t lifetime = LifetimeTicks(field, record.TickRate);
			uint32_t reported = 0;

			for (PhysicalParticle &particle : record.Particles) {
				particle.Previous = particle.Position;
				++particle.Age;
				if (particle.Resting) continue;
				for (int axis = 0; axis < 3; ++axis)
					particle.Velocity[axis] += acceleration[axis] / rate;
				Q3 next;
				for (int axis = 0; axis < 3; ++axis)
					next[axis] = particle.Position[axis] + particle.Velocity[axis] / rate;

				const auto hit = StaticHit(store, FromQ(particle.Position), FromQ(next));
				if (!hit) {
					particle.Position = next;
					continue;
				}
				const Q3 normal = ToQ(hit->Normal);
				const Q3 at = ToQ(hit->Position);
				const Q3 before = particle.Velocity;
				if (report && !record.Dying && reported < field.MaxEventsPerStep) {
					++reported;
					scene::ParticleCollision collision;
					collision.Field = record.Field;
					collision.Hit = hit->Owner;
					collision.Position = hit->Position;
					collision.Velocity = FromQ(before);
					collision.Normal = hit->Normal;
					collision.Colour = field.Colour;
					collision.Radius = field.Radius;
					collision.AgeSeconds = static_cast<float>(particle.Age) / record.TickRate;
					collision.ParticleId = particle.Id;
					collision.Response = field.Response;
					scene::LogParticleCollision(store, collision);
				}

				// Rest one radius off the surface so the next step starts outside it.
				for (int axis = 0; axis < 3; ++axis)
					particle.Position[axis] = at[axis] + normal[axis] * radius / ONE;
				switch (field.Response) {
				case scene::ParticleResponse::Die:
					particle.Age = lifetime;
					break;
				case scene::ParticleResponse::Stick:
					particle.Velocity = {};
					particle.Resting = true;
					break;
				case scene::ParticleResponse::Bounce: {
					const int64_t along = Dot(before, normal);
					if (along < 0) {
						const int64_t kept = along + along * restitution / ONE;
						for (int axis = 0; axis < 3; ++axis)
							particle.Velocity[axis] -= normal[axis] * kept / ONE;
					}
					const int64_t speed = Dot(particle.Velocity, particle.Velocity);
					if (speed < REST_SPEED * REST_SPEED / ONE) {
						particle.Velocity = {};
						particle.Resting = true;
					}
					break;
				}
				}
			}
			std::erase_if(record.Particles, [&](const PhysicalParticle &particle) {
				return particle.Age >= lifetime;
			});
			Spawn(store, record, tick);
		}

		// Brings one record to `clock.Now`. A new record, a changed clock or a
		// gap longer than a lifetime replays from `Now - Lifetime` with no
		// reports; an unshared clock skips the replay, since no other machine
		// could agree with it anyway.
		void Advance(ecs::Store &store, PhysicalParticleRecord &record, const Clock &clock) {
			const uint32_t lifetime = LifetimeTicks(record.Settings, clock.Rate);
			const auto changed = [&] {
				return record.Epoch != clock.Epoch || record.TickRate != clock.Rate ||
					   clock.Now < record.Tick;
			};
			if (record.Dying && record.Started && changed()) {
				// A dying record has no instance left to agree about; it only
				// needs to keep falling, so it adopts whatever clock remains.
				record.Epoch = clock.Epoch;
				record.TickRate = clock.Rate;
				record.Tick = clock.Now > 0 ? clock.Now - 1 : 0;
			}
			bool report = true;
			if (!record.Started || changed() || clock.Now - record.Tick > lifetime) {
				record.Particles.clear();
				record.Epoch = clock.Epoch;
				record.TickRate = clock.Rate;
				const uint64_t replay = clock.Shared ? lifetime : 0;
				record.Tick = clock.Now - std::min<uint64_t>(clock.Now, replay);
				record.Started = true;
				report = false;
			}
			const uint64_t live = clock.Now;
			while (record.Tick < live) {
				++record.Tick;
				// The last tick is the only one this machine reaches live
				// after a replay; ordinary catch-up reports every tick.
				Step(store, record, record.Tick, report || record.Tick == live);
			}
			record.Alpha = clock.Alpha;
		}
	}

	void StepPhysicalParticles(ecs::Store &store) {
		const bool anyField = store.CountMatching<scene::PhysicalParticleField>() > 0;
		if (!anyField && !store.HasResource<scene::PhysicalParticleWorld>()) return;
		ENGINE_PROFILE("physical particles");
		if (!store.HasResource<scene::PhysicalParticleWorld>())
			store.SetResource(scene::PhysicalParticleWorld{});

		// Collected first: stepping queries the weather, which may touch the store.
		struct Live {
			ecs::Entity Field;
			PhysicalParticleField Settings;
			core::Vector3 Origin;
		};
		std::vector<Live> fields;
		store.Each<const PhysicalParticleField, const scene::Transform>(
			[&](ecs::Entity entity, const PhysicalParticleField &field, const scene::Transform &transform) {
				fields.push_back(
					{entity, scene::SanitizePhysicalParticleField(field), transform.Frame.Position}
				);
			}
		);

		// Taken out of the store while stepping: a weather query or the first
		// logged collision sets a resource, which may move the one holding them.
		std::vector<PhysicalParticleRecord> records =
			std::move(store.ResourceMutable<scene::PhysicalParticleWorld>()->Records);
		for (PhysicalParticleRecord &record : records)
			record.Dying = true;
		for (const Live &field : fields) {
			auto found = std::find_if(records.begin(), records.end(), [&](const auto &record) {
				return record.Field == field.Field;
			});
			if (found == records.end()) {
				records.push_back({});
				found = records.end() - 1;
				found->Field = field.Field;
			}
			found->Settings = field.Settings;
			found->Origin = field.Origin;
			const auto *link = store.Get<scene::PhysicalParticleSource>(field.Field);
			found->Source = link ? link->Source : ecs::NULL_ENTITY;
			found->Dying = false;
		}

		for (PhysicalParticleRecord &record : records)
			Advance(store, record, ClockOf(store, record.Source));
		std::erase_if(records, [](const auto &record) { return record.Dying && record.Particles.empty(); });
		store.ResourceMutable<scene::PhysicalParticleWorld>()->Records = std::move(records);
	}

	void RegisterPhysicalParticleSystem(ecs::Scheduler &scheduler) {
		scheduler.Add(
			"particles.physical",
			ecs::Phase::Animation,
			StepPhysicalParticles,
			ecs::SystemOrder{{}, {}, {}, {"weather.runtime", "weather.ground"}}
		);
	}
}
