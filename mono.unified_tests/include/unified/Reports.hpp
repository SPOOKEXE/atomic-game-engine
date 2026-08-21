#pragma once

// Every module's own report, side by side, and the contradictions between them.
//
// **This file imports report types rather than declaring fields.** A
// `server::ContentRelayStatistics` here *is* the one `mono.server` defines, and
// a `cdn::PublishReport` is the one `cdn::Publish` returns. That is deliberate
// and it is the whole design: a field renamed on the other side breaks this
// build, where a hand-copied mirror of it would keep compiling and keep
// reporting a number nothing produces any more. This module has already been
// bitten by exactly that - `AGENTS.md`'s note about the duplicated component
// list is the same lesson from the other direction.
//
// **A module cannot check its own seams and this is the reason the file
// exists.** `server`'s suite can say the relay counted eight chunks out.
// `client`'s can say the link counted eight chunks in. Neither can say *those
// are the same eight*, because neither links the other, and the tier system is
// right to stop them. Here both are on the link line, so the claim is
// checkable - and `CrossCheck` is the list of claims that are only checkable
// here.
//
// **What this is not.** It does not re-test a module against itself. A
// contradiction below always names two modules, and if one ever names one, it
// belongs in that module's own suite instead.
//
// @tier client · escapes to server

#include <engine/core/HeapProfile.hpp>
#include <engine/net/ConnectionStats.hpp>
#include <engine/net/LossyTransport.hpp>
#include <engine/replication/Authority.hpp>
#include <engine/replication/Replica.hpp>
#include <engine/replication/Session.hpp>
#include <engine/replication/SnapshotBuffer.hpp>

#include <cdn/Publisher.hpp>
#include <client/ContentLink.hpp>
#include <cstddef>
#include <cstdint>
#include <network/Beacon.hpp>
#include <network/Directory.hpp>
#include <optional>
#include <server/ContentRelay.hpp>
#include <string>
#include <unified/Arrangement.hpp>
#include <vector>

namespace unified {

	// What every module says it did, for one run.
	//
	// **Optional where the arrangement omits the module**, rather than
	// zero-filled. A relay that served nothing and a run with no relay in it
	// are different facts, and a zero for both is how a matrix reports twelve
	// passes when it ran one arrangement eleven times.
	//
	// @since v0.18
	struct Reports {
		// How the run was wired.
		Arrangement Ran;

		// Ticks stepped, the join included.
		uint64_t Ticks = 0;

		// --- what only this module can count ---------------------------------

		// Messages the authority produced across the run.
		//
		// **Counted here rather than read off a module**, because no module
		// counts it: `Authority::Statistics::Messages` is the last publish's,
		// and the four numbers below have to be a partition of one total for
		// the arithmetic in `CrossCheck` to mean anything.
		uint64_t Produced = 0;

		// Of those, the ones handed to whatever was carrying.
		uint64_t Handed = 0;

		// Of those, the ones the link would not take, handed back to the
		// authority through `Authority::Unsent`.
		//
		// **Not loss.** A refusal the sender knows about is repaired next tick;
		// the interesting failure is the one nobody is told about.
		uint64_t Refused = 0;

		// Of those, the ones discarded on purpose by `Settings::Drop`.
		uint64_t Lost = 0;

		// The largest single message produced, in bytes, before framing.
		//
		// The one thing an arrangement with no wire can say about the wire.
		size_t LargestMessage = 0;

		// --- engine ----------------------------------------------------------

		// What the server's half sent. `Engine::replication`.
		engine::replication::Authority::Statistics Authority;

		// What the client's half saw. `Engine::replication`.
		engine::replication::Replica::Statistics Replica;

		// How the replicated world was interpolated. `Engine::replication`.
		//
		// Absent when the client world holds no buffer, which is a failure and
		// not a configuration.
		std::optional<engine::replication::SnapshotBuffer::Statistics> Presentation;

		// The two sessions, server end and client end. `Engine::replication`.
		//
		// Absent under `Transport::Direct`, which has no session at all.
		//@{
		std::optional<engine::replication::Session::Statistics> ServerSession;
		std::optional<engine::replication::Session::Statistics> ClientSession;
		//@}

		// The two links. `Engine::net`.
		//
		// Absent under `Transport::Direct`.
		//@{
		std::optional<engine::net::ConnectionStats> ServerLink;
		std::optional<engine::net::ConnectionStats> ClientLink;
		//@}

		// What each end's lossy wrapper did to what reached it. `Engine::net`.
		//
		// `ToClient` is the wrapper on the client's end, so it is the one that
		// loses what the server sent. Absent unless `Transport::Lossy`.
		//@{
		std::optional<engine::net::LossStatistics> ToClient;
		std::optional<engine::net::LossStatistics> ToServer;
		//@}

		// What the process allocated. `Engine::core`.
		//
		// Always present, and zeroed rather than absent when the heap profiler
		// is compiled out - `HeapProfile::IsCompiledIn` is how a reader tells
		// those apart, since a build without the allocator hooks reports the
		// same zeroes as a process that has allocated nothing.
		engine::core::HeapTotals Heap;

		// --- cdn -------------------------------------------------------------

		// What the publication behind the relay contained. `Mono::cdn`.
		//
		// Absent unless `Content::Relayed`.
		std::optional<cdn::PublishReport> Published;

		// --- server ----------------------------------------------------------

		// What the host's relay did. `Mono::server`.
		//
		// Absent unless `Content::Relayed`.
		std::optional<server::ContentRelayStatistics> Relay;

		// --- client ----------------------------------------------------------

		// What the client's content link did. `Mono::client`.
		//
		// Absent unless `Content::Relayed`.
		std::optional<client::ContentLink::Counters> Link;

		// Entities each side held, and rows drawn, on the last tick.
		//@{
		size_t ServerEntities = 0;
		size_t ClientEntities = 0;
		size_t Drawn = 0;
		//@}

		// --- network ---------------------------------------------------------

		// What the beacon announced. `Mono::network`.
		//
		// Absent unless `Discovery::Advertised`.
		std::optional<network::BeaconCounters> Beacon;

		// What the directory heard. `Mono::network`.
		//
		// Absent unless `Discovery::Advertised`.
		std::optional<network::DirectoryCounters> Directory;
	};

	// Two modules' reports disagreeing.
	//
	// @since v0.18
	struct Contradiction {
		// The two modules whose reports cannot both be right, as
		// `"server/client"`. **Always two**: a claim about one module belongs
		// in that module's own suite.
		std::string Between;

		// What each said, spelled out with both numbers in it.
		//
		// Written to be the whole of a failure message. A reader of this line
		// should not have to open the report to know which number is wrong.
		std::string Says;
	};

	// Every claim that spans two modules, checked.
	//
	// **A claim only belongs here if no single module can make it.** "The
	// replica refused nothing" is `replication`'s. "The relay sent as many
	// chunks as the link accepted" is nobody's, because the two ends are in
	// two modules that do not link each other, and it is the class of bug this
	// whole program exists for.
	//
	// @param reports What every module said.
	// @return The disagreements, empty when the modules agree.
	// @since v0.18
	std::vector<Contradiction> CrossCheck(const Reports &reports);

	// Every module's report, printed for a person.
	//
	// One block per module, in tier order, with the absent ones named as
	// absent rather than omitted - so a reader can see that an arrangement had
	// no relay in it rather than guessing.
	//
	// @param reports What every module said.
	// @return The text, newline terminated.
	// @since v0.18
	std::string Format(const Reports &reports);
}
