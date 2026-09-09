#include <engine/core/Bytes.hpp>
#include <engine/game/Play.hpp>
#include <engine/game/PortalSession.hpp>

#include <algorithm>
#include <cmath>

namespace engine::game {
	namespace {
		constexpr size_t MAXIMUM_WIRE_BYTES = 2048;
		constexpr size_t MAXIMUM_NAME_BYTES = 256;
		constexpr size_t MAXIMUM_DIAGNOSTIC_BYTES = 512;

		bool NameValid(std::string_view name) {
			return !name.empty() && name.size() <= MAXIMUM_NAME_BYTES && name.find('\0') == name.npos;
		}

		bool ClaimValid(const PortalResume &claim, bool unresolved = false) {
			return NameValid(claim.Transfer.SourceWorld) && NameValid(claim.Destination) &&
				   claim.Transfer.SourceWorld != claim.Destination && claim.Transfer.SourceIncarnation != 0 &&
				   claim.Transfer.Sequence != 0 && claim.SourceSession != 0 &&
				   (unresolved || claim.DestinationIncarnation != 0) &&
				   std::any_of(claim.Capability.begin(), claim.Capability.end(), [](std::byte byte) {
					   return byte != std::byte{0};
				   });
		}

		bool SameClaim(const PortalResume &left, const PortalResume &right) {
			unsigned difference = 0;
			for (size_t index = 0; index < left.Capability.size(); index++) {
				difference |= std::to_integer<unsigned>(left.Capability[index] ^ right.Capability[index]);
			}
			return difference == 0 && left.Transfer == right.Transfer &&
				   left.Destination == right.Destination &&
				   left.DestinationIncarnation == right.DestinationIncarnation &&
				   left.SourceSession == right.SourceSession;
		}

		bool ThroughValid(const scene::SeamTransform &through) {
			const auto rotation = through.Frame.Rotation();
			const float values[] = {
				through.Origin.X,
				through.Origin.Y,
				through.Origin.Z,
				through.Frame.Position.X,
				through.Frame.Position.Y,
				through.Frame.Position.Z,
				rotation.x,
				rotation.y,
				rotation.z,
				rotation.w,
				through.Scale
			};
			return std::all_of(
					   std::begin(values), std::end(values), [](float value) { return std::isfinite(value); }
				   ) &&
				   through.Scale > 0 && std::abs(glm::dot(rotation, rotation) - 1.0f) < .001f;
		}

		bool Valid(const PortalSessionMessage &message) {
			if (message.Attempt == 0) return false;
			switch (message.Kind) {
			case PortalSessionKind::Motion:
				return ClaimValid(message.Claim) && message.Motion &&
					   script::ValidPortalTransferMotion(*message.Motion) &&
					   message.Motion->DestinationIncarnation == message.Claim.DestinationIncarnation;
			case PortalSessionKind::Fresh:
				return true;
			case PortalSessionKind::Resume:
			case PortalSessionKind::Commit:
			case PortalSessionKind::Proceed:
			case PortalSessionKind::LeaseAdopted:
			case PortalSessionKind::Crossed:
				return ClaimValid(message.Claim);
			case PortalSessionKind::Ready:
			case PortalSessionKind::Committed:
				return message.Player != ecs::NULL_ENTITY && NameValid(message.World);
			case PortalSessionKind::Transfer:
			case PortalSessionKind::LeaseRoute:
				return ClaimValid(message.Claim) && ThroughValid(message.Through) &&
					   !message.Identity.IsZero() && message.Port != 0;
			case PortalSessionKind::LeaseRequest:
				return ClaimValid(message.Claim, true) && ThroughValid(message.Through) &&
					   !message.Identity.IsZero();
			case PortalSessionKind::Refused:
				return !message.Diagnostic.empty() && message.Diagnostic.size() <= MAXIMUM_DIAGNOSTIC_BYTES;
			}
			return false;
		}

		void WriteClaim(core::ByteWriter &writer, const PortalResume &claim) {
			writer.WriteString(claim.Transfer.SourceWorld);
			writer.WriteUInt64(claim.Transfer.SourceIncarnation);
			writer.WriteUInt64(claim.Transfer.Sequence);
			writer.WriteString(claim.Destination);
			writer.WriteUInt64(claim.DestinationIncarnation);
			writer.WriteUInt64(claim.SourceSession);
			writer.WriteRaw(claim.Capability.data(), claim.Capability.size());
		}

		bool ReadName(core::ByteReader &reader, std::string &out) {
			const auto text = reader.ReadString();
			if (reader.Failed() || !NameValid(text)) return false;
			out = text;
			return true;
		}

		bool ReadClaim(core::ByteReader &reader, PortalResume &claim) {
			if (!ReadName(reader, claim.Transfer.SourceWorld)) return false;
			claim.Transfer.SourceIncarnation = reader.ReadUInt64();
			claim.Transfer.Sequence = reader.ReadUInt64();
			if (!ReadName(reader, claim.Destination)) return false;
			claim.DestinationIncarnation = reader.ReadUInt64();
			claim.SourceSession = reader.ReadUInt64();
			return reader.ReadRaw(claim.Capability.data(), claim.Capability.size());
		}

		void WriteThrough(core::ByteWriter &writer, const scene::SeamTransform &through) {
			const auto rotation = through.Frame.Rotation();
			const float values[] = {
				through.Origin.X,
				through.Origin.Y,
				through.Origin.Z,
				through.Frame.Position.X,
				through.Frame.Position.Y,
				through.Frame.Position.Z,
				rotation.x,
				rotation.y,
				rotation.z,
				rotation.w,
				through.Scale
			};
			for (float value : values)
				writer.WriteFloat(value);
		}

		void ReadThrough(core::ByteReader &reader, scene::SeamTransform &through) {
			float values[11];
			for (float &value : values)
				value = reader.ReadFloat();
			through.Origin = {values[0], values[1], values[2]};
			through.Frame = core::CFrame(
				{values[3], values[4], values[5]}, glm::quat(values[9], values[6], values[7], values[8])
			);
			through.Scale = values[10];
		}
	}

	std::vector<std::byte> EncodePortalSession(const PortalSessionMessage &message) {
		if (!Valid(message)) return {};
		core::ByteWriter writer;
		writer.WriteUInt8(static_cast<uint8_t>(PlayMessage::PortalSession));
		writer.WriteUInt8(static_cast<uint8_t>(message.Kind));
		writer.WriteUInt64(message.Attempt);
		switch (message.Kind) {
		case PortalSessionKind::Motion:
			WriteClaim(writer, message.Claim);
			if (!script::WritePortalTransferMotion(writer, *message.Motion)) return {};
			break;
		case PortalSessionKind::Fresh:
			break;
		case PortalSessionKind::Resume:
		case PortalSessionKind::Commit:
		case PortalSessionKind::Proceed:
		case PortalSessionKind::LeaseAdopted:
		case PortalSessionKind::Crossed:
			WriteClaim(writer, message.Claim);
			break;
		case PortalSessionKind::Ready:
		case PortalSessionKind::Committed:
			writer.WriteUInt64(message.Player.Id);
			writer.WriteString(message.World);
			break;
		case PortalSessionKind::Transfer:
		case PortalSessionKind::LeaseRoute:
		case PortalSessionKind::LeaseRequest:
			WriteClaim(writer, message.Claim);
			WriteThrough(writer, message.Through);
			writer.WriteRaw(message.Identity.Value.data(), message.Identity.Value.size());
			if (message.Kind != PortalSessionKind::LeaseRequest) writer.WriteUInt16(message.Port);
			break;
		case PortalSessionKind::Refused:
			writer.WriteString(message.Diagnostic);
			break;
		}
		return {writer.Bytes().begin(), writer.Bytes().end()};
	}

	bool DecodePortalSession(std::span<const std::byte> bytes, PortalSessionMessage &out) {
		if (bytes.size() > MAXIMUM_WIRE_BYTES) return false;
		core::ByteReader reader(bytes);
		if (reader.ReadUInt8() != static_cast<uint8_t>(PlayMessage::PortalSession)) return false;
		PortalSessionMessage message;
		message.Kind = static_cast<PortalSessionKind>(reader.ReadUInt8());
		message.Attempt = reader.ReadUInt64();
		switch (message.Kind) {
		case PortalSessionKind::Motion: {
			if (!ReadClaim(reader, message.Claim)) return false;
			script::PortalTransferMotion motion;
			if (!script::ReadPortalTransferMotion(reader, motion)) return false;
			message.Motion = motion;
			break;
		}
		case PortalSessionKind::Fresh:
			break;
		case PortalSessionKind::Resume:
		case PortalSessionKind::Commit:
		case PortalSessionKind::Proceed:
		case PortalSessionKind::LeaseAdopted:
		case PortalSessionKind::Crossed:
			if (!ReadClaim(reader, message.Claim)) return false;
			break;
		case PortalSessionKind::Ready:
		case PortalSessionKind::Committed:
			message.Player = ecs::Entity(reader.ReadUInt64());
			if (!ReadName(reader, message.World)) return false;
			break;
		case PortalSessionKind::Transfer:
		case PortalSessionKind::LeaseRoute:
		case PortalSessionKind::LeaseRequest:
			if (!ReadClaim(reader, message.Claim)) return false;
			ReadThrough(reader, message.Through);
			if (!reader.ReadRaw(message.Identity.Value.data(), message.Identity.Value.size())) return false;
			if (message.Kind != PortalSessionKind::LeaseRequest) message.Port = reader.ReadUInt16();
			break;
		case PortalSessionKind::Refused: {
			const auto diagnostic = reader.ReadString();
			if (diagnostic.size() > MAXIMUM_DIAGNOSTIC_BYTES) return false;
			message.Diagnostic = diagnostic;
			break;
		}
		default:
			return false;
		}
		if (reader.Failed() || reader.Remaining() != 0 || !Valid(message)) return false;
		out = std::move(message);
		return true;
	}

	bool
	PortalSessionLeases::Offer(const PortalResume &claim, const assets::PublicKey &identity, double now) {
		if (!ClaimValid(claim) || identity.IsZero() || !std::isfinite(now)) return false;
		Expire(now);
		for (auto &lease : Entries) {
			if (SameClaim(lease.Claim, claim)) {
				if (lease.Identity != identity) return false;
				lease.Deadline = now + LIFETIME_SECONDS;
				return true;
			}
			if (lease.Claim.Transfer == claim.Transfer && lease.Claim.Destination == claim.Destination &&
				lease.Claim.DestinationIncarnation == claim.DestinationIncarnation)
				return false;
		}
		if (Entries.size() >= MAXIMUM_LEASES) return false;
		Entries.push_back(
			{.Claim = claim,
			 .Identity = identity,
			 .Player = ecs::NULL_ENTITY,
			 .Peer = 0,
			 .Deadline = now + LIFETIME_SECONDS,
			 .Committed = false}
		);
		return true;
	}

	bool PortalSessionLeases::Reserve(
		const PortalResume &claim,
		const assets::PublicKey &identity,
		uint64_t peer,
		ecs::Entity committedPlayer,
		double now
	) {
		if (peer == 0 || committedPlayer == ecs::NULL_ENTITY || !std::isfinite(now)) return false;
		for (const auto &lease : Entries) {
			if (lease.Peer == peer && now < lease.Deadline && !SameClaim(lease.Claim, claim)) return false;
		}
		for (auto &lease : Entries) {
			if (!SameClaim(lease.Claim, claim) || lease.Identity != identity || now >= lease.Deadline ||
				(lease.Peer != 0 && lease.Peer != peer) ||
				(lease.Player != ecs::NULL_ENTITY && lease.Player != committedPlayer))
				continue;
			lease.Peer = peer;
			lease.Player = committedPlayer;
			return true;
		}
		return false;
	}

	bool PortalSessionLeases::Commit(const PortalResume &claim, uint64_t peer, double now) {
		if (peer == 0 || !std::isfinite(now)) return false;
		for (auto &lease : Entries) {
			if (SameClaim(lease.Claim, claim) && lease.Peer == peer && now < lease.Deadline &&
				lease.Player != ecs::NULL_ENTITY) {
				lease.Committed = true;
				return true;
			}
		}
		return false;
	}

	bool PortalSessionLeases::Adopted(
		const PortalResume &claim, const assets::PublicKey &identity, double now
	) const {
		if (!std::isfinite(now)) return false;
		return std::any_of(Entries.begin(), Entries.end(), [&](const Lease &lease) {
			return lease.Committed && now < lease.Deadline && lease.Identity == identity &&
				   SameClaim(lease.Claim, claim);
		});
	}

	bool PortalSessionLeases::Reserved(uint64_t peer, double now) const {
		if (peer == 0 || !std::isfinite(now)) return false;
		return std::any_of(Entries.begin(), Entries.end(), [=](const Lease &lease) {
			return lease.Peer == peer && now < lease.Deadline && lease.Player != ecs::NULL_ENTITY;
		});
	}

	void PortalSessionLeases::Drop(uint64_t peer) {
		for (auto &lease : Entries) {
			if (lease.Peer == peer) lease.Peer = 0;
		}
	}

	bool PortalSessionLeases::Committed(const PortalResume &claim, uint64_t peer, double now) const {
		if (peer == 0 || !std::isfinite(now)) return false;
		for (const auto &lease : Entries) {
			if (SameClaim(lease.Claim, claim) && lease.Peer == peer && now < lease.Deadline)
				return lease.Committed;
		}
		return false;
	}

	void PortalSessionLeases::Expire(double now) {
		if (!std::isfinite(now)) return;
		std::erase_if(Entries, [now](const Lease &lease) { return now >= lease.Deadline; });
	}

	std::optional<ecs::Entity>
	PortalSessionLeases::Player(const PortalResume &claim, uint64_t peer, double now) const {
		if (peer == 0 || !std::isfinite(now)) return {};
		for (const auto &lease : Entries) {
			if (SameClaim(lease.Claim, claim) && lease.Peer == peer && now < lease.Deadline &&
				lease.Player != ecs::NULL_ENTITY)
				return lease.Player;
		}
		return {};
	}
}
