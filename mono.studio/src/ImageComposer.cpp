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
#include <type_traits>

namespace studio {
	namespace {
		using namespace engine::imagegraph;
		constexpr size_t HISTORY = 64;
		std::string ImageComposerDiagnosticMessage(const Diagnostic &diagnostic) {
			return diagnostic.Node.empty() ? diagnostic.Message : diagnostic.Node + ": " + diagnostic.Message;
		}
		ImageComposerSnapshot Snapshot(const ImageComposerState &state) {
			return {
				state.Graph,
				state.Outputs,
				state.SelectedOutput,
				state.SourceRoot,
				state.Parameters,
				state.Bindings
			};
		}
		bool Same(const ImageComposerSnapshot &left, const ImageComposerSnapshot &right) {
			if (nodegraph::Save(left.Graph) != nodegraph::Save(right.Graph) ||
				left.SelectedOutput != right.SelectedOutput || left.Outputs.size() != right.Outputs.size() ||
				left.SourceRoot != right.SourceRoot || left.Parameters.size() != right.Parameters.size() ||
				left.Bindings.size() != right.Bindings.size())
				return false;
			for (size_t i = 0; i < left.Outputs.size(); ++i)
				if (left.Outputs[i].Name != right.Outputs[i].Name ||
					left.Outputs[i].Node != right.Outputs[i].Node ||
					left.Outputs[i].Space != right.Outputs[i].Space)
					return false;
			for (size_t i = 0; i < left.Parameters.size(); ++i)
				if (left.Parameters[i].Name != right.Parameters[i].Name ||
					left.Parameters[i].Default != right.Parameters[i].Default)
					return false;
			for (size_t i = 0; i < left.Bindings.size(); ++i)
				if (left.Bindings[i].Node != right.Bindings[i].Node ||
					left.Bindings[i].Property != right.Bindings[i].Property ||
					left.Bindings[i].Input != right.Bindings[i].Input)
					return false;
			return true;
		}
		void Restore(ImageComposerState &state, const ImageComposerSnapshot &saved) {
			// Native graph copies retain document identities and incomplete editing state.
			state.Graph = saved.Graph;
			state.Outputs = saved.Outputs;
			state.Parameters = saved.Parameters;
			state.Bindings = saved.Bindings;
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
		bool
		ResolveFile(const ImageComposerState &state, const Source &source, Image &out, std::string &failure) {
			return assetc::ReadImageGraphSourceFileTyped(
				state.SourceRoot,
				state.SourceRoot / ".imagegraph-source-context",
				source,
				engine::assets::ContentPolicy::Process(engine::assets::ContentVerb::Handle),
				out,
				failure
			);
		}
		std::string SourceKey(const Source &source) {
			return std::to_string(static_cast<unsigned>(source.Interpretation)) + ":" + source.Path;
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
				bits(output.Space);
			}
			for (const auto &parameter : state.Parameters) {
				append(parameter.Name);
				bits(parameter.Default.index());
				std::visit(
					[&](const auto &value) {
						using T = std::decay_t<decltype(value)>;
						if constexpr (std::is_same_v<T, std::string>)
							append(value);
						else
							bits(value);
					},
					parameter.Default
				);
			}
			for (const auto &binding : state.Bindings) {
				append(binding.Node);
				append(binding.Property);
				append(binding.Input);
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
				const auto identity = node->Widgets.find("__composer.id");
				const auto binding =
					std::find_if(state.Bindings.begin(), state.Bindings.end(), [&](const Binding &item) {
						return identity != node->Widgets.end() && item.Node == identity->second.Text &&
							   item.Property == spec.Key;
					});
				if (binding != state.Bindings.end()) {
					ImGui::Text("%s: %s", spec.Label.c_str(), binding->Input.c_str());
					continue;
				}
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
			ImGui::InputText("Input name", state.InputName.data(), state.InputName.size());
			if (ImGui::BeginCombo("Property", state.BindingProperty.c_str())) {
				for (const auto &spec : type->Widgets) {
					if (spec.Key == "interpretation") continue;
					if (ImGui::Selectable(spec.Label.c_str(), state.BindingProperty == spec.Key))
						state.BindingProperty = spec.Key;
				}
				ImGui::EndCombo();
			}
			if (ImGui::Button("Bind named input")) {
				Diagnostic diagnostic;
				if (!BindImageComposerInput(
						state, id, state.BindingProperty, state.InputName.data(), diagnostic
					))
					state.Error = ImageComposerDiagnosticMessage(diagnostic);
			}
		}
	}

	bool DrawImageComposerInput(const char *label, InputValue &value) {
		if (auto *number = std::get_if<double>(&value))
			return ImGui::InputDouble(label, number, 1, 10, "%.3f");
		if (auto *flag = std::get_if<bool>(&value)) return ImGui::Checkbox(label, flag);
		if (auto *colour = std::get_if<std::array<uint8_t, 4>>(&value)) {
			float rgba[4];
			for (size_t channel = 0; channel < 4; ++channel)
				rgba[channel] = (*colour)[channel] / 255.f;
			if (!ImGui::ColorEdit4(label, rgba, ImGuiColorEditFlags_NoInputs)) return false;
			for (size_t channel = 0; channel < 4; ++channel)
				(*colour)[channel] =
					static_cast<uint8_t>(std::lround(std::clamp(rgba[channel], 0.f, 1.f) * 255.f));
			return true;
		}
		std::array<char, 4097> text{};
		auto &string = std::get<std::string>(value);
		std::snprintf(text.data(), text.size(), "%s", string.c_str());
		if (!ImGui::InputText(label, text.data(), text.size())) return false;
		string = text.data();
		return true;
	}

	void InitialiseImageComposer(ImageComposerState &state) {
		if (state.Initialized) return;
		RegisterImageComposerNodeTypes();
		Document document;
		document.Nodes.push_back({"solid", Solid{256, 256, {255, 255, 255, 255}}, {}, {40, 40}});
		document.Outputs.push_back({"image", "solid"});
		Diagnostic diagnostic;
		if (!LoadImageComposerGraph(document, state.Graph, diagnostic)) {
			state.Error = ImageComposerDiagnosticMessage(diagnostic);
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
		state.SourceVersions.clear();
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
		if (!SaveImageComposerDocument(state, document, diagnostic)) {
			return refuse(ImageComposerDiagnosticMessage(diagnostic));
		}
		Document resolved;
		if (!ResolveInputs(document, {}, resolved, diagnostic))
			return refuse(ImageComposerDiagnosticMessage(diagnostic));
		document = std::move(resolved);
		// Evict sources no longer referenced. Changing one source must not retain every prior file.
		for (auto held = state.Sources.begin(); held != state.Sources.end();) {
			const bool wanted =
				std::any_of(document.Nodes.begin(), document.Nodes.end(), [&](const auto &node) {
					const auto *source = std::get_if<Source>(&node.Value);
					return source != nullptr && SourceKey(*source) == held->first;
				});
			if (wanted) {
				++held;
				continue;
			}
			state.SourceBytes -= held->second.Pixels.size();
			state.SourceVersions.erase(held->first);
			held = state.Sources.erase(held);
		}
		const TypedSourceResolver sources = [&](const Source &source, Image &out, std::string &failure) {
			const std::string name = SourceKey(source);
			if (const auto found = state.Sources.find(name); found != state.Sources.end()) {
				out = found->second;
				return true;
			}
			Image decoded;
			if (!(host.TypedSources ? host.TypedSources(source, decoded, failure)
				  : host.Sources	? host.Sources(source.Path, decoded, failure)
									: ResolveFile(state, source, decoded, failure)))
				return false;
			if (!decoded.IsValid() ||
				decoded.Pixels.size() > Limits::MaximumRetainedBytes - state.SourceBytes) {
				failure = "decoded source cache budget exceeded";
				return false;
			}
			const auto [held, inserted] = state.Sources.emplace(name, std::move(decoded));
			if (inserted) {
				state.SourceBytes += held->second.Pixels.size();
				state.SourceVersions[name] = ++state.SourceVersion;
			}
			out = held->second;
			return true;
		};
		if (host.Gpu) {
			// ImGui draws into an UNORM target. Preserve stored channels rather than applying sRGB sampling.
			for (auto &output : document.Outputs)
				if (output.Name == state.SelectedOutput) output.Space = OutputSpace::Linear;
			ImageComposerPreview preview;
			if (!host.Gpu(document, state.SelectedOutput, sources, preview, diagnostic))
				return refuse(ImageComposerDiagnosticMessage(diagnostic));
			if (preview.Width == 0 || preview.Height == 0) return refuse("GPU preview has no extent");
			state.PreviewHandle = preview.Handle;
			state.PreviewWidth = preview.Width;
			state.PreviewHeight = preview.Height;
			state.GpuPreview = true;
			++state.PreviewRevision;
			state.Error.clear();
			return true;
		}
		Plan plan;
		Image candidate;
		if (!Compile(document, plan, diagnostic) ||
			!EvaluateTyped(document, plan, state.SelectedOutput, sources, candidate, diagnostic)) {
			return refuse(ImageComposerDiagnosticMessage(diagnostic));
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
		state.PreviewWidth = state.Preview.Width;
		state.PreviewHeight = state.Preview.Height;
		state.GpuPreview = false;
		++state.PreviewRevision;
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
			state.Error = ImageComposerDiagnosticMessage(diagnostic);
			return false;
		}
		state.Graph = std::move(graph);
		state.Outputs = std::move(document.Outputs);
		state.Parameters = std::move(document.Parameters);
		state.Bindings = std::move(document.Bindings);
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
		if (!SaveImageComposerDocument(state, document, diagnostic)) {
			state.Error = ImageComposerDiagnosticMessage(diagnostic);
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
		auto rebase = [&](std::string &reference) {
			const auto original = std::filesystem::weakly_canonical(state.SourceRoot / reference, error);
			if (error) {
				state.Error = "invalid source path";
				return false;
			}
			const auto relative = original.lexically_relative(root);
			if (relative.empty() || *relative.begin() == "..") {
				state.Error = "save the project in a directory containing its source images";
				return false;
			}
			changedReferences = changedReferences || reference != relative.generic_string();
			reference = relative.generic_string();
			return true;
		};
		for (auto &node : document.Nodes)
			if (auto *source = std::get_if<Source>(&node.Value))
				if (!rebase(source->Path)) return false;
		for (auto &parameter : document.Parameters) {
			const bool sourcePath =
				std::any_of(document.Bindings.begin(), document.Bindings.end(), [&](const Binding &binding) {
					if (binding.Input != parameter.Name || binding.Property != "path") return false;
					return std::any_of(document.Nodes.begin(), document.Nodes.end(), [&](const Node &node) {
						return node.Id == binding.Node && std::holds_alternative<Source>(node.Value);
					});
				});
			if (sourcePath && !rebase(std::get<std::string>(parameter.Default))) return false;
		}
		if (!Write(document, encoded, diagnostic)) {
			state.Error = ImageComposerDiagnosticMessage(diagnostic);
			return false;
		}
		nodegraph::Graph accepted;
		if (!LoadImageComposerGraph(document, accepted, diagnostic)) {
			state.Error = ImageComposerDiagnosticMessage(diagnostic);
			return false;
		}
		if (!PublishFile(state, path, std::as_bytes(std::span(encoded)))) return false;
		const bool changedRoot = state.SourceRoot != root;
		state.SourceRoot = root;
		if (changedReferences) {
			state.Graph = std::move(accepted);
			state.Parameters = document.Parameters;
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
		if (state.GpuPreview) {
			Document document, resolved;
			Diagnostic diagnostic;
			if (!SaveImageComposerDocument(state, document, diagnostic) ||
				!ResolveInputs(document, {}, resolved, diagnostic)) {
				state.Error = ImageComposerDiagnosticMessage(diagnostic);
				return false;
			}
			const TypedSourceResolver sources = [&](const Source &source, Image &out, std::string &failure) {
				const auto held = state.Sources.find(SourceKey(source));
				if (held != state.Sources.end()) {
					out = held->second;
					return true;
				}
				return host.TypedSources ? host.TypedSources(source, out, failure)
					   : host.Sources	 ? host.Sources(source.Path, out, failure)
										 : ResolveFile(state, source, out, failure);
			};
			engine::assets::TextureData texture;
			if (!engine::bake::BakeImageGraphTyped(
					resolved, state.SelectedOutput, sources, texture, diagnostic
				)) {
				state.Error = ImageComposerDiagnosticMessage(diagnostic);
				return false;
			}
			state.PreviewTexture = std::move(texture);
		}
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
	bool CanApplyLiveImageComposer(const ImageComposerState &state) {
		return !state.PublishedGraph.empty() && !state.TargetProperty.empty() &&
			   state.PublishedKey == RevisionKey(state) + std::to_string(state.SourceRevision);
	}
	bool PublishLiveImageComposer(
		ImageComposerState &state, const ImageComposerHost &host, std::string_view name
	) {
		if (!host.LivePublish) {
			state.Error = "live publication is unavailable";
			return false;
		}
		Document document;
		Diagnostic diagnostic;
		if (!SaveImageComposerDocument(state, document, diagnostic)) {
			state.Error = ImageComposerDiagnosticMessage(diagnostic);
			return false;
		}
		std::string accepted, failure;
		if (!host.LivePublish(document, name, accepted, failure)) {
			state.Error = failure;
			return false;
		}
		state.PublishedGraph = std::move(accepted);
		state.PublishedKey = RevisionKey(state) + std::to_string(state.SourceRevision);
		state.Error.clear();
		state.Notice = "Published " + state.PublishedGraph;
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
			if (!state.Parameters.empty()) ImGui::SeparatorText("Named inputs");
			for (size_t input = 0; input < state.Parameters.size();) {
				auto &parameter = state.Parameters[input];
				ImGui::PushID(static_cast<int>(input));
				ImGui::SetNextItemWidth(std::max(80.f, ImGui::GetContentRegionAvail().x - 100.f));
				const bool changed = DrawImageComposerInput(parameter.Name.c_str(), parameter.Default);
				if ((changed && (parameter.Default.index() == 1 || parameter.Default.index() == 2)) ||
					ImGui::IsItemDeactivatedAfterEdit())
					CommitImageComposer(state);
				if (ImGui::SmallButton("Remove input")) {
					const auto name = parameter.Name;
					std::erase_if(state.Bindings, [&](const auto &binding) { return binding.Input == name; });
					state.Parameters.erase(state.Parameters.begin() + static_cast<std::ptrdiff_t>(input));
					CommitImageComposer(state);
				} else
					++input;
				ImGui::PopID();
			}
			ImGui::Separator();
			if (ImGui::BeginCombo("Preview output", state.SelectedOutput.c_str())) {
				for (const auto &output : state.Outputs)
					if (ImGui::Selectable(output.Name.c_str(), state.SelectedOutput == output.Name)) {
						state.SelectedOutput = output.Name;
						CommitImageComposer(state);
					}
				ImGui::EndCombo();
			}
			const auto output =
				std::find_if(state.Outputs.begin(), state.Outputs.end(), [&](const auto &item) {
					return item.Name == state.SelectedOutput;
				});
			if (output != state.Outputs.end()) {
				const bool linear = output->Space == OutputSpace::Linear;
				if (ImGui::BeginCombo("Output space", linear ? "Linear data" : "sRGB colour")) {
					if (ImGui::Selectable("sRGB colour", !linear)) {
						output->Space = OutputSpace::SRGB;
						CommitImageComposer(state);
					}
					if (ImGui::Selectable("Linear data", linear)) {
						output->Space = OutputSpace::Linear;
						CommitImageComposer(state);
					}
					ImGui::EndCombo();
				}
			}
			(void)RefreshImageComposer(state, host);
			if (state.PreviewWidth != 0 && state.PreviewHeight != 0) {
				ImGui::Text("%u x %u", state.PreviewWidth, state.PreviewHeight);
				if (state.PreviewHandle != nullptr) {
					const float width = std::min(ImGui::GetContentRegionAvail().x, 256.f);
					ImGui::Image(
						ImTextureRef(state.PreviewHandle),
						ImVec2(width, width * state.PreviewHeight / state.PreviewWidth)
					);
				}
			}
			ImGui::InputText("Asset", state.AssetName.data(), state.AssetName.size());
			if (ImGui::Button("Export .atex")) ExportImageComposer(state, host, state.AssetName.data());
			if (host.LivePublish) {
				ImGui::InputText("Live asset", state.LiveAssetName.data(), state.LiveAssetName.size());
				if (ImGui::Button("Publish live graph"))
					PublishLiveImageComposer(state, host, state.LiveAssetName.data());
				if (host.Apply) {
					if (ImGui::BeginCombo("Image slot", state.TargetProperty.c_str())) {
						for (const auto &slot : host.ImageSlots)
							if (ImGui::Selectable(slot.c_str(), state.TargetProperty == slot))
								state.TargetProperty = slot;
						ImGui::EndCombo();
					}
					ImGui::BeginDisabled(
						!CanApplyLiveImageComposer(state) ||
						std::find(host.ImageSlots.begin(), host.ImageSlots.end(), state.TargetProperty) ==
							host.ImageSlots.end()
					);
					if (ImGui::Button("Apply to selection"))
						host.Apply(state.PublishedGraph, state.SelectedOutput, state.TargetProperty);
					ImGui::EndDisabled();
				}
			}
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
