#pragma once

#include <engine/assets/ContentHash.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/scene/PortalCrossing.hpp>
#include <engine/scene/PortalTransfer.hpp>
#include <engine/scene/SurfaceCameras.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace engine::ecs {
	class Store;
	class Scheduler;
}

namespace engine::core {
	class ByteWriter;
	class ByteReader;
}

namespace engine::script {

	// arch-crossing: a receipt names its origin and incarnation, never a local entity.
	struct PortalTransferId {
		// Stable source-world name that owns the transfer.
		std::string SourceWorld;
		// Source-world incarnation that rejects stale receipts.
		uint64_t SourceIncarnation = 0;
		// Per-incarnation sequence assigned by the source transfer service.
		uint64_t Sequence = 0;
		// Compares the complete cross-world transfer identity.
		bool operator==(const PortalTransferId &) const = default;
	};

	// A named seam barrier. The domain is a stable world-clock name, never a local clock pointer.
	struct PortalTransferClock {
		// Stable name of the shared clock domain.
		std::string Domain;
		// Source-world tick at the barrier.
		uint64_t SourceTick = 0;
		// Destination-world tick at the barrier.
		uint64_t DestinationTick = 0;
		// Compares the complete named barrier clock.
		bool operator==(const PortalTransferClock &) const = default;
	};

	// The immutable fence carried by preparation, sealing and commit messages.
	struct PortalTransferFence {
		// Portal topology revision captured at preparation.
		uint64_t TopologyRevision = 0;
		// Authority epoch captured at preparation.
		uint64_t AuthorityEpoch = 0;
		// Monotonic revision of the preparation state.
		uint64_t PrepareRevision = 0;
		// Identifier of the sealed body baseline.
		uint64_t BaselineId = 0;
		// Content hash of the sealed body baseline.
		assets::ContentHash BaselineHash;
		// Source and destination ticks for the handoff barrier.
		PortalTransferClock H;
		// Compares the complete immutable handoff fence.
		bool operator==(const PortalTransferFence &) const = default;
	};

	// A portal transfer stage value.
	enum class PortalTransferStage : uint8_t {
		Preparing,
		Prepared,
		Committing,
		Committed,
		Refused,
		Cancelling
	};

	// Completed destination state in destination coordinates. The enclosing receipt owns identity.
	struct PortalTransferMotion {
		// Destination-world incarnation that completed the transfer.
		uint64_t DestinationIncarnation = 0;
		// Destination simulation tick that produced this motion sample.
		uint64_t DestinationTick = 0;
		// Source input tick incorporated into the destination state.
		uint64_t InputTick = 0;
		// World-space transform.
		core::CFrame Frame;
		// World-space linear velocity at the completed destination tick.
		core::Vector3 Linear;
		// World-space angular velocity at the completed destination tick.
		core::Vector3 Angular;
		// Walk speed.
		float WalkSpeed = 0;
		// Jump speed.
		float JumpSpeed = 0;
		// Whether the destination character was grounded at the sampled tick.
		bool Grounded = false;
		// Completed world time, independent of input coalescing and transport delay.
		double SimulationSeconds = 0;
	};

	// Shared bounded codec for host-bus and authenticated play-session samples.
	bool ValidPortalTransferMotion(const PortalTransferMotion &motion);
	// Encodes a bounded completed-motion acknowledgement.
	bool WritePortalTransferMotion(core::ByteWriter &writer, const PortalTransferMotion &motion);
	// Reads portal transfer motion.
	bool ReadPortalTransferMotion(core::ByteReader &reader, PortalTransferMotion &motion);

	// Cross-world receipt that tracks a portal transfer through its lifecycle.
	struct PortalTransferReceipt {
		// Stable identifier.
		PortalTransferId Id;
		// Destination world.
		std::string DestinationWorld;
		// Current lifecycle stage of the transfer receipt.
		PortalTransferStage Stage = PortalTransferStage::Preparing;
		// Seam transform that maps source state into the destination world.
		scene::SeamTransform Through;
		// Human-readable refusal or lifecycle diagnostic.
		std::string Diagnostic;
		// Record kind discriminator.
		scene::PortalBodyKind Kind = scene::PortalBodyKind::Player;
		// The latest destination-confirmed immutable handoff fence.
		PortalTransferFence Fence;
		// Destination control assignment, not a completed physics pose acknowledgement.
		uint64_t AcknowledgedInputTick = 0;
		// Optional completed destination motion acknowledgement.
		std::optional<PortalTransferMotion> Motion;
	};

	// Controls whether a store may commit without a host durable-decision acknowledgement.
	enum class PortalTransferDurability : uint8_t {
		InMemoryOnly,
		RequirePrepareCommit,
		// A listening host without a persistent journal exposes no source transfers.
		Disabled
	};

	// A sealed handoff awaiting durable host storage. The host may persist this after a tick.
	struct PortalTransferDecision {
		// Sealed transfer receipt awaiting durable storage.
		PortalTransferReceipt Receipt;
		// Persistent identity of the transferred body.
		scene::BodyIdentity Body;
		// Exact sealed transfer body hashed by Receipt.Fence.BaselineHash.
		std::vector<std::byte> Baseline;
	};

	// Registers portal transfer components.
	void RegisterPortalTransferComponents();
	// Host control must assign a fresh nonzero incarnation on world recreation.
	// A snapshot restore retains the stored incarnation and must not reconfigure.
	bool ConfigurePortalTransfers(
		ecs::Store &store,
		uint64_t incarnation,
		bool requirePlayerAdmission = false,
		PortalTransferDurability durability = PortalTransferDurability::InMemoryOnly
	);
	// Returns sealed durable decisions that still require a host acknowledgement.
	std::vector<PortalTransferDecision> PortalTransferPendingDecisions(const ecs::Store &store);
	// Marks a matching sealed decision durable after the host's journal write completes.
	// Repeating the acknowledgement for the same receipt is safe.
	bool MarkPortalTransferDurable(
		ecs::Store &store, const PortalTransferId &id, const assets::ContentHash &baselineHash
	);
	// Recreates an exact sealed source record after a host restart. It accepts
	// only the durable baseline and fence, never a reconstructed local pose.
	bool RestorePortalTransferDecision(ecs::Store &store, const PortalTransferDecision &decision);
	// Host admission names the exact destination incarnation before source retirement.
	bool
	AdmitPortalPlayerTransfer(ecs::Store &store, const PortalTransferId &id, uint64_t destinationIncarnation);
	// Keep source authority paused until the destination acknowledges reservation removal.
	bool
	CancelPortalPlayerTransfer(ecs::Store &store, const PortalTransferId &id, std::string_view diagnostic);
	// Returns the current source-world incarnation used for new receipt ids.
	uint64_t PortalTransferIncarnation(const ecs::Store &store);
	// Starts a player transfer after capturing its bounded portal body.
	bool BeginPortalTransfer(
		ecs::Store &store,
		ecs::Entity player,
		std::string_view destination,
		const scene::SeamTransform &through,
		PortalTransferId &out,
		std::string &failure,
		std::optional<scene::PortalBodySweep> sweep = std::nullopt
	);
	// Starts a movable-object transfer after capturing its bounded subtree.
	bool BeginPortalObjectTransfer(
		ecs::Store &store,
		ecs::Entity object,
		std::string_view destination,
		const scene::SeamTransform &through,
		PortalTransferId &out,
		std::string &failure,
		std::optional<scene::PortalBodySweep> sweep = std::nullopt
	);
	// Advances pending transfer stages at the world's tick boundary.
	void PumpPortalTransfers(ecs::Store &store);
	// Registers portal transfer systems.
	void RegisterPortalTransferSystems(ecs::Scheduler &scheduler);
	// Returns a snapshot of locally retained transfer receipts.
	std::vector<PortalTransferReceipt> PortalTransferReceipts(const ecs::Store &store);
	// Transfers a player through a portal.
	std::optional<PortalTransferReceipt>
	PortalTransferOfPlayer(const ecs::Store &store, ecs::Entity sourcePlayer);
	// Resolves the exact committed receipt inside the destination store only.
	ecs::Entity PortalTransferPlayer(const ecs::Store &store, const PortalTransferId &id);
	// Returns the destination's sealed fence before its local body is made live.
	std::optional<PortalTransferFence>
	PortalTransferDestinationFence(const ecs::Store &store, const PortalTransferId &id);
	// Queues validated source-space movement for this player's transfer. Delivery
	// is acknowledged after destination control assignment; no source entity crosses the bus.
	// inputTick preserves the client clock; zero denotes unstamped host/local control.
	// stepSeconds is the duration of that clock's tick; zero denotes unknown timing.
	bool ForwardPortalPlayerMove(
		ecs::Store &store,
		ecs::Entity sourcePlayer,
		const core::Vector3 &direction,
		bool jump,
		uint64_t inputTick = 0,
		double stepSeconds = 0
	);
	// Native destination input takes over permanently from the previous host.
	void ClosePortalPlayerMoveForwarding(ecs::Store &store, ecs::Entity destinationPlayer);
	// Records that the source applied this player's input tick to its still-live
	// body, so the sealed baseline tells the destination not to replay it.
	void NotePortalPlayerInputApplied(ecs::Store &store, ecs::Entity sourcePlayer, uint64_t inputTick);
	// The newest input a sealed source body holds while it is frozen for a player
	// transfer, or nothing when this world still applies the player's input.
	// Later input is forwarded rather than applied, so a host must not report it
	// consumed: the client keeps predicting it until the destination applies it.
	std::optional<uint64_t> FrozenPortalPlayerInput(const ecs::Store &store, ecs::Entity sourcePlayer);

	// A portal input disposition value.
	enum class PortalInputDisposition : uint8_t { Immediate, Queued, Refused };
	// Queues timed native input against the last forwarded physics assignment.
	// Forwarding from the previous host stays open until the first queued move
	// is applied, so input sent there before takeover still fills that time.
	// Untimed local control cancels pending input and applies immediately.
	PortalInputDisposition SchedulePortalPlayerMove(
		ecs::Store &store,
		ecs::Entity player,
		const core::Vector3 &direction,
		bool jump,
		uint64_t inputTick,
		double stepSeconds
	);
	// Actual applied input for a live rig with a forwarded or native portal clock.
	std::optional<uint64_t> AppliedPortalPlayerInput(const ecs::Store &store, ecs::Entity player);
	// Transfers an object through a portal.
	std::optional<PortalTransferReceipt>
	PortalTransferOfObject(const ecs::Store &store, ecs::Entity sourceObject);
	// Returns the destination object entity for a committed receipt, when local.
	ecs::Entity PortalTransferObject(const ecs::Store &store, const PortalTransferId &id);

}
