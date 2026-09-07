#include <engine/core/Profiling.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/graph/Cull.hpp>
#include <engine/render/PortalGeometryDraw.hpp>
#include <engine/render/PortalImageDemand.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/CameraContinuation.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/Sunlight.hpp>
#include <engine/world/Postbox.hpp>

#include <algorithm>
#include <bit>
#include <cmath>

namespace engine::render {
	namespace {
		const world::Replica *ReplicaOf(ecs::Store &store) {
			// Plain scene stores need no mailbox types. Avoid registering a fallback
			// name here before world startup installs the replica snapshot codec.
			static const core::Name replicaName("world.Replica");
			if (!ecs::Components::Find(replicaName).IsValid()) return nullptr;
			return store.Resource<world::Replica>();
		}
		bool Finite(const core::Vector3 &value) {
			return std::isfinite(value.X) && std::isfinite(value.Y) && std::isfinite(value.Z);
		}
		bool Rigid(const core::CFrame &frame) {
			const auto rotation = frame.Rotation();
			const float norm = glm::dot(rotation, rotation);
			return Finite(frame.Position) && std::isfinite(norm) && std::abs(norm - 1.0f) <= .001f;
		}
		bool Finite(const glm::mat4 &matrix) {
			for (int column = 0; column < 4; ++column) {
				for (int row = 0; row < 4; ++row) {
					if (!std::isfinite(matrix[column][row])) {
						return false;
					}
				}
			}
			return true;
		}
		void Sign(uint64_t &signature, float value) {
			signature = scene::MixSignature(signature, std::bit_cast<uint32_t>(value == 0 ? 0.0f : value));
		}
		void Sign(uint64_t &signature, const core::Vector3 &value) {
			Sign(signature, value.X);
			Sign(signature, value.Y);
			Sign(signature, value.Z);
		}
		void Sign(uint64_t &signature, const core::CFrame &value) {
			Sign(signature, value.Position);
			const auto rotation = value.Rotation();
			Sign(signature, rotation.x);
			Sign(signature, rotation.y);
			Sign(signature, rotation.z);
			Sign(signature, rotation.w);
		}
	}

	PortalDemandStatus BuildPortalEyeDemand(
		core::Name cameraKey,
		const View &eye,
		const PortalImageDemandSettings &settings,
		PortalImageDemand &demand
	) {
		const auto &camera = eye.Camera;
		if (!cameraKey.IsValid() || cameraKey.Text().size() > 256 ||
			cameraKey.Text().find('\0') != std::string_view::npos || !Rigid(eye.CameraFrame) ||
			settings.Width == 0 || settings.Height == 0 || settings.MaximumExtent == 0 ||
			settings.MaximumExtent > MAX_PORTAL_IMAGE_EXTENT ||
			settings.RecursionDepth > MAX_PORTAL_IMAGE_RECURSION || settings.PixelBudget == 0 ||
			settings.PixelBudget > MAX_PORTAL_IMAGE_PIXELS || !std::isfinite(camera.NearPlane) ||
			camera.NearPlane <= 0 || !std::isfinite(camera.FarPlane) || camera.FarPlane <= camera.NearPlane ||
			!std::isfinite(camera.FieldOfViewRadians) || camera.FieldOfViewRadians <= 0 ||
			camera.FieldOfViewRadians >= 3.14159265358979323846f)
			return PortalDemandStatus::Invalid;
		const auto projection = eye.Projection.value_or(
			scene::ResolveCamera(eye.CameraFrame, camera, float(settings.Width) / settings.Height).Projection
		);
		if (!Finite(projection) || projection[0][0] <= 0 || projection[1][1] <= 0)
			return PortalDemandStatus::Invalid;
		PortalImageRequest result;
		result.Projection = PortalImageProjection::Eye;
		if (eye.EyePlayer) result.EyePlayer = std::to_string(*eye.EyePlayer);
		result.ClipPlane = {};
		result.Key.PortalKey = cameraKey.Text();
		const auto &position = eye.CameraFrame.Position;
		const auto rotation = eye.CameraFrame.Rotation();
		result.Position = {position.X, position.Y, position.Z};
		result.Orientation = {rotation.x, rotation.y, rotation.z, rotation.w};
		const float near = camera.NearPlane;
		result.Frustum = {
			near * (projection[2][0] - 1) / projection[0][0],
			near * (projection[2][0] + 1) / projection[0][0],
			near * (projection[2][1] - 1) / projection[1][1],
			near * (projection[2][1] + 1) / projection[1][1],
			near,
			camera.FarPlane
		};
		scene::SurfaceLens lens;
		lens.Left = result.Frustum[0];
		lens.Right = result.Frustum[1];
		lens.Bottom = result.Frustum[2];
		lens.Top = result.Frustum[3];
		lens.NearPlane = near;
		lens.FarPlane = camera.FarPlane;
		const auto reconstructed = scene::SurfaceProjection(lens, eye.CameraFrame);
		if (!Finite(reconstructed)) return PortalDemandStatus::Invalid;
		for (int column = 0; column < 4; ++column)
			for (int row = 0; row < 4; ++row)
				if (std::abs(reconstructed[column][row] - projection[column][row]) > .00001f)
					return PortalDemandStatus::Unsupported;
		const uint32_t longest = std::max(settings.Width, settings.Height);
		const uint32_t extent = std::min(settings.MaximumExtent, longest);
		result.Width = std::max(1u, uint32_t(uint64_t(settings.Width) * extent / longest));
		result.Height = std::max(1u, uint32_t(uint64_t(settings.Height) * extent / longest));
		if (uint64_t(result.Width) * result.Height > settings.PixelBudget) return PortalDemandStatus::Invalid;
		result.PixelBudget = settings.PixelBudget;
		result.RecursionDepth = settings.RecursionDepth;
		Sign(result.Key.CameraRevision, eye.CameraFrame);
		for (const unsigned char byte : result.EyePlayer)
			result.Key.CameraRevision = scene::MixSignature(result.Key.CameraRevision, byte);
		for (float value : result.Frustum)
			Sign(result.Key.CameraRevision, value);
		for (uint32_t value : {result.Width, result.Height, result.PixelBudget, result.RecursionDepth})
			result.Key.CameraRevision = scene::MixSignature(result.Key.CameraRevision, value);
		PortalImageDemand built;
		built.Request = std::move(result);
		built.Binding.World = eye.World;
		built.Binding.WorldName = eye.WorldName;
		built.Binding.ViewSlot = eye.Slot;
		built.Binding.Portal = cameraKey;
		built.Binding.Sampling = scene::ResolveSurfaceCamera(eye.CameraFrame, reconstructed).ViewProjection;
		if (!Finite(built.Binding.Sampling)) return PortalDemandStatus::Invalid;
		demand = std::move(built);
		return PortalDemandStatus::Ready;
	}

	bool CollectPortalEyeGeometry(
		ecs::Store &store,
		core::Name destination,
		std::span<const scene::DrawInstance> instances,
		std::span<const core::CFrame> joints,
		std::vector<std::byte> &out,
		std::string &error
	) {
		ENGINE_PROFILE_CAT("collect eye geometry", core::ProfileCategory::Render);
		if (!destination.IsValid()) {
			error = "invalid whole-eye geometry destination";
			return false;
		}
		const auto *replica = ReplicaOf(store);
		const auto ownWorld =
			replica && replica->Active && replica->Of.IsValid() ? replica->Of.Text() : store.Name();
		if (instances.empty()) {
			out.clear();
			error.clear();
			return true;
		}
		if (destination.Text() == ownWorld) {
			const auto *held = store.Resource<scene::CameraCharacterHold>();
			ecs::Entity root = held ? held->SourceRoot : ecs::NULL_ENTITY;
			if (!held) {
				const auto *local = store.Resource<scene::LocalPlayer>();
				const auto *character =
					local ? store.Get<scene::Character>(scene::CharacterOf(store, local->Instance)) : nullptr;
				if (character && store.Alive(character->Root)) root = character->Root;
			}
			std::vector<scene::DrawInstance> body;
			if (root != ecs::NULL_ENTITY) {
				for (const auto &row : instances) {
					if (row.Rig == root.Id && row.Variant == 0 && row.Surface < 0 &&
						(!row.SourceWorld.IsValid() || row.SourceWorld.Text() == store.Name())) {
						if (body.size() == MAX_PORTAL_GEOMETRY_ROWS) {
							error = "retained eye body exceeds row budget";
							return false;
						}
						body.push_back(row);
					}
				}
			}
			if (!body.empty()) {
				const auto light = scene::SunOf(store).Direction;
				for (auto &row : body)
					row.SeamLight = light;
				return EncodePortalDraws(store, body, joints, out, error);
			}
			out.clear();
			error.clear();
			return true;
		}
		std::vector<scene::PortalSeam> seams;
		scene::GatherPortalSeams(store, seams);
		std::erase_if(seams, [&](const auto &seam) {
			return !seam.Crosses || seam.DestinationWorld != destination;
		});
		if (seams.empty()) {
			out.clear();
			error.clear();
			return true;
		}
		const auto native = [&](const scene::DrawInstance &row) {
			return row.Variant == 0 && (!row.SourceWorld.IsValid() || row.SourceWorld.Text() == store.Name());
		};
		std::vector<scene::DrawInstance> owned;
		const auto firstForeign = std::find_if_not(instances.begin(), instances.end(), native);
		// Composed views normally append foreign and synthetic rows after the
		// native prefix. Borrow that prefix instead of copying the whole room.
		if (std::find_if(firstForeign, instances.end(), native) == instances.end())
			instances = instances.first(static_cast<size_t>(firstForeign - instances.begin()));
		else {
			for (const auto &row : instances)
				if (native(row)) owned.push_back(row);
			instances = owned;
		}
		std::vector<scene::DrawInstance> clones;
		for (const auto &seam : seams) {
			const size_t before = clones.size();
			scene::AppendPortalClones(store, seam, instances, clones);
			if (clones.size() > MAX_PORTAL_GEOMETRY_ROWS) {
				error = "whole-eye crossing geometry exceeds row budget";
				return false;
			}
			for (size_t index = before; index < clones.size(); ++index) {
				const auto &row = clones[index];
				const uint64_t owner = row.Rig != 0 ? row.Rig : row.Source;
				for (size_t previous = 0; owner != 0 && previous < before; ++previous) {
					const auto &other = clones[previous];
					if (owner == (other.Rig != 0 ? other.Rig : other.Source)) {
						error = "whole-eye body crosses multiple destination mouths";
						return false;
					}
				}
			}
		}
		if (clones.empty()) {
			out.clear();
			error.clear();
			return true;
		}
		return EncodePortalDraws(store, clones, joints, out, error);
	}

	PortalImageDemandCounts CollectPortalImageDemands(
		ecs::Store &store,
		const View &viewer,
		const PortalImageDemandSettings &settings,
		std::vector<PortalImageDemand> &demands,
		std::vector<PortalView> &portals,
		std::span<const scene::SurfaceSlot> slots
	) {
		demands.clear();
		std::string bodyPlayer;
		if (settings.ComposePlayerBody) {
			const auto *local = store.Resource<scene::LocalPlayer>();
			const auto *held = store.Resource<scene::CameraCharacterHold>();
			const auto player = held && held->Active ? held->Player
								: local				 ? local->Instance
													 : ecs::NULL_ENTITY;
			const auto *identity = store.Get<scene::PlayerIdentity>(player);
			if (identity) bodyPlayer = std::to_string(identity->UserId);
		}
		auto fitted = settings;
		if (!bodyPlayer.empty()) {
			fitted.MaximumExtent = std::min(fitted.MaximumExtent, 256u);
			fitted.RecursionDepth = 0;
			fitted.PixelBudget /= 4;
		}
		PortalImageDemandCounts counts;
		std::vector<scene::PortalSeam> seams;
		scene::GatherPortalSeams(store, seams);
		std::vector<std::string> names;
		names.reserve(seams.size());
		for (auto &seam : seams) {
			names.push_back(seam.Crosses ? store.GetFullName(seam.Camera) : std::string{});
			if (!slots.empty()) {
				seam.Surface = -1;
				for (const auto &slot : slots) {
					if (slot.Camera == seam.Camera) {
						seam.Surface = slot.Index;
						break;
					}
				}
			}
		}
		for (size_t index = 0; index < seams.size(); ++index) {
			const auto &seam = seams[index];
			if (!seam.Crosses) {
				continue;
			}
			if (seam.Surface < 0 || size_t(seam.Surface) >= scene::MAX_SURFACES) {
				counts.Invalid++;
				continue;
			}
			PortalView claim;
			claim.Index = seam.Surface;
			claim.ExternalImage = true;
			claim.Centre = seam.Centre;
			claim.Normal = seam.Normal;
			claim.First = seam.First;
			claim.Second = seam.Second;
			const auto &name = names[index];
			if (name.empty() || name.size() > 256 || name.find('\0') != std::string::npos ||
				std::count(names.begin(), names.end(), name) != 1) {
				counts.Invalid++;
				portals.push_back(claim);
				continue;
			}
			claim.ImagePortal = core::Name(name);
			PortalImageDemand demand;
			switch (BuildPortalImageDemand(seam, claim.ImagePortal, viewer, viewer.Slot, fitted, demand)) {
			case PortalDemandStatus::Ready: {
				if (!bodyPlayer.empty()) {
					demand.Request.OrderedLayers = true;
					demand.Request.Scope = PortalImageScope::OpaqueLighting;
					demand.Request.PixelBudget = settings.PixelBudget;
					demand.Request.EyePlayer = bodyPlayer;
					demand.Binding.ExpectedScope = PortalImageScope::OpaqueLighting;
				}
				if (const auto *replica = ReplicaOf(store);
					replica && replica->Active && replica->Of.IsValid()) {
					demand.Request.Entrance->SourceWorld = replica->Of.Text();
				}
				std::vector<scene::DrawInstance> clones;
				scene::AppendPortalClones(store, seam, viewer.Instances, clones);
				std::string error;
				if (!clones.empty() &&
					!EncodePortalDraws(store, clones, viewer.JointFrames, demand.Request.Geometry, error)) {
					counts.Invalid++;
					break;
				}
				claim = demand.Portal;
				demands.push_back(std::move(demand));
				counts.Ready++;
				break;
			}
			case PortalDemandStatus::Hidden:
				counts.Hidden++;
				break;
			case PortalDemandStatus::Invalid:
				counts.Invalid++;
				break;
			case PortalDemandStatus::Unsupported:
				counts.Unsupported++;
				break;
			}
			portals.push_back(claim);
		}
		return counts;
	}

	PortalDemandStatus BuildPortalImageDemand(
		const scene::PortalSeam &seam,
		core::Name portalKey,
		const View &viewer,
		size_t viewSlot,
		const PortalImageDemandSettings &settings,
		PortalImageDemand &demand
	) {
		if (!seam.Crosses || !seam.DestinationWorld.IsValid() || !portalKey.IsValid() ||
			portalKey.Text().size() > 256 || !viewer.WorldName.IsValid() || seam.Surface < 0 ||
			size_t(seam.Surface) >= scene::MAX_SURFACES || settings.Width == 0 || settings.Height == 0 ||
			settings.MaximumExtent == 0 || settings.MaximumExtent > MAX_PORTAL_IMAGE_EXTENT ||
			settings.RecursionDepth > MAX_PORTAL_IMAGE_RECURSION || settings.PixelBudget == 0 ||
			settings.PixelBudget > MAX_PORTAL_IMAGE_PIXELS || !std::isfinite(seam.Scale) || seam.Scale <= 0 ||
			!Rigid(viewer.CameraFrame) || !Rigid(seam.Destination) || !Finite(seam.Centre) ||
			!Finite(seam.Normal) || !Finite(seam.First) || !Finite(seam.Second) || !Finite(seam.Up) ||
			std::abs(seam.Normal.Dot(seam.Normal) - 1.0f) > 0.001f) {
			return PortalDemandStatus::Invalid;
		}
		const float firstLength = seam.First.Magnitude();
		const float secondLength = seam.Second.Magnitude();
		if (!std::isfinite(firstLength) || !std::isfinite(secondLength) || firstLength <= 0 ||
			secondLength <= 0) {
			return PortalDemandStatus::Invalid;
		}
		const auto firstDirection = seam.First / firstLength;
		const auto secondDirection = seam.Second / secondLength;
		if (std::abs(firstDirection.Dot(secondDirection)) > .001f ||
			std::abs(firstDirection.Dot(seam.Normal)) > .001f ||
			std::abs(secondDirection.Dot(seam.Normal)) > .001f) {
			return PortalDemandStatus::Invalid;
		}
		if (seam.TagFilter != 0) {
			return PortalDemandStatus::Unsupported;
		}
		auto camera = viewer.Camera;
		camera.NearPlane = scene::PortalNearPlane(
			camera.NearPlane,
			scene::RectangleDistance(seam.Centre, seam.First, seam.Second, viewer.CameraFrame.Position)
		);
		const float aspect = float(settings.Width) / float(settings.Height);
		const auto projection =
			viewer.Projection.value_or(scene::ResolveCamera(viewer.CameraFrame, camera, aspect).Projection);
		if (!Finite(projection) || projection[0][0] <= 0 || projection[1][1] <= 0 ||
			!std::isfinite(camera.NearPlane) || camera.NearPlane <= 0 || !std::isfinite(camera.FarPlane) ||
			camera.FarPlane <= camera.NearPlane) {
			return PortalDemandStatus::Invalid;
		}
		// The wire lens is perspective. Refuse orthographic/skewed inputs instead
		// of quietly replacing their rays with a symmetric perspective camera.
		if (projection[2][3] != -1 || projection[3][3] != 0 || projection[0][1] != 0 ||
			projection[1][0] != 0 || projection[3][0] != 0 || projection[3][1] != 0) {
			return PortalDemandStatus::Unsupported;
		}
		const auto sourceMatrices = scene::ResolveSurfaceCamera(viewer.CameraFrame, projection);
		if (!graph::VisiblePane(sourceMatrices.ViewProjection, seam.Centre, seam.First, seam.Second)) {
			return PortalDemandStatus::Hidden;
		}
		const auto through = scene::SeamMapping(seam);
		const auto frame = through.Place(viewer.CameraFrame);
		const float side = scene::SeamOffset(seam, viewer.CameraFrame.Position);
		const auto normal = through.Rotate(seam.Normal) * (side >= 0 ? -1.0f : 1.0f);
		const auto point =
			through.Point(seam.Centre) - normal * scene::PortalClipBias(through.Length(std::abs(side)));
		const float clipDistance = normal.Dot(point);
		if (!std::isfinite(side) || normal.Dot(frame.Position) - clipDistance >= -1e-4f) {
			return PortalDemandStatus::Hidden;
		}
		PortalImageDemand result;
		result.DestinationWorld = seam.DestinationWorld;
		result.Portal.Index = seam.Surface;
		result.Portal.ExternalImage = true;
		result.Portal.ImagePortal = portalKey;
		result.Portal.Centre = seam.Centre;
		result.Portal.Normal = seam.Normal;
		result.Portal.First = seam.First;
		result.Portal.Second = seam.Second;
		result.Portal.Warp = through;
		auto &request = result.Request;
		const auto centre = through.Point(seam.Centre);
		const auto first = through.Rotate(seam.First) * through.Scale;
		const auto second = through.Rotate(seam.Second) * through.Scale;
		request.Entrance = PortalImageEntrance{
			std::string(viewer.WorldName.Text()),
			{centre.X, centre.Y, centre.Z},
			{first.X, first.Y, first.Z},
			{second.X, second.Y, second.Z}
		};
		request.Key.PortalKey = portalKey.Text();
		request.Position = {frame.Position.X, frame.Position.Y, frame.Position.Z};
		const auto rotation = frame.Rotation();
		request.Orientation = {rotation.x, rotation.y, rotation.z, rotation.w};
		const float near = camera.NearPlane;
		request.Frustum = {
			near * (projection[2][0] - 1) / projection[0][0],
			near * (projection[2][0] + 1) / projection[0][0],
			near * (projection[2][1] - 1) / projection[1][1],
			near * (projection[2][1] + 1) / projection[1][1],
			near,
			camera.FarPlane
		};
		request.ClipPlane = {normal.X, normal.Y, normal.Z, -clipDistance};
		const uint32_t longest = std::max(settings.Width, settings.Height);
		const uint32_t extent = std::min(settings.MaximumExtent, longest);
		request.Width = std::max(1u, uint32_t(uint64_t(settings.Width) * extent / longest));
		request.Height = std::max(1u, uint32_t(uint64_t(settings.Height) * extent / longest));
		if (uint64_t(request.Width) * request.Height > settings.PixelBudget) {
			return PortalDemandStatus::Invalid;
		}
		request.RecursionDepth = settings.RecursionDepth;
		request.PixelBudget = settings.PixelBudget;
		uint64_t &cameraSignature = request.Key.CameraRevision;
		Sign(cameraSignature, viewer.CameraFrame);
		for (const auto value : request.Frustum) {
			Sign(cameraSignature, value);
		}
		for (const auto value :
			 {request.Width, request.Height, request.RecursionDepth, request.PixelBudget}) {
			cameraSignature = scene::MixSignature(cameraSignature, value);
		}
		uint64_t &seamSignature = request.Key.SeamRevision;
		Sign(seamSignature, seam.Centre);
		Sign(seamSignature, seam.Normal);
		Sign(seamSignature, seam.First);
		Sign(seamSignature, seam.Second);
		Sign(seamSignature, through.Frame);
		Sign(seamSignature, through.Origin);
		Sign(seamSignature, through.Scale);
		for (const auto value : seam.DestinationWorld.Text()) {
			seamSignature = scene::MixSignature(seamSignature, uint8_t(value));
		}
		result.Binding.World = viewer.World;
		result.Binding.WorldName = viewer.WorldName;
		result.Binding.ViewSlot = viewSlot;
		result.Binding.Portal = portalKey;
		result.Binding.Index = seam.Surface;
		const auto destinationProjection = scene::ObliqueProjection(projection, frame, normal, clipDistance);
		glm::mat4 mapping = through.Frame.ToMatrix();
		const auto translation = through.Point({});
		for (int column = 0; column < 3; ++column) {
			mapping[column] *= through.Scale;
		}
		mapping[3] = {translation.X, translation.Y, translation.Z, 1};
		result.Binding.Sampling =
			scene::ResolveSurfaceCamera(frame, destinationProjection).ViewProjection * mapping;
		if (!Finite(result.Binding.Sampling)) {
			return PortalDemandStatus::Invalid;
		}
		demand = std::move(result);
		return PortalDemandStatus::Ready;
	}
}
