#include "ConvexQuery.hpp"
#include "ShapeSupport.hpp"

#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/physics/Clock.hpp>
#include <engine/physics/CopiedContacts.hpp>
#include <engine/physics/Integrate.hpp>
#include <engine/physics/Query.hpp>
#include <engine/scene/CollisionShapes.hpp>
#include <engine/scene/SurfaceTable.hpp>
#include <engine/spatial/CollisionGroups.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <tuple>

namespace engine::physics {
	namespace {
		struct ContactStep {
			CopiedBodyContacts Body;
			core::CFrame Before;
			bool Started = false;
		};
		struct ContactCache {
			uint64_t Tick = 0;
			std::vector<ContactStep> Bodies;
		};
		struct DynamicContactCache {
			uint64_t Tick = 0;
			std::vector<CopiedDynamicBodyContacts> Bodies;
		};
		struct Plane {
			core::Vector3 Normal;
			float Offset = 0;
		};
		bool Finite(const core::Vector3 &value) {
			return std::isfinite(value.X) && std::isfinite(value.Y) && std::isfinite(value.Z);
		}
		bool Rigid(const core::CFrame &frame) {
			const auto q = frame.Rotation();
			const float norm = q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z;
			return Finite(frame.Position) && std::isfinite(norm) && std::abs(norm - 1) < .001f;
		}
		std::array<Plane, 6> Planes(const ContactWindow &window) {
			const auto first = window.First.Unit(), second = window.Second.Unit();
			return {
				{{window.Normal, window.Normal.Dot(window.Centre)},
				 {-window.Normal, -window.Normal.Dot(window.Centre) + window.Depth},
				 {first, first.Dot(window.Centre) + window.First.Magnitude()},
				 {-first, -first.Dot(window.Centre) + window.First.Magnitude()},
				 {second, second.Dot(window.Centre) + window.Second.Magnitude()},
				 {-second, -second.Dot(window.Centre) + window.Second.Magnitude()}}
			};
		}
		void WriteVector(core::ByteWriter &writer, const core::Vector3 &value) {
			writer.WriteFloat(value.X);
			writer.WriteFloat(value.Y);
			writer.WriteFloat(value.Z);
		}
		core::Vector3 ReadVector(core::ByteReader &reader) {
			return {reader.ReadFloat(), reader.ReadFloat(), reader.ReadFloat()};
		}
		std::string ReadText(core::ByteReader &reader) {
			const size_t size = reader.ReadUInt32();
			if (size > 256 || size > reader.Remaining()) {
				reader.Fail();
				return {};
			}
			std::string text(size, '\0');
			reader.ReadRaw(text.data(), size);
			if (text.empty() || text.find('\0') != std::string::npos) reader.Fail();
			return text;
		}
		bool WriteMask(core::ByteWriter &writer, spatial::LayerMask mask) {
			if (mask == spatial::LayerMask::All()) {
				writer.WriteUInt8(1);
				return true;
			}
			writer.WriteUInt8(0);
			std::vector<std::string> names;
			for (uint32_t bit = 0; bit < 32; ++bit) {
				if (!mask.Overlaps(spatial::LayerMask::Only(bit))) continue;
				const auto name = spatial::CollisionGroups::NameOf(bit);
				if (!name.IsValid()) return false;
				names.emplace_back(name.Text());
			}
			std::sort(names.begin(), names.end());
			writer.WriteUInt8(static_cast<uint8_t>(names.size()));
			for (const auto &name : names)
				writer.WriteString(name);
			return true;
		}
		spatial::LayerMask ReadMask(core::ByteReader &reader) {
			const auto all = reader.ReadUInt8();
			if (all == 1) return spatial::LayerMask::All();
			if (all != 0) {
				reader.Fail();
				return {};
			}
			const size_t count = reader.ReadUInt8();
			if (count > 32) {
				reader.Fail();
				return {};
			}
			spatial::LayerMask mask;
			std::string previous;
			for (size_t at = 0; at < count; ++at) {
				const auto name = ReadText(reader);
				if ((!previous.empty() && previous >= name) || reader.Failed()) {
					reader.Fail();
					return {};
				}
				previous = name;
				bool found = false;
				for (uint32_t bit = 0; bit < 32; ++bit) {
					if (spatial::CollisionGroups::NameOf(bit).Text() != name) continue;
					mask = mask | spatial::LayerMask::Only(bit);
					found = true;
					break;
				}
				if (!found) {
					reader.Fail();
					return {};
				}
			}
			return mask;
		}
		constexpr std::array<std::string_view, 4> KINDS = {"box", "sphere", "cylinder", "hull"};
		bool ValidShape(const CopiedContactShape &shape) {
			if (!Rigid(shape.Frame) || !Finite(shape.Extent) ||
				static_cast<size_t>(shape.Kind) >= KINDS.size() ||
				shape.Points.size() > MAXIMUM_COPIED_CONTACT_POINTS || !std::isfinite(shape.Friction) ||
				shape.Friction < 0 || !std::isfinite(shape.Restitution) || shape.Restitution < 0 ||
				shape.Restitution > 1 || !Finite(shape.Linear) || !Finite(shape.Angular))
				return false;
			if (shape.Kind == scene::ShapeKind::Hull)
				return !shape.Points.empty() && std::all_of(shape.Points.begin(), shape.Points.end(), Finite);
			return shape.Points.empty() && shape.Extent.X > 0 &&
				   (shape.Kind == scene::ShapeKind::Sphere || shape.Extent.Y > 0) &&
				   (shape.Kind != scene::ShapeKind::Box || shape.Extent.Z > 0);
		}
		bool ValidDynamic(const CopiedDynamicContact &body) {
			return body.Identity.Key.IsValid() && body.Identity.Generation != 0 && Rigid(body.Frame) &&
				   Finite(body.Motion.Linear) && Finite(body.Motion.Angular) && Finite(body.Extent) &&
				   body.Extent.X > 0 && body.Extent.Y > 0 && body.Extent.Z > 0 && std::isfinite(body.Mass) &&
				   body.Mass > 0 && std::isfinite(body.Friction) && body.Friction >= 0 &&
				   std::isfinite(body.Restitution) && body.Restitution >= 0 && body.Restitution <= 1 &&
				   static_cast<size_t>(body.Kind) < KINDS.size() && ValidContactWindow(body.Window);
		}
		// Intersect a convex point cloud with each plane. Intersections of all
		// inside/outside segments contain every new extreme; rebuilding discards
		// only interior points. Bounds are checked before accepting the result.
		bool Clip(std::vector<core::Vector3> &points, const std::array<Plane, 6> &planes) {
			if (points.size() > MAXIMUM_COPIED_CONTACT_POINTS) return false;
			for (const auto &plane : planes) {
				std::vector<core::Vector3> inside, outside;
				for (const auto &point : points)
					(plane.Normal.Dot(point) <= plane.Offset ? inside : outside).push_back(point);
				if (inside.empty()) {
					points.clear();
					return true;
				}
				if (outside.empty()) continue;
				std::vector<core::Vector3> clipped = inside;
				for (const auto &a : inside)
					for (const auto &b : outside) {
						const float share = (plane.Offset - plane.Normal.Dot(a)) / plane.Normal.Dot(b - a);
						clipped.push_back(a + (b - a) * share);
					}
				auto hull = collision::BuildConvexHull(clipped);
				if (hull.Points.size() >= collision::MAXIMUM_HULL_POINTS) return false;
				points = std::move(hull.Points);
			}
			return true;
		}
		bool Append(CopiedStaticContacts &contacts, CopiedContactShape shape) {
			if (!ValidShape(shape) || contacts.Shapes.size() == MAXIMUM_COPIED_CONTACT_SHAPES) return false;
			contacts.Shapes.push_back(std::move(shape));
			return true;
		}
		bool ClipAlignedBox(
			const ShapeInstance &shape,
			const std::array<Plane, 6> &planes,
			CopiedStaticContacts &contacts,
			bool &handled,
			const scene::SurfaceProperties &material,
			const scene::Motion &supportMotion
		) {
			float lower[] = {-shape.Extent.X, -shape.Extent.Y, -shape.Extent.Z};
			float upper[] = {shape.Extent.X, shape.Extent.Y, shape.Extent.Z};
			for (const auto &plane : planes) {
				const auto local = shape.Frame.VectorToObjectSpace(plane.Normal);
				const float axes[] = {local.X, local.Y, local.Z};
				size_t axis = 0;
				for (size_t at = 1; at < 3; ++at)
					if (std::abs(axes[at]) > std::abs(axes[axis])) axis = at;
				for (size_t at = 0; at < 3; ++at)
					if (at != axis && std::abs(axes[at]) > 1e-6f) return true;
				const float boundary = (plane.Offset - plane.Normal.Dot(shape.Frame.Position)) / axes[axis];
				if (axes[axis] > 0)
					upper[axis] = std::min(upper[axis], boundary);
				else
					lower[axis] = std::max(lower[axis], boundary);
			}
			handled = true;
			for (size_t axis = 0; axis < 3; ++axis)
				if (lower[axis] > upper[axis]) return true;
			const core::Vector3 centre{
				(lower[0] + upper[0]) * .5f, (lower[1] + upper[1]) * .5f, (lower[2] + upper[2]) * .5f
			};
			const core::Vector3 extent{
				(upper[0] - lower[0]) * .5f, (upper[1] - lower[1]) * .5f, (upper[2] - lower[2]) * .5f
			};
			if (extent.X > 0 && extent.Y > 0 && extent.Z > 0)
				return Append(
					contacts,
					{shape.Frame * core::CFrame(centre),
					 extent,
					 scene::ShapeKind::Box,
					 {},
					 material.Friction,
					 material.Restitution,
					 supportMotion.Linear,
					 supportMotion.Angular}
				);
			std::vector<core::Vector3> points;
			for (float x : {lower[0], upper[0]})
				for (float y : {lower[1], upper[1]})
					for (float z : {lower[2], upper[2]}) {
						const auto point = shape.Frame.PointToWorldSpace({x, y, z});
						if (std::find(points.begin(), points.end(), point) == points.end())
							points.push_back(point);
					}
			return Append(
				contacts,
				{core::CFrame{},
				 {},
				 scene::ShapeKind::Hull,
				 std::move(points),
				 material.Friction,
				 material.Restitution,
				 supportMotion.Linear,
				 supportMotion.Angular}
			);
		}

		void WriteCache(core::ByteWriter &, const void *, size_t) {}
		void ReadCache(core::ByteReader &, void *out, size_t count) {
			for (size_t at = 0; at < count; ++at)
				static_cast<ContactCache *>(out)[at] = {};
		}
		void WriteDynamicCache(core::ByteWriter &, const void *, size_t) {}
		void ReadDynamicCache(core::ByteReader &, void *out, size_t count) {
			for (size_t at = 0; at < count; ++at)
				static_cast<DynamicContactCache *>(out)[at] = {};
		}
		collision::ConvexHull HullOf(const CopiedContactShape &shape) {
			collision::ConvexHull hull;
			hull.Points = shape.Points;
			if (!hull.Points.empty()) {
				hull.Bounds = {hull.Points[0], hull.Points[0]};
				for (const auto &point : hull.Points)
					hull.Bounds = hull.Bounds.Union({point, point});
			}
			return hull;
		}
	}
	bool ValidContactWindow(const ContactWindow &window) {
		if (!Finite(window.Centre) || !Finite(window.Normal) || !Finite(window.First) ||
			!Finite(window.Second) || !std::isfinite(window.Depth) || window.Depth <= 0 ||
			window.Depth > 100000)
			return false;
		const float first = window.First.Magnitude(), second = window.Second.Magnitude();
		return first > 0 && second > 0 && std::abs(window.Normal.MagnitudeSquared() - 1) < .001f &&
			   std::abs(window.First.Dot(window.Normal)) < first * .001f &&
			   std::abs(window.Second.Dot(window.Normal)) < second * .001f &&
			   std::abs(window.First.Dot(window.Second)) < first * second * .001f;
	}
	bool CollectStaticContacts(
		ecs::Store &store, const ContactWindow &window, CopiedStaticContacts &out, std::string &failure
	) {
		ENGINE_PROFILE_CAT("physics.copy-static-contacts", core::ProfileCategory::Physics);
		if (!ValidContactWindow(window)) {
			failure = "invalid contact aperture";
			return false;
		}
		const auto planes = Planes(window);
		const auto *baked = scene::CollisionShapesOf(store);
		const auto *surfaces = store.Resource<scene::SurfaceTable>();
		CopiedStaticContacts contacts;
		bool complete = true;
		store.Each<const scene::Transform, const scene::Collider>(
			[&](ecs::Entity entity, const scene::Transform &pose, const scene::Collider &collider) {
				if (!complete || collider.Trigger || !collider.Layer.Overlaps(window.Mask) ||
					!collider.Mask.Overlaps(window.Layer))
					return;
				const auto *hull = baked && collider.Shape == scene::ShapeKind::Hull
									   ? baked->FindHull(collider.Geometry)
									   : nullptr;
				const auto *mesh = baked && collider.Shape == scene::ShapeKind::Mesh
									   ? baked->FindMesh(collider.Geometry)
									   : nullptr;
				const ShapeInstance shape{pose.Frame, collider.Extent, collider.Shape, hull, mesh};
				bool inside = true;
				for (const auto &plane : planes) {
					if (SupportPoint(shape, -plane.Normal).Dot(plane.Normal) > plane.Offset) return;
					inside = inside && SupportPoint(shape, plane.Normal).Dot(plane.Normal) <= plane.Offset;
				}
				const scene::RigidBody *rigid = store.Get<scene::RigidBody>(entity);
				// Dynamic rows travel through CollectDynamicContacts and the joined
				// portal-island solve. Treating their current transform as a static
				// obstacle would apply a second, stale impulse on this side.
				if (store.Has<scene::Simulated>(entity) &&
					(rigid == nullptr || rigid->Kind == scene::BodyKind::Dynamic))
					return;
				const scene::Motion supportMotion =
					store.Get<scene::Motion>(entity) ? *store.Get<scene::Motion>(entity) : scene::Motion{};
				if (!Rigid(pose.Frame)) {
					failure = "invalid static contact pose";
					complete = false;
					return;
				}
				scene::SurfaceProperties material;
				if (const auto *surface = store.Get<scene::Surface>(entity); surface && surfaces) {
					if (const auto *found = surfaces->Find(surface->Material)) material = *found;
				}
				if (const auto *override = store.Get<scene::PhysicsProperties>(entity);
					override && override->Custom) {
					material.Friction = override->Friction;
					material.Restitution = override->Elasticity;
				}
				if (!std::isfinite(material.Friction) || material.Friction < 0 ||
					!std::isfinite(material.Restitution) || material.Restitution < 0 ||
					material.Restitution > 1) {
					failure = "invalid static contact material";
					complete = false;
					return;
				}
				if (inside && shape.Shape != scene::ShapeKind::Mesh) {
					CopiedContactShape copy{
						pose.Frame,
						collider.Extent,
						shape.Shape,
						{},
						material.Friction,
						material.Restitution,
						supportMotion.Linear,
						supportMotion.Angular
					};
					if (hull) copy.Points = hull->Points;
					complete = Append(contacts, std::move(copy));
					return;
				}
				const auto addPoints = [&](std::vector<core::Vector3> points) {
					for (auto &point : points)
						point = pose.Frame.PointToWorldSpace(point);
					if (!Clip(points, planes)) return false;
					if (points.empty()) return true;
					return Append(
						contacts,
						{core::CFrame{},
						 {},
						 scene::ShapeKind::Hull,
						 std::move(points),
						 material.Friction,
						 material.Restitution,
						 supportMotion.Linear,
						 supportMotion.Angular}
					);
				};
				if (shape.Shape == scene::ShapeKind::Box) {
					bool handled = false;
					complete = ClipAlignedBox(shape, planes, contacts, handled, material, supportMotion);
					if (handled || !complete) return;
					std::vector<core::Vector3> points;
					for (int x : {-1, 1})
						for (int y : {-1, 1})
							for (int z : {-1, 1})
								points.push_back(
									{collider.Extent.X * x, collider.Extent.Y * y, collider.Extent.Z * z}
								);
					complete = addPoints(std::move(points));
				} else if (hull)
					complete = addPoints(hull->Points);
				else if (mesh) {
					if (mesh->TriangleCount() > 16384) {
						complete = false;
						return;
					}
					for (size_t at = 0; at < mesh->TriangleCount() && complete; ++at) {
						const auto triangle = mesh->TriangleAt(at);
						complete = addPoints({triangle.A, triangle.B, triangle.C});
					}
				} else {
					failure = "partially clipped curved static shape is unsupported";
					complete = false;
				}
			}
		);
		if (!complete) {
			if (failure.empty()) failure = "static contact geometry exceeds bounded representation";
			return false;
		}
		out = std::move(contacts);
		failure.clear();
		return true;
	}
	bool CollectDynamicContacts(
		ecs::Store &store, const ContactWindow &window, CopiedDynamicContacts &out, std::string &failure
	) {
		ENGINE_PROFILE_CAT("physics.copy-dynamic-contacts", core::ProfileCategory::Physics);
		if (!ValidContactWindow(window)) {
			failure = "invalid contact aperture";
			return false;
		}
		const auto planes = Planes(window);
		const auto *surfaces = store.Resource<scene::SurfaceTable>();
		CopiedDynamicContacts contacts;
		bool complete = true;
		store
			.Each<const scene::Transform, const scene::Collider, const scene::Motion, const scene::RigidBody>(
				[&](ecs::Entity entity,
					const scene::Transform &pose,
					const scene::Collider &collider,
					const scene::Motion &motion,
					const scene::RigidBody &rigid) {
					if (!complete || collider.Trigger || !store.Has<scene::Simulated>(entity) ||
						rigid.Kind != scene::BodyKind::Dynamic || !collider.Layer.Overlaps(window.Mask) ||
						!collider.Mask.Overlaps(window.Layer))
						return;
					const ShapeInstance shape{pose.Frame, collider.Extent, collider.Shape, nullptr, nullptr};
					for (const Plane &plane : planes)
						if (SupportPoint(shape, -plane.Normal).Dot(plane.Normal) > plane.Offset) return;
					if (!Rigid(pose.Frame) || !(rigid.Mass > 0)) {
						failure = "invalid dynamic contact body";
						complete = false;
						return;
					}
					const scene::BodyIdentity *identity = store.Get<scene::BodyIdentity>(entity);
					if (identity == nullptr || !identity->Key.IsValid() || identity->Generation == 0) {
						failure = "dynamic contact body has no stable identity";
						complete = false;
						return;
					}
					scene::SurfaceProperties material;
					if (const auto *surface = store.Get<scene::Surface>(entity); surface && surfaces) {
						if (const auto *found = surfaces->Find(surface->Material)) material = *found;
					}
					if (const auto *override = store.Get<scene::PhysicsProperties>(entity);
						override && override->Custom) {
						material.Friction = override->Friction;
						material.Restitution = override->Elasticity;
					}
					CopiedDynamicContact copy{
						*identity,
						pose.Frame,
						motion,
						collider.Extent,
						rigid.Mass,
						material.Friction,
						material.Restitution,
						collider.Shape,
						window
					};
					if (!ValidDynamic(copy) || contacts.Bodies.size() >= MAXIMUM_COPIED_CONTACT_SHAPES) {
						failure = "dynamic contact geometry exceeds bounded representation";
						complete = false;
						return;
					}
					contacts.Bodies.push_back(std::move(copy));
				}
			);
		if (!complete) return false;
		std::sort(contacts.Bodies.begin(), contacts.Bodies.end(), [](const auto &left, const auto &right) {
			return std::tie(left.Identity.Key.High, left.Identity.Key.Low, left.Identity.Generation) <
				   std::tie(right.Identity.Key.High, right.Identity.Key.Low, right.Identity.Generation);
		});
		out = std::move(contacts);
		failure.clear();
		return true;
	}
	bool WriteContactWindow(core::ByteWriter &writer, const ContactWindow &window) {
		if (!ValidContactWindow(window)) return false;
		core::ByteWriter bytes;
		bytes.WriteUInt8(1);
		WriteVector(bytes, window.Centre);
		WriteVector(bytes, window.Normal);
		WriteVector(bytes, window.First);
		WriteVector(bytes, window.Second);
		bytes.WriteFloat(window.Depth);
		if (!WriteMask(bytes, window.Layer) || !WriteMask(bytes, window.Mask)) return false;
		writer.WriteRaw(bytes.Bytes().data(), bytes.Size());
		return true;
	}
	bool ReadContactWindow(core::ByteReader &reader, ContactWindow &out) {
		if (reader.ReadUInt8() != 1) {
			reader.Fail();
			return false;
		}
		ContactWindow window{
			ReadVector(reader),
			ReadVector(reader),
			ReadVector(reader),
			ReadVector(reader),
			reader.ReadFloat(),
			ReadMask(reader),
			ReadMask(reader)
		};
		if (reader.Failed() || !ValidContactWindow(window)) {
			reader.Fail();
			return false;
		}
		out = window;
		return true;
	}
	bool WriteCopiedContacts(core::ByteWriter &writer, const CopiedStaticContacts &contacts) {
		if (contacts.Shapes.size() > MAXIMUM_COPIED_CONTACT_SHAPES) return false;
		core::ByteWriter bytes;
		bytes.WriteUInt8(3);
		bytes.WriteUInt32(static_cast<uint32_t>(contacts.Shapes.size()));
		for (const auto &shape : contacts.Shapes) {
			if (!ValidShape(shape)) return false;
			bytes.WriteString(KINDS[static_cast<size_t>(shape.Kind)]);
			WriteVector(bytes, shape.Frame.Position);
			const auto q = shape.Frame.Rotation();
			bytes.WriteFloat(q.w);
			bytes.WriteFloat(q.x);
			bytes.WriteFloat(q.y);
			bytes.WriteFloat(q.z);
			WriteVector(bytes, shape.Extent);
			bytes.WriteFloat(shape.Friction);
			bytes.WriteFloat(shape.Restitution);
			WriteVector(bytes, shape.Linear);
			WriteVector(bytes, shape.Angular);
			bytes.WriteUInt32(static_cast<uint32_t>(shape.Points.size()));
			for (const auto &point : shape.Points)
				WriteVector(bytes, point);
		}
		core::Metrics::Count("physics.copied_contact.bytes", static_cast<double>(bytes.Size()));
		core::Metrics::Count("physics.copied_contact.messages", 1);
		writer.WriteRaw(bytes.Bytes().data(), bytes.Size());
		return true;
	}
	bool ReadCopiedContacts(core::ByteReader &reader, CopiedStaticContacts &out) {
		if (reader.ReadUInt8() != 3) {
			reader.Fail();
			return false;
		}
		const size_t count = reader.ReadUInt32();
		if (count > MAXIMUM_COPIED_CONTACT_SHAPES) {
			reader.Fail();
			return false;
		}
		CopiedStaticContacts contacts;
		for (size_t at = 0; at < count; ++at) {
			const auto name = ReadText(reader);
			const auto kind = std::find(KINDS.begin(), KINDS.end(), name);
			if (kind == KINDS.end()) {
				reader.Fail();
				return false;
			}
			CopiedContactShape shape;
			shape.Kind = static_cast<scene::ShapeKind>(kind - KINDS.begin());
			const auto position = ReadVector(reader);
			const float w = reader.ReadFloat(), x = reader.ReadFloat(), y = reader.ReadFloat(),
						z = reader.ReadFloat();
			shape.Frame = {position, glm::quat{w, x, y, z}};
			shape.Extent = ReadVector(reader);
			shape.Friction = reader.ReadFloat();
			shape.Restitution = reader.ReadFloat();
			shape.Linear = ReadVector(reader);
			shape.Angular = ReadVector(reader);
			const size_t points = reader.ReadUInt32();
			if (points > MAXIMUM_COPIED_CONTACT_POINTS || points > reader.Remaining() / 12) {
				reader.Fail();
				return false;
			}
			for (size_t point = 0; point < points; ++point)
				shape.Points.push_back(ReadVector(reader));
			if (reader.Failed() || !ValidShape(shape)) {
				reader.Fail();
				return false;
			}
			contacts.Shapes.push_back(std::move(shape));
		}
		out = std::move(contacts);
		return true;
	}
	bool WriteCopiedDynamicContacts(core::ByteWriter &writer, const CopiedDynamicContacts &contacts) {
		if (contacts.Bodies.size() > MAXIMUM_COPIED_CONTACT_SHAPES) return false;
		core::ByteWriter bytes;
		bytes.WriteUInt8(1);
		bytes.WriteUInt32(static_cast<uint32_t>(contacts.Bodies.size()));
		for (const CopiedDynamicContact &body : contacts.Bodies) {
			if (!ValidDynamic(body)) return false;
			bytes.WriteUInt64(body.Identity.Key.High);
			bytes.WriteUInt64(body.Identity.Key.Low);
			bytes.WriteUInt64(body.Identity.Generation);
			WriteVector(bytes, body.Frame.Position);
			const auto rotation = body.Frame.Rotation();
			bytes.WriteFloat(rotation.w);
			bytes.WriteFloat(rotation.x);
			bytes.WriteFloat(rotation.y);
			bytes.WriteFloat(rotation.z);
			WriteVector(bytes, body.Motion.Linear);
			WriteVector(bytes, body.Motion.Angular);
			WriteVector(bytes, body.Extent);
			bytes.WriteFloat(body.Mass);
			bytes.WriteFloat(body.Friction);
			bytes.WriteFloat(body.Restitution);
			bytes.WriteString(KINDS[static_cast<size_t>(body.Kind)]);
			if (!WriteContactWindow(bytes, body.Window)) return false;
		}
		writer.WriteRaw(bytes.Bytes().data(), bytes.Size());
		return true;
	}
	bool ReadCopiedDynamicContacts(core::ByteReader &reader, CopiedDynamicContacts &out) {
		if (reader.ReadUInt8() != 1) {
			reader.Fail();
			return false;
		}
		const size_t count = reader.ReadUInt32();
		if (count > MAXIMUM_COPIED_CONTACT_SHAPES) {
			reader.Fail();
			return false;
		}
		CopiedDynamicContacts contacts;
		for (size_t at = 0; at < count; ++at) {
			CopiedDynamicContact body;
			body.Identity.Key = {reader.ReadUInt64(), reader.ReadUInt64()};
			body.Identity.Generation = reader.ReadUInt64();
			const core::Vector3 position = ReadVector(reader);
			body.Frame = {
				position, {reader.ReadFloat(), reader.ReadFloat(), reader.ReadFloat(), reader.ReadFloat()}
			};
			body.Motion.Linear = ReadVector(reader);
			body.Motion.Angular = ReadVector(reader);
			body.Extent = ReadVector(reader);
			body.Mass = reader.ReadFloat();
			body.Friction = reader.ReadFloat();
			body.Restitution = reader.ReadFloat();
			const std::string kind = ReadText(reader);
			const auto found = std::find(KINDS.begin(), KINDS.end(), kind);
			if (found == KINDS.end() || !ReadContactWindow(reader, body.Window)) {
				reader.Fail();
				return false;
			}
			body.Kind = static_cast<scene::ShapeKind>(found - KINDS.begin());
			if (reader.Failed() || !ValidDynamic(body)) {
				reader.Fail();
				return false;
			}
			contacts.Bodies.push_back(std::move(body));
		}
		out = std::move(contacts);
		return true;
	}
	void RegisterCopiedContactComponents() {
		ecs::Components::Register<ContactCache>("physics.CopiedContactCache", WriteCache, ReadCache);
		ecs::Components::Register<DynamicContactCache>(
			"physics.CopiedDynamicContactCache", WriteDynamicCache, ReadDynamicCache
		);
	}
	void SetCopiedBodyContacts(ecs::Store &store, std::vector<CopiedBodyContacts> contacts) {
		if (store.AdoptOnly()) return;
		ContactCache cache;
		cache.Tick = store.Time().Tick;
		for (auto &body : contacts) {
			const auto existing =
				std::find_if(cache.Bodies.begin(), cache.Bodies.end(), [&](const auto &step) {
					return step.Body.Root == body.Root;
				});
			if (existing != cache.Bodies.end()) {
				// Two active windows cannot independently decide one body's contact result.
				existing->Body.Complete = false;
				continue;
			}
			cache.Bodies.push_back({std::move(body), {}, false});
		}
		store.SetResource(cache);
	}
	void SetCopiedDynamicBodyContacts(ecs::Store &store, std::vector<CopiedDynamicBodyContacts> contacts) {
		if (store.AdoptOnly()) return;
		DynamicContactCache cache;
		cache.Tick = store.Time().Tick;
		for (auto &body : contacts) {
			if (body.Root == ecs::NULL_ENTITY ||
				body.Contacts.Bodies.size() > MAXIMUM_COPIED_CONTACT_SHAPES) {
				body.Complete = false;
				body.Contacts.Bodies.clear();
			}
			const auto duplicate =
				std::find_if(cache.Bodies.begin(), cache.Bodies.end(), [&](const auto &existing) {
					return existing.Root == body.Root;
				});
			if (duplicate != cache.Bodies.end()) {
				duplicate->Complete = false;
				duplicate->Contacts.Bodies.clear();
				continue;
			}
			cache.Bodies.push_back(std::move(body));
		}
		store.SetResource(cache);
	}
	std::span<const CopiedDynamicContact>
	CopiedDynamicContactsFor(const ecs::Store &store, ecs::Entity root) {
		const auto *cache = store.Resource<DynamicContactCache>();
		if (cache == nullptr || cache->Tick != store.Time().Tick) return {};
		const auto found = std::find_if(cache->Bodies.begin(), cache->Bodies.end(), [&](const auto &body) {
			return body.Root == root && body.Complete;
		});
		return found == cache->Bodies.end() ? std::span<const CopiedDynamicContact>{}
											: std::span<const CopiedDynamicContact>(found->Contacts.Bodies);
	}
	void BeginCopiedContactStep(ecs::Store &store) {
		auto *cache = store.ResourceMutable<ContactCache>();
		if (!cache || cache->Tick != store.Time().Tick) return;
		for (auto &step : cache->Bodies) {
			const auto *pose = store.Get<scene::Transform>(step.Body.Root);
			step.Started = pose && store.Has<scene::Motion>(step.Body.Root);
			if (step.Started) step.Before = pose->Frame;
		}
	}
	void SolveCopiedContactStep(ecs::Store &store) {
		ENGINE_PROFILE_CAT("physics.copied-static-contact", core::ProfileCategory::Physics);
		const auto *cache = store.Resource<ContactCache>();
		if (!cache || cache->Tick != store.Time().Tick || store.AdoptOnly()) return;
		const auto *baked = scene::CollisionShapesOf(store);
		for (const auto &step : cache->Bodies) {
			if (!step.Started) continue;
			const auto root = step.Body.Root;
			const auto *pose = store.Get<scene::Transform>(root);
			const auto *motion = store.Get<scene::Motion>(root);
			const auto *collider = store.Get<scene::Collider>(root);
			if (!pose || !motion || !collider) continue;
			if (!step.Body.Complete) {
				store.Set(root, scene::Transform{step.Before});
				store.Set(root, scene::Motion{});
				continue;
			}
			auto remaining = pose->Frame.Position - step.Before.Position;
			auto angular = motion->Angular * PhysicsStepSeconds(store);
			auto current = step.Before;
			auto stopped = *motion;
			bool contacted = false;
			const auto *hull = baked && collider->Shape == scene::ShapeKind::Hull
								   ? baked->FindHull(collider->Geometry)
								   : nullptr;
			// Resolve a bounded sequence of sliding contacts. Tangential floor motion
			// must survive the gravity contact that holds the body on the surface.
			for (int slide = 0; slide < 4; ++slide) {
				const ShapeInstance moving{current, collider->Extent, collider->Shape, hull, nullptr};
				ConvexSweep earliest;
				const CopiedContactShape *copiedHit = nullptr;
				// Resolve the supporting local floor before the copied floor's cut edge.
				const auto local = SweepPlacement(store, *collider, current, remaining, angular, root, true);
				if (!local.Complete) {
					current = step.Before;
					stopped = {};
					contacted = true;
					break;
				}
				earliest.Hit = local.Hit;
				earliest.Fraction = local.Fraction;
				earliest.Normal = local.Normal;
				earliest.ConservativeFallback = local.ConservativeFallback;
				for (const auto &fixed : step.Body.Geometry.Shapes) {
					const auto fixedHull = HullOf(fixed);
					const ShapeInstance obstacle{
						fixed.Frame,
						fixed.Extent,
						fixed.Kind,
						fixed.Kind == scene::ShapeKind::Hull ? &fixedHull : nullptr,
						nullptr
					};
					const auto hit = SweepConvexMotion(moving, remaining, angular, obstacle, {}, {}, 1, true);
					if (!hit.Hit) continue;
					if (!hit.ConservativeFallback && angular.MagnitudeSquared() < 1e-12f &&
						remaining.Dot(hit.Normal) >= -1e-6f)
						continue;
					if (!earliest.Hit || hit.Fraction < earliest.Fraction) {
						earliest = hit;
						copiedHit = &fixed;
					}
				}
				if (!earliest.Hit) {
					current = Advanced(current, remaining, angular, 1);
					break;
				}
				contacted = true;
				// Keep the next sweep outside a touching simplex with an ambiguous normal.
				const float length = remaining.Magnitude();
				const float safeFraction =
					length > 0 ? std::max(0.0f, earliest.Fraction - .001f / length) : earliest.Fraction;
				current = Advanced(current, remaining, angular, safeFraction);
				remaining = remaining * (1 - safeFraction);
				if (copiedHit) {
					// Match the local solver's material combination using resolved copied values.
					scene::SurfaceProperties own;
					if (const auto *table = store.Resource<scene::SurfaceTable>()) {
						if (const auto *surface = store.Get<scene::Surface>(root)) {
							if (const auto *row = table->Find(surface->Material)) own = *row;
						}
					}
					if (const auto *properties = store.Get<scene::PhysicsProperties>(root);
						properties && properties->Custom) {
						own.Friction = properties->Friction;
						own.Restitution = properties->Elasticity;
					}
					const core::Vector3 supportVelocity =
						copiedHit->Linear +
						copiedHit->Angular.Cross(current.Position - copiedHit->Frame.Position);
					const core::Vector3 relativeVelocity = stopped.Linear - supportVelocity;
					const float inward = std::max(0.0f, -relativeVelocity.Dot(earliest.Normal));
					const float restitution = std::max(own.Restitution, copiedHit->Restitution);
					const float friction = std::sqrt(std::max(0.0f, own.Friction * copiedHit->Friction));
					auto tangent = relativeVelocity + earliest.Normal * inward;
					const float tangentSpeed = tangent.Magnitude();
					if (tangentSpeed > 0 && inward > 0) {
						const float kept =
							std::max(0.0f, tangentSpeed - friction * inward * (1 + restitution));
						tangent = tangent * (kept / tangentSpeed);
					}
					stopped.Linear = supportVelocity + tangent + earliest.Normal * (inward * restitution);
					remaining = stopped.Linear * (PhysicsStepSeconds(store) * (1 - safeFraction));
				} else {
					const float displacementInto = remaining.Dot(earliest.Normal);
					if (displacementInto < 0) remaining = remaining - earliest.Normal * displacementInto;
					const float velocityInto = stopped.Linear.Dot(earliest.Normal);
					if (velocityInto < 0) stopped.Linear = stopped.Linear - earliest.Normal * velocityInto;
				}
				if (angular.MagnitudeSquared() > 0) stopped.Angular = {};
				angular = {};
				if (earliest.ConservativeFallback) break;
			}
			if (!contacted) continue;
			store.Set(root, scene::Transform{current});
			store.Set(root, stopped);
		}
	}
}
