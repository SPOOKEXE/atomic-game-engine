// The instance methods that are written once and installed by both VMs.
//
// **Nine to begin with, and they are the nine JavaScript did not have.** The
// layer's first act is closing a real gap rather than churning code that works:
// `Instances.cpp` held thirty methods and `JsSurface.cpp` twenty-one, and the
// pivot pair, the three tag calls and the four attribute calls were reachable
// from Luau and absent from JavaScript — while `mono.tools/bindings` declared
// every one of them in the TypeScript surface an author writes against.
//
// The other twenty-one stay where they are and migrate later, one at a time, so
// the engine works at every step. `mono.engine/script/AGENTS.md` records which of
// them are not a straight lift.
//
// **Nothing in this file names a VM, and that is the point.** A method reads its
// arguments through `ScriptCall`, calls the same `scene::` and `ecs::` functions
// a C++ system would, and answers through one `Return`. Whether the argument was
// a Luau userdata or a QuickJS object is the adapter's business.
//
// @tier L9 · shared

#include "ScriptCall.hpp"

#include <engine/ecs/Attributes.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Tagging.hpp>

#include <array>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace engine::script {

	namespace {
		using core::Name;
		using ecs::AttributeValue;

		// --- the pivot pair --------------------------------------------------
		//
		// **Roblox's pair, and the whole reason it is a pair.** A `Transform`
		// says where the *centre* of something is; almost nothing an author
		// places is placed by its centre — a door turns on its hinge, a lid sits
		// on its rim, a character stands on the ground under its feet.
		// `PivotOffset` is where the handle is and these two are how it is used.
		//
		// **Methods rather than a `Pivot` property**, which is Roblox's shape and
		// is right for a reason of its own: `GetPivot` is *derived* from two
		// fields and `PivotTo` writes a third thing entirely, so a read-write
		// property would look like storage and behave like a computation.

		// `instance:GetPivot()`
		//
		// **Not refused for a non-`PVInstance`**, matching every other method
		// here: `scene::PivotOf` answers the identity for something with no
		// placement, which is what a script asking a `Folder` for its pivot
		// should get rather than an error mid-frame.
		void GetPivot(ScriptCall &call) {
			call.ReturnCFrame(scene::PivotOf(call.World(), call.Subject()));
		}

		// `instance:PivotTo(target)`
		void PivotTo(ScriptCall &call) {
			const core::CFrame target = call.AsCFrame(0);

			// The answer is dropped on purpose: Roblox's `PivotTo` returns
			// nothing, and an instance with no placement to move is the same
			// "did nothing" a `Folder` would be.
			(void)scene::PivotTo(call.World(), call.Subject(), target);
		}

		// `instance:Equals(other)`
		//
		// **Roblox has no such method and this engine needs one, which is the
		// rare case where the two surfaces differ because a *language* does.**
		// Luau compares two instance userdata with `==` and gets the right answer
		// from the metatable. JavaScript has no operator overloading and `===` on
		// two objects is identity — and `MakeJsInstance` builds a fresh object
		// per call, so two handles to one part are never `===`. A JavaScript
		// script had no way at all to ask whether two handles name the same
		// instance.
		//
		// **The house convention already answers this**, which is why the name is
		// not invented here: `Vector3`, `Color3`, `Vector2`, `Signal` and
		// `EnumItem` all carry an `Equals` for exactly the same reason.
		// `Instance` was the one that did not, and `JsBindings.hpp`'s header
		// lists `a.Equals(b)` among the differences that *are* the language.
		//
		// Neutral rather than JavaScript-only, because a method that exists in
		// one language is the drift this layer was built to end — and on the Luau
		// side it is simply a second, longer spelling of `==`.
		void Equals(ScriptCall &call) {
			call.ReturnBoolean(call.Subject() == call.AsInstance(0));
		}

		// --- the players ------------------------------------------------------
		//
		// **On `Instance` like everything else here, and declared on `Players` and
		// `Player` in the binding file.** That is this engine's existing shape
		// rather than a new decision — `GetPlayers` has always been an instance
		// method that happens to be useful on one instance — and the type
		// declarations are where an author is told which one to call it on.
		//
		// **Written once, which is the whole point of the layer they are the
		// first new members of.** Before it these would have been two functions
		// each, in two files, in two calling conventions, and the pair would have
		// drifted the way the previous nine did.

		// `Players:GetPlayerFromCharacter(model)`
		//
		// **`CharacterOf`'s inverse and a read rather than a search.**
		// `scene::Character::Owner` already holds the answer — the field exists so
		// a server admitting a client can find the character it spawned again — so
		// this never walks every player asking whose model it is. A walk would
		// also be wrong the moment two rows disagreed.
		void GetPlayerFromCharacter(ScriptCall &call) {
			// **Nil for an NPC, which is Roblox's answer too.** Anything that is
			// not a person at a keyboard leaves `Owner` unset, and a game asking
			// "whose is this" about a wandering monster wants nil rather than an
			// error — `if player then` is how the question is written.
			call.ReturnInstance(scene::PlayerOf(call.World(), call.AsInstance(0)));
		}

		// `Player:LoadCharacter()`
		//
		// **Roblox's, and it destroys the old body first** — `scene::LoadCharacter`
		// carries that rule and this is the binding it never had. A game could
		// spawn a player only by whatever its host happened to do at admission; a
		// script had no way to respawn anybody.
		//
		// **Where it spawns is `scene::FindSpawn`'s business**, which is a part
		// named `SpawnLocation` if the world has one and the origin otherwise.
		// Roblox takes no argument here either.
		void LoadCharacter(ScriptCall &call) {
			// **The model is returned, where Roblox returns nothing.** A script
			// that has just made a body almost always wants it — the alternative
			// is reading `player.Character` on the next line and hoping the
			// assignment has landed — and a caller ignoring the answer reads
			// exactly as Roblox's does.
			call.ReturnInstance(scene::LoadCharacter(call.World(), call.Subject()));
		}

		// --- the tags ---------------------------------------------------------
		//
		// **Roblox puts these on `CollectionService` and they are methods here**,
		// which is the one place this binding departs from that vocabulary
		// deliberately. A service would need a world to be found through, and the
		// thing being tagged is already in hand; `scene::AddTag` takes the store
		// and the entity and there is nothing a service would add but a lookup.
		//
		// `AddTag` answers `false` when the world's tag table is full — see
		// `TagTable::MAXIMUM` — rather than raising, because a scene that has run
		// out of tags is a scene mistake and not a script one, and a script that
		// wanted to know can read the answer.

		// `instance:AddTag(name)`
		void AddTag(ScriptCall &call) {
			call.ReturnBoolean(scene::AddTag(call.World(), call.Subject(), Name(call.AsString(0))));
		}

		// `instance:RemoveTag(name)`
		void RemoveTag(ScriptCall &call) {
			call.ReturnBoolean(scene::RemoveTag(call.World(), call.Subject(), Name(call.AsString(0))));
		}

		// `instance:HasTag(name)`
		void HasTag(ScriptCall &call) {
			call.ReturnBoolean(scene::HasTag(call.World(), call.Subject(), Name(call.AsString(0))));
		}

		// --- the attributes ----------------------------------------------------
		//
		// **The same marshalling as a property and deliberately so.** An
		// attribute and a property are one question — what can userland hold —
		// asked at run time and at declaration time. What differs is that an
		// attribute has no descriptor, so the *type* comes from the script value
		// itself on the way in and from the stored value on the way out, which is
		// why `ScriptCall` carries an `AttributeValue` rather than a byte buffer
		// and a `PropertyType`.

		// `instance:GetAttribute(name)`
		void GetAttribute(ScriptCall &call) {
			const Name name(call.AsString(0));

			AttributeValue value;
			if (!ecs::GetAttribute(call.World(), call.Subject(), name, value)) {
				// **Nil for an attribute nobody set**, which is Roblox's answer
				// and the only one a script can act on: an error would make
				// `if part:GetAttribute("Health") then` a crash rather than a
				// test.
				call.ReturnNil();
				return;
			}

			// An `Opaque` value lands as nil through the same door — see
			// `ScriptCall::ReturnAttribute`.
			call.ReturnAttribute(value);
		}

		// `instance:SetAttribute(name, value)`
		//
		// **Omitting the value removes**, which is `SetAttribute(name, nil)` in
		// both languages and the only spelling that takes an attribute back —
		// `ecs::SetAttribute` carries the argument for why removal is not a
		// method of its own.
		void SetAttribute(ScriptCall &call) {
			const Name name(call.AsString(0));

			// An `Opaque` value is what `ecs::SetAttribute` reads as remove, and
			// leaving the default in place is how a missing argument says it.
			AttributeValue value;
			if (!call.IsNil(1)) {
				call.ReadAttribute(1, value);
				if (value.Type == ecs::PropertyType::Opaque) {
					call.Raise(
						("SetAttribute: '" + std::string(name.Text()) + "' cannot hold that type").c_str()
					);
				}
			}

			if (!ecs::SetAttribute(call.World(), call.Subject(), name, value)) {
				call.Raise(("could not set attribute '" + std::string(name.Text()) + "'").c_str());
			}

			// **Queued rather than fired**, so an attribute signals on the same
			// barrier a property does and with the same dedup —
			// `ChangeQueue::Record` carries the argument. Each language's pump is
			// what turns this into `.Changed` and into whatever
			// `GetAttributeChangedSignal` connected.
			call.Changes().Record(call.Subject(), name);
		}

		// `instance:GetAttributes()` — every attribute, as a map.
		//
		// Roblox's name and Roblox's shape: a map from name to value rather than
		// an array, because that is what a caller iterates.
		void GetAttributes(ScriptCall &call) {
			// Collected before anything is returned, because the adapter builds
			// the whole map at once — a two-call "open then add" protocol would
			// be state a method body could get wrong, and this is the surface
			// nobody should be able to get wrong twice.
			std::vector<std::pair<Name, AttributeValue>> found;
			for (const Name &name : ecs::AttributeNames(call.World(), call.Subject())) {
				AttributeValue value;
				if (!ecs::GetAttribute(call.World(), call.Subject(), name, value)) {
					continue;
				}
				found.emplace_back(name, std::move(value));
			}

			call.ReturnAttributes(found);
		}

		// `instance:GetAttributeChangedSignal(name)`
		void GetAttributeChangedSignal(ScriptCall &call) {
			// **The property-changed signal, keyed by the attribute's name.**
			// `SignalKind::PropertyChanged` already filters by a `core::Name`, and
			// an attribute name and a property name live in the same registry — so
			// a second signal kind would be a second table to fan out from for a
			// filter that already exists.
			//
			// The cost is that an attribute sharing a name with a property fires
			// both listeners. That is a collision an author can see and avoid, and
			// the alternative is a parallel signal path for a case nobody has hit.
			call.ReturnSignal(SignalKind::PropertyChanged, Name(call.AsString(0)));
		}

		// The table both VMs install.
		//
		// **Order is install order and nothing depends on it**, unlike the service
		// catalogue: a method table is a map from a name to a callable and no
		// entry can be reached before another. Grouped by what they do, so a
		// reader can see that the four attribute calls arrived together.
		constexpr std::array<InstanceMethod, 12> METHODS{{
			{"GetPivot", GetPivot},
			{"PivotTo", PivotTo},

			{"AddTag", AddTag},
			{"RemoveTag", RemoveTag},
			{"HasTag", HasTag},

			{"GetAttribute", GetAttribute},
			{"SetAttribute", SetAttribute},
			{"GetAttributes", GetAttributes},
			{"GetAttributeChangedSignal", GetAttributeChangedSignal},

			{"Equals", Equals},

			{"GetPlayerFromCharacter", GetPlayerFromCharacter},
			{"LoadCharacter", LoadCharacter},
		}};
	}

	std::span<const InstanceMethod> NeutralInstanceMethods() {
		return METHODS;
	}
}
