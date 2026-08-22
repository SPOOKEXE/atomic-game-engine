#include <engine/audio/Mp3.hpp>
#include <engine/audio/Wav.hpp>
#include <engine/core/Log.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/scene/Audio.hpp>
#include <engine/scene/Components.hpp>

#include <algorithm>
#include <client/Sounds.hpp>
#include <utility>

// The seam between a world's `Sound` rows and the mixer's graph.
//
// Two rules shape the whole file, and they are the two halves of one fact: the
// command queue is bounded and a full one drops rather than blocks, which is
// right, because the consumer has a deadline.
//
// **Post only what changed**, or a pass that reposted its whole state every
// frame would fill the queue with no-ops and start dropping the commands that
// were real.
//
// **And a drop is repaired, never recorded as landed.** Every `Post` here is
// checked and every last-posted value is written only after one returned true.
// Until v0.19 all fifteen call sites discarded the answer, so a full queue left
// a permanently silent voice, a fader stuck at the old level or a node nothing
// could ever remove - three different permanent faults from one transient
// condition. `audio/Commands.hpp` states the three classes and what each owes a
// refusal; this file is where they are all met.

namespace client {
	namespace {
		using engine::audio::Command;
		using engine::audio::CommandKind;
		using engine::audio::NodeKind;

		// How far ahead a start is scheduled.
		//
		// A tenth of a second, which is what `Client::BeginAudio` already uses:
		// far enough ahead that the command is applied at the deadline it names
		// rather than at the top of whichever block it lands in, and short
		// enough that nobody hears the wait. `audio/AGENTS.md` is explicit that
		// a deadline is the point and "play now" is the thing not to add.
		constexpr uint32_t START_DELAY_DIVISOR = 10;

		// Below this a `Volume` write is not worth a command.
		//
		// Not an optimisation: a float assigned from a script and read back is
		// not always bit-identical, so an exact compare would repost a gain
		// every frame for a sound nobody touched, forever.
		constexpr float GAIN_EPSILON = 1.0f / 4096.0f;
	}

	std::optional<engine::audio::SampleBuffer> DecodeAudio(std::span<const std::byte> bytes) {
		// **On the bytes, not on the name.** A publisher's extension is what
		// somebody typed and the content is what arrived; the two disagree the
		// first time a file is renamed, and a decoder handed the wrong format
		// produces noise at full volume rather than nothing.
		if (engine::audio::IsWav(bytes)) {
			return engine::audio::DecodeWav(bytes);
		}
		if (engine::audio::IsMp3(bytes)) {
			return engine::audio::DecodeMp3(bytes);
		}
		return std::nullopt;
	}

	bool
	SoundCatalogue::Add(engine::core::Name name, std::shared_ptr<const engine::audio::SampleBuffer> samples) {
		if (!name.IsValid() || samples == nullptr) {
			return false;
		}
		ByName[name.Id()] = std::move(samples);
		return true;
	}

	std::shared_ptr<const engine::audio::SampleBuffer> SoundCatalogue::Find(engine::core::Name name) const {
		const auto found = ByName.find(name.Id());
		return found == ByName.end() ? nullptr : found->second;
	}

	const Voice *SoundStage::Find(engine::ecs::Entity instance) const {
		const auto found = Voices.find(instance.Id);
		return found == Voices.end() ? nullptr : &found->second;
	}

	std::optional<Voice> SoundStage::Open(
		engine::audio::AudioMixer &mixer,
		const std::shared_ptr<const engine::audio::SampleBuffer> &samples,
		bool positional
	) {
		auto &queue = mixer.Commands();

		// **Counted here so the reservation and the posts cannot disagree.**
		// Three nodes and three wires for a positional voice, two and two for
		// one under a service, plus the `SetSound` either way. A number written
		// twice is a number that drifts, and the way it would fail is a burst
		// that half fits.
		const size_t nodes = positional ? 3 : 2;
		const size_t wires = positional ? 3 : 2;
		const size_t burst = nodes + wires + 1;
		if (queue.Free() < burst) {
			RefusedCommands += burst;
			return std::nullopt;
		}

		Voice voice;
		voice.Player = queue.Allocate();
		voice.Fader = queue.Allocate();
		if (positional) {
			voice.Placement = queue.Allocate();
		}

		// **Unchecked from here, and that is the reservation doing its job.**
		// One producer is the whole contract of `CommandQueue`, so nothing else
		// can take the room that was there a line ago and the consumer only ever
		// frees more. Checking each post again would be code no test could
		// reach.
		Command command;
		command.Kind = CommandKind::AddNode;
		command.Target = voice.Player;
		command.Node = NodeKind::Player;
		(void)queue.Post(command);

		command.Target = voice.Fader;
		command.Node = NodeKind::Fader;
		(void)queue.Post(command);

		if (positional) {
			command.Target = voice.Placement;
			command.Node = NodeKind::Emitter;
			(void)queue.Post(command);
		}

		// Player into fader, fader into the emitter when there is one, and
		// whichever is last into the output. **The emitter is after the fader
		// rather than before it** so that `Volume` means what a script thinks
		// it means: a level authored on the sound, then distance applied to it.
		// The other order would make a distant sound's volume slider do almost
		// nothing.
		command = {};
		command.Kind = CommandKind::Connect;
		command.Target = voice.Player;
		command.Second = voice.Fader;
		(void)queue.Post(command);

		if (positional) {
			command.Target = voice.Fader;
			command.Second = voice.Placement;
			(void)queue.Post(command);

			command.Target = voice.Placement;
			command.Second = mixer.Graph().Output();
			(void)queue.Post(command);
		} else {
			command.Target = voice.Fader;
			command.Second = mixer.Graph().Output();
			(void)queue.Post(command);
		}

		command = {};
		command.Kind = CommandKind::SetSound;
		command.Target = voice.Player;
		command.Sound = samples;
		(void)queue.Post(command);

		return voice;
	}

	bool SoundStage::Close(engine::audio::AudioMixer &mixer, const Voice &voice) {
		auto &queue = mixer.Commands();

		size_t burst = 1;
		for (const engine::audio::NodeId node : {voice.Player, voice.Fader, voice.Placement}) {
			burst += node.IsValid() ? 1 : 0;
		}

		// **Reserved for the same reason `Open` is, arriving at a worse
		// failure.** A half-posted teardown removes the fader and leaves the
		// player, and the entity that named them has already gone - so nothing
		// is left that could finish the job. The caller holds the voice instead
		// and this runs again next pass.
		if (queue.Free() < burst) {
			RefusedCommands += burst;
			return false;
		}

		// Stopped before it is removed. `RemoveNode` takes its wires with it,
		// so the order is not load-bearing for correctness - but a player that
		// is removed mid-block while still marked playing is a state the graph
		// briefly holds and nothing needs it to.
		Command command;
		command.Kind = CommandKind::Stop;
		command.Target = voice.Player;
		(void)queue.Post(command);

		command = {};
		command.Kind = CommandKind::RemoveNode;
		for (const engine::audio::NodeId node : {voice.Player, voice.Fader, voice.Placement}) {
			if (node.IsValid()) {
				command.Target = node;
				(void)queue.Post(command);
			}
		}

		return true;
	}

	void SoundStage::RetireClosed(engine::audio::AudioMixer &mixer) {
		size_t kept = 0;
		for (Voice &voice : Closing) {
			if (!Close(mixer, voice)) {
				Closing[kept++] = voice;
			}
		}
		Closing.resize(kept);
	}

	void SoundStage::Clear(engine::audio::AudioMixer &mixer) {
		RetireClosed(mixer);
		for (const auto &[entity, voice] : Voices) {
			if (!Close(mixer, voice)) {
				Closing.push_back(voice);
			}
		}
		Voices.clear();

		// **Said out loud, because nothing after this will try again.** `Clear`
		// is what a world teardown calls, so a teardown that did not fit is
		// nodes left in a mixer with the last thing that knew their ids about to
		// be destroyed. Every other refusal in this file repairs itself; this
		// one cannot.
		if (!Closing.empty()) {
			ENGINE_WARN(
				"audio: {} voice(s) could not be torn down - the command queue was full at teardown, so "
				"their nodes stay in the mixer",
				Closing.size()
			);
		}
	}

	void SoundStage::Sync(
		engine::ecs::Store &store,
		engine::audio::AudioMixer &mixer,
		const SoundCatalogue &catalogue,
		const engine::core::Vector3 &listener,
		uint32_t sampleRate
	) {
		ENGINE_PROFILE_CAT("client::SoundStage::Sync", engine::core::ProfileCategory::Engine);

		Seen.clear();
		auto &queue = mixer.Commands();
		const uint64_t startAt = mixer.Clock() + (sampleRate / START_DELAY_DIVISOR);
		const uint64_t refusedBefore = RefusedCommands;

		// **Before anything new is opened.** A teardown is the one refusal
		// nothing else remembers, so it gets the room first; opening a voice
		// ahead of it would keep the graph full of nodes whose rows have gone.
		RetireClosed(mixer);

		// What the world decided about itself, or the defaults for a world that
		// has never been told. **Read once per pass, not once per row**, because a
		// resource lookup per sound in a level full of ambience is a hash probe
		// for a number that cannot have changed inside the walk.
		const engine::scene::AudioState *audio = store.Resource<engine::scene::AudioState>();

		// Negative gain is a phase inversion rather than a quieter sound, and a
		// script that wrote one meant silence. Clamped here rather than in the
		// property setter so the resource keeps what was written and only what is
		// *posted* is bounded - the same split `Sound::Volume` is on, where above
		// 1 is legal and the output stage clips it once.
		const float master = audio == nullptr ? 1.0f : std::max(audio->MasterVolume, 0.0f);

		store.Each<const engine::scene::Sound>([&](engine::ecs::Entity instance,
												   const engine::scene::Sound &sound) {
			Seen.push_back(instance.Id);

			const auto samples = catalogue.Find(sound.SoundId);
			const auto existing = Voices.find(instance.Id);

			// A sound that should not be sounding, or one whose asset has
			// not arrived yet. The second is the ordinary state while
			// content streams, not an error - and it is why a script may
			// set `Playing` before anything has been delivered and still
			// have it start when it does.
			if (!sound.Playing || samples == nullptr) {
				if (existing != Voices.end()) {
					if (!Close(mixer, existing->second)) {
						Closing.push_back(existing->second);
					}
					Voices.erase(existing);
				}
				return;
			}

			// **Where it is heard from is its parent's**, which is the whole
			// of the positional rule and is read here rather than stored.
			// A parent with a place in the world makes this an emitter; a
			// parent that is a service - or none at all - makes it heard
			// everywhere at one level.
			const engine::ecs::Entity parent = store.ParentOf(instance);
			const engine::scene::Transform *placement =
				parent == engine::ecs::NULL_ENTITY ? nullptr : store.Get<engine::scene::Transform>(parent);
			const bool positional = placement != nullptr;

			if (existing != Voices.end() && (existing->second.Sound != sound.SoundId ||
											 existing->second.Placement.IsValid() != positional)) {
				// A different asset, or reparented between a service and a
				// part. Rebuilt rather than repointed in both cases: `SetSound`
				// rewinds and a caller who changed the name meant a different
				// sound rather than a seek, and a chain of a different shape is
				// a different chain.
				if (!Close(mixer, existing->second)) {
					Closing.push_back(existing->second);
				}
				Voices.erase(existing);
			}

			auto voice = Voices.find(instance.Id);
			if (voice == Voices.end()) {
				// **Nothing is recorded when there was no room.** The row keeps
				// no voice, so the next pass reaches exactly this branch again
				// and builds it - which is the whole repair. Recording a
				// half-built one instead is a voice that is silent for ever and
				// looks perfectly healthy from every side.
				std::optional<Voice> made = Open(mixer, samples, positional);
				if (!made.has_value()) {
					return;
				}
				made->Sound = sound.SoundId;
				voice = Voices.emplace(instance.Id, *made).first;
			}

			Command command;

			// **The world's master gain multiplies the sound's own**, which is
			// what makes `SoundService.Volume` mean "turn this place down"
			// rather than "replace what every sound was authored at". `Level` is
			// the product because it is what was last *posted*, and the
			// change-detection this file is built around compares against that.
			const float level = sound.Volume * master;
			if (std::abs(voice->second.Level - level) > GAIN_EPSILON) {
				command = {};
				command.Kind = CommandKind::SetGain;
				command.Target = voice->second.Fader;
				command.Value = level;

				// **Recorded only if it landed.** This is the coalescable class
				// and the whole of what makes it coalesce: a refusal leaves
				// `Level` where it was, the compare above is still true next
				// pass, and the gain is posted again. Assigning first - which
				// this file did until v0.19 - turns one dropped command into a
				// fader stuck at the old level for the life of the world.
				if (queue.Post(command)) {
					voice->second.Level = level;
				} else {
					RefusedCommands++;
				}
			}

			if (!voice->second.LoopsPosted || voice->second.Loops != sound.Looped) {
				command = {};
				command.Kind = CommandKind::SetLooping;
				command.Target = voice->second.Player;
				command.Flag = sound.Looped;
				if (queue.Post(command)) {
					voice->second.Loops = sound.Looped;
					voice->second.LoopsPosted = true;
				} else {
					RefusedCommands++;
				}
			}

			if (positional) {
				engine::audio::EmitterPlacement where;
				where.X = placement->Frame.Position.X;
				where.Y = placement->Frame.Position.Y;
				where.Z = placement->Frame.Position.Z;
				where.FalloffStart = sound.RollOffMinDistance;
				where.FalloffEnd = sound.RollOffMaxDistance;

				// Posted every frame for a moving emitter and skipped for a
				// still one. A part that has not moved is the common case in
				// a level full of ambience, and a command per still emitter
				// per frame is what fills the queue.
				const engine::audio::EmitterPlacement &last = voice->second.Where;
				const bool moved = where.X != last.X || where.Y != last.Y || where.Z != last.Z ||
								   where.FalloffStart != last.FalloffStart ||
								   where.FalloffEnd != last.FalloffEnd;
				if (!voice->second.WherePosted || moved) {
					command = {};
					command.Kind = CommandKind::SetPlacement;
					command.Target = voice->second.Placement;
					command.Placement = where;
					if (queue.Post(command)) {
						voice->second.Where = where;
						voice->second.WherePosted = true;
					} else {
						RefusedCommands++;
					}
				}
			}

			if (!voice->second.Started) {
				// **Scheduled against the sample clock**, which is the
				// whole reason `Command` carries a deadline: a start
				// applied at the top of whichever block it lands in
				// quantises to the block, and a run of them is audibly
				// uneven. `audio/AGENTS.md` names this as the one place
				// "close enough to the frame" is wrong.
				//
				// **Against this pass's deadline, so a retry is not late by
				// however long it waited.** `startAt` is a tenth of a second
				// ahead of *now*, and a refused `Play` reposted next frame
				// against the frame before's deadline would be applied at the
				// top of the next block, which is exactly the quantisation the
				// deadline exists to avoid.
				command = {};
				command.Kind = CommandKind::Play;
				command.Target = voice->second.Player;
				command.AtSample = startAt;
				if (queue.Post(command)) {
					voice->second.Started = true;
				} else {
					RefusedCommands++;
				}
			}
		});

		// The ear. Position only: a listener's facing would need the camera's
		// rotation, and every sound this engine places today is either
		// omnidirectional or attenuated by distance alone - panning against a
		// right vector nobody set would put a sound in the wrong ear rather
		// than in the middle. That is also why `scene::ListenerMode` has two
		// members where Roblox's `Enum.ListenerType` has four.
		//
		// **A world may move it, and an override that resolves to nothing falls
		// back rather than to the origin.** `SoundService:SetListener` names an
		// instance, the instance may be destroyed while the setting stands, and
		// an ear silently teleported to (0, 0, 0) is a scene that goes quiet for
		// no stated reason - where falling back to the camera is what the world
		// meant before it was told otherwise.
		engine::core::Vector3 ear = listener;
		if (audio != nullptr && audio->Mode == engine::scene::ListenerMode::ObjectPosition) {
			if (const auto *placed = store.Get<engine::scene::Transform>(audio->Listener)) {
				ear = placed->Frame.Position;
			}
		}

		// **Posted when it moves, not every pass.** `SetListener` is the same
		// coalescable kind as a gain and was the one command here with no
		// last-posted value beside it, so a still listener spent a queue slot
		// per world per frame saying where it already was.
		if (!EarPosted || ear.X != Ear.X || ear.Y != Ear.Y || ear.Z != Ear.Z) {
			Command listen;
			listen.Kind = CommandKind::SetListener;
			listen.Pose.X = ear.X;
			listen.Pose.Y = ear.Y;
			listen.Pose.Z = ear.Z;
			if (queue.Post(listen)) {
				Ear = listen.Pose;
				EarPosted = true;
			} else {
				RefusedCommands++;
			}
		}

		// Anything that had a voice and is no longer a row. A `Sound` that was
		// destroyed leaves nodes behind otherwise, and a mixer accumulates
		// players walking buffers nothing references.
		std::sort(Seen.begin(), Seen.end());
		for (auto entry = Voices.begin(); entry != Voices.end();) {
			if (std::binary_search(Seen.begin(), Seen.end(), entry->first)) {
				++entry;
				continue;
			}
			if (!Close(mixer, entry->second)) {
				Closing.push_back(entry->second);
			}
			entry = Voices.erase(entry);
		}

		// **Rate limited, because this is a per-frame failure.** A queue that is
		// full is full for as long as the scene is doing whatever filled it, so
		// a line per pass would be sixty a second and the first one - the only
		// one that says when it started - would be gone from the terminal. The
		// counts are cumulative for the same reason: what an operator needs is
		// the trend, not this frame's number.
		if (RefusedCommands != refusedBefore) {
			ENGINE_WARN_EVERY(
				5.0,
				"audio: the mixer command queue is full - {} command(s) refused for this world's sounds so "
				"far, {} teardown(s) still waiting. Voices repair themselves; a reading that keeps climbing "
				"means the queue is too small for what this scene does.",
				RefusedCommands,
				Closing.size()
			);
		}
	}
}
