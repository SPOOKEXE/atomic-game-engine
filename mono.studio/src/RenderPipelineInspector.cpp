#include "GraphParams.hpp"

#include <engine/core/FrameGraph.hpp>
#include <engine/graph/PipelineCatalogue.hpp>
#include <engine/graph/PipelineProfile.hpp>
#include <engine/ui/Metrics.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
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

		inline double FrameGraphWallTime(const engine::graph::ProfilePass &pass) {
			double microseconds = 0.0;
			for (const engine::core::FrameSpan &span : engine::core::FrameGraph::Spans())
				if (span.Name == pass.Name.Text() || span.Name == pass.Kind.Text())
					microseconds += static_cast<double>(span.Milliseconds) * 1000.0;
			return microseconds;
		}

		// Text input over a fixed buffer; true when the text changed this frame.
		bool EditText(const char *label, std::string &text, bool multiline) {
			std::array<char, 8192> buffer{};
			std::snprintf(buffer.data(), buffer.size(), "%s", text.c_str());
			const bool changed = multiline ? ImGui::InputTextMultiline(
												 label,
												 buffer.data(),
												 buffer.size(),
												 ImVec2(-1.0f, ImGui::GetTextLineHeight() * 8.0f)
											 )
										   : ImGui::InputText(label, buffer.data(), buffer.size());
			if (changed) text = buffer.data();
			return changed;
		}
	}

	void Editor::DrawRenderPipelineInspector() {
		const std::vector<std::string> &selection = RenderPipelineCanvas->Selection();
		if (selection.size() != 1) {
			ImGui::TextDisabled(selection.empty() ? "select a pass" : "multiple passes selected");
			return;
		}
		const std::string id = selection.front();
		const engine::ang::NodeInstance *node = RenderPipelineGraph.FindNode(id);
		if (node == nullptr) {
			return;
		}
		const auto type = RenderPipelineTypes().FindNode(node->Def);
		const engine::graph::NodeKindSpec *spec = engine::graph::NodeCatalogue::Find(
			engine::core::Name(std::string_view(node->Def).substr(std::string_view("render.pass.").size()))
		);

		// One drag or typing run in a field is one undo step.
		const auto commit = [&](const std::string &field, engine::ang::Value value) {
			const std::string editing = id + "\n" + field;
			const bool amend = RenderPipelineEditing == editing;
			if (ApplyRenderPipelineEdit(engine::ang::edit::SetParam{id, field, std::move(value)}, amend)) {
				RenderPipelineEditing = editing;
			}
		};
		const auto finished = [&] {
			if (ImGui::IsItemDeactivated()) RenderPipelineEditing.clear();
		};

		std::string label = RenderPipelineLabel(*node);
		ImGui::SetNextItemWidth(-1.0f);
		if (EditText("##pass-name", label, false)) commit(RENDER_PIPELINE_LABEL, engine::ang::Value(label));
		finished();
		if (spec != nullptr) ImGui::TextDisabled("%s", spec->Summary.c_str());

		if (const engine::ang::Group *group =
				node->Group.empty() ? nullptr : RenderPipelineGraph.FindGroup(node->Group)) {
			std::string title = group->Label;
			ImGui::SetNextItemWidth(engine::ui::Scaled(160.0f));
			if (EditText("Group", title, false)) {
				const std::string editing = id + "\ngroup";
				if (ApplyRenderPipelineEdit(
						engine::ang::edit::SetGroupLabel{group->Id, title}, RenderPipelineEditing == editing
					))
					RenderPipelineEditing = editing;
			}
			finished();
			ImGui::SameLine();
			if (ImGui::SmallButton("Ungroup")) {
				ApplyRenderPipelineEdit(engine::ang::edit::RemoveGroup{group->Id});
				node = RenderPipelineGraph.FindNode(id);
			}
		}

		if (type != nullptr) {
			ImGui::SeparatorText("Settings");
			for (const engine::ang::ParamDef &param : type->Params) {
				engine::ang::Value value = RenderPipelineParam(*node, param.Name);
				ImGui::PushID(param.Name.c_str());
				ImGui::SetNextItemWidth(
					std::max(80.0f, ImGui::GetContentRegionAvail().x - engine::ui::Scaled(110.0f))
				);
				if (DrawGraphParam(param, value, param.Name == "source"))
					commit(param.Name, std::move(value));
				finished();
				ImGui::PopID();
			}

			ImGui::SeparatorText("Inputs");
			for (const engine::ang::PortDef &port : type->Inputs) {
				const bool linked = std::any_of(
					RenderPipelineGraph.Links().begin(),
					RenderPipelineGraph.Links().end(),
					[&](const engine::ang::Link &link) {
						return link.ToNode == id && link.ToPort == port.Name;
					}
				);
				ImGui::BulletText("%s  %s", port.Name.c_str(), linked ? "connected" : "unwired");
			}
			ImGui::SeparatorText("Outputs");
			for (const engine::ang::PortDef &port : type->Outputs) {
				size_t consumers = 0;
				for (const engine::ang::Link &link : RenderPipelineGraph.Links())
					consumers += link.FromNode == id && link.FromPort == port.Name ? 1 : 0;
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
			[&](const engine::ang::NodeInstance &candidate) { return candidate.Id == id; }
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
		ImGui::TextDisabled("Wires determine data order; box position does not.");
	}
}
