#include <client/Replicated.hpp>
#include <engine/core/Log.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/scene/Components.hpp>
#include <engine/world/Postbox.hpp>
#include <engine/world/Universe.hpp>
#include <studio/PlayLink.hpp>

#include <algorithm>
#include <string>

namespace studio {

	namespace {
		using engine::core::Name;
		using engine::ecs::Store;
		using engine::replication::ChangeDetection;

		// What crosses, and how the authority notices it moved.
		//
		// **The same four as `mono.unified_server_client` and `mono.server`, and
		// the pairing is not arbitrary.** A `Transform` is written every tick by
		// a system, so the dirty bits already know and the hash would be a pass
		// over the world to learn it again. `Bounds` and `Visual` are written
		// once by a script and then never — observing them buys a dirty column
		// paid for every tick and read never, and *not* signing them is how a
		// part recoloured by a script kept its old colour on every client until
		// something happened to re-snapshot it.
		//
		// **This is the third copy, and that is filed rather than excused —
		// `D00018`.** `mono.server` and `mono.unified_server_client` each carry
		// the same four, and all three agree today. Three copies of a policy is
		// three chances for one to drift, which is the failure this repository
		// has already paid for more than once.
		//
		// It is written here anyway because there is nowhere correct to put it
		// yet, and the constraint is a layer rather than an opinion: the table
		// pairs a component name with a `replication::ChangeDetection`, and
		// `replication` is L12 while `scene` is L7 — so `scene` cannot name the
		// enum, and `replication` naming `scene`'s components would couple the
		// generic module to one component module. The real answer is a game file
		// declaring its own replicated set, which is what the entry is against.
		struct Replicated {
			const char *Component;
			ChangeDetection Detection;
		};

		const Replicated REPLICATED[] = {
			{"scene.Transform", ChangeDetection::Observed},
			{"scene.Motion", ChangeDetection::Observed},
			{"scene.Bounds", ChangeDetection::Signature},
			{"scene.Visual", ChangeDetection::Signature},

			// **The mirror, and the tree that says what it is a mirror of.** A
			// `SurfaceCamera` names a face; which part's face it is comes from
			// the parent link and nowhere else, so replicating the camera
			// without `ecs.Hierarchy` puts a camera on the client that cannot
			// find its pane. That is what a replica's mirrors being plain white
			// parts was: `client::CollectSurfaceViews` found nothing to render
			// because `AimSurfaceCameras` had no parent to project off.
			//
			// **Aim is not among them and must not be.** `scene.Transform` is
			// replicated, so the authority's placement for a surface camera does
			// arrive — and `aim-surface-cameras` overwrites it in `PreRender`
			// from this client's own eye, every frame, before anything reads it.
			// A reflection is of the viewer, and the viewer is here.
			//
			// `Signature`, because none of the three changes on a steady mirror:
			// a face, a lens and a parent are authored once and then sit still,
			// and observing them would pay a comparison per mirror per tick to
			// discover that.
			{"scene.SurfaceCamera", ChangeDetection::Signature},
			{"scene.Camera", ChangeDetection::Signature},
			{"ecs.Hierarchy", ChangeDetection::Signature},
		};

		// Entities carrying a pose, which is what "how much world is there"
		// means on both sides of the link.
		size_t PosedEntities(Store &store) {
			size_t count = 0;
			store.Each<const engine::scene::Transform>([&count](engine::ecs::Entity, const engine::scene::Transform &) {
				count++;
			});
			return count;
		}
	}

	bool PlayLink::Start(
		engine::world::Universe &universe,
		engine::world::WorldId authority,
		double tickRate,
		std::string &error,
		std::string_view label
	) {
		if (IsRunning()) {
			error = "this link is already running";
			return false;
		}

		if (!authority.IsValid() || universe.IsRemote(authority)) {
			error = "there is no local world to be the authority";
			return false;
		}

		const Name authorityName = universe.NameOf(authority);

		engine::world::WorldSettings settings;
		settings.Name = Name(std::string(authorityName.Text()) + " (" + std::string(label) + ")");

		// **The authority's rate, not the display's.** The replica never
		// simulates, but its interpolation delay is counted in ticks, and a
		// world whose nominal rate disagreed with the one producing its
		// snapshots would interpolate across a gap the link never produced.
		settings.TickRate = tickRate;
		settings.IdleTickRate = tickRate;

		engine::world::WorldStatus status = engine::world::WorldStatus::Ok;
		const engine::world::WorldId replica = universe.Create(settings, &status);
		if (!replica.IsValid()) {
			error = "could not create the client view: " + std::string(Describe(status));
			return false;
		}

		engine::replication::InterpolationSettings interpolation;
		interpolation.TickRate = tickRate;

		universe.Enter(replica, [&interpolation](Store &store, engine::ecs::Scheduler &systems) {
			// The presentation seam, reused rather than reimplemented: a draw
			// list, a snapshot buffer and one `PreRender` system. **A replica
			// registers no simulation system at all** — one that ticked would be
			// this process disagreeing with an authority it can see.
			client::BuildReplicatedWorld(store, systems, interpolation);

			// **Marked in both of the places v0.6 settled, because they are two
			// halves of one fact.** `world::Replica` refuses the bus writes at
			// the call; `SetAdoptOnly` refuses minting an entity in the storage.
			// A replica that could mint one would allocate the same index the
			// authority is about to allocate, and `Store::Apply` would be right
			// to merge two different things into one.
			store.SetResource(engine::world::Replica{});
			store.SetAdoptOnly(true);
		});

		for (const Replicated &component : REPLICATED) {
			Server.Replicate(Name(component.Component), component.Detection);
		}

		Handle = Server.Admit();

		Authority_ = authority;
		Replica_ = replica;
		Last = LinkReport{};

		ENGINE_INFO(
			"play: '{}' is served to '{}' in this process", authorityName.Text(), settings.Name.Text()
		);
		return true;
	}

	void PlayLink::Step(engine::world::Universe &universe) {
		if (!IsRunning()) {
			return;
		}

		ENGINE_PROFILE("play link");

		LinkReport report;
		report.TotalMessages = Last.TotalMessages;
		report.TotalBytes = Last.TotalBytes;

		// ---- what the authority has to say ----------------------------------
		//
		// Inside `Enter`, because a store is only reachable through it — and the
		// publish has to happen here rather than after, since the span it hands
		// back is the authority's own and the messages are read below.
		universe.Enter(Authority_, [this, &report](Store &store) {
			report.Tick = store.Time().Tick;
			report.ServerEntities = PosedEntities(store);

			Server.Publish(store, report.Tick);
		});

		const std::span<const std::vector<std::byte>> messages = Server.Outgoing(Handle);
		report.Messages = messages.size();

		// ---- and the replica reads exactly that -----------------------------
		//
		// **No wire, and that is the arrangement rather than a shortcut.** The
		// bytes the authority produced are the bytes the replica reads, in the
		// order they were produced. What this catches is the class of bug a
		// perfect link cannot hide: state that is never sent at all.
		universe.Enter(Replica_, [this, &report, messages](Store &store) {
			for (const std::vector<std::byte> &message : messages) {
				report.Bytes += message.size();
				report.LargestMessage = std::max(report.LargestMessage, message.size());

				Client.Receive(store, message);
			}

			report.Applied = Client.Applied();
			report.ClientEntities = PosedEntities(store);

			// Recorded at the instant the store holds the tick the authority
			// described, which is the same place `Client::PollServer` and the
			// unified harness both do it — and deliberately not in a render
			// pass, where a frame rate below the tick rate would miss a received
			// tick entirely and leave the buffer interpolating across a gap the
			// link never produced.
			client::RecordReplicatedTick(store, report.Applied);
		});

		// The acknowledgement, which is what stops the authority resending the
		// join snapshot for ever. Straight back, same seam.
		const std::vector<std::byte> acknowledgement = Client.Acknowledge();
		if (!acknowledgement.empty()) {
			Server.Receive(Handle, acknowledgement);
		}

		report.TotalMessages += report.Messages;
		report.TotalBytes += report.Bytes;
		Last = report;
	}

	void PlayLink::Stop(engine::world::Universe &universe) {
		if (!IsRunning()) {
			return;
		}

		// **The world goes and the handle is cleared before anything else can
		// ask.** `Editor::IsReplicaWorld` is consulted by the worlds panel, the
		// lifecycle and the save path, and a link naming a world that has been
		// destroyed would have each of them answering about a hole.
		const engine::world::WorldId replica = Replica_;
		Replica_ = engine::world::WorldId{};
		Authority_ = engine::world::WorldId{};
		Last = LinkReport{};

		universe.Destroy(replica);

		// Rebuilt on the next `Start` rather than reset here: an `Authority`
		// holds per-client history and a `Replica` holds a partially received
		// snapshot, and neither means anything once the world it described is
		// gone.
		Server = engine::replication::Authority{};
		Client = engine::replication::Replica{};
		Handle = engine::replication::ClientId{};
	}
}
