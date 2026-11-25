#include "ImageComposerGraph.hpp"

#include "ImageComposer.hpp"

#include <engine/imagegraph/AngGraph.hpp>

#include <algorithm>

namespace studio {
	namespace {
		using namespace engine::imagegraph;
	}

	const engine::ang::Registry &ImageComposerTypes() {
		return GraphTypes();
	}

	engine::ang::Value ImageComposerParam(const engine::ang::NodeInstance &node, std::string_view key) {
		return GraphParam(node, key);
	}

	bool LoadImageComposerGraph(
		const engine::imagegraph::Document &document,
		engine::ang::Graph &out,
		engine::imagegraph::Diagnostic &diagnostic
	) {
		return ToAngGraph(document, out, diagnostic);
	}

	bool SaveImageComposerGraph(
		const engine::ang::Graph &graph,
		std::span<const engine::imagegraph::Output> outputs,
		engine::imagegraph::Document &out,
		engine::imagegraph::Diagnostic &diagnostic
	) {
		return FromAngGraph(graph, outputs, out, diagnostic);
	}

	bool SaveImageComposerDocument(const ImageComposerState &state, Document &out, Diagnostic &diagnostic) {
		Document candidate;
		if (!SaveImageComposerGraph(state.Graph, state.Outputs, candidate, diagnostic)) return false;
		candidate.Parameters = state.Parameters;
		candidate.Bindings = state.Bindings;
		candidate.Storage = state.Storage;
		Plan plan;
		if (!Compile(candidate, plan, diagnostic)) return false;
		out = std::move(candidate);
		return true;
	}

	bool BindImageComposerInput(
		ImageComposerState &state,
		std::string_view id,
		std::string_view property,
		std::string_view input,
		Diagnostic &diagnostic
	) {
		const auto *node = state.Graph.FindNode(id);
		if (node == nullptr || input.empty()) {
			diagnostic = {{}, "select a node and name its input"};
			return false;
		}
		Operation operation;
		if (!ReadGraphOperation(*node, operation, diagnostic)) return false;
		const int kind = engine::imagegraph::PropertyType(operation, property);
		if (kind < 0) {
			diagnostic = {node->Id, "property cannot be bound"};
			return false;
		}
		InputValue initial;
		if (!GetProperty(operation, property, initial)) return false;
		Document candidate;
		if (!SaveImageComposerDocument(state, candidate, diagnostic)) return false;
		if (std::none_of(
				candidate.Parameters.begin(), candidate.Parameters.end(), [&](const auto &parameter) {
					return parameter.Name == input;
				}
			))
			candidate.Parameters.push_back({std::string(input), std::move(initial)});
		std::erase_if(candidate.Bindings, [&](const auto &binding) {
			return binding.Node == node->Id && binding.Property == property;
		});
		candidate.Bindings.push_back({node->Id, std::string(property), std::string(input)});
		Plan plan;
		if (!Compile(candidate, plan, diagnostic)) return false;
		state.Parameters = std::move(candidate.Parameters);
		state.Bindings = std::move(candidate.Bindings);
		CommitImageComposer(state);
		return true;
	}

	bool SetImageComposerOutput(ImageComposerState &state, std::string_view name, std::string_view id) {
		const auto *node = state.Graph.FindNode(id);
		if (node == nullptr || name.empty()) return false;
		const auto found = std::find_if(state.Outputs.begin(), state.Outputs.end(), [&](const auto &output) {
			return output.Name == name;
		});
		if (found != state.Outputs.end())
			found->Node = node->Id;
		else {
			if (state.Outputs.size() >= engine::imagegraph::Limits::MaximumOutputs) return false;
			state.Outputs.push_back({std::string(name), node->Id});
		}
		state.SelectedOutput = name;
		CommitImageComposer(state);
		return true;
	}
}
