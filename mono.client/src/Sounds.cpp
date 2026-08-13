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
// One rule shapes the whole file: **post only what changed.** The command queue
// is bounded and a full one drops rather than blocks — right, because the
// consumer has a deadline — so a pass that reposted its whole state every frame
// would fill it with no-ops and start dropping the commands that were real.

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

	Voice SoundStage::Open(
		engine::audio::AudioMixer &mixer,
		const std::shared_ptr<const engine::audio::SampleBuffer> &samples,
		bool positional
	) {
		auto &queue = mixer.Commands();

		Voice voice;
		voice.Player = queue.Allocate();
		voice.Fader = queue.Allocate();
		if (positional) {
			voice.Placement = queue.Allocate();
		}

		Command command;
		command.Kind = CommandKind::AddNode;
		command.Target = voice.Player;
		command.Node = NodeKind::Player;
		queue.Post(command);

		command.Target = voice.Fader;
		command.Node = NodeKind::Fader;
		queue.Post(command);

		if (positional) {
			command.Target = voice.Placement;
			command.Node = NodeKind::Emitter;
			queue.Post(command);
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
		queue.Post(command);

		if (positional) {
			command.Target = voice.Fader;
			command.Second = voice.Placement;
			queue.Post(command);

			command.Target = voice.Placement;
			command.Second = mixer.Graph().Output();
			queue.Post(command);
		} else {
			command.Target = voice.Fader;
			command.Second = mixer.Graph().Output();
			queue.Post(command);
		}

		command = {};
		command.Kind = CommandKind::SetSound;
		command.Target = voice.Player;
		command.Sound = samples;
		queue.Post(command);

		return voice;
	}

	void SoundStage::Close(engine::audio::AudioMixer &mixer, const Voice &voice) {
		auto &queue = mixer.Commands();

		// Stopped before it is removed. `RemoveNode` takes its wires with it,
		// so the order is not load-bearing for correctness — but a player that
		// is removed mid-block while still marked playing is a state the graph
		// briefly holds and nothing needs it to.
		Command command;
		command.Kind = CommandKind::Stop;
		command.Target = voice.Player;
		queue.Post(command);

		command = {};
		command.Kind = CommandKind::RemoveNode;
		for (const engine::audio::NodeId node : {voice.Player, voice.Fader, voice.Placement}) {
			if (node.IsValid()) {
				command.Target = node;
				queue.Post(command);
			}
		}
	}

	void SoundStage::Clear(engine::audio::AudioMixer &mixer) {
		for (const auto &[entity, voice] : Voices) {
			Close(mixer, voice);
		}
		Voices.clear();
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

		// What the world decided about itself, or the defaults for a world that
		// has never been told. **Read once per pass, not once per row**, because a
		// resource lookup per sound in a level full of ambience is a hash probe
		// for a number that cannot have changed inside the walk.
		const engine::scene::AudioState *audio = store.Resource<engine::scene::AudioState>();

		// Negative gain is a phase inversion rather than a quieter sound, and a
		// script that wrote one meant silence. Clamped here rather than in the
		// property setter so the resource keeps what was written and only what is
		// *posted* is bounded — the same split `Sound::Volume` is on, where above
		// 1 is legal and the output stage clips it once.
		const float master = audio == nullptr ? 1.0f : std::max(audio->MasterVolume, 0.0f);

		store.Each<const engine::scene::Sound>([&](engine::ecs::Entity instance,
												   const engine::scene::Sound &sound) {
			Seen.push_back(instance.Id);

			const auto samples = catalogue.Find(sound.SoundId);
			const auto existing = Voices.find(instance.Id);

			// A sound that should not be sounding, or one whose asset has
			// not arrived yet. The second is the ordinary state while
			// content streams, not an error — and it is why a script may
			// set `Playing` before anything has been delivered and still
			// have it start when it does.
			if (!sound.Playing || samples == nullptr) {
				if (existing != Voices.end()) {
					Close(mixer, existing->second);
					Voices.erase(existing);
				}
				return;
			}

			// **Where it is heard from is its parent's**, which is the whole
			// of the positional rule and is read here rather than stored.
			// A parent with a place in the world makes this an emitter; a
			// parent that is a service — or none at all — makes it heard
			// everywhere at one level.
			const engine::ecs::Entity parent = store.ParentOf(instance);
			const engine::scene::Transform *placement =
				parent == engine::ecs::NULL_ENTITY ? nullptr : store.Get<engine::scene::Transform>(parent);
			const bool positional = placement != nullptr;

			if (existing != Voices.end() && existing->second.Sound != sound.SoundId) {
				// A different asset. Rebuilt rather than repointed:
				// `SetSound` rewinds, and a caller who changed the name
				// meant a different sound rather than a seek.
				Close(mixer, existing->second);
				Voices.erase(existing);
			} else if (existing != Voices.end() && existing->second.Placement.IsValid() != positional) {
				// Reparented between a service and a part. The chain's
				// shape is different, so it is a rebuild too.
				Close(mixer, existing->second);
				Voices.erase(existing);
			}

			auto voice = Voices.find(instance.Id);
			const bool opened = voice == Voices.end();
			if (opened) {
				Voice made = Open(mixer, samples, positional);
				made.Sound = sound.SoundId;
				made.Level = -1.0f; // Forces the first gain to be posted.
				voice = Voices.emplace(instance.Id, made).first;
			}

			Command command;

			// **The world's master gain multiplies the sound's own**, which is
			// what makes `SoundService.Volume` mean "turn this place down"
			// rather than "replace what every sound was authored at". `Level` is
			// the product because it is what was last *posted*, and the
			// change-detection this file is built around compares against that.
			const float level = sound.Volume * master;
			if (std::abs(voice->second.Level - level) > GAIN_EPSILON) {
				voice->second.Level = level;
				command = {};
				command.Kind = CommandKind::SetGain;
				command.Target = voice->second.Fader;
				command.Value = level;
				queue.Post(command);
			}

			if (opened || voice->second.Loops != sound.Looped) {
				voice->second.Loops = sound.Looped;
				command = {};
				command.Kind = CommandKind::SetLooping;
				command.Target = voice->second.Player;
				command.Flag = sound.Looped;
				queue.Post(command);
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
				if (opened || moved) {
					voice->second.Where = where;
					command = {};
					command.Kind = CommandKind::SetPlacement;
					command.Target = voice->second.Placement;
					command.Placement = where;
					queue.Post(command);
				}
			}

			if (opened) {
				// **Scheduled against the sample clock**, which is the
				// whole reason `Command` carries a deadline: a start
				// applied at the top of whichever block it lands in
				// quantises to the block, and a run of them is audibly
				// uneven. `audio/AGENTS.md` names this as the one place
				// "close enough to the frame" is wrong.
				command = {};
				command.Kind = CommandKind::Play;
				command.Target = voice->second.Player;
				command.AtSample = startAt;
				queue.Post(command);
			}
		});

		// The ear. Position only: a listener's facing would need the camera's
		// rotation, and every sound this engine places today is either
		// omnidirectional or attenuated by distance alone — panning against a
		// right vector nobody set would put a sound in the wrong ear rather
		// than in the middle. That is also why `scene::ListenerMode` has two
		// members where Roblox's `Enum.ListenerType` has four.
		//
		// **A world may move it, and an override that resolves to nothing falls
		// back rather than to the origin.** `SoundService:SetListener` names an
		// instance, the instance may be destroyed while the setting stands, and
		// an ear silently teleported to (0, 0, 0) is a scene that goes quiet for
		// no stated reason — where falling back to the camera is what the world
		// meant before it was told otherwise.
		engine::core::Vector3 ear = listener;
		if (audio != nullptr && audio->Mode == engine::scene::ListenerMode::ObjectPosition) {
			if (const auto *placed = store.Get<engine::scene::Transform>(audio->Listener)) {
				ear = placed->Frame.Position;
			}
		}

		Command listen;
		listen.Kind = CommandKind::SetListener;
		listen.Pose.X = ear.X;
		listen.Pose.Y = ear.Y;
		listen.Pose.Z = ear.Z;
		queue.Post(listen);

		// Anything that had a voice and is no longer a row. A `Sound` that was
		// destroyed leaves nodes behind otherwise, and a mixer accumulates
		// players walking buffers nothing references.
		std::sort(Seen.begin(), Seen.end());
		for (auto entry = Voices.begin(); entry != Voices.end();) {
			if (std::binary_search(Seen.begin(), Seen.end(), entry->first)) {
				++entry;
				continue;
			}
			Close(mixer, entry->second);
			entry = Voices.erase(entry);
		}
	}
}
