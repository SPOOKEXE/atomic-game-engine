// The instance methods that are written once and installed by both VMs.
//
// **Nine to begin with, and they were the nine JavaScript did not have.** The
// layer's first act was closing a real gap rather than churning code that works:
// `LuauInstances.cpp` held thirty methods and `JsSurface.cpp` twenty-one, and the
// pivot pair, the three tag calls and the four attribute calls were reachable
// from Luau and absent from JavaScript — while `mono.tools/bindings` declared
// every one of them in the TypeScript surface an author writes against.
//
// **The migration is finished, and the last twenty crossed at v0.18.** There is
// no per-VM instance method table any longer: `LuauInstances.cpp` keeps the property
// surface, the signal branches and `Instance.new`, and `JsSurface.cpp` keeps
// their JavaScript twins, and neither holds a method. Three of the twenty were
// named in `script/AGENTS.md` as not a straight lift and each is answered here
// rather than skipped —
//
//   - **`Destroy` and `ClearAllChildren`** mutate the tree under a walk and
//     release the VM's own refs while doing it. What crosses is an entity and a
//     request to forget it, which is `ScriptCall::Forget`; the *walk* is shared,
//     so a grandchild's connections can no longer outlive the row they watched
//     in one language and not the other.
//   - **`GetPropertyChangedSignal`** refused a non-scriptable name in Luau and
//     accepted one in JavaScript, because the Luau half went through a finder
//     that honoured `PropertyDescriptor::Scriptable` and the JavaScript half
//     compared `Name` and ignored it. One reader settles it, JavaScript gets the
//     stricter answer, and that is a behaviour change rather than a lift.
//   - **`SetNetworkOwner`** takes an optional instance, and the two refused
//     differently: `CheckInstance` raised for a non-instance where `JsEntityOf`
//     answered a null entity, so the JavaScript half carried a hand-written
//     guard. `IsNil` and `AsInstance` are the two questions it was really
//     asking, and asking them in that order is what makes one body correct in
//     both.
//
// **Nothing in this file names a VM, and that is the point.** A method reads its
// arguments through `ScriptCall`, calls the same `scene::` and `ecs::` functions
// a C++ system would, and answers through one `Return`. Whether the argument was
// a Luau userdata or a QuickJS object is the adapter's business.
//
// **`GuiMethods.cpp` holds the rest of the table**, and the split is by what a
// method has to reach rather than by size: the four there are the ones that name
// `engine::gui` or drive a `TweenTable`, and keeping them apart is what lets this
// file's include list stay the class tree and the store.
//
// @tier L9 · shared

#include "ScriptCall.hpp"
#include "Subtree.hpp"

#include <engine/ecs/Attributes.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/scene/Awake.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Ownership.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/Tagging.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace engine::script {

	namespace {
		using core::Name;
		using ecs::AttributeValue;
		using ecs::Entity;

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

		// `Players:GetPlayers()`
		//
		// **The `Player` children of the receiver, which on `Players` is the
		// answer Roblox gives and on anything else is an empty list.** Filtered
		// rather than returning every child, because `Players` is an ordinary
		// container: a `Folder` somebody parented there is not a player, and a
		// list that included it is a list every caller has to re-filter.
		//
		// **Neutral since v0.17, and it was the last twin in the pair this layer
		// exists to end.** It was a `lua_CFunction` in `LuauInstances.cpp` and a
		// `JSCFunction` in `JsSurface.cpp` — two walks of one container, in two
		// calling conventions, which is exactly the drift `ScriptCall.hpp`
		// opens by describing.
		void GetPlayers(ScriptCall &call) {
			const ecs::Store &store = call.World();
			const ecs::ClassId player = scene::PlayerClass();

			std::vector<ecs::Entity> players;
			store.EachChild(call.Subject(), [&](ecs::Entity child) {
				if (store.IsA(child, player)) {
					players.push_back(child);
				}
			});

			call.ReturnInstances(players);
		}

		// `Players:GetPlayerByUserId(userId)`
		//
		// **Nil for a number nobody holds**, which is Roblox's answer: a game
		// asking about somebody who has already left wants `if player then`
		// rather than an error, and that is the same shape
		// `GetPlayerFromCharacter` beside it answers with.
		void GetPlayerByUserId(ScriptCall &call) {
			call.ReturnInstance(scene::PlayerByUserId(call.World(), static_cast<int64_t>(call.AsNumber(0))));
		}

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

		// `instance:GetTags()` — every tag this instance carries, sorted.
		//
		// **Roblox has this on `Instance` and this engine only had it on the
		// service.** `CollectionService:GetTags(instance)` answered the same
		// question and `AddTag`, `RemoveTag` and `HasTag` were already on both,
		// so the instance surface was three quarters of a pair — which is the
		// shape a script copied from a Roblox place trips over.
		//
		// Empty for an instance whose class has no `Tags` component, which is
		// everything that is not a `BasePart`, and empty for a world nothing has
		// tagged. Both are "this instance carries nothing", which is what was
		// asked.
		void GetTags(ScriptCall &call) {
			const ecs::Store &store = call.World();
			const scene::Tags *tags = store.Get<scene::Tags>(call.Subject());
			const scene::TagTable *table = store.Resource<scene::TagTable>();
			if (tags == nullptr || table == nullptr) {
				call.ReturnStrings({});
				return;
			}

			// `Describe` hands them back in bit order, which is registration
			// order; sorting is what makes the answer independent of which
			// script ran first — the same rule `CollectionService:GetTags` keeps.
			std::vector<Name> names = table->Describe(tags->Mask);
			std::sort(names.begin(), names.end(), [](const Name &left, const Name &right) {
				return left.Text() < right.Text();
			});

			std::vector<std::string_view> spellings;
			spellings.reserve(names.size());
			for (const Name &name : names) {
				spellings.push_back(name.Text());
			}

			call.ReturnStrings(spellings);
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

		// --- the tree ---------------------------------------------------------
		//
		// **Every one of these is a call `Store` already had and a script could
		// not spell**, and every one of them was written twice until v0.18.
		// Nothing new happens in the storage; what is new is that one body
		// answers both languages, so `FindFirstChild("Humanoid", true)` cannot
		// mean two things again.

		// The class an argument names, or an invalid id.
		//
		// **Not raised for, matching `IsA`'s rule below**: a script naming a
		// class this game does not register is asking a question with a correct
		// answer, and every lookup below answers it with "nothing found".
		ecs::ClassId ClassArgument(ScriptCall &call, size_t index) {
			return ecs::Classes::Find(Name(call.AsString(index)));
		}

		// `instance:IsA(className)`
		void IsA(ScriptCall &call) {
			const ecs::ClassId wanted = ClassArgument(call, 0);
			if (!wanted.IsValid()) {
				// False rather than an error, matching Roblox — see
				// `ClassArgument`.
				call.ReturnBoolean(false);
				return;
			}
			call.ReturnBoolean(ecs::Classes::IsA(call.World().ClassOf(call.Subject()), wanted));
		}

		// `instance:Destroy()`
		void Destroy(ScriptCall &call) {
			const Entity instance = call.Subject();

			// **The signal table is told before the storage is**, and it is told
			// about the whole subtree. `DestroyInstance` takes every descendant,
			// so a connection anywhere under here would otherwise outlive the row
			// it watched: the ref is never given up, so the closure and
			// everything it captured stay alive for the rest of the world's life.
			call.Forget(instance);
			call.World().DestroyInstance(instance);
		}

		// `instance:ClearAllChildren()`
		void ClearAllChildren(ScriptCall &call) {
			ecs::Store &store = call.World();

			// Collected first. `DestroyInstance` unlinks from the sibling list
			// the walk is standing in, so destroying inside `EachChild` would
			// visit whatever moved into the slot — or nothing.
			std::vector<Entity> children;
			store.EachChild(call.Subject(), [&](Entity child) { children.push_back(child); });

			for (const Entity child : children) {
				// The child's whole subtree, because that is what destroying it
				// takes — forgetting only the child leaves every grandchild's
				// connections pointing at rows that no longer exist.
				call.Forget(child);
				store.DestroyInstance(child);
			}
		}

		// `instance:Clone()`
		//
		// Nil for something unclonable, matching Roblox, and a script can test
		// for it — `ReturnInstance` is what turns a null entity into each
		// language's own nothing.
		void Clone(ScriptCall &call) {
			call.ReturnInstance(call.World().CloneInstance(call.Subject()));
		}

		// `instance:GetChildren()`
		void GetChildren(ScriptCall &call) {
			const ecs::Store &store = call.World();

			std::vector<Entity> children;
			store.EachChild(call.Subject(), [&](Entity child) { children.push_back(child); });

			call.ReturnInstances(children);
		}

		// `instance:GetDescendants()`
		//
		// Depth first, children before grandchildren, which is Roblox's order
		// and the one a script writing a recursive walk by hand would produce.
		void GetDescendants(ScriptCall &call) {
			std::vector<Entity> found;
			EachDescendant(call.World(), call.Subject(), [&](Entity descendant) {
				found.push_back(descendant);
			});

			call.ReturnInstances(found);
		}

		// `instance:FindFirstChild(name, recursive)`
		//
		// **The second argument, which both halves used to read and ignore.** A
		// script calling `FindFirstChild("Humanoid", true)` got the non-recursive
		// answer — nil for anything not a direct child — and nothing said so.
		void FindFirstChild(ScriptCall &call) {
			call.ReturnInstance(
				call.World().FindFirstChild(call.Subject(), call.AsString(0), call.OptionalBoolean(1, false))
			);
		}

		// `instance:FindFirstChildOfClass(className)`
		void FindFirstChildOfClass(ScriptCall &call) {
			call.ReturnInstance(call.World().FindFirstChildOfClass(call.Subject(), ClassArgument(call, 0)));
		}

		// `instance:FindFirstChildWhichIsA(className, recursive)`
		void FindFirstChildWhichIsA(ScriptCall &call) {
			const ecs::ClassId wanted = ClassArgument(call, 0);
			call.ReturnInstance(
				call.World().FindFirstChildWhichIsA(call.Subject(), wanted, call.OptionalBoolean(1, false))
			);
		}

		// `instance:FindFirstAncestor(name)`
		void FindFirstAncestor(ScriptCall &call) {
			call.ReturnInstance(call.World().FindFirstAncestor(call.Subject(), call.AsString(0)));
		}

		// `instance:FindFirstAncestorOfClass(className)`
		void FindFirstAncestorOfClass(ScriptCall &call) {
			call.ReturnInstance(
				call.World().FindFirstAncestorOfClass(call.Subject(), ClassArgument(call, 0))
			);
		}

		// `instance:FindFirstAncestorWhichIsA(className)`
		void FindFirstAncestorWhichIsA(ScriptCall &call) {
			call.ReturnInstance(
				call.World().FindFirstAncestorWhichIsA(call.Subject(), ClassArgument(call, 0))
			);
		}

		// `instance:GetFullName()`
		void GetFullName(ScriptCall &call) {
			call.ReturnString(call.World().GetFullName(call.Subject()));
		}

		// `instance:IsDescendantOf(ancestor)`
		//
		// No case for the workspace, and losing it was the point:
		// `part:IsDescendantOf(workspace)` used to be true for every live
		// instance in the world, because the world was every root's ancestor. It
		// is the real subtree question — the same one the renderer asks — so a
		// script and the render gate cannot disagree about whether something is
		// in the scene.
		void IsDescendantOf(ScriptCall &call) {
			call.ReturnBoolean(call.World().IsDescendantOf(call.Subject(), call.AsInstance(0)));
		}

		// `instance:IsAncestorOf(descendant)`
		//
		// **Roblox's other half, and this engine only had one of them.**
		// `IsDescendantOf` reversed rather than a second walk: `Store` answers
		// the question one way and asking it the other way round is the same
		// answer with the arguments swapped, so there is nothing here for the
		// two to disagree about.
		void IsAncestorOf(ScriptCall &call) {
			call.ReturnBoolean(call.World().IsDescendantOf(call.AsInstance(0), call.Subject()));
		}

		// `instance:GetPropertyChangedSignal(name)`
		void GetPropertyChangedSignal(ScriptCall &call) {
			const std::string field = call.AsString(0);

			// Refused for a property that does not exist, which is the one place
			// a typo in a signal name can still be caught. A signal that silently
			// never fired would be indistinguishable from a value that never
			// changed.
			//
			// **`ScriptableProperty` and not a bare name compare**, which is the
			// behaviour change this method's migration carries: the JavaScript
			// half used to ignore `PropertyDescriptor::Scriptable`, so the two
			// languages disagreed about whether a script may watch a member it
			// cannot read.
			if (ScriptableProperty(call.World(), call.Subject(), field) == nullptr) {
				call.Raise(("'" + field + "' is not a valid member of this instance").c_str());
			}

			call.ReturnSignal(SignalKind::PropertyChanged, Name(field));
		}

		// --- the world's sleep -------------------------------------------------

		// `instance:KeepWorldAwake(reason)`
		void KeepWorldAwake(ScriptCall &call) {
			// **The reason is required**, which is the one thing this surface
			// insists on. A world that will not sleep costs a machine until
			// somebody works out what is holding it up, and the answer should be
			// a sentence rather than an entity id — see `scene/Awake.hpp`.
			const Name reason(call.AsString(0));
			if (!scene::KeepWorldAwake(call.World(), call.Subject(), reason)) {
				call.Raise("KeepWorldAwake needs a live instance");
			}
		}

		// `instance:LetWorldSleep()`
		void LetWorldSleep(ScriptCall &call) {
			scene::LetWorldSleep(call.World(), call.Subject());
		}

		// `instance:IsKeepingWorldAwake()`
		void IsKeepingWorldAwake(ScriptCall &call) {
			call.ReturnBoolean(scene::HoldsWorldAwake(call.World(), call.Subject()));
		}

		// --- who simulates a body ----------------------------------------------

		// `instance:SetNetworkOwner(player)`
		//
		// **Roblox's pair, spelled Roblox's way, including the nil.** Passing no
		// argument gives the body back to the server, which is what
		// `SetNetworkOwner(nil)` means there and what a script that has just seen
		// a player leave will reach for.
		//
		// The refusal raises rather than answering `false`, which departs from
		// the tag calls above and for the reason those give: a full tag table is
		// a *scene* running out of room, where handing a body to a `Folder` is a
		// script naming the wrong variable. A silent `false` there is a body
		// nothing simulates and no line of output.
		//
		// **Two things are refused and the message names both**, because a
		// script that gets this wrong is far more likely to have passed a good
		// player and an anchored part than a bad player: ownership decides who
		// runs the physics, and an anchored part has none to run.
		//
		// **`IsNil` first and `AsInstance` second, which is the whole of what
		// made this not a straight lift.** The Luau half raised for a
		// non-instance and the JavaScript half read a null entity out of
		// anything at all, so `SetNetworkOwner(5)` was a refusal in one language
		// and a silent hand-back to the server in the other until a hand-written
		// guard was added beside it. Asking the two questions in this order is
		// one body that refuses in both.
		void SetNetworkOwner(ScriptCall &call) {
			const Entity player = call.IsNil(0) ? ecs::NULL_ENTITY : call.AsInstance(0);

			if (!scene::SetNetworkOwner(call.World(), call.Subject(), player)) {
				call.Raise("SetNetworkOwner needs a Player or nil, and an unanchored part to hand over");
			}
		}

		// `instance:GetNetworkOwner()`
		//
		// Nil is "the server", which is the same answer Roblox gives and the
		// same answer an unassigned body gives. A script that wants to know which
		// of the two it is asked the wrong question.
		void GetNetworkOwner(ScriptCall &call) {
			call.ReturnInstance(scene::NetworkOwnerOf(call.World(), call.Subject()));
		}

		// The table both VMs install.
		//
		// **Order is install order and nothing depends on it**, unlike the service
		// catalogue: a method table is a map from a name to a callable and no
		// entry can be reached before another. Grouped by what they do, so a
		// reader can see that the four attribute calls arrived together.
		constexpr std::array<InstanceMethod, 36> METHODS{{
			{"GetPivot", GetPivot},
			{"PivotTo", PivotTo},

			{"AddTag", AddTag},
			{"RemoveTag", RemoveTag},
			{"HasTag", HasTag},
			{"GetTags", GetTags},

			{"GetAttribute", GetAttribute},
			{"SetAttribute", SetAttribute},
			{"GetAttributes", GetAttributes},
			{"GetAttributeChangedSignal", GetAttributeChangedSignal},

			{"Equals", Equals},

			{"GetPlayers", GetPlayers},
			{"GetPlayerByUserId", GetPlayerByUserId},
			{"GetPlayerFromCharacter", GetPlayerFromCharacter},
			{"LoadCharacter", LoadCharacter},

			{"IsA", IsA},
			{"Destroy", Destroy},
			{"ClearAllChildren", ClearAllChildren},
			{"Clone", Clone},
			{"GetChildren", GetChildren},
			{"GetDescendants", GetDescendants},
			{"FindFirstChild", FindFirstChild},
			{"FindFirstChildOfClass", FindFirstChildOfClass},
			{"FindFirstChildWhichIsA", FindFirstChildWhichIsA},
			{"FindFirstAncestor", FindFirstAncestor},
			{"FindFirstAncestorOfClass", FindFirstAncestorOfClass},
			{"FindFirstAncestorWhichIsA", FindFirstAncestorWhichIsA},
			{"GetFullName", GetFullName},
			{"IsDescendantOf", IsDescendantOf},
			{"IsAncestorOf", IsAncestorOf},
			{"GetPropertyChangedSignal", GetPropertyChangedSignal},

			{"KeepWorldAwake", KeepWorldAwake},
			{"LetWorldSleep", LetWorldSleep},
			{"IsKeepingWorldAwake", IsKeepingWorldAwake},

			{"SetNetworkOwner", SetNetworkOwner},
			{"GetNetworkOwner", GetNetworkOwner},
		}};
	}

	const ecs::PropertyDescriptor *
	ScriptableProperty(const ecs::Store &store, ecs::Entity instance, std::string_view name) {
		for (const ecs::PropertyDescriptor &property : store.PropertiesOf(instance)) {
			if (property.Spelling == name) {
				return property.Scriptable ? &property : nullptr;
			}
		}
		return nullptr;
	}

	std::span<const InstanceMethod> NeutralInstanceMethods() {
		// **One table built from two arrays, and a function-local static because
		// the order across a translation-unit boundary is otherwise nobody's.**
		// Both VMs install every row and each carries the row's *index* — the
		// Luau trampoline on an upvalue and the JavaScript one as magic — so the
		// concatenation has to happen once and hand back the same span forever.
		static const std::vector<InstanceMethod> ALL = [] {
			std::vector<InstanceMethod> rows(METHODS.begin(), METHODS.end());
			const std::span<const InstanceMethod> gui = GuiInstanceMethods();
			rows.insert(rows.end(), gui.begin(), gui.end());
			return rows;
		}();
		return ALL;
	}
}
