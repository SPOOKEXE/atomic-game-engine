#pragma once

// A server, a client, and whatever the arrangement says goes between them.
//
// **This is `Harness` grown an axis.** That class held both halves of
// replication with `net` cut out of the middle, and answered one question very
// well: which of four stages lost the world. This holds the same two halves and
// lets the middle be a real link, lets the server also carry content out of a
// `cdn` publication, and lets the session announce itself to a `network`
// directory - so it answers a second question the first could not: **do the
// modules agree with each other about what happened.**
//
// `Harness` is this class with `Arrangement{}`, and is still the right tool for
// the first question. See `unified/Harness.hpp`.
//
// **Why this is not in any module's own suite.** Every piece below has a suite
// where it belongs, and none of those suites can hold this: `mono.server` may
// not link `mono.client`, `Engine::replication` may not link either, and
// `mono.cdn` knows nothing about a game link. The tier system is right to
// refuse all three. So the arrangement lives at the one place where every tier
// is already on the link line, which is here.
//
// **What it is still not for.** It does not re-test a module against itself. A
// case that started asserting things about delta encoding would be a copy of
// `engine.replication.*`, and a case that asserted things about chunk layout
// would be a copy of `cdn.delivery`. What is this program's own is the seams
// *between* those, and `unified/Reports.hpp` is the list of them.
//
// **Time is passed in, never read.** A tick is a call and a frame is a call, so
// a run is reproducible from its `Settings` and its `Arrangement` and from
// nothing else. Same rule as `net/AGENTS.md` and `replication/AGENTS.md`.
//
// @tier client · escapes to server

#include <engine/ecs/Entity.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/net/LossyTransport.hpp>
#include <engine/net/Transport.hpp>
#include <engine/replication/Authority.hpp>
#include <engine/replication/QuicSession.hpp>
#include <engine/replication/Replica.hpp>
#include <engine/replication/Session.hpp>
#include <engine/replication/SessionPort.hpp>
#include <engine/replication/SnapshotBuffer.hpp>

#include <client/ContentLink.hpp>
#include <client/Scene.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <network/Beacon.hpp>
#include <network/Directory.hpp>
#include <server/ContentRelay.hpp>
#include <span>
#include <string>
#include <unified/Arrangement.hpp>
#include <unified/Reports.hpp>
#include <vector>

namespace unified {

	// How the two halves are built and how hard the middle is made to be.
	//
	// @since v0.5
	struct Settings {
		// Entities in the server's placeholder world.
		uint32_t Entities = 64;

		// A scene file to author the server's world from, or empty for the
		// placeholder.
		//
		// The same loader the client and the server call, over the same file.
		// This harness exists to prove a server and a client agree, so a
		// scene it could not author would be the one thing it cannot check.
		std::string ScenePath;

		// The authority's tick rate, in ticks per second. The server's own
		// default, because that is what a client meets in practice and the
		// mismatch with the client's 60 is a thing worth being able to see.
		double TickRate = 30.0;

		// Frames drawn per tick.
		//
		// Four is enough that "moved once per tick" and "moved every frame" are
		// far apart: judder shows as three identical frames in every four.
		int FramesPerTick = 4;

		// Ordinals to discard without telling the sender.
		//
		// **What they number depends on the arrangement, and it has to.**
		// Under `Transport::Direct` there are no datagrams, so these are
		// outgoing *message* ordinals counted across the whole run from zero.
		// Under `Transport::Lossy` they are datagram *arrival* numbers on the
		// client's end, which is what `net::LossSettings` counts and the only
		// thing a real link can lose. Under `Transport::Loopback` nothing is
		// dropped at all and these are ignored - that arrangement exists to add
		// framing and nothing else.
		//
		// **Silent in every case, which is what makes it loss** -
		// `Authority::Unsent` is a refusal the sender knows about and is
		// repaired next tick, and the interesting failure is the one nobody is
		// told about. There is no percentage here on purpose: a nominated
		// ordinal is a test and a percentage is a flake with a story attached.
		std::vector<uint64_t> Drop;

		// How the replicated world is interpolated.
		//
		// Defaulted from `TickRate` by the constructor when left at zero, so
		// that a harness told the server runs at 30 does not quietly measure
		// its delay against 60.
		engine::replication::InterpolationSettings Interpolation;

		// Workers the job system runs with.
		//
		// One, because a diagnostic that reports a different number on every
		// run is not a diagnostic. The placeholder world's motion goes through
		// `EachParallel`, so this cannot be zero.
		unsigned Workers = 1;

		// Ticks between announcements, under `Discovery::Advertised`.
		//
		// **In ticks rather than in seconds**, so a short run announces enough
		// times for the directory's count to mean something. A beacon's real
		// interval is a second and a hundred-tick run at thirty hertz would
		// announce three times, which is a sample rather than a check.
		//
		// @since v0.18
		uint32_t AnnounceEveryTicks = 4;

		// Files in the publication behind the relay, under `Content::Relayed`.
		//
		// **Enough for two things that both need volume**: one route has to be
		// several chunks on the wire, which is the case the relay's chunking
		// exists for and a single small file would skip, and there has to be
		// enough for zstd to train a dictionary on - a publication with no
		// dictionary answers `/dictionary` with a refusal, and a refusal is a
		// thinner check than a route that actually crosses.
		//
		// @since v0.18
		uint32_t ContentFiles = 8;
	};

	// What crossed in one tick, and what came out the other side.
	//
	// **Every field is a fact about one of the four stages** - produced, sent,
	// applied, drawn - because the whole point is to say which stage lost it.
	//
	// @since v0.5
	struct Report {
		// The server tick this describes.
		uint64_t Tick = 0;

		// Messages the authority produced for this client.
		size_t Messages = 0;

		// Bytes in them, before anything would have framed or sealed them.
		//
		// **Not what would go on a wire under `Transport::Direct`.** `net` adds
		// a header and a tag per datagram and that arrangement counts neither,
		// so a figure near `net::MAXIMUM_MESSAGE_BYTES` there is already a
		// message that would not fit - which is the one thing an arrangement
		// with no wire can say about the wire.
		size_t Bytes = 0;

		// The largest single message, which is the number that matters when
		// asking whether a tick would have crossed a real link.
		size_t LargestMessage = 0;

		// Messages discarded by `Settings::Drop`, or refused by the link.
		//
		// **The two are one number here and separate in `Reports`.** A
		// per-tick table is read by scanning a column for the first value that
		// stops making sense, and two columns that are each usually zero make
		// that harder rather than easier.
		size_t Dropped = 0;

		// The last tick the replica holds in full.
		//
		// Behind `Tick` by design. Equal to zero long after the join means the
		// snapshot never finished.
		uint64_t Applied = 0;

		// Entities carrying a `scene::Transform` on each side.
		//
		// **The first place a blank scene shows.** Equal counts and an empty
		// draw list is a drawing problem; unequal counts is a replication one.
		//@{
		size_t ServerEntities = 0;
		size_t ClientEntities = 0;
		//@}

		// Rows the client's draw pass produced on the last frame of this tick.
		//
		// Below `ClientEntities` means rows arrived without a `Bounds` or a
		// `Visual` - the components that are sent once in the snapshot and
		// never again, so losing them is permanent and invisible everywhere
		// else.
		size_t Drawn = 0;

		// Where the probe entity is on the server, where the client's store
		// says it is, and where it was actually drawn.
		//
		// The three differ, and each difference is meant: server against store
		// is the network's lag, and store against drawn is the snapshot
		// buffer's delay. Store equal to drawn on every frame is the judder
		// `D00010` was opened for.
		//@{
		float ServerX = 0.0f;
		float ClientX = 0.0f;
		float DrawnX = 0.0f;
		//@}

		// How far behind the newest received tick the world was drawn.
		double Behind = 0.0;

		// Frames in this tick on which the drawn position did not move.
		//
		// **The judder counter, and the number to read first.** Zero is a world
		// being interpolated. Equal to `Settings::FramesPerTick - 1` is a world
		// stepping once per tick, which is what this looks like with the
		// snapshot buffer taken out.
		int FrozenFrames = 0;

		// Content routes the client finished reassembling this tick.
		//
		// Zero in every arrangement but `Content::Relayed`.
		//
		// @since v0.18
		size_t Routes = 0;
	};

	// A server, a client, and one arrangement of everything between them.
	//
	// Build it, `Join`, then `Step` in a loop and read the reports. `Gather` is
	// the cross-module answer and `Step`'s return is the per-tick one.
	//
	// @since v0.18
	class Crossing {
	  public:
		// Builds both worlds, whatever the arrangement asks for, and admits the
		// client. Starts the job system.
		//
		// @param settings    How big, how fast, and what to drop.
		// @param arrangement What goes between the halves.
		explicit Crossing(const Settings &settings = {}, const Arrangement &arrangement = {});

		// Tears down the arrangement and stops the job system.
		~Crossing();

		Crossing(const Crossing &) = delete;
		Crossing &operator=(const Crossing &) = delete;

		// Steps until the joining snapshot has been applied.
		//
		// @param limit How many ticks to allow before giving up.
		// @return `true` once the client holds the world.
		bool Join(int limit = 512);

		// One tick: simulate, publish, carry, apply, record, draw.
		//
		// @return What crossed and what came out.
		Report Step();

		// Every module's own report, as it stands now.
		//
		// **Read after the run rather than per tick.** A `ContentRelay`'s
		// counters are a running total and a `Directory`'s table is a live
		// thing; sampling them each tick would report the same numbers over and
		// over with the last one being the only one that mattered.
		//
		// @return The reports, refreshed from every live object.
		const Reports &Gather();

		// How this crossing is wired.
		const Arrangement &Wiring() const {
			return Wired;
		}

		// The authority's world.
		engine::ecs::Store &ServerWorld() {
			return Server;
		}

		// The replicated world, with its draw list and its snapshot buffer.
		engine::ecs::Store &ClientWorld() {
			return Client;
		}

		// The entity whose position the reports follow.
		//
		// The lowest-numbered row the placeholder world made, so it is the same
		// entity across runs with the same entity count.
		engine::ecs::Entity Probe() const {
			return Probe_;
		}

		// What the client's half has seen.
		const engine::replication::Replica &Replica() const {
			return Replica_;
		}

		// What the server's half has sent.
		const engine::replication::Authority &Authority() const {
			return Authority_;
		}

		// The tick the server is on.
		uint64_t Tick() const {
			return Tick_;
		}

		// Messages handed over since the crossing was built.
		uint64_t Handed() const {
			return Handed_;
		}

		// The client's content link, or null when nothing carries content.
		//
		// @since v0.18
		const client::ContentLink *ContentLink() const {
			return Link_.get();
		}

		// The host's relay, or null when nothing carries content.
		//
		// @since v0.18
		const server::ContentRelay *ContentRelay() const {
			return Relay_.get();
		}

		// The directory collecting announcements, or null when nothing
		// announces.
		//
		// @since v0.18
		const network::Directory *Listings() const {
			return Directory_.get();
		}

	  private:
		// Builds the two worlds and the authority. Every arrangement has these.
		void BuildWorlds();

		// Builds the loopback ends and the two sessions, on whichever stack the
		// arrangement names.
		void BuildWire();

		// Stands a datagram `Session` up on each end, with real keys.
		void BuildDatagramWire();

		// Stands a `QuicSession` up on each end and runs their handshake out.
		void BuildQuicWire();

		// Publishes content, and builds the relay and the link over it.
		void BuildContent();

		// Builds the broadcast subnet, the beacon and the directory.
		void BuildDiscovery();

		// Hands this tick's messages over with nothing in between.
		void CarryDirect(Report &report);

		// Sends this tick's messages over the wire, and drains both ends.
		void CarryOverWire(Report &report);

		// Drains one transport into its session.
		void Drain(engine::net::Transport &transport, engine::replication::SessionPort &into);

		// Wraps a payload in a `User` message and queues it on a session.
		//
		// **The two calls `Connector::SendUser` makes.** A `Session` refuses a
		// payload that is not a `replication` message, because it has no
		// channel to put one on - so content that crosses a wire crosses
		// wrapped, and this is where the wrapping happens.
		//
		// @param over    The session to queue it on.
		// @param payload The bytes to carry.
		// @return Whether the link took it.
		bool SendUser(engine::replication::SessionPort &over, std::span<const std::byte> payload);

		// Routes one arrived message to the content link or to the replica.
		//
		// **The kind decides over a wire and the answer decides without one.**
		// A wire puts a `MessageKind` on the front of everything, so the route
		// is read rather than guessed; with nothing in between there is no
		// header at all, and `ContentLink::Receive` saying "this was not a
		// content message" is the only thing there is to go on.
		void TakeAtClient(const std::vector<std::byte> &message);

		// Routes one arrived message to the relay or to the authority.
		void TakeAtServer(const std::vector<std::byte> &message);

		// Asks for the next route, collects finished ones, and pumps the relay.
		//
		// @return Routes finished this tick.
		size_t PumpContent();

		// Announces if it is time to, and collects what was announced.
		void PumpDiscovery();

		// One entity's X in one store, or zero when it holds no such row.
		float PositionOf(engine::ecs::Store &store, engine::ecs::Entity entity) const;

		// One entity's X as it was actually drawn, by its ordinal in the walk
		// that filled the list.
		float DrawnPositionOf(const client::DrawList &drawList, engine::ecs::Entity entity);

		Settings Options;
		Arrangement Wired;

		engine::ecs::Store Server;
		engine::ecs::Scheduler ServerSystems;

		engine::ecs::Store Client;
		engine::ecs::Scheduler ClientSystems;

		engine::replication::Authority Authority_;
		engine::replication::Replica Replica_;
		engine::replication::ClientId Handle;

		engine::ecs::Entity Probe_;
		uint64_t Tick_ = 0;
		uint64_t Handed_ = 0;

		// The clock every timed thing is measured against, advanced by one tick
		// period per `Step` and never read from the machine.
		double Now = 0.0;

		// The loopback ends, kept alive under the lossy wrappers. Empty under
		// `Transport::Direct`.
		std::unique_ptr<engine::net::LossyTransport> ServerEnd;
		std::unique_ptr<engine::net::LossyTransport> ClientEnd;
		// Whichever session the arrangement's stack produced, behind the one
		// interface both fill. **The point of running the axis at all**: every
		// line below this pair is the same code on either stack.
		std::unique_ptr<engine::replication::SessionPort> ServerSide;
		std::unique_ptr<engine::replication::SessionPort> ClientSide;

		// The same objects as the pair above under the datagram stack, and null
		// under QUIC. What they are for is the two counters `SessionPort` does
		// not carry: `Session::Stats` and `net::Link::Stats`, which `Reports`
		// compares against each other and which have no QUIC equivalent to
		// compare.
		engine::replication::Session *ServerDatagram = nullptr;
		engine::replication::Session *ClientDatagram = nullptr;

		// Where the publication was written, removed by the destructor.
		std::string StoreRoot;
		std::string ContentRoot;
		std::unique_ptr<server::ContentRelay> Relay_;
		std::unique_ptr<client::ContentLink> Link_;

		// Which route is asked for next, cycling so the counters keep moving.
		size_t NextRoute = 0;
		bool Asking = false;

		// The broadcast subnet, the announcing end and the listening end.
		std::vector<std::unique_ptr<engine::net::Transport>> Subnet;
		std::unique_ptr<network::Beacon> Beacon_;
		std::unique_ptr<network::Directory> Directory_;

		// Everything gathered so far, refreshed by `Gather`.
		Reports Tally;
	};
}
