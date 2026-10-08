#pragma once

#include <engine/imagegraph/Document.hpp>

#include <algorithm>
#include <array>

namespace engine::imagegraph {
	struct StatefulTemporalCone {
		bool Valid = true;
		bool Simulation = false;
		bool FixedSimulationSteps = false;
		size_t Nodes = 0;
		size_t SurfaceCaches = 0;
		size_t RandomGenerators = 0;
		size_t DataProcessors = 0;
		size_t SourceFrameCaches = 0;
		size_t RigidActors = 0;
		// First-frame curve captures require bounded replay from frame zero on a fresh seek.
		bool FirstFrameData = false;
	};
	// Follows the compiled union of output dependencies, including group allocation and inline owners.
	// Bounded stack storage keeps temporal admission independent of graph traversal allocations.
	inline StatefulTemporalCone AnalyzeStatefulTemporalCone(
		const Document &document,
		const Plan &plan,
		std::span<const std::string> outputIds,
		std::string_view selectedNodeId = {},
		std::span<const std::string_view> actionNodeIds = {}
	) {
		StatefulTemporalCone cone;
		if (document.Nodes.size() > Limits::MaximumNodes || outputIds.size() > Limits::MaximumOutputs ||
			actionNodeIds.size() > Limits::MaximumNodes) {
			cone.Valid = false;
			return cone;
		}
		std::array<bool, Limits::MaximumNodes> visited{};
		std::array<size_t, Limits::MaximumNodes> pending{};
		size_t count = 0;
		const auto addIndex = [&](size_t index) {
			if (index >= document.Nodes.size()) {
				cone.Valid = false;
				return;
			}
			if (!visited[index]) {
				visited[index] = true;
				pending[count++] = index;
			}
		};
		const auto addNode = [&](std::string_view id) {
			for (size_t index = 0; index < document.Nodes.size(); index++)
				if (document.Nodes[index].Id == id) {
					addIndex(index);
					return;
				}
			cone.Valid = false;
		};
		if (!selectedNodeId.empty()) addNode(selectedNodeId);
		for (const auto id : actionNodeIds)
			addNode(id);
		for (const auto &id : outputIds) {
			const auto output =
				std::find_if(document.Outputs.begin(), document.Outputs.end(), [&](const auto &candidate) {
					return candidate.Id == id;
				});
			if (output == document.Outputs.end()) {
				cone.Valid = false;
				continue;
			}
			addNode(output->NodeId);
		}
		while (count) {
			const size_t index = pending[--count];
			const auto &node = document.Nodes[index];
			++cone.Nodes;
			cone.SurfaceCaches += node.Type == "pc.interlaced" || node.Type == "pc.time_remap" ||
								  node.Type == "pc.anim_loop" || node.Type == "pc.delay" ||
								  node.Type == "pc.rate_remap" || node.Type == "pc.revert" ||
								  node.Type == "pc.stagger";
			cone.SourceFrameCaches += node.Type == "pc.cache" || node.Type == "pc.cache_array";
			cone.RandomGenerators += node.Type == "pc.random";
			cone.RigidActors += node.Type.starts_with("pc.rigid_");
			cone.DataProcessors += node.Type.starts_with("pc.strand_") || node.Type == "pc.trigger_bool" ||
								   node.Type == "pc.differential" || node.Type == "pc.counter" ||
								   node.Type == "pc.delay_value" || node.Type == "pc.cache_value_array" ||
								   node.Type == "pc.cache_results" || node.Type == "pc.text" ||
								   node.Type == "pc.cache" || node.Type == "pc.cache_array" ||
								   node.Type == "pc.3_d_affector" || node.Type == "pc.3_d_particle" ||
								   node.Type == "pc.particle" || node.Type == "pc.segment_filter" ||
								   node.Type == "pc.path_blend" || node.Type == "pc.path_to_curve" ||
								   node.Type == "pc.path_redistribute" || node.Type == "pc.path_skew" ||
								   node.Type == "pc.path_map_area" || node.Type == "pc.path_shape_3_d" ||
								   node.Type == "pc.path_extends" || node.Type == "pc.path_flattern" ||
								   node.Type == "pc.path_smoothen" || node.Type == "pc.path_spiral" ||
								   node.Type == "pc.path_wave" || node.Type == "pc.path_sample" ||
								   node.Type == "pc.crop_content" || node.Type == "pc.smear" ||
								   node.Type == "pc.tunnel_in" || node.Type == "pc.tunnel_out";
			cone.FirstFrameData |= node.Type == "pc.3_d_particle" || node.Type == "pc.3_d_affector" ||
								   node.Type == "pc.particle" || node.Type == "pc.crop_content" ||
								   node.Type == "pc.smear" || node.Type == "pc.path_extends" ||
								   node.Type == "pc.path_flattern" || node.Type == "pc.path_smoothen" ||
								   node.Type == "pc.path_sample" || node.Type == "pc.path_spiral" ||
								   node.Type == "pc.path_wave";
			cone.Simulation |= node.Type == "image.verlet_simple" ||
							   (node.Type.starts_with("pc.strand_") || node.Type.starts_with("pc.verlet_") ||
								node.Type.starts_with("pc.flip_"));
			cone.FixedSimulationSteps |= node.Type == "image.verlet_simple" ||
										 node.Type == "pc.strand_update" ||
										 node.Type == "pc.verlet_sim_step" || node.Type == "pc.flip_update";
			for (const auto &link : plan.EffectiveLinks)
				if (link.ToNode == node.Id) addNode(link.FromNode);
			for (const auto &dependency : plan.GroupSurfaceDependencies)
				if (dependency.Consumer == index) addIndex(dependency.Producer);
			for (const auto &dependency : plan.InlineOwnerDependencies)
				if (dependency.Consumer == index && !dependency.ControlsOnly) addIndex(dependency.Owner);
			for (const auto &dependency : plan.InlineControlDependencies)
				if (dependency.Consumer == index) addIndex(dependency.Producer);
			for (const auto &dependency : plan.PcxNamedDependencies)
				if (dependency.Consumer == index) addIndex(dependency.Producer);
		}
		return cone;
	}
}
