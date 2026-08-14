#pragma once

// Every tween in one VM, and the clock they all step on.
//
// **The shared half of `TweenService`, for the reason `Signals.hpp` and
// `Tasks.hpp` are shared halves.** What a tween *is* - a target, a curve, a set
// of goals and how far through it is - names no VM, and everything about
// *ordering* has to be one implementation or the two languages would disagree
// about a thing a recording depends on. What each binding supplies is the
// handle a script holds and the callables its `Completed` signal fires.
//
// ## A tween is an entity, and it is not an instance
//
// **The entity is the identity, and that is all it is for.** A tween needs a
// name that is unique in a world, survives a stale handle without naming
// something else, and can be the *subject* of a `SignalTable` entry - which is
// keyed on `ecs::Entity` and on nothing else. `Store::Create("")` answers all
// three for the price of one row, so `Completed` is an ordinary
// `RBXScriptSignal` in both languages with no second connection table anywhere.
//
// **It carries no class and no components, so it is not in the tree.** A tween
// is not parented, is never drawn, is not saved and is not replicated -
// registering a `Tween` class with `ecs::Classes::Register` would have put it in
// all four, made `Instance.new("Tween")` mint a tween with no target and no
// goals, and added a row to the class table every consumer of that table then
// has to describe. `World:CreateEntity` already establishes that a bare entity
// is a legitimate thing for a script to hold; this is the same shape one door
// along.
//
// **The script-side handle is a `Tween` userdata per VM rather than the ordinary
// instance handle**, and the reason is the method table rather than taste: the
// neutral instance methods in `ScriptMethods.cpp` are installed flat on *every*
// instance, so putting `Play` there would claim that name for every part,
// folder and sound in the engine - and `Play` is a name Roblox puts on three
// classes. Three small methods written twice is the cheaper of the two, and it
// is what `RBXScriptConnection` already pays.
//
// ## The clock is the tick's, and the drain order is stated
//
// `Advance` takes the fixed tick delta and integrates in **simulated** seconds.
// Never wall time, and never a real clock read inside the step: a tween that
// advanced by how long the last frame took would put the scene somewhere else
// on a busy machine, and `just replay-check` would fail a long way from here.
//
// Records are held in creation order and walked in it, so two tweens that
// finish on the same tick fire their `Completed` handlers in the order the
// scripts made them. A map keyed on the entity would have made that order a
// hash order, which is exactly the trap `docs/retired/SCRIPT_CONCURRENCY.md` §3
// names for the codec.
//
// @tier L9 · shared

#include <engine/core/Name.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/TweenInfo.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/ecs/Store.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace engine::script {

	// What a tween is doing.
	//
	// **`Completed` and `Cancelled` are two states rather than one "stopped"**,
	// because `Play` means different things from each: a completed tween starts
	// again from the beginning, and a cancelled one does too - but only the
	// first fired its signal, and a caller reading the state to decide whether
	// to fire is exactly the caller that must not confuse them.
	//
	// @since v0.16
	enum class TweenState : uint8_t {
		// Created and never played.
		Idle,

		// Advancing on every tick.
		Playing,

		// Holding its position. `Play` resumes from here rather than restarting.
		Paused,

		// Reached the end of its last pass. `Completed` has fired.
		Completed,

		// Stopped by `Cancel`. `Completed` does not fire, matching what a script
		// asking for a stop means.
		Cancelled,
	};

	// One property a tween drives, and the two ends it drives it between.
	//
	// **The bytes are a fixed buffer rather than a variant**, because the type
	// is already carried and every interpolable value fits in a `CFrame` - the
	// widest of them. A variant would be a second list of the types this file
	// supports, kept in step with `Interpolable` by hand.
	//
	// @since v0.16
	struct TweenGoal {
		// What a script named. Held rather than a `PropertyDescriptor *`: a
		// descriptor belongs to the class table, which this module does not own
		// and must not assume the lifetime of, and a tween writes a handful of
		// properties a tick.
		core::Name Property;

		ecs::PropertyType Type = ecs::PropertyType::Opaque;

		// What the descriptor said, so a write can be size-checked without
		// looking it up again to ask.
		uint32_t Size = 0;

		// Where the property was when the tween was played, and where it is
		// going. `Start` is captured by `TweenTable::Play` rather than by
		// `Create`, which is Roblox's behaviour and the useful one: a tween
		// built ahead of time and played later interpolates from wherever the
		// instance actually is.
		//@{
		alignas(alignof(core::CFrame)) std::byte Start[sizeof(core::CFrame)]{};
		alignas(alignof(core::CFrame)) std::byte Goal[sizeof(core::CFrame)]{};
		//@}
	};

	// The easing enums, converted between their C++ form and their member name.
	//
	// `TweenInfo` holds a `core::EasingStyle` and a script names one, so something
	// has to join the two. Here rather than in `core` because the *names* are
	// userland vocabulary - `core/types/TweenInfo.hpp` is L1 and knows nothing
	// about a script - and here rather than in either binding because both need
	// them: each header declared all four until v0.18, on the grounds that neither
	// may include the other's VM. Neither has to.
	//
	// An unknown member reads as `Linear` and `Out` rather than raising, because
	// the caller has already checked membership through `ScriptCall::ReadEnum`.
	//@{
	core::EasingStyle EasingStyleOf(core::Name member);
	core::Name NameOf(core::EasingStyle style);
	core::EasingDirection EasingDirectionOf(core::Name member);
	core::Name NameOf(core::EasingDirection direction);
	//@}

	// Reports whether a property type has a meaningful midpoint.
	//
	// **A closed list, and everything outside it is refused by name.** The
	// property surface is a switch over `PropertyType`, so a tween could be
	// pointed at a `Bool`, a `Name` or an `Instance` reference and would then
	// have to invent what half way between two of them means. Roblox's answer
	// for those is to snap at the end, which is a tween that does nothing for
	// its whole duration and then jumps - indistinguishable from a broken one.
	//
	// @param type What the property holds.
	// @return `true` when `Interpolate` can blend two of them.
	// @since v0.16
	bool Interpolable(ecs::PropertyType type);

	// Blends two values of one type.
	//
	// **Unclamped alpha, deliberately.** `Back` and `Elastic` overshoot past one
	// and below zero by design - `core::TweenInfo` says so - and clamping here
	// would quietly flatten both curves into their neighbours.
	//
	// @param type  What the bytes mean. Must be `Interpolable`.
	// @param start The value at alpha zero.
	// @param goal  The value at alpha one.
	// @param alpha How far along, already eased.
	// @param out   Filled in. At least `Schemas::SizeOf(type)` bytes.
	// @return `false` when the type has no midpoint.
	// @since v0.16
	bool Interpolate(ecs::PropertyType type, const void *start, const void *goal, float alpha, void *out);

	// Every tween one VM has made.
	//
	// One per runtime, beside `SignalTable` and `TaskQueue`, and for the same
	// reason: two runtimes over two worlds must not be able to step each
	// other's tweens.
	//
	// @since v0.16
	class TweenTable {
	  public:
		// How many tweens one world may hold at once.
		//
		// **A cap, because a handle is not a lifetime.** A script can make a
		// tween in a loop and drop every one of them on the floor; nothing here
		// can tell an unplayed tween somebody still holds from one nobody does,
		// so an uncapped table is a leak with a pleasant API. Roblox reclaims
		// them with its collector and this engine will not run one on a table a
		// recording depends on the order of.
		//
		// **Full means the oldest *finished* tween is reclaimed, and only a
		// table of live ones is refused.** A finished tween has already done
		// what it was made for, so taking it back costs a replay nobody has
		// asked for; refusing when every record is still running is the honest
		// answer to a world that genuinely wants a thousand simultaneous
		// tweens, and it names the cap rather than animating nothing.
		static constexpr size_t MAXIMUM = 1024;

		// Makes a tween, minting the entity that names it.
		//
		// @param store   The world. Both the tween's entity and its target live
		//        in it.
		// @param target  The instance whose properties this drives.
		// @param info    The curve.
		// @param goals   What to drive, **sorted by property name** - see
		//        `Advance`, which writes them in this order.
		// @param dropped Appended with a reclaimed tween's entity, if the table
		//        was full. The caller drops its connections and destroys it,
		//        because only a VM can release a callable.
		// @return The tween's entity, or `NULL_ENTITY` when every record is
		//         still live.
		ecs::Entity Create(
			ecs::Store &store,
			ecs::Entity target,
			const core::TweenInfo &info,
			std::vector<TweenGoal> goals,
			std::vector<ecs::Entity> &dropped
		);

		// Starts or resumes one, capturing where its properties are now.
		//
		// **A paused tween resumes and every other state restarts**, which is
		// Roblox's rule: pausing is the only state that means "part way
		// through". A completed or cancelled tween played again captures fresh
		// start values, so a scene that plays one tween twice is not animating
		// from where it finished last time.
		//
		// @param store The world.
		// @param tween The tween's entity.
		// @return `false` when the tween is unknown or its target has gone.
		bool Play(ecs::Store &store, ecs::Entity tween);

		// Holds one where it is.
		//
		// @param tween The tween's entity.
		// @return `false` when the tween is unknown or was not playing.
		bool Pause(ecs::Entity tween);

		// Stops one and takes it back to the start.
		//
		// **The properties are left where they are**, which is Roblox's
		// behaviour: a cancel is a stop, not an undo, and a tween that snapped
		// its target back would make a cancel visible as a jump.
		//
		// @param tween The tween's entity.
		// @return `false` when the tween is unknown.
		bool Cancel(ecs::Entity tween);

		// Reports whether an entity names a record this table still holds.
		//
		// @param tween The tween's entity.
		// @return `true` when it is known.
		bool Known(ecs::Entity tween) const;

		// Whether anything is currently animating an instance.
		//
		// **The pair below is `GuiObject:TweenPosition`'s `override` argument and
		// nothing else asks.** Roblox's flag means "replace whatever is already
		// running on this object", and answering it needs the two halves of one
		// question - is there one, and stop it - which a caller cannot ask of
		// `Cancel` because it names a tween rather than a target.
		//
		// `Playing` only: a finished or cancelled record has already done what it
		// was made for, so it does not stand in the way of the next one.
		//
		// @param target The instance a tween would drive.
		// @return `true` when at least one record is playing against it.
		bool Driving(ecs::Entity target) const;

		// Stops everything currently animating an instance.
		//
		// `Cancel`'s rule per record, so the properties are left where they are
		// and no `Completed` fires - a script asking for an override is asking for
		// the new motion, not to be told the old one arrived.
		//
		// @param target The instance whose tweens stop.
		// @return How many were stopped.
		size_t CancelFor(ecs::Entity target);

		// What a tween is doing.
		//
		// @param tween The tween's entity.
		// @return The state, or `Cancelled` for a tween this table has taken
		//         back - which is what a handle to a reclaimed tween is.
		TweenState StateOf(ecs::Entity tween) const;

		// Advances every playing tween by one tick.
		//
		// **In creation order, and each tween writes its goals in name order.**
		// Two properties of one instance may project onto one component -
		// `Position` and `CFrame` both write `Transform` - so the order two
		// goals are written in is observable, and it has to be stated rather
		// than left to however a script's table happened to be walked.
		//
		// @param store     The world.
		// @param delta     The **fixed tick delta**, in simulated seconds.
		// @param completed Appended, in order, with every tween that finished
		//        this step. The caller fires `Completed` for each.
		// @param dropped   Appended with every tween whose target has gone. The
		//        caller drops its connections and destroys its entity.
		void Advance(
			ecs::Store &store,
			float delta,
			std::vector<ecs::Entity> &completed,
			std::vector<ecs::Entity> &dropped
		);

		// How many records the table holds.
		size_t Count() const {
			return Records.size();
		}

		// **There is no `Clear`, and a runtime being torn down needs none.** The
		// entities belong to the store, which is destroyed after the VM that
		// built them, and the connections belong to a `SignalTable` whose
		// callables the VM releases when it closes - the same reason
		// `LuauRuntime::~LuauRuntime` empties neither `Tasks` nor `Signals`.

	  private:
		// One tween.
		struct Record {
			// The entity that names it, and the subject of its `Completed`.
			ecs::Entity Tween;

			// What it drives. Checked for life on every step, because an
			// instance destroyed mid-flight is ordinary.
			ecs::Entity Target;

			core::TweenInfo Info;

			// Sorted by property name - see `Create`.
			std::vector<TweenGoal> Goals;

			TweenState State = TweenState::Idle;

			// Simulated seconds since `Play`, including the delay. A double
			// rather than a float because a long tween accumulates a tick at a
			// time, and a float's mantissa runs out of room to hold both a
			// minute and a sixtieth of a second.
			double Elapsed = 0.0;
		};

		Record *Find(ecs::Entity tween);
		const Record *Find(ecs::Entity tween) const;

		// Writes one tween's goals at one alpha.
		static void Apply(ecs::Store &store, const Record &record, float alpha);

		// In creation order, which is the order `Advance` walks. A vector
		// rather than a map for that reason alone: the order is the guarantee.
		std::vector<Record> Records;
	};
}
