#pragma once

// What this client does with a world it does not own.
//
// **This file used to be the duplication and is now the opposite of it.** It
// declared a `ReplicatedPosition` and a `ReplicatedVelocity` registered under
// `server.Position` and `server.Velocity`, because a snapshot travels by
// component *name* and the two programs shared no component set - so the client
// had to declare the server's types a second time and keep the layouts in step
// by hand. `mono.engine/scene` at L7 ended that: both programs register the same
// `scene` components under the same names, and a snapshot and a delta cross with
// no translation layer at all.
//
// What is left is the half that is genuinely the client's own: a replicated
// world arrives as rows and has to be *drawn*, and nothing about drawing it
// crosses the wire. That is presentation over somebody else's simulation, which
// is neither the demo scene nor the engine's business, so it lives here.
//
// **It is interpolated, and the buffer that does it is not in this file.**
// `engine::replication::SnapshotBuffer` holds the received ticks and decides
// where along them the world is drawn, because the decision is about the link
// rather than about drawing. What is here is the two halves that need this
// process's own knowledge: which component carries a pose, and when the store
// holds a tick in full.
//
// **A replica simulates nothing, and running a script is not simulating.** No
// system here advances the world: the `PreRender` ones derive what to draw and
// the two `PreSimulation` ones adopt what arrived. A replica that *ticked* the
// world would be this process disagreeing with the authority once per tick,
// which is the bug replication exists to avoid.
//
// **Nor is dead-reckoning a body the buffer has run out of ticks for.** When
// nothing has arrived to interpolate toward, a row carrying a `scene::Motion`
// and no `scene::NetworkOwner` is drawn where that velocity says it would be -
// bounded in time by `replication::InterpolationSettings::ExtrapolateSeconds`
// and in distance by the body's own size, through `physics::Advanced` and into
// a `DrawInstance`. No system runs, no phase is added and no component is
// written, which is why it advances no world. `replication/AGENTS.md` carries
// the invariant this was amended out of and the reasons it distinguishes a body
// from a player.
//
// ## What a script running here may write, and what refuses the rest
//
// **The mechanism is `ecs::Store`'s and this file adds none.** A replica's rows
// belong to the authority, and two calls in the store say so - property writes
// to authoritative instances are refused by `Store::SetProperty`, and every attempt
// to mint an authoritative entity is refused by the same flag through
// `Store::Create`, `CreateInstance` and `CloneInstance`. A property may explicitly
// opt into runtime writes on live predicted instances through `PredictedWritable`.
// `CameraSubject` uses this for the client's own camera; its target may be an
// authoritative Humanoid, but assigning the selection does not modify that target.
//
// So a `LocalScript` here **reads, connects and calls**. It cannot set an
// authoritative property or create an authoritative instance.
// What it can write is what is not a row the authority owns: an attribute, a
// world resource, its own upvalues - and the client-only surfaces the engine
// hands it, `UserInputService` and the interface it is shown.
//
// **A refusal is visible to the author rather than silent, which is the half
// that matters.** `LuauInstances.cpp` raises on a refused write with a message
// naming the world as a replica and telling the author to test
// `RunService:IsServer()` first, because a script author cannot tell a write
// that was rejected from one that was applied and overwritten by the next delta.
//
// **A script here cannot see what the server did not send.** There is no second
// store and no back channel: the VM is opened over *this* store - one runtime,
// one world, `script/AGENTS.md` - and this store holds exactly what
// `replication::Replica::Apply` put in it. Interest is decided on the authority
// by `Authority::SetInterest` over `scene::VisibleToClients` and
// `scene::PlayerOwning`, so a row this client was not sent is not a row it can
// name, and `game:GetService` finds nothing under a container that never
// arrived.

#include <engine/core/types/CFrame.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/game/Play.hpp>
#include <engine/game/PortalSession.hpp>
#include <engine/replication/Protocol.hpp>
#include <engine/replication/SnapshotBuffer.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Controls.hpp>
#include <engine/script/Runtime.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <span>

namespace client {
	// Register transient prediction resources before the client seals its
	// component table. Local scripted worlds can inspect the same resources as
	// replicated worlds during capture.
	void RegisterClientPredictionComponents();

	// Client-local state for the one character a connection may predict.
	//
	// Ordinary replicas replay unconfirmed input from each received authority pose.
	// A retained portal rig instead uses completed destination motion and its history.
	// It is a render overlay, not a second entity or a replica physics pass.
	struct LocalPlayerPrediction {
		// Player entity associated with this record.
		engine::ecs::Entity Player;
		// Root entity associated with this record.
		engine::ecs::Entity Root;
		// Coordinate frame associated with this record.
		engine::core::CFrame Frame;
		// Linear velocity associated with this record.
		engine::core::Vector3 Linear;
		// Angular velocity associated with this record.
		engine::core::Vector3 Angular;
		// Humanoid state associated with this record.
		engine::scene::Humanoid Humanoid;
		// Latest authoritative tick represented by this state.
		uint64_t AuthorityTick = 0;
		// Keeps fractional input time continuous when adoption changes local clocks.
		double PresentationOffsetSeconds = 0;
		// Shared body/camera correction, never replayed or written to authority rows.
		engine::core::Vector3 PositionCorrection;
		// Duration over which the presentation correction is blended.
		float CorrectionSeconds = 0;
		// Whether this state is currently active.
		bool Active = false;
	};

	// Portal Prediction Input declaration.
	struct PortalPredictionInput {
		// Simulation tick associated with this record.
		uint64_t Tick = 0;
		// Input command replayed by the client prediction.
		engine::game::MoveInput Move;
		// Simulation step duration for this input.
		float Delta = 0;
	};

	// Maps acknowledged input duration onto completed destination time.
	struct PredictionReplayClock {
		// Accumulated simulation time represented by the replay clock.
		double SimulationSeconds = 0;
		// Unacknowledged input duration represented by the replay clock.
		double InputLeadSeconds = 0;
		// Simulation tick for input.
		uint64_t InputTick = 0;
	};

	// Only submitted moves belong here; source consumption does not retire them.
	struct PortalInputHistory {
		// Maximum number of retained entries.
		static constexpr size_t CAPACITY = 1024;
		// Portal resume claim that owns this history.
		engine::game::PortalResume Claim;
		// Portal seam transform used to carry prediction state.
		engine::scene::SeamTransform Through;
		// Inputs retained for reconciliation.
		std::array<PortalPredictionInput, CAPACITY> Inputs{};
		// Ring-buffer index of the first retained entry.
		size_t Begin = 0;
		// Number of entries currently retained.
		size_t Count = 0;
		// Last input tick covered by authoritative motion.
		uint64_t CoveredThrough = 0;
		// Latest input tick retained in the history.
		uint64_t LastRecordedTick = 0;
		// Destination motion tick already applied locally.
		uint64_t AppliedDestinationTick = 0;
		// Input tick already applied locally.
		uint64_t AppliedInputTick = 0;
		// Number of inputs discarded during reconciliation.
		uint64_t DiscardedInputs = 0;
		// Replay clock carried with the prediction state.
		PredictionReplayClock Clock;
	};

	// Starts a predicted-input history at the portal handoff's submitted tick.
	bool BeginPortalInputHistory(
		engine::ecs::Store &store,
		const engine::game::PortalResume &claim,
		const engine::scene::SeamTransform &through,
		uint64_t submittedTick
	);
	// Appends one locally submitted move input to the active portal prediction history.
	bool RecordPortalPredictionInput(
		engine::ecs::Store &store, uint64_t tick, const engine::game::MoveInput &move, float delta
	);
	// Replays unacknowledged portal inputs from the authority's covered tick.
	bool ReconcilePortalInputHistory(
		engine::ecs::Store &store,
		const engine::game::PortalResume &claim,
		const engine::script::PortalTransferMotion &motion
	);

	// The same corrected fractional pose used to draw the body and follow its camera.
	std::optional<engine::core::CFrame> PresentedPlayerPrediction(const engine::ecs::Store &store);

	// Presentation values only. Destination entity handles are resolved on adoption.
	// Motion's ticks identify the baseline and replay frontier, not a new authority sample.
	struct PortalPredictionContinuation {
		// Authoritative or transferred motion sample.
		engine::script::PortalTransferMotion Motion;
		// Movement direction used by the prediction replay.
		engine::core::Vector3 MoveDirection;
		// Last input tick covered by authoritative motion.
		uint64_t CoveredThrough = 0;
		// Presentation time carried across the portal.
		double PresentationSeconds = 0;
		// Render-only correction from the authoritative pose.
		engine::core::Vector3 PositionCorrection;
		// Duration over which the presentation correction is blended.
		float CorrectionSeconds = 0;
		// Replay clock carried with the prediction state.
		PredictionReplayClock Clock;
		// Inputs retained for reconciliation.
		std::vector<engine::replication::Input> Inputs;
	};
	// Native Player Prediction declaration.
	struct NativePlayerPrediction {
		// World incarnation that produced this prediction.
		uint64_t Incarnation = 0;
		// Simulation tick for applied pose.
		uint64_t AppliedPoseTick = 0;
		// Input tick already applied locally.
		uint64_t AppliedInputTick = 0;
		// Latest authoritative player-motion sample.
		std::optional<engine::game::PlayerMotion> Sample;
		// Replay clock carried with the prediction state.
		PredictionReplayClock Clock;
	};
	// Accepts an authoritative local-player motion sample into native prediction state.
	bool AcceptNativePlayerMotion(
		engine::ecs::Store &store, const engine::game::PlayerMotion &sample, uint64_t submittedTick
	);
	// Replays buffered local inputs after an authoritative native-player correction.
	std::optional<uint64_t> ReconcileNativePlayerPrediction(
		engine::ecs::Store &store, std::span<const engine::replication::Input> inputs, uint64_t coveredThrough
	);
	// Copies the current predicted player pose for transfer through a portal claim.
	std::optional<PortalPredictionContinuation> CapturePortalPrediction(
		const engine::ecs::Store &store, const engine::game::PortalResume &claim, float alpha = 0
	);
	// Restores transferred prediction state into the destination replica world.
	bool AdoptPortalPrediction(
		engine::ecs::Store &store,
		engine::ecs::Entity player,
		const PortalPredictionContinuation &continuation,
		float alpha = 0
	);
	// Seeds an arrived player from an applied destination snapshot when no source
	// motion continuation has arrived yet. Refusal leaves the replica untouched.
	bool SeedPortalAdoptionPrediction(
		engine::ecs::Store &store,
		engine::ecs::Entity player,
		uint64_t appliedTick,
		std::span<const engine::replication::Input> unconfirmed,
		uint64_t destinationIncarnation
	);

	// Installs the presentation half of a replicated world, and opens its VM.
	//
	// Called once, when the world is created and before any snapshot is
	// applied - a resource added later is still legal, but a draw list that
	// appeared halfway through a join would leave the first frames with nothing
	// to publish.
	//
	// **The VM is opened here and its scripts arrive later**, which is the one
	// way a replica differs from every other world that runs scripts. A host
	// calls `game::StartWorldScripts` over a world it has already built, so
	// `RunWorldScripts` starts everything in one pass; this world is empty when
	// its runtime opens and fills from the wire, so `replica-scripts` starts
	// what arrived on the tick it arrives - `script::ClientScriptsIn` decides
	// which, and `script::Runtime::RunNewScripts` starts each exactly once.
	//
	// **Through `game::StartWorldScripts` and not a loader of its own**, so the
	// heartbeat is installed on the fixed tick delta by the same call the studio
	// and the server use. The role is `HostRole::OfClient`, which is what makes
	// a `Script` in this tree stay unrun.
	//
	// @param store        The replicated world. Gains a `DrawList` and a
	//        `engine::replication::SnapshotBuffer` resource.
	// @param scheduler    Its scheduler, which gains the `PreRender` systems, the
	//        two `PreSimulation` adopters and the script heartbeat.
	// @param interpolation How far behind the newest received tick to draw, and
	//        the rate that delay is measured against. The tick rate must be the
	//        authority's, not this client's frame rate.
	// @param limits The client script role and user-message sender for this replica.
	// @return The world's runtime, which is never null. The scheduler holds a
	//         reference too and drops it with the world, so a caller that keeps
	//         none still leaves the VM alive for exactly as long as the store -
	//         `game::StartWorldScripts` carries the argument.
	std::shared_ptr<engine::script::Runtime> BuildReplicatedWorld(
		engine::ecs::Store &store,
		engine::ecs::Scheduler &scheduler,
		const engine::replication::InterpolationSettings &interpolation = {},
		const engine::script::RuntimeLimits &limits = {}
	);

	// Records where everything in a replicated world is, at the tick that put it
	// there.
	//
	// **Called straight after the connection has applied its inbound messages,
	// not from a render system.** That instant is the one where the store holds
	// the tick the server described; a render pass reads the same rows, but a
	// pass that only ran when a frame was drawn would miss a tick whenever the
	// frame rate dipped below the tick rate, and the buffer would then be
	// interpolating across gaps that the network never produced.
	//
	// Cheap to call on a tick already recorded - it asks the buffer first and
	// walks nothing.
	//
	// @param store The replicated world, after the connection wrote into it.
	// @param tick  The last tick applied in full, from
	//        `engine::replication::Connector::Applied`. Zero does nothing:
	//        the snapshot has not landed and there is no state to record.
	void RecordReplicatedTick(engine::ecs::Store &store, uint64_t tick);

	// Removes authoritative rows the server withdrew from this replica's visibility.
	// Predicted rows belong to this client and a server cannot legitimately name them.
	void ForgetReplicatedRows(engine::ecs::Store &store, std::span<const engine::ecs::Entity> forgotten);

	// Replaces the local prediction with the just-applied authoritative player
	// state, then replays every still-unacknowledged movement input.
	void ReconcileLocalPlayerPrediction(
		engine::ecs::Store &store, uint64_t tick, std::span<const engine::replication::Input> unconfirmed
	);

	// Advances the local overlay as soon as a move submission was accepted by
	// the transport. Inputs refused before reaching the connector are not
	// predicted because the authority cannot consume them.
	void PredictLocalPlayerMove(engine::ecs::Store &store, const engine::game::MoveInput &move, float delta);

	// Points the replica's own camera at wherever this client is looking.
	//
	// **A replica has to construct its own viewer, and this is why.** A mirror
	// reflects the *eye*, so a reflection computed on the authority is correct
	// for the authority's camera and wrong for every client watching. What
	// crosses the wire is therefore the mirror - the `SurfaceCamera`, its lens
	// and the tree that says which pane it belongs to - and never the aim. Each
	// client aims it again, from where that client is standing, which is the
	// only place the right answer exists.
	//
	// The camera is minted from the **predicted** range. A replica may not mint
	// an authoritative entity - `Store::SetAdoptOnly` says why: the index would
	// collide exactly with one the authority allocates and `Apply` would be
	// right to merge them - but the reserved high range is a client's own, and a
	// camera nobody else can see is precisely what it is for.
	//
	// Created on the first call and reused after. The supplied pose and lens are
	// fallback values while automatic follow waits for a subject; explicit
	// selections and scripted cameras retain their own values. Unchanged fallback
	// values cause no writes. Destroying the world destroys the local camera.
	//
	// @param store The replicated world.
	// @param frame Where this client's camera is, in world space.
	// @param lens  Its field of view and clipping distances.
	// @return The camera entity, or `NULL_ENTITY` when one could not be made.
	engine::ecs::Entity AimReplicaViewer(
		engine::ecs::Store &store, const engine::core::CFrame &frame, const engine::scene::Camera &lens
	);
}
