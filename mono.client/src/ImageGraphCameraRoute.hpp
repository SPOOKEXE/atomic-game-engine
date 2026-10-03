#pragma once

#include <engine/imagegraph/Document.hpp>

#include <algorithm>

namespace client::detail {
	// Unproven cones go through the core row scheduler before any source inputs execute.
	inline bool SourceCameraSingletonCone(
		const engine::imagegraph::Document &document, const engine::imagegraph::Node &camera
	) {
		using namespace engine::imagegraph;
		const auto plain = [&](const Node &node) {
			if (!node.GroupId.empty() || !node.InstanceBase.empty() || !node.SourceInputExpressions.empty() ||
				!node.SourceAnimatedInputs.empty())
				return false;
			for (const auto &value : node.Values)
				if (std::holds_alternative<ArrayValue>(value.Data)) return false;
			for (const auto &value : node.SourceProperties)
				if (std::holds_alternative<ArrayValue>(value.Data)) return false;
			for (const auto &input : node.DynamicInputs)
				if (input.Default && std::holds_alternative<ArrayValue>(*input.Default)) return false;
			return std::none_of(document.Keyframes.begin(), document.Keyframes.end(), [&](const auto &key) {
				return key.NodeId == node.Id;
			});
		};
		if ((camera.Type != "pc.3_d_camera" && camera.Type != "pc.3_d_camera_set") || !plain(camera))
			return false;
		const auto find = [&](std::string_view id) -> const Node * {
			const auto found =
				std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &node) {
					return node.Id == id;
				});
			return found == document.Nodes.end() ? nullptr : &*found;
		};
		for (const auto &link : document.Links) {
			if (link.ToNode != camera.Id) continue;
			if (link.ToPort != "scene" || link.FromPort != "scene") return false;
			const auto *scene = find(link.FromNode);
			if (!scene || scene->Type != "pc.3_d_scene" || !plain(*scene) || scene->DynamicInputs.size() > 32)
				return false;
			// Scene has its own processor. Its gatherer alone does not prove singleton output.
			for (const auto &input : document.Links) {
				if (input.ToNode != scene->Id) continue;
				const auto *object = find(input.FromNode);
				if (!object || !plain(*object)) return false;
				const bool mesh = (object->Type == "pc.3_d_cube" || object->Type == "pc.3_d_mesh_cube") &&
								  input.FromPort == "mesh";
				const bool light =
					(object->Type == "pc.3_d_light_point" || object->Type == "pc.3_d_light_directional") &&
					input.FromPort == "light";
				if (!mesh && !light) return false;
				for (const auto &upstream : document.Links)
					if (upstream.ToNode == object->Id) return false;
			}
		}
		return true;
	}
}
