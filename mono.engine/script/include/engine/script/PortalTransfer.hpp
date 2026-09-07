#pragma once

#include <engine/core/types/CFrame.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/scene/PortalTransfer.hpp>
#include <engine/scene/SurfaceCameras.hpp>

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
		std::string SourceWorld;
		uint64_t SourceIncarnation = 0;
		uint64_t Sequence = 0;
		bool operator==(const PortalTransferId &) const = default;
	};

	enum class PortalTransferStage : uint8_t { Preparing, Committing, Committed, Refused, Cancelling };

	// Completed destination state in destination coordinates. The enclosing receipt owns identity.
	struct PortalTransferMotion {
		uint64_t DestinationIncarnation = 0;
		uint64_t DestinationTick = 0;
		uint64_t InputTick = 0;
		core::CFrame Frame;
		core::Vector3 Linear;
		core::Vector3 Angular;
		float WalkSpeed = 0;
		float JumpSpeed = 0;
		bool Grounded = false;
		// Completed world time, independent of input coalescing and transport delay.
		double SimulationSeconds = 0;
	};

	// Shared bounded codec for host-bus and authenticated play-session samples.
	bool ValidPortalTransferMotion(const PortalTransferMotion &motion);
	bool WritePortalTransferMotion(core::ByteWriter &writer, const PortalTransferMotion &motion);
	bool ReadPortalTransferMotion(core::ByteReader &reader, PortalTransferMotion &motion);

	struct PortalTransferReceipt {
		PortalTransferId Id;
		std::string DestinationWorld;
		PortalTransferStage Stage = PortalTransferStage::Preparing;
		scene::SeamTransform Through;
		std::string Diagnostic;
		scene::PortalBodyKind Kind = scene::PortalBodyKind::Player;
		// Destination control assignment, not a completed physics pose acknowledgement.
		uint64_t AcknowledgedInputTick = 0;
		std::optional<PortalTransferMotion> Motion;
	};

	void RegisterPortalTransferComponents();
	// Host control must assign a fresh nonzero incarnation on world recreation.
	// A snapshot restore retains the stored incarnation and must not reconfigure.
	bool
	ConfigurePortalTransfers(ecs::Store &store, uint64_t incarnation, bool requirePlayerAdmission = false);
	// Host admission names the exact destination incarnation before source retirement.
	bool
	AdmitPortalPlayerTransfer(ecs::Store &store, const PortalTransferId &id, uint64_t destinationIncarnation);
	// Keep source authority paused until the destination acknowledges reservation removal.
	bool
	CancelPortalPlayerTransfer(ecs::Store &store, const PortalTransferId &id, std::string_view diagnostic);
	uint64_t PortalTransferIncarnation(const ecs::Store &store);
	bool BeginPortalTransfer(
		ecs::Store &store,
		ecs::Entity player,
		std::string_view destination,
		const scene::SeamTransform &through,
		PortalTransferId &out,
		std::string &failure,
		std::optional<scene::PortalBodySweep> sweep = std::nullopt
	);
	bool BeginPortalObjectTransfer(
		ecs::Store &store,
		ecs::Entity object,
		std::string_view destination,
		const scene::SeamTransform &through,
		PortalTransferId &out,
		std::string &failure,
		std::optional<scene::PortalBodySweep> sweep = std::nullopt
	);
	void PumpPortalTransfers(ecs::Store &store);
	void RegisterPortalTransferSystems(ecs::Scheduler &scheduler);
	std::vector<PortalTransferReceipt> PortalTransferReceipts(const ecs::Store &store);
	std::optional<PortalTransferReceipt>
	PortalTransferOfPlayer(const ecs::Store &store, ecs::Entity sourcePlayer);
	// Resolves the exact committed receipt inside the destination store only.
	ecs::Entity PortalTransferPlayer(const ecs::Store &store, const PortalTransferId &id);
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

	enum class PortalInputDisposition : uint8_t { Immediate, Queued, Refused };
	// Queues timed native input against the last forwarded physics assignment.
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
	std::optional<PortalTransferReceipt>
	PortalTransferOfObject(const ecs::Store &store, ecs::Entity sourceObject);
	ecs::Entity PortalTransferObject(const ecs::Store &store, const PortalTransferId &id);

}
