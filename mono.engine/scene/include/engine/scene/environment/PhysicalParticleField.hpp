#pragma once

// Particles that collide with the world and count for gameplay: hail that
// bounces off a roof, rain that dies on the ground.
//
// The authored component is plain settings. The live particles are not saved
// and not replicated: server and client each step them from the same seed and
// the same clock tick, so both get the same particles. `game::PhysicalParticles`
// steps them; the renderer draws them from `PhysicalParticleWorld`; scripts get
// `OnParticleCollision` from `ParticleCollisionLog`.
//
// @tier L7 · shared

#include <engine/core/types/Color3.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Entity.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace engine::ecs {
	class Store;
}

namespace engine::scene {
	struct DrawInstance;

	// What a particle does when it hits a static part.
	enum class ParticleResponse : uint8_t {
		// Reflects with `Restitution`, and rests once it is slow.
		Bounce = 0,
		// Rests where it hit until its lifetime ends.
		Stick = 1,
		// Is removed at the hit.
		Die = 2,
	};

	// Which precipitation rate of a field source scales the spawn rate, in
	// millimetres per hour.
	enum class ParticleChannel : uint8_t {
		Rain = 0,
		Snow = 1,
		Hail = 2,
		All = 3,
	};

	// Most live particles one field may hold.
	inline constexpr uint32_t MAX_PHYSICAL_PARTICLES = 4096;
	// Most collision events one field may log per step.
	inline constexpr uint32_t MAX_PARTICLE_EVENTS_PER_STEP = 256;
	// Longest particle lifetime, in seconds. Bounds the replay a late joiner runs.
	inline constexpr float MAX_PHYSICAL_PARTICLE_LIFETIME = 30.0f;

	// An emitter of world-colliding particles. Particles spawn on the top face
	// of a world-aligned box of `HalfExtent` around the instance, fall under
	// `Acceleration`, and collide with anchored parts only (moving bodies can
	// differ between server and client at the same tick).
	//
	// @since v0.26
	struct PhysicalParticleField {
		core::Color3 Colour{0.88f, 0.92f, 0.96f};
		// Spawn box half size in metres. Particles start at the top face.
		core::Vector3 HalfExtent{48.0f, 32.0f, 48.0f};
		// Initial velocity in metres per second, before `Jitter`.
		core::Vector3 Velocity{0.0f, -14.0f, 0.0f};
		// Constant acceleration in metres per second squared.
		core::Vector3 Acceleration{0.0f, -9.81f, 0.0f};
		// Particles per second, before a source scales it.
		float Rate = 40.0f;
		// Seconds from spawn to removal, resting time included.
		float Lifetime = 6.0f;
		// Sphere radius in metres, used to draw and to report.
		float Radius = 0.06f;
		// Fraction of speed kept along the normal after a bounce.
		float Restitution = 0.35f;
		// Largest random change to the initial velocity, per axis, in metres per second.
		float Jitter = 1.5f;
		// Live particle cap, at most `MAX_PHYSICAL_PARTICLES`. Each step spawns at
		// most `MaxParticles / (Lifetime * tick rate)`, which keeps the count under it.
		uint32_t MaxParticles = 1024;
		uint32_t Seed = 0x4A11u;
		// Collision events logged per step, at most `MAX_PARTICLE_EVENTS_PER_STEP`.
		// Collisions past the cap still respond; they are not reported.
		uint32_t MaxEventsPerStep = 32;
		ParticleResponse Response = ParticleResponse::Bounce;
		bool Enabled = true;

		// Explicit padding, for the reason `Components.hpp` opens with.
		uint8_t Reserved[2] = {};
	};

	// On a `PhysicalParticleField`: the `WeatherCompute` whose clock steps the
	// particles and whose precipitation scales the spawn rate, as `Rate` times
	// `Scale` times the channel's rate at the field's centre. The weather clock
	// is shared by server and clients, which is what makes their particles
	// agree. Unset, the field steps on each world's own tick and spawns at
	// `Rate`, so machines need not agree.
	//
	// @since v0.26
	struct PhysicalParticleSource {
		ecs::Entity Source;
		float Scale = 1.0f;
		ParticleChannel Channel = ParticleChannel::Hail;

		// Explicit padding, for the reason `Components.hpp` opens with.
		uint8_t Reserved[3] = {};
	};

	// One live particle, in Q16 fixed point so every machine steps it the same.
	struct PhysicalParticle {
		// Position in 1/65536 metres, now and one step ago.
		std::array<int64_t, 3> Position{};
		std::array<int64_t, 3> Previous{};
		// Velocity in 1/65536 metres per second.
		std::array<int64_t, 3> Velocity{};
		// Spawn step shifted left 12 bits, plus the index within that step.
		uint64_t Id = 0;
		// Steps since spawn.
		uint32_t Age = 0;
		bool Resting = false;
	};

	// The live state of one field. Outlives its instance: when the instance is
	// destroyed the record is marked `Dying`, spawns stop, and the record goes
	// once its last particle has.
	struct PhysicalParticleRecord {
		ecs::Entity Field;
		// Settings copied each step while the instance lives, kept once it dies.
		PhysicalParticleField Settings;
		// Centre of the spawn box, copied with the settings.
		core::Vector3 Origin;
		// The field's source when it was last alive; a dying record keeps it.
		ecs::Entity Source;
		// Steps per second of the clock in use, and the last step taken.
		uint32_t TickRate = 20;
		uint64_t Tick = 0;
		// Identity of the clock in use; a change restarts the record.
		uint64_t Epoch = 0;
		// Interpolation between `Previous` and `Position` for drawing.
		float Alpha = 1.0f;
		bool Started = false;
		bool Dying = false;
		std::vector<PhysicalParticle> Particles;
	};

	// Every field's live state. A world resource, never saved.
	struct PhysicalParticleWorld {
		std::vector<PhysicalParticleRecord> Records;
	};

	// One reported collision. `Velocity` is the particle's velocity just
	// before the hit.
	struct ParticleCollision {
		uint64_t Sequence = 0;
		ecs::Entity Field;
		ecs::Entity Hit;
		core::Vector3 Position;
		core::Vector3 Velocity;
		core::Vector3 Normal;
		core::Color3 Colour;
		float Radius = 0.0f;
		float AgeSeconds = 0.0f;
		uint64_t ParticleId = 0;
		ParticleResponse Response = ParticleResponse::Bounce;
	};

	// Recent collisions, read by each script runtime past its own cursor.
	// Bounded: the oldest records drop first.
	struct ParticleCollisionLog {
		static constexpr size_t CAPACITY = 4096;
		uint64_t NextSequence = 1;
		std::deque<ParticleCollision> Records;
	};

	// Clamps settings into their supported ranges.
	[[nodiscard]] PhysicalParticleField SanitizePhysicalParticleField(const PhysicalParticleField &);
	// Appends one sphere row per live particle, placed between its last two
	// steps by the record's `Alpha`.
	void AppendPhysicalParticleRows(const ecs::Store &, std::vector<DrawInstance> &out);
	// Appends a collision to the store's log, creating the log on first use.
	void LogParticleCollision(ecs::Store &, ParticleCollision collision);
}
