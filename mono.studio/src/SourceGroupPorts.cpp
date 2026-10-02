#include <engine/bake/Pxcx.hpp>
#include <engine/imagegraphio/PxcxImport.hpp>

#include <algorithm>
#include <nlohmann/json.hpp>
#include <studio/ImageGraph.hpp>

namespace studio {
	bool AddSourceImageGraphGroupPort(
		engine::imagegraph::Document &document,
		std::string_view groupId,
		std::string_view controlId,
		engine::imagegraph::PortDirection direction,
		engine::imagegraph::Diagnostic &error
	) try {
		using namespace engine::imagegraph;
		using Json = nlohmann::json;
		error = {};
		const auto reject = [&](Status code, std::string message) {
			error = {code, std::string(groupId), std::string(controlId), std::move(message)};
			return false;
		};
		const auto group =
			std::find_if(document.Groups.begin(), document.Groups.end(), [&](const auto &item) {
				return item.Id == groupId;
			});
		if (group == document.Groups.end()) return reject(Status::InvalidGroup, "group does not exist");
		if (controlId.empty() || controlId.size() + 13 > Limits::MaximumTextBytes ||
			group->Ports.size() >= Limits::MaximumGroupPorts ||
			document.Nodes.size() >= Limits::MaximumNodes ||
			document.Junctions.size() >= Limits::MaximumJunctions ||
			document.Links.size() >= Limits::MaximumLinks)
			return reject(Status::LimitExceeded, "source group socket exceeds authoring bounds");
		if (direction != PortDirection::Input && direction != PortDirection::Output)
			return reject(Status::InvalidGroup, "socket direction is invalid");
		const std::string id(controlId), gid(groupId), junctionId = id + "/parent-value";
		if (std::any_of(
				document.Groups.begin(),
				document.Groups.end(),
				[&](const auto &item) { return item.Id == id || item.Id == junctionId; }
			) ||
			std::any_of(
				document.Nodes.begin(),
				document.Nodes.end(),
				[&](const auto &node) { return node.Id == id || node.Id == junctionId; }
			) ||
			std::any_of(
				document.Junctions.begin(),
				document.Junctions.end(),
				[&](const auto &node) { return node.Id == id || node.Id == junctionId; }
			) ||
			std::any_of(group->Ports.begin(), group->Ports.end(), [&](const auto &port) {
				return port.Id == id;
			}))
			return reject(Status::DuplicateId, "source socket identity already exists");
		const auto bytes = DocumentRetainedPayloadBytes(document);
		if (!bytes || *bytes > Limits::MaximumEvaluationBytes / 2)
			return reject(Status::LimitExceeded, "source socket edit copy exceeds live payload budget");
		const bool input = direction == PortDirection::Input;
		// Import the pinned source constructor shape so flags and synthetic routes have one owner.
		Json root{
			{"nodes",
			 Json::array(
				 {Json{
					  {"id", gid},
					  {"type", "Node_Group"},
					  {"x", 0},
					  {"y", 0},
					  {"inputs", input ? Json::array({Json::object()}) : Json::array()},
					  {"attri",
					   {{"custom_input_list", input ? Json::array({id}) : Json::array()},
						{"custom_output_list", input ? Json::array() : Json::array({id})},
						{"color_depth", 1},
						{"interpolate", 0},
						{"oversample", 0}}}
				  },
				  Json{
					  {"id", id},
					  {"type", input ? "Node_Group_Input" : "Node_Group_Output"},
					  {"group", gid},
					  {"x", 0},
					  {"y", 0},
					  {"inputs", std::vector<Json>(input ? 16 : 1, Json::object())}
				  }}
			 )}
		};
		engine::bake::PxcxArchive archive;
		archive.MetadataNumber = 121092;
		archive.MetadataText = "1.22.10.201";
		archive.GraphJson = root.dump();
		archive.GraphJson.push_back('\0');
		std::vector<std::byte> encoded;
		std::string failure;
		if (!engine::bake::WritePxcx(archive, encoded, failure) ||
			!engine::bake::ReadPxcx(encoded, archive, failure))
			return reject(Status::InvalidValue, std::move(failure));
		engine::imagegraphio::PxcxImport imported;
		if (!engine::imagegraphio::ImportPxcxImageGraph(archive, imported, failure) ||
			imported.Graph.Groups.size() != 1 || imported.Graph.Groups[0].Ports.size() != 1 ||
			imported.Graph.Nodes.size() != 1)
			return reject(
				Status::InvalidGroup,
				failure.empty() ? "source socket constructor did not project" : std::move(failure)
			);
		Document staged = document;
		auto &target = staged.Groups[size_t(group - document.Groups.begin())];
		target.Ports.push_back(std::move(imported.Graph.Groups[0].Ports[0]));
		staged.Nodes.push_back(std::move(imported.Graph.Nodes[0]));
		for (auto &junction : imported.Graph.Junctions)
			staged.Junctions.push_back(std::move(junction));
		for (auto &link : imported.Graph.Links)
			staged.Links.push_back(std::move(link));
		staged.FormatVersion = std::max(staged.FormatVersion, 9u);
		document = std::move(staged);
		return true;
	} catch (const std::bad_alloc &) {
		error = {engine::imagegraph::Status::LimitExceeded, {}, {}, "source socket allocation failed"};
		return false;
	}
}
