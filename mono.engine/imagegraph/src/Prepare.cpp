#include "Execution.hpp"
#include "Inputs.hpp"

#include <engine/core/Profiling.hpp>

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace engine::imagegraph {
	namespace {
		bool AdmissionFailure(Diagnostic &diagnostic, std::string_view node, std::string_view message) {
			diagnostic = {std::string(node.substr(0, 128)), std::string(message.substr(0, 4096))};
			return false;
		}
	}
	bool Prepare(
		const Document &authored,
		const Plan &plan,
		std::string_view output,
		std::span<const SourceExtent> sources,
		ExecutionPlan &out,
		Diagnostic &diagnostic
	) {
		ENGINE_PROFILE("imagegraph execution admission");
		if (sources.size() > Limits::MaximumNodes)
			return AdmissionFailure(diagnostic, {}, "source extent count exceeds limits");
		try {
			Document resolved;
			const Document *document = &authored;
			if (!authored.Parameters.empty() || !authored.Bindings.empty()) {
				if (!ResolveInputs(authored, {}, resolved, diagnostic)) return false;
				document = &resolved;
			}
			Plan checked;
			if (!detail::CompileResolved(*document, checked, diagnostic)) return false;
			if (checked != plan)
				return AdmissionFailure(diagnostic, {}, "compile plan does not match the document");
			size_t selected = 0;
			if (output.empty()) {
				if (document->Outputs.size() != 1)
					return AdmissionFailure(diagnostic, {}, "select one named output explicitly");
			} else {
				const auto found = std::find_if(
					document->Outputs.begin(), document->Outputs.end(), [&](const Output &binding) {
						return binding.Name == output;
					}
				);
				if (found == document->Outputs.end())
					return AdmissionFailure(diagnostic, {}, "selected output does not exist");
				selected = size_t(found - document->Outputs.begin());
			}
			ExecutionPlan execution;
			execution.Target = plan.Outputs[selected];
			execution.Output = selected;
			execution.Extents.resize(document->Nodes.size());
			std::vector<uint8_t> needed(document->Nodes.size());
			needed[execution.Target] = 1;
			for (auto node = plan.Order.rbegin(); node != plan.Order.rend(); ++node)
				if (needed[*node])
					for (size_t input : plan.Inputs[*node])
						needed[input] = 1;
			std::unordered_map<std::string_view, const SourceExtent *> supplied;
			for (const auto &source : sources) {
				const auto node =
					std::find_if(document->Nodes.begin(), document->Nodes.end(), [&](const Node &candidate) {
						return candidate.Id == source.Node;
					});
				if (node == document->Nodes.end() || !std::holds_alternative<Source>(node->Value) ||
					!supplied.emplace(source.Node, &source).second)
					return AdmissionFailure(
						diagnostic,
						source.Node,
						"source extent names missing/non-source node or is duplicated"
					);
				if (!source.Width || !source.Height || source.Width > Limits::MaximumDimension ||
					source.Height > Limits::MaximumDimension ||
					uint64_t(source.Width) * source.Height * 4 > Limits::MaximumImageBytes)
					return AdmissionFailure(diagnostic, source.Node, "source dimensions exceed image limits");
			}
			for (size_t index : plan.Order) {
				if (!needed[index]) continue;
				const auto &node = document->Nodes[index];
				ImageExtent extent;
				if (std::holds_alternative<Source>(node.Value)) {
					const auto found = supplied.find(node.Id);
					if (found == supplied.end())
						return AdmissionFailure(
							diagnostic, node.Id, "reachable source requires exact dimensions"
						);
					const auto &source = *found->second;
					extent = {source.Width, source.Height, size_t(source.Width) * source.Height * 4};
				} else {
					ImageExtent input;
					if (!plan.Inputs[index].empty()) input = execution.Extents[plan.Inputs[index][0]];
					if (plan.Inputs[index].size() == 2) {
						const auto &foreground = execution.Extents[plan.Inputs[index][1]];
						if (input.Width != foreground.Width || input.Height != foreground.Height)
							return AdmissionFailure(
								diagnostic, node.Id, "blend inputs require matching canvas dimensions"
							);
					}
					extent = detail::Extent(node.Value, input);
				}
				const uint64_t work =
					uint64_t(extent.Width) * extent.Height * detail::WorkPerPixel(node.Value);
				if (extent.Bytes > Limits::MaximumRetainedBytes - execution.RetainedBytes)
					return AdmissionFailure(diagnostic, node.Id, "retained image budget exceeded");
				if (work > Limits::MaximumPixelWork - execution.PixelWork)
					return AdmissionFailure(diagnostic, node.Id, "pixel work budget exceeded");
				execution.RetainedBytes += extent.Bytes;
				execution.PixelWork += work;
				execution.Extents[index] = extent;
				execution.Order.push_back(index);
			}
			out = std::move(execution);
			diagnostic = {};
			return true;
		} catch (const std::exception &error) {
			return AdmissionFailure(diagnostic, {}, error.what());
		}
	}
}
