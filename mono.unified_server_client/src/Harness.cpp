#include <engine/core/Log.hpp>
#include <engine/core/Name.hpp>
#include <engine/examples/Scene.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/replication/Defaults.hpp>
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
		// Jobs is process-wide and this diagnostic owns it only for its last instance.
		int LiveHarnesses = 0;
	}

	Harness::Harness(const Settings &settings)
		: Options(settings), Server("unified.server"), Client("unified.client") {
		// One worker keeps diagnostic runs deterministic.
		if (LiveHarnesses == 0) {
			engine::parallel::Jobs::Start(Options.Workers);
		}
		LiveHarnesses++;

		// Both sides register the same names independently.
		server::RegisterPlaceholderComponents();
		if (Options.ScenePath.empty()) {
			server::BuildPlaceholderWorld(Server, ServerSystems, Options.Entities);
		} else {
			// Load only the server half; the client must receive its state.
			std::string error;
			if (!engine::examples::LoadScene(Server, ServerSystems, Options.ScenePath, error)) {
				ENGINE_ERROR("--scene '{}' failed:\n{}", Options.ScenePath, error);
				throw std::runtime_error("the scene script failed");
			}
		}

		// Observe per-tick components; signed components use snapshot signatures.
		Server.Observe<Transform>();
		Server.Observe<engine::scene::Motion>();

		if (Options.Interpolation.TickRate <= 0.0) {
			Options.Interpolation.TickRate = Options.TickRate;
		}
		client::BuildReplicatedWorld(Client, ClientSystems, Options.Interpolation);

		for (const engine::replication::ReplicatedComponent &component :
			 engine::replication::DefaultReplicatedComponents()) {
			Authority_.Replicate(engine::core::Name(component.Name), component.Detection);
		}

		Handle = Authority_.Admit();

		// Scan for the lowest row instead of assuming entity allocation order.
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
				const engine::scene::Visual &visual) {
				if (found) {
					return;
				}

				// **The same skip `CollectReplicated` makes, and leaving it out
				// was silently wrong.** This walk exists to turn an entity into
				// its ordinal in the draw list, which means it has to match the
				// walk that *built* the list row for row. That one drops an
				// invisible instance; this one counted it, so one hidden part
				// anywhere ahead of the subject shifted every ordinal after it
				// and this reported a different entity's position — as a plain
				// number, with nothing to say it was the wrong entity's.
				if (!visual.Visible) {
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

		// Clear before the tick so the delta sees this tick's changes.
		Server.ClearChanges();
		Server.AdvanceTick(static_cast<float>(1.0 / Options.TickRate));
		ServerSystems.RunPhases(Server, Phase::PreSimulation, Phase::PostSimulation);
		Server.FlushSignals();

		Tick_ = Server.Time().Tick;
		report.Tick = Tick_;

		Authority_.Publish(Server, Tick_);
		const std::span<const std::vector<std::byte>> messages = Authority_.Outgoing(Handle);
		report.Messages = messages.size();

		// Direct handoff: this harness excludes transport framing and timing.
		for (const std::vector<std::byte> &message : messages) {
			report.Bytes += message.size();
			report.LargestMessage = std::max(report.LargestMessage, message.size());

			const uint64_t ordinal = Handed_++;
			if (std::find(Options.Drop.begin(), Options.Drop.end(), ordinal) != Options.Drop.end()) {
				// Deliberate loss is invisible to the authority.
				report.Dropped++;
				continue;
			}

			Replica_.Receive(Client, message);
		}

		Server.ClearChanges();

		const std::vector<std::byte> acknowledgement = Replica_.Acknowledge();
		if (!acknowledgement.empty()) {
			Authority_.Receive(Handle, acknowledgement);
		}

		report.Applied = Replica_.Applied();

		// Record on receipt, before presentation can miss a tick.
		client::RecordReplicatedTick(Client, report.Applied);

		const auto *buffer = Client.Resource<engine::replication::SnapshotBuffer>();

		float previousDrawn = 0.0f;
		bool haveDrawn = false;
		for (int frame = 0; frame < Options.FramesPerTick; frame++) {
			Client.SetFrame(static_cast<float>(1.0 / (Options.TickRate * Options.FramesPerTick)), 0.0f);
			ClientSystems.RunPhases(Client, Phase::PreRender, Phase::PreRender);

			const auto *drawList = Client.Resource<client::DrawList>();
			report.Drawn = drawList == nullptr ? 0 : drawList->Instances.size();

			// DrawList has no entity id; resolve the probe by traversal ordinal.
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
