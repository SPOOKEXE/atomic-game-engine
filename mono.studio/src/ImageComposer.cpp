#include "ImageComposerGraph.hpp"

#include <engine/assets/ContentPolicy.hpp>
#include <engine/bake/ImageGraph.hpp>
#include <engine/core/Bytes.hpp>
#include <engine/core/Profiling.hpp>

#include <algorithm>
#include <assetc/ImageGraph.hpp>
#include <cmath>
#include <cstdio>
#include <imgui.h>
#include <nodegraph/Layout.hpp>
#include <nodegraph/Registry.hpp>
#include <nodegraph/Serialize.hpp>
#include <studio/ImageComposer.hpp>
#include <system_error>

namespace studio {
	namespace {
		using namespace engine::imagegraph;
		constexpr size_t HISTORY = 64;
		std::string Message(const Diagnostic &diagnostic) {
			return diagnostic.Node.empty() ? diagnostic.Message : diagnostic.Node + ": " + diagnostic.Message;
		}
		ImageComposerSnapshot Snapshot(const ImageComposerState &state) {
			return {state.Graph, state.Outputs, state.SelectedOutput, state.SourceRoot};
		}
		bool Same(const ImageComposerSnapshot &left, const ImageComposerSnapshot &right) {
			if (nodegraph::Save(left.Graph) != nodegraph::Save(right.Graph) ||
				left.SelectedOutput != right.SelectedOutput || left.Outputs.size() != right.Outputs.size() ||
				left.SourceRoot != right.SourceRoot)
				return false;
			for (size_t i = 0; i < left.Outputs.size(); ++i)
				if (left.Outputs[i].Name != right.Outputs[i].Name ||
					left.Outputs[i].Node != right.Outputs[i].Node)
					return false;
			return true;
		}
		void Restore(ImageComposerState &state, const ImageComposerSnapshot &saved) {
			// Native graph copies retain document identities and incomplete editing state.
			state.Graph = saved.Graph;
			state.Outputs = saved.Outputs;
			state.SelectedOutput = saved.SelectedOutput;
			if (state.SourceRoot != saved.SourceRoot) ReloadImageComposerSources(state);
			state.SourceRoot = saved.SourceRoot;
			state.Canvas.Select(nodegraph::NO_NODE);
			state.Attempt.clear();
		}

		bool PublishFile(
			ImageComposerState &state, const std::filesystem::path &path, std::span<const std::byte> bytes
		) {
			return assetc::PublishImageGraphTexture(
				path.has_parent_path() ? path : std::filesystem::current_path() / path, bytes, state.Error
			);
		}
		bool ResolveFile(
			const ImageComposerState &state, std::string_view source, Image &out, std::string &failure
		) {
			return assetc::ReadImageGraphSourceFile(
				state.SourceRoot,
				state.SourceRoot / ".imagegraph-source-context",
				source,
				engine::assets::ContentPolicy::Process(engine::assets::ContentVerb::Handle),
				out,
				failure
			);
		}
		std::string RevisionKey(const ImageComposerState &state) {
			std::string key;
			key.reserve(state.Attempt.size());
			const auto bits = [&](const auto &value) {
				key.append(reinterpret_cast<const char *>(&value), sizeof(value));
			};
			const auto append = [&](std::string_view text) {
				bits(text.size());
				key.append(text);
			};
			// Hashing upstream recursively repeats shared DAGs exponentially. This key visits stored data
			// once.
			bits(state.Graph.Nodes().size());
			for (const auto &node : state.Graph.Nodes()) {
				bits(node.Id);
				append(node.Type);
				std::vector<std::string_view> controls;
				controls.reserve(node.Widgets.size());
				for (const auto &[name, value] : node.Widgets)
					controls.push_back(name);
				std::sort(controls.begin(), controls.end());
				bits(controls.size());
				for (const auto name : controls) {
					append(name);
					const auto &value = node.Widgets.at(std::string(name));
					bits(value.Kind);
					bits(value.Number);
					bits(value.Flag);
					append(value.Text);
					bits(value.Tint.R);
					bits(value.Tint.G);
					bits(value.Tint.B);
					bits(value.Tint.A);
				}
			}
			bits(state.Graph.Links().size());
			for (const auto &link : state.Graph.Links()) {
				bits(link.From);
				append(link.FromPort);
				bits(link.To);
				append(link.ToPort);
			}
			append(state.SelectedOutput);
			for (const auto &output : state.Outputs) {
				append(output.Name);
				append(output.Node);
			}
			return key;
		}

		void Inspector(ImageComposerState &state) {
			if (state.Canvas.Selection().empty()) return;
			const auto id = state.Canvas.Selection().front();
			const auto *node = state.Graph.Find(id);
			if (node == nullptr) return;
			const auto *type = nodegraph::NodeTypes::Find(node->Type);
			if (type == nullptr) return;
			ImGui::Separator();
			ImGui::TextUnformatted(type->Title.c_str());
			bool immediate = false, finished = false;
			for (const auto &spec : type->Widgets) {
				auto value = nodegraph::ValueOf(state.Graph, id, spec);
				bool changed = false;
				ImGui::PushID(spec.Key.c_str());
				ImGui::SetNextItemWidth(std::max(80.f, ImGui::GetContentRegionAvail().x - 135.f));
				switch (spec.Kind) {
				case nodegraph::WidgetKind::Number:
					changed = ImGui::InputDouble(spec.Label.c_str(), &value.Number, 1, 10, "%.3f");
					break;
				case nodegraph::WidgetKind::Slider: {
					float number = static_cast<float>(value.Number);
					changed = ImGui::SliderFloat(
						spec.Label.c_str(),
						&number,
						static_cast<float>(spec.Minimum),
						static_cast<float>(spec.Maximum)
					);
					value.Number = number;
					break;
				}
				case nodegraph::WidgetKind::Text: {
					std::array<char, 1024> text{};
					std::snprintf(text.data(), text.size(), "%s", value.Text.c_str());
					changed = ImGui::InputText(spec.Label.c_str(), text.data(), text.size());
					if (changed) value.Text = text.data();
					break;
				}
				case nodegraph::WidgetKind::Toggle:
					changed = ImGui::Checkbox(spec.Label.c_str(), &value.Flag);
					immediate = changed || immediate;
					break;
				case nodegraph::WidgetKind::Select:
					if (ImGui::BeginCombo(spec.Label.c_str(), value.Text.c_str())) {
						for (size_t choice = 0; choice < spec.Options.size(); ++choice)
							if (ImGui::Selectable(
									spec.Options[choice].c_str(), value.Text == spec.Options[choice]
								)) {
								value.Text = spec.Options[choice];
								value.Number = static_cast<double>(choice);
								changed = immediate = true;
							}
						ImGui::EndCombo();
					}
					break;
				case nodegraph::WidgetKind::Colour: {
					float rgba[4]{value.Tint.R, value.Tint.G, value.Tint.B, value.Tint.A};
					changed = ImGui::ColorEdit4(spec.Label.c_str(), rgba, ImGuiColorEditFlags_NoInputs);
					if (changed) value.Tint = {rgba[0], rgba[1], rgba[2], rgba[3]};
					break;
				}
				}
				finished = ImGui::IsItemDeactivatedAfterEdit() || finished;
				if (changed) nodegraph::SetValue(state.Graph, id, spec.Key, value);
				ImGui::PopID();
			}
			if (finished || immediate) CommitImageComposer(state);
			ImGui::InputText("Output name", state.OutputName.data(), state.OutputName.size());
			if (ImGui::Button("Bind selected node"))
				SetImageComposerOutput(state, state.OutputName.data(), id);
		}
	}

	void InitialiseImageComposer(ImageComposerState &state) {
		if (state.Initialized) return;
		RegisterImageComposerNodeTypes();
		Document document;
		document.Nodes.push_back({"solid", Solid{256, 256, {255, 255, 255, 255}}, {}, {40, 40}});
		document.Outputs.push_back({"image", "solid"});
		Diagnostic diagnostic;
		if (!LoadImageComposerGraph(document, state.Graph, diagnostic)) {
			state.Error = Message(diagnostic);
			return;
		}
		state.Outputs = document.Outputs;
		state.Canvas.Signals.Changed = [&state] { CommitImageComposer(state); };
		state.Canvas.Signals.Rerun = [&state](nodegraph::NodeId) { ReloadImageComposerSources(state); };
		state.Canvas.AcceptsType = [](const nodegraph::NodeType &type) {
			return type.Category == "Image Composer";
		};
		state.Canvas.Look.Background = state.Canvas.Look.NodeBody = 0xFF000000;
		state.Canvas.Look.Text = 0xFFFFFFFF;
		state.Canvas.Look.Muted = 0xFFBFBFBF;
		state.Canvas.Look.NodeSelected = state.Canvas.Look.WidgetFill = state.Canvas.Look.Refused =
			0xFFFFFFFF;
		state.Canvas.Look.Widget = state.Canvas.Look.NodeBorder = 0xFF333333;
		state.Canvas.Look.Marquee = 0x22FFFFFF;
		state.Initialized = true;
		state.Last = Snapshot(state);
	}
	void CommitImageComposer(ImageComposerState &state) {
		AssignImageComposerIdentities(state.Graph, state.Outputs);
		auto next = Snapshot(state);
		if (Same(next, state.Last)) return;
		if (state.Initialized) state.Past.push_back(std::move(state.Last));
		state.Last = std::move(next);
		state.Future.clear();
		if (state.Past.size() > HISTORY) state.Past.erase(state.Past.begin());
	}
	bool UndoImageComposer(ImageComposerState &state) {
		CommitImageComposer(state);
		if (state.Past.empty()) return false;
		const auto previous = state.Past.back();
		Restore(state, previous);
		state.Future.push_back(std::move(state.Last));
		state.Past.pop_back();
		state.Last = previous;
		return true;
	}
	bool RedoImageComposer(ImageComposerState &state) {
		if (state.Future.empty()) return false;
		const auto next = state.Future.back();
		Restore(state, next);
		state.Past.push_back(std::move(state.Last));
		state.Future.pop_back();
		state.Last = next;
		return true;
	}
	void ReloadImageComposerSources(ImageComposerState &state) {
		state.Sources.clear();
		state.SourceBytes = 0;
		++state.SourceRevision;
	}
	bool RefreshImageComposer(ImageComposerState &state, const ImageComposerHost &host) {
		InitialiseImageComposer(state);
		const auto revision = RevisionKey(state);
		if (revision == state.Attempt && state.AttemptRevision == state.SourceRevision)
			return state.EvaluationError.empty();
		state.Attempt = revision;
		state.AttemptRevision = state.SourceRevision;
		state.EvaluationError.clear();
		const auto refuse = [&](std::string failure) {
			state.EvaluationError = failure;
			state.Error = std::move(failure);
			return false;
		};
		ENGINE_PROFILE_CAT("image composer evaluate", engine::core::ProfileCategory::Assets);
		Document document;
		Diagnostic diagnostic;
		if (!SaveImageComposerGraph(state.Graph, state.Outputs, document, diagnostic)) {
			return refuse(Message(diagnostic));
		}
		// Evict sources no longer referenced. Changing one source must not retain every prior file.
		for (auto held = state.Sources.begin(); held != state.Sources.end();) {
			const bool wanted =
				std::any_of(document.Nodes.begin(), document.Nodes.end(), [&](const auto &node) {
					const auto *source = std::get_if<Source>(&node.Value);
					return source != nullptr && source->Path == held->first;
				});
			if (wanted) {
				++held;
				continue;
			}
			state.SourceBytes -= held->second.Pixels.size();
			held = state.Sources.erase(held);
		}
		const SourceResolver sources = [&](std::string_view path, Image &out, std::string &failure) {
			const std::string name(path);
			if (const auto found = state.Sources.find(name); found != state.Sources.end()) {
				out = found->second;
				return true;
			}
			Image decoded;
			if (!(host.Sources ? host.Sources(path, decoded, failure)
							   : ResolveFile(state, path, decoded, failure)))
				return false;
			if (!decoded.IsValid() ||
				decoded.Pixels.size() > Limits::MaximumRetainedBytes - state.SourceBytes) {
				failure = "decoded source cache budget exceeded";
				return false;
			}
			const auto [held, inserted] = state.Sources.emplace(name, std::move(decoded));
			if (inserted) state.SourceBytes += held->second.Pixels.size();
			out = held->second;
			return true;
		};
		Plan plan;
		Image candidate;
		if (!Compile(document, plan, diagnostic) ||
			!Evaluate(document, plan, state.SelectedOutput, sources, candidate, diagnostic)) {
			return refuse(Message(diagnostic));
		}
		engine::assets::TextureData texture;
		std::string failure;
		if (!engine::bake::ImageGraphTexture(candidate, texture, failure)) {
			return refuse(failure);
		}
		void *handle = state.PreviewHandle;
		if (host.Upload && !host.Upload(texture, handle, failure)) {
			return refuse(failure);
		}
		state.Preview = std::move(candidate);
		state.PreviewTexture = std::move(texture);
		state.PreviewHandle = handle;
		state.Error.clear();
		return true;
	}
	bool OpenImageComposerProject(ImageComposerState &state, const std::filesystem::path &path) {
		InitialiseImageComposer(state);
		Document document;
		nodegraph::Graph graph;
		Diagnostic diagnostic;
		const auto parent = path.has_parent_path() ? path.parent_path() : std::filesystem::current_path();
		if (!assetc::ReadImageGraphProject(parent, path, document, state.Error)) return false;
		if (!LoadImageComposerGraph(document, graph, diagnostic)) {
			state.Error = Message(diagnostic);
			return false;
		}
		state.Graph = std::move(graph);
		state.Outputs = std::move(document.Outputs);
		state.SelectedOutput = state.Outputs.front().Name;
		state.SourceRoot = path.has_parent_path() ? path.parent_path() : std::filesystem::current_path();
		std::snprintf(state.ProjectPath.data(), state.ProjectPath.size(), "%s", path.string().c_str());
		state.Canvas.Select(nodegraph::NO_NODE);
		state.Canvas.Fit(state.Graph);
		state.Past.clear();
		state.Future.clear();
		state.Last = Snapshot(state);
		ReloadImageComposerSources(state);
		state.Attempt.clear();
		state.Error.clear();
		state.Notice = "Opened " + path.filename().string();
		return true;
	}
	bool SaveImageComposerProject(ImageComposerState &state, const std::filesystem::path &path) {
		Document document;
		Diagnostic diagnostic;
		std::string encoded;
		if (!SaveImageComposerGraph(state.Graph, state.Outputs, document, diagnostic)) {
			state.Error = Message(diagnostic);
			return false;
		}
		// Source paths remain project-relative after Save As. Refuse moves that would require escape paths.
		std::error_code error;
		const auto root =
			std::filesystem::weakly_canonical(path.has_parent_path() ? path.parent_path() : ".", error);
		if (error) {
			state.Error = "invalid project directory";
			return false;
		}
		bool changedReferences = false;
		for (auto &node : document.Nodes)
			if (auto *source = std::get_if<Source>(&node.Value)) {
				const auto original =
					std::filesystem::weakly_canonical(state.SourceRoot / source->Path, error);
				if (error) {
					state.Error = "invalid source path";
					return false;
				}
				const auto relative = original.lexically_relative(root);
				if (relative.empty() || *relative.begin() == "..") {
					state.Error = "save the project in a directory containing its source images";
					return false;
				}
				changedReferences = changedReferences || source->Path != relative.generic_string();
				source->Path = relative.generic_string();
			}
		if (!Write(document, encoded, diagnostic)) {
			state.Error = Message(diagnostic);
			return false;
		}
		nodegraph::Graph accepted;
		if (!LoadImageComposerGraph(document, accepted, diagnostic)) {
			state.Error = Message(diagnostic);
			return false;
		}
		if (!PublishFile(state, path, std::as_bytes(std::span(encoded)))) return false;
		const bool changedRoot = state.SourceRoot != root;
		state.SourceRoot = root;
		if (changedReferences) {
			state.Graph = std::move(accepted);
			state.Canvas.Select(nodegraph::NO_NODE);
			ReloadImageComposerSources(state);
		}
		if (changedRoot || changedReferences) CommitImageComposer(state);
		std::snprintf(state.ProjectPath.data(), state.ProjectPath.size(), "%s", path.string().c_str());
		state.Error.clear();
		state.Notice = "Saved " + path.filename().string();
		return true;
	}
	bool
	ExportImageComposer(ImageComposerState &state, const ImageComposerHost &host, std::string_view name) {
		std::string asset(name);
		if (asset.empty() || asset == "." || asset == ".." ||
			asset.find_first_of("/\\:") != std::string::npos || asset.find('\0') != std::string::npos ||
			asset.size() > 128) {
			state.Error = "asset name must be a file name";
			return false;
		}
		if (!asset.ends_with(".atex")) asset += ".atex";
		if (!RefreshImageComposer(state, host)) return false;
		engine::core::ByteWriter encoded;
		if (!engine::assets::Texture::Write(encoded, state.PreviewTexture)) {
			state.Error = "output is not an ordinary texture";
			return false;
		}
		if (host.BakedRoot.empty()) {
			state.Error = "baked content directory is unavailable";
			return false;
		}
		if (!PublishFile(state, host.BakedRoot / asset, encoded.Bytes())) return false;
		if (host.Register) host.Register(encoded.Bytes(), asset);
		state.Error.clear();
		state.Notice = "Exported " + asset;
		return true;
	}
	void DrawImageComposer(ImageComposerState &state, bool &open, const ImageComposerHost &host) {
		if (!open) return;
		InitialiseImageComposer(state);
		ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 1));
		ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 1));
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
		if (!ImGui::Begin("Image Composer", &open)) {
			ImGui::End();
			ImGui::PopStyleColor(3);
			return;
		}
		ImGui::SetNextItemWidth(std::max(140.f, ImGui::GetContentRegionAvail().x - 295));
		ImGui::InputText("##project", state.ProjectPath.data(), state.ProjectPath.size());
		ImGui::SameLine();
		if (ImGui::Button("Open")) OpenImageComposerProject(state, state.ProjectPath.data());
		ImGui::SameLine();
		if (ImGui::Button("Save")) SaveImageComposerProject(state, state.ProjectPath.data());
		ImGui::SameLine();
		if (ImGui::Button("Undo")) UndoImageComposer(state);
		ImGui::SameLine();
		if (ImGui::Button("Redo")) RedoImageComposer(state);
		ImGui::SameLine();
		if (ImGui::Button("Reload")) ReloadImageComposerSources(state);
		ImGui::Separator();
		const float sidebar = std::min(300.f, ImGui::GetContentRegionAvail().x * .42f);
		if (ImGui::BeginChild(
				"##image-canvas", ImVec2(std::max(1.f, ImGui::GetContentRegionAvail().x - sidebar - 8), 0)
			))
			state.Canvas.Draw(state.Graph);
		ImGui::EndChild();
		ImGui::SameLine();
		if (ImGui::BeginChild("##image-controls", ImVec2(0, 0))) {
			if (ImGui::BeginTable("##palette", 2)) {
				for (const auto &type : nodegraph::NodeTypes::All()) {
					if (type.Category != "Image Composer") continue;
					ImGui::TableNextColumn();
					if (ImGui::Button(type.Title.c_str(), ImVec2(-1, 0))) {
						const auto id = state.Graph.Add(
							type.Id, 40, 40 + static_cast<float>(state.Graph.Nodes().size() % 5) * 40
						);
						state.Canvas.Select(id);
						CommitImageComposer(state);
					}
				}
				ImGui::EndTable();
			}
			Inspector(state);
			ImGui::Separator();
			if (ImGui::BeginCombo("Preview output", state.SelectedOutput.c_str())) {
				for (const auto &output : state.Outputs)
					if (ImGui::Selectable(output.Name.c_str(), state.SelectedOutput == output.Name)) {
						state.SelectedOutput = output.Name;
						CommitImageComposer(state);
					}
				ImGui::EndCombo();
			}
			(void)RefreshImageComposer(state, host);
			if (state.Preview.IsValid()) {
				ImGui::Text("%u x %u", state.Preview.Width, state.Preview.Height);
				if (state.PreviewHandle != nullptr) {
					const float width = std::min(ImGui::GetContentRegionAvail().x, 256.f);
					ImGui::Image(
						ImTextureRef(state.PreviewHandle),
						ImVec2(width, width * state.Preview.Height / state.Preview.Width)
					);
				}
			}
			ImGui::InputText("Asset", state.AssetName.data(), state.AssetName.size());
			if (ImGui::Button("Export .atex")) ExportImageComposer(state, host, state.AssetName.data());
			if (host.Publish) {
				ImGui::SameLine();
				if (ImGui::Button("Publish assets")) host.Publish();
			}
			if (!state.Error.empty())
				ImGui::TextWrapped("%s", state.Error.c_str());
			else if (!state.Notice.empty())
				ImGui::TextWrapped("%s", state.Notice.c_str());
		}
		ImGui::EndChild();
		ImGui::End();
		ImGui::PopStyleColor(3);
	}
}
