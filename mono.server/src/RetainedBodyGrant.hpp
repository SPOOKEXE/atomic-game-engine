#pragma once

#include <engine/replication/Authority.hpp>
#include <engine/script/PortalTransfer.hpp>
#include <engine/world/PresentationPeer.hpp>

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace server {
	// Server-owned proof that one authenticated presentation connection may request
	// exclusion for the player that a specific portal transfer committed here.
	class RetainedBodyGrants final {
	  public:
		static constexpr size_t MAXIMUM_GRANTS = 16;
		struct Grant {
			engine::replication::ClientId Client;
			engine::script::PortalTransferId Transfer;
			uint64_t DestinationIncarnation = 0;
			int64_t UserId = 0;
		};
		using CommittedTarget = std::function<bool(const Grant &)>;

		bool Issue(Grant grant);
		bool Authorizes(
			engine::replication::ClientId client,
			const engine::world::PresentationPeer &peer,
			const engine::world::PresentationAddress &receipt,
			std::string_view player,
			const CommittedTarget &committed
		) const;
		void Drop(engine::replication::ClientId client);
		void Prune(const CommittedTarget &committed);
		size_t Size() const {
			return Grants.size();
		}

	  private:
		std::vector<Grant> Grants;
	};
}
