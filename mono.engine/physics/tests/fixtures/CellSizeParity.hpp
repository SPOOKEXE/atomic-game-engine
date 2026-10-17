#pragma once

#include <engine/ecs/Store.hpp>
#include <engine/physics/PhysicsWorld.hpp>
#include <engine/scene/Components.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string_view>
#include <vector>

namespace engine::physics::testing {
	// Capture public contacts and body outputs without padding or a hash.
	// Grid topology and private solver warm-start caches are not part of this comparison.
	struct CellSizeSnapshot {
		std::vector<uint64_t> Pairs;
		std::vector<uint64_t> Manifolds;
		std::vector<uint64_t> Events;
		std::vector<uint64_t> Bodies;
	};

	// Opt-in canonical words for cross-binary comparison. Hosts capture stdout in memory;
	// no padding, addresses or hash collisions can affect the compared payload.
	inline bool CanonicalOutputEnabled() {
		const char *value = std::getenv("ATOMIC_SOLVER_PARITY_DUMP");
		return value != nullptr && value[0] == '1' && value[1] == '\0';
	}

	inline void DumpWords(
		const char *scope, size_t scene, size_t tick, const char *section, std::span<const uint64_t> words
	) {
		if (!CanonicalOutputEnabled()) return;
		std::printf(
			"# physics-parity scope=%s scene=%zu tick=%zu section=%s words=%zu hex=",
			scope,
			scene,
			tick,
			section,
			words.size()
		);
		constexpr std::string_view digits = "0123456789abcdef";
		std::array<char, 4096> block{};
		size_t used = 0;
		for (uint64_t word : words) {
			for (size_t nibble = 0; nibble < 16; nibble++)
				block[used++] = digits[(word >> (60 - nibble * 4)) & 15];
			if (used == block.size()) {
				std::fwrite(block.data(), 1, used, stdout);
				used = 0;
			}
		}
		if (used) std::fwrite(block.data(), 1, used, stdout);
		std::fputc('\n', stdout);
	}

	inline void DumpSnapshot(const char *scope, size_t scene, size_t tick, const CellSizeSnapshot &snapshot) {
		DumpWords(scope, scene, tick, "pairs", snapshot.Pairs);
		DumpWords(scope, scene, tick, "manifolds", snapshot.Manifolds);
		DumpWords(scope, scene, tick, "events", snapshot.Events);
		DumpWords(scope, scene, tick, "bodies", snapshot.Bodies);
	}

	inline void Append(std::vector<uint64_t> &words, float value) {
		words.push_back(std::bit_cast<uint32_t>(value));
	}

	inline void Append(std::vector<uint64_t> &words, core::Vector3 value) {
		Append(words, value.X);
		Append(words, value.Y);
		Append(words, value.Z);
	}

	inline bool SameSnapshot(const CellSizeSnapshot &left, const CellSizeSnapshot &right) {
		return left.Pairs == right.Pairs && left.Manifolds == right.Manifolds &&
			   left.Events == right.Events && left.Bodies == right.Bodies;
	}

	inline CellSizeSnapshot Capture(ecs::Store &store) {
		CellSizeSnapshot result;
		const PhysicsWorld &world = *store.Resource<PhysicsWorld>();
		for (const CandidatePair &pair : world.Pairs()) {
			result.Pairs.push_back(pair.A.Id);
			result.Pairs.push_back(pair.B.Id);
		}
		for (const ContactManifold &manifold : world.Manifolds()) {
			auto &words = result.Manifolds;
			words.push_back(manifold.A.Id);
			words.push_back(manifold.B.Id);
			Append(words, manifold.Normal);
			words.push_back(manifold.PointCount);
			words.push_back(manifold.Trigger);
			for (size_t point = 0; point < manifold.PointCount; point++) {
				const ContactPoint &contact = manifold.Points[point];
				Append(words, contact.Position);
				Append(words, contact.Penetration);
				Append(words, contact.Separation);
				words.push_back(contact.Feature);
			}
		}
		for (const ContactEvent &event : world.Events()) {
			result.Events.push_back(event.A.Id);
			result.Events.push_back(event.B.Id);
			result.Events.push_back(static_cast<uint8_t>(event.Phase));
		}

		std::vector<ecs::Entity> entities;
		store.Query<const scene::Transform>().Each([&](ecs::Entity entity, const scene::Transform &) {
			entities.push_back(entity);
		});
		std::sort(entities.begin(), entities.end(), [](ecs::Entity a, ecs::Entity b) { return a.Id < b.Id; });
		for (const ecs::Entity entity : entities) {
			auto &words = result.Bodies;
			words.push_back(entity.Id);
			const core::CFrame &frame = store.Get<scene::Transform>(entity)->Frame;
			Append(words, frame.Position);
			Append(words, frame.QuaternionX);
			Append(words, frame.QuaternionY);
			Append(words, frame.QuaternionZ);
			Append(words, frame.QuaternionW);
			const scene::Motion *motion = store.Get<scene::Motion>(entity);
			words.push_back(motion != nullptr);
			if (motion != nullptr) {
				Append(words, motion->Linear);
				Append(words, motion->Angular);
			}
			const scene::RigidBody *body = store.Get<scene::RigidBody>(entity);
			words.push_back(body != nullptr);
			if (body != nullptr) {
				Append(words, body->AppliedForce);
				Append(words, body->AppliedTorque);
				Append(words, body->Mass);
				Append(words, body->LinearDamping);
				Append(words, body->AngularDamping);
				words.push_back(static_cast<uint8_t>(body->Kind));
			}
			words.push_back(store.Get<scene::Simulated>(entity) != nullptr);
		}
		return result;
	}
}
