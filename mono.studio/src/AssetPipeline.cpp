// The Asset Pipeline panel: a node canvas over the selected world's
// `bake::PipelineSet`, drawn beside the Render Pipeline editor.
//
// The world resource is the saved form and is read every frame for the list of
// names. The canvas holds the open pipeline's unsaved edits; Save, Delete and
// Bake are queued and applied by `ApplyAssetPipelinePending`, outside every
// `Universe::Enter`.

#include "AssetPipeline.hpp"

#include "AssetPipelineGraph.hpp"
#include "GraphParams.hpp"

#include <engine/assets/LocalStore.hpp>
#include <engine/bake/GraphDocument.hpp>
#include <engine/core/Log.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/ui/NodeGraphPainter.hpp>

#include <array>
#include <cstring>
#include <fstream>
#include <imgui.h>
#include <iterator>
#include <studio/Editor.hpp>
#include <vector>

namespace studio {
	namespace {
		// The selected node's settings, each change one undo step (a typing run
		// in one field folds into one).
		void Inspector(AssetPipelineState &state) {
			const auto &selection = state.Canvas->Selection();
			if (selection.size() != 1) return;
			const std::string id = selection.front();
			const engine::ang::NodeInstance *node = state.Graph.FindNode(id);
			const auto def = node == nullptr ? nullptr : AssetPipelineTypes().FindNode(node->Def);
			if (def == nullptr || def->Params.empty()) return;
			ImGui::TextUnformatted(def->Label.c_str());
			for (const engine::ang::ParamDef &param : def->Params) {
				ImGui::SameLine();
				ImGui::PushID(param.Name.c_str());
				ImGui::SetNextItemWidth(120.0f);
				engine::ang::Value value = AssetPipelineParam(*node, param.Name);
				if (DrawGraphParam(param, value)) {
					engine::ang::EditResult result =
						state.Graph.Apply(engine::ang::edit::SetParam{id, param.Name, std::move(value)});
					const std::string editing = id + "\n" + param.Name;
					if (result.Ok) {
						if (state.Editing == editing)
							state.History.Amend(std::move(result.Record));
						else
							state.History.Record(std::move(result.Record));
						state.Editing = editing;
						state.Dirty = true;
						state.Canvas->GraphChanged();
					}
					node = state.Graph.FindNode(id);
				}
				if (ImGui::IsItemDeactivated()) state.Editing.clear();
				ImGui::PopID();
			}
		}

		// A starter chain so a new pipeline is something to edit, not a blank
		// canvas: raw file, import, write.
		engine::bake::Document Starter(const std::string &name) {
			using engine::bake::Operation;
			using engine::bake::OperationKind;
			engine::bake::Document document;
			document.Record({.Kind = OperationKind::AddSource, .Text = name + ".png"});
			document.Record({.Kind = OperationKind::AddNode, .Node = engine::bake::NodeKind::Import});
			document.Record({.Kind = OperationKind::AddWrite, .Text = name});
			document.Record({.Kind = OperationKind::Connect, .From = 1, .To = 2});
			document.Record({.Kind = OperationKind::Connect, .From = 2, .To = 3});
			return document;
		}
	}

	void Editor::EnsureAssetPipelineState() {
		if (AssetPipeline != nullptr) return;
		AssetPipeline = std::make_unique<AssetPipelineState>();
		auto &fresh = *AssetPipeline;
		// Its own registry, so the palette offers asset steps only.
		fresh.Canvas = std::make_unique<engine::angview::View>(
			fresh.Graph, AssetPipelineTypes(), engine::angview::Theme::Dark()
		);
	}

	void Editor::OpenAssetPipeline(const std::string &name, bool bake) {
		EnsureAssetPipelineState();
		ShowAssetPipeline = true;
		auto &state = *AssetPipeline;
		const WorldId world = SelectionWorld.IsValid() ? static_cast<WorldId>(SelectionWorld) : Active;
		state.World = world;
		state.Queue.push_back(
			{AssetPipelineState::Action::Save, world, engine::core::Name(name), Starter(name)}
		);
		if (bake)
			state.Queue.push_back(
				{AssetPipelineState::Action::Bake, world, engine::core::Name(name), Starter(name)}
			);
		state.Open = name;
		state.Loaded = false;
	}

	void Editor::DrawAssetPipeline() {
		if (!ShowAssetPipeline) return;
		ENGINE_PROFILE("Studio::AssetPipeline");
		EnsureAssetPipelineState();
		auto &state = *AssetPipeline;
		const WorldId world = SelectionWorld.IsValid() ? static_cast<WorldId>(SelectionWorld) : Active;
		if (world != state.World) {
			state.World = world;
			state.Open.clear();
			state.Loaded = false;
			state.Dirty = false;
			state.Graph = {};
			state.History.Clear();
			state.Canvas->Select({});
			state.Canvas->GraphChanged();
		}

		ImGui::SetNextWindowSize(ImVec2(900, 520), ImGuiCond_FirstUseEver);
		if (!ImGui::Begin("Asset Pipeline", &ShowAssetPipeline)) {
			ImGui::End();
			return;
		}
		if (Universe == nullptr || !world.IsValid()) {
			ImGui::TextUnformatted("no world");
			ImGui::End();
			return;
		}

		// Names from the world every frame; the open document only when the
		// canvas is not already holding it.
		std::vector<std::string> names;
		bool openExists = false;
		Universe->Enter(world, [&](engine::ecs::Store &store) {
			const auto *set = store.Resource<engine::bake::PipelineSet>();
			if (set == nullptr) return;
			for (const auto name : set->Names())
				names.emplace_back(name.Text());
			if (state.Open.empty()) return;
			const auto *document = set->Find(engine::core::Name(state.Open));
			openExists = document != nullptr;
			if (document != nullptr && !state.Loaded) {
				std::string problem;
				if (!LoadAssetPipelineGraph(*document, state.Graph, problem)) state.Status = problem;
				state.Loaded = true;
				state.Dirty = false;
				state.History.Clear();
				state.Editing.clear();
				state.Canvas->Select({});
				state.Canvas->GraphChanged();
				state.FrameWanted = true;
			}
		});
		const bool editable = AuthorityOf(world) == EditAuthority::Authoritative;

		// Left: the world's pipelines.
		ImGui::BeginChild("pipelines", ImVec2(200, 0), ImGuiChildFlags_ResizeX);
		for (const auto &name : names) {
			const bool chosen = name == state.Open;
			if (ImGui::Selectable(name.c_str(), chosen) && !chosen) {
				state.Open = name;
				state.Loaded = false;
				state.Status.clear();
			}
		}
		if (names.empty()) ImGui::TextUnformatted("no pipelines");
		ImGui::Separator();
		ImGui::BeginDisabled(!editable);
		ImGui::SetNextItemWidth(-1);
		const bool entered = ImGui::InputTextWithHint(
			"##new",
			"new pipeline",
			state.NewName.data(),
			state.NewName.size(),
			ImGuiInputTextFlags_EnterReturnsTrue
		);
		if ((ImGui::Button("Add") || entered) && state.NewName[0] != '\0') {
			const std::string name = state.NewName.data();
			state.Queue.push_back(
				{AssetPipelineState::Action::Save, world, engine::core::Name(name), Starter(name)}
			);
			state.Open = name;
			state.Loaded = false;
			state.NewName[0] = '\0';
		}
		ImGui::EndDisabled();
		ImGui::EndChild();
		ImGui::SameLine();

		// Right: toolbar and canvas.
		ImGui::BeginGroup();
		if (state.Open.empty() || !openExists) {
			ImGui::TextUnformatted("select or add a pipeline");
			if (!state.Status.empty()) ImGui::TextUnformatted(state.Status.c_str());
			ImGui::EndGroup();
			ImGui::End();
			return;
		}
		const auto queue = [&](AssetPipelineState::Action action) {
			AssetPipelineState::Pending pending{action, world, engine::core::Name(state.Open), {}};
			std::string problem;
			if (action != AssetPipelineState::Action::Remove &&
				!SaveAssetPipelineGraph(state.Graph, pending.Document, problem)) {
				state.Status = problem;
				return;
			}
			state.Queue.push_back(std::move(pending));
		};
		ImGui::BeginDisabled(!editable || !state.Dirty);
		if (ImGui::Button("Save")) queue(AssetPipelineState::Action::Save);
		ImGui::SameLine();
		if (ImGui::Button("Revert")) state.Loaded = false;
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (ImGui::Button("Bake")) queue(AssetPipelineState::Action::Bake);
		ImGui::SameLine();
		if (ImGui::Button("Fit")) state.Canvas->Frame();
		ImGui::SameLine();
		const auto stepped = [&](bool moved) {
			if (!moved) return;
			state.Canvas->GraphChanged();
			state.Editing.clear();
			state.Dirty = true;
		};
		ImGui::BeginDisabled(!state.History.CanUndo());
		if (ImGui::Button("Undo")) stepped(state.History.Undo(state.Graph));
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::BeginDisabled(!state.History.CanRedo());
		if (ImGui::Button("Redo")) stepped(state.History.Redo(state.Graph));
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::BeginDisabled(!editable);
		if (ImGui::Button("Delete")) queue(AssetPipelineState::Action::Remove);
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::Text(
			"%s%s  %zu nodes", state.Open.c_str(), state.Dirty ? " *" : "", state.Graph.Nodes().size()
		);
		if (!state.Status.empty()) ImGui::TextUnformatted(state.Status.c_str());
		Inspector(state);
		ImGui::BeginChild("canvas", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
		const ImVec2 canvasSize = ImGui::GetContentRegionAvail();
		state.Canvas->SetViewport({canvasSize.x, canvasSize.y});
		if (state.FrameWanted) {
			state.Canvas->Frame();
			state.FrameWanted = false;
		}
		engine::ui::NodeGraphCanvas("##asset-pipeline-graph", *state.Canvas);
		std::vector<engine::ang::EditRecord> records = state.Canvas->TakeEdits();
		if (!records.empty()) {
			state.History.Record(std::move(records));
			state.Editing.clear();
			state.Dirty = true;
		}
		ImGui::EndChild();
		ImGui::EndGroup();
		ImGui::End();
	}

	void Editor::ApplyAssetPipelinePending() {
		if (AssetPipeline == nullptr || AssetPipeline->Queue.empty() || Universe == nullptr) return;
		ENGINE_PROFILE("Studio::AssetPipelineActions");
		auto &state = *AssetPipeline;
		auto queue = std::move(state.Queue);
		state.Queue.clear();
		for (auto &pending : queue) {
			if (pending.Kind == AssetPipelineState::Action::Bake) {
				// Sources are names under the local `raw/` folder; written assets
				// land in `baked/` and register into this editor immediately.
				const auto paths = engine::assets::DefaultLocalPaths();
				std::vector<engine::bake::BakedAsset> baked;
				if (!BakeAssetPipeline(pending.Document, paths.Raw, paths.Baked, baked, state.Status))
					continue;
				size_t written = baked.size();
				for (const auto &asset : baked)
					RegisterBakedAsset(asset.Bytes, asset.Name);
				RefreshStoreContents();
				state.Status = "baked " + std::to_string(written) + " asset(s) from " +
							   std::string(pending.Pipeline.Text());
				ENGINE_INFO("asset pipeline: {}", state.Status);
				continue;
			}
			if (AuthorityOf(pending.World) != EditAuthority::Authoritative) {
				state.Status = "this world is not editable here";
				continue;
			}
			Universe->Enter(pending.World, [&](engine::ecs::Store &store) {
				engine::bake::PipelineSet set;
				if (const auto *held = store.Resource<engine::bake::PipelineSet>()) set = *held;
				if (pending.Kind == AssetPipelineState::Action::Remove) {
					(void)set.Remove(pending.Pipeline);
				} else if (!set.Set(pending.Pipeline, std::move(pending.Document))) {
					state.Status = "a pipeline needs a name";
					return;
				}
				if (set.Count() == 0)
					store.RemoveResource<engine::bake::PipelineSet>();
				else
					store.SetResource(set);
			});
			MarkModified();
			if (pending.Kind == AssetPipelineState::Action::Remove) {
				if (state.Open == pending.Pipeline.Text()) state.Open.clear();
				state.Status = "deleted " + std::string(pending.Pipeline.Text());
			} else {
				state.Dirty = false;
				state.Status = "saved " + std::string(pending.Pipeline.Text());
			}
		}
	}
}
