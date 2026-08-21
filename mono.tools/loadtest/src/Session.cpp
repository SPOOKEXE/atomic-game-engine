#include <engine/core/Clock.hpp>
#include <engine/core/Log.hpp>
#include <engine/game/Play.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/script/Instances.hpp>

#include <cmath>
#include <loadtest/Session.hpp>

namespace loadtest {

	const char *Describe(Stage stage) {
		switch (stage) {
		case Stage::Dialling:
			return "dialling";
		case Stage::Streaming:
			return "streaming";
		case Stage::Playing:
			return "playing";
		case Stage::Refused:
			return "refused";
		case Stage::TimedOut:
			return "timed out";
		}
		return "?";
	}

	bool Terminal(Stage stage) {
		return stage == Stage::Refused || stage == Stage::TimedOut;
	}

	Stage NextStage(Stage current, const Progress &progress, double waitedSeconds, double deadlineSeconds) {
		if (Terminal(current)) {
			return current;
		}

		// Refusal first. A connector that was refused after being admitted is a
		// server that dropped this client, and reading the flags in the other
		// order would leave it looking as though it were still playing.
		if (progress.Rejected) {
			return Stage::Refused;
		}
		if (progress.Joined) {
			return Stage::Playing;
		}
		if (progress.Admitted) {
			return Stage::Streaming;
		}

		// Only a session that has not moved is written off, and only when a
		// deadline was asked for. A run being debugged passes zero.
		if (deadlineSeconds > 0.0 && waitedSeconds > deadlineSeconds) {
			return Stage::TimedOut;
		}

		return current;
	}

	void RegisterReplicaTypes() {
		engine::scene::RegisterSceneComponents();

		// **`scene`'s classes, for the reason `client::BuildReplicatedWorld`
		// gives at length.** Without them every instance a snapshot names
		// arrives untyped, `ecs.InstanceClass` crosses as an empty name, and the
		// audit disputes every group it looks at - which re-arms the recovery
		// walk for ever. This harness is what measured that: 91,507 B/s at rest
		// before, 16,964 after.
		engine::scene::RegisterSceneClasses();

		// Both are idempotent and both register their components before their
		// classes, which is why the return values are discarded here as they are
		// in `client::BuildReplicatedWorld`.
		(void)engine::gui::RegisterGuiClasses();
		(void)engine::script::ScriptClass();
	}

	Session::Session(const SessionSettings &settings, double nowSeconds)
		: Settings(settings), Store_("replica"), StartedAt(nowSeconds), StageChangedAt(nowSeconds) {
		// An ephemeral port per session, because a server on a datagram socket
		// tells its clients apart by address. Two hundred sessions sharing one
		// socket would be one client to the server, which is the shape this
		// harness exists not to measure.
		Wire = engine::net::MakeUdpTransport(0);
		if (Wire == nullptr) {
			Stage_ = Stage::Refused;
			return;
		}

		Link = std::make_unique<engine::replication::Connector>(*Wire, Settings.Server, nowSeconds);

		// What `client::Client` does with the same message. The player is the one
		// fact a replica cannot derive, and the resource is what the input path
		// reads to find out whether this client has a character yet.
		Link->OnUserMessage([this](std::span<const std::byte> message) {
			engine::game::JoinNotice notice;
			if (engine::game::DecodeJoinNotice(message, notice)) {
				Mine = notice.Player;
				Store_.SetResource(engine::scene::LocalPlayer{notice.Player});
			}
		});
	}

	Session::~Session() {
		// The connector before the socket it borrows. A link outliving its
		// transport is a dangling reference in a destructor.
		Link.reset();
		if (Wire != nullptr) {
			Wire->Close();
		}
	}

	void Session::Tick(double nowSeconds, uint64_t tick) {
		if (Link == nullptr || Terminal(Stage_)) {
			return;
		}

		// **Nothing but `Poll` announces this client.** A server on a datagram
		// socket cannot stream to an address it has never heard from, so the
		// first thing a client does is speak - and `Poll` is what does it.
		const uint64_t before = engine::core::Clock::Nanoseconds();
		Link->Poll(Store_, nowSeconds);
		ApplyMicroseconds += static_cast<double>(engine::core::Clock::Nanoseconds() - before) / 1000.0;
		Polls++;

		const Progress progress{
			.Rejected = Link->Rejected(),
			.Admitted = Link->Admitted(),
			.Joined = Link->Joined(),
		};

		const Stage moved = NextStage(Stage_, progress, nowSeconds - StageChangedAt, Settings.StallSeconds);
		if (moved != Stage_) {
			Stage_ = moved;
			StageChangedAt = nowSeconds;
			if (moved == Stage::Streaming && AdmittedAt == 0.0) {
				AdmittedAt = nowSeconds;
			}
			if (moved == Stage::Playing) {
				// A join that arrives inside one `Poll` never passes through
				// `Streaming`, so the admission time is taken here too rather
				// than left at zero for the fastest sessions.
				if (AdmittedAt == 0.0) {
					AdmittedAt = nowSeconds;
				}
				JoinedAt = nowSeconds;
			}
		}

		if (Stage_ == Stage::Playing && Settings.InputEveryTicks > 0 &&
			tick % Settings.InputEveryTicks == 0) {
			Submit(nowSeconds);
		}

		Link->Advance(nowSeconds);

		// The forgotten list is the server saying "stop drawing these". Nothing
		// here draws, but leaving it to grow would be a leak in the one
		// structure a replica keeps outside the store.
		Link->ClearForgotten();
	}

	void Session::Submit(double nowSeconds) {
		// Nothing to move until the server has said which player is ours and put
		// a character under it. Sending before that is an input the host drops,
		// which would make the refusal counters mean two different things.
		if (Mine == engine::ecs::NULL_ENTITY ||
			engine::scene::CharacterOf(Store_, Mine) == engine::ecs::NULL_ENTITY) {
			return;
		}

		engine::game::MoveInput move;
		move.Direction =
			engine::core::Vector3{std::cos(Settings.HeadingRadians), 0.0f, std::sin(Settings.HeadingRadians)};

		// The tick this client last applied, which is what the server rewinds
		// against. `Applied` and not a local count: a client's clock is the
		// server's, seen late.
		if (Link->Submit(Link->Applied(), engine::game::EncodeMoveInput(move), nowSeconds)) {
			InputsSent++;
		} else {
			InputsRefused++;
		}
	}

	SessionReport Session::Report() {
		SessionReport report;
		report.Final = Stage_;
		report.InputsSent = InputsSent;
		report.InputsRefused = InputsRefused;
		report.ApplyMicroseconds = ApplyMicroseconds;
		report.Polls = Polls;

		if (AdmittedAt > 0.0) {
			report.AdmitSeconds = AdmittedAt - StartedAt;
		}
		if (JoinedAt > 0.0) {
			report.JoinSeconds = JoinedAt - StartedAt;
		}

		if (Link != nullptr) {
			const engine::net::ConnectionStats &stats = Link->Link().Stats();
			report.BytesSent = stats.BytesSent;
			report.BytesReceived = stats.BytesReceived;
			report.PacketsSent = stats.PacketsSent;
			report.PacketsReceived = stats.PacketsReceived;
			report.PacketsLost = stats.PacketsLost;

			report.Applied = Link->Applied();
			report.Deltas = Link->ReplicaStats().Deltas;
			report.Incomplete = Link->ReplicaStats().Incomplete;
			report.Refusals = Link->Stats().Refused;
		}

		Store_.EachEntity([&report](engine::ecs::Entity) { report.Entities++; });
		return report;
	}
}
