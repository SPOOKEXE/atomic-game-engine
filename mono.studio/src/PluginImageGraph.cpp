#include "PluginImageGraph.hpp"

#include "ImageComposerGraph.hpp"

#include <engine/imagegraph/Document.hpp>

#include <array>
#include <imgui.h>

namespace studio {
	namespace {
		std::string PluginGraphMessage(const engine::imagegraph::Diagnostic &diagnostic) {
			return diagnostic.Node.empty() ? diagnostic.Message : diagnostic.Node + ": " + diagnostic.Message;
		}
	}

	bool OpenPluginImageGraph(PluginImageGraphEditor &editor, std::string_view json, std::string &failure) {
		engine::imagegraph::Document document;
		engine::imagegraph::Diagnostic diagnostic;
		std::string canonical;
		if (!engine::imagegraph::Read(json, document, diagnostic) ||
			!engine::imagegraph::Write(document, canonical, diagnostic) ||
			!LoadImageComposerDocument(editor.State, document, diagnostic)) {
			failure = PluginGraphMessage(diagnostic);
			return false;
		}
		editor.Document = std::move(canonical);
		editor.Error.clear();
		return true;
	}

	bool SavePluginImageGraph(PluginImageGraphEditor &editor, std::string &json) {
		engine::imagegraph::Document document;
		engine::imagegraph::Diagnostic diagnostic;
		if (!SaveImageComposerDocument(editor.State, document, diagnostic) ||
			!engine::imagegraph::Write(document, json, diagnostic)) {
			editor.Error = PluginGraphMessage(diagnostic);
			return false;
		}
		editor.Error.clear();
		return true;
	}

	bool DrawPluginImageGraph(PluginImageGraphEditor &editor, std::string &changed) {
		auto &state = editor.State;
		const float sidebar = std::min(280.f, ImGui::GetContentRegionAvail().x * .4f);
		const float canvasHeight =
			std::max(1.f, ImGui::GetContentRegionAvail().y - ImGui::GetTextLineHeightWithSpacing());
		if (ImGui::BeginChild(
				"##plugin-graph-canvas",
				ImVec2(std::max(1.f, ImGui::GetContentRegionAvail().x - sidebar - 8), canvasHeight)
			))
			DrawImageComposerCanvas(state);
		ImGui::EndChild();
		ImGui::SameLine();
		if (ImGui::BeginChild("##plugin-graph-controls", ImVec2(0, canvasHeight))) {
			DrawImageComposerPalette(state);
			ImGui::Separator();
			bool wide = state.Storage == engine::imagegraph::Precision::Wide;
			if (ImGui::Checkbox("16-bit intermediates", &wide)) {
				state.Storage =
					wide ? engine::imagegraph::Precision::Wide : engine::imagegraph::Precision::Byte;
				CommitImageComposer(state);
			}
			for (auto &output : state.Outputs) {
				ImGui::PushID(output.Name.c_str());
				constexpr std::array<const char *, 3> FORMATS{"RGBA8", "R16F", "RGBA16F"};
				int format = static_cast<int>(output.Format);
				const std::string label = output.Name + " format";
				if (ImGui::Combo(label.c_str(), &format, FORMATS.data(), static_cast<int>(FORMATS.size()))) {
					output.Format = static_cast<engine::imagegraph::OutputFormat>(format);
					CommitImageComposer(state);
				}
				ImGui::PopID();
			}
		}
		ImGui::EndChild();
		std::string json;
		const bool valid = SavePluginImageGraph(editor, json);
		ImGui::TextUnformatted(valid ? "" : editor.Error.c_str());
		if (!valid || json == editor.Document) return false;
		editor.Document = json;
		changed = std::move(json);
		return true;
	}
}
