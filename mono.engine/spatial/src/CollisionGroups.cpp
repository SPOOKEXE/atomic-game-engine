#include <engine/spatial/CollisionGroups.hpp>

#include <algorithm>
#include <mutex>

namespace engine::spatial {

	namespace {
		struct Table {
			std::mutex Guard;

			// Index to name. Position in this vector *is* the layer index, which
			// is why nothing is ever removed — an erase would renumber every
			// group above it and silently rewrite every `Collider::Layer` in
			// every world that had already been built.
			std::vector<core::Name> Groups;

			// One mask per group: which groups it collides with. The matrix is
			// held twice, once from each side, because that is what
			// `LayerMask::Overlaps` reads — and `SetCollidable` writing both is
			// what keeps the two halves in step.
			std::vector<LayerMask> Matrix;

			bool Started = false;
		};

		Table &Registry() {
			static Table table;
			return table;
		}

		// Caller holds the lock.
		void EnsureDefault(Table &table) {
			if (table.Started) {
				return;
			}

			table.Started = true;
			table.Groups.push_back(core::Name(CollisionGroups::DEFAULT));
			table.Matrix.push_back(LayerMask::All());
		}

		// Caller holds the lock.
		uint32_t Find(const Table &table, core::Name name) {
			const auto found = std::find(table.Groups.begin(), table.Groups.end(), name);
			return found == table.Groups.end()
					   ? NO_GROUP
					   : static_cast<uint32_t>(std::distance(table.Groups.begin(), found));
		}
	}

	uint32_t CollisionGroups::Register(std::string_view name) {
		Table &table = Registry();
		const std::lock_guard lock(table.Guard);
		EnsureDefault(table);

		const core::Name key(name);
		if (const uint32_t existing = Find(table, key); existing != NO_GROUP) {
			return existing;
		}

		if (table.Groups.size() >= LayerMask::LAYER_COUNT) {
			// Refused rather than folded onto an existing bit. Two unrelated
			// groups sharing one layer is a physics bug in a scene neither
			// author was looking at.
			return NO_GROUP;
		}

		const auto index = static_cast<uint32_t>(table.Groups.size());
		table.Groups.push_back(key);

		// Collides with everything, matching Roblox. A group that collided with
		// nothing until configured would look broken rather than new.
		table.Matrix.push_back(LayerMask::All());
		return index;
	}

	uint32_t CollisionGroups::IndexOf(core::Name name) {
		Table &table = Registry();
		const std::lock_guard lock(table.Guard);
		EnsureDefault(table);

		return Find(table, name);
	}

	core::Name CollisionGroups::NameOf(uint32_t index) {
		Table &table = Registry();
		const std::lock_guard lock(table.Guard);
		EnsureDefault(table);

		return index < table.Groups.size() ? table.Groups[index] : core::Name{};
	}

	bool CollisionGroups::SetCollidable(core::Name first, core::Name second, bool collidable) {
		Table &table = Registry();
		const std::lock_guard lock(table.Guard);
		EnsureDefault(table);

		const uint32_t left = Find(table, first);
		const uint32_t right = Find(table, second);
		if (left == NO_GROUP || right == NO_GROUP) {
			return false;
		}

		const LayerMask rightBit = LayerMask::Only(right);
		const LayerMask leftBit = LayerMask::Only(left);

		// **Both sides, always.** A pair is considered only when each side's
		// layer is in the other's mask, so a one-sided setting produces a pair
		// one collider believes in and the other does not.
		if (collidable) {
			table.Matrix[left] = LayerMask(table.Matrix[left].Bits | rightBit.Bits);
			table.Matrix[right] = LayerMask(table.Matrix[right].Bits | leftBit.Bits);
		} else {
			table.Matrix[left] = LayerMask(table.Matrix[left].Bits & ~rightBit.Bits);
			table.Matrix[right] = LayerMask(table.Matrix[right].Bits & ~leftBit.Bits);
		}
		return true;
	}

	bool CollisionGroups::Collidable(core::Name first, core::Name second) {
		Table &table = Registry();
		const std::lock_guard lock(table.Guard);
		EnsureDefault(table);

		const uint32_t left = Find(table, first);
		const uint32_t right = Find(table, second);
		if (left == NO_GROUP || right == NO_GROUP) {
			return false;
		}
		return (table.Matrix[left].Bits & LayerMask::Only(right).Bits) != 0;
	}

	LayerMask CollisionGroups::MaskFor(uint32_t index) {
		Table &table = Registry();
		const std::lock_guard lock(table.Guard);
		EnsureDefault(table);

		// `All()` for an unregistered index, which is what an unconfigured
		// collider already has. Returning `None()` would make a part quietly
		// stop colliding with everything because of a lookup failure.
		return index < table.Matrix.size() ? table.Matrix[index] : LayerMask::All();
	}

	std::vector<core::Name> CollisionGroups::Names() {
		Table &table = Registry();
		const std::lock_guard lock(table.Guard);
		EnsureDefault(table);

		return table.Groups;
	}

	uint32_t CollisionGroups::Count() {
		Table &table = Registry();
		const std::lock_guard lock(table.Guard);
		EnsureDefault(table);

		return static_cast<uint32_t>(table.Groups.size());
	}

	void CollisionGroups::Reset() {
		Table &table = Registry();
		const std::lock_guard lock(table.Guard);

		table.Groups.clear();
		table.Matrix.clear();
		table.Started = false;
		EnsureDefault(table);
	}
}
