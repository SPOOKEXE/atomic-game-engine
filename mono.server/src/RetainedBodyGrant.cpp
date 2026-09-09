#include "RetainedBodyGrant.hpp"

#include <algorithm>

namespace server {
	namespace {
		bool Valid(const RetainedBodyGrants::Grant &grant) {
			return grant.Client.IsValid() && grant.Transfer.SourceWorld.size() <= 256 &&
				   !grant.Transfer.SourceWorld.empty() &&
				   grant.Transfer.SourceWorld.find('\0') == std::string::npos &&
				   grant.Transfer.SourceIncarnation != 0 && grant.Transfer.Sequence != 0 &&
				   grant.DestinationIncarnation != 0 && grant.UserId > 0;
		}
	}

	bool RetainedBodyGrants::Issue(Grant grant) {
		if (!Valid(grant)) return false;
		auto found = std::find_if(Grants.begin(), Grants.end(), [&](const auto &existing) {
			return existing.Client == grant.Client;
		});
		if (found != Grants.end()) {
			*found = std::move(grant);
			return true;
		}
		if (Grants.size() >= MAXIMUM_GRANTS) return false;
		Grants.push_back(std::move(grant));
		return true;
	}

	bool RetainedBodyGrants::Authorizes(
		engine::replication::ClientId client,
		const engine::world::PresentationPeer &peer,
		const engine::world::PresentationAddress &receipt,
		std::string_view player,
		const CommittedTarget &committed
	) const {
		if (!client.IsValid() || !peer.OwnsReceipt(receipt) || !committed) return false;
		return std::any_of(Grants.begin(), Grants.end(), [&](const auto &grant) {
			return grant.Client == client && player == std::to_string(grant.UserId) && committed(grant);
		});
	}

	void RetainedBodyGrants::Drop(engine::replication::ClientId client) {
		std::erase_if(Grants, [&](const auto &grant) { return grant.Client == client; });
	}

	void RetainedBodyGrants::Prune(const CommittedTarget &committed) {
		if (!committed) {
			Grants.clear();
			return;
		}
		std::erase_if(Grants, [&](const auto &grant) { return !committed(grant); });
	}
}
