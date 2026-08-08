#include <engine/graph/Cull.hpp>
#include <engine/graph/EntityFlow.hpp>

#include <algorithm>

namespace engine::graph {

	void EntityFlow::Clear() {
		// **The storage is kept and the lists are emptied**, because this runs
		// once per view per frame and the vectors are the only allocation in the
		// whole of this file. A steady scene settles to zero.
		for (EntityList &list : Lists) {
			list.Indices.clear();
		}
		Live = 0;
	}

	void EntityFlow::Set(core::Name name, std::span<const uint32_t> indices) {
		std::vector<uint32_t> &into = Open(name);
		into.assign(indices.begin(), indices.end());
	}

	std::vector<uint32_t> &EntityFlow::Open(core::Name name) {
		for (size_t index = 0; index < Live; index++) {
			if (Lists[index].Name == name) {
				Lists[index].Indices.clear();
				return Lists[index].Indices;
			}
		}

		// Reuse a slot a previous view left behind before growing.
		if (Live < Lists.size()) {
			EntityList &list = Lists[Live++];
			list.Name = name;
			list.Indices.clear();
			return list.Indices;
		}

		Lists.push_back(EntityList{name, {}});
		Live = Lists.size();
		return Lists.back().Indices;
	}

	std::span<const uint32_t> EntityFlow::Get(core::Name name) const {
		for (size_t index = 0; index < Live; index++) {
			if (Lists[index].Name == name) {
				return Lists[index].Indices;
			}
		}
		return {};
	}

	bool EntityFlow::Has(core::Name name) const {
		for (size_t index = 0; index < Live; index++) {
			if (Lists[index].Name == name) {
				return true;
			}
		}
		return false;
	}

	void Viewpoints::Clear() {
		Live = 0;
	}

	void Viewpoints::Set(core::Name name, const Viewpoint &viewpoint) {
		for (size_t index = 0; index < Live; index++) {
			if (Entries[index].Name == name) {
				Entries[index].Value = viewpoint;
				return;
			}
		}

		if (Live < Entries.size()) {
			Entries[Live].Name = name;
			Entries[Live].Value = viewpoint;
			Live++;
			return;
		}

		Entries.push_back(Entry{name, viewpoint});
		Live = Entries.size();
	}

	bool Viewpoints::Get(core::Name name, Viewpoint &out) const {
		for (size_t index = 0; index < Live; index++) {
			if (Entries[index].Name == name) {
				out = Entries[index].Value;
				return true;
			}
		}
		return false;
	}

	glm::mat4 ViewProjectionOf(const Viewpoint &viewpoint, float aspect) {
		// **A fitted projection wins, and it has to.** A light's box comes from
		// the scene bound rather than from a field of view, so resolving it
		// through a lens would replace the thing that makes shadows cover the
		// scene with a guess about how wide the light is.
		if (viewpoint.Fitted) {
			return viewpoint.Projection;
		}
		return scene::ResolveCamera(viewpoint.Frame, viewpoint.Lens, aspect).ViewProjection;
	}

	void AllEntities(size_t count, std::vector<uint32_t> &into) {
		into.resize(count);
		for (size_t index = 0; index < count; index++) {
			into[index] = static_cast<uint32_t>(index);
		}
	}

	size_t FilterByFrustum(
		std::span<const scene::DrawInstance> instances,
		std::span<const uint32_t> from,
		const Frustum &frustum,
		std::vector<uint32_t> &into
	) {
		into.clear();
		into.reserve(from.size());

		for (const uint32_t index : from) {
			if (index >= instances.size()) {
				continue;
			}
			if (frustum.Intersects(BoundsOf(instances[index]))) {
				into.push_back(index);
			}
		}
		return into.size();
	}

	size_t FilterByTag(
		std::span<const scene::DrawInstance> instances,
		std::span<const uint32_t> from,
		uint32_t mask,
		std::vector<uint32_t> &into
	) {
		into.clear();

		// **Zero keeps everything.** An unset filter is the common case — a node
		// somebody dropped in and has not configured — and the alternative
		// reading, "match nothing", turns that into a black frame with no
		// explanation.
		if (mask == 0) {
			into.assign(from.begin(), from.end());
			return into.size();
		}

		into.reserve(from.size());
		for (const uint32_t index : from) {
			if (index >= instances.size()) {
				continue;
			}
			if ((instances[index].TagMask & mask) != 0) {
				into.push_back(index);
			}
		}
		return into.size();
	}

	size_t FilterByDistance(
		std::span<const scene::DrawInstance> instances,
		std::span<const uint32_t> from,
		const core::Vector3 &eye,
		float radius,
		std::vector<uint32_t> &into
	) {
		into.clear();

		// Non-positive keeps everything, for `FilterByTag`'s reason: an
		// unconfigured node should be a no-op rather than an empty frame.
		if (radius <= 0.0f) {
			into.assign(from.begin(), from.end());
			return into.size();
		}

		// Squared, because the square root is monotonic and this runs over every
		// instance every frame per view.
		const float limit = radius * radius;

		into.reserve(from.size());
		for (const uint32_t index : from) {
			if (index >= instances.size()) {
				continue;
			}
			if ((instances[index].Frame.Position - eye).MagnitudeSquared() <= limit) {
				into.push_back(index);
			}
		}
		return into.size();
	}

	size_t OrderEntities(
		std::span<const scene::DrawInstance> instances,
		std::span<const uint32_t> from,
		const core::Vector3 &eye,
		std::vector<uint32_t> &into
	) {
		// **`scene::OrderSubset`, not a second copy of the sort.** What a
		// transparent pane looks like is decided in one place; this is the node
		// that applies it to whatever the filters left.
		return scene::OrderSubset(instances, from, eye, into);
	}
}
