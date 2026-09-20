#pragma once

#include <engine/world/Universe.hpp>

namespace engine::world {
	// Presentation channels admitted for one authenticated peer.
	struct PresentationPeerChannels {
		// Exact consumer channel names the peer may receive.
		std::vector<std::string> Consumers;
		// Slash-terminated prefixes permit viewport subchannels of a consumer.
		std::vector<std::string> ConsumerPrefixes;
		// Producer channel names the peer may publish.
		std::vector<std::string> Producers;
	};

	// One authenticated connection receives a private range of 64 numeric reply
	// slots on a host-owned world. The normal local directory publishes these
	// receipts through process hosts. The universe outlives the peer.
	class PresentationPeer {
	  public:
		// Reserves this connection's reply slots on a host-owned world.
		PresentationPeer(
			Universe &universe, WorldId world, uint32_t firstSlot, PresentationPeerChannels channels
		);
		~PresentationPeer();
		PresentationPeer(const PresentationPeer &) = delete;
		PresentationPeer &operator=(const PresentationPeer &) = delete;
		// Replaces the peer's authenticated endpoint directory.
		PresentationStatus Apply(const PresentationDirectory &directory);
		// Admits an incoming message only for a mapped live endpoint.
		PresentationStatus Accept(const PresentationMessage &message);
		// Exact live receipt ownership for the server's authenticated connection lookup.
		bool OwnsReceipt(const PresentationAddress &alias) const;
		// Drains messages waiting to return to this peer.
		std::vector<PresentationMessage> Take();
		// Returns the endpoint aliases currently advertised to the peer.
		PresentationDirectory Routes() const;
		// Retires all endpoint aliases and queued peer traffic.
		void Close();

	  private:
		struct Endpoint {
			PresentationAddress Original;
			PresentationAddress Alias;
			uint64_t ReceivedSequence = 0;
		};
		std::string Consumer(std::string_view channel) const;
		bool Producer(std::string_view channel) const;
		Universe &Worlds;
		WorldId World;
		uint32_t FirstSlot;
		PresentationPeerChannels Channels;
		uint64_t PeerSession = 0;
		uint64_t PeerRevision = 0;
		std::vector<Endpoint> Endpoints;
		bool Closed = false;
	};
}
