#pragma once

#include <engine/assets/Signature.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/script/PortalTransfer.hpp>

#include <array>
#include <optional>
#include <span>
#include <vector>

namespace engine::game {

	// A resumable player admission is bound to one transfer and destination
	// incarnation. The capability travels only over authenticated channels.
	struct PortalResume {
		script::PortalTransferId Transfer;
		std::string Destination;
		uint64_t DestinationIncarnation = 0;
		uint64_t SourceSession = 0;
		std::array<std::byte, 32> Capability{};
		bool operator==(const PortalResume &) const = default;
	};

	enum class PortalSessionKind : uint8_t {
		Fresh,
		Resume,
		Ready,
		Commit,
		Committed,
		Transfer,
		Refused,
		LeaseRequest,
		LeaseRoute,
		Proceed,
		Crossed,
		Motion,
		LeaseAdopted
	};

	// One bounded application message, carried inside the authenticated play
	// connection or an authenticated world-bus envelope. Attempt correlates
	// responses with one connection attempt; it is not an entity identifier.
	struct PortalSessionMessage {
		PortalSessionKind Kind = PortalSessionKind::Fresh;
		uint64_t Attempt = 0;
		ecs::Entity Player;
		std::string World;
		PortalResume Claim;
		scene::SeamTransform Through;
		assets::PublicKey Identity;
		uint16_t Port = 0;
		std::string Diagnostic;
		std::optional<script::PortalTransferMotion> Motion{};
	};

	std::vector<std::byte> EncodePortalSession(const PortalSessionMessage &message);
	bool DecodePortalSession(std::span<const std::byte> bytes, PortalSessionMessage &message);

	// Host-local admissions. Time is monotonic host time supplied by the caller,
	// never simulation time or a timestamp supplied by a peer. Disconnect before
	// commit releases the reservation without destroying the transferred player.
	class PortalSessionLeases {
	  public:
		static constexpr size_t MAXIMUM_LEASES = 64;
		static constexpr double LIFETIME_SECONDS = 30.0;

		bool Offer(const PortalResume &claim, const assets::PublicKey &identity, double now);
		bool Reserve(
			const PortalResume &claim,
			const assets::PublicKey &identity,
			uint64_t peer,
			ecs::Entity committedPlayer,
			double now
		);
		bool Commit(const PortalResume &claim, uint64_t peer, double now);
		bool Committed(const PortalResume &claim, uint64_t peer, double now) const;
		// Destination adoption survives a dropped peer while its retry lease is live.
		bool Adopted(const PortalResume &claim, const assets::PublicKey &identity, double now) const;
		bool Reserved(uint64_t peer, double now) const;
		void Drop(uint64_t peer);
		void Expire(double now);
		std::optional<ecs::Entity> Player(const PortalResume &claim, uint64_t peer, double now) const;
		size_t Size() const {
			return Entries.size();
		}

	  private:
		struct Lease {
			PortalResume Claim;
			assets::PublicKey Identity;
			ecs::Entity Player;
			uint64_t Peer = 0;
			double Deadline = 0;
			bool Committed = false;
		};
		std::vector<Lease> Entries;
	};
}
