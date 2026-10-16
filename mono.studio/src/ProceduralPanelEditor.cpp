#include "AssetPublication.hpp"

#include <engine/assets/LocalStore.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/scene/DrawInstance.hpp>
#include <engine/scene/EditableMesh.hpp>
#include <engine/script/ProceduralScene.hpp>

#include <algorithm>
#include <assetc/Procedural.hpp>
#include <cmath>
#include <studio/Editor.hpp>
#include <studio/ProceduralEditor.hpp>
#include <studio/ProceduralPanel.hpp>

namespace studio {
	namespace procedural_editor_adapter {
		const engine::script::ProceduralSceneTarget *
		Selected(const ProceduralEditor &preview, const ProceduralPanelState &panel) {
			const auto *authority = preview.PreviewStore().Resource<engine::script::ProceduralAuthority>();
			if (!authority || panel.RegionIndex < 0 ||
				static_cast<size_t>(panel.RegionIndex) >= authority->Regions.size())
				return nullptr;
			const auto scope = authority->Regions[static_cast<size_t>(panel.RegionIndex)].Scope;
			for (const auto &target : preview.Targets())
				if (target.Scope.X == scope.X && target.Scope.Y == scope.Y &&
					target.Scope.CellsPerSide == scope.CellsPerSide)
					return &target;
			return nullptr;
		}
		std::vector<std::byte> HistoryBytes(const std::vector<uint8_t> &bytes) {
			const auto view = std::as_bytes(std::span(bytes));
			return {view.begin(), view.end()};
		}
	}
	void Editor::ReleaseProceduralPreview() {
		ProceduralPreviewWanted = false;
		ProceduralPreview.reset();
		ProceduralPanel.reset();
		ProceduralWorld = {};
		if (ProceduralPreviewOwner.IsValid()) {
			EditableMeshes.ForgetOwner(ProceduralPreviewOwner);
			Renderer.DropContentOwner(ProceduralPreviewOwner);
		}
		if (ProceduralPreviewTexture.IsValid()) Renderer.DropTexture(ProceduralPreviewTexture);
		ProceduralPreviewOwner = {};
		ProceduralPreviewTexture = {};
		ProceduralPreviewHandle = nullptr;
		ProceduralRenderedMesh = {};
		ProceduralRenderedRevision = 0;
	}
	void Editor::DrawProceduralGeneration() {
		if (!ShowProcedural) {
			ProceduralPreviewWanted = false;
			return;
		}
		const auto world = SelectionWorld.IsValid() ? SelectionWorld : Active;
		if (ProceduralWorld != world) ReleaseProceduralPreview();
		ProceduralWorld = world;
		if (!ProceduralPanel) ProceduralPanel = std::make_unique<ProceduralPanelState>();
		bool drawn = false;
		if (Universe && world.IsValid())
			Universe->Enter(world, [&](engine::ecs::Store &store) {
				drawn = true;
				DrawProceduralPanel(
					*ProceduralPanel, ShowProcedural, &store, ProceduralPreview.get(), ProceduralPreviewHandle
				);
			});
		if (!drawn)
			DrawProceduralPanel(
				*ProceduralPanel, ShowProcedural, nullptr, ProceduralPreview.get(), ProceduralPreviewHandle
			);
		if (!ShowProcedural || !ProceduralPreview) return;
		const auto *target = procedural_editor_adapter::Selected(*ProceduralPreview, *ProceduralPanel);
		const auto *mesh =
			target ? ProceduralPreview->PreviewStore().Get<engine::scene::EditableMesh>(target->Mesh)
				   : nullptr;
		if (mesh && mesh->Revision &&
			(ProceduralRenderedMesh != target->Mesh || ProceduralRenderedRevision != mesh->Revision ||
			 ProceduralRenderedOrbit != ProceduralPanel->Orbit))
			ProceduralPreviewWanted = true;
	}
	void Editor::ApplyProceduralPending() {
		ENGINE_PROFILE("Studio::ProceduralActions");
		using namespace engine;
		if (!ProceduralPanel || !Universe || !ProceduralWorld.IsValid()) return;
		if (!ShowProcedural && ProceduralPanel->Pending.empty()) {
			if (ProceduralPreview) {
				ProceduralPreview->Cancel();
				procedural::Diagnostic closedDiagnostic;
				ProceduralPreview->Pump(closedDiagnostic);
			}
			return;
		}
		auto actions = std::move(ProceduralPanel->Pending);
		ProceduralPanel->Pending.clear();
		if (!actions.empty()) {
			ProceduralPanel->Status.clear();
			ProceduralPanel->HasPreviewDiagnostic = false;
		}
		procedural::Diagnostic diagnostic;
		if (!ProceduralPreview) {
			ProceduralPreview = std::make_unique<ProceduralEditor>();
			ProceduralPreviewOwner = core::Name(
				"studio.procedural/" + std::to_string(ProceduralPreview->PreviewStore().Identity())
			);
			ProceduralPreviewTexture = core::Name(std::string(ProceduralPreviewOwner.Text()) + "/image");
		}
		for (const auto &action : actions) {
			if (IsProceduralAuthoringAction(action.Kind)) {
				if (AuthorityOf(ProceduralWorld) != EditAuthority::Authoritative) continue;
				bool changed = false;
				Universe->Enter(ProceduralWorld, [&](ecs::Store &store) {
					std::vector<uint8_t> before, after;
					if (store.Resource<script::ProceduralAuthority>() &&
						!script::EncodeProceduralAuthority(store, before, diagnostic))
						return;
					if (!ApplyProceduralAuthoringAction(store, action, diagnostic) ||
						!script::EncodeProceduralAuthority(store, after, diagnostic))
						return;
					changed = before != after;
					if (changed && Commands)
						Commands->RecordProceduralAuthority(
							ProceduralWorld,
							procedural_editor_adapter::HistoryBytes(before),
							procedural_editor_adapter::HistoryBytes(after),
							"Edit procedural world"
						);
				});
				if (changed) MarkModified();
			} else if (action.Kind == ProceduralActionKind::Preview) {
				ProceduralPreview->Start();
			} else if (action.Kind == ProceduralActionKind::Cancel) {
				ProceduralPreview->Cancel();
				ProceduralPanel->Status = "Preview canceled";
			} else if (action.Kind == ProceduralActionKind::Activate) {
				bool activated = false;
				Universe->Enter(ProceduralWorld, [&](ecs::Store &store) {
					activated = ProceduralPreview->Activate(
						store, ProceduralPreview->PreviewStore().Time().Tick, diagnostic
					);
				});
				if (activated) ProceduralPreviewWanted = true;
			} else if (action.Kind == ProceduralActionKind::Bake ||
					   action.Kind == ProceduralActionKind::Publish) {
				std::string failure;
				if (action.Kind == ProceduralActionKind::Publish &&
					!ReadAssetPublishingKey(AssetSigningKey, Content, failure)) {
					ShowAssets = true;
					ProceduralPanel->Status = failure;
					continue;
				}
				script::ProceduralPublication accepted;
				bool captured = false;
				Universe->Enter(ProceduralWorld, [&](ecs::Store &store) {
					captured = ProceduralPreview->CaptureAccepted(store, action.Scope, accepted, diagnostic);
				});
				if (!captured) {
					ProceduralPanel->Status = "Activate a preview for the current source before baking";
					continue;
				}
				bake::ProceduralBake cooked;
				if (!assetc::CookProceduralArtifact(accepted.ArtifactBytes, action.Scope, cooked, failure)) {
					ProceduralPanel->Status = failure;
					continue;
				}
				bool current = false;
				Universe->Enter(ProceduralWorld, [&](ecs::Store &store) {
					script::ProceduralPublication checked;
					current = ProceduralPreview->CaptureAccepted(store, action.Scope, checked, diagnostic) &&
							  checked.ArtifactDigest == accepted.ArtifactDigest &&
							  checked.GenerationKey == accepted.GenerationKey;
				});
				if (!current) {
					ProceduralPanel->Status = "Source changed before bake publication";
					continue;
				}
				if (!assetc::PublishProceduralBake(
						assets::DefaultLocalPaths().Baked, action.Output, cooked, failure
					)) {
					ProceduralPanel->Status = failure;
					continue;
				}
				for (const auto &mesh : cooked.Meshes)
					RegisterBakedAsset(mesh.Bytes, mesh.Name);
				RegisterBakedAsset(cooked.DescriptionBytes, action.Output);
				RefreshStoreContents();
				if (action.Kind == ProceduralActionKind::Publish && !PublishAssets(AssetSigningKey)) {
					ProceduralPanel->Status = AssetStatus;
					continue;
				}
				if (action.Kind == ProceduralActionKind::Publish) RebuildContentClients();
				ProceduralPanel->Status = "Accepted " + action.Output;
			}
			if (diagnostic.Code != procedural::Status::Ok) {
				ProceduralPanel->Status = diagnostic.Field;
				diagnostic = {};
			}
		}
		if (!ShowProcedural) {
			ProceduralPreview->Cancel();
			ProceduralPreview->Pump(diagnostic);
			return;
		}
		procedural::Diagnostic syncDiagnostic;
		bool synchronized = false;
		Universe->Enter(ProceduralWorld, [&](ecs::Store &store) {
			synchronized = ProceduralPreview->SyncSource(store, syncDiagnostic);
		});
		procedural::Diagnostic pumpDiagnostic;
		const bool pumped = ProceduralPreview->Pump(pumpDiagnostic);
		if (!synchronized) {
			ProceduralPanel->Status = syncDiagnostic.Field;
			ProceduralPanel->HasPreviewDiagnostic = true;
		} else if (!pumped) {
			ProceduralPanel->Status = pumpDiagnostic.Field;
			ProceduralPanel->HasPreviewDiagnostic = true;
		} else if (ProceduralPanel->HasPreviewDiagnostic) {
			ProceduralPanel->Status.clear();
			ProceduralPanel->HasPreviewDiagnostic = false;
		}
	}
	bool Editor::RenderProceduralPreviewSlot() {
		using namespace engine;
		if (!ProceduralPreviewWanted || !ShowProcedural || !ProceduralPreview || !ProceduralPanel ||
			Renderer.Backend().Device == nullptr)
			return false;
		const auto *target = procedural_editor_adapter::Selected(*ProceduralPreview, *ProceduralPanel);
		if (!target) {
			ProceduralPreviewWanted = false;
			return false;
		}
		auto &store = ProceduralPreview->PreviewStore();
		const auto *mesh = store.Get<scene::EditableMesh>(target->Mesh);
		const auto *bounds = store.Get<scene::Bounds>(target->Part);
		if (!mesh || !bounds) {
			ProceduralPreviewWanted = false;
			return false;
		}
		ENGINE_PROFILE_CAT("procedural preview", core::ProfileCategory::Render);
		EditableMeshes.Refresh(store, Renderer, ProceduralPreviewOwner);
		if (LastPostProcessShader.IsValid()) {
			Renderer.ClearPostProcessShader();
			LastPostProcessShader = {};
		}
		scene::DrawInstance instance;
		instance.Mesh = target->Content;
		instance.HalfExtent = bounds->HalfExtent;
		const float radius = std::max(1.0f, bounds->HalfExtent.Magnitude());
		const float angle = ProceduralPanel->Orbit;
		const core::Vector3 direction{std::cos(angle), 0.9f, std::sin(angle)};
		scene::Camera camera;
		camera.FieldOfViewRadians = 0.6109f;
		camera.NearPlane = std::max(0.01f, radius * 0.001f);
		camera.FarPlane = radius * 100.0f;
		render::SceneTarget sceneTarget;
		sceneTarget.Width = sceneTarget.Height = 512;
		render::View view;
		view.Camera = camera;
		view.CameraFrame = core::CFrame::LookAt(
			direction.Unit() * (radius / std::tan(camera.FieldOfViewRadians * 0.5f) * 1.15f), {}
		);
		view.Instances = mesh->Indices.empty() ? std::span<const scene::DrawInstance>{}
											   : std::span<const scene::DrawInstance>(&instance, 1);
		view.Target = &sceneTarget;
		view.Slot = PreviewSlot();
		view.WorldName = ProceduralPreviewOwner;
		view.ContentOwner = ProceduralPreviewOwner;
		Renderer.Render(std::span<const render::View>(&view, 1), Overlay, nullptr, true, &Interface);
		if (Renderer.CaptureSceneTexture(PreviewSlot(), ProceduralPreviewTexture)) {
			ProceduralPreviewHandle = Renderer.TextureHandle(ProceduralPreviewTexture);
			ProceduralRenderedMesh = target->Mesh;
			ProceduralRenderedRevision = mesh->Revision;
			ProceduralRenderedOrbit = angle;
			ProceduralPanel->ImageScope = target->Scope;
			ProceduralPanel->HasImageScope = true;
		}
		ProceduralPreviewWanted = false;
		return true;
	}
}
