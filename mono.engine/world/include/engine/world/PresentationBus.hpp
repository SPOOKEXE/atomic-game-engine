#pragma once

// Host-owned presentation traffic. These queues never enter simulation inboxes,
// snapshots or replay. Call only on the host's presentation owner thread.
// Endpoint receipts are exchanged by trusted host setup, not by message senders.

#include <engine/core/Bytes.hpp>
#include <engine/core/Name.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace engine::world {

	// Named endpoint incarnation. Session is assigned by host control and must
	// change on host restart; Generation changes on every local endpoint open.
	// arch-crossing
	struct PresentationAddress {
		// Stable name of the endpoint's local world.
		std::string World;
		// Application-defined channel opened in that world.
		std::string Channel;
		// Host-assigned incarnation that changes after a host restart.
		uint64_t Session = 0;
		// Local-open incarnation within Session.
		uint64_t Generation = 0;
		// Compares all four endpoint-incarnation fields.
		bool operator==(const PresentationAddress &) const = default;
	};

	// Maximum endpoints one advertised host directory may contain.
	inline constexpr uint32_t MAX_PRESENTATION_DIRECTORY = 64;
	// Full local endpoint set. Newer empty sets withdraw all endpoints from a host.
	// arch-crossing
	struct PresentationDirectory {
		// Host session to which every advertised endpoint belongs.
		uint64_t Session = 0;
		// Monotonic directory revision inside Session.
		uint64_t Revision = 0;
		// Complete endpoint set, with an empty set withdrawing all routes.
		std::vector<PresentationAddress> Endpoints;
		// Compares the directory session, revision, and endpoint set.
		bool operator==(const PresentationDirectory &) const = default;
	};

	// Presentation transport urgency. Only protocol producers may mark a message
	// urgent; receivers preserve each endpoint's sequence regardless of urgency.
	enum class PresentationPriority : uint8_t { Routine, TransferEye };

	// An owned message. Sequence is stamped by the sending endpoint; Correlation
	// belongs to the consumer protocol and is never interpreted by this layer.
	// arch-crossing
	struct PresentationMessage {
		// Endpoint that stamped Sequence and supplied the payload.
		PresentationAddress From;
		// Endpoint that must have opened before delivery.
		PresentationAddress To;
		// Per-sender order number assigned at admission.
		uint64_t Sequence = 0;
		// Opaque request and reply correlation owned by the consumer protocol.
		uint64_t Correlation = 0;
		// Owned consumer bytes, bounded before allocation.
		std::vector<std::byte> Payload;
		// Scheduling hint carried by the authenticated presentation route.
		PresentationPriority Priority = PresentationPriority::Routine;
	};

	// Hard wire bounds apply before names or payloads are allocated. Wire names
	// remain owned text, so rejected senders cannot grow the global intern table.
	inline constexpr uint32_t MAX_PRESENTATION_PAYLOAD = 4u * 1024u * 1024u;
	// Maximum UTF-8 byte length for a world or channel name on the wire.
	inline constexpr uint32_t MAX_PRESENTATION_NAME = 256;

	// Queue acceptance is not remote delivery acknowledgement. A caller needing
	// that guarantee sends a protocol reply and handles transport refusal.
	enum class PresentationStatus : uint8_t {
		Ok,
		Invalid,
		NoEndpoint,
		StaleEndpoint,
		WrongHost,
		Duplicate,
		TooLarge,
		Full,
	};

	// Per-endpoint and total bounds include incoming and outgoing payloads.
	struct PresentationLimits {
		// Maximum opened endpoints in one local bus.
		uint32_t Endpoints = 64;
		// Maximum retained messages for one endpoint.
		uint32_t MessagesPerEndpoint = 8;
		// Maximum retained payload bytes for one endpoint.
		uint64_t BytesPerEndpoint = 8u * 1024u * 1024u;
		// Maximum retained messages across all endpoints.
		uint32_t Messages = 64;
		// Maximum retained payload bytes across all endpoints.
		uint64_t Bytes = 32u * 1024u * 1024u;
		// Maximum payload accepted for one message.
		uint32_t MaximumPayload = MAX_PRESENTATION_PAYLOAD;
	};

	// A failed open carries no usable receipt.
	struct PresentationOpen {
		// Admission outcome for the requested endpoint.
		PresentationStatus Status = PresentationStatus::Invalid;
		// Usable endpoint receipt only when Status is Ok.
		PresentationAddress Address;
	};

	// Remote destination attribution is supplied by the host directory.
	struct PresentationOutbound {
		// Remote host selected from the trusted directory.
		core::Name Host;
		// Message awaiting delivery to that host.
		PresentationMessage Message;
	};

	// Current retained payload, not cumulative transfer volume.
	struct PresentationQueueSize {
		// Retained incoming and outgoing payload bytes.
		uint64_t Bytes = 0;
		// Retained incoming and outgoing message count.
		uint32_t Messages = 0;
	};

	// Cumulative accepted payload copies and ownership transfers. Discarded
	// counts are lifecycle purges; failed admission never counts as a copy.
	struct PresentationTraffic {
		// Messages accepted into a retained queue.
		uint64_t EnqueuedMessages = 0;
		// Payload bytes copied during message admission.
		uint64_t EnqueuedBytes = 0;
		// Messages removed by consumers or the outbound driver.
		uint64_t TakenMessages = 0;
		// Payload bytes transferred out of retained queues.
		uint64_t TakenBytes = 0;
		// Messages purged by endpoint or host retirement.
		uint64_t DiscardedMessages = 0;
		// Payload bytes purged by endpoint or host retirement.
		uint64_t DiscardedBytes = 0;
	};

	// Canonical little-endian encoding, transactional on failure. Names are text.
	// Encodes one bounded presentation message into the canonical wire form.
	bool WritePresentationMessage(core::ByteWriter &writer, const PresentationMessage &message);
	// Decodes one bounded presentation message without partial output on failure.
	bool ReadPresentationMessage(core::ByteReader &reader, PresentationMessage &message);
	// Encodes a complete endpoint directory into the canonical wire form.
	bool WritePresentationDirectory(core::ByteWriter &writer, const PresentationDirectory &directory);
	// Decodes a complete endpoint directory without partial output on failure.
	bool ReadPresentationDirectory(core::ByteReader &reader, PresentationDirectory &directory);
	// Driver route snapshots preserve each producer's original endpoint session.
	bool WritePresentationRoutes(core::ByteWriter &writer, const PresentationDirectory &directory);
	// Decodes driver routes while preserving producer endpoint sessions.
	bool ReadPresentationRoutes(core::ByteReader &reader, PresentationDirectory &directory);

	// Bounded router for owned presentation endpoints and copied messages.
	class PresentationBus {
	  public:
		// Zero session disables local opens until the host configures a session.
		explicit PresentationBus(uint64_t session = 0, const PresentationLimits &limits = {});
		// Opens a locally owned named endpoint and returns its incarnation receipt.
		PresentationOpen Open(core::Name world, core::Name channel);
		// Returns only an endpoint already admitted by local open or trusted host setup.
		PresentationAddress Lookup(std::string_view world, std::string_view channel) const;
		// Returns this host's complete locally opened endpoint directory.
		PresentationDirectory LocalDirectory() const;
		// Applies a remote host's newer complete endpoint directory.
		PresentationStatus ApplyDirectory(core::Name host, const PresentationDirectory &directory);
		// Removes a remote host's routes and queued traffic.
		void RetireHost(core::Name host);
		// Returns routes the named local consumer may use for remote sends.
		PresentationDirectory RoutesFor(core::Name consumer) const;
		// Applies authenticated routes supplied by the presentation driver.
		PresentationStatus ApplyRoutesFromDriver(const PresentationDirectory &directory);
		// Trusted host control only. Replacing an incarnation requires Close first.
		// Registers one trusted remote endpoint outside a full directory update.
		PresentationStatus RegisterRemote(core::Name host, const PresentationAddress &address);
		// Closes a local endpoint and purges its queued traffic.
		PresentationStatus Close(const PresentationAddress &address);
		// Closes every local endpoint belonging to a removed world.
		void RemoveWorld(core::Name world);
		// Invalidates all receipts and pending traffic; session must increase.
		bool Reset(uint64_t session, const PresentationLimits &limits = {});
		// Admits a locally sent message and queues any required outbound copy.
		PresentationStatus Send(
			const PresentationAddress &from,
			const PresentationAddress &to,
			uint64_t correlation,
			std::span<const std::byte> payload,
			PresentationPriority priority = PresentationPriority::Routine
		);
		// Validates the sender against an authenticated host connection.
		// Admits a message from an authenticated remote host.
		PresentationStatus Receive(core::Name host, const PresentationMessage &message);
		// Host side only: the driver has authenticated the remote sender already.
		// Admits a message already authenticated by the host driver.
		PresentationStatus ReceiveFromDriver(const PresentationMessage &message);
		// Transfers all queued inbound messages for one local endpoint.
		std::vector<PresentationMessage> Take(const PresentationAddress &address);
		// Transfers all messages queued for remote hosts.
		std::vector<PresentationOutbound> TakeOutbound();
		// Returns the current retained payload and message totals.
		PresentationQueueSize Queued() const;
		// Returns cumulative ownership-transfer and retirement totals.
		PresentationTraffic Traffic() const;

	  private:
		PresentationStatus
		ApplyDirectory(core::Name host, const PresentationDirectory &directory, bool routes);
		struct Endpoint {
			PresentationAddress Address;
			core::Name Host;
			uint64_t Sequence = 0;
		};
		Endpoint *Find(const PresentationAddress &address);
		const Endpoint *Find(const PresentationAddress &address) const;
		PresentationStatus Route(const PresentationMessage &message, std::span<const std::byte> payload);
		PresentationStatus Accept(core::Name host, const PresentationMessage &message, bool driver);
		void Purge(const PresentationAddress &address);
		void ReportRetired(PresentationQueueSize before, bool discarded);
		uint64_t Session = 0;
		uint64_t NextGeneration = 1;
		uint64_t DirectoryRevision = 1;
		uint64_t RoutesRevision = 1;
		struct DirectoryVersion {
			core::Name Host;
			uint64_t Session;
			uint64_t Revision;
			bool Retired = false;
		};
		std::vector<DirectoryVersion> DirectoryVersions;
		PresentationLimits Limits;
		std::vector<Endpoint> Endpoints;
		std::vector<PresentationOutbound> Pending;
		PresentationQueueSize Size;
		PresentationTraffic TrafficTotals;
	};
}
