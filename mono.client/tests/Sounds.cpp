#include <engine/audio/Mixer.hpp>
#include <engine/audio/Sample.hpp>
#include <engine/core/Name.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Audio.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <client/Sounds.hpp>
#include <cstddef>
#include <memory>
#include <string_view>
#include <vector>

TEST_SUITE_ID("client.sounds")
TEST_DEPENDS("engine.scene.sound")
TEST_DEPENDS("engine.audio.mixer")

using client::DecodeAudio;
using client::SoundCatalogue;
using client::SoundStage;
using client::Voice;
using engine::audio::AudioMixer;
using engine::audio::SampleBuffer;
using engine::core::Name;
using engine::core::Vector3;
using engine::ecs::Entity;
using engine::ecs::NULL_ENTITY;
using engine::ecs::Store;
using engine::scene::MakePart;
using engine::scene::PartDesc;
using engine::scene::SoundClass;

namespace {
	constexpr Vector3 EAR{0.0f, 0.0f, 0.0f};

	// A second of something audible, in the mixer's own format.
	std::shared_ptr<const SampleBuffer> Tone(size_t frames = 48000) {
		std::vector<float> samples(frames * 2, 0.25f);
		return std::make_shared<const SampleBuffer>(engine::audio::AudioFormat{}, samples);
	}

	Entity NewSound(Store &store, std::string_view id, bool playing = true) {
		const Entity instance = store.CreateInstance(SoundClass());
		REQUIRE(instance != NULL_ENTITY);

		engine::scene::Sound *sound = store.GetMutable<engine::scene::Sound>(instance);
		REQUIRE(sound != nullptr);
		sound->SoundId = Name(id);
		sound->Playing = playing;
		return instance;
	}

	// A catalogue holding one track under the name a script would write.
	SoundCatalogue With(std::string_view id) {
		SoundCatalogue catalogue;
		REQUIRE(catalogue.Add(Name(id), Tone()));
		return catalogue;
	}
}

TEST_CASE("a decoder is picked from the bytes, not from a name", "[client][sounds]") {
	// The name is what a publisher typed and the content is what arrived, and
	// the two disagree the first time somebody renames a file.
	const std::vector<std::byte> nothing;
	CHECK_FALSE(DecodeAudio(nothing).has_value());

	const char text[] = "this is not audio at all, by any reading of it";
	const std::span<const std::byte> prose(
		reinterpret_cast<const std::byte *>(text), reinterpret_cast<const std::byte *>(text) + sizeof(text)
	);
	CHECK_FALSE(DecodeAudio(prose).has_value());
}

TEST_CASE("a catalogue refuses what it cannot key or hold", "[client][sounds]") {
	SoundCatalogue catalogue;

	CHECK_FALSE(catalogue.Add(Name(), Tone()));
	CHECK_FALSE(catalogue.Add(Name("audio/track.mp3"), nullptr));
	CHECK(catalogue.Count() == 0);

	REQUIRE(catalogue.Add(Name("audio/track.mp3"), Tone()));
	CHECK(catalogue.Count() == 1);
	CHECK(catalogue.Find(Name("audio/track.mp3")) != nullptr);

	// A miss is the ordinary state while content is still streaming, not an
	// error - which is what lets a script set `Playing` before the asset has
	// arrived and still have it start when it does.
	CHECK(catalogue.Find(Name("audio/other.mp3")) == nullptr);
}

TEST_CASE("a playing sound opens a voice", "[client][sounds]") {
	Store store("sounds_test.play");
	AudioMixer mixer;
	SoundStage stage;

	const Entity sound = NewSound(store, "audio/track.mp3");
	stage.Sync(store, mixer, With("audio/track.mp3"), EAR, mixer.Format().SampleRate);

	CHECK(stage.Count() == 1);
	const Voice *voice = stage.Find(sound);
	REQUIRE(voice != nullptr);
	CHECK(voice->Player.IsValid());
	CHECK(voice->Fader.IsValid());

	// **No emitter**, because nothing gave this sound a parent with a place in
	// the world. Under a service it is heard everywhere at one level, which is
	// the case a music track is.
	CHECK_FALSE(voice->Placement.IsValid());
}

TEST_CASE("a sound whose asset has not arrived waits", "[client][sounds]") {
	Store store("sounds_test.waiting");
	AudioMixer mixer;
	SoundStage stage;

	const Entity sound = NewSound(store, "audio/track.mp3");

	// An empty catalogue: the content is still streaming. Not an error and not
	// a refusal - the row keeps asking, and the frame the asset lands is the
	// frame it starts.
	const SoundCatalogue nothing;
	stage.Sync(store, mixer, nothing, EAR, mixer.Format().SampleRate);
	CHECK(stage.Count() == 0);

	stage.Sync(store, mixer, With("audio/track.mp3"), EAR, mixer.Format().SampleRate);
	CHECK(stage.Find(sound) != nullptr);
}

TEST_CASE("a sound that is not playing has no voice", "[client][sounds]") {
	Store store("sounds_test.silent");
	AudioMixer mixer;
	SoundStage stage;

	const Entity sound = NewSound(store, "audio/track.mp3", false);
	const SoundCatalogue catalogue = With("audio/track.mp3");

	stage.Sync(store, mixer, catalogue, EAR, mixer.Format().SampleRate);
	CHECK(stage.Count() == 0);

	store.GetMutable<engine::scene::Sound>(sound)->Playing = true;
	stage.Sync(store, mixer, catalogue, EAR, mixer.Format().SampleRate);
	CHECK(stage.Count() == 1);

	// And stopping releases the nodes rather than leaving a silent player
	// walking a buffer forever.
	store.GetMutable<engine::scene::Sound>(sound)->Playing = false;
	stage.Sync(store, mixer, catalogue, EAR, mixer.Format().SampleRate);
	CHECK(stage.Count() == 0);
}

TEST_CASE("a sound inside a part is positional", "[client][sounds]") {
	// The whole of the parent rule, arriving where it is acted on: a sound
	// inside something with a place in the world gets an emitter and falls off
	// from that thing's position.
	Store store("sounds_test.positional");
	AudioMixer mixer;
	SoundStage stage;

	const Entity part = MakePart(store, PartDesc{});
	const Entity sound = NewSound(store, "audio/track.mp3");
	REQUIRE(store.SetParent(sound, part));

	stage.Sync(store, mixer, With("audio/track.mp3"), EAR, mixer.Format().SampleRate);

	const Voice *voice = stage.Find(sound);
	REQUIRE(voice != nullptr);
	CHECK(voice->Placement.IsValid());
}

TEST_CASE("reparenting between a service and a part rebuilds the chain", "[client][sounds]") {
	Store store("sounds_test.reparent");
	AudioMixer mixer;
	SoundStage stage;
	const SoundCatalogue catalogue = With("audio/track.mp3");

	const Entity sound = NewSound(store, "audio/track.mp3");
	stage.Sync(store, mixer, catalogue, EAR, mixer.Format().SampleRate);
	const engine::audio::NodeId first = stage.Find(sound)->Player;
	CHECK_FALSE(stage.Find(sound)->Placement.IsValid());

	const Entity part = MakePart(store, PartDesc{});
	REQUIRE(store.SetParent(sound, part));
	stage.Sync(store, mixer, catalogue, EAR, mixer.Format().SampleRate);

	// A different chain shape, so a rebuild rather than a repoint - the
	// emitter has to sit between the fader and the output and there is no
	// command that inserts one.
	REQUIRE(stage.Find(sound) != nullptr);
	CHECK(stage.Find(sound)->Placement.IsValid());
	CHECK_FALSE(stage.Find(sound)->Player == first);
}

TEST_CASE("changing SoundId rebuilds rather than repoints", "[client][sounds]") {
	Store store("sounds_test.swap");
	AudioMixer mixer;
	SoundStage stage;

	SoundCatalogue catalogue;
	REQUIRE(catalogue.Add(Name("audio/one.mp3"), Tone()));
	REQUIRE(catalogue.Add(Name("audio/two.mp3"), Tone()));

	const Entity sound = NewSound(store, "audio/one.mp3");
	stage.Sync(store, mixer, catalogue, EAR, mixer.Format().SampleRate);
	const engine::audio::NodeId first = stage.Find(sound)->Player;

	// `SetSound` rewinds, and somebody who wrote a different name meant a
	// different sound rather than a seek.
	store.GetMutable<engine::scene::Sound>(sound)->SoundId = Name("audio/two.mp3");
	stage.Sync(store, mixer, catalogue, EAR, mixer.Format().SampleRate);

	REQUIRE(stage.Find(sound) != nullptr);
	CHECK_FALSE(stage.Find(sound)->Player == first);
	CHECK(stage.Find(sound)->Sound == Name("audio/two.mp3"));
}

TEST_CASE("a destroyed sound takes its voice with it", "[client][sounds]") {
	Store store("sounds_test.destroy");
	AudioMixer mixer;
	SoundStage stage;
	const SoundCatalogue catalogue = With("audio/track.mp3");

	const Entity sound = NewSound(store, "audio/track.mp3");
	stage.Sync(store, mixer, catalogue, EAR, mixer.Format().SampleRate);
	REQUIRE(stage.Count() == 1);

	store.Destroy(sound);
	stage.Sync(store, mixer, catalogue, EAR, mixer.Format().SampleRate);

	// Otherwise the mixer accumulates players walking buffers nothing
	// references - a leak that is inaudible right up until it is not.
	CHECK(stage.Count() == 0);
	CHECK(stage.Find(sound) == nullptr);
}

TEST_CASE("a pass that changed nothing posts nothing", "[client][sounds]") {
	// The property the whole file is shaped around. The queue is bounded and a
	// full one drops rather than blocks, so a sync that reposted its state
	// every frame would fill it with no-ops and start dropping the commands
	// that were real changes.
	Store store("sounds_test.quiet");
	AudioMixer mixer;
	SoundStage stage;
	const SoundCatalogue catalogue = With("audio/track.mp3");

	NewSound(store, "audio/track.mp3");
	stage.Sync(store, mixer, catalogue, EAR, mixer.Format().SampleRate);

	// Drain what opening the voice posted, so what follows is measured against
	// an empty queue.
	SampleBuffer block(mixer.Format(), 512);
	mixer.Render(block);
	const size_t settled = mixer.Commands().Pending();

	for (int pass = 0; pass < 8; ++pass) {
		stage.Sync(store, mixer, catalogue, EAR, mixer.Format().SampleRate);
	}

	// **Nothing at all, including the listener.** `SetListener` used to be
	// posted unconditionally, which was one command per world per frame saying
	// where the ear already was - the one place in this file that broke the rule
	// the rest of it is built around.
	CHECK(mixer.Commands().Pending() == settled);
}

// --- a full queue, and what each kind of refusal owes -----------------------
//
// **The failure these are about is not the drop, it is the bookkeeping.** A
// command queue that is briefly full is an ordinary condition with a deadline on
// the far side of it; a stage that recorded the refused command as landed turns
// that transient into a permanently silent voice, a fader stuck at the old
// level, or a node nothing can ever remove. `audio/Commands.hpp` names the three
// classes.

namespace {
	// Fills the mixer's command queue, leaving `spare` slots.
	//
	// The commands name no node, so applying them does nothing - which is what
	// makes this a test of the queue being full rather than of what was in it.
	void Flood(AudioMixer &mixer, size_t spare = 0) {
		auto &queue = mixer.Commands();
		while (queue.Free() > spare) {
			engine::audio::Command filler;
			filler.Kind = engine::audio::CommandKind::SetGain;
			REQUIRE(queue.Post(filler));
		}
	}

	// Empties it again, the way the device thread would.
	void Drain(AudioMixer &mixer) {
		SampleBuffer block(mixer.Format(), 64);
		mixer.Render(block);
	}
}

TEST_CASE("a voice the queue had no room for is built by the next pass", "[client][sounds]") {
	Store store("sounds_test.refused_open");
	AudioMixer mixer;
	SoundStage stage;
	const SoundCatalogue catalogue = With("audio/track.mp3");

	const Entity sound = NewSound(store, "audio/track.mp3");

	// Five commands short of what a non-positional voice needs, so the
	// reservation refuses and - this is the half that matters - posts none of
	// them. A stage that posted until it ran out would have left a player wired
	// to nothing, with the id of every node in it already spent.
	Flood(mixer, 4);
	stage.Sync(store, mixer, catalogue, EAR, mixer.Format().SampleRate);
	CHECK(stage.Count() == 0);
	CHECK(stage.Find(sound) == nullptr);
	CHECK(stage.Refused() > 0);

	Drain(mixer);
	stage.Sync(store, mixer, catalogue, EAR, mixer.Format().SampleRate);
	REQUIRE(stage.Find(sound) != nullptr);

	// And it is a whole voice rather than the remains of the refused one.
	float loudest = 0.0f;
	SampleBuffer block(mixer.Format(), 512);
	for (int rendered = 0; rendered < 32; ++rendered) {
		block.Silence();
		mixer.Render(block);
		loudest = std::max(loudest, block.Peak());
	}
	CHECK(loudest > 0.0f);
}

TEST_CASE("a refused gain is posted again rather than coalesced away", "[client][sounds]") {
	Store store("sounds_test.refused_gain");
	AudioMixer mixer;
	SoundStage stage;
	const SoundCatalogue catalogue = With("audio/track.mp3");

	const Entity sound = NewSound(store, "audio/track.mp3");
	store.GetMutable<engine::scene::Sound>(sound)->Volume = 0.75f;
	stage.Sync(store, mixer, catalogue, EAR, mixer.Format().SampleRate);
	Drain(mixer);
	REQUIRE(stage.Find(sound) != nullptr);
	REQUIRE(stage.Find(sound)->Level == 0.75f);

	store.GetMutable<engine::scene::Sound>(sound)->Volume = 0.25f;

	Flood(mixer);
	stage.Sync(store, mixer, catalogue, EAR, mixer.Format().SampleRate);

	// **The last-posted value has not moved**, which is the whole repair: the
	// compare next pass still differs, so the gain is posted again. Recording
	// `0.25` here would leave the fader at `0.75` for the life of the world
	// with every side of the system reporting it as correct.
	CHECK(stage.Find(sound)->Level == 0.75f);

	Drain(mixer);
	stage.Sync(store, mixer, catalogue, EAR, mixer.Format().SampleRate);
	CHECK(stage.Find(sound)->Level == 0.25f);

	mixer.ApplyPending();
	const engine::audio::Node *fader = mixer.Graph().Find(stage.Find(sound)->Fader);
	REQUIRE(fader != nullptr);
	CHECK(fader->Gain == 0.25f);
}

TEST_CASE("a refused start is retried until the voice is audible", "[client][sounds]") {
	Store store("sounds_test.refused_play");
	AudioMixer mixer;
	SoundStage stage;
	const SoundCatalogue catalogue = With("audio/track.mp3");

	const Entity sound = NewSound(store, "audio/track.mp3");

	// Exactly the opening burst and nothing more: five commands build the
	// chain, and the `SetGain`, `SetLooping` and `Play` that follow it are
	// refused. That is the worst of the three - a fully built, correctly wired,
	// permanently silent voice that nothing downstream could ever notice.
	Flood(mixer, 5);
	stage.Sync(store, mixer, catalogue, EAR, mixer.Format().SampleRate);
	REQUIRE(stage.Find(sound) != nullptr);
	CHECK_FALSE(stage.Find(sound)->Started);

	Drain(mixer);
	stage.Sync(store, mixer, catalogue, EAR, mixer.Format().SampleRate);
	CHECK(stage.Find(sound)->Started);

	SampleBuffer block(mixer.Format(), 512);
	float loudest = 0.0f;
	for (int rendered = 0; rendered < 32; ++rendered) {
		block.Silence();
		mixer.Render(block);
		loudest = std::max(loudest, block.Peak());
	}
	CHECK(loudest > 0.0f);
}

TEST_CASE("a teardown the queue refused is held and retried", "[client][sounds]") {
	Store store("sounds_test.refused_close");
	AudioMixer mixer;
	SoundStage stage;
	const SoundCatalogue catalogue = With("audio/track.mp3");

	const Entity sound = NewSound(store, "audio/track.mp3");
	stage.Sync(store, mixer, catalogue, EAR, mixer.Format().SampleRate);
	REQUIRE(stage.Find(sound) != nullptr);
	const engine::audio::NodeId player = stage.Find(sound)->Player;
	Drain(mixer);

	store.Destroy(sound);
	Flood(mixer);
	stage.Sync(store, mixer, catalogue, EAR, mixer.Format().SampleRate);

	// The row has gone, so nothing in the world will ever ask for this teardown
	// again. It is remembered here or it is not remembered at all.
	CHECK(stage.Count() == 0);
	CHECK(stage.PendingCloses() == 1);

	Drain(mixer);
	stage.Sync(store, mixer, catalogue, EAR, mixer.Format().SampleRate);
	CHECK(stage.PendingCloses() == 0);

	mixer.ApplyPending();
	CHECK(mixer.Graph().Find(player) == nullptr);
}

TEST_CASE("what the stage built actually mixes", "[client][sounds]") {
	// **The case that proves the chain rather than the bookkeeping.** Every
	// test above checks that the right nodes exist; this one renders past the
	// scheduled start and asks whether anything came out. A wiring mistake -
	// the fader connected to nothing, the player never told to play, the start
	// scheduled at a deadline that never arrives - passes all of them and
	// produces silence.
	Store store("sounds_test.audible");
	AudioMixer mixer;
	SoundStage stage;

	NewSound(store, "audio/track.mp3");
	stage.Sync(store, mixer, With("audio/track.mp3"), EAR, mixer.Format().SampleRate);

	// The start is a tenth of a second ahead, on purpose: a command applied at
	// the top of whichever block it lands in quantises to the block. So render
	// past it rather than once.
	SampleBuffer block(mixer.Format(), 512);
	float loudest = 0.0f;
	for (int rendered = 0; rendered < 32; ++rendered) {
		block.Silence();
		mixer.Render(block);
		loudest = std::max(loudest, block.Peak());
	}

	CHECK(loudest > 0.0f);
}

TEST_CASE("nothing is mixed before the scheduled start", "[client][sounds]") {
	// The other half of the same property. A `Play` carries a sample deadline
	// and `audio/AGENTS.md` names this as the one place "close enough to the
	// frame" is wrong - so a block rendered before the deadline must be silent
	// rather than nearly so.
	Store store("sounds_test.deadline");
	AudioMixer mixer;
	SoundStage stage;

	NewSound(store, "audio/track.mp3");
	stage.Sync(store, mixer, With("audio/track.mp3"), EAR, mixer.Format().SampleRate);

	// One block is 512 frames and the start is 4800 away, so this is well
	// inside the wait.
	SampleBuffer block(mixer.Format(), 512);
	mixer.Render(block);
	CHECK(block.Peak() == 0.0f);
}

TEST_CASE("clearing a stage releases every voice", "[client][sounds]") {
	Store store("sounds_test.clear");
	AudioMixer mixer;
	SoundStage stage;
	const SoundCatalogue catalogue = With("audio/track.mp3");

	NewSound(store, "audio/track.mp3");
	NewSound(store, "audio/track.mp3");
	stage.Sync(store, mixer, catalogue, EAR, mixer.Format().SampleRate);
	REQUIRE(stage.Count() == 2);

	// What a world teardown calls. Without it the nodes outlive the entities
	// they stood in for.
	stage.Clear(mixer);
	CHECK(stage.Count() == 0);
}

// --- what a script decided, arriving through `scene::AudioState` -------------
//
// **The tier seam, from the acting end.** `engine::audio` is `client` and the
// script layer is `shared`, so `SoundService` cannot reach a mixer - it writes a
// resource and this file is what reads it. These cases are the client half of
// `engine.script.soundservice`.

TEST_CASE("a world's master volume scales every voice", "[client][sounds]") {
	Store store("sounds_test.master");
	AudioMixer mixer;
	SoundStage stage;
	const SoundCatalogue catalogue = With("audio/track.mp3");

	const Entity sound = NewSound(store, "audio/track.mp3");
	store.GetMutable<engine::scene::Sound>(sound)->Volume = 0.8f;

	// No resource at all is the ordinary state of a world nobody has told, and
	// it means "as authored" rather than silence.
	stage.Sync(store, mixer, catalogue, EAR, mixer.Format().SampleRate);
	REQUIRE(stage.Find(sound) != nullptr);
	CHECK(stage.Find(sound)->Level == 0.8f);

	engine::scene::AudioState settings;
	settings.MasterVolume = 0.5f;
	store.SetResource(settings);

	// **The product, because `Level` is what was last posted** and the whole
	// file's change detection compares against that. A stage that stored the
	// sound's own volume here would post nothing on the next pass and the world
	// would stay loud.
	stage.Sync(store, mixer, catalogue, EAR, mixer.Format().SampleRate);
	CHECK(stage.Find(sound)->Level == 0.4f);

	mixer.ApplyPending();
	const engine::audio::Node *fader = mixer.Graph().Find(stage.Find(sound)->Fader);
	REQUIRE(fader != nullptr);
	CHECK(fader->Gain == 0.4f);

	// A negative gain is a phase inversion rather than a quieter sound, and a
	// script that wrote one meant silence. Clamped here rather than in the
	// property setter, so the resource keeps what was written.
	store.ResourceMutable<engine::scene::AudioState>()->MasterVolume = -2.0f;
	stage.Sync(store, mixer, catalogue, EAR, mixer.Format().SampleRate);
	CHECK(stage.Find(sound)->Level == 0.0f);
}

TEST_CASE("a world may move the ear off the camera", "[client][sounds]") {
	Store store("sounds_test.listener");
	AudioMixer mixer;
	SoundStage stage;
	const SoundCatalogue catalogue = With("audio/track.mp3");

	PartDesc where;
	where.Frame.Position = Vector3{40.0f, 0.0f, 0.0f};
	const Entity ear = MakePart(store, where);

	NewSound(store, "audio/track.mp3");

	// The default is the camera's position, which is what the caller passes.
	stage.Sync(store, mixer, catalogue, EAR, mixer.Format().SampleRate);
	mixer.ApplyPending();
	CHECK(mixer.Graph().Listener().X == 0.0f);

	engine::scene::AudioState settings;
	settings.Mode = engine::scene::ListenerMode::ObjectPosition;
	settings.Listener = ear;
	store.SetResource(settings);

	stage.Sync(store, mixer, catalogue, EAR, mixer.Format().SampleRate);
	mixer.ApplyPending();
	CHECK(mixer.Graph().Listener().X == 40.0f);

	// **A listener that has gone away falls back rather than teleporting the ear
	// to the origin.** The setting outlives the instance - a script sets it once
	// and something else destroys the part - and a scene that went quiet with
	// nothing said would be the harder of the two to explain.
	store.Destroy(ear);
	stage.Sync(store, mixer, catalogue, EAR, mixer.Format().SampleRate);
	mixer.ApplyPending();
	CHECK(mixer.Graph().Listener().X == 0.0f);
}
