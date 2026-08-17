#include <engine/core/Log.hpp>
#include <engine/game/Content.hpp>
#include <engine/net/Packet.hpp>

#include <algorithm>
#include <server/ContentRelay.hpp>
#include <utility>

namespace server {
	namespace {
		using engine::delivery::RouteState;
		using engine::replication::ClientId;

		// A chunk has to fit a sealed datagram, and `game` cannot say so itself.
		//
		// `game/Content.hpp` derives the number in a comment because that module
		// deliberately links no `net`. This is the place both constants are
		// visible, so this is where the arithmetic is checked rather than
		// trusted: the framing is `replication::User`'s two version bytes, one
		// kind byte and four-byte length, plus this message's tag, ticket, total,
		// offset and its own length prefix.
		constexpr size_t USER_FRAMING_BYTES = 2 + 1 + 4;
		constexpr size_t CHUNK_HEADER_BYTES = 1 + 8 + 4 + 4 + 4;
		static_assert(
			engine::game::MAXIMUM_CONTENT_CHUNK_BYTES + CHUNK_HEADER_BYTES + USER_FRAMING_BYTES <=
				engine::net::Packet::MAXIMUM_MESSAGE_BYTES,
			"a content chunk must fit one sealed datagram"
		);
	}

	ContentRelay::ContentRelay(
		std::unique_ptr<engine::delivery::RouteFetcher> fetcher, const ContentRelayLimits &limits
	)
		: Routes(std::move(fetcher)), Limits(limits) {}

	ContentRelay::~ContentRelay() = default;

	void ContentRelay::UseGrant(std::span<const std::byte> token) {
		Routes->UseGrant(token);
	}

	ContentRelay::Session &ContentRelay::SessionFor(ClientId client, double nowSeconds) {
		Session &session = Sessions[client.Index];
		if (!session.Opened || session.Generation != client.Generation) {
			// **A slot is reused the moment somebody leaves**, so a bucket left
			// behind would hand the next client on this slot the previous one's
			// allowance - or its punishment. `ClientId::Generation` exists for
			// exactly this and is what makes a stale handle safe.
			session = Session{};
			session.Opened = true;
			session.Generation = client.Generation;
			session.Tokens = Limits.Burst;
			session.LastSeconds = nowSeconds;
		}
		return session;
	}

	bool ContentRelay::Admits(Session &session, ClientId client, double nowSeconds) {
		if (nowSeconds < session.FlaggedUntil) {
			++session.Dropped;
			++Tally.Dropped;
			return false;
		}
		if (session.FlaggedUntil > 0.0) {
			// The cooldown is over. The count goes with it, so a client that
			// behaves after a bad minute is not one refusal away from being
			// flagged for the rest of the session.
			session.FlaggedUntil = 0.0;
			session.Dropped = 0;
		}

		const double elapsed = std::max(0.0, nowSeconds - session.LastSeconds);
		session.LastSeconds = nowSeconds;
		session.Tokens = std::min(Limits.Burst, session.Tokens + elapsed * Limits.RequestsPerSecond);

		if (session.Tokens < 1.0) {
			++session.Dropped;
			++Tally.Dropped;
			if (session.Dropped >= Limits.FloodThreshold) {
				session.FlaggedUntil = nowSeconds + Limits.FloodCooldownSeconds;
				++Tally.Flagged;
				// **Said once per flagging rather than once per drop**, which is
				// the difference between a line an operator reads and a log a
				// flood writes for them.
				ENGINE_WARN(
					"server: client {} is asking for content faster than the relay allows - refusing it "
					"for {:.0f}s",
					client.Index,
					Limits.FloodCooldownSeconds
				);
			}
			return false;
		}

		session.Tokens -= 1.0;
		return true;
	}

	bool ContentRelay::Receive(ClientId client, std::span<const std::byte> message, double nowSeconds) {
		engine::game::ContentRouteRequest asked;
		if (!engine::game::DecodeContentRequest(message, asked)) {
			// Somebody else's message on a shared channel. A non-event, which is
			// what the tag exists for.
			return false;
		}
		if (!client.IsValid()) {
			return true;
		}

		Session &session = SessionFor(client, nowSeconds);

		// **The rate check comes before the route is looked at**, the ordering
		// `Admission.hpp` states for its own three checks: the cheapest refusal
		// first, so a flood buys the least work possible.
		if (!Admits(session, client, nowSeconds)) {
			return true;
		}

		Job job;
		job.Ticket = asked.Ticket;

		if (session.Jobs.size() >= Limits.OutstandingPerClient ||
			!engine::delivery::RelayableRoute(asked.Route)) {
			job.Refused = true;
			session.Jobs.push_back(std::move(job));
			++Tally.Refused;
			return true;
		}

		job.Fetch = Routes->Request(asked.Route);
		if (job.Fetch == 0) {
			// The fetcher is full or the route is not one it carries. Refused
			// rather than queued: a queue here would hide a host asking for more
			// than it configured for, and the client falls through to its next
			// source either way.
			job.Refused = true;
			++Tally.Refused;
		} else {
			++Tally.Requests;
		}
		session.Jobs.push_back(std::move(job));
		return true;
	}

	void ContentRelay::Pump(const std::function<bool(ClientId, std::span<const std::byte>)> &send) {
		Routes->Pump();

		for (auto &entry : Sessions) {
			const ClientId client{.Index = entry.first, .Generation = entry.second.Generation};
			Session &session = entry.second;

			for (Job &job : session.Jobs) {
				if (job.Ready || job.Refused || job.Fetch == 0) {
					continue;
				}
				const RouteState state = Routes->StateOf(job.Fetch);
				if (state == RouteState::Pending) {
					continue;
				}
				if (state == RouteState::Ready) {
					Routes->Take(job.Fetch, job.Bytes);
					job.Ready = true;
				} else {
					Routes->Take(job.Fetch, job.Bytes);
					job.Refused = true;
					++Tally.Refused;
				}
				job.Fetch = 0;
			}

			size_t spent = 0;
			for (auto job = session.Jobs.begin(); job != session.Jobs.end();) {
				if (job->Refused) {
					const std::vector<std::byte> refusal = engine::game::EncodeContentRefusal(
						engine::game::ContentRefusal{.Ticket = job->Ticket}
					);
					if (!send(client, refusal)) {
						++Tally.Deferred;
						break;
					}
					job = session.Jobs.erase(job);
					continue;
				}
				if (!job->Ready) {
					++job;
					continue;
				}

				bool blocked = false;
				const auto total = static_cast<uint32_t>(job->Bytes.size());
				while (spent < Limits.ChunksPerClientPerPump && (job->Offset < total || !job->Opened)) {
					const auto carried = static_cast<uint32_t>(
						std::min<size_t>(engine::game::MAXIMUM_CONTENT_CHUNK_BYTES, total - job->Offset)
					);
					engine::game::ContentChunk piece;
					piece.Ticket = job->Ticket;
					piece.TotalBytes = total;
					piece.Offset = job->Offset;
					piece.Bytes.assign(
						job->Bytes.begin() + job->Offset, job->Bytes.begin() + job->Offset + carried
					);

					const std::vector<std::byte> encoded = engine::game::EncodeContentChunk(piece);
					if (!send(client, encoded)) {
						// **Ordinary backpressure**, not a loss. The link spent
						// its tick on the world, which is the ordering this
						// whole class is built around, so the piece goes out on
						// the next pump from exactly where it stopped.
						++Tally.Deferred;
						blocked = true;
						break;
					}

					job->Offset += carried;
					job->Opened = true;
					Tally.SentBytes += carried;
					++spent;
				}

				if (blocked || spent >= Limits.ChunksPerClientPerPump) {
					break;
				}
				if (job->Opened && job->Offset >= total) {
					++Tally.Served;
					job = session.Jobs.erase(job);
					continue;
				}
				++job;
			}
		}
	}

	void ContentRelay::Forget(ClientId client) {
		const auto found = Sessions.find(client.Index);
		if (found == Sessions.end() || found->second.Generation != client.Generation) {
			return;
		}
		for (Job &job : found->second.Jobs) {
			if (job.Fetch != 0) {
				Routes->Cancel(job.Fetch);
			}
		}
		Sessions.erase(found);
	}

	size_t ContentRelay::Busy() const {
		size_t busy = 0;
		for (const auto &entry : Sessions) {
			if (!entry.second.Jobs.empty()) {
				++busy;
			}
		}
		return busy;
	}
}
