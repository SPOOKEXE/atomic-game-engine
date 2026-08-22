#include <engine/net/Packet.hpp>

#include <cstdio>
#include <unified/Reports.hpp>

namespace unified {

	namespace {
		// A number, formatted the way every line below wants it.
		std::string Number(uint64_t value) {
			return std::to_string(value);
		}

		// One contradiction, with both numbers in the sentence.
		Contradiction Says(std::string between, std::string says) {
			return Contradiction{.Between = std::move(between), .Says = std::move(says)};
		}

		// A byte count as a person reads it.
		std::string Weight(uint64_t bytes) {
			char text[32];
			if (bytes < 1024) {
				std::snprintf(text, sizeof(text), "%llu B", static_cast<unsigned long long>(bytes));
			} else if (bytes < 1024 * 1024) {
				std::snprintf(text, sizeof(text), "%.1f KiB", static_cast<double>(bytes) / 1024.0);
			} else {
				std::snprintf(text, sizeof(text), "%.1f MiB", static_cast<double>(bytes) / (1024.0 * 1024.0));
			}
			return text;
		}
	}

	std::vector<Contradiction> CrossCheck(const Reports &reports) {
		std::vector<Contradiction> found;

		// --- replication against this program's own tally --------------------

		// **The partition.** Every message the authority produced was handed
		// over, refused by the link, or lost on purpose, and nothing else can
		// have happened to it. A message that falls out of this sum is one
		// somebody dropped on the floor between `Outgoing` and the wire, which
		// is a bug in the send loop rather than in either module - and is the
		// bug a send loop that ignored `Unsent` would have.
		if (reports.Handed + reports.Refused != reports.Produced) {
			found.push_back(Says(
				"replication/unified",
				"the authority produced " + Number(reports.Produced) + " message(s) and " +
					Number(reports.Handed) + " were handed over with " + Number(reports.Refused) +
					" refused, which does not account for all of them"
			));
		}

		// The client cannot hold a tick the server has not published. If it
		// does, the two are counting ticks against different clocks, and every
		// delay measured against either is meaningless.
		if (reports.Ticks > 0 && reports.Replica.Snapshots == 0 && reports.Replica.Deltas == 0) {
			found.push_back(Says(
				"replication/unified",
				"the authority produced " + Number(reports.Produced) +
					" message(s) and the replica applied no snapshot and no delta"
			));
		}

		// --- replication against net -----------------------------------------

		// **The four-times bug.** There is no framing under `Transport::Direct`
		// so a message over the limit crosses happily here and is refused by a
		// real link - which is why this is checked in every arrangement and not
		// only in the ones that have a wire.
		if (reports.LargestMessage > engine::net::Packet::MAXIMUM_MESSAGE_BYTES) {
			found.push_back(Says(
				"replication/net",
				"the authority produced a message of " + Number(reports.LargestMessage) +
					" bytes and a datagram carries at most " +
					Number(engine::net::Packet::MAXIMUM_MESSAGE_BYTES)
			));
		}

		if (reports.ServerSession.has_value() && reports.ClientSession.has_value()) {
			// A packet that did not open is forged, tampered with, or from a
			// session that ended. There is nobody else on a loopback, so any of
			// those means the two ends disagree about the cipher or the frame.
			const uint64_t unopened = reports.ServerSession->Unopened + reports.ClientSession->Unopened;
			if (unopened > 0) {
				found.push_back(Says(
					"replication/net",
					Number(unopened) +
						" packet(s) did not open, on a link with nobody else on it - the two ends "
						"disagree about the cipher or the frame"
				));
			}
		}

		if (reports.ServerLink.has_value() && reports.ClientLink.has_value()) {
			// Same argument one layer down: a refused packet is a bad magic, an
			// unknown version, or a length contradicting the frame, and the only
			// two builds on this loopback are the same build.
			const uint64_t refused = reports.ServerLink->PacketsRefused + reports.ClientLink->PacketsRefused;
			if (refused > 0) {
				found.push_back(Says(
					"replication/net",
					Number(refused) +
						" packet(s) were refused as not being this protocol, between two ends of one "
						"build"
				));
			}
		}

		// A lossy arrangement that lost nothing passes for the wrong reason,
		// which is the failure `net::LossStatistics` asks a caller to assert
		// against by name.
		if (Loses(reports.Ran.Carrying) && reports.ToClient.has_value() && reports.ToClient->Dropped == 0) {
			found.push_back(Says(
				"net/unified",
				"the arrangement is lossy and the wrapper on the client's end dropped nothing of the " +
					Number(reports.ToClient->Arrived) + " datagram(s) that reached it"
			));
		}

		// --- server against client -------------------------------------------

		// **The claim neither module can make.** `server`'s suite counts what
		// the relay sent and `client`'s counts what the link took; only a
		// process holding both can say they are the same traffic.
		if (reports.Relay.has_value() && reports.Link.has_value()) {
			// **Every ask is accounted for or it went missing.** A request the
			// relay accepted and one it refused for rate are both requests that
			// arrived; anything short of the client's own count is an ask that
			// left one module and reached neither branch of the other.
			const uint64_t arrived = reports.Relay->Requests + reports.Relay->Dropped;
			if (arrived > reports.Link->Requests) {
				found.push_back(Says(
					"client/server",
					"the relay was asked for " + Number(arrived) +
						" route(s) and the client only asked for " + Number(reports.Link->Requests)
				));
			}
			// Under `Transport::Direct` a request reaches the relay inside the
			// call that made it, so the two counts are equal on the same tick.
			// Over a wire the last ask of a run is legitimately still in flight,
			// and requiring equality there would be requiring that a link has no
			// delay.
			if (reports.Ran.Carrying == Transport::Direct && arrived != reports.Link->Requests) {
				found.push_back(Says(
					"client/server",
					"the client asked for " + Number(reports.Link->Requests) +
						" route(s) with nothing in between, and " + Number(arrived) + " reached the relay"
				));
			}

			// Only meaningful when nothing was allowed to go missing. Under
			// `Transport::Lossy` a chunk can genuinely be in flight or gone, and
			// asserting equality there would be asserting that loss does not
			// happen on a link built to make it happen.
			if (!Loses(reports.Ran.Carrying)) {
				if (reports.Relay->Served != reports.Link->Completed) {
					found.push_back(Says(
						"client/server",
						"the relay answered " + Number(reports.Relay->Served) +
							" route(s) in full and the client reassembled " + Number(reports.Link->Completed)
					));
				}
			}

			// A piece the client threw away is one the relay believes it
			// delivered. Nothing corrupts a chunk on any of these transports, so
			// this is the two ends disagreeing about the chunk format.
			if (reports.Link->Discarded > 0) {
				found.push_back(Says(
					"client/server",
					"the client discarded " + Number(reports.Link->Discarded) +
						" content piece(s) the relay had sent"
				));
			}
		}

		// --- cdn against server ------------------------------------------------

		if (reports.Published.has_value() && reports.Relay.has_value()) {
			// A publication with assets in it and a relay that refused every
			// route means the fetcher is not looking where the publisher wrote.
			// Each half is correct on its own and the pair is useless.
			if (reports.Published->Assets > 0 && reports.Relay->Served == 0 && reports.Relay->Refused > 0) {
				found.push_back(Says(
					"cdn/server",
					"the publication holds " + Number(reports.Published->Assets) +
						" asset(s) and the relay refused all " + Number(reports.Relay->Refused) +
						" request(s) for them"
				));
			}
		}

		// --- network against itself's other half -------------------------------

		// One announcement out is one listing or one refresh in. The subnet is
		// a lossless loopback, so a shortfall is the encoder and the decoder
		// disagreeing rather than a datagram going missing - which is the whole
		// reason to run the pair rather than each side's own suite.
		if (reports.Beacon.has_value() && reports.Directory.has_value()) {
			const uint64_t heard = reports.Directory->Listed + reports.Directory->Refreshed;
			if (heard != reports.Beacon->Announcements) {
				found.push_back(Says(
					"network/network",
					"the beacon announced " + Number(reports.Beacon->Announcements) +
						" time(s) and the directory listed or refreshed " + Number(heard)
				));
			}
			if (reports.Directory->Malformed > 0) {
				found.push_back(Says(
					"network/network",
					Number(reports.Directory->Malformed) +
						" announcement(s) did not decode, from the encoder in the same build"
				));
			}
		}

		// --- the world, across every module in the path -------------------------

		// The symptom the whole program is named after, and it spans all four:
		// the server's store, the wire, the client's store and the draw pass.
		if (reports.ClientEntities != reports.ServerEntities) {
			found.push_back(Says(
				"client/server",
				"the server holds " + Number(reports.ServerEntities) + " entity(s) and the client holds " +
					Number(reports.ClientEntities)
			));
		} else if (reports.ClientEntities > 0 && reports.Drawn != reports.ClientEntities) {
			found.push_back(Says(
				"client/scene",
				"the client holds " + Number(reports.ClientEntities) + " entity(s) and drew " +
					Number(reports.Drawn) + " of them"
			));
		}

		return found;
	}

	std::string Format(const Reports &reports) {
		std::string text;
		char line[256];

		const auto Row = [&text, &line](const char *format, auto... values) {
			std::snprintf(line, sizeof(line), format, values...);
			text += line;
		};

		Row("arrangement  %s, %llu tick(s)\n",
			reports.Ran.Name().c_str(),
			static_cast<unsigned long long>(reports.Ticks));

		text += "\nengine/replication\n";
		Row("  produced   %llu message(s), %llu handed, %llu refused, %llu lost on purpose\n",
			static_cast<unsigned long long>(reports.Produced),
			static_cast<unsigned long long>(reports.Handed),
			static_cast<unsigned long long>(reports.Refused),
			static_cast<unsigned long long>(reports.Lost));
		Row("  largest    %zu bytes of %zu a datagram carries\n",
			reports.LargestMessage,
			engine::net::Packet::MAXIMUM_MESSAGE_BYTES);
		Row("  authority  %zu visible, %zu resnapshot(s), %zu refused, %zu oversized\n",
			reports.Authority.Visible,
			reports.Authority.Resnapshots,
			reports.Authority.Refused,
			reports.Authority.Oversized);
		Row("  replica    %llu snapshot(s), %llu delta(s), %llu malformed, %llu stale\n",
			static_cast<unsigned long long>(reports.Replica.Snapshots),
			static_cast<unsigned long long>(reports.Replica.Deltas),
			static_cast<unsigned long long>(reports.Replica.Malformed),
			static_cast<unsigned long long>(reports.Replica.Stale));
		if (reports.Presentation.has_value()) {
			Row("  buffer     %llu tick(s), %llu stall(s), %llu resync(s), %llu extrapolated\n",
				static_cast<unsigned long long>(reports.Presentation->Ticks),
				static_cast<unsigned long long>(reports.Presentation->Stalls),
				static_cast<unsigned long long>(reports.Presentation->Resyncs),
				static_cast<unsigned long long>(reports.Presentation->Extrapolated));
		} else {
			text += "  buffer     absent - the client world holds no snapshot buffer\n";
		}

		text += "\nengine/net\n";
		if (reports.ServerSession.has_value() && reports.ClientSession.has_value()) {
			Row("  sessions   server %llu sent / %llu resent, client %llu sent / %llu resent\n",
				static_cast<unsigned long long>(reports.ServerSession->Sent),
				static_cast<unsigned long long>(reports.ServerSession->Retransmissions),
				static_cast<unsigned long long>(reports.ClientSession->Sent),
				static_cast<unsigned long long>(reports.ClientSession->Retransmissions));
		} else {
			text += "  sessions   absent - nothing is between the two halves\n";
		}
		if (reports.ServerLink.has_value() && reports.ClientLink.has_value()) {
			Row("  links      server %s out, client %s in, %llu packet(s) lost\n",
				Weight(reports.ServerLink->BytesSent).c_str(),
				Weight(reports.ClientLink->BytesReceived).c_str(),
				static_cast<unsigned long long>(reports.ClientLink->PacketsLost));
		} else {
			text += "  links      absent - nothing is between the two halves\n";
		}
		if (reports.ToClient.has_value() && reports.ToServer.has_value()) {
			Row("  loss       to client %llu of %llu dropped, to server %llu of %llu\n",
				static_cast<unsigned long long>(reports.ToClient->Dropped),
				static_cast<unsigned long long>(reports.ToClient->Arrived),
				static_cast<unsigned long long>(reports.ToServer->Dropped),
				static_cast<unsigned long long>(reports.ToServer->Arrived));
		} else {
			text += "  loss       absent - this link loses nothing\n";
		}

		text += "\nengine/core\n";
		if (engine::core::HeapProfile::IsCompiledIn()) {
			Row("  heap       %s live in %lld block(s), %s peak, %u tag(s)\n",
				Weight(static_cast<uint64_t>(reports.Heap.LiveBytes)).c_str(),
				static_cast<long long>(reports.Heap.LiveBlocks),
				Weight(static_cast<uint64_t>(reports.Heap.PeakBytes)).c_str(),
				reports.Heap.Nodes);
		} else {
			text += "  heap       absent - built without MONO_HEAP_PROFILE\n";
		}

		text += "\ncdn\n";
		if (reports.Published.has_value()) {
			Row("  published  %zu asset(s), %zu chunk(s), %zu bundle(s), %s stored\n",
				reports.Published->Assets,
				reports.Published->Chunks,
				reports.Published->Bundles,
				Weight(reports.Published->StoredBytes).c_str());
		} else {
			text += "  published  absent - this arrangement carries no content\n";
		}

		text += "\nserver\n";
		if (reports.Relay.has_value()) {
			Row("  relay      %llu asked, %llu served, %llu refused, %llu rate-dropped, %llu deferred, %s "
				"out\n",
				static_cast<unsigned long long>(reports.Relay->Requests),
				static_cast<unsigned long long>(reports.Relay->Served),
				static_cast<unsigned long long>(reports.Relay->Refused),
				static_cast<unsigned long long>(reports.Relay->Dropped),
				static_cast<unsigned long long>(reports.Relay->Deferred),
				Weight(reports.Relay->SentBytes).c_str());
		} else {
			text += "  relay      absent - this arrangement carries no content\n";
		}
		Row("  world      %zu entity(s)\n", reports.ServerEntities);

		text += "\nclient\n";
		if (reports.Link.has_value()) {
			Row("  content    %llu asked, %llu chunk(s), %llu completed, %llu refused, %llu discarded\n",
				static_cast<unsigned long long>(reports.Link->Requests),
				static_cast<unsigned long long>(reports.Link->Chunks),
				static_cast<unsigned long long>(reports.Link->Completed),
				static_cast<unsigned long long>(reports.Link->Refused),
				static_cast<unsigned long long>(reports.Link->Discarded));
		} else {
			text += "  content    absent - this arrangement carries no content\n";
		}
		Row("  world      %zu entity(s), %zu drawn\n", reports.ClientEntities, reports.Drawn);

		text += "\nnetwork\n";
		if (reports.Beacon.has_value() && reports.Directory.has_value()) {
			Row("  discovery  %llu announced, %llu listed, %llu refreshed, %llu malformed\n",
				static_cast<unsigned long long>(reports.Beacon->Announcements),
				static_cast<unsigned long long>(reports.Directory->Listed),
				static_cast<unsigned long long>(reports.Directory->Refreshed),
				static_cast<unsigned long long>(reports.Directory->Malformed));
		} else {
			text += "  discovery  absent - this arrangement announces nothing\n";
		}

		const std::vector<Contradiction> disagreements = CrossCheck(reports);
		if (disagreements.empty()) {
			text += "\nevery module's report agrees with every other\n";
		} else {
			text += "\ncontradictions\n";
			for (const Contradiction &contradiction : disagreements) {
				Row("  %-18s %s\n", contradiction.Between.c_str(), contradiction.Says.c_str());
			}
		}

		return text;
	}
}
