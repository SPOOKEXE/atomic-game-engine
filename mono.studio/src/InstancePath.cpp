#include <algorithm>
#include <studio/InstancePath.hpp>

namespace studio {

	using engine::ecs::Entity;
	using engine::ecs::NULL_ENTITY;
	using engine::ecs::Store;

	namespace {
		// The most names one path may have. A tree this deep is a tree nobody
		// authored, and the cap is what stops a cycle in the parent chain
		// becoming a walk that does not end.
		constexpr size_t MAXIMUM_DEPTH = 64;
	}

	InstancePath PathOf(const Store &store, Entity instance) {
		if (instance == NULL_ENTITY || !store.Alive(instance)) {
			return {};
		}

		InstancePath path;
		for (Entity walk = instance; walk != NULL_ENTITY && store.Alive(walk); walk = store.ParentOf(walk)) {
			path.emplace_back(store.InstanceNameOf(walk).Text());
			if (path.size() > MAXIMUM_DEPTH) {
				// A cycle, or a tree past anything anybody authored. Refused
				// rather than truncated: half a path resolves somewhere, and
				// somewhere is worse than nowhere.
				return {};
			}
		}

		std::reverse(path.begin(), path.end());
		return path;
	}

	Entity ResolvePath(const Store &store, const InstancePath &path) {
		if (path.empty()) {
			return NULL_ENTITY;
		}

		Entity found = NULL_ENTITY;
		store.EachRoot([&](Entity root) {
			if (found == NULL_ENTITY && store.InstanceNameOf(root).Text() == path.front()) {
				found = root;
			}
		});

		for (size_t index = 1; index < path.size() && found != NULL_ENTITY; ++index) {
			Entity next = NULL_ENTITY;
			store.EachChild(found, [&](Entity child) {
				// **The first match wins.** Two siblings may share a name, so a
				// path is not a key - it is the best identity available without
				// the document format carrying one, and where it is ambiguous
				// the ambiguity was already there in what a person sees.
				if (next == NULL_ENTITY && store.InstanceNameOf(child).Text() == path[index]) {
					next = child;
				}
			});
			found = next;
		}

		return found;
	}

	bool Contains(const InstancePath &ancestor, const InstancePath &path) {
		// **An empty ancestor contains nothing.** A lock over nothing must not
		// read as a lock over everything, which is what a plain prefix test
		// would make it.
		if (ancestor.empty() || ancestor.size() > path.size()) {
			return false;
		}
		for (size_t index = 0; index < ancestor.size(); ++index) {
			if (ancestor[index] != path[index]) {
				return false;
			}
		}
		return true;
	}

	bool Overlaps(const InstancePath &left, const InstancePath &right) {
		return Contains(left, right) || Contains(right, left);
	}

	std::string Describe(const InstancePath &path) {
		if (path.empty()) {
			return "nothing";
		}

		std::string text;
		for (const std::string &name : path) {
			if (!text.empty()) {
				text.push_back('.');
			}
			text += name;
		}
		return text;
	}
}
