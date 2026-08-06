#include <engine/core/Log.hpp>
#include <engine/core/Name.hpp>
#include <engine/examples/Scene.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/scene/Components.hpp>

#include <algorithm>
#include <client/Replicated.hpp>
#include <client/Scene.hpp>
#include <cmath>
#include <server/Simulation.hpp>
#include <stdexcept>
#include <unified/Harness.hpp>

namespace unified {

	using engine::ecs::Entity;
	using engine::ecs::Phase;
	using engine::ecs::Store;
	using engine::scene::Transform;

	namespace {
		// How many harnesses are alive.
		//
		// **`Jobs` is process-wide and `Stop` is not reference counted**, so two
		// harnesses in one process — which the determinism case deliberately
		// builds — would have the first destructor stop the pool the second is
		// still running on. Counting here rather than asking `Jobs`, because
		// "somebody else already started it" and "I started it" want different
		// answers on the way out and only this class knows which it was.
		//
		// Not atomic: a harness is a diagnostic driven from one thread, and the
		// two places this is touched are a constructor and a destructor on that
		// thread.
		int LiveHarnesses = 0;

		// The four the placeholder world replicates, and the same four
		// `mono.server/src/Server.cpp` names.
		//
		// **Written out here rather than shared with the server, and that is
		// the point of the harness.** If these two lists ever disagree this
		// program says so on its first tick — a component the server replicates
		// and this does not comes out as rows arriving with no `Bounds` and
		// nothing being drawn, which is exactly the symptom that sent somebody
		// looking at the network. Sharing the list would hide the class of bug
		// this exists to find.
		// **What crosses, and how each one's changes are noticed.** The two
		// detectors are not interchangeable and the pairing is the thing worth
		// stating: a transform moves every tick and its dirty bit is free, and
		// a size or a colour moves once in a session and has no bit at all
		// because nothing observes it. See `replication::ChangeDetection`.
		struct Replicated {
			const char *Name;
			engine::replication::ChangeDetection Detection;
		};

		const Replicated REPLICATED[] = {
			{"scene.Transform", engine::replication::ChangeDetection::Observed},
			{"scene.Motion", engine::replication::ChangeDetection::Observed},
			{"scene.Bounds", engine::replication::ChangeDetection::Signature},
			{"scene.Visual", engine::replication::ChangeDetection::Signature},

			// v0.9's two. See `mono.server/src/Server.cpp` for the whole
			// argument; the short form is that a mesh name without a texture
			// name is half a model, and that a replica's `TagTable` stays empty
			// because resources have no wire form.
			{"scene.SurfaceAppearance", engine::replication::ChangeDetection::Signature},
			{"scene.Tags", engine::replication::ChangeDetection::Signature},
		};
	}

	Harness::Harness(const Settings &settings)
		: Options(settings), Server("unified.server"), Client("unified.client") {
		// The placeholder world's motion runs through `EachParallel`, so there
		// has to be a pool for it to run on. One worker, so two runs of this
		// program agree.
		if (LiveHarnesses == 0) {
			engine::parallel::Jobs::Start(Options.Workers);
		}
		LiveHarnesses++;

		// **Both sides register the same set, and neither registers the
		// other's.** `server::RegisterPlaceholderComponents` is the server
		// program's call and `BuildReplicatedWorld` makes the client's; a
		// snapshot crosses because both name `scene.Transform` the same way,
		// not because they share a header.
		server::RegisterPlaceholderComponents();
		if (Options.ScenePath.empty()) {
			server::BuildPlaceholderWorld(Server, ServerSystems, Options.Entities);
		} else {
			// The same loader, the same file, no camera and no draw list — a
			// server's half. What this harness checks is that the client ends
			// up holding what the server authored, and a scene it could not
			// author is the one case that would go unchecked.
			std::string error;
			if (!engine::examples::LoadScene(Server, ServerSystems, Options.ScenePath, error)) {
				ENGINE_ERROR("--scene '{}' failed:\n{}", Options.ScenePath, error);
				throw std::runtime_error("the scene script failed");
			}
		}

		// The dirty bits a delta is built from. `Bounds` and `Visual` are
		// deliberately not observed, for the reason `Server.cpp` gives: nothing
		// writes them per tick, so a dirty column for them is paid for every
		// tick and read never. They are signed instead — see `REPLICATED` — so
		// a write to one still crosses rather than being lost until the next
		// snapshot.
		Server.Observe<Transform>();
		Server.Observe<engine::scene::Motion>();

		if (Options.Interpolation.TickRate <= 0.0) {
			Options.Interpolation.TickRate = Options.TickRate;
		}
		client::BuildReplicatedWorld(Client, ClientSystems, Options.Interpolation);

		for (const Replicated &component : REPLICATED) {
			Authority_.Replicate(engine::core::Name(component.Name), component.Detection);
		}

		Handle = Authority_.Admit();

		// The lowest-numbered row the world made, so the probe is the same
		// entity between two runs at the same entity count. Taken by scanning
		// rather than by assuming `Create` starts at one, because the id layout
		// is the store's business — `ecs/Entity.hpp` says so in those words.
		Server.Each<const Transform>([this](Entity entity, const Transform &) {
			if (Probe_ == engine::ecs::NULL_ENTITY || entity.Id < Probe_.Id) {
				Probe_ = entity;
			}
		});
	}

	Harness::~Harness() {
		LiveHarnesses--;
		if (LiveHarnesses == 0) {
			engine::parallel::Jobs::Stop();
		}
	}

	float Harness::PositionOf(Store &store, Entity entity) const {
		const Transform *transform = store.Get<Transform>(entity);
		return transform == nullptr ? 0.0f : transform->Frame.Position.X;
	}

	float Harness::DrawnPositionOf(const client::DrawList &drawList, Entity entity) {
		size_t ordinal = 0;
		bool found = false;

		Client.Each<const Transform, const engine::scene::Bounds, const engine::scene::Visual>(
			[&](Entity candidate,
				const Transform &,
				const engine::scene::Bounds &,
				const engine::scene::Visual &) {
				if (found) {
					return;
				}
				if (candidate == entity) {
					found = true;
					return;
				}
				ordinal++;
			}
		);

		if (!found || ordinal >= drawList.Instances.size()) {
			return 0.0f;
		}
		return drawList.Instances[ordinal].Frame.Position.X;
	}

	bool Harness::Join(int limit) {
		for (int attempt = 0; attempt < limit && !Replica_.Joined(); attempt++) {
			Step();
		}
		return Replica_.Joined();
	}

	Report Harness::Step() {
		Report report;

		// ---- Simulate -------------------------------------------------------
		//
		// The order `world::World::Tick` uses, and it matters: the change bits
		// are cleared at the *start* of a tick so that what this tick recorded
		// is still there when the delta is built out of it below.
		Server.ClearChanges();
		Server.AdvanceTick(static_cast<float>(1.0 / Options.TickRate));
		ServerSystems.RunPhases(Server, Phase::PreSimulation, Phase::PostSimulation);
		Server.FlushSignals();

		Tick_ = Server.Time().Tick;
		report.Tick = Tick_;

		// ---- Serialise ------------------------------------------------------
		Authority_.Publish(Server, Tick_);
		const std::span<const std::vector<std::byte>> messages = Authority_.Outgoing(Handle);
		report.Messages = messages.size();

		// ---- Deserialise, with nothing in between ---------------------------
		//
		// **This is the whole harness.** No datagram, no header, no cipher, no
		// window: the bytes the authority produced are the bytes the replica
		// reads, in the order they were produced. A message dropped here is
		// dropped because a caller nominated it.
		for (const std::vector<std::byte> &message : messages) {
			report.Bytes += message.size();
			report.LargestMessage = std::max(report.LargestMessage, message.size());

			const uint64_t ordinal = Handed_++;
			if (std::find(Options.Drop.begin(), Options.Drop.end(), ordinal) != Options.Drop.end()) {
				// Silently, and the authority is not told. A refusal it knows
				// about is `Unsent` and is repaired next tick; loss is the one
				// nobody reports.
				report.Dropped++;
				continue;
			}

			Replica_.Receive(Client, message);
		}

		Server.ClearChanges();

		// The acknowledgement, which is what stops the authority resending the
		// snapshot for ever. Straight back, same seam.
		const std::vector<std::byte> acknowledgement = Replica_.Acknowledge();
		if (!acknowledgement.empty()) {
			Authority_.Receive(Handle, acknowledgement);
		}

		report.Applied = Replica_.Applied();

		// ---- Record and draw ------------------------------------------------
		//
		// Recorded once per tick, at the point the store holds what the server
		// described — the same place `Client::PollServer` does it, and not in
		// the render pass, where a frame rate below the tick rate would miss a
		// received tick entirely.
		client::RecordReplicatedTick(Client, report.Applied);

		const auto *buffer = Client.Resource<engine::replication::SnapshotBuffer>();

		float previousDrawn = 0.0f;
		bool haveDrawn = false;
		for (int frame = 0; frame < Options.FramesPerTick; frame++) {
			Client.SetFrame(static_cast<float>(1.0 / (Options.TickRate * Options.FramesPerTick)), 0.0f);
			ClientSystems.RunPhases(Client, Phase::PreRender, Phase::PreRender);

			const auto *drawList = Client.Resource<client::DrawList>();
			report.Drawn = drawList == nullptr ? 0 : drawList->Instances.size();

			// The probe's drawn position.
			//
			// **A `DrawInstance` carries no entity**, deliberately — it is what
			// a renderer uploads and an entity id would be along for the ride.
			// So the probe is found by its *ordinal* in the same walk that
			// filled the list: `CollectReplicated` pushes one row per match of
			// `Each<Transform, Bounds, Visual>` in that walk's order, so the
			// n-th match is the n-th instance. Repeating the walk here rather
			// than assuming the probe is first, because the store decides the
			// order and nothing promises it is entity order.
			if (drawList != nullptr) {
				report.DrawnX = DrawnPositionOf(*drawList, Probe_);
			}

			if (haveDrawn && std::abs(report.DrawnX - previousDrawn) < 1e-5f) {
				report.FrozenFrames++;
			}
			previousDrawn = report.DrawnX;
			haveDrawn = true;
		}

		report.ServerEntities = Server.CountMatching<Transform>();
		report.ClientEntities = Client.CountMatching<Transform>();
		report.ServerX = PositionOf(Server, Probe_);
		report.ClientX = PositionOf(Client, Probe_);
		report.Behind = buffer == nullptr ? 0.0 : buffer->Behind();

		return report;
	}
}
