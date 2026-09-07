#include "PortalPlayerIdentity.hpp"

#include <engine/core/Profiling.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/render/PortalGeometryDraw.hpp>
#include <engine/scene/CameraContinuation.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/SurfaceCameras.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <functional>
#include <limits>

namespace engine::render {
	namespace {
		constexpr std::array<std::string_view, 4> ALPHA{"overlay", "transparency", "tint-mask", "opaque"};
		constexpr std::array<std::string_view, 2> RESAMPLE{"default", "pixelated"};
		PortalGeometryPose Pose(const core::CFrame &frame) {
			const auto rotation = frame.Rotation();
			return {
				frame.Position.X,
				frame.Position.Y,
				frame.Position.Z,
				rotation.x,
				rotation.y,
				rotation.z,
				rotation.w
			};
		}
		core::CFrame Pose(const PortalGeometryPose &pose) {
			return {core::Vector3{pose[0], pose[1], pose[2]}, glm::quat(pose[6], pose[3], pose[4], pose[5])};
		}
		std::array<float, 3> Vector(const core::Vector3 &value) {
			return {value.X, value.Y, value.Z};
		}
		std::array<float, 3> Colour(const core::Color3 &value) {
			return {value.R, value.G, value.B};
		}
	}
	bool SplitPortalBodyDraws(
		std::span<const scene::DrawInstance> body,
		std::span<const core::CFrame> joints,
		const scene::SeamTransform &through,
		const core::Vector3 &normal,
		float offset,
		PortalBodyDraws &out
	) {
		ENGINE_PROFILE("split current portal body");
		const auto overlaps = [](auto input, const auto &storage) {
			return !input.empty() && !storage.empty() &&
				   std::less{}(input.data(), storage.data() + storage.size()) &&
				   std::less{}(storage.data(), input.data() + input.size());
		};
		if (overlaps(body, out.Near) || overlaps(body, out.Far) || overlaps(joints, out.Joints)) return false;
		const auto finite = [](const core::Vector3 &v) {
			return std::isfinite(v.X) && std::isfinite(v.Y) && std::isfinite(v.Z);
		};
		const auto rigid = [&](const core::CFrame &frame) {
			const auto rotation = frame.Rotation();
			const float norm = glm::dot(rotation, rotation);
			return finite(frame.Position) && std::isfinite(norm) && std::abs(norm - 1) < .001f;
		};
		if (body.size() > MAX_PORTAL_GEOMETRY_ROWS || !finite(normal) ||
			std::abs(normal.Dot(normal) - 1) > .0001f || !std::isfinite(offset) || !rigid(through.Frame) ||
			!finite(through.Origin) || !std::isfinite(through.Scale) || through.Scale <= 0)
			return false;
		const auto farNormal = through.Rotate(-normal);
		const float farOffset = farNormal.Dot(through.Point(normal * offset));
		if (!std::isfinite(farOffset)) return false;
		struct PaletteRange {
			uint32_t First = 0, Count = 0, Compact = 0;
		};
		std::array<PaletteRange, MAX_PORTAL_GEOMETRY_ROWS> ranges{};
		std::array<uint32_t, MAX_PORTAL_GEOMETRY_ROWS> compact{};
		size_t rangeCount = 0, jointCount = 0;
		for (size_t i = 0; i < body.size(); ++i) {
			const auto &row = body[i];
			const auto mappedHalf = row.HalfExtent * through.Scale;
			if (row.Surface >= 0 || row.Variant != 0 || !rigid(row.Frame) || !finite(row.HalfExtent) ||
				row.HalfExtent.X <= 0 || row.HalfExtent.Y <= 0 || row.HalfExtent.Z <= 0 ||
				!finite(mappedHalf) || mappedHalf.X <= 0 || mappedHalf.Y <= 0 || mappedHalf.Z <= 0 ||
				!rigid(through.Place(row.Frame)) || !finite(through.Rotate(row.SeamLight)) ||
				!finite(row.SeamNormal) || !std::isfinite(row.SeamOffset) ||
				(row.SeamNormal != core::Vector3{} && ((row.SeamNormal - normal).Magnitude() > .0001f ||
													   std::abs(row.SeamOffset - offset) > .0001f)) ||
				row.SkinFirst > joints.size() || row.SkinCount > joints.size() - row.SkinFirst)
				return false;
			if (row.SkinCount == 0) continue;
			size_t range = 0;
			while (range < rangeCount &&
				   (ranges[range].First != row.SkinFirst || ranges[range].Count != row.SkinCount))
				++range;
			if (range == rangeCount) {
				if (row.SkinCount > MAX_PORTAL_GEOMETRY_JOINTS - jointCount) return false;
				for (const auto &joint : joints.subspan(row.SkinFirst, row.SkinCount))
					if (!rigid(joint)) return false;
				ranges[rangeCount++] = {row.SkinFirst, row.SkinCount, uint32_t(jointCount)};
				jointCount += row.SkinCount;
			}
			compact[i] = ranges[range].Compact;
		}
		out.Near.resize(body.size());
		out.Far.resize(body.size());
		out.Joints.resize(jointCount);
		for (size_t range = 0; range < rangeCount; ++range) {
			const auto &palette = ranges[range];
			std::copy_n(joints.begin() + palette.First, palette.Count, out.Joints.begin() + palette.Compact);
		}
		for (size_t i = 0; i < body.size(); ++i) {
			auto &near = out.Near[i];
			near = body[i];
			near.SkinFirst = compact[i];
			near.SeamNormal = normal;
			near.SeamOffset = offset;
			auto &far = out.Far[i];
			far = near;
			far.Frame = through.Place(near.Frame);
			far.HalfExtent = near.HalfExtent * through.Scale;
			far.SeamNormal = farNormal;
			far.SeamOffset = farOffset;
			far.SeamLight = through.Rotate(near.SeamLight);
			far.TagMask = 0;
		}
		return true;
	}

	bool ForwardPortalDraws(
		std::span<const std::byte> incoming,
		const scene::PortalSeam &seam,
		std::vector<std::byte> &child,
		std::string &error
	) {
		if (incoming.empty()) return true;
		ENGINE_PROFILE("forward portal draw geometry");
		const auto overBudget = [&] {
			error = "forwarded portal geometry exceeds budget";
			return false;
		};
		PortalGeometry source, destination;
		if (!DecodePortalGeometry(incoming, source, error) ||
			(!child.empty() && !DecodePortalGeometry(child, destination, error)))
			return false;
		const auto through = scene::SeamMapping(seam);
		PortalGeometry forwarded;
		for (auto row : source.Rows) {
			const auto frame = Pose(row.Pose);
			const core::Vector3 half{row.HalfExtent[0], row.HalfExtent[1], row.HalfExtent[2]};
			const core::Vector3 clipped{row.SeamPlane[0], row.SeamPlane[1], row.SeamPlane[2]};
			auto cut = scene::CutOfSeam(seam, through, frame, half);
			if (clipped.Magnitude() > 0) {
				// A cut already accepted by the source also carries a body that has
				// fully cleared the mouth. A different mouth cannot borrow that cut.
				if (std::abs(clipped.Dot(seam.Normal)) < .999f ||
					std::abs(row.SeamPlane[3] - seam.Centre.Dot(clipped)) > .001f)
					continue;
				cut.FarNormal = through.Rotate(-clipped);
				cut.FarOffset = through.Point(seam.Centre).Dot(cut.FarNormal);
			} else if (!cut.Fits || !scene::SeamStraddled(seam, frame.Position, half.Magnitude())) {
				continue;
			}
			row.Pose = Pose(through.Place(frame));
			row.HalfExtent = Vector(half * through.Scale);
			row.SeamPlane = {cut.FarNormal.X, cut.FarNormal.Y, cut.FarNormal.Z, cut.FarOffset};
			row.SeamLight =
				Vector(through.Rotate(core::Vector3{row.SeamLight[0], row.SeamLight[1], row.SeamLight[2]}));
			if (row.JointCount > MAX_PORTAL_GEOMETRY_JOINTS - forwarded.Joints.size()) return overBudget();
			const auto first = row.FirstJoint;
			row.FirstJoint = static_cast<uint32_t>(forwarded.Joints.size());
			forwarded.Joints.insert(
				forwarded.Joints.end(),
				source.Joints.begin() + first,
				source.Joints.begin() + first + row.JointCount
			);
			forwarded.Rows.push_back(std::move(row));
		}
		if (forwarded.Rows.empty()) return true;
		PortalGeometry merged;
		for (auto row : destination.Rows) {
			if (!row.Player.empty() &&
				std::any_of(forwarded.Rows.begin(), forwarded.Rows.end(), [&](const auto &copy) {
					return copy.Player == row.Player;
				}))
				continue;
			if (merged.Rows.size() >= MAX_PORTAL_GEOMETRY_ROWS - forwarded.Rows.size() ||
				row.JointCount > MAX_PORTAL_GEOMETRY_JOINTS - forwarded.Joints.size() - merged.Joints.size())
				return overBudget();
			const auto first = row.FirstJoint;
			row.FirstJoint = static_cast<uint32_t>(merged.Joints.size());
			merged.Joints.insert(
				merged.Joints.end(),
				destination.Joints.begin() + first,
				destination.Joints.begin() + first + row.JointCount
			);
			merged.Rows.push_back(std::move(row));
		}
		const auto first = static_cast<uint32_t>(merged.Joints.size());
		merged.Joints.insert(merged.Joints.end(), forwarded.Joints.begin(), forwarded.Joints.end());
		for (auto &row : forwarded.Rows) {
			row.FirstJoint += first;
			merged.Rows.push_back(std::move(row));
		}
		return EncodePortalGeometry(merged, child, error);
	}
	bool EncodePortalDraws(
		ecs::Store &source,
		std::span<const scene::DrawInstance> rows,
		std::span<const core::CFrame> joints,
		std::vector<std::byte> &out,
		std::string &error
	) {
		error = "invalid source portal geometry";
		if (rows.size() > MAX_PORTAL_GEOMETRY_ROWS) {
			return false;
		}
		PortalGeometry geometry;
		for (const auto &draw : rows) {
			if (draw.SkinFirst > joints.size() || draw.SkinCount > joints.size() - draw.SkinFirst ||
				draw.SkinCount > MAX_PORTAL_GEOMETRY_JOINTS - geometry.Joints.size() ||
				static_cast<size_t>(draw.Alpha) >= ALPHA.size() ||
				static_cast<size_t>(draw.Resample) >= RESAMPLE.size()) {
				return false;
			}
			PortalGeometryRow row;
			if (!draw.SourceWorld.IsValid() || draw.SourceWorld.Text() == source.Name()) {
				if (source.Alive(ecs::Entity(draw.Source)))
					row.Name = source.GetFullName(ecs::Entity(draw.Source));
				const ecs::Entity root(draw.Rig);
				auto player = scene::PlayerOf(source, source.ParentOf(root));
				if (const auto *held = source.Resource<scene::CameraCharacterHold>();
					held && held->Active && root != ecs::NULL_ENTITY && root == held->SourceRoot)
					player = held->Player;
				if (const auto *identity = source.Get<scene::PlayerIdentity>(player))
					row.Player = std::to_string(identity->UserId);
			}
			const std::array names{
				draw.Mesh,
				draw.Texture,
				draw.NormalMap,
				draw.RoughnessMap,
				draw.OcclusionMap,
				draw.HeightMap,
				draw.MetalnessMap,
				draw.EmissiveMap,
				draw.Shader
			};
			for (size_t index = 0; index < names.size(); ++index) {
				row.Assets[index] = names[index].Text();
			}
			row.Pose = Pose(draw.Frame);
			row.HalfExtent = Vector(draw.HalfExtent);
			row.Tint = Colour(draw.Tint);
			row.SurfaceColour = Colour(draw.SurfaceColour);
			row.EmissiveTint = Colour(draw.EmissiveTint);
			row.EmissiveStrength = draw.EmissiveStrength;
			row.Transparency = draw.Transparency;
			row.AlphaCutoff = draw.AlphaCutoff;
			row.SeamPlane = {draw.SeamNormal.X, draw.SeamNormal.Y, draw.SeamNormal.Z, draw.SeamOffset};
			row.SeamLight = Vector(draw.SeamLight);
			row.Alpha = ALPHA[static_cast<size_t>(draw.Alpha)];
			row.Resample = RESAMPLE[static_cast<size_t>(draw.Resample)];
			row.CastShadow = draw.CastShadow;
			if (draw.SkinCount != 0) {
				row.FirstJoint = static_cast<uint32_t>(geometry.Joints.size());
				row.JointCount = draw.SkinCount;
				for (const auto &joint : joints.subspan(draw.SkinFirst, draw.SkinCount)) {
					geometry.Joints.push_back(Pose(joint));
				}
			}
			geometry.Rows.push_back(std::move(row));
		}
		return EncodePortalGeometry(geometry, out, error);
	}
	bool AppendPortalDraws(
		std::span<const std::byte> bytes,
		core::Name sourceWorld,
		std::vector<scene::DrawInstance> &rows,
		std::vector<core::CFrame> &joints,
		std::string &error,
		PortalDrawSelection *selection,
		const ecs::Store *destination
	) {
		PortalGeometry geometry;
		if (!DecodePortalGeometry(bytes, geometry, error)) {
			return false;
		}
		if (!sourceWorld.IsValid() || rows.size() > UINT32_MAX - geometry.Rows.size() ||
			(selection && !ValidPortalPlayerIdentity(selection->Player)) ||
			joints.size() > std::numeric_limits<uint32_t>::max() - geometry.Joints.size()) {
			error = "invalid portal draw destination";
			return false;
		}
		const auto previousRows = rows.size();
		if (destination) {
			std::vector<int64_t> players;
			for (const auto &row : geometry.Rows) {
				if (row.Player.empty()) continue;
				int64_t player = 0;
				std::from_chars(row.Player.data(), row.Player.data() + row.Player.size(), player);
				players.push_back(player);
			}
			std::sort(players.begin(), players.end());
			players.erase(std::unique(players.begin(), players.end()), players.end());
			const auto *held = destination->Resource<scene::CameraCharacterHold>();
			if (!players.empty())
				std::erase_if(rows, [&](const auto &row) {
					if (row.Rig == 0 || row.Variant != 0 ||
						(row.SourceWorld.IsValid() && row.SourceWorld.Text() != destination->Name()))
						return false;
					auto player = scene::PlayerOf(*destination, destination->ParentOf(ecs::Entity(row.Rig)));
					if (held && held->Active && row.Rig == held->SourceRoot.Id) player = held->Player;
					const auto *identity = destination->Get<scene::PlayerIdentity>(player);
					return identity && std::binary_search(players.begin(), players.end(), identity->UserId);
				});
		}
		const auto first = static_cast<uint32_t>(joints.size());
		if (selection) {
			selection->Hidden.clear();
			selection->Appended = geometry.Rows.size();
			selection->Replaced = previousRows - rows.size();
		}
		for (const auto &row : geometry.Rows) {
			scene::DrawInstance draw;
			draw.Frame = Pose(row.Pose);
			draw.HalfExtent = {row.HalfExtent[0], row.HalfExtent[1], row.HalfExtent[2]};
			draw.Tint = {row.Tint[0], row.Tint[1], row.Tint[2]};
			draw.SurfaceColour = {row.SurfaceColour[0], row.SurfaceColour[1], row.SurfaceColour[2]};
			draw.EmissiveTint = {row.EmissiveTint[0], row.EmissiveTint[1], row.EmissiveTint[2]};
			draw.EmissiveStrength = row.EmissiveStrength;
			draw.Transparency = row.Transparency;
			draw.AlphaCutoff = row.AlphaCutoff;
			draw.SeamNormal = {row.SeamPlane[0], row.SeamPlane[1], row.SeamPlane[2]};
			draw.SeamOffset = row.SeamPlane[3];
			draw.SeamLight = {row.SeamLight[0], row.SeamLight[1], row.SeamLight[2]};
			std::array fields{
				&draw.Mesh,
				&draw.Texture,
				&draw.NormalMap,
				&draw.RoughnessMap,
				&draw.OcclusionMap,
				&draw.HeightMap,
				&draw.MetalnessMap,
				&draw.EmissiveMap,
				&draw.Shader
			};
			for (size_t index = 0; index < fields.size(); ++index) {
				*fields[index] = core::Name(row.Assets[index]);
			}
			for (size_t index = 0; index < ALPHA.size(); ++index) {
				if (row.Alpha == ALPHA[index]) {
					draw.Alpha = static_cast<scene::AlphaMode>(index);
				}
			}
			draw.Resample = row.Resample == "pixelated" ? scene::SurfaceResampleMode::Pixelated
														: scene::SurfaceResampleMode::Default;
			draw.CastShadow = row.CastShadow;
			draw.SourceWorld = sourceWorld;
			draw.SkinFirst = row.JointCount != 0 ? first + row.FirstJoint : 0;
			draw.SkinCount = static_cast<uint16_t>(row.JointCount);
			if (selection && !selection->Player.empty() && selection->Player == row.Player)
				selection->Hidden.push_back(static_cast<uint32_t>(rows.size()));
			rows.push_back(draw);
		}
		for (const auto &joint : geometry.Joints) {
			joints.push_back(Pose(joint));
		}
		error.clear();
		return true;
	}
}
