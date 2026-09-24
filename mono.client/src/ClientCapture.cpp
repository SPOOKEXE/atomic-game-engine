#include "PortalReadiness.hpp"

#include <engine/core/Clock.hpp>
#include <engine/core/Log.hpp>
#include <engine/render/PortalImageImport.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/CameraContinuation.hpp>
#include <engine/scene/CameraPortalView.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Controls.hpp>
#include <engine/scene/Services.hpp>

#include <chrono>
#include <client/Client.hpp>
#include <client/Replicated.hpp>
#include <cstdlib>
#include <fstream>
#include <nlohmann/json.hpp>

namespace client {

	void Client::CaptureFrame(const engine::render::View &view, engine::world::WorldId inputWorld) {
		if (Settings.CaptureSequence.empty()) return;
		using namespace engine;
		using nlohmann::json;
		const auto vector = [](const core::Vector3 &value) {
			return json::array({value.X, value.Y, value.Z});
		};
		const auto pose = [&](const core::CFrame &value) {
			const auto &rotation = value.Rotation();
			return json{
				{"position", vector(value.Position)},
				{"rotation", json::array({rotation.x, rotation.y, rotation.z, rotation.w})}
			};
		};
		json frame{
			{"frame", FramesDrawn},
			{"loading_portals", WaitingForPortalViews()},
			{"submitted_move_tick", SubmittedMoveTick},
			{"submitted_move_direction", CapturedSubmittedMoveDirection},
			{"seconds", core::Clock::Seconds()},
			{"input_world", std::string(Universe_->NameOf(inputWorld).Text())},
			{"view_world", std::string(view.WorldName.Text())},
			{"content_owner", std::string(view.ContentOwner.Text())},
			{"delivered_meshes", ContentMeshes},
			{"delivered_textures", ContentTextures},
			{"camera", pose(view.CameraFrame)},
			{"field_of_view", view.Camera.FieldOfViewRadians},
			{"near", view.Camera.NearPlane},
			{"far", view.Camera.FarPlane},
			{"eye_image", view.EyeImage != 0},
			{"eye_rig", view.EyeRig},
			{"instances", view.Instances.size()},
			{"portals", view.Portals.size()},
		};
		if (std::getenv("PORTAL_BODY_ROWS_DIAGNOSTIC") != nullptr) {
			frame["eye_hidden_rows"] =
				std::vector<uint32_t>(view.EyeHiddenRows.begin(), view.EyeHiddenRows.end());
			json rows = json::array();
			for (const auto &row : view.Instances)
				rows.push_back({
					{"source_world", std::string(row.SourceWorld.Text())},
					{"mesh", std::string(row.Mesh.Text())},
					{"content_owner", std::string(view.ContentOwnerOf(row.SourceWorld).Text())},
					{"source", row.Source},
					{"rig", row.Rig},
					{"body_key", json::array({row.BodyKeyHigh, row.BodyKeyLow, row.BodyGeneration})},
					{"variant", row.Variant},
					{"position", vector(row.Frame.Position)},
					{"tint", json::array({row.Tint.R, row.Tint.G, row.Tint.B})},
					{"seam_normal", vector(row.SeamNormal)},
					{"seam_offset", row.SeamOffset},
					{"seam_mask", row.SeamMask},
				});
			frame["draw_rows"] = std::move(rows);
		}
		const double portalCaptureNow = core::Clock::Seconds();
		PortalCaptureCandidate approachCapture;
		if (PortalApproach && PortalApproach->Connection) {
			const auto &connection = *PortalApproach->Connection;
			approachCapture = {
				PortalApproach->World,
				connection.Admitted(),
				connection.Joined(),
				connection.Live(),
				connection.Rejected(),
				portalCaptureNow >= PortalApproach->Deadline
			};
		}
		PortalCaptureCandidate successorCapture;
		if (PortalNext && PortalNext->Connection) {
			const auto &connection = *PortalNext->Connection;
			successorCapture = {
				PortalNext->World,
				connection.Admitted(),
				connection.Joined(),
				connection.Live(),
				connection.Rejected() || PortalNext->Refused,
				!PortalNext->Failure.empty() || portalCaptureNow >= PortalNext->Deadline
			};
		}
		const auto captureCandidate = [&](const PortalCaptureCandidate &candidate) {
			return json{
				{"world", std::string(Universe_->NameOf(candidate.World).Text())},
				{"admitted", candidate.Admitted},
				{"joined", candidate.Joined},
				{"live", candidate.Live},
				{"rejected", candidate.Rejected},
				{"failed", candidate.Failed}
			};
		};
		frame["portal_capture_route"] = {
			{"selected",
			 std::string(Universe_
							 ->NameOf(PortalCaptureDestination(Rendered, approachCapture, successorCapture))
							 .Text())},
			{"approach", captureCandidate(approachCapture)},
			{"successor", captureCandidate(successorCapture)},
		};
		if (PortalApproach) {
			frame["portal_capture_route"]["approach"]["active"] = PortalApproach->Active;
			frame["portal_capture_route"]["approach"]["distance"] = PortalApproach->LastDistance;
			frame["portal_capture_route"]["approach"]["through_origin"] =
				vector(PortalApproach->Route.Through.Origin);
		}
		if (const auto pipeline = Renderer.ResolvePipelineIdentity(view.Pipeline)) {
			frame["pipeline"] = {
				{"name", std::string(pipeline->Name.Text())},
				{"revision", pipeline->Revision},
			};
		}
		const auto memory = Renderer.MemoryStatistics();
		frame["gpu_memory"] = {
			{"live_bytes", memory.LiveBytes},
			{"texture_bytes", memory.TextureBytes},
			{"textures", memory.Textures},
			{"buffer_bytes", memory.BufferBytes},
			{"released_bytes", memory.ReleasedBytes}
		};
		// CaptureFrame runs before this frame's render. These are completed work
		// and residency observed at that boundary, with no guessed upload sizes.
		frame["previous_render_uploaded_bytes"] = LastFrame.UploadedBytes;
		const auto import = Renderer.PortalImageUsage();
		frame["portal_import"] = {
			{"images", import.Images},
			{"pending_cpu_bytes", import.PendingCpuBytes},
			{"texture_bytes", import.TextureBytes},
			{"cached_texture_bytes", import.CachedTextureBytes},
			{"staging_bytes", import.StagingBytes},
			{"uploads", import.Uploads},
			{"uploaded_bytes", import.UploadedBytes},
			{"reuses", import.Reuses},
		};
		if (PortalImages) {
			const auto inbox = PortalImages->InboxUsage();
			frame["portal_inbox"] = {
				{"pending_count", inbox.PendingCount},
				{"pending_bytes", inbox.PendingBytes},
				{"held_count", inbox.HeldCount},
				{"held_bytes", inbox.HeldBytes},
				{"decoded_bytes", inbox.DecodedBytes},
				{"stale_rejections", inbox.StaleRejections},
				{"pending_capacity", inbox.PendingCapacity},
				{"held_capacity", inbox.HeldCapacity},
				{"pending_byte_capacity", inbox.PendingByteCapacity},
				{"held_byte_capacity", inbox.HeldByteCapacity},
			};
		}
		const auto pending = [](const std::unique_ptr<ContentSession> &content) {
			return content ? content->Pending.size() + content->Issued.size() : size_t{0};
		};
		frame["pending_content"] = pending(ContentState) +
								   (PortalPrevious ? pending(PortalPrevious->Content) : 0) +
								   (PortalNext ? pending(PortalNext->Content) : 0);
		if (PortalPrevious)
			frame["retained_world"] = std::string(Universe_->NameOf(PortalPrevious->World).Text());
		if (PortalDrawing) {
			const bool successor = PortalNext && PortalDrawing == &PortalNext->View;
			frame["observed_successor"] = successor;
			frame["observed_world"] = successor ? PortalNext->Offer.Claim.Destination
												: std::string(PortalPrevious->Authored.Text());
			frame["observed_tick"] = PortalDrawing->Frame.Tick;
		}
		// Record the accepted camera, not only the current eye, to diagnose delayed-image handoffs.
		const auto captureOf = [&](engine::core::Name key) -> json {
			if (!PortalImages) return nullptr;
			const auto capture = PortalImages->Capture(view.Slot, key);
			if (!capture) return nullptr;
			json lenses = json::array();
			for (const auto &lens : capture->Lenses.Entries)
				lenses.push_back({{"shader", lens.Shader}, {"program_hash", lens.ProgramHash.ToHex()}});
			return {
				{"lens_count", capture->Lenses.Entries.size()},
				{"lens_time", capture->Lenses.TimeSeconds},
				{"lenses", std::move(lenses)},
				{"image", capture->Image},
				{"spatial_overlay_image", capture->SpatialOverlayImage},
				{"producer", capture->Producer.World},
				{"session", capture->Producer.Session},
				{"generation", capture->Producer.Generation},
				{"request", capture->Binding.Expected.RequestId},
				{"capture_tick", capture->CaptureTick},
				{"accepted_age_ms",
				 std::chrono::duration<double, std::milli>(
					 std::chrono::steady_clock::now() - capture->AcceptedAt
				 )
					 .count()},
				{"camera_revision", capture->Binding.Expected.CameraRevision},
				{"seam_revision", capture->Binding.Expected.SeamRevision},
				{"position", capture->Camera.Position},
				{"orientation", capture->Camera.Orientation},
				{"frustum", capture->Camera.Frustum},
				{"clip_plane", capture->Camera.ClipPlane},
				{"projection",
				 capture->Binding.ExpectedProjection == render::PortalImageProjection::Eye ? "eye" : "seam"},
				{"scope",
				 capture->Binding.ExpectedScope == render::PortalImageScope::CompleteWorld ? "complete-world"
				 : capture->Binding.ExpectedScope == render::PortalImageScope::SeamRadiance
					 ? "seam-radiance"
					 : "opaque-lighting"},
				{"width", capture->Width},
				{"height", capture->Height},
			};
		};
		frame["view_slot"] = view.Slot;
		if (view.EyeImage != 0) {
			frame["eye_image_handle"] = view.EyeImage;
			frame["eye_capture"] = captureOf(view.EyeImageKey);
		}
		if (view.EyePlayer) frame["eye_player"] = *view.EyePlayer;
		if (Connection) {
			frame["replication_applied_tick"] = Connection->Applied();
			frame["unconfirmed_inputs"] = Connection->Unconfirmed().size();
			frame["prediction_covered_through"] = Connection->PredictionCoverage();
			frame["input_local_epoch"] = InputLocalEpoch;
			frame["input_sequence_epoch"] = InputSequenceEpoch;
		}
		frame["portal_views"] = json::array();
		for (const auto &portal : view.Portals) {
			const auto demandStatus = [&] {
				switch (portal.ImageDemandStatus) {
				case engine::render::PortalDemandStatus::Ready:
					return "ready";
				case engine::render::PortalDemandStatus::Hidden:
					return "hidden";
				case engine::render::PortalDemandStatus::Invalid:
					return "invalid";
				case engine::render::PortalDemandStatus::Unsupported:
					return "unsupported";
				}
				return "invalid";
			};
			const auto hiddenReason = [&] {
				switch (portal.ImageHiddenReason) {
				case engine::render::PortalDemandHiddenReason::None:
					return "none";
				case engine::render::PortalDemandHiddenReason::Frustum:
					return "frustum";
				case engine::render::PortalDemandHiddenReason::ClipPlane:
					return "clip_plane";
				}
				return "none";
			};
			frame["portal_views"].push_back({
				{"index", portal.Index},
				{"key", std::string(portal.ImagePortal.Text())},
				{"external", portal.ExternalImage},
				{"demand_status", demandStatus()},
				{"hidden_reason", hiddenReason()},
				{"frustum_visible", portal.ImageFrustumVisible},
				{"demand_camera", pose(portal.ImageDemandCamera)},
				{"demand_camera_revision", portal.ImageCameraRevision},
				{"warp",
				 json{
					 {"frame", pose(portal.Warp.Frame)},
					 {"origin", vector(portal.Warp.Origin)},
					 {"scale", portal.Warp.Scale}
				 }},
				{"image", portal.ImportedImage},
				{"capture", portal.ExternalImage ? captureOf(portal.ImagePortal) : json(nullptr)},
				{"centre", vector(portal.Centre)},
				{"normal", vector(portal.Normal)},
				{"first", vector(portal.First)},
				{"second", vector(portal.Second)},
			});
		}
		Universe_->Enter(inputWorld, [&](ecs::Store &store) {
			frame["tick"] = store.Time().Tick;
			frame["alpha"] = store.Time().Alpha;
			const auto *active = store.Resource<scene::ActiveCamera>();
			const auto *control = store.Resource<scene::CameraController>();
			if (control) {
				frame["control_angles"] = json::array({control->Angles.X, control->Angles.Y});
				frame["control_basis"] = pose(control->Basis);
				frame["first_person"] = control->Mode == scene::CameraMode::LockFirstPerson;
				frame["zoom"] = control->Distance;
				frame["head_offset"] = vector(control->Basis.UpVector() * control->HeadHeight);
			}
			if (active) {
				if (const auto *cameraFrame = store.Get<scene::Transform>(active->Entity))
					frame["input_camera"] = pose(cameraFrame->Frame);
				if (const auto *subject = store.Get<scene::CameraSubject>(active->Entity)) {
					frame["subject"] = subject->Target.Id;
					frame["subject_automatic"] = subject->Automatic;
					frame["subject_is_humanoid"] = store.Has<scene::Humanoid>(subject->Target);
				}
				if (const auto *history = store.Get<scene::CameraPortalView>(active->Entity);
					history && history->Started) {
					frame["eye_world"] = history->World;
					frame["eye_from_input"] = pose(history->FromInput.Frame);
					frame["eye_origin"] = vector(history->FromInput.Origin);
					frame["eye_scale"] = history->FromInput.Scale;
					frame["eye_route_hops"] = history->Route.size();
					if (!history->ArrivedFrom.empty()) {
						frame["arrival_from"] = history->ArrivedFrom;
						frame["arrival_point"] = vector(history->ArrivalPoint);
						frame["arrival_normal"] = vector(history->ArrivalNormal);
						frame["arrival_tolerance"] = history->ArrivalTolerance;
					}
				}
			}
			if (const auto *history = store.Resource<PortalInputHistory>()) {
				frame["portal_input_history"] = {
					{"retained", history->Count},
					{"covered_through", history->CoveredThrough},
					{"last_recorded_tick", history->LastRecordedTick},
					{"applied_input_tick", history->AppliedInputTick},
					{"applied_destination_tick", history->AppliedDestinationTick},
					{"discarded", history->DiscardedInputs}
				};
				if (history->AppliedDestinationTick != 0)
					frame["prediction_authority_world"] = history->Claim.Destination;
			}
			if (const auto *native = store.Resource<NativePlayerPrediction>();
				native && native->Incarnation != 0) {
				frame["native_prediction"] = {
					{"incarnation", native->Incarnation},
					{"applied_pose_tick", native->AppliedPoseTick},
					{"applied_input_tick", native->AppliedInputTick},
					{"clock_simulation_seconds", native->Clock.SimulationSeconds},
					{"clock_input_tick", native->Clock.InputTick},
					{"clock_input_lead_seconds", native->Clock.InputLeadSeconds}
				};
				if (native->Sample) {
					frame["native_prediction"]["sample_pose_tick"] = native->Sample->Motion.DestinationTick;
					frame["native_prediction"]["sample_input_tick"] = native->Sample->Motion.InputTick;
					frame["native_prediction"]["sample_simulation_seconds"] =
						native->Sample->Motion.SimulationSeconds;
				}
			}
			const auto *local = store.Resource<scene::LocalPlayer>();
			const auto *rig =
				local ? store.Get<scene::Character>(scene::CharacterOf(store, local->Instance)) : nullptr;
			if (!rig) return;
			frame["player"] = local->Instance.Id;
			if (const auto *identity = store.Get<scene::PlayerIdentity>(local->Instance))
				frame["player_user_id"] = identity->UserId;
			frame["root"] = rig->Root.Id;
			frame["humanoid"] = rig->Humanoid.Id;
			if (const auto *humanoid = store.Get<scene::Humanoid>(rig->Humanoid))
				frame["humanoid_move_direction"] = vector(humanoid->MoveDirection);
			const auto *hold = store.Resource<scene::CameraCharacterHold>();
			const bool retained =
				hold && hold->Active && hold->Root == rig->Root && hold->Player == local->Instance;
			frame["retained_character"] = retained;
			if (const auto *root = store.Get<scene::Transform>(rig->Root))
				frame[retained ? "retained_root" : "authoritative_root"] = pose(root->Frame);
			if (const auto *prediction = store.Resource<LocalPlayerPrediction>();
				prediction && prediction->Active && prediction->Root == rig->Root) {
				frame["predicted_root"] = pose(prediction->Frame);
				if (const auto presented = PresentedPlayerPrediction(store))
					frame["presented_predicted_root"] = pose(*presented);
				frame["prediction_position_correction"] = vector(prediction->PositionCorrection);
				frame["prediction_correction_seconds"] = prediction->CorrectionSeconds;
				frame["prediction_authority_tick"] = prediction->AuthorityTick;
				frame["predicted_velocity"] = vector(prediction->Linear);
				frame["predicted_move_direction"] = vector(prediction->Humanoid.MoveDirection);
			}
		});
		std::string presentedEyeWorld = frame.value("eye_world", std::string(view.WorldName.Text()));
		if (PortalPrevious && PortalDrawing == &PortalPrevious->View) {
			Universe_->Enter(PortalPrevious->World, [&](ecs::Store &store) {
				const auto *active = store.Resource<scene::ActiveCamera>();
				const auto *history = active ? store.Get<scene::CameraPortalView>(active->Entity) : nullptr;
				if (history && history->Started) presentedEyeWorld = history->World;
			});
		}
		frame["presented_eye_world"] = presentedEyeWorld;
		if (PortalNext) {
			const auto &next = *PortalNext;
			json handoff{
				{"attempt", next.Offer.Attempt},
				{"destination", next.Offer.Claim.Destination},
				{"proceed", next.ProceedSent},
				{"proceed_blocker", next.ProceedBlocker ? next.ProceedBlocker : ""},
				{"proceed_eye_image", next.ProceedEyeImage},
				{"proceed_eye_submitted", next.ProceedEyeSubmitted},
				{"proceed_eye_source", next.ProceedEyeSource},
				{"proceed_eye_source_remote", next.ProceedEyeSourceRemote},
				{"proceed_eye_capture", next.ProceedEyeCapture},
				{"proceed_producer_requests", next.ProceedProducerRequests},
				{"proceed_producer_rendered", next.ProceedProducerRendered},
				{"proceed_producer_refused", next.ProceedProducerRefused},
				{"crossed", next.Crossed},
				{"resume", next.ResumeSent},
				{"ready", next.Ready},
				{"scene_admitted", next.Connection && next.Connection->Admitted()},
				{"scene_joined", next.Connection && next.Connection->Joined()},
				{"scene_live", next.Connection && next.Connection->Live()},
				{"scene_rejected", next.Connection && next.Connection->Rejected()},
				{"refused", next.Refused},
				{"failure", next.Failure},
				{"commit", next.CommitSent},
				{"committed", next.Committed},
				{"drawing_player", next.DrawingArrivedPlayer},
				{"promotion_blocker", next.PromotionBlocker ? next.PromotionBlocker : ""},
				{"reconnecting", next.ReconnectAt > core::Clock::Seconds()},
			};
			if (next.CapturedEyeProbe) {
				const auto &probe = *next.CapturedEyeProbe;
				handoff["arrived_eye_probe"] = {
					{"selected_world", probe.SelectedWorld},
					{"eye_world", probe.EyeWorld},
					{"image_key", probe.ImageKey},
					{"bound_world", probe.BoundWorld},
					{"prepared", probe.Prepared},
					{"slot", probe.Slot},
					{"image", probe.Image},
					{"current_image", probe.CurrentImage},
					{"capture_image", probe.CaptureImage},
					{"capture_tick", probe.CaptureTick},
					{"submitted", probe.Submitted},
					{"producer_valid", probe.ProducerValid},
					{"viewport_images", probe.ViewportImages},
					{"viewport_destinations", probe.ViewportDestinations},
				};
			}
			if (next.CapturedReadiness && next.CapturedReadinessDecision) {
				const auto &facts = *next.CapturedReadiness;
				const auto &decision = *next.CapturedReadinessDecision;
				const auto reason = [](PortalImageOnlyReason value) {
					switch (value) {
					case PortalImageOnlyReason::None:
						return "none";
					case PortalImageOnlyReason::OutsideEnterRange:
						return "outside-enter-range";
					case PortalImageOnlyReason::ReplicaBaseline:
						return "replica-baseline";
					case PortalImageOnlyReason::Topology:
						return "topology";
					case PortalImageOnlyReason::Assets:
						return "assets";
					case PortalImageOnlyReason::PoseRange:
						return "pose-range";
					case PortalImageOnlyReason::Capacity:
						return "capacity";
					case PortalImageOnlyReason::StaleCapture:
						return "stale-capture";
					}
					return "unknown";
				};
				handoff["readiness"] = {
					{"reason", reason(decision.ImageOnly)},
					{"live", decision.Live},
					{"distance", facts.Distance},
					{"required_baseline", facts.RequiredBaseline},
					{"replica_baseline", facts.ReplicaBaseline},
					{"baseline_hash_match",
					 !facts.RequiredBaselineHash.IsZero() &&
						 facts.RequiredBaselineHash == facts.ReplicaBaselineHash},
					{"required_topology", facts.RequiredTopologyRevision},
					{"replica_topology", facts.ReplicaTopologyRevision},
					{"required_authority_epoch", facts.RequiredAuthorityEpoch},
					{"replica_authority_epoch", facts.ReplicaAuthorityEpoch},
					{"required_prepare_revision", facts.RequiredPrepareRevision},
					{"replica_prepare_revision", facts.ReplicaPrepareRevision},
					{"clock_domain_match",
					 !facts.RequiredClockDomain.empty() &&
						 facts.RequiredClockDomain == facts.ReplicaClockDomain},
					{"required_source_tick", facts.RequiredSourceTick},
					{"replica_source_tick", facts.ReplicaSourceTick},
					{"required_destination_tick", facts.RequiredDestinationTick},
					{"replica_destination_tick", facts.ReplicaDestinationTick},
					{"required_asset_revision", facts.RequiredAssetRevision},
					{"resident_asset_revision", facts.ResidentAssetRevision},
					{"assets_resident", facts.AssetsResident},
					{"required_pose_begin", facts.RequiredPoseBegin},
					{"required_pose_end", facts.RequiredPoseEnd},
					{"replica_pose_begin", facts.ReplicaPoseBegin},
					{"replica_pose_end", facts.ReplicaPoseEnd},
					{"capacity_reserved", facts.CapacityReserved},
					{"retained_capture", facts.RetainedCapture},
					{"retained_capture_fresh", facts.RetainedCaptureFresh}
				};
			}
			if (next.Motion) {
				handoff["completed_motion"] = {
					{"input_tick", next.Motion->InputTick},
					{"destination_tick", next.Motion->DestinationTick},
					{"destination_incarnation", next.Motion->DestinationIncarnation},
					{"root", pose(next.Motion->Frame)},
					{"linear", vector(next.Motion->Linear)},
					{"angular", vector(next.Motion->Angular)},
					{"grounded", next.Motion->Grounded}
				};
			}
			handoff["through"] = pose(next.Offer.Through.Frame);
			handoff["through_origin"] = vector(next.Offer.Through.Origin);
			handoff["through_scale"] = next.Offer.Through.Scale;
			if (next.Connection) handoff["destination_applied_tick"] = next.Connection->Applied();
			if (next.World.IsValid()) {
				Universe_->Enter(next.World, [&](ecs::Store &store) {
					handoff["destination_tick"] = store.Time().Tick;
					handoff["destination_characters"] = store.CountMatching<scene::Character>();
					const auto *rig = store.Get<scene::Character>(scene::CharacterOf(store, next.Player));
					handoff["accepted_rig"] = rig && store.Has<scene::Humanoid>(rig->Humanoid) &&
											  store.Has<scene::Transform>(rig->Root);
					if (rig) {
						if (const auto *root = store.Get<scene::Transform>(rig->Root))
							handoff["accepted_root"] = pose(root->Frame);
					}
				});
			}
			frame["portal_handoff"] = std::move(handoff);
		}
		const auto stem = Settings.CaptureSequence / std::to_string(FramesDrawn);
		std::ofstream metadata(stem.string() + ".json");
		metadata << frame.dump() << '\n';
		if (!metadata) ENGINE_ERROR("cannot write frame metadata at '{}'", stem.string());
		Renderer.RequestSceneCapture(stem.string() + ".bmp", view.Slot);
	}
}
