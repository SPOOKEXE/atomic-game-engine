#include <engine/assets/ContentHash.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/procedural/Artifact.hpp>
#include <engine/scene/EditableMesh.hpp>
#include <engine/scene/Part.hpp>
#include <engine/script/ProceduralScene.hpp>

#include <algorithm>
#include <new>
#include <studio/ProceduralEditor.hpp>
#include <utility>
#include <vector>

namespace studio {
	using namespace engine;
	namespace procedural_preview_detail {
		bool SameScope(procedural::RegionScope left, procedural::RegionScope right) {
			return left.X == right.X && left.Y == right.Y && left.CellsPerSide == right.CellsPerSide;
		}
		bool Refuse(procedural::Diagnostic &diagnostic, procedural::Status status, const char *field) {
			diagnostic = {status, field};
			return false;
		}
		procedural::Digest Hash(std::span<const uint8_t> bytes) {
			return {assets::Hasher::Of(std::as_bytes(bytes)).Digest};
		}
		bool CaptureKey(
			const ecs::Store &source,
			procedural::RegionScope scope,
			std::vector<uint8_t> &key,
			procedural::Diagnostic &diagnostic
		) {
			procedural::CompiledRecipe recipe;
			procedural::RegionRequest request;
			return script::CaptureProceduralRegion(source, scope, recipe, request, diagnostic) &&
				   procedural::BuildRegionKey(recipe, request, key, diagnostic) == procedural::Status::Ok;
		}
		const script::ProceduralPublication *
		Publication(const ecs::Store &store, procedural::RegionScope scope) {
			const auto *published = store.Resource<script::ProceduralPublished>();
			if (!published) return nullptr;
			const auto found =
				std::find_if(published->Regions.begin(), published->Regions.end(), [&](const auto &row) {
					return SameScope(row.Scope, scope);
				});
			return found == published->Regions.end() ? nullptr : &*found;
		}
	}

	struct ProceduralEditor::State {
		struct Request {
			uint64_t Ticket = 0;
			script::ProceduralSceneTarget Target{};
		};
		ecs::Store Preview{"procedural-preview"};
		// Destruction runs in reverse declaration order, so Jobs joins before Preview dies.
		script::ProceduralJobs Jobs{Preview};
		parallel::JobContext Context;
		std::vector<uint8_t> AdmissionBytes{};
		std::vector<script::ProceduralSceneTarget> Targets{};
		std::vector<Request> Requests{};
		std::vector<script::ProceduralProgress> Progress{};
		bool Enabled = false;
		bool HasSource = false;

		explicit State(parallel::JobContext context) : Context(context) {
			Targets.reserve(script::ProceduralHostLimits::MaximumRegions);
			Requests.reserve(script::ProceduralHostLimits::MaximumRetainedRequests);
			Progress.reserve(script::ProceduralHostLimits::MaximumRetainedRequests);
		}
		void CancelRequests() {
			for (const auto &request : Requests)
				Jobs.Cancel(request.Ticket);
		}
		void RefreshProgress() {
			Progress.clear();
			for (const auto &request : Requests) {
				script::ProceduralProgress progress;
				if (Jobs.Progress(request.Ticket, progress)) Progress.push_back(progress);
			}
		}
		bool ReconcileTargets(procedural::Diagnostic &diagnostic) {
			const auto *authority = Preview.Resource<script::ProceduralAuthority>();
			const auto registered = [&](procedural::RegionScope scope) {
				return std::any_of(
					authority->Regions.begin(), authority->Regions.end(), [&](const auto &row) {
						return procedural_preview_detail::SameScope(row.Scope, scope);
					}
				);
			};
			if (auto *published = Preview.ResourceMutable<script::ProceduralPublished>()) {
				std::erase_if(published->Regions, [&](const auto &row) { return !registered(row.Scope); });
			}
			std::erase_if(Targets, [&](const auto &target) {
				if (registered(target.Scope)) return false;
				Preview.DestroyInstance(target.Part);
				Preview.DestroyInstance(target.Mesh);
				return true;
			});
			for (const auto &region : authority->Regions) {
				if (std::any_of(Targets.begin(), Targets.end(), [&](const auto &target) {
						return procedural_preview_detail::SameScope(target.Scope, region.Scope);
					}))
					continue;
				const auto mesh = Preview.CreateInstance(scene::EditableMeshClass(), "ProceduralRegionMesh");
				scene::PartDesc description;
				description.Class = ecs::Classes::Find(core::Name("MeshPart"));
				description.Mesh = scene::EditableMeshContentName(Preview, mesh);
				const auto part = scene::MakePart(Preview, description);
				if (mesh == ecs::NULL_ENTITY || part == ecs::NULL_ENTITY) {
					if (part != ecs::NULL_ENTITY) Preview.DestroyInstance(part);
					if (mesh != ecs::NULL_ENTITY) Preview.DestroyInstance(mesh);
					return procedural_preview_detail::Refuse(
						diagnostic, procedural::Status::AllocationFailed, "preview.target"
					);
				}
				auto *collider = Preview.GetMutable<scene::Collider>(part);
				collider->Trigger = true;
				collider->Mask = spatial::LayerMask::None();
				collider->CanQuery = false;
				auto *visual = Preview.GetMutable<scene::Visual>(part);
				visual->Tint = core::Color3{1, 1, 1};
				visual->Transparency = 0;
				visual->Visible = true;
				script::ProceduralSceneTarget target;
				if (!script::BindProceduralSceneTarget(
						Preview, region.Scope, mesh, part, target, diagnostic
					)) {
					Preview.DestroyInstance(part);
					Preview.DestroyInstance(mesh);
					return false;
				}
				Targets.push_back(target);
			}
			return true;
		}
	};

	bool CurrentProceduralKey(
		const ecs::Store &source,
		procedural::RegionScope scope,
		procedural::Digest &out,
		procedural::Diagnostic &diagnostic
	) {
		try {
			std::vector<uint8_t> key;
			if (!procedural_preview_detail::CaptureKey(source, scope, key, diagnostic)) return false;
			out = procedural_preview_detail::Hash(key);
			diagnostic = {};
			return true;
		} catch (const std::bad_alloc &) {
			return procedural_preview_detail::Refuse(
				diagnostic, procedural::Status::AllocationFailed, "preview.key"
			);
		}
	}

	ProceduralEditor::ProceduralEditor(parallel::JobContext context)
		: Held(std::make_unique<State>(context)) {}
	ProceduralEditor::~ProceduralEditor() = default;

	bool ProceduralEditor::SyncSource(const ecs::Store &source, procedural::Diagnostic &diagnostic) {
		ENGINE_PROFILE("Studio::ProceduralSync");
		if (!source.IsOnOwningThread() || !Held->Preview.IsOnOwningThread())
			return procedural_preview_detail::Refuse(
				diagnostic, procedural::Status::InvalidInput, "preview.owner"
			);
		try {
			std::vector<uint8_t> bytes;
			if (!script::EncodeProceduralAuthority(source, bytes, diagnostic)) {
				Held->HasSource = false;
				Held->CancelRequests();
				Held->RefreshProgress();
				return false;
			}
			if (Held->HasSource && bytes == Held->AdmissionBytes) {
				diagnostic = {};
				return true;
			}
			Held->HasSource = false;
			if (!Held->Jobs.SynchronizePreviewInputs(Held->Preview, bytes, diagnostic) ||
				!Held->ReconcileTargets(diagnostic)) {
				Held->CancelRequests();
				Held->RefreshProgress();
				return false;
			}
			Held->AdmissionBytes.swap(bytes);
			Held->HasSource = true;
			Held->RefreshProgress();
			diagnostic = {};
			return true;
		} catch (const std::bad_alloc &) {
			Held->HasSource = false;
			Held->CancelRequests();
			return procedural_preview_detail::Refuse(
				diagnostic, procedural::Status::AllocationFailed, "preview.sync"
			);
		}
	}
	void ProceduralEditor::Start() {
		Held->Enabled = true;
	}
	void ProceduralEditor::Cancel() {
		Held->Enabled = false;
		Held->CancelRequests();
		Held->RefreshProgress();
	}

	bool ProceduralEditor::Pump(procedural::Diagnostic &diagnostic) {
		ENGINE_PROFILE("Studio::ProceduralPump");
		try {
			Held->Jobs.Poll();
			bool succeeded = true;
			for (size_t index = 0; index < Held->Requests.size();) {
				const auto ticket = Held->Requests[index].Ticket;
				const auto completions = Held->Jobs.Completions();
				const auto completed =
					std::find_if(completions.begin(), completions.end(), [&](const auto &row) {
						return row.Ticket == ticket;
					});
				if (completed == completions.end() ||
					completed->Status == script::ProceduralJobStatus::Ready) {
					++index;
					continue;
				}
				if (completed->Status == script::ProceduralJobStatus::Failed) {
					diagnostic = completed->Diagnostic;
					Held->Enabled = false;
					succeeded = false;
				}
				Held->Jobs.Release(ticket);
				Held->Requests.erase(Held->Requests.begin() + static_cast<std::ptrdiff_t>(index));
			}
			if (Held->Enabled && Held->HasSource) {
				for (auto &target : Held->Targets) {
					if (Held->Jobs.ReservedBytes() > script::ProceduralHostLimits::MaximumRetainedBytes -
														 script::ProceduralHostLimits::BytesPerRequest)
						break;
					if (std::any_of(Held->Requests.begin(), Held->Requests.end(), [&](const auto &request) {
							return procedural_preview_detail::SameScope(request.Target.Scope, target.Scope);
						}))
						continue;
					procedural::Digest current;
					if (!CurrentProceduralKey(Held->Preview, target.Scope, current, diagnostic)) {
						Held->Enabled = false;
						return false;
					}
					const auto *accepted =
						procedural_preview_detail::Publication(Held->Preview, target.Scope);
					if (accepted && accepted->GenerationKey == current) continue;
					if (!script::CaptureProceduralSceneTarget(
							Held->Preview, target.Scope, target.Mesh, target.Part, target, diagnostic
						)) {
						Held->Enabled = false;
						return false;
					}
					const auto ticket = Held->Jobs.Submit(Held->Preview, target.Scope, Held->Context);
					if (ticket == 0) {
						diagnostic = Held->Jobs.LastDiagnostic();
						Held->Enabled = false;
						return false;
					}
					Held->Requests.push_back({ticket, target});
				}
			}
			Held->RefreshProgress();
			if (succeeded) diagnostic = {};
			return succeeded;
		} catch (const std::bad_alloc &) {
			Cancel();
			return procedural_preview_detail::Refuse(
				diagnostic, procedural::Status::AllocationFailed, "preview.pump"
			);
		}
	}

	bool ProceduralEditor::Activate(
		const ecs::Store &source, uint64_t activationTick, procedural::Diagnostic &diagnostic
	) {
		ENGINE_PROFILE("Studio::ProceduralActivate");
		if (!SyncSource(source, diagnostic)) return false;
		try {
			Held->Jobs.Poll();
			std::vector<uint64_t> tickets;
			std::vector<script::ProceduralSceneTarget> targets;
			for (const auto &request : Held->Requests) {
				const auto completions = Held->Jobs.Completions();
				if (!std::any_of(completions.begin(), completions.end(), [&](const auto &row) {
						return row.Ticket == request.Ticket &&
							   row.Status == script::ProceduralJobStatus::Ready;
					}))
					continue;
				tickets.push_back(request.Ticket);
				targets.push_back(request.Target);
			}
			if (tickets.empty())
				return procedural_preview_detail::Refuse(
					diagnostic, procedural::Status::InvalidInput, "preview.not_ready"
				);
			if (!Held->Jobs.PublishSceneBatch(Held->Preview, tickets, targets, activationTick)) {
				diagnostic = Held->Jobs.LastDiagnostic();
				return false;
			}
			for (const auto ticket : tickets)
				Held->Jobs.Release(ticket);
			std::erase_if(Held->Requests, [&](const auto &request) {
				return std::find(tickets.begin(), tickets.end(), request.Ticket) != tickets.end();
			});
			Held->RefreshProgress();
			diagnostic = {};
			return true;
		} catch (const std::bad_alloc &) {
			return procedural_preview_detail::Refuse(
				diagnostic, procedural::Status::AllocationFailed, "preview.activate"
			);
		}
	}

	bool ProceduralEditor::CaptureAccepted(
		const ecs::Store &source,
		procedural::RegionScope scope,
		script::ProceduralPublication &out,
		procedural::Diagnostic &diagnostic
	) const {
		if (!Held->Preview.IsOnOwningThread())
			return procedural_preview_detail::Refuse(
				diagnostic, procedural::Status::InvalidInput, "preview.owner"
			);
		try {
			std::vector<uint8_t> key;
			if (!procedural_preview_detail::CaptureKey(source, scope, key, diagnostic)) return false;
			const auto *accepted = procedural_preview_detail::Publication(Held->Preview, scope);
			if (!accepted || accepted->GenerationKey != procedural_preview_detail::Hash(key))
				return procedural_preview_detail::Refuse(
					diagnostic, procedural::Status::StaleArtifact, "preview.accepted_key"
				);
			if (accepted->ArtifactDigest != procedural_preview_detail::Hash(accepted->ArtifactBytes))
				return procedural_preview_detail::Refuse(
					diagnostic, procedural::Status::InvalidInput, "preview.artifact_digest"
				);
			procedural::RegionResult checked;
			if (procedural::DecodeRegionResult(accepted->ArtifactBytes, checked, diagnostic, key) !=
				procedural::Status::Ok)
				return false;
			auto candidate = *accepted;
			out = std::move(candidate);
			diagnostic = {};
			return true;
		} catch (const std::bad_alloc &) {
			return procedural_preview_detail::Refuse(
				diagnostic, procedural::Status::AllocationFailed, "preview.capture"
			);
		}
	}

	ecs::Store &ProceduralEditor::PreviewStore() {
		return Held->Preview;
	}
	const ecs::Store &ProceduralEditor::PreviewStore() const {
		return Held->Preview;
	}
	std::span<const script::ProceduralSceneTarget> ProceduralEditor::Targets() const {
		return Held->Targets;
	}
	std::span<const script::ProceduralProgress> ProceduralEditor::Progress() const {
		return Held->Progress;
	}
	bool ProceduralEditor::Running() const {
		return Held->Enabled;
	}
	size_t ProceduralEditor::ReservedBytes() const {
		return Held->Jobs.ReservedBytes();
	}
}
