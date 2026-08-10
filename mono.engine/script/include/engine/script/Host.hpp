#pragma once

// The one way a program offers a script something the engine does not.
//
// **A script's vocabulary is the world, and a tool's is the program.** Every
// surface in this module so far — `Instance`, `workspace`, `World`, the
// datatypes — is about the scene, because that is what a *game* script talks
// about. An editor tool talks about the editor: a toolbar, a button, a docked
// panel, the file a script instance was loaded from. None of that is a world,
// and none of it can be added here, because this module does not know what an
// editor is and must not learn.
//
// So a host installs its own names, through one seam:
//
//     host.CreateToolbar("My Tools")            -- Luau, in a plugin
//        -> HostSurface::Call("CreateToolbar", ...)   -- C++, in the editor
//
// ## Why this is a value tree and not a `lua_State`
//
// `script/AGENTS.md`'s first rule is that no VM type appears in a public header,
// so a host cannot be handed a state to push onto. What crosses instead is
// `HostValue` — the same shape the bus codec uses, plus the two things an
// in-process call can carry that a wire cannot:
//
// - **An `Instance`.** A handle is meaningless outside the world holding it,
//   which is exactly why `ScriptValue` refuses one and why this accepts one: a
//   host call is inside one process against one store, and an editor asking
//   "what is selected" has to be able to answer with instances.
// - **A `Callback`.** A button's handler lives in the plugin's VM and the press
//   happens in the host's frame, so something has to name a function across
//   that gap. `HostCallback` is an opaque id; the reference it stands for never
//   leaves this module.
//
// **It is deliberately not `ScriptValue` widened.** That type is what crosses a
// world boundary, where rule 3 says everything is a copy and a handle means
// nothing — adding an instance tag to it would make the wrong thing expressible
// on a bus. Two types is the correct answer here, and the duplication is a
// dozen fields rather than a mechanism.
//
// ## What a host must hold to
//
// **Every call runs on the thread that beat the runtime**, inside the host's own
// frame, because that is where `Runtime::Heartbeat` was called from. A host that
// answered from another thread would be writing a world it does not own, which
// `ecs::Store` aborts on.
//
// **A refusal is a message, not a crash.** `Call` returns `false` and fills
// `failure`, and the script sees an ordinary error naming it. A host that
// aborted would take the editor down with a plugin's typo.
//
// @tier L9 · shared

#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Color3.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Entity.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace engine::script {

	// What a `HostValue` carries.
	//
	// @since v0.12
	enum class HostTag : uint8_t {
		Nil,
		Boolean,
		Number,
		String,
		Array,
		Map,

		// An entity in the world this runtime was built against.
		Instance,

		// A function in the script's VM, named across the gap. See
		// `HostCallback`.
		Callback,

		Vector3,
		Color3,
		CFrame,
	};

	// A function in a script's VM that a host may call later.
	//
	// **An opaque id, because the reference is the module's.** What this stands
	// for is a registry ref, which is a VM concept and stays behind
	// `Runtime::Invoke`. A host holds the number, calls it when a button is
	// pressed, and releases it when the button goes away.
	//
	// **A host that never releases leaks a closure for the life of the
	// runtime**, which is a plugin's whole session — survivable, and still worth
	// releasing, because a panel that is opened and closed a hundred times
	// should not hold a hundred handlers.
	//
	// @since v0.12
	struct HostCallback {
		// Zero for a value that names no function.
		uint64_t Id = 0;

		// Whether this names a function.
		//
		// @return `true` when it came from a script.
		bool Valid() const {
			return Id != 0;
		}

		// Compares ids, so a host can find the entry it stored.
		//
		// @param other The callback to compare.
		// @return `true` when both name the same function.
		bool operator==(const HostCallback &other) const {
			return Id == other.Id;
		}
	};

	// One value crossing between a script and its host.
	//
	// @since v0.12
	struct HostValue {
		// Which of the fields below carries the value.
		HostTag Tag = HostTag::Nil;

		// Set for `Boolean`.
		bool Boolean = false;

		// Set for `Number`. A double, always: both languages hold one.
		double Number = 0.0;

		// Set for `String`. Bytes rather than characters.
		std::string Text;

		// Set for `Array`, in order.
		std::vector<HostValue> Items;

		// Set for `Map`, in the order the script's table was walked.
		//
		// **Not sorted, unlike `ScriptValue`'s.** That type sorts because it is
		// about to be hashed into a wire format two machines have to agree on;
		// this one is consumed in the same process by the host that asked for
		// it, and imposing an order would cost a sort on every call to make a
		// guarantee nobody here needs.
		std::vector<std::pair<std::string, HostValue>> Entries;

		// Set for `Instance`.
		ecs::Entity Instance;

		// Set for `Callback`.
		HostCallback Callback;

		// Set for `Vector3`, `Color3` and `CFrame` respectively.
		//@{
		core::Vector3 Vector;
		core::Color3 Colour;
		core::CFrame Frame;
		//@}

		// Constructs nil.
		HostValue() = default;

		// Constructs a value of one tag with everything else defaulted.
		//
		// @param tag Which kind of value.
		explicit HostValue(HostTag tag) : Tag(tag) {}

		// --- the shapes a host reads, so every reader is not a switch ---------
		//
		// A host answers questions like "the second argument, as a string, or
		// empty". Written once here rather than at every call site, because the
		// alternative is thirty copies of a tag test and one of them forgetting.

		// This value's text, or empty when it is not a string.
		//
		// @return The text.
		std::string_view AsText() const {
			return Tag == HostTag::String ? std::string_view(Text) : std::string_view{};
		}

		// This value's number, or a fallback when it is not one.
		//
		// @param fallback What to answer for a value of another tag.
		// @return The number.
		double AsNumber(double fallback = 0.0) const {
			return Tag == HostTag::Number ? Number : fallback;
		}

		// This value's truth. A nil or a false is false and everything else is
		// true, which is Luau's own rule.
		//
		// @return Whether the value is truthy.
		bool AsBoolean() const {
			return Tag != HostTag::Nil && (Tag != HostTag::Boolean || Boolean);
		}

		// This value's instance, or `NULL_ENTITY`.
		//
		// @return The entity.
		ecs::Entity AsInstance() const {
			return Tag == HostTag::Instance ? Instance : ecs::Entity{};
		}

		// Builders, so a host answering a call reads as what it answers.
		//@{
		static HostValue Of(bool value);
		static HostValue Of(double value);
		static HostValue Of(std::string_view value);
		static HostValue Of(ecs::Entity value);
		static HostValue List(std::vector<HostValue> items);
		//@}
	};

	// One argument list, as a host receives it.
	using HostArguments = std::span<const HostValue>;

	// What a program offers the scripts it runs.
	//
	// **One method, because the alternative is a vtable per feature.** A host
	// that grew a virtual per name would need this module recompiled every time
	// it added one, which is the coupling the seam exists to remove: the editor
	// adds `CreateDockWidget` and `script` does not change.
	//
	// @since v0.12
	class HostSurface {
	  public:
		virtual ~HostSurface() = default;

		HostSurface(const HostSurface &) = delete;
		HostSurface &operator=(const HostSurface &) = delete;
		HostSurface(HostSurface &&) = delete;
		HostSurface &operator=(HostSurface &&) = delete;

		// Answers one call from a script.
		//
		// **Runs on the thread that beat the runtime**, inside the host's frame,
		// so it may touch the world the runtime was built against and nothing
		// else.
		//
		// @param name      What the script called.
		// @param arguments What it passed, in order.
		// @param result    Filled in on success. Left nil for a call that
		//                  answers nothing.
		// @param failure   Filled in on refusal, in the words a script author
		//                  should read.
		// @return `false` to raise a script error naming `failure`.
		virtual bool
		Call(std::string_view name, HostArguments arguments, HostValue &result, std::string &failure) = 0;

		// Every name this host answers.
		//
		// **Used to build the global rather than to document it**, so a name the
		// host does not list is one a script cannot call — which turns a typo
		// into "no such member" at the call site instead of a refusal from
		// inside the host.
		//
		// @return The callable names.
		virtual std::vector<std::string> Names() const = 0;

		// What the table of those names is called in a script.
		//
		// **So a host's API reads as its own rather than as plumbing.** An
		// editor's is `plugin`, and `plugin.CreateToolbar(...)` is what a person
		// who has written a Roblox plugin expects — where `host.CreateToolbar`
		// would read as an implementation detail leaking into a surface.
		//
		// One table and one name, not several: a host that wanted two
		// vocabularies would be two hosts.
		//
		// @return The global's name.
		virtual std::string_view GlobalName() const {
			return "host";
		}

	  protected:
		HostSurface() = default;
	};
}
