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
		std::string World;
		std::string Channel;
		uint64_t Session = 0;
		uint64_t Generation = 0;
		bool operator==(const PresentationAddress &) const = default;
	};

	inline constexpr uint32_t MAX_PRESENTATION_DIRECTORY = 64;
	// Full local endpoint set. Newer empty sets withdraw all endpoints from a host.
	// arch-crossing
	struct PresentationDirectory {
		uint64_t Session = 0;
		uint64_t Revision = 0;
		std::vector<PresentationAddress> Endpoints;
		bool operator==(const PresentationDirectory &) const = default;
	};

	// An owned message. Sequence is stamped by the sending endpoint; Correlation
	// belongs to the consumer protocol and is never interpreted by this layer.
	// arch-crossing
	struct PresentationMessage {
		PresentationAddress From;
		PresentationAddress To;
		uint64_t Sequence = 0;
		uint64_t Correlation = 0;
		std::vector<std::byte> Payload;
	};

	// Hard wire bounds apply before names or payloads are allocated. Wire names
	// remain owned text, so rejected senders cannot grow the global intern table.
	inline constexpr uint32_t MAX_PRESENTATION_PAYLOAD = 4u * 1024u * 1024u;
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
		uint32_t Endpoints = 64;
		uint32_t MessagesPerEndpoint = 8;
		uint64_t BytesPerEndpoint = 8u * 1024u * 1024u;
		uint32_t Messages = 64;
		uint64_t Bytes = 32u * 1024u * 1024u;
		uint32_t MaximumPayload = MAX_PRESENTATION_PAYLOAD;
	};

	// A failed open carries no usable receipt.
	struct PresentationOpen {
		PresentationStatus Status = PresentationStatus::Invalid;
		PresentationAddress Address;
	};

	// Remote destination attribution is supplied by the host directory.
	struct PresentationOutbound {
		core::Name Host;
		PresentationMessage Message;
	};

	// Current retained payload, not cumulative transfer volume.
	struct PresentationQueueSize {
		uint64_t Bytes = 0;
		uint32_t Messages = 0;
	};

	// Cumulative accepted payload copies and ownership transfers. Discarded
	// counts are lifecycle purges; failed admission never counts as a copy.
	struct PresentationTraffic {
		uint64_t EnqueuedMessages = 0;
		uint64_t EnqueuedBytes = 0;
		uint64_t TakenMessages = 0;
		uint64_t TakenBytes = 0;
		uint64_t DiscardedMessages = 0;
		uint64_t DiscardedBytes = 0;
	};

	// Canonical little-endian encoding, transactional on failure. Names are text.
	bool WritePresentationMessage(core::ByteWriter &writer, const PresentationMessage &message);
	bool ReadPresentationMessage(core::ByteReader &reader, PresentationMessage &message);
	bool WritePresentationDirectory(core::ByteWriter &writer, const PresentationDirectory &directory);
	bool ReadPresentationDirectory(core::ByteReader &reader, PresentationDirectory &directory);
	// Driver route snapshots preserve each producer's original endpoint session.
	bool WritePresentationRoutes(core::ByteWriter &writer, const PresentationDirectory &directory);
	bool ReadPresentationRoutes(core::ByteReader &reader, PresentationDirectory &directory);

	class PresentationBus {
	  public:
		// Zero session disables local opens until the host configures a session.
		explicit PresentationBus(uint64_t session = 0, const PresentationLimits &limits = {});
		PresentationOpen Open(core::Name world, core::Name channel);
		// Returns only an endpoint already admitted by local open or trusted host setup.
		PresentationAddress Lookup(std::string_view world, std::string_view channel) const;
		PresentationDirectory LocalDirectory() const;
		PresentationStatus ApplyDirectory(core::Name host, const PresentationDirectory &directory);
		void RetireHost(core::Name host);
		PresentationDirectory RoutesFor(core::Name consumer) const;
		PresentationStatus ApplyRoutesFromDriver(const PresentationDirectory &directory);
		// Trusted host control only. Replacing an incarnation requires Close first.
		PresentationStatus RegisterRemote(core::Name host, const PresentationAddress &address);
		PresentationStatus Close(const PresentationAddress &address);
		void RemoveWorld(core::Name world);
		// Invalidates all receipts and pending traffic; session must increase.
		bool Reset(uint64_t session, const PresentationLimits &limits = {});
		PresentationStatus Send(
			const PresentationAddress &from,
			const PresentationAddress &to,
			uint64_t correlation,
			std::span<const std::byte> payload
		);
		// Validates the sender against an authenticated host connection.
		PresentationStatus Receive(core::Name host, const PresentationMessage &message);
		// Host side only: the driver has authenticated the remote sender already.
		PresentationStatus ReceiveFromDriver(const PresentationMessage &message);
		std::vector<PresentationMessage> Take(const PresentationAddress &address);
		std::vector<PresentationOutbound> TakeOutbound();
		PresentationQueueSize Queued() const;
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
