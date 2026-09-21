#include <engine/core/FrameGraph.hpp>
#include <engine/graph/PipelineProfile.hpp>

#include <algorithm>
#include <imgui.h>
#include <studio/Editor.hpp>
#include <studio/RenderPipelineGraph.hpp>

namespace studio {

	namespace {
		template <typename Timings>
		double PassTiming(const Timings &timings, const engine::graph::ProfilePass &pass) {
			if (const auto named = timings.find(pass.Name.Id()); named != timings.end()) {
				return named->second;
			}
			if (const auto kind = timings.find(pass.Kind.Id()); kind != timings.end()) {
				return kind->second;
			}
			return 0.0;
		}

		double FrameGraphWallTime(const engine::graph::ProfilePass &pass) {
			double microseconds = 0.0;
			for (const engine::core::FrameSpan &span : engine::core::FrameGraph::Spans())
				if (span.Name == pass.Name.Text() || span.Name == pass.Kind.Text())
					microseconds += static_cast<double>(span.Milliseconds) * 1000.0;
			return microseconds;
		}
	}

	void Editor::DrawRenderPipelineInspector() {
		const std::vector<nodegraph::NodeId> &selection = RenderPipelineCanvas.Selection();
		if (selection.size() != 1) {
			ImGui::TextDisabled(selection.empty() ? "select a pass" : "multiple passes selected");
			return;
		}
		const nodegraph::Node *node = RenderPipelineGraph.Find(selection.front());
		if (node == nullptr) {
			return;
		}
		const nodegraph::NodeType *type = nodegraph::NodeTypes::Find(node->Type);
		ImGui::TextUnformatted(node->Label.empty() ? node->Type.c_str() : node->Label.c_str());
		if (type != nullptr) {
			ImGui::TextDisabled("%s", type->Subtitle.c_str());
			ImGui::SeparatorText("Inputs");
			for (const nodegraph::PortSpec &port : type->Inputs) {
				const nodegraph::Link *link = RenderPipelineGraph.LinkInto(node->Id, port.Name);
				ImGui::BulletText("%s  %s", port.Name.c_str(), link == nullptr ? "unwired" : "connected");
			}
			ImGui::SeparatorText("Outputs");
			for (const nodegraph::PortSpec &port : type->Outputs) {
				size_t consumers = 0;
				for (const nodegraph::Link &link : RenderPipelineGraph.Links())
					consumers += link.From == node->Id && link.FromPort == port.Name ? 1 : 0;
				ImGui::BulletText(
					"%s  %zu consumer%s", port.Name.c_str(), consumers, consumers == 1 ? "" : "s"
				);
			}
		}

		engine::graph::PipelineDocument document;
		std::string error;
		engine::graph::RenderGraph graph;
		engine::core::Name offender;
		engine::graph::CompiledGraph compiled;
		const auto canvasNode = std::find_if(
			RenderPipelineGraph.Nodes().begin(),
			RenderPipelineGraph.Nodes().end(),
			[node](const nodegraph::Node &candidate) { return candidate.Id == node->Id; }
		);
		if (canvasNode != RenderPipelineGraph.Nodes().end() &&
			SaveRenderPipelineGraph(RenderPipelineGraph, RenderPipelineBasis, document, error) &&
			engine::graph::Build(document, graph, offender) == engine::graph::PipelineDocumentStatus::Ok &&
			graph.Compile(compiled, offender) == engine::graph::GraphStatus::Ok) {
			const auto offset = static_cast<uint32_t>(canvasNode - RenderPipelineGraph.Nodes().begin());
			const engine::graph::Node *renderNode = graph.Find(engine::graph::NodeId{offset + 1});
			if (renderNode != nullptr) {
				ImGui::SeparatorText("Profile");
				const engine::graph::ProfilePass pass{
					engine::graph::NodeId{offset + 1}, renderNode->Name, renderNode->Kind
				};
				const double gpu = PassTiming(Renderer.PassTimings(), pass);
				double wall = PassTiming(Renderer.PassWallTimes(), pass);
				if (wall <= 0.0) wall = FrameGraphWallTime(pass);
				if (gpu > 0.0)
					ImGui::Text("GPU %.3f ms, wall %.3f ms", gpu / 1000.0, wall / 1000.0);
				else
					ImGui::TextDisabled(
						Renderer.Timed() ? "GPU pending, wall %.3f ms" : "GPU unavailable, wall %.3f ms",
						wall / 1000.0
					);
				const uint32_t width = WorldTarget.IsValid() ? WorldTarget.Width : 1920;
				const uint32_t height = WorldTarget.IsValid() ? WorldTarget.Height : 1080;
				const engine::graph::PipelineProfile profile =
					engine::graph::ProfilePipeline(graph, compiled, width, height);
				for (const engine::graph::ResourceId id : renderNode->Writes) {
					for (const auto &resource : profile.Resources)
						if (resource.Id == id && (resource.Kind == engine::graph::ResourceKind::Colour ||
												  resource.Kind == engine::graph::ResourceKind::Depth ||
												  resource.Kind == engine::graph::ResourceKind::Texture ||
												  resource.Kind == engine::graph::ResourceKind::Storage))
							DrawProfileImage(resource.Name, resource.Width, resource.Height, 240.0f);
				}
			}
		}
		ImGui::Spacing();
		ImGui::TextWrapped(
			"Queue, async, culling, and compute dispatch controls live on the node body. Wires determine "
			"data order; box position does not."
		);
	}
}
