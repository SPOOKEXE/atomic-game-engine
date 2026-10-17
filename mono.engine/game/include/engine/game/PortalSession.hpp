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
		// Stable identifier for transfer.
		script::PortalTransferId Transfer;
		// Destination world or endpoint identifier.
		std::string Destination;
		// Destination incarnation used by this object.
		uint64_t DestinationIncarnation = 0;
		// Source session used by this object.
		uint64_t SourceSession = 0;
		// Capability authorizing the portal resume.
		std::array<std::byte, 32> Capability{};
		// Compares every resume claim field, including the opaque capability bytes.
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
		LeaseAdopted,
		// A bounded, non-authoritative route discovery used to warm a visible
		// destination before any player transfer exists.
		Approach
	};

	// One bounded application message, carried inside the authenticated play
	// connection or an authenticated world-bus envelope. Attempt correlates
	// responses with one connection attempt; it is not an entity identifier.
	struct PortalSessionMessage {
		// Classification of this record.
		PortalSessionKind Kind = PortalSessionKind::Fresh;
		// Attempt used by this object.
		uint64_t Attempt = 0;
		// Player entity associated with this record.
		ecs::Entity Player;
		// Source or destination world name carried across the authenticated session.
		std::string World;
		// Destination named by an advisory approach route.
		std::string Destination;
		// Portal resume claim that owns this history.
		PortalResume Claim;
		// Portal seam transform used to carry prediction state.
		scene::SeamTransform Through;
		// Peer identity that signed the session message.
		assets::PublicKey Identity;
		// Network port carried by the session message.
		uint16_t Port = 0;
		// Stable authored source-mouth identity. It is a route string, never a
		// process-local entity handle, so a client can retire one prewarm safely.
		std::string Seam;
		// Diagnostic text carried with the session message.
		std::string Diagnostic;
		// Authoritative or transferred motion sample.
		std::optional<script::PortalTransferMotion> Motion{};
		// Destination-observed sealed fence. A client promotes local portal geometry
		// only after this matches the source preparation receipt exactly.
		std::optional<script::PortalTransferFence> Fence{};
	};

	std::vector<std::byte> EncodePortalSession(const PortalSessionMessage &message);
	bool DecodePortalSession(std::span<const std::byte> bytes, PortalSessionMessage &message);

	// Host-local admissions. Time is monotonic host time supplied by the caller,
	// never simulation time or a timestamp supplied by a peer. Disconnect before
	// commit releases the reservation without destroying the transferred player.
	class PortalSessionLeases {
	  public:
		// Configured limit for maximumleases.
		static constexpr size_t MAXIMUM_LEASES = 64;
		// Lifetimeseconds used by this object.
		static constexpr double LIFETIME_SECONDS = 30.0;

		// Records a fresh destination claim when its capacity and identity checks pass.
		bool Offer(const PortalResume &claim, const assets::PublicKey &identity, double now);
		// Binds an offered claim to the authenticated peer and committed player.
		bool Reserve(
			const PortalResume &claim,
			const assets::PublicKey &identity,
			uint64_t peer,
			ecs::Entity committedPlayer,
			double now
		);
		// Marks the peer's reservation committed while its monotonic lease remains live.
		bool Commit(const PortalResume &claim, uint64_t peer, double now);
		// Tests whether the claim belongs to a currently committed peer lease.
		bool Committed(const PortalResume &claim, uint64_t peer, double now) const;
		// Destination adoption survives a dropped peer while its retry lease is live.
		bool Adopted(const PortalResume &claim, const assets::PublicKey &identity, double now) const;
		// Tests whether the peer still owns a nonexpired reservation.
		bool Reserved(uint64_t peer, double now) const;
		// Releases every lease owned by a disconnected peer.
		void Drop(uint64_t peer);
		// Removes claims whose caller-supplied monotonic deadline has elapsed.
		void Expire(double now);
		// Returns the committed destination entity for a live claim and peer.
		std::optional<ecs::Entity> Player(const PortalResume &claim, uint64_t peer, double now) const;
		// Number of currently retained destination leases.
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
