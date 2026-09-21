#pragma once

// Owned camera state carried between worlds, including the eye's authored world
// name. The caller resolves routing; scene resolves only destination entities.
// @tier L7 · shared

#include <engine/scene/CameraPortalView.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Controls.hpp>
#include <engine/scene/DrawInstance.hpp>

#include <optional>
#include <vector>

namespace engine::scene {
	struct SeamTransform;

	// Camera state copied through a portal until a destination camera resumes it.
	struct CameraContinuation {
		// Authored lens settings copied into the destination camera.
		Camera Lens;
		// Eye pose mapped through the portal seam.
		core::CFrame Frame;
		// TransitSubject is null and the transit baseline is empty in captured values.
		CameraController Control;
		// Whether the destination camera resumes automatic subject following.
		bool Automatic = true;
		// Portal-view state retained across the world transition.
		CameraPortalView PortalView;
		// Explicit null differs from automatic follow awaiting a subject.
		bool SubjectCleared = false;
	};

	// Local presentation identities. These handles stay in their replica store.
	struct CameraCharacterHold {
		// Source player identity retained while replica rows retire.
		ecs::Entity SourcePlayer;
		// Source character model identity.
		ecs::Entity SourceModel;
		// Source character root identity.
		ecs::Entity SourceRoot;
		// Source humanoid identity.
		ecs::Entity SourceHumanoid;
		// Source camera identity.
		ecs::Entity SourceCamera;
		// Source camera-subject identity.
		ecs::Entity SourceSubject;
		// Destination player identity after replication arrives.
		ecs::Entity Player;
		// Destination character model identity.
		ecs::Entity Model;
		// Destination character root identity.
		ecs::Entity Root;
		// Destination humanoid identity.
		ecs::Entity Humanoid;
		// Whether the held camera resumes automatic subject following.
		bool Automatic = true;
		// Whether the held destination identities are currently selected.
		bool Active = false;
		// Explicit padding retained for the local hold record layout.
		uint8_t Reserved[6]{};
	};

	// Last presented pose, owned by the replica while its source rows retire.
	// Local only; snapshot restoration discards this derived geometry.
	struct CameraBodyPose {
		// Source root whose final pose this record preserves.
		ecs::Entity SourceRoot{};
		// Last presented world pose of SourceRoot.
		core::CFrame RootFrame;
		// Owned draw rows from the final presentation frame.
		std::vector<DrawInstance> Rows;
		// Owned joint poses aligned with the copied skin rows.
		std::vector<core::CFrame> Joints;
	};

	// Captures a prepared body's pose, then carries it with the held root after
	// retirement. Call after skin palette collection and before seam clipping.
	// Retains the last limb/skin pose, not an animation player. Invalid or over-budget
	// input preserves the previous pose and leaves the supplied draw lists unchanged.
	bool ContinueCameraBodyPose(
		ecs::Store &store,
		const core::CFrame &rootFrame,
		std::vector<DrawInstance> &rows,
		std::vector<core::CFrame> &joints
	);

	// Prepares four predicted instances before replication can retire the source.
	// Refreshes their pose while the source is intact; no scripts or body geometry are cloned.
	bool PrepareCameraCharacterHold(ecs::Store &store, ecs::Entity player, const core::CFrame &presentedRoot);
	// Switches the local player after source retirement; unrelated camera selections stay unchanged.
	bool ActivateCameraCharacterHold(ecs::Store &store);
	// Releases owned instances and restores original references where still applicable.
	void ReleaseCameraCharacterHold(ecs::Store &store);

	// Captures the active camera and its controls, with no source entity identity.
	std::optional<CameraContinuation> CaptureCameraContinuation(const ecs::Store &store);

	// Maps a copied camera through one finite, positive-scale rigid seam. Invalid
	// inputs leave the value untouched. Lens clipping distances scale with the room.
	bool MapCameraContinuation(CameraContinuation &camera, const SeamTransform &through);

	// Installs onto a live destination camera and subject after their snapshot arrives.
	// An explicitly cleared selection installs without a subject. Other missing roots
	// or invalid state are refused without changing the store.
	bool ApplyCameraContinuation(
		ecs::Store &store, ecs::Entity camera, ecs::Entity subject, const CameraContinuation &continuation
	);
}
