#pragma once

// Where a delivered audio asset becomes a voice in the mixer.
//
// **The client is where this belongs and nowhere else.** `scene::Sound` says
// what a sound *is* and is `shared`, because a server decides what is audible
// and replicates that. `engine::audio` says how a graph mixes and is `client`,
// because a server has no output. Neither knows the other exists, and this file
// is the seam - it walks rows on one side and posts commands on the other.
//
// Two halves, and they are separate for a reason:
//
// - `SoundCatalogue` is what arrived. Keyed by the manifest's name, exactly as
//   the renderer keys a mesh, because a `SoundId` is a published name and the
//   one place a lookup could go wrong is a spelling.
// - `SoundStage` is what is sounding. It owns the mapping from an entity to the
//   nodes standing in for it, which is state neither the world nor the mixer
//   can hold: the world must not know about node ids, and the mixer must not
//   know about entities.
//
// **The parent decides whether a sound is positional**, which is `scene`'s rule
// arriving where it is acted on. A `Sound` under a service is heard everywhere
// at one level; a `Sound` inside something with a `Transform` gets an `Emitter`
// node between its fader and the output, and falls off from that thing's
// position. Nothing here reads a position off the sound itself, because there
// is none to read.
//
// **Every change is a command with a deadline**, never a reach into the graph.
// The mixer owns the graph and only the device callback touches it -
// `audio/AGENTS.md`'s first rule - so this posts and never writes.
//
// @client

#include <engine/audio/Commands.hpp>
#include <engine/audio/Graph.hpp>
#include <engine/audio/Mixer.hpp>
#include <engine/audio/Sample.hpp>
#include <engine/core/Name.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/ecs/Store.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace client {

	// Decodes a delivered audio asset.
	//
	// **Dispatched on the bytes, not on the extension.** The manifest's name is
	// what a publisher typed and the content is what arrived, and the two
	// disagree the first time somebody renames a file. `IsWav` and `IsMp3` are
	// the two questions there are; a blob answering neither is refused rather
	// than handed to a decoder to guess at, because a decoder that guessed
	// would produce noise at full volume.
	//
	// @param bytes The asset, as delivered.
	// @return The samples, or nothing when this is not audio this engine
	//         decodes.
	// @since v0.9
	std::optional<engine::audio::SampleBuffer> DecodeAudio(std::span<const std::byte> bytes);

	// The sounds this client has, by the name the manifest published them under.
	//
	// @since v0.9
	class SoundCatalogue {
	  public:
		// Registers a decoded asset.
		//
		// **Shared ownership, never a copy per voice.** Two hundred parts
		// playing one footstep hold one buffer, and a `SoundRef` copied on the
		// tick side is what keeps the device callback from ever dropping the
		// last reference to one - `audio/AGENTS.md` requires exactly that.
		//
		// @param name The manifest's name, extension included.
		// @param samples The decoded audio, already in the device's format.
		// @return False for an invalid name or null samples.
		bool Add(engine::core::Name name, std::shared_ptr<const engine::audio::SampleBuffer> samples);

		// What a `SoundId` resolves to.
		//
		// @param name The name a script wrote.
		// @return The samples, or nullptr when nothing by that name has
		//         arrived - which is the ordinary state while content is still
		//         streaming, not an error.
		std::shared_ptr<const engine::audio::SampleBuffer> Find(engine::core::Name name) const;

		// How many sounds are registered.
		size_t Count() const {
			return ByName.size();
		}

	  private:
		std::unordered_map<uint32_t, std::shared_ptr<const engine::audio::SampleBuffer>> ByName;
	};

	// What one `scene::Sound` row is currently sounding as.
	//
	// @since v0.9
	struct Voice {
		// The player walking the samples.
		engine::audio::NodeId Player;

		// Its own fader, so a `Volume` write is one command and not a rebuild.
		engine::audio::NodeId Fader;

		// The emitter placing it, for a positional sound. Invalid for one under
		// a service, which is heard everywhere at one level.
		engine::audio::NodeId Placement;

		// What it was built for. A `SoundId` write while a voice exists is a
		// different sound, and the chain is rebuilt rather than repointed -
		// `SetSound` rewinds, which is right, and the name here is how that is
		// noticed at all.
		engine::core::Name Sound;

		// --- what was last posted -------------------------------------------
		//
		// **Kept so that a pass that changed nothing posts nothing.** The queue
		// is bounded and a full one drops rather than blocks; a sync that
		// reposted its whole state every frame would fill it with no-ops and
		// start dropping the commands that were real changes. These are the
		// values the mixer was last told, not the values the world holds.
		//
		// **And "told" means the command was accepted.** Every one of these is
		// written only after `CommandQueue::Post` returned true, which is what
		// makes a dropped command repair itself on the next pass rather than
		// becoming permanent - `audio/Commands.hpp` calls this the coalescable
		// class and states the rule. Recording the value first, which is what
		// this file did until v0.19, coalesces the command into nothing and the
		// node keeps the old level for the life of the world.

		// The gain last sent, or a negative number before the first one - which
		// no legal volume is, so the first pass always posts.
		float Level = -1.0f;

		// Whether the player was last told to loop, and whether it was told at
		// all. The second is not redundant: `false` is a legal value, so
		// nothing about the first says whether the `SetLooping` landed.
		// Coalescable, so a refused one is reposted by the next pass that finds
		// the two still differ.
		//@{
		bool Loops = false;
		bool LoopsPosted = false;
		//@}

		// Where the emitter was last placed, and whether that placement landed.
		// Unused for a sound with no placement node. `SetPlacement` is
		// coalescable too: a refused one is reposted by the next pass that sees
		// the emitter has moved.
		//@{
		engine::audio::EmitterPlacement Where;
		bool WherePosted = false;
		//@}

		// Whether the `Play` that starts this voice was accepted.
		//
		// **The repair for the one command in the chain that is an edge rather
		// than a value.** A dropped `Play` is a fully built, correctly wired,
		// permanently silent voice, and nothing downstream would ever notice:
		// the row says `Playing`, the stage says it has a voice, and the mixer
		// is holding a player that was never started. While this is false the
		// next pass posts it again, against a fresh deadline.
		bool Started = false;
	};

	// The voices standing in for a world's `Sound` rows.
	//
	// **One stage per world**, because node ids are minted per mixer and an
	// entity is only unique within its own store. A client hosting two worlds
	// holds two of these.
	//
	// @since v0.9
	class SoundStage {
	  public:
		// Brings the mixer into line with the world.
		//
		// Idempotent and cheap when nothing changed: a row that is already
		// sounding the right thing at the right level posts nothing. That
		// matters because this runs every frame and the queue is bounded -
		// a pass that reposted its whole state would fill it and start dropping
		// the commands that were real changes.
		//
		// **And idempotent is also what repairs a full queue.** A refused
		// command leaves this stage recording that the mixer has not been told,
		// so the next pass finds the same difference and posts it again. That
		// covers every kind but one: a teardown whose row has gone is held in
		// `PendingCloses()` instead, because nothing in the world would ever
		// ask for it a second time.
		//
		// **`scene::AudioState` is what a script decides here**, and it is read
		// rather than pushed for the tier reason the whole file exists for: the
		// script layer is `shared` and cannot name a mixer, so it writes a
		// resource and this is what acts on it. Its master gain multiplies each
		// voice's own `Volume` rather than the output node's, because a client
		// hosts several worlds and has one output - a number applied there would
		// have N worlds writing it and the last one of the frame winning.
		//
		// @param store     The world to read. Not modified.
		// @param mixer     Where the commands go.
		// @param catalogue What a `SoundId` resolves to.
		// @param listener  Where the ear is by default, for positional sounds.
		//        A world naming a listener instance under
		//        `scene::ListenerMode::ObjectPosition` overrides this for its own
		//        pass.
		// @param sampleRate The device's rate, for scheduling a start.
		void Sync(
			engine::ecs::Store &store,
			engine::audio::AudioMixer &mixer,
			const SoundCatalogue &catalogue,
			const engine::core::Vector3 &listener,
			uint32_t sampleRate
		);

		// Stops everything and releases every node.
		//
		// What a world teardown calls. Without it the nodes outlive the
		// entities they were standing in for, and a mixer accumulates players
		// walking buffers nothing references.
		//
		// @param mixer Where the commands go.
		void Clear(engine::audio::AudioMixer &mixer);

		// How many voices are sounding.
		size_t Count() const {
			return Voices.size();
		}

		// Whether an entity has a voice.
		//
		// @param instance The `Sound` row.
		// @return Its voice, or nullptr.
		const Voice *Find(engine::ecs::Entity instance) const;

		// How many commands this stage could not post, over its life.
		//
		// **Beside `CommandQueue::Dropped` rather than instead of it.** That
		// counter is the whole mixer's and is what an operator reads; this one
		// says how much of it was a world's sound sync, which is the number that
		// says whether the queue is too small for what this scene does.
		uint64_t Refused() const {
			return RefusedCommands;
		}

		// How many teardowns are waiting for room in the queue.
		//
		// Non-zero means a `Stop` or a `RemoveNode` was refused and the nodes
		// are still in the graph. It settles to zero on its own; a reading that
		// stays up is a mixer accumulating players.
		size_t PendingCloses() const {
			return Closing.size();
		}

	  private:
		// Builds the chain for one row and posts it.
		//
		// **All of it or none of it, decided before anything is posted.** A
		// voice is five commands or seven, and half of one is a player wired to
		// nothing that no later pass can find its way back from - the undo
		// would itself be commands, into the queue that just refused. So the
		// room is asked for with `CommandQueue::Free` first and the ids are
		// allocated only once it is there. `audio/Commands.hpp` calls this the
		// repairable class.
		//
		// @return The voice, or nothing when there was no room - in which case
		//         nothing was posted, no id was spent, and the caller should
		//         leave the row without a voice so the next pass tries again.
		std::optional<Voice> Open(
			engine::audio::AudioMixer &mixer,
			const std::shared_ptr<const engine::audio::SampleBuffer> &samples,
			bool positional
		);

		// Tears one down.
		//
		// Reserved like `Open` and for the sharper version of its reason: a
		// dropped `Stop` or `RemoveNode` is terminal, because the row that would
		// have noticed is the one being torn down.
		//
		// @return `false` when there was no room and nothing was posted. The
		//         caller must hold the voice for a later attempt rather than
		//         forgetting it, or the nodes outlive everything that knows
		//         their ids.
		bool Close(engine::audio::AudioMixer &mixer, const Voice &voice);

		// Retries every teardown the queue had no room for.
		//
		// First thing in a pass, so the room a drain has just freed goes to the
		// nodes nothing else remembers before it goes to new voices.
		void RetireClosed(engine::audio::AudioMixer &mixer);

		std::unordered_map<uint64_t, Voice> Voices;

		// Teardowns the queue had no room for, retried at the top of each pass.
		//
		// **The one piece of state a refusal cannot repair by itself.** A
		// coalescable command is reposted because the value still differs and a
		// half-built voice is rebuilt because the row still has none, but a
		// voice whose row has gone is remembered by nothing else - so it is
		// remembered here.
		std::vector<Voice> Closing;

		// The listener pose last accepted, and whether one ever was.
		//
		// The `Voice` fields' arrangement for the world's one ear:
		// `SetListener` used to be posted unconditionally every pass, which is a
		// command per world per frame saying what it already said.
		engine::audio::ListenerPose Ear;
		bool EarPosted = false;

		uint64_t RefusedCommands = 0;

		// Scratch, reused: the entities visited this pass, so one that has gone
		// away can be closed without allocating a set every frame.
		std::vector<uint64_t> Seen;
	};
}
