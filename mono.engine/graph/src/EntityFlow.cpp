#include <engine/core/Log.hpp>
#include <engine/core/Metrics.hpp>
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

		// **Zero keeps everything.** An unset filter is the common case - a node
		// somebody dropped in and has not configured - and the alternative
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

	EntityNodeRun RunEntityNode(
		const RenderGraph &graph,
		const Node &node,
		std::span<const scene::DrawInstance> instances,
		const Viewpoint &fallback,
		float aspect,
		EntityFlow &entities,
		Viewpoints &viewpoints
	) {
		const auto firstResource = [&](std::span<const ResourceId> ids, ResourceKind kind) {
			for (const ResourceId id : ids) {
				const ResourceDesc *resource = graph.FindResource(id);
				if (resource != nullptr && resource->Kind == kind) {
					return resource->Name;
				}
			}
			return core::Name{};
		};
		const auto viewpointFor = [&](std::span<const ResourceId> reads) {
			Viewpoint viewpoint;
			const core::Name input = firstResource(reads, ResourceKind::Camera);
			return input.IsValid() && viewpoints.Get(input, viewpoint) ? viewpoint : fallback;
		};

		const core::Name cameraOutput = firstResource(node.Writes, ResourceKind::Camera);
		if (cameraOutput.IsValid()) {
			viewpoints.Set(cameraOutput, viewpointFor(node.Reads));
			EntityNodeRun result;
			result.Handled = true;
			return result;
		}

		const core::Name output = firstResource(node.Writes, ResourceKind::Entities);
		const std::string_view kind = node.Kind.Text();
		if (!output.IsValid() || (kind != "entities" && kind != "cull-frustum" && kind != "cull-distance" &&
								  kind != "filter-tag" && kind != "order-draw")) {
			// The node stays in the graph, runs every frame, and produces
			// nothing. A pipeline missing half its objects looks exactly like
			// one whose cull is too aggressive.
			ENGINE_WARN_EVERY(
				5.0,
				"'{}' is not an entity node this build runs (kind '{}'); it produces nothing",
				node.Name.Text(),
				kind
			);
			return {};
		}

		const core::Name input = firstResource(node.Reads, ResourceKind::Entities);
		std::span<const uint32_t> source = entities.Get(input);
		std::vector<uint32_t> aliasedSource;
		if (input == output) {
			aliasedSource.assign(source.begin(), source.end());
			source = aliasedSource;
		}
		std::vector<uint32_t> &into = entities.Open(output);

		EntityNodeRun result{.Handled = true, .Output = output};
		if (kind == "entities") {
			AllEntities(instances.size(), into);
		} else if (kind == "cull-frustum") {
			const std::string *mode = node.Parameter(core::Name("culling"));
			if (mode != nullptr && *mode == "none") {
				into.assign(source.begin(), source.end());
			} else {
				FilterByFrustum(
					instances,
					source,
					Frustum::FromViewProjection(ViewProjectionOf(viewpointFor(node.Reads), aspect)),
					into
				);
			}
		} else if (kind == "cull-distance") {
			FilterByDistance(
				instances,
				source,
				viewpointFor(node.Reads).Frame.Position,
				node.Number(core::Name("radius"), 0.0f),
				into
			);
		} else if (kind == "filter-tag") {
			FilterByTag(instances, source, node.Integer(core::Name("mask"), 0), into);
		} else {
			result.Ordered = true;
			result.Opaque = OrderEntities(instances, source, viewpointFor(node.Reads).Frame.Position, into);
		}

		result.Count = into.size();

		// **Entities in against entities out, per pass.** "Why did half the
		// world stop drawing" is answered by which pass dropped them, and the
		// two counts are already here.
		core::Metrics::Count("graph.entities.out", static_cast<double>(into.size()));
		ENGINE_TRACE("'{}' ({}): {} entities in, {} out", node.Name.Text(), kind, source.size(), into.size());
		return result;
	}
}
