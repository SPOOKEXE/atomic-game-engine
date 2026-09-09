#include <engine/core/Clock.hpp>
#include <engine/core/Log.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/CameraContinuation.hpp>
#include <engine/scene/CameraPortalView.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Controls.hpp>
#include <engine/scene/Services.hpp>

#include <client/Client.hpp>
#include <client/Replicated.hpp>
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
		const auto memory = Renderer.MemoryStatistics();
		frame["gpu_memory"] = {
			{"live_bytes", memory.LiveBytes},
			{"texture_bytes", memory.TextureBytes},
			{"textures", memory.Textures},
			{"buffer_bytes", memory.BufferBytes},
			{"released_bytes", memory.ReleasedBytes}
		};
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
				{"camera_revision", capture->Binding.Expected.CameraRevision},
				{"seam_revision", capture->Binding.Expected.SeamRevision},
				{"position", capture->Camera.Position},
				{"orientation", capture->Camera.Orientation},
				{"frustum", capture->Camera.Frustum},
				{"clip_plane", capture->Camera.ClipPlane},
				{"projection",
				 capture->Binding.ExpectedProjection == render::PortalImageProjection::Eye ? "eye" : "seam"},
				{"scope",
				 capture->Binding.ExpectedScope == render::PortalImageScope::CompleteWorld
					 ? "complete-world"
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
			frame["portal_views"].push_back({
				{"index", portal.Index},
				{"key", std::string(portal.ImagePortal.Text())},
				{"external", portal.ExternalImage},
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
		if (PortalNext) {
			const auto &next = *PortalNext;
			json handoff{
				{"attempt", next.Offer.Attempt},
				{"destination", next.Offer.Claim.Destination},
				{"proceed", next.ProceedSent},
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
				{"drawing_player", next.DrawingArrivedPlayer},
				{"reconnecting", next.ReconnectAt > core::Clock::Seconds()},
			};
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
