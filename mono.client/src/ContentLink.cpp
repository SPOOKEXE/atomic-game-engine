#include <engine/core/Log.hpp>
#include <engine/game/Content.hpp>
#include <engine/net/Endpoint.hpp>

#include <algorithm>
#include <client/ContentLink.hpp>
#include <utility>

namespace client {

	OfferedContent AcceptOfferedContent(
		const engine::game::ContentDirectory &directory, const std::vector<std::string> &allowedHosts
	) {
		OfferedContent accepted;
		for (const engine::game::ContentEndpoint &endpoint : directory.Endpoints) {
			if (endpoint.Name.empty() || endpoint.Location.empty()) {
				++accepted.UnknownKinds;
				continue;
			}

			engine::delivery::Source source;
			source.Name = endpoint.Name;
			source.Location = endpoint.Location;
			source.Enabled = true;
			// **Read, never write.** A server naming somewhere for a client to
			// upload to would be a server deciding what gets published, and
			// `SourceRole::Write` is invisible to a fetch anyway.
			source.Role = engine::delivery::SourceRole::Read;

			if (endpoint.Kind == "dir") {
				source.Kind = engine::delivery::SourceKind::Directory;
			} else if (endpoint.Kind == "http") {
				source.Kind = engine::delivery::SourceKind::Http;
				if (!engine::delivery::HostPermitted(source.Location, allowedHosts)) {
					// **The check lives with the list rather than at the fetch**,
					// which is `delivery/AGENTS.md`'s rule: a call site added later
					// is a call site that forgot.
					++accepted.RefusedByAllowList;
					continue;
				}
				if (!engine::net::Endpoint::Parse(source.Location)) {
					++accepted.UnresolvedNames;
					continue;
				}
			} else {
				// A kind this build has no row for. Skipped rather than refused, so
				// a server of a later build naming one more kind does not cost this
				// client the endpoints it can use.
				++accepted.UnknownKinds;
				continue;
			}

			accepted.Permitted.push_back(std::move(source));
		}
		return accepted;
	}

	std::vector<engine::delivery::Source> MergeContentSources(
		const std::vector<std::string> &configured,
		const std::vector<engine::delivery::Source> &offered,
		std::string_view relayLabel
	) {
		std::vector<engine::delivery::Source> merged;
		for (const std::string &entry : configured) {
			// `dir:` names a published store on this machine; anything else is an
			// address. One flag rather than two, because the priority order is a
			// single list and splitting it across two flags would make "local
			// first, then remote" unexpressible.
			const bool directory = entry.starts_with("dir:");
			merged.push_back(
				engine::delivery::Source{
					.Name = entry,
					.Kind = directory ? engine::delivery::SourceKind::Directory
									  : engine::delivery::SourceKind::Http,
					.Location = directory ? entry.substr(4) : entry,
					.Enabled = true,
				}
			);
		}

		merged.insert(merged.end(), offered.begin(), offered.end());

		if (!relayLabel.empty()) {
			merged.push_back(
				engine::delivery::Source{
					.Name = "server",
					.Kind = engine::delivery::SourceKind::Relay,
					.Location = std::string(relayLabel),
					.Enabled = true,
					.Role = engine::delivery::SourceRole::Read,
				}
			);
		}
		return merged;
	}

	ContentLink::ContentLink(Sender send) : Send(std::move(send)) {}

	ContentLink::Arriving *ContentLink::Find(uint64_t ticket) {
		for (Arriving &route : Live) {
			if (route.Ticket == ticket) {
				return &route;
			}
		}
		return nullptr;
	}

	bool ContentLink::Ask(uint64_t ticket, std::string_view route) {
		if (Find(ticket) == nullptr && Live.size() >= MAXIMUM_RELAYED_ROUTES) {
			// Refused rather than queued, `net::http::Client`'s reason: a queue
			// here would hide this end asking for more than it configured for,
			// and the symptom would be latency nobody can attribute.
			return false;
		}

		const std::vector<std::byte> message = engine::game::EncodeContentRequest(
			engine::game::ContentRouteRequest{.Ticket = ticket, .Route = std::string(route)}
		);
		if (!Send(message)) {
			return false;
		}

		if (Find(ticket) == nullptr) {
			Arriving arriving;
			arriving.Ticket = ticket;
			Live.push_back(std::move(arriving));
		}
		++Tally.Requests;
		return true;
	}

	void ContentLink::Collect(std::vector<engine::delivery::RelayAnswer> &into) {
		for (engine::delivery::RelayAnswer &answer : Finished) {
			into.push_back(std::move(answer));
		}
		Finished.clear();
	}

	void ContentLink::Abandon(uint64_t ticket) {
		for (auto route = Live.begin(); route != Live.end(); ++route) {
			if (route->Ticket == ticket) {
				Live.erase(route);
				break;
			}
		}
		for (auto answer = Finished.begin(); answer != Finished.end(); ++answer) {
			if (answer->Ticket == ticket) {
				Finished.erase(answer);
				break;
			}
		}
	}

	bool ContentLink::Receive(std::span<const std::byte> message) {
		engine::game::ContentRefusal refusal;
		if (engine::game::DecodeContentRefusal(message, refusal)) {
			if (Find(refusal.Ticket) == nullptr) {
				++Tally.Discarded;
				return true;
			}
			Abandon(refusal.Ticket);
			Finished.push_back(
				engine::delivery::RelayAnswer{.Ticket = refusal.Ticket, .Served = false, .Bytes = {}}
			);
			++Tally.Refused;
			return true;
		}

		engine::game::ContentChunk piece;
		if (!engine::game::DecodeContentChunk(message, piece)) {
			// Somebody else's message on a shared channel. A non-event, which is
			// what the tag exists for.
			return false;
		}

		Arriving *const route = Find(piece.Ticket);
		if (route == nullptr) {
			// A ticket this end never issued, or one already finished. Dropped
			// rather than assembled: a server cannot start a transfer nobody
			// asked for.
			++Tally.Discarded;
			return true;
		}

		if (!route->Sized) {
			if (piece.TotalBytes > MAXIMUM_RELAYED_ROUTE_BYTES) {
				ENGINE_WARN(
					"content: the server offered a {} byte route, which is past what this client will "
					"assemble",
					piece.TotalBytes
				);
				Abandon(piece.Ticket);
				Finished.push_back(
					engine::delivery::RelayAnswer{.Ticket = piece.Ticket, .Served = false, .Bytes = {}}
				);
				++Tally.Discarded;
				return true;
			}
			route->Bytes.resize(piece.TotalBytes);
			route->Sized = true;
		} else if (route->Bytes.size() != piece.TotalBytes) {
			// The total changed mid-route, which no honest sender does and which
			// would resize a buffer under bytes already held.
			++Tally.Discarded;
			return true;
		}

		if (piece.Offset != route->Filled) {
			++Tally.Discarded;
			return true;
		}

		std::copy(piece.Bytes.begin(), piece.Bytes.end(), route->Bytes.begin() + route->Filled);
		route->Filled += static_cast<uint32_t>(piece.Bytes.size());
		++Tally.Chunks;

		if (route->Filled >= route->Bytes.size()) {
			engine::delivery::RelayAnswer answer;
			answer.Ticket = route->Ticket;
			answer.Served = true;
			answer.Bytes = std::move(route->Bytes);
			Abandon(route->Ticket);
			Finished.push_back(std::move(answer));
			++Tally.Completed;
		}
		return true;
	}
}
