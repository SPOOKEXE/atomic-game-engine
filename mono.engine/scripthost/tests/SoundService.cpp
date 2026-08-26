// What a script may decide about how a world is heard.
//
// **The half of the seam a script can see.** `SoundService` writes
// `scene::AudioState` and nothing else - `engine::audio` is L12 `client` and this
// module is L9 `shared`, so the binding cannot reach a mixer and does not try.
// `client.sounds` is the other half: it pins that the client acts on exactly
// these fields, and between the two suites the whole path is covered without
// either of them needing a sound card.
//
// Three kinds of case here, and the third is the one worth having:
//
//   - the property is live and round-trips, read through a local, which is the
//     only form `safeenv` leaves working
//   - the listener pair keeps what it was given, including the refusals
//   - **the eleven Roblox members that are absent stay absent.** An engine grows
//     a member back by reflex, and `SoundService.cpp` spends a screen on why each
//     one is not there. A case that names them is what makes that argument
//     something the build holds rather than something a comment asks for.

#include <engine/ecs/Store.hpp>
#include <engine/scene/Audio.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/scripthost/Runtime.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>

TEST_SUITE_ID("engine.scripthost.soundservice")
TEST_DEPENDS("engine.scene.audio")

using engine::ecs::NULL_ENTITY;
using engine::ecs::Store;
using engine::scene::AudioState;
using engine::scene::ListenerMode;
using engine::script::Language;
using engine::script::MakeRuntime;
using engine::script::Runtime;

namespace {
	// Registered before the store exists, for `ContentService`'s reason: a
	// resource id minted before the explicit registration lands takes the
	// compiler's spelling of the type and aborts when the real one arrives.
	Store Fresh(const char *name) {
		engine::scene::EnsureClassTree();
		engine::scene::RegisterSceneComponents();
		return Store(name);
	}

	void MustRun(Runtime &runtime, const char *source) {
		INFO(source);
		const bool ok = runtime.Run(source);
		INFO(runtime.LastError());
		REQUIRE(ok);
	}

	// Runs something that must fail, and hands back why.
	std::string MustFail(Runtime &runtime, const char *source) {
		INFO(source);
		REQUIRE_FALSE(runtime.Run(source));
		return runtime.LastError();
	}
}

TEST_CASE("a world nobody has told is as authored", "[scripting][sound]") {
	// **A read never creates the resource**, which is `PublishedCatalogue`'s
	// rule and the same trap: a getter that acquired one would put a write
	// inside every property read, and a headless server would grow a row for a
	// setting nothing there acts on.
	Store store = Fresh("sound_default");
	const auto runtime = MakeRuntime(store, Language::Luau);
	REQUIRE(runtime != nullptr);

	MustRun(*runtime, R"(
		local SoundService = game:GetService('SoundService')
		assert(SoundService.Volume == 1, 'a fresh world is not at full volume')

		local mode, listener = SoundService:GetListener()
		assert(mode == Enum.ListenerType.Camera, 'the default ear is not the camera')
		assert(listener == nil, 'the camera mode named something')
	)");

	CHECK(store.Resource<AudioState>() == nullptr);
}

TEST_CASE("Volume is live and reaches the resource", "[scripting][sound]") {
	Store store = Fresh("sound_volume");
	const auto runtime = MakeRuntime(store, Language::Luau);
	REQUIRE(runtime != nullptr);

	MustRun(*runtime, R"(
		-- **Through a local, which is how a Roblox script is written anyway** and
		-- is also the only form that works: `luaL_sandbox` enables `safeenv`, so
		-- a bare `Global.Field` compiles to a `GETIMPORT` that resolves once and
		-- caches the value. `DEFERRED.md` D00030 carries it.
		local SoundService = game:GetService('SoundService')
		SoundService.Volume = 0.25
		assert(SoundService.Volume == 0.25, 'Volume did not round-trip')

		-- Above 1 is legal here for `Sound.Volume`'s reason: a mixer sums, and
		-- the clamp happens once at the output stage.
		SoundService.Volume = 2
		assert(SoundService.Volume == 2, 'a volume above 1 was clamped in the wrong place')
	)");

	// The write reached the world rather than a copy. **This is the whole seam**
	// - the client reads this field and nothing else tells it what to do.
	const AudioState *settings = store.Resource<AudioState>();
	REQUIRE(settings != nullptr);
	CHECK(settings->MasterVolume == 2.0f);
}

TEST_CASE("the listener keeps what it was given", "[scripting][sound]") {
	Store store = Fresh("sound_listener");
	const auto runtime = MakeRuntime(store, Language::Luau);
	REQUIRE(runtime != nullptr);

	MustRun(*runtime, R"(
		local SoundService = game:GetService('SoundService')

		local ear = Instance.new('Part')
		ear.Name = 'Ear'
		ear.Parent = workspace

		SoundService:SetListener(Enum.ListenerType.ObjectPosition, ear)

		local mode, listener = SoundService:GetListener()
		assert(mode == Enum.ListenerType.ObjectPosition, 'the mode did not round-trip')
		assert(listener == ear, 'the listener came back as something else')

		-- Back to the camera, which takes no instance.
		SoundService:SetListener(Enum.ListenerType.Camera)
		local back, none = SoundService:GetListener()
		assert(back == Enum.ListenerType.Camera, 'the mode did not go back')
		assert(none == nil, 'the camera mode still names something')
	)");

	const AudioState *settings = store.Resource<AudioState>();
	REQUIRE(settings != nullptr);
	CHECK(settings->Mode == ListenerMode::Camera);
}

TEST_CASE("the listener refuses the two mistakes it can tell apart", "[scripting][sound]") {
	Store store = Fresh("sound_listener_refusals");
	const auto runtime = MakeRuntime(store, Language::Luau);
	REQUIRE(runtime != nullptr);

	// **`ObjectPosition` with nothing to point at.** Accepting it and keeping
	// the old instance is the outcome hardest to notice, so it raises.
	CHECK(
		MustFail(*runtime, "game:GetService('SoundService'):SetListener(Enum.ListenerType.ObjectPosition)")
			.find("Instance") != std::string::npos
	);

	// **`Camera` with an instance.** A script that passed a part meant one of
	// the two and got neither.
	CHECK(
		MustFail(
			*runtime,
			"local p = Instance.new('Part')\n"
			"game:GetService('SoundService'):SetListener(Enum.ListenerType.Camera, p)"
		)
			.find("takes no listener") != std::string::npos
	);

	// **The two Roblox modes this engine cannot honour fail where they are
	// named**, because they are absent from the enum rather than refused by the
	// method - `scene/Audio.hpp` gives the reason, which is that the mixer is
	// posted a position and never a facing.
	CHECK(MustFail(*runtime, "return Enum.ListenerType.CFrame").find("CFrame") != std::string::npos);

	CHECK(store.Resource<AudioState>() == nullptr);
}

TEST_CASE("a dead listener reads as nil", "[scripting][sound]") {
	// **The setting outlives the instance**, and `client::SoundStage` falls back
	// to the camera for exactly this case. A handle to a destroyed row here
	// would say the ear is somewhere the mixer is not putting it.
	Store store = Fresh("sound_listener_dead");
	const auto runtime = MakeRuntime(store, Language::Luau);
	REQUIRE(runtime != nullptr);

	MustRun(*runtime, R"(
		local SoundService = game:GetService('SoundService')
		local ear = Instance.new('Part')
		ear.Parent = workspace
		SoundService:SetListener(Enum.ListenerType.ObjectPosition, ear)
		ear:Destroy()

		local mode, listener = SoundService:GetListener()
		assert(mode == Enum.ListenerType.ObjectPosition, 'the mode should be unchanged')
		assert(listener == nil, 'a destroyed listener came back as a handle')
	)");
}

TEST_CASE("what SoundService deliberately does not have", "[scripting][sound]") {
	// **Absent rather than present-and-refusing**, which is `HttpService`'s
	// argument and applies here eleven times over: a member that exists and does
	// nothing looks decided, so the next reader assumes somebody thought about
	// it. `SoundService.cpp` says what each of these would need first - a filter
	// node, a Doppler node, a shape in the emitter, a rolloff curve, a
	// client-to-server sound path, a signal that a sound has finished.
	//
	// A method reads as nil and a property raises, because a userdata service's
	// `__index` has to answer something for an unknown field and nil would make
	// every typo silent.
	Store store = Fresh("sound_absent");
	const auto runtime = MakeRuntime(store, Language::Luau);
	REQUIRE(runtime != nullptr);

	for (const char *method : {"PlayLocalSound", "GetMixerTime"}) {
		INFO(method);
		const std::string source =
			std::string("assert(game:GetService('SoundService')['") + method + "'] == nil)";
		CHECK_FALSE(runtime->Run(source.c_str()));
		CHECK(runtime->LastError().find("has no member") != std::string::npos);
	}

	for (const char *property :
		 {"AmbientReverb",
		  "RolloffScale",
		  "DistanceFactor",
		  "DopplerScale",
		  "VolumetricAudio",
		  "RespectFilteringEnabled",
		  "ListenerType"}) {
		INFO(property);
		const std::string source = std::string("return game:GetService('SoundService').") + property;
		CHECK_FALSE(runtime->Run(source.c_str()));
		CHECK(runtime->LastError().find("has no member") != std::string::npos);
	}

	// And writing an unknown one says so rather than accepting it into nowhere.
	CHECK(
		MustFail(*runtime, "game:GetService('SoundService').AmbientReverb = 1").find("read-only") !=
		std::string::npos
	);
}

TEST_CASE("the absent members are absent in JavaScript too, differently", "[scripting][sound]") {
	// **The one thing about this service that could not cross, stated rather than
	// papered over.** A Luau property-bearing service is a *userdata*, so
	// `__index` has to answer something for a name nothing declares and raising
	// is the only answer that does not make a typo silent. A JavaScript object
	// has no such hook: intercepting an unknown *read* would need a `Proxy`, and
	// `JsBindings.cpp` excludes `Proxy` deliberately because a script could then
	// wrap an instance and intercept the whole property surface. So a missing
	// member reads as `undefined` here and raises there.
	//
	// **The write half does agree**, and that took the object being sealed: the
	// Luau twin is a userdata and a userdata has no fields, so an extensible
	// object would have kept `SoundService.AmbientReverb = 1` as a new property
	// and read the typo back forever. Chunks run strict, so the refused add
	// throws.
	Store store = Fresh("sound_absent_javascript");
	const auto runtime = MakeRuntime(store, Language::JavaScript);
	REQUIRE(runtime != nullptr);

	MustRun(*runtime, R"(
		const SoundService = game.GetService('SoundService')

		for (const absent of ['PlayLocalSound', 'GetMixerTime', 'AmbientReverb', 'DopplerScale']) {
			if (SoundService[absent] !== undefined) {
				throw new Error(absent + ' came back as something')
			}
		}

		if (typeof SoundService.GetListener !== 'function') {
			throw new Error('GetListener is missing')
		}
	)");

	CHECK_FALSE(runtime->Run("game.GetService('SoundService').AmbientReverb = 1"));
	CHECK_FALSE(runtime->LastError().empty());

	// A declared but read-only property refuses by name in this language, which
	// is the half that does not depend on the seal.
	CHECK_FALSE(runtime->Run("game.GetService('UserInputService').TouchEnabled = true"));
	CHECK(runtime->LastError().find("read-only") != std::string::npos);
}
