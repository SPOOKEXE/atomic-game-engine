#pragma once

// Public selection values with change tracking. Ordinary reads expose no
// mutable alias to the stored values.
// Code that previously held a mutable vector reference can copy the selection,
// edit the copy and assign it back. World indices are value proxies, not
// mutable uint32_t references.

#include <engine/ecs/Entity.hpp>
#include <engine/world/World.hpp>

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <stdexcept>
#include <utility>
#include <vector>

namespace studio {
	// A root-selection flag tied to an editor's selection revision.
	class SelectionRootFlag {
	  public:
		// Copies are detached snapshots; assignment retains the destination revision.
		//@{
		explicit SelectionRootFlag(uint64_t *revision = nullptr) : Revision(revision) {}
		SelectionRootFlag(const SelectionRootFlag &other) : Value(other.Value) {}
		SelectionRootFlag &operator=(const SelectionRootFlag &other) {
			return *this = other.Value;
		}
		SelectionRootFlag &operator=(bool value) {
			if (Value != value) {
				Value = value;
				if (Revision != nullptr) ++*Revision;
			}
			return *this;
		}
		operator bool() const {
			return Value;
		}
		//@}

	  private:
		bool Value = false;
		uint64_t *Revision = nullptr;
	};
	// A world index that tracks direct writes as well as whole-world assignment.
	class SelectionIndex {
	  public:
		// Copies are detached snapshots; assignment retains the destination revision.
		//@{
		explicit SelectionIndex(uint64_t *revision = nullptr) : Revision(revision) {}
		SelectionIndex(const SelectionIndex &other) : Value(other.Value) {}
		SelectionIndex &operator=(const SelectionIndex &other) {
			return *this = other.Value;
		}
		SelectionIndex &operator=(uint32_t value) {
			if (Value != value) {
				Value = value;
				if (Revision != nullptr) ++*Revision;
			}
			return *this;
		}
		operator uint32_t() const {
			return Value;
		}
		//@}

	  private:
		uint32_t Value = engine::world::WorldId::INVALID;
		uint64_t *Revision = nullptr;
	};

	// A world handle whose index cannot be changed without updating the revision.
	class SelectionWorldHandle {
	  public:
		// Copies are detached snapshots; assignment retains the destination revision.
		//@{
		explicit SelectionWorldHandle(uint64_t *revision = nullptr) : Index(revision) {}
		SelectionWorldHandle(const SelectionWorldHandle &other) : Index(other.Index) {}
		SelectionWorldHandle &operator=(const SelectionWorldHandle &other) {
			Index = other.Index;
			return *this;
		}
		SelectionWorldHandle &operator=(engine::world::WorldId world) {
			Index = world.Index;
			return *this;
		}
		SelectionWorldHandle &operator=(std::initializer_list<uint32_t> index) {
			if (index.size() > 1) throw std::invalid_argument("one world index expected");
			Index = index.size() == 0 ? engine::world::WorldId::INVALID : *index.begin();
			return *this;
		}
		operator engine::world::WorldId() const {
			return engine::world::WorldId{Index};
		}
		bool IsValid() const {
			return static_cast<engine::world::WorldId>(*this).IsValid();
		}
		bool operator==(engine::world::WorldId other) const {
			return static_cast<engine::world::WorldId>(*this) == other;
		}
		bool operator!=(engine::world::WorldId other) const {
			return !(*this == other);
		}
		//@}
		// The tracked index, including for callers assigning it directly.
		SelectionIndex Index;
	};

	// An ordered instance selection. Iteration and element access are read-only.
	class SelectionList {
	  public:
		// Standard selection value and const iterator types.
		//@{
		using Entity = engine::ecs::Entity;
		using const_iterator = std::vector<Entity>::const_iterator;
		//@}
		// Copies are detached snapshots; assignment retains the destination revision.
		//@{
		explicit SelectionList(uint64_t *revision = nullptr) : Revision(revision) {}
		SelectionList(const SelectionList &other) : Values(other.Values) {}
		SelectionList &operator=(const SelectionList &other) {
			return *this = other.Values;
		}
		SelectionList &operator=(const std::vector<Entity> &values) {
			if (Values != values) {
				Values = values;
				Changed();
			}
			return *this;
		}
		SelectionList &operator=(std::vector<Entity> &&values) {
			if (Values != values) {
				Values = std::move(values);
				Changed();
			}
			return *this;
		}
		SelectionList &operator=(std::initializer_list<Entity> values) {
			if (Values.size() != values.size() || !std::equal(Values.begin(), Values.end(), values.begin())) {
				Values.assign(values.begin(), values.end());
				Changed();
			}
			return *this;
		}
		operator const std::vector<Entity> &() const {
			return Values;
		}
		//@}
		// Read-only access to the ordered instances.
		//@{
		const_iterator begin() const {
			return Values.begin();
		}
		const_iterator end() const {
			return Values.end();
		}
		bool empty() const {
			return Values.empty();
		}
		size_t size() const {
			return Values.size();
		}
		const Entity &front() const {
			return Values.front();
		}
		const Entity &back() const {
			return Values.back();
		}
		const Entity *data() const {
			return Values.data();
		}
		const Entity &operator[](size_t index) const {
			return Values[index];
		}
		//@}
		// Mutations update the revision only when values change.
		//@{
		void clear() {
			if (!Values.empty()) {
				Values.clear();
				Changed();
			}
		}
		void push_back(Entity value) {
			Values.push_back(value);
			Changed();
		}
		const_iterator erase(const_iterator position) {
			auto next = Values.erase(position);
			Changed();
			return next;
		}
		const_iterator erase(const_iterator first, const_iterator last) {
			if (first == last) return first;
			auto next = Values.erase(first, last);
			Changed();
			return next;
		}
		void assign(size_t count, Entity value) {
			*this = std::vector<Entity>(count, value);
		}
		template <class It> void assign(It first, It last) {
			*this = std::vector<Entity>(first, last);
		}
		bool operator==(const std::vector<Entity> &other) const {
			return Values == other;
		}
		//@}

	  private:
		void Changed() {
			if (Revision != nullptr) ++*Revision;
		}
		std::vector<Entity> Values;
		uint64_t *Revision = nullptr;
	};
}
