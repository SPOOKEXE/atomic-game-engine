#pragma once

#include <engine/world/HostLink.hpp>
#include <engine/world/World.hpp>

namespace engine::world {
	class Universe;

	// Delegates selected local-world presentation channels to one trusted child.
	// The child and parent retain separate endpoint incarnations. Only envelopes
	// are translated; payloads stay opaque and no simulation storage is shared.
	// The universe must outlive the relay. Pump outside world tick batches.
	class PresentationRelay {
	  public:
		PresentationRelay(
			Universe &universe, WorldId world, uint64_t childSession, std::vector<std::string> channels
		);
		~PresentationRelay();
		PresentationRelay(const PresentationRelay &) = delete;
		PresentationRelay &operator=(const PresentationRelay &) = delete;
		// Nonblocking. False retires delegated endpoints; the caller retires the child.
		// An open coordinated frame defers traffic and disconnect cleanup until its end.
		bool Pump(HostLink &child);
		void Close();
		uint64_t Refused() const {
			return Refusals;
		}
		PresentationQueueSize Queued() const {
			return PendingSize;
		}

	  private:
		struct Binding {
			PresentationAddress Child;
			PresentationAddress Local;
			uint64_t ReceivedSequence = 0;
		};
		struct ReturnRoute {
			PresentationAddress Original;
			PresentationAddress Forwarded;
		};
		PresentationStatus Apply(const PresentationDirectory &directory);
		PresentationDirectory Routes();
		bool Retain(std::vector<PresentationMessage> &queue, PresentationMessage message);
		void FlushReplies();
		void FlushRequests(HostLink &child);
		Universe &Worlds;
		WorldId World;
		uint64_t ChildSession;
		uint64_t ChildRevision = 0;
		std::vector<std::string> Channels;
		std::vector<Binding> Bindings;
		std::vector<ReturnRoute> Returns;
		std::vector<PresentationMessage> Requests;
		std::vector<PresentationMessage> Replies;
		PresentationQueueSize PendingSize;
		uint64_t Refusals = 0;
		std::string ReturnWorld;
	};
}
