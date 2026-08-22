// The instance methods that are written once and installed by both VMs.
//
// **Nine to begin with, and they were the nine JavaScript did not have.** The
// layer's first act was closing a real gap rather than churning code that works:
// `LuauInstances.cpp` held thirty methods and `JsSurface.cpp` twenty-one, and the
// pivot pair, the three tag calls and the four attribute calls were reachable
// from Luau and absent from JavaScript - while `mono.tools/bindings` declared
// every one of them in the TypeScript surface an author writes against.
//
// **The migration is finished, and the last twenty crossed at v0.18.** There is
// no per-VM instance method table any longer: `LuauInstances.cpp` keeps the property
// surface, the signal branches and `Instance.new`, and `JsSurface.cpp` keeps
// their JavaScript twins, and neither holds a method. Three of the twenty were
// named in `script/AGENTS.md` as not a straight lift and each is answered here
// rather than skipped -
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
// **`WaitForChild` is the thirty-seventh and the only one that suspends.**
// Every other row answers from the store on the tick it was called; this one
// registers in `ChildWaiters` and hands the script back at the barrier, either
// with the child that arrived or with nothing once its deadline has passed. What
// that cost the interface is two members - a table accessor and `AwaitChild` -
// and what it cost the *surface* is Roblox's no-timeout form, which is refused
// here rather than approximated. The method's own comment carries that decision.
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

#include <engine/ecs/Attributes.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/effects/ParticleSystem.hpp>
#include <engine/scene/Awake.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/EditableImage.hpp>
#include <engine/scene/EditableMesh.hpp>
#include <engine/scene/Ownership.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/Tagging.hpp>
#include <engine/script/ScriptCall.hpp>
#include <engine/script/Subtree.hpp>
#include <engine/script/Tasks.hpp>

#include <algorithm>
#include <array>
#include <cmath>
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
		// places is placed by its centre - a door turns on its hinge, a lid sits
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

		// `workspace:BulkMoveTo(parts, placements)`
		//
		// **Roblox's `WorldRoot:BulkMoveTo`**, and it is here for the reason
		// `scene::BulkMoveTo` gives: a per-part `CFrame` write crosses the
		// language boundary once per part, and the boundary is most of the cost.
		// `ScriptableProperty` alone is a string compare against every property
		// the class has, per write.
		//
		// **The subject is not checked and carries nothing.** Roblox puts this
		// on `WorldRoot` because that is where a spatial API belongs there; here
		// every part is named in the argument, so there is no world to infer and
		// nothing for a `Workspace` to add. Accepting any subject means a script
		// written against Roblox works unchanged, which is the direction this
		// surface is allowed to differ in.
		//
		// **A length mismatch raises rather than moving the shorter list.**
		// `scene::BulkMoveTo` takes the shorter of the two because a C++ caller
		// with two spans has already decided what it means; a script that built
		// two tables of different lengths has made a mistake, and moving half
		// the parts hides it until somebody notices the other half never left.
		void BulkMoveTo(ScriptCall &call) {
			std::vector<ecs::Entity> parts;
			std::vector<core::CFrame> placements;

			if (!call.ReadPlacements(0, parts, placements)) {
				call.Raise("BulkMoveTo needs as many placements as parts");
			}

			// The count is dropped: a list naming something with no placement is
			// the same "did nothing" `PivotTo` above allows for a `Folder`, and
			// Roblox's returns nothing either.
			(void)scene::BulkMoveTo(call.World(), parts, placements);
		}

		// `workspace:BulkPivotTo(parts, targets)`
		//
		// `BulkMoveTo` for handles. **Not a Roblox method**, which is worth
		// saying: Roblox has `BulkMoveTo` and no bulk pivot, so a script written
		// here that uses this will not run there. It exists because the engine's
		// own single-instance pair is `CFrame =` *and* `PivotTo`, and offering a
		// batch for one of them would push every author of a model - the case a
		// pivot is for - back onto the per-instance path this exists to get off.
		void BulkPivotTo(ScriptCall &call) {
			std::vector<ecs::Entity> parts;
			std::vector<core::CFrame> targets;

			if (!call.ReadPlacements(0, parts, targets)) {
				call.Raise("BulkPivotTo needs as many targets as parts");
			}

			(void)scene::BulkPivotTo(call.World(), parts, targets);
		}

		// `instance:Equals(other)`
		//
		// **Roblox has no such method and this engine needs one, which is the
		// rare case where the two surfaces differ because a *language* does.**
		// Luau compares two instance userdata with `==` and gets the right answer
		// from the metatable. JavaScript has no operator overloading and `===` on
		// two objects is identity - and `MakeJsInstance` builds a fresh object
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
		// one language is the drift this layer was built to end - and on the Luau
		// side it is simply a second, longer spelling of `==`.
		void Equals(ScriptCall &call) {
			call.ReturnBoolean(call.Subject() == call.AsInstance(0));
		}

		// --- the players ------------------------------------------------------
		//
		// **On `Instance` like everything else here, and declared on `Players` and
		// `Player` in the binding file.** That is this engine's existing shape
		// rather than a new decision - `GetPlayers` has always been an instance
		// method that happens to be useful on one instance - and the type
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
		// `JSCFunction` in `JsSurface.cpp` - two walks of one container, in two
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
		// `scene::Character::Owner` already holds the answer - the field exists so
		// a server admitting a client can find the character it spawned again - so
		// this never walks every player asking whose model it is. A walk would
		// also be wrong the moment two rows disagreed.
		void GetPlayerFromCharacter(ScriptCall &call) {
			// **Nil for an NPC, which is Roblox's answer too.** Anything that is
			// not a person at a keyboard leaves `Owner` unset, and a game asking
			// "whose is this" about a wandering monster wants nil rather than an
			// error - `if player then` is how the question is written.
			call.ReturnInstance(scene::PlayerOf(call.World(), call.AsInstance(0)));
		}

		// `Player:LoadCharacter()`
		//
		// **Roblox's, and it destroys the old body first** - `scene::LoadCharacter`
		// carries that rule and this is the binding it never had. A game could
		// spawn a player only by whatever its host happened to do at admission; a
		// script had no way to respawn anybody.
		//
		// **Where it spawns is `scene::FindSpawn`'s business**, which is a part
		// named `SpawnLocation` if the world has one and the origin otherwise.
		// Roblox takes no argument here either.
		void LoadCharacter(ScriptCall &call) {
			// **The model is returned, where Roblox returns nothing.** A script
			// that has just made a body almost always wants it - the alternative
			// is reading `player.Character` on the next line and hoping the
			// assignment has landed - and a caller ignoring the answer reads
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
		// `AddTag` answers `false` when the world's tag table is full - see
		// `TagTable::MAXIMUM` - rather than raising, because a scene that has run
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

		// `instance:GetTags()` - every tag this instance carries, sorted.
		//
		// **Roblox has this on `Instance` and this engine only had it on the
		// service.** `CollectionService:GetTags(instance)` answered the same
		// question and `AddTag`, `RemoveTag` and `HasTag` were already on both,
		// so the instance surface was three quarters of a pair - which is the
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
			// script ran first - the same rule `CollectionService:GetTags` keeps.
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
		// attribute and a property are one question - what can userland hold -
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

			// An `Opaque` value lands as nil through the same door - see
			// `ScriptCall::ReturnAttribute`.
			call.ReturnAttribute(value);
		}

		// `instance:SetAttribute(name, value)`
		//
		// **Omitting the value removes**, which is `SetAttribute(name, nil)` in
		// both languages and the only spelling that takes an attribute back -
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
			// barrier a property does and with the same dedup -
			// `ChangeQueue::Record` carries the argument. Each language's pump is
			// what turns this into `.Changed` and into whatever
			// `GetAttributeChangedSignal` connected.
			call.Changes().Record(call.Subject(), name);
		}

		// `instance:GetAttributes()` - every attribute, as a map.
		//
		// Roblox's name and Roblox's shape: a map from name to value rather than
		// an array, because that is what a caller iterates.
		void GetAttributes(ScriptCall &call) {
			// Collected before anything is returned, because the adapter builds
			// the whole map at once - a two-call "open then add" protocol would
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
			// an attribute name and a property name live in the same registry - so
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
				// False rather than an error, matching Roblox - see
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

			// **Authored, so a fixture refuses.** A script destroying
			// `Lighting` turns every `game:GetService` in the place into a
			// runtime error a long way from the delete that caused it, and
			// `scene::ServiceComponent::Fixture` has said so since v0.7 with
			// nothing reading it.
			if (!call.World().DestroyAuthored(instance)) {
				// **The connections are already given up and that is correct.**
				// `Forget` releases what a script had attached to this instance,
				// and a refused destroy leaves the instance rather than the
				// handlers - which is the same state a script would be in after
				// disconnecting them itself. Re-attaching them would mean the
				// signal table remembering what it had, which is a second
				// record of something the VM owns.
				return;
			}
		}

		// `instance:ClearAllChildren()`
		void ClearAllChildren(ScriptCall &call) {
			ecs::Store &store = call.World();

			// Collected first. `DestroyInstance` unlinks from the sibling list
			// the walk is standing in, so destroying inside `EachChild` would
			// visit whatever moved into the slot - or nothing.
			std::vector<Entity> children;
			store.EachChild(call.Subject(), [&](Entity child) { children.push_back(child); });

			for (const Entity child : children) {
				// The child's whole subtree, because that is what destroying it
				// takes - forgetting only the child leaves every grandchild's
				// connections pointing at rows that no longer exist.
				call.Forget(child);
				store.DestroyInstance(child);
			}
		}

		// `instance:Clone()`
		//
		// Nil for something unclonable, matching Roblox, and a script can test
		// for it - `ReturnInstance` is what turns a null entity into each
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
		// answer - nil for anything not a direct child - and nothing said so.
		void FindFirstChild(ScriptCall &call) {
			call.ReturnInstance(
				call.World().FindFirstChild(call.Subject(), call.AsString(0), call.OptionalBoolean(1, false))
			);
		}

		// `instance:WaitForChild(name, timeout)`
		//
		// **The timeout is required, and Roblox's no-timeout form raises rather
		// than waiting for ever.** That is a deliberate divergence from every
		// script that would be ported in, and it is the only honest one
		// available: Roblox's `WaitForChild(name)` waits until the child
		// arrives - warning after five seconds and otherwise never giving up -
		// and a script that waits for ever is a script that never finishes its
		// tick. `Runtime::Run` refuses a suspended thread that nothing will
		// resume, `script/AGENTS.md` records that as a decision rather than a
		// gap, and rule 5 is the whole of the argument: work inside a tick may
		// be parallel, work across ticks may not. A wait with no end is work
		// across an unbounded number of them.
		//
		// **Refused at the call site rather than approximated**, which is
		// `HttpService.cpp`'s and `SoundService.cpp`'s rule applied to an
		// *argument* instead of a member. The two approximations both look
		// reasonable and are worse than a refusal: a silent default timeout
		// gives a script that ported cleanly a nil it never checks for, and a
		// version that returned `FindFirstChild`'s answer immediately
		// typechecks, works in every scene where the child is already there, and
		// answers nil in exactly the case the method exists for.
		//
		// So an author porting a place gets an error naming the fix, once, on
		// the line that needs it - and `mono.tools/bindings` declares the second
		// argument as required, so the Luau and TypeScript halves both refuse it
		// before the script ever runs.
		void WaitForChild(ScriptCall &call) {
			ecs::Store &store = call.World();
			const std::string name = call.AsString(0);

			// **Roblox's own first step, and it is not the cheap version of this
			// method.** A child that is already there is answered on the tick it
			// was asked for; only a miss suspends, which is the half a lookup
			// cannot do.
			if (const Entity found = store.FindFirstChild(call.Subject(), name); found != ecs::NULL_ENTITY) {
				call.ReturnInstance(found);
				return;
			}

			if (call.IsNil(1)) {
				call.Raise(
					"WaitForChild needs a timeout in seconds, because this engine has no "
					"unbounded wait: work does not cross a tick here, so pass "
					"WaitForChild(name, seconds) and handle a nil answer"
				);
			}

			// **A deadline in ticks, through the same `TicksFor` `task.wait` and
			// `Debris:AddItem` use.** Seconds is what an author means and what
			// Roblox takes; ticks is what a deadline has to be, or the same
			// script gives up after a different amount of simulation on a busy
			// machine than on an idle one.
			const uint64_t waiter = call.Waiters().Add(
				call.Subject(), name, store.Time().Tick + TicksFor(store, call.AsNumber(1))
			);

			if (waiter == 0) {
				// Refused rather than evicting somebody else's wait - see
				// `ChildWaiters::Add`, which carries the direction and why.
				call.Raise("too many instances are waiting for a child at once");
			}

			call.AwaitChild(waiter);
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
		// is the real subtree question - the same one the renderer asks - so a
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
			// a sentence rather than an entity id - see `scene/Awake.hpp`.
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

		// A `Vector3` argument, raising with `method`'s own name when the
		// value is not one - `AttributeValue` carries every datatype
		// `ScriptCall::AsCFrame` and its siblings do not, and this is the
		// door onto it for the one that `AddVertex` and its neighbours need.
		core::Vector3 AsVector3(ScriptCall &call, size_t index, const char *method) {
			ecs::AttributeValue value;
			call.ReadAttribute(index, value);
			if (value.Type != ecs::PropertyType::Vector3) {
				call.Raise((std::string(method) + " needs a Vector3").c_str());
			}
			return value.Vector3;
		}

		// The same, for `Vector2`.
		core::Vector2 AsVector2(ScriptCall &call, size_t index, const char *method) {
			ecs::AttributeValue value;
			call.ReadAttribute(index, value);
			if (value.Type != ecs::PropertyType::Vector2) {
				call.Raise((std::string(method) + " needs a Vector2").c_str());
			}
			return value.Vector2;
		}

		// The same, for `Color3`.
		core::Color3 AsColor3(ScriptCall &call, size_t index, const char *method) {
			ecs::AttributeValue value;
			call.ReadAttribute(index, value);
			if (value.Type != ecs::PropertyType::Color3) {
				call.Raise((std::string(method) + " needs a Color3").c_str());
			}
			return value.Color3;
		}

		// `editableMesh:AddVertex(position, normal?, uv?)`
		void EditableMeshAddVertex(ScriptCall &call) {
			const core::Vector3 position = AsVector3(call, 0, "AddVertex");
			const core::Vector3 normal = call.Arguments() > 1 && !call.IsNil(1)
											 ? AsVector3(call, 1, "AddVertex")
											 : core::Vector3{0.0f, 1.0f, 0.0f};
			const core::Vector2 uv =
				call.Arguments() > 2 && !call.IsNil(2) ? AsVector2(call, 2, "AddVertex") : core::Vector2{};

			const auto id = scene::AddVertex(call.World(), call.Subject(), position, normal, uv);
			if (!id.has_value()) {
				call.Raise("AddVertex needs an EditableMesh");
			}
			call.ReturnNumber(static_cast<double>(*id));
		}

		// `editableMesh:AddTriangle(a, b, c)`
		//
		// **Raises for the wrong instance and returns `nil` for a bad
		// vertex id, and the two are told apart rather than folded
		// together.** The first is a script calling this on something that
		// was never going to work; the second is the ordinary shape of
		// building a mesh from computed indices, where asking "did that
		// work" is ordinary control flow and not a bug to stop the script
		// over.
		void EditableMeshAddTriangle(ScriptCall &call) {
			if (call.World().Get<scene::EditableMesh>(call.Subject()) == nullptr) {
				call.Raise("AddTriangle needs an EditableMesh");
			}

			const auto a = static_cast<uint32_t>(call.AsNumber(0));
			const auto b = static_cast<uint32_t>(call.AsNumber(1));
			const auto c = static_cast<uint32_t>(call.AsNumber(2));

			const auto id = scene::AddTriangle(call.World(), call.Subject(), a, b, c);
			if (!id.has_value()) {
				call.ReturnNil();
				return;
			}
			call.ReturnNumber(static_cast<double>(*id));
		}

		// `editableMesh:RemoveTriangle(id)`
		//
		// **Returns whether it removed one, and does not raise on a
		// range failure** - `RemoveTriangle`'s own header explains the
		// swap-and-pop this can otherwise silently walk into, and a script
		// asking "did that work" is the ordinary way to stay clear of it.
		void EditableMeshRemoveTriangle(ScriptCall &call) {
			const auto id = static_cast<uint32_t>(call.AsNumber(0));
			call.ReturnBoolean(scene::RemoveTriangle(call.World(), call.Subject(), id));
		}

		// `editableMesh:SetVertexPosition(id, position)`
		void EditableMeshSetVertexPosition(ScriptCall &call) {
			const auto id = static_cast<uint32_t>(call.AsNumber(0));
			const core::Vector3 position = AsVector3(call, 1, "SetVertexPosition");
			call.ReturnBoolean(scene::SetVertexPosition(call.World(), call.Subject(), id, position));
		}

		// `editableMesh:SetVertexNormal(id, normal)`
		void EditableMeshSetVertexNormal(ScriptCall &call) {
			const auto id = static_cast<uint32_t>(call.AsNumber(0));
			const core::Vector3 normal = AsVector3(call, 1, "SetVertexNormal");
			call.ReturnBoolean(scene::SetVertexNormal(call.World(), call.Subject(), id, normal));
		}

		// `editableMesh:SetVertexUV(id, uv)`
		void EditableMeshSetVertexUV(ScriptCall &call) {
			const auto id = static_cast<uint32_t>(call.AsNumber(0));
			const core::Vector2 uv = AsVector2(call, 1, "SetVertexUV");
			call.ReturnBoolean(scene::SetVertexUV(call.World(), call.Subject(), id, uv));
		}

		// `editableMesh:SetVertexColor(id, colour, alpha?)`
		void EditableMeshSetVertexColor(ScriptCall &call) {
			const auto id = static_cast<uint32_t>(call.AsNumber(0));
			const core::Color3 colour = AsColor3(call, 1, "SetVertexColor");
			const float alpha =
				call.Arguments() > 2 && !call.IsNil(2) ? static_cast<float>(call.AsNumber(2)) : 0.0f;
			call.ReturnBoolean(scene::SetVertexColor(call.World(), call.Subject(), id, colour, alpha));
		}

		// `editableMesh:Clear()` or `particleEmitter:Clear()`
		void EditableMeshClear(ScriptCall &call) {
			if (call.World().Get<effects::ParticleEmitter>(call.Subject()) != nullptr) {
				call.ReturnBoolean(effects::ClearParticles(call.World(), call.Subject()));
				return;
			}
			call.ReturnBoolean(scene::ClearEditableMesh(call.World(), call.Subject()));
		}

		// `particleEmitter:Emit(count)`
		void ParticleEmitterEmit(ScriptCall &call) {
			const double requested = call.AsNumber(0);
			if (!std::isfinite(requested) || requested < 0.0 || requested > 4294967295.0 ||
				std::floor(requested) != requested) {
				call.Raise("Emit needs a non-negative whole particle count");
			}
			if (!effects::EmitParticles(call.World(), call.Subject(), static_cast<uint32_t>(requested))) {
				call.Raise("Emit needs a ParticleEmitter");
			}
		}

		// Raises unless the subject is an `EditableImage` - the four methods
		// below share this guard rather than each spelling it, because
		// their own return value is already spoken for: `false` means "this
		// EditableImage refused the call" - an absurd `Resize`, mainly -
		// and folding "the wrong kind of instance entirely" into the same
		// boolean would make the two indistinguishable from a script that
		// only checked the result.
		void RequireEditableImage(ScriptCall &call, const char *method) {
			if (call.World().Get<scene::EditableImage>(call.Subject()) == nullptr) {
				call.Raise((std::string(method) + " needs an EditableImage").c_str());
			}
		}

		// `editableImage:Resize(width, height)`
		void EditableImageResize(ScriptCall &call) {
			RequireEditableImage(call, "Resize");
			const auto width = static_cast<uint32_t>(call.AsNumber(0));
			const auto height = static_cast<uint32_t>(call.AsNumber(1));
			call.ReturnBoolean(scene::ResizeEditableImage(call.World(), call.Subject(), width, height));
		}

		// `editableImage:DrawRectangle(position, size, colour, transparency?)`
		void EditableImageDrawRectangle(ScriptCall &call) {
			RequireEditableImage(call, "DrawRectangle");
			const core::Vector2 position = AsVector2(call, 0, "DrawRectangle");
			const core::Vector2 size = AsVector2(call, 1, "DrawRectangle");
			const core::Color3 colour = AsColor3(call, 2, "DrawRectangle");
			const float transparency =
				call.Arguments() > 3 && !call.IsNil(3) ? static_cast<float>(call.AsNumber(3)) : 0.0f;
			call.ReturnBoolean(
				scene::DrawRectangle(call.World(), call.Subject(), position, size, colour, transparency)
			);
		}

		// `editableImage:DrawLine(from, to, colour, transparency?)`
		void EditableImageDrawLine(ScriptCall &call) {
			RequireEditableImage(call, "DrawLine");
			const core::Vector2 from = AsVector2(call, 0, "DrawLine");
			const core::Vector2 to = AsVector2(call, 1, "DrawLine");
			const core::Color3 colour = AsColor3(call, 2, "DrawLine");
			const float transparency =
				call.Arguments() > 3 && !call.IsNil(3) ? static_cast<float>(call.AsNumber(3)) : 0.0f;
			call.ReturnBoolean(scene::DrawLine(call.World(), call.Subject(), from, to, colour, transparency));
		}

		// `editableImage:DrawCircle(centre, radius, colour, transparency?)`
		void EditableImageDrawCircle(ScriptCall &call) {
			RequireEditableImage(call, "DrawCircle");
			const core::Vector2 centre = AsVector2(call, 0, "DrawCircle");
			const auto radius = static_cast<float>(call.AsNumber(1));
			const core::Color3 colour = AsColor3(call, 2, "DrawCircle");
			const float transparency =
				call.Arguments() > 3 && !call.IsNil(3) ? static_cast<float>(call.AsNumber(3)) : 0.0f;
			call.ReturnBoolean(
				scene::DrawCircle(call.World(), call.Subject(), centre, radius, colour, transparency)
			);
		}

		// `part:SetLocalTransparency(value)`
		//
		// **The one door onto `scene::LocalTransparency`, and it is a method
		// rather than a property write for the reason its own header gives:**
		// `part.LocalTransparency = x` would go through `Store::SetProperty`,
		// which refuses every write on a replica regardless of which property -
		// and a viewer fading their own character, standing in a world they do
		// not own, is exactly the case this exists for. `Instance:SetAttribute`
		// already asks an author to accept a method for the same reason.
		//
		// Reading is the ordinary property table: `part.LocalTransparency`
		// works everywhere, because a read is never refused.
		void SetLocalTransparency(ScriptCall &call) {
			const float value = static_cast<float>(call.AsNumber(0));
			if (!scene::SetLocalTransparency(call.World(), call.Subject(), value)) {
				call.Raise("SetLocalTransparency needs a BasePart");
			}
		}

		// The table both VMs install.
		//
		// **Order is install order and nothing depends on it**, unlike the service
		// catalogue: a method table is a map from a name to a callable and no
		// entry can be reached before another. Grouped by what they do, so a
		// reader can see that the four attribute calls arrived together.
		constexpr std::array<InstanceMethod, 53> SCRIPT_METHODS{{
			{"GetPivot", GetPivot},
			{"PivotTo", PivotTo},
			{"BulkMoveTo", BulkMoveTo},
			{"BulkPivotTo", BulkPivotTo},
			{"SetLocalTransparency", SetLocalTransparency},

			{"AddVertex", EditableMeshAddVertex},
			{"AddTriangle", EditableMeshAddTriangle},
			{"RemoveTriangle", EditableMeshRemoveTriangle},
			{"SetVertexPosition", EditableMeshSetVertexPosition},
			{"SetVertexNormal", EditableMeshSetVertexNormal},
			{"SetVertexUV", EditableMeshSetVertexUV},
			{"SetVertexColor", EditableMeshSetVertexColor},
			{"Clear", EditableMeshClear},
			{"Emit", ParticleEmitterEmit},

			{"Resize", EditableImageResize},
			{"DrawRectangle", EditableImageDrawRectangle},
			{"DrawLine", EditableImageDrawLine},
			{"DrawCircle", EditableImageDrawCircle},

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
			{"WaitForChild", WaitForChild},
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
		// Both VMs install every row and each carries the row's *index* - the
		// Luau trampoline on an upvalue and the JavaScript one as magic - so the
		// concatenation has to happen once and hand back the same span forever.
		static const std::vector<InstanceMethod> ALL = [] {
			std::vector<InstanceMethod> rows(SCRIPT_METHODS.begin(), SCRIPT_METHODS.end());
			const std::span<const InstanceMethod> gui = GuiInstanceMethods();
			rows.insert(rows.end(), gui.begin(), gui.end());
			return rows;
		}();
		return ALL;
	}
}
