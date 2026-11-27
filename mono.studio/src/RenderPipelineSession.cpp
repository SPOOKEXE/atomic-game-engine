#include <engine/graph/PipelineDocument.hpp>
#include <engine/render/pipeline/PipelineAdmission.hpp>

#include <studio/Editor.hpp>
#include <studio/RenderPipelineGraph.hpp>

namespace studio {
	bool ValidateRenderPipelineAdmission(
		const engine::graph::PipelineDocument &document,
		const engine::render::Renderer &renderer,
		std::string &error
	) {
		const engine::render::PipelineAdmissionResult admission = renderer.ValidatePipelineDocument(document);
		if (admission) {
			error.clear();
			return true;
		}
		error = engine::render::FormatPipelineFailure(*admission.Failure);
		return false;
	}

	void Editor::LoadRenderPipeline(WorldId world, engine::core::Name wanted) {
		RenderPipelineTriedWorld = world.Index;
		PrepareRenderPipelineCanvas();
		RenderPipelineHistory.Clear();
		RenderPipelineEditing.clear();
		if (Universe == nullptr || !world.IsValid()) {
			RenderPipelineGraph = {};
			RenderPipelineCanvas->GraphChanged();
			RenderPipelineWorld = {};
			RenderPipelineStatus = "no active world";
			return;
		}

		engine::core::Name selected = wanted;
		if (!selected.IsValid()) {
			selected = Universe->SettingsOf(world).RenderingProfile;
		}
		if (RenderingProfiles.Count() == 0) {
			RenderingProfiles.Set(engine::core::Name("Default PBR"), engine::graph::DefaultPbrDocument());
		}
		if (RenderingProfiles.Find(selected) == nullptr) {
			selected = RenderingProfiles.Find(engine::core::Name("Default PBR")) != nullptr
						   ? engine::core::Name("Default PBR")
						   : RenderingProfiles.Names().front();
		}
		const engine::graph::PipelineDocument document = *RenderingProfiles.Find(selected);

		RenderPipelineWorld = world;
		RenderPipelineName = selected;
		RenderPipelineInstalledName =
			engine::core::Name(std::string(selected.Text()) + "#" + std::to_string(world.Index));
		RenderPipelineBasis = document;
		RenderPipelineLoaded = engine::graph::Write(document);
		RenderPipelineDirty = false;

		const bool loaded = LoadRenderPipelineGraph(document, RenderPipelineGraph, RenderPipelineStatus);
		RenderPipelineCanvas->Select({});
		RenderPipelineCanvas->ClearRunFeedback();
		RenderPipelineCanvas->GraphChanged();
		if (!loaded) {
			return;
		}
		RenderPipelineStatus = "loaded " + std::string(selected.Text());
		RenderPipelineFrameWanted = true;
	}

	bool Editor::SaveRenderPipeline() {
		engine::graph::PipelineDocument saved;
		if (!SaveRenderPipelineGraph(RenderPipelineGraph, RenderPipelineBasis, saved, RenderPipelineStatus)) {
			return false;
		}
		if (Universe == nullptr || !RenderPipelineWorld.IsValid() || !RenderPipelineName.IsValid()) {
			RenderPipelineStatus = "no universe to save into";
			return false;
		}
		if (!ValidateRenderPipelineAdmission(saved, Renderer, RenderPipelineStatus)) {
			return false;
		}

		RenderingProfiles.Set(RenderPipelineName, saved);
		RenderPipelineInstalledName = engine::core::Name(
			std::string(RenderPipelineName.Text()) + "#" + std::to_string(RenderPipelineWorld.Index)
		);
		PipelineSelected.clear();
		MarkModified();

		RenderPipelineBasis = saved;
		RenderPipelineLoaded = engine::graph::Write(saved);
		RenderPipelineDirty = false;
		RenderPipelineStatus = "saved " + std::string(RenderPipelineName.Text()) + " into the universe";
		return true;
	}

	bool Editor::ApplyRenderingProfileText(engine::core::Name profile, const std::string &text) {
		if (text.empty()) {
			RenderingProfiles.Remove(profile);
		} else {
			engine::graph::PipelineDocument document;
			engine::core::Name offender;
			if (engine::graph::Read(text, document, offender) != engine::graph::PipelineDocumentStatus::Ok) {
				return false;
			}
			RenderingProfiles.Set(profile, std::move(document));
		}
		MarkModified();
		// The pipeline editor holds its own copy; reload it so it shows this.
		if (RenderPipelineName == profile && RenderPipelineWorld.IsValid()) {
			LoadRenderPipeline(RenderPipelineWorld, profile);
		}
		return true;
	}

	bool Editor::EditRenderingProfile(
		engine::core::Name profile,
		const engine::graph::PipelineDocument &after,
		std::string description,
		std::string &error
	) {
		// Attributed to the active world, or any world: a profile belongs to the
		// universe, and the world only gives the step somewhere to live.
		const WorldId attributed = Active.IsValid() || Universe == nullptr || Universe->Worlds().empty()
									   ? Active
									   : Universe->Worlds().front();
		if (Commands == nullptr || !attributed.IsValid()) {
			error = "this editor has no history to record a profile edit into";
			return false;
		}
		const engine::graph::PipelineDocument *before = RenderingProfiles.Find(profile);
		std::string beforeText = before != nullptr ? engine::graph::Write(*before) : std::string();
		std::string afterText = engine::graph::Write(after);
		Commands->SetRenderingProfileApplier([this](engine::core::Name name, const std::string &text) {
			return ApplyRenderingProfileText(name, text);
		});
		if (!ApplyRenderingProfileText(profile, afterText)) {
			error = "the edited profile could not be read back";
			return false;
		}
		Commands->RecordRenderingProfile(
			attributed, profile, std::move(beforeText), std::move(afterText), std::move(description)
		);
		return true;
	}
}
