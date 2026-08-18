#include <engine/ecs/Components.hpp>
#include <engine/ecs/TypeDescriptor.hpp>
#include <engine/replication/Defaults.hpp>

#include <string>
#include <vector>

namespace engine::replication {

	namespace {
		// The prefixes a world's shared state lives under.
		//
		// **`scene.` is the game's state and `gui.` is the game's interface**,
		// and the two are one question asked in three dimensions and in two.
		// `physics.` is derived from the shared state every tick and
		// reconstructed on the far side; a module that wants its components on
		// the wire says so rather than inheriting it from a naming convention.
		//
		// **`gui.` joined at v0.15 and that is what makes an authored interface
		// arrive.** A `ScreenGui` crossed as a name and a class with no
		// `gui.Element` on it, so a client held the shape of an interface and
		// none of it - which reads as the server having authored nothing.
		constexpr std::string_view SHARED_PREFIXES[] = {"scene.", "gui."};

		bool UnderASharedPrefix(std::string_view component) {
			for (const std::string_view prefix : SHARED_PREFIXES) {
				if (component.starts_with(prefix)) {
					return true;
				}
			}
			return false;
		}

		// What an instance *is*, and all three of them named rather than a
		// prefix.
		//
		// **`ecs.` used to be a prefix with one exception written into it, and
		// the exception is how two components went missing.** The rule read
		// "`scene.` or `ecs.Hierarchy`", so `ecs.InstanceName` and
		// `ecs.InstanceClass` crossed in a join snapshot - `Store::Save` carries
		// every component - and never in a delta. An entity the world already
		// held arrived named, an entity created while a client was connected
		// arrived with no name and no class, and nothing said so: a client's own
		// `Player` had four children whose names were empty strings.
		//
		// A prefix cannot fail loudly. This list can: `engine.replication.defaults`
		// walks every registered `ecs.` component and requires each one to be
		// either here or in the exclusions it names, so the next component added
		// to `ecs` is a red test rather than a silence. `AGENTS.md` rule 6.
		//
		// **Once per change, not once per creation, and the difference is the
		// v0.7 bug again.** Saying a name in the `Structure` message that creates
		// the entity would be one reliable copy and nothing per tick after it -
		// but `.Name` is a writable property a script sets whenever it likes, and
		// a fact that crosses only at birth is exactly the shape of the part that
		// was recoloured at runtime and kept its old colour on every client for
		// ever. So they are signed like every other write-once component, which
		// costs a hash of two four-byte columns per tick and **zero bytes on a
		// tick where nothing was renamed**. The recovery walk is what guarantees
		// arrival: a value stays in `Unconfirmed` and is re-offered every tick
		// until the client acknowledges a tick it was in, which is the same
		// promise every other replicated value gets and needs no second one.
		//
		// That is also the answer to what a repeated class name costs. It is
		// text on a wire - `ecs.InstanceClass` crosses as `Classes::Describe`'s
		// name, because a `ClassId` is a registration index and rule 4 forbids
		// one - but it crosses on the tick an instance is created and on no
		// other, so a per-connection string table would be a dictionary
		// negotiated to compress a message that is already only sent once. The
		// steady-state saving would be zero.
		constexpr std::string_view INSTANCE_COMPONENTS[] = {
			"ecs.Hierarchy",
			"ecs.InstanceName",
			"ecs.InstanceClass",
		};

		bool PartOfAnInstance(std::string_view component) {
			for (const std::string_view named : INSTANCE_COMPONENTS) {
				if (component == named) {
					return true;
				}
			}
			return false;
		}

		// What a script *is*, named rather than taken by prefix - and the one it
		// leaves out is the reason why.
		//
		// **`script.SourceCache` must never be here.** It is a world resource:
		// one table of every program the world holds, keyed by path, with no
		// entity to hang it on. Interest filters entities, so a resource can
		// only cross whole - and whole means `ServerScriptService`'s programs on
		// every client, which is the leak `scene::VisibleToClients` was added to
		// close for the instances themselves. `script.Program` is the row that
		// crosses instead: it sits on the instance, so it is hidden by exactly
		// the rule that hides the instance, and `script::MirrorSourcePrograms`
		// never writes one for a `Script`.
		//
		// A prefix could not have said that, and the suite is what keeps it
		// true: `engine.replication.defaults` walks every registered `script.`
		// component and requires each to be classified here or named in the
		// exclusions it carries.
		constexpr std::string_view SCRIPT_COMPONENTS[] = {
			"script.LuaSourceContainer",
			"script.JavaScriptSourceContainer",
			"script.CodeSourceContainerSelector",
			"script.Disabled",
			"script.Program",
		};

		bool PartOfAScript(std::string_view component) {
			for (const std::string_view named : SCRIPT_COMPONENTS) {
				if (component == named) {
					return true;
				}
			}
			return false;
		}

		// The two written by a system every tick, so the dirty bits already
		// know.
		//
		// Hashing either would be a pass over the world to learn what was free.
		// Everything else is written once by a script and then never - observing
		// those buys a dirty column paid every tick and read never, and *not*
		// signing them is the v0.7 bug where a part recoloured at runtime kept
		// its old colour on every client for ever.
		//
		// **Tried widening this to everything a grep for `SetComponent`/`Set<T>`
		// found no write site for, at v0.18, and reverted it the same day.**
		// `scene.Visual` and `scene.Bounds` passed that search and are exactly
		// as write-once as `scene.Transform` looked from the same angle - and
		// `mono.unified_server_client/tests/Harness.cpp` still failed five cases,
		// because a real system reaches both through `Store::GetMutable<T>` and
		// `Store::EachBatch<T>`, which hand out raw column pointers and set no
		// bit *by design* - the cost that path exists to avoid is exactly the
		// per-row check `Observed` would add back. A grep across call sites
		// cannot rule that out for a component it has not been told is safe;
		// only a system's own author can say a component is never reached that
		// way, and none of the ten beyond `Transform` and `Motion` have made
		// that claim. `Authority::Resign`'s profile against
		// `examples/ReplicationStress.luau` found the two functions above
		// costing roughly 3% of the tick each at twenty thousand carriers - real
		// and worth reducing, but not by guessing a component's write path from
		// outside the module that owns it.
		bool WrittenEveryTick(std::string_view component) {
			return component == "scene.Transform" || component == "scene.Motion";
		}

		// The three a signature cannot cover, because a signature hashes the
		// object representation.
		//
		// **A `std::string` is a pointer in its object representation, so
		// hashing one answers about the allocation and not about the text.**
		// Two boxes holding the same words hash differently, one box hashes
		// differently after a reallocation that did not change a character, and
		// - the failure that matters - text edited in place inside a capacity
		// that did not move hashes the *same*. `Authority::Resign` refuses a
		// non-trivial type outright and says to observe it instead; these are
		// the three that took it up on that.
		//
		// **What `Observed` costs is a dirty bit per row and nothing per tick**,
		// which is the opposite trade from `Signature` and the right one here:
		// a label's text and a script's program are written by an author and
		// then left alone for the life of the world, so the bit is set on the
		// tick somebody wrote and never again. A signature over
		// `script.Program` would be a hash of every kilobyte of Luau in the
		// world, sixty times a second, to learn that nobody had touched it.
		//
		// `Authority::Survey` is what turns the observation on, so declaring one
		// here is the whole of the wiring.
		bool CannotBeSigned(std::string_view component) {
			// **`scene.EditableMesh` joins this list for the same reason as
			// the three above it, arriving at a different scale.** Five
			// `std::vector`s are five pointers in the object representation
			// a signature would hash - it would answer about the allocation
			// and never about a vertex actually moving. `Observed` is exactly
			// right here rather than merely convenient: every mutator in
			// `scene/EditableMesh.hpp` reaches the row through `Store::
			// GetMutable`, which marks the dirty bit for free - there is no
			// `EachBatch` or raw-pointer door for this type the way there is
			// for `Visual` and `Bounds`, so there is no hole `Observed`
			// leaves open for it.
			return component == "gui.Label" || component == "gui.Entry" || component == "script.Program" ||
				   component == "scene.EditableMesh" || component == "scene.EditableImage";
		}
	}

	bool LocalToTheClient(std::string_view component) {
		// **The client makes its own main camera, and the component that says
		// *which* camera that is, is the one to keep local.** `ActiveCamera`
		// names the live one and `client::AimReplicaViewer` mints a predicted
		// camera and points it there - a replica may not mint an authoritative
		// entity - so a replicated `ActiveCamera` would be a second answer to
		// which eye the world is seen through, and the two would fight every
		// frame. `CameraController` is how a machine drives its own.
		//
		// **`scene.Camera` itself must cross, and that is not a hedge.** It is a
		// *lens*, not a viewpoint: a `SurfaceCamera` carries one, so a mirror
		// with no replicated `Camera` cannot be aimed at all -
		// `AimSurfaceCameras` finds nothing, the pane samples nothing, and the
		// mirror is a flat grey rectangle on every client. That is not
		// hypothetical: excluding it broke `studio.playlink`'s "a mirror arrives
		// on the client whole", which is the case that exists to catch exactly
		// this. An authored `Camera` instance is scene content like any other.
		// **A portal proxy is a piece of another room, made and unmade inside one
		// tick.** It exists so a body standing in a hole has the far room's floor
		// under it - `physics/Portals.hpp` - and it is never the same entity two
		// ticks running, so replicating one would be a create and a destroy per
		// tick per proxy on the wire, describing geometry the client already has
		// on the other side of the pane.
		if (component == "scene.PortalProxy") {
			return true;
		}

		if (component == "scene.ActiveCamera" || component == "scene.CameraController") {
			return true;
		}

		// A client's own input and its own identity. Sending the server's copy
		// would tell every client what some other machine is pressing.
		if (component == "scene.InputState" || component == "scene.LocalPlayer") {
			return true;
		}

		// **The authority's bookkeeping about a life that has not started yet.**
		// `scene.PlayerRespawn` is a deadline `scene::UpdateRespawns` computes
		// and only the authority runs that pass, so a replicated one is a row a
		// client can do nothing with - and it is added and removed on every
		// death and spawn, which is an archetype move and a structural message
		// per respawn per player for a number nobody reads.
		//
		// `scene.PlayersService` and `scene.PlayerIdentity` are deliberately not
		// here: `MaxPlayers`, `UserId` and `DisplayName` are what a game's own
		// interface shows, and Roblox puts all three on the client too.
		//
		// **`scene.CharacterChanges` is a *queue*, and the whole of it is
		// per-machine.** Each side records its own transitions and drains them
		// into its own VM - a client learns it has a character by receiving
		// `PlayerCharacter` and rebuilding the link locally - so shipping the
		// authority's list would fire every client's `CharacterAdded` twice, once
		// for its own record and once for a copy of somebody else's.
		if (component == "scene.PlayerRespawn" || component == "scene.CharacterChanges") {
			return true;
		}

		// **A statement about hosting, not about what the world looks like.**
		// `scene.AwakeWorld` is how a game tells its host that a world with
		// nobody in it still has to tick - NPCs, an economy, a round timer. A
		// client neither needs it nor has any business setting it, and a
		// replicated one would be a client asking a server to keep a machine
		// running.
		if (component == "scene.AwakeWorld") {
			return true;
		}

		// **Derived every frame on whichever machine draws.** A previous
		// transform is what interpolation is measured from and the client builds
		// its own in `replication::SnapshotBuffer`; the render gate is computed
		// by the client's own presentation pass; the hash exists to notice a
		// change and means nothing to a receiver. Sending any of them is paying
		// wire for something the far side is about to overwrite.
		if (component == "scene.PreviousTransform" || component == "scene.Rendered" ||
			component == "scene.QuickHash") {
			return true;
		}

		// **A fact about this viewer's own camera, not about the part.**
		// `scene::LocalTransparency` exists so a poppercam can thin out
		// whatever stands between the eye and its subject on *this* machine
		// without editing `Visual::Transparency`, which every other client
		// draws by. Replicating it would put one viewer's occlusion fade on
		// every screen, which is the exact bug the split between the two
		// fields exists to prevent - see the component's own header.
		if (component == "scene.LocalTransparency") {
			return true;
		}

		// **The same argument as `scene.Camera`'s, arriving one step further
		// on.** A `SurfaceCamera` crosses because it is authored scene content;
		// the *frustum fitted to its pane* does not, because that fit is made
		// from where the local eye is standing. The authority's answer is
		// correct for the authority's camera and wrong for every client
		// watching - which is the rule `client/Replicated.hpp` states for the
		// placement, and a lens is the placement's other half.
		//
		// Both ends run `AimSurfaceCameras` and recompute it, so what crosses is
		// the mirror and never the aim. Replicating it would pay wire to send
		// every client a frustum aimed at somebody else's eye, which the
		// receiver then overwrites - wrong *and* wasteful, and wrong in a way
		// that would only show on a second machine.
		//
		// `scene.Portal` is deliberately not here: which part a portal leads to
		// is a fact about the scene, not about the viewer.
		if (component == "scene.SurfaceLens") {
			return true;
		}

		// **The rule the interface set turns on: what the authority decided
		// crosses, what this machine's display, pointer or keyboard decided does
		// not.**
		//
		// A `TextLabel`'s words, colour and rectangle are authored - a server
		// wrote them and every client should read the same thing. Where that
		// rectangle *lands* is not: `gui.Resolved` is what `gui::Layout`
		// computes from the local window's size, so a replicated one would be
		// the server's answer for the server's display, overwritten by the next
		// layout pass on every client that has a different one. `scene.Rendered`
		// and `scene.PreviousTransform` are excluded one section up for exactly
		// this reason.
		//
		// `gui.SpatialCanvas` is the same fact for a surface: it is fitted by
		// whoever holds a camera, which is the machine looking.
		//
		// **`gui.GuiServiceState` is the sharpest of the three, because sending
		// it would be wrong rather than merely wasteful.** It holds
		// `FocusedTextBox` - which box *this* person is typing into - and there
		// is one row of it per world. Two clients typing into two boxes would be
		// two clients writing one row, and the authority would hand each of them
		// back the other's answer. A client's own keyboard is its own, exactly
		// as `scene.InputState` is.
		if (component == "gui.Resolved" || component == "gui.SpatialCanvas" ||
			component == "gui.GuiServiceState") {
			return true;
		}

		// Marked as not outliving the run that made it, so replicating it would
		// be shipping a thing whose whole point is that it is not kept.
		return component == "scene.Transient";
	}

	std::span<const ReplicatedComponent> DefaultReplicatedComponents() {
		// **Built on first use rather than at static-initialisation time**,
		// because it walks the component registry and that registry is filled by
		// `RegisterSceneComponents` during start-up. A table built before that
		// would be empty and nothing would say so.
		static const std::vector<ReplicatedComponent> table = [] {
			std::vector<ReplicatedComponent> found;

			for (size_t index = 0; index < ecs::Components::Count(); index++) {
				const ecs::TypeDescriptor &type =
					ecs::Components::Describe(ecs::ComponentId{static_cast<uint32_t>(index)});

				const std::string_view name = type.Name.Text();
				const bool shared = UnderASharedPrefix(name) || PartOfAnInstance(name) || PartOfAScript(name);
				if (!shared || LocalToTheClient(name)) {
					continue;
				}

				// **A type with no serialisation cannot cross and is skipped
				// rather than declared.** Declaring one would have the authority
				// refuse it per tick - a component that looks replicated,
				// reports nothing and costs a check for ever.
				//
				// **A *tag* is not that, and reading it as one kept
				// `script.Disabled` off the wire.** `Serialisable` is false for
				// an empty struct because there are no bytes to write, not
				// because there is no way to say it - and `WriteComponents` has
				// always handled a zero-sized component by naming the entity and
				// writing nothing, which is the same test `BuildComponents`
				// applies one layer down. A script the author disabled must not
				// run on a client either.
				//
				// **What a delta cannot say is that a tag was *removed*, and
				// that is the delta path's shape rather than this rule's.** A
				// message names rows to write; nothing in it says "this entity
				// stopped carrying that". A script switched back on mid-session
				// therefore stays switched off on a client until a snapshot -
				// which sweeps what it does not mention - reaches it. Sending
				// the removal is a wire change and wants a caller asking for it.
				if (type.Size > 0 && !type.Serialisable) {
					continue;
				}

				// **A type that is not trivially copyable cannot be *signed*,
				// so it crosses only if somebody decided it should be
				// observed.** `Authority::Resign` warns and declines one, so
				// declaring an unnamed non-trivial component is a warning per
				// host per run describing a component nobody meant to send: the
				// catalogues are the case - `scene.TextureCatalogue` and its two
				// siblings are resources holding maps, they hang off no entity,
				// and they arrived here only because they share the prefix.
				//
				// Which is a decision per component rather than something a
				// prefix can infer - and `CannotBeSigned` is where the three
				// that have been made are written down, one line from the
				// argument for each.
				const bool observed = WrittenEveryTick(name) || CannotBeSigned(name);
				if (!type.Trivial && !observed) {
					continue;
				}

				// **A limb's transform is derived on whichever machine draws**, so
				// sending it every tick spends wire on a row `scene::PoseCharacters`
				// overwrites the moment it lands: five extra rows per character per
				// tick, each a ten-byte quantised `CFrame`, against roughly ten
				// bytes for the root alone.
				//
				// `scene.CharacterLimb` is the tag because it already means exactly
				// this - an entity carrying one *is* an entity whose frame is a
				// product of its root and its rest offset. Nothing new had to be
				// declared, which is also why this needed no second consumer to
				// check the idea against: the second consumer would have wanted the
				// same tag it already has.
				//
				// The offsets still cross, because `scene.CharacterLimb` is itself
				// replicated and is not what this filters. Only `scene.Transform`
				// rows for those entities stop, and only as deltas - the baseline a
				// newly admitted client receives still carries one copy, so its
				// first frame is right before any derivation has run. `D00115`.
				//
				// **`gui.Label` is the second row on that mechanism, and it is
				// there for a person rather than for a budget.** The text of a
				// `TextBox` is what somebody is typing: `gui::Type` writes
				// `Label::Text` in the replica as they type, so the two ends are
				// *meant* to disagree - and an authority that went on offering its
				// own copy would wipe a half-typed word, either through a delta or
				// through an audit reporting the box as divergent and repairing it
				// back. `gui.Entry` is the tag because a `TextBox` is the only class
				// that carries one, which is the same "it already means exactly
				// this" the limb tag is chosen for.
				//
				// The baseline still carries one copy, so an authored placeholder
				// or default text arrives; what stops is the per-tick repeat, and
				// what it costs is that a script writing `TextBox.Text` after a
				// client has joined does not reach that client. That is the honest
				// half of the trade and it is the same one Roblox makes from the
				// other side, where the server's write lands on top of whatever
				// somebody had typed.
				const std::string_view suppressor = name == "scene.Transform" ? "scene.CharacterLimb"
													: name == "gui.Label"	  ? "gui.Entry"
																			  : std::string_view();

				found.push_back(
					ReplicatedComponent{
						name,
						observed ? ChangeDetection::Observed : ChangeDetection::Signature,
						suppressor,
					}
				);
			}
			return found;
		}();
		return table;
	}
}
