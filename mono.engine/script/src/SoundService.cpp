// What a world decides about how it is heard — **and almost nothing else**.
//
// Roblox's `SoundService` is seven properties and six methods. Two of the
// thirteen are here, and the other eleven are absent rather than stubbed, for
// `HttpService.cpp`'s reason: a member that exists and does nothing is a surface
// an author writes against and then finds does nothing, and worse, it *looks*
// decided, so the next reader assumes somebody thought about it. Everything left
// out is listed at the bottom of this comment with the one thing that would have
// to exist first.
//
// ## The tier is the whole shape of this file
//
// `engine::audio` is **L12 `client`** and this module is **L9 `shared`**, so
// nothing here can name a mixer, a graph or a node — `mono.build/MonoLibrary.cmake`
// fails at configure time with the edge named, and it is right to. The seam is
// the one `scene::InputState` established and `client/Sounds.hpp` already uses
// for every sound in the world: a script writes a resource on the world, and the
// client walks it and posts commands.
//
// So this file is a property surface over `scene::AudioState` and contains no
// audio at all. That is not a limitation being worked around; it is why a
// headless server runs the same script and simply changes a number nobody is
// listening to.
//
// ## `Volume` is not a Roblox property, and that is deliberate
//
// Roblox has no `SoundService.Volume`. Its answer to "turn the music down" is a
// `SoundGroup` — an instance sounds join through `Sound.SoundGroup`, with a
// `Volume` of its own — and this engine has no such class: `client::SoundStage`
// builds one fader per `Sound` straight into the output, and there is no node in
// between for a group to be. A master gain is the honest one-line version of what
// a group is for, and naming it something Roblox does not use is better than
// naming it `SoundGroup` and having it not be one.
//
// **The day `SoundGroup` exists this stays**, because it is a different question:
// a group is authored content that some sounds are in, and this is the whole
// world's level.
//
// ## What is absent, and what each would need first
//
// - **`PlayLocalSound(sound)`** — `sound.Playing = true` already *is* it. A
//   `Sound` with no parent plays and is heard everywhere at one level, and a
//   client's own world is not replicated upward, so on the side where "local"
//   means anything the property is already the whole method. Roblox's version
//   plays a *copy*, which would need something that reports a sound has finished
//   so the copy can be reaped — this engine has no such signal, so a
//   fire-and-forget copy is an entity nobody deletes. The non-replicated half
//   would need a property write the wire skips, and there is not one.
// - **`GetMixerTime()`** — the mixer's sample clock is exactly what
//   `script/AGENTS.md` refuses `os.clock` for: a script branching on it produces
//   a run that does not replay, and `just replay-check` would fail a long way
//   from the cause. `store.Time()` is the clock a world has, and it is simulated.
// - **`AmbientReverb`** — there is no filter node of any kind.
//   `audio/AGENTS.md` names reverb first in its "not here yet" list.
// - **`RolloffScale`** — `audio::EmitterPlacement` is two distances and no curve,
//   and `audio/Graph.hpp` argues for that on purpose. There is no rolloff
//   exponent to scale.
// - **`DistanceFactor` and `DopplerScale`** — both describe the Doppler effect
//   and the mixer has no Doppler node. A player's cursor is advanced by the
//   sample rate alone.
// - **`VolumetricAudio`** — an `Emitter` is a point. Emitting from the interior
//   of a part needs a shape in the placement.
// - **`RespectFilteringEnabled`** — this decides whether a client's `Play`
//   replicates to the server, and there is no client-to-server sound path at all.
// - **`OpenAttenuationCurveEditor` / `OpenDirectionalCurveEditor`** — plugin
//   security, an editor window, and curve objects none of which exist.
// - **`ListenerCFrame`, `ListenerObject`, `ListenerType`** — Roblox's newer
//   property form of `GetListener`/`SetListener`. Two spellings of one fact is
//   the debt the root `AGENTS.md` calls the most expensive kind, so only the
//   method pair is here — which is also what Roblox's own documentation still
//   tells authors to use.
//
// @tier L9 · shared
// @since v0.16

#include "Bindings.hpp"

#include <engine/ecs/EnumTable.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Audio.hpp>
#include <engine/scene/Components.hpp>

#include <algorithm>
#include <lua.h>
#include <lualib.h>
#include <string>
#include <string_view>

namespace engine::script {
	namespace {
		using scene::AudioState;
		using scene::ListenerMode;

		// Where `SoundService`'s method table lives, since the service is a
		// userdata and a userdata has no fields.
		//
		// **One constant read by both ends**, exactly as `InputServices.cpp`
		// keeps one: two spellings of this key is a service whose methods are all
		// nil, with nothing in the build to say so.
		constexpr const char *SOUND_METHODS_KEY = "engine.soundservice.methods";

		// This VM's world.
		//
		// `ContextOf` rather than `UpvalueContext` so the same helper is right
		// from a bound closure and from anywhere else — `InputServices.cpp`
		// carries the crash that taught that difference.
		ecs::Store &WorldOf(lua_State *state) {
			return *ContextOf(state).World;
		}

		// What the world decided, or the defaults for one that has never been
		// told.
		//
		// **Never creates the resource.** `PublishedCatalogue.hpp` states the
		// rule and the trap: a read that acquired a resource would put a write
		// inside every getter, and a world that answers "as authored" is exactly
		// what a world nobody has configured means.
		AudioState SettingsOf(lua_State *state) {
			const AudioState *settings = WorldOf(state).Resource<AudioState>();
			return settings == nullptr ? AudioState{} : *settings;
		}

		// The world's settings, made if it has none.
		//
		// **A write does create it**, unlike a read: a script setting
		// `SoundService.Volume` on a world with no resource would otherwise have
		// its write vanish, which reads as the property being ignored rather than
		// as the world being unfurnished.
		AudioState &MutableSettingsOf(lua_State *state) {
			ecs::Store &store = WorldOf(state);
			if (store.Resource<AudioState>() == nullptr) {
				store.SetResource(AudioState{});
			}
			return *store.ResourceMutable<AudioState>();
		}

		// `SoundService:GetListener()` -> `(Enum.ListenerType, Instance?)`
		//
		// Roblox's two-value return exactly: the mode, then whatever that mode
		// points at. `Camera` points at nothing and hands back nil, which is also
		// what `SetListener` takes for it.
		int GetListener(lua_State *state) {
			const AudioState settings = SettingsOf(state);

			PushEnumItem(
				state,
				core::Name("ListenerType"),
				ecs::EnumTable::MemberAt(core::Name("ListenerType"), static_cast<size_t>(settings.Mode))
			);

			// **Nil for an instance that has gone away**, rather than a handle to
			// a dead row. `client::SoundStage` falls back to the camera for the
			// same case, so the two agree about what the setting now means — a
			// handle here would say the ear is somewhere the mixer is not putting
			// it.
			if (settings.Mode == ListenerMode::ObjectPosition && WorldOf(state).Alive(settings.Listener)) {
				PushInstanceValue(state, settings.Listener);
			} else {
				lua_pushnil(state);
			}
			return 2;
		}

		// `SoundService:SetListener(listenerType, listener)`
		//
		// **`Enum.ListenerType` has two members here and four in Roblox**, and the
		// two missing ones are missing from the *enum* rather than refused by this
		// method — `scene/Audio.hpp` gives the reason, which is that the mixer is
		// posted a position and never a facing. So `Enum.ListenerType.CFrame` does
		// not exist to be passed, and a script that names it fails where it names
		// it rather than here.
		int SetListener(lua_State *state) {
			core::Name member;
			if (!ReadEnumValue(state, 2, core::Name("ListenerType"), member)) {
				luaL_errorL(state, "SetListener expects an Enum.ListenerType");
			}

			size_t ordinal = 0;
			if (!ecs::EnumTable::OrdinalOf(core::Name("ListenerType"), member, ordinal) ||
				ordinal >= static_cast<size_t>(ListenerMode::Count)) {
				luaL_errorL(state, "unknown ListenerType");
			}

			const auto mode = static_cast<ListenerMode>(ordinal);

			// **The instance is required for `ObjectPosition` and refused for
			// `Camera`**, rather than accepted and ignored. A script that passed
			// a part with the camera mode meant one of the two and got neither,
			// and silently keeping the mode is the outcome that is hardest to
			// notice.
			ecs::Entity listener;
			if (mode == ListenerMode::ObjectPosition) {
				listener = CheckInstanceArgument(state, 3);
			} else if (!lua_isnoneornil(state, 3)) {
				luaL_errorL(state, "SetListener takes no listener for Enum.ListenerType.Camera");
			}

			AudioState &settings = MutableSettingsOf(state);
			settings.Mode = mode;
			settings.Listener = listener;
			return 0;
		}

		// `SoundService.Volume`, read.
		//
		// **A userdata's `__index` and not a table field**, which
		// `ServiceSurface::Index` forces rather than suggests: `luaL_sandbox`
		// enables `safeenv`, so a property read off a constant global table
		// compiles to a `GETIMPORT` resolved once per closure, and a live value
		// would read as the value it had the first time anybody asked.
		int SoundServiceIndex(lua_State *state) {
			const std::string_view field = luaL_checkstring(state, 2);

			if (field == "Volume") {
				lua_pushnumber(state, SettingsOf(state).MasterVolume);
				return 1;
			}

			// The methods, from the shared table. A userdata has no fields, so
			// `InstallService` stashes them in the registry under the one key
			// `OpenSoundService` also names.
			lua_getfield(state, LUA_REGISTRYINDEX, SOUND_METHODS_KEY);
			lua_pushvalue(state, 2);
			lua_rawget(state, -2);
			if (!lua_isnil(state, -1)) {
				return 1;
			}

			luaL_errorL(state, "SoundService has no member '%s'", std::string(field).c_str());
		}

		// `SoundService.Volume`, written.
		int SoundServiceNewIndex(lua_State *state) {
			const std::string_view field = luaL_checkstring(state, 2);

			if (field == "Volume") {
				// **Stored as written, including above 1.** `Sound::Volume` is on
				// the same footing and `audio/AGENTS.md` says why: a mixer sums,
				// exceeding ±1 inside the graph is expected, and the clamp
				// happens once at the output stage. `client::SoundStage` is what
				// bounds a negative gain, which is a phase inversion rather than
				// a quieter sound.
				MutableSettingsOf(state).MasterVolume = static_cast<float>(luaL_checknumber(state, 3));
				return 0;
			}

			luaL_errorL(state, "SoundService.%s is read-only", std::string(field).c_str());
		}
	}

	void OpenSoundService(lua_State *state) {
		// **A userdata rather than a table, because it has a property.** See
		// `ServiceSurface::Index`, and `DEFERRED.md` D00030 for the edge that
		// survives it: read the service through a local, which is the form a
		// Roblox script uses anyway since `game:GetService` is a method call and
		// cannot be an import.
		//
		//     local SoundService = game:GetService("SoundService")
		//     SoundService.Volume = 0.25
		static constexpr ServiceMethod METHODS[] = {
			{"GetListener", GetListener},
			{"SetListener", SetListener},
		};

		ServiceSurface surface;
		surface.Name = "SoundService";
		surface.Methods = METHODS;
		surface.Index = SoundServiceIndex;
		surface.NewIndex = SoundServiceNewIndex;
		surface.Tag = TAG_SOUND_SERVICE;
		surface.MethodsKey = SOUND_METHODS_KEY;

		InstallService(state, surface);
	}
}
