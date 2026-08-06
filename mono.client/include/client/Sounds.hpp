#pragma once

// Where a delivered audio asset becomes a voice in the mixer.
//
// **The client is where this belongs and nowhere else.** `scene::Sound` says
// what a sound *is* and is `shared`, because a server decides what is audible
// and replicates that. `engine::audio` says how a graph mixes and is `client`,
// because a server has no output. Neither knows the other exists, and this file
// is the seam — it walks rows on one side and posts commands on the other.
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
// The mixer owns the graph and only the device callback touches it —
// `audio/AGENTS.md`'s first rule — so this posts and never writes.
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
		// last reference to one — `audio/AGENTS.md` requires exactly that.
		//
		// @param name The manifest's name, extension included.
		// @param samples The decoded audio, already in the device's format.
		// @return False for an invalid name or null samples.
		bool Add(engine::core::Name name, std::shared_ptr<const engine::audio::SampleBuffer> samples);

		// What a `SoundId` resolves to.
		//
		// @param name The name a script wrote.
		// @return The samples, or nullptr when nothing by that name has
		//         arrived — which is the ordinary state while content is still
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
		// different sound, and the chain is rebuilt rather than repointed —
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

		// The gain last sent, or a negative number before the first one — which
		// no legal volume is, so the first pass always posts.
		float Level = -1.0f;

		// Whether the player was last told to loop.
		bool Loops = false;

		// Where the emitter was last placed. Unused for a sound with no
		// placement node.
		engine::audio::EmitterPlacement Where;
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
		// matters because this runs every frame and the queue is bounded —
		// a pass that reposted its whole state would fill it and start dropping
		// the commands that were real changes.
		//
		// @param store     The world to read. Not modified.
		// @param mixer     Where the commands go.
		// @param catalogue What a `SoundId` resolves to.
		// @param listener  Where the ear is, for positional sounds.
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

	  private:
		// Builds the chain for one row and posts it.
		Voice Open(
			engine::audio::AudioMixer &mixer,
			const std::shared_ptr<const engine::audio::SampleBuffer> &samples,
			bool positional
		);

		// Tears one down.
		void Close(engine::audio::AudioMixer &mixer, const Voice &voice);

		std::unordered_map<uint64_t, Voice> Voices;

		// Scratch, reused: the entities visited this pass, so one that has gone
		// away can be closed without allocating a set every frame.
		std::vector<uint64_t> Seen;
	};
}
