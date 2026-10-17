#include "Inertia.hpp"

#include <engine/core/Bytes.hpp>
#include <engine/physics/PortalIsland.hpp>

#include <algorithm>
#include <cmath>
#include <tuple>

namespace engine::physics {
	namespace {
		void WriteVector(core::ByteWriter &writer, const core::Vector3 &value) {
			writer.WriteFloat(value.X);
			writer.WriteFloat(value.Y);
			writer.WriteFloat(value.Z);
		}
		core::Vector3 ReadVector(core::ByteReader &reader) {
			return {reader.ReadFloat(), reader.ReadFloat(), reader.ReadFloat()};
		}
		void WriteId(core::ByteWriter &writer, const PortalIslandBodyId &id) {
			writer.WriteUInt64(id.KeyHigh);
			writer.WriteUInt64(id.KeyLow);
			writer.WriteUInt64(id.Generation);
			writer.WriteUInt32(id.Part);
		}
		PortalIslandBodyId ReadId(core::ByteReader &reader) {
			return {reader.ReadUInt64(), reader.ReadUInt64(), reader.ReadUInt64(), reader.ReadUInt32()};
		}
		PortalIslandBody *Find(std::vector<PortalIslandBody> &bodies, const PortalIslandBodyId &id) {
			const auto found =
				std::lower_bound(bodies.begin(), bodies.end(), id, [](const auto &body, const auto &key) {
					return body.Id < key;
				});
			return found != bodies.end() && found->Id == id ? &*found : nullptr;
		}
		std::array<core::Vector3, 3> Axes(const PortalIslandBody &body) {
			const std::array<core::Vector3, 3> fallback{
				core::Vector3::XAxis, core::Vector3::YAxis, core::Vector3::ZAxis
			};
			auto axes = body.PrincipalAxes;
			for (size_t index = 0; index < axes.size(); ++index) {
				if (!(axes[index].MagnitudeSquared() > 0.0f))
					axes[index] = fallback[index];
				else
					axes[index] = axes[index].Unit();
			}
			return axes;
		}
		core::Vector3 AngularResponse(const PortalIslandBody &body, const core::Vector3 &torque) {
			return AngularAcceleration(Axes(body), body.InverseInertia, torque);
		}

		void ApplyImpulse(PortalIslandBody &body, const core::Vector3 &arm, const core::Vector3 &impulse) {
			if (!(body.InverseMass > 0.0f)) return;
			body.LinearVelocity = body.LinearVelocity + impulse * body.InverseMass;
			body.AngularVelocity = body.AngularVelocity + AngularResponse(body, arm.Cross(impulse));
		}

		float
		AngularMass(const PortalIslandBody &body, const core::Vector3 &arm, const core::Vector3 &direction) {
			if (!(body.InverseMass > 0.0f)) return 0.0f;
			const core::Vector3 turn = AngularResponse(body, arm.Cross(direction));
			return turn.Cross(arm).Dot(direction);
		}
	}

	PortalIslandBody MakePortalIslandBody(const CopiedDynamicContact &body) {
		scene::Collider collider;
		collider.Extent = body.Extent;
		collider.Shape = body.Kind;
		return {
			{body.Identity.Key.High, body.Identity.Key.Low, body.Identity.Generation, 0},
			body.Frame.Position,
			body.Motion.Linear,
			body.Motion.Angular,
			InverseInertiaOf(collider, body.Mass),
			1.0f / body.Mass,
			body.Friction,
			body.Restitution,
			true,
			true,
			{body.Frame.RightVector(), body.Frame.UpVector(), body.Frame.ZVector()}
		};
	}

	PortalIslandStatus SolvePortalIsland(
		std::vector<PortalIslandBody> &bodies,
		std::vector<PortalIslandContact> contacts,
		std::vector<PortalIslandResult> &results,
		uint32_t sweeps
	) {
		results.clear();
		if (sweeps == 0) return PortalIslandStatus::Invalid;
		std::sort(bodies.begin(), bodies.end(), [](const auto &left, const auto &right) {
			return left.Id < right.Id;
		});
		if (std::adjacent_find(bodies.begin(), bodies.end(), [](const auto &left, const auto &right) {
				return left.Id == right.Id;
			}) != bodies.end())
			return PortalIslandStatus::Invalid;
		if (std::any_of(bodies.begin(), bodies.end(), [](const auto &body) { return !body.Available; }))
			return PortalIslandStatus::Unavailable;

		std::sort(contacts.begin(), contacts.end(), [](const auto &left, const auto &right) {
			return std::tie(
					   left.First,
					   left.Second,
					   left.Point.X,
					   left.Point.Y,
					   left.Point.Z,
					   left.Normal.X,
					   left.Normal.Y,
					   left.Normal.Z
				   ) <
				   std::tie(
					   right.First,
					   right.Second,
					   right.Point.X,
					   right.Point.Y,
					   right.Point.Z,
					   right.Normal.X,
					   right.Normal.Y,
					   right.Normal.Z
				   );
		});
		for (const PortalIslandContact &contact : contacts) {
			if (Find(bodies, contact.First) == nullptr || Find(bodies, contact.Second) == nullptr ||
				contact.First == contact.Second || !(contact.Normal.Magnitude() > 0.0f))
				return PortalIslandStatus::Invalid;
		}

		for (uint32_t sweep = 0; sweep < sweeps; ++sweep) {
			for (const PortalIslandContact &contact : contacts) {
				PortalIslandBody *first = Find(bodies, contact.First);
				PortalIslandBody *second = Find(bodies, contact.Second);
				const core::Vector3 normal = contact.Normal.Unit();
				const core::Vector3 firstArm = contact.Point - first->Centre;
				const core::Vector3 secondArm = contact.Point - second->Centre;
				const core::Vector3 firstPointVelocity =
					first->LinearVelocity + first->AngularVelocity.Cross(firstArm);
				const core::Vector3 secondPointVelocity =
					second->LinearVelocity + second->AngularVelocity.Cross(secondArm);
				const float closing = (secondPointVelocity - firstPointVelocity).Dot(normal);
				if (!(closing < 0.0f)) continue;
				const float mass = first->InverseMass + second->InverseMass +
								   AngularMass(*first, firstArm, normal) +
								   AngularMass(*second, secondArm, normal);
				if (!(mass > 0.0f)) continue;
				const float restitution =
					std::max({0.0f, contact.Restitution, first->Restitution, second->Restitution});
				const float impulse = -(1.0f + restitution) * closing / mass;
				ApplyImpulse(*first, firstArm, -normal * impulse);
				ApplyImpulse(*second, secondArm, normal * impulse);
			}
		}

		results.reserve(bodies.size());
		for (const PortalIslandBody &body : bodies)
			results.push_back({body.Id, body.LinearVelocity, body.AngularVelocity});
		return PortalIslandStatus::Complete;
	}

	core::Vector3 MapPortalInverseInertia(const core::Vector3 &sourceInverseInertia, float scale) {
		if (!(std::isfinite(scale) && scale > 0.0f)) return core::Vector3::Zero;
		return sourceInverseInertia / (scale * scale);
	}

	core::Vector3 MapPortalImpulseBack(const core::Vector3 &canonicalImpulse, float scale) {
		return std::isfinite(scale) && scale > 0.0f ? canonicalImpulse * scale : core::Vector3::Zero;
	}

	bool WritePortalIslandPacket(std::vector<std::byte> &out, const PortalIslandPacket &packet) {
		if (packet.Bodies.size() > 256 || packet.Contacts.size() > 512) return false;
		core::ByteWriter writer;
		writer.WriteUInt8(2);
		writer.WriteUInt32(static_cast<uint32_t>(packet.Bodies.size()));
		for (const auto &body : packet.Bodies) {
			WriteId(writer, body.Id);
			WriteVector(writer, body.Centre);
			WriteVector(writer, body.LinearVelocity);
			WriteVector(writer, body.AngularVelocity);
			WriteVector(writer, body.InverseInertia);
			for (const core::Vector3 &axis : body.PrincipalAxes)
				WriteVector(writer, axis);
			writer.WriteFloat(body.InverseMass);
			writer.WriteFloat(body.Friction);
			writer.WriteFloat(body.Restitution);
			writer.WriteUInt8(body.Available ? 1 : 0);
			writer.WriteUInt8(body.Owned ? 1 : 0);
		}
		writer.WriteUInt32(static_cast<uint32_t>(packet.Contacts.size()));
		for (const auto &contact : packet.Contacts) {
			WriteId(writer, contact.First);
			WriteId(writer, contact.Second);
			WriteVector(writer, contact.Point);
			WriteVector(writer, contact.Normal);
			writer.WriteFloat(contact.Penetration);
			writer.WriteFloat(contact.Friction);
			writer.WriteFloat(contact.Restitution);
		}
		out.assign(writer.Bytes().begin(), writer.Bytes().end());
		return true;
	}

	bool ReadPortalIslandPacket(std::span<const std::byte> bytes, PortalIslandPacket &packet) {
		core::ByteReader reader(bytes);
		if (reader.ReadUInt8() != 2) return false;
		PortalIslandPacket decoded;
		const uint32_t bodies = reader.ReadUInt32();
		if (bodies > 256) return false;
		for (uint32_t i = 0; i < bodies; ++i) {
			PortalIslandBody body;
			body.Id = ReadId(reader);
			body.Centre = ReadVector(reader);
			body.LinearVelocity = ReadVector(reader);
			body.AngularVelocity = ReadVector(reader);
			body.InverseInertia = ReadVector(reader);
			for (core::Vector3 &axis : body.PrincipalAxes)
				axis = ReadVector(reader);
			body.InverseMass = reader.ReadFloat();
			body.Friction = reader.ReadFloat();
			body.Restitution = reader.ReadFloat();
			body.Available = reader.ReadUInt8() != 0;
			body.Owned = reader.ReadUInt8() != 0;
			decoded.Bodies.push_back(body);
		}
		const uint32_t contacts = reader.ReadUInt32();
		if (contacts > 512) return false;
		for (uint32_t i = 0; i < contacts; ++i) {
			PortalIslandContact contact;
			contact.First = ReadId(reader);
			contact.Second = ReadId(reader);
			contact.Point = ReadVector(reader);
			contact.Normal = ReadVector(reader);
			contact.Penetration = reader.ReadFloat();
			contact.Friction = reader.ReadFloat();
			contact.Restitution = reader.ReadFloat();
			decoded.Contacts.push_back(contact);
		}
		if (reader.Failed() || reader.Remaining()) return false;
		packet = std::move(decoded);
		return true;
	}
}
