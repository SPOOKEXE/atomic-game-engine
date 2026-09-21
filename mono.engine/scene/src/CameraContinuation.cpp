#include <engine/core/Profiling.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/CameraContinuation.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/SurfaceCameras.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace engine::scene {
	namespace {
		bool FiniteFrame(const core::CFrame &frame) {
			const auto q = frame.Rotation();
			return std::isfinite(frame.Position.X) && std::isfinite(frame.Position.Y) &&
				   std::isfinite(frame.Position.Z) && std::isfinite(q.x) && std::isfinite(q.y) &&
				   std::isfinite(q.z) && std::isfinite(q.w) && std::abs(glm::dot(q, q) - 1.0f) < 0.001f;
		}

		bool Valid(const CameraContinuation &value) {
			const auto &c = value.Control;
			const float lengths[] = {
				c.Distance,
				c.MinimumDistance,
				c.FirstPersonTolerance,
				c.MaximumDistance,
				c.ZoomStep,
				c.KeyZoomSpeed,
				c.HeadHeight,
				c.ShoulderOffset
			};
			for (const float length : lengths) {
				if (!std::isfinite(length) || length < 0.0f) return false;
			}
			return (!value.SubjectCleared || !value.Automatic) && ValidCameraPortalView(value.PortalView) &&
				   FiniteFrame(value.Frame) && FiniteFrame(c.Basis) &&
				   c.Basis.Position == core::Vector3::Zero && std::isfinite(c.Angles.X) &&
				   std::isfinite(c.Angles.Y) && std::isfinite(c.Sensitivity) && c.Sensitivity >= 0.0f &&
				   std::isfinite(c.OccludedDistance) && c.MinimumDistance <= c.MaximumDistance &&
				   static_cast<unsigned>(c.Mode) <= static_cast<unsigned>(CameraMode::Scriptable) &&
				   std::isfinite(value.Lens.FieldOfViewRadians) && value.Lens.FieldOfViewRadians > 0 &&
				   value.Lens.FieldOfViewRadians < std::numbers::pi_v<float> &&
				   std::isfinite(value.Lens.NearPlane) && value.Lens.NearPlane > 0 &&
				   std::isfinite(value.Lens.FarPlane) && value.Lens.FarPlane > value.Lens.NearPlane;
		}

		void ClearTransit(CameraController &control) {
			control.TransitSubject = ecs::NULL_ENTITY;
			control.SeenTransit = 0;
			control.TransitFrame = {};
			control.TransitScale = 1.0f;
			std::fill(std::begin(control.Reserved), std::end(control.Reserved), uint8_t{0});
		}
	}

	std::optional<CameraContinuation> CaptureCameraContinuation(const ecs::Store &store) {
		const auto *active = store.Resource<ActiveCamera>();
		const auto *control = store.Resource<CameraController>();
		if (active == nullptr || control == nullptr) return std::nullopt;
		const auto *frame = store.Get<Transform>(active->Entity);
		const auto *lens = store.Get<Camera>(active->Entity);
		const auto *subject = store.Get<CameraSubject>(active->Entity);
		if (frame == nullptr || lens == nullptr || subject == nullptr) return std::nullopt;
		CameraContinuation value{*lens, frame->Frame, *control, subject->Automatic, {}};
		value.SubjectCleared = !subject->Automatic && subject->Target == ecs::NULL_ENTITY;
		if (const auto *view = store.Get<CameraPortalView>(active->Entity)) value.PortalView = *view;
		ClearTransit(value.Control);
		return Valid(value) ? std::optional{value} : std::nullopt;
	}

	bool MapCameraContinuation(CameraContinuation &camera, const SeamTransform &through) {
		if (!Valid(camera) || !FiniteFrame(through.Frame) || !std::isfinite(through.Scale) ||
			through.Scale <= 0 || !std::isfinite(through.Origin.X) || !std::isfinite(through.Origin.Y) ||
			!std::isfinite(through.Origin.Z))
			return false;
		CameraContinuation mapped = camera;
		if (!RebaseCameraPortalView(mapped.PortalView, through)) return false;
		mapped.Frame = through.Place(camera.Frame).Orthonormalize();
		auto &c = mapped.Control;
		c.Basis =
			core::CFrame(core::Vector3::Zero, glm::normalize(through.Frame.Rotation() * c.Basis.Rotation()));
		c.Distance *= through.Scale;
		c.MinimumDistance *= through.Scale;
		c.FirstPersonTolerance *= through.Scale;
		c.MaximumDistance *= through.Scale;
		c.ZoomStep *= through.Scale;
		c.KeyZoomSpeed *= through.Scale;
		c.HeadHeight *= through.Scale;
		c.ShoulderOffset *= through.Scale;
		if (c.OccludedDistance >= 0) c.OccludedDistance *= through.Scale;
		mapped.Lens.NearPlane *= through.Scale;
		mapped.Lens.FarPlane *= through.Scale;
		ClearTransit(c);
		if (!Valid(mapped)) return false;
		camera = mapped;
		return true;
	}

	bool ApplyCameraContinuation(
		ecs::Store &store, ecs::Entity camera, ecs::Entity subject, const CameraContinuation &continuation
	) {
		if (!Valid(continuation) || !store.Has<Camera>(camera) || !store.Has<Transform>(camera)) return false;
		if (continuation.SubjectCleared) subject = ecs::NULL_ENTITY;
		ecs::Entity root = subject;
		if (const auto *humanoid = store.Get<Humanoid>(subject)) {
			if (humanoid->RootPart != ecs::NULL_ENTITY || store.IsA(subject, HumanoidClass()))
				root = humanoid->RootPart;
		}
		if (!continuation.SubjectCleared && !store.Has<Transform>(root)) return false;
		CameraController control = continuation.Control;
		ClearTransit(control);
		control.TransitSubject = root;
		if (const auto *transit = store.Get<PortalTransit>(root)) {
			control.SeenTransit = transit->Serial;
			control.TransitFrame = transit->Frame;
			control.TransitScale = transit->Scale;
		}
		store.Set(camera, continuation.Lens);
		store.Set(camera, Transform{continuation.Frame});
		store.Set(camera, CameraSubject{.Target = subject, .Automatic = continuation.Automatic});
		store.Set(camera, continuation.PortalView);
		store.SetResource(control);
		store.SetResource(ActiveCamera{camera});
		return true;
	}
	bool
	PrepareCameraCharacterHold(ecs::Store &store, ecs::Entity player, const core::CFrame &presentedRoot) {
		if (!store.AdoptOnly() || !FiniteFrame(presentedRoot)) return false;
		if (const auto *held = store.Resource<CameraCharacterHold>(); held && held->Active) return true;
		const auto model = CharacterOf(store, player);
		const auto *rig = store.Get<Character>(model);
		const auto *active = store.Resource<ActiveCamera>();
		if (!rig || !active || !store.IsPredicted(active->Entity)) return false;
		const auto *subject = store.Get<CameraSubject>(active->Entity);
		const auto *humanoid = store.Get<scene::Humanoid>(rig->Humanoid);
		const auto *identity = store.Get<PlayerIdentity>(player);
		if (!subject || !humanoid || !identity || !store.Has<Transform>(rig->Root)) return false;
		const auto sourceRig = *rig;
		const auto sourceHumanoid = *humanoid;
		const auto sourceIdentity = *identity;
		const auto sourceSubject = *subject;
		const auto camera = active->Entity;
		auto *held = store.ResourceMutable<CameraCharacterHold>();
		if (held && (held->SourcePlayer != player || held->SourceModel != model ||
					 held->SourceRoot != sourceRig.Root || held->SourceHumanoid != sourceRig.Humanoid)) {
			ReleaseCameraCharacterHold(store);
			held = nullptr;
		}
		if (!held) {
			CameraCharacterHold prepared;
			prepared.SourcePlayer = player;
			prepared.SourceModel = model;
			prepared.SourceRoot = sourceRig.Root;
			prepared.SourceHumanoid = sourceRig.Humanoid;
			prepared.Player =
				store.CreatePredictedInstance(PlayerClass(), store.InstanceNameOf(player).Text());
			prepared.Model = store.CreatePredictedInstance(
				ecs::Classes::Find(core::Name("Model")), store.InstanceNameOf(model).Text()
			);
			prepared.Root =
				store.CreatePredictedInstance(ecs::Classes::Find(core::Name("Part")), "HumanoidRootPart");
			prepared.Humanoid = store.CreatePredictedInstance(HumanoidClass(), "Humanoid");
			store.SetResource(prepared);
			if (!store.Alive(prepared.Player) || !store.Alive(prepared.Model) ||
				!store.Alive(prepared.Root) || !store.Alive(prepared.Humanoid)) {
				ReleaseCameraCharacterHold(store);
				return false;
			}
			store.SetParent(prepared.Root, prepared.Model);
			store.SetParent(prepared.Humanoid, prepared.Model);
			store.Remove<Visual>(prepared.Root);
			store.Remove<Collider>(prepared.Root);
			store.Remove<RigidBody>(prepared.Root);
			store.Remove<Motion>(prepared.Root);
			store.Remove<Simulated>(prepared.Root);
			held = store.ResourceMutable<CameraCharacterHold>();
		}
		held->SourceCamera = camera;
		held->SourceSubject = sourceSubject.Target;
		held->Automatic = sourceSubject.Automatic;
		store.Set(held->Player, sourceIdentity);
		store.Set(held->Player, PlayerCharacter{held->Model});
		store.Set(held->Model, Character{held->Root, held->Humanoid, held->Player});
		store.Set(held->Model, Transform{presentedRoot});
		store.Set(held->Root, Transform{presentedRoot});
		auto retainedHumanoid = sourceHumanoid;
		retainedHumanoid.RootPart = held->Root;
		retainedHumanoid.Enabled = false;
		store.Set(held->Humanoid, retainedHumanoid);
		return true;
	}

	bool ActivateCameraCharacterHold(ecs::Store &store) {
		auto *held = store.ResourceMutable<CameraCharacterHold>();
		if (!held) return false;
		if (held->Active) return true;
		const auto *sourceRig = store.Get<Character>(held->SourceModel);
		if (CharacterOf(store, held->SourcePlayer) == held->SourceModel && sourceRig &&
			sourceRig->Root == held->SourceRoot && sourceRig->Humanoid == held->SourceHumanoid &&
			store.Has<Transform>(held->SourceRoot) && store.Has<scene::Humanoid>(held->SourceHumanoid))
			return false;
		if (!store.Alive(held->Player) || !store.Alive(held->Model) || !store.Has<Transform>(held->Root) ||
			!store.Has<scene::Humanoid>(held->Humanoid))
			return false;
		const auto *local = store.Resource<LocalPlayer>();
		if (!local || local->Instance != held->SourcePlayer) return false;
		store.SetResource(LocalPlayer{held->Player});
		const auto *active = store.Resource<ActiveCamera>();
		if (active && active->Entity == held->SourceCamera &&
			(held->SourceSubject == held->SourceRoot || held->SourceSubject == held->SourceHumanoid)) {
			if (auto *subject = store.GetMutable<CameraSubject>(active->Entity);
				subject && subject->Automatic == held->Automatic &&
				(subject->Target == held->SourceSubject ||
				 (subject->Automatic && subject->Target == ecs::NULL_ENTITY))) {
				subject->Target = held->SourceSubject == held->SourceRoot ? held->Root : held->Humanoid;
			}
		}
		held->Active = true;
		return true;
	}

	bool ContinueCameraBodyPose(
		ecs::Store &store,
		const core::CFrame &rootFrame,
		std::vector<DrawInstance> &rows,
		std::vector<core::CFrame> &joints
	) {
		const auto *held = store.Resource<CameraCharacterHold>();
		if (!held) {
			store.RemoveResource<CameraBodyPose>();
			return true;
		}
		ENGINE_PROFILE("continue camera body pose");
		if (!FiniteFrame(rootFrame)) return false;
		const auto nativeBody = [&](const DrawInstance &row) {
			return row.Rig == held->SourceRoot.Id && row.Variant == 0 && row.Surface < 0 &&
				   (!row.SourceWorld.IsValid() || row.SourceWorld.Text() == store.Name());
		};
		auto *pose = store.ResourceMutable<CameraBodyPose>();
		if (!held->Active) {
			size_t rowCount = 0, jointCount = 0;
			for (const auto &row : rows) {
				if (!nativeBody(row)) continue;
				if (row.SkinFirst > joints.size() || row.SkinCount > joints.size() - row.SkinFirst)
					return false;
				++rowCount;
				jointCount += row.SkinCount;
			}
			// Bound the extra pose residency while admission is pending.
			if (rowCount > 256 || jointCount > 4096) return false;
			if (!pose) {
				store.SetResource(CameraBodyPose{});
				pose = store.ResourceMutable<CameraBodyPose>();
			}
			pose->SourceRoot = held->SourceRoot;
			pose->RootFrame = rootFrame;
			pose->Rows.clear();
			pose->Joints.clear();
			for (auto row : rows) {
				if (!nativeBody(row)) continue;
				const auto first = row.SkinFirst;
				row.SkinFirst = static_cast<uint32_t>(pose->Joints.size());
				pose->Joints.insert(
					pose->Joints.end(), joints.begin() + first, joints.begin() + first + row.SkinCount
				);
				pose->Rows.push_back(row);
			}
			return true;
		}
		if (!pose || pose->SourceRoot != held->SourceRoot || !store.Alive(held->Root)) return false;
		if (joints.size() > UINT32_MAX - pose->Joints.size()) return false;
		const auto through = rootFrame * pose->RootFrame.Inverse();
		std::erase_if(rows, nativeBody);
		const auto firstJoint = static_cast<uint32_t>(joints.size());
		joints.insert(joints.end(), pose->Joints.begin(), pose->Joints.end());
		for (auto row : pose->Rows) {
			row.Frame = through * row.Frame;
			row.SkinFirst += firstJoint;
			rows.push_back(row);
		}
		return true;
	}

	void ReleaseCameraCharacterHold(ecs::Store &store) {
		const auto *resource = store.Resource<CameraCharacterHold>();
		if (!resource) return;
		const auto held = *resource;
		if (auto *local = store.ResourceMutable<LocalPlayer>(); local && local->Instance == held.Player)
			local->Instance = store.Alive(held.SourcePlayer) ? held.SourcePlayer : ecs::NULL_ENTITY;
		if (auto *subject = store.GetMutable<CameraSubject>(held.SourceCamera);
			subject && (subject->Target == held.Root || subject->Target == held.Humanoid))
			subject->Target = store.Alive(held.SourceSubject) ? held.SourceSubject : ecs::NULL_ENTITY;
		store.DestroyInstance(held.Root);
		store.DestroyInstance(held.Humanoid);
		store.DestroyInstance(held.Model);
		store.DestroyInstance(held.Player);
		store.RemoveResource<CameraCharacterHold>();
		store.RemoveResource<CameraBodyPose>();
	}

}
