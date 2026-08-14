// Stepping every tween in one world, on the tick's own clock.
//
// Nothing here names a VM. `LuauTween.cpp` and `JsTween.cpp` are what meet one
// on this file's behalf — the *handle* is per language and the service is not,
// which is the split `ScriptCall::ReturnTween` sits on.
//
// **The easing name conversions are at the foot of this file rather than in a
// binding**, and moving them there closed a duplicate: `LuauBindings.hpp` and
// `JsBindings.hpp` each declared all four, on the stated grounds that neither
// header may include the other's VM. Neither has to — a name and an enum member
// are what a tween is made of and this is the tween file.
//
// @tier L9 · shared

#include "Tweens.hpp"

#include <engine/core/types/Color3.hpp>
#include <engine/core/types/Rect.hpp>
#include <engine/core/types/UDim.hpp>
#include <engine/core/types/Vector2.hpp>
#include <engine/core/types/Vector3.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace engine::script {

	namespace {
		// Reads one value out of a goal's byte buffer, and writes one back.
		//
		// **`memcpy` rather than a cast through the buffer**, which is the same
		// choice `PushPropertyValue` and every other marshalling path here
		// makes: the bytes came from a property's storage and starting an
		// object's lifetime in them is not something a `static_cast` does.
		//@{
		template <class T> T Read(const void *bytes) {
			T value;
			std::memcpy(&value, bytes, sizeof(T));
			return value;
		}

		template <class T> void Write(void *bytes, const T &value) {
			std::memcpy(bytes, &value, sizeof(T));
		}
		//@}

		// One scalar, blended.
		//
		// In double throughout, so an `Int64` goal does not lose its low bits on
		// the way through a float.
		double Blend(double start, double goal, float alpha) {
			return start + (goal - start) * static_cast<double>(alpha);
		}

		// Where in its own timeline a tween sits.
		struct Phase {
			// How far through the current pass, before easing. Zero during the
			// delay.
			float Alpha = 0.0f;

			// Whether every pass has been run.
			bool Finished = false;
		};

		// Turns elapsed simulated seconds into a position on the curve.
		//
		// **Everything `TweenInfo` says, read once and in one place.** The delay,
		// the repeats and the reversal are three fields that interact, and the
		// interaction is the only part of a tween that is not obvious — so it is
		// a pure function of the info and a number of seconds, which is a thing
		// a test can sweep.
		Phase PhaseOf(const core::TweenInfo &info, double elapsed) {
			Phase phase;

			const double time = static_cast<double>(info.Time);
			const double delay = info.DelayTime > 0.0f ? static_cast<double>(info.DelayTime) : 0.0;

			// A `Time` of zero or less is a tween that is already over: there is
			// no curve to sit on, and dividing by it is what produces the
			// infinity that lands in a property.
			if (time <= 0.0) {
				phase.Alpha = info.Reverses ? 0.0f : 1.0f;
				phase.Finished = true;
				return phase;
			}

			// A pass is `Time` seconds forward, and `Time` more back when it
			// reverses; a cycle is the delay and then that pass.
			const double cycle = delay + (info.Reverses ? time * 2.0 : time);

			// `RepeatCount` is *extra* cycles, so the count is one more than it;
			// below zero is endless, matching Roblox.
			const bool endless = info.RepeatCount < 0;
			const double cycles = static_cast<double>(info.RepeatCount) + 1.0;

			const double index = std::floor(elapsed / cycle);

			// Past the last cycle the tween is finished, and it holds at the end
			// of its last pass — which is the start again when it reverses.
			if (!endless && index >= cycles) {
				phase.Alpha = info.Reverses ? 0.0f : 1.0f;
				phase.Finished = true;
				return phase;
			}

			// Inside the delay the alpha is zero, so a delayed tween holds at
			// its start rather than jumping when it begins.
			const double within = elapsed - index * cycle;
			if (within < delay) {
				return phase;
			}

			const double moving = within - delay;
			phase.Alpha = moving <= time ? static_cast<float>(moving / time)
										 : static_cast<float>(1.0 - (moving - time) / time);
			return phase;
		}
	}

	bool Interpolable(ecs::PropertyType type) {
		// The numbers, and the value types that are numbers in a trench coat.
		// Everything else — `Bool`, `Name`, `Enum`, `String`, `Reference`, the
		// two sequences, `NumberRange` and `Opaque` — has no midpoint, and the
		// caller refuses it by name.
		switch (type) {
		case ecs::PropertyType::Int32:
		case ecs::PropertyType::Int64:
		case ecs::PropertyType::Float:
		case ecs::PropertyType::Double:
		case ecs::PropertyType::Vector2:
		case ecs::PropertyType::Vector3:
		case ecs::PropertyType::Color3:
		case ecs::PropertyType::CFrame:
		case ecs::PropertyType::UDim:
		case ecs::PropertyType::UDim2:
		case ecs::PropertyType::Rect:
			return true;
		default:
			return false;
		}
	}

	// **The goal buffers are exactly wide enough, and the build says so rather
	// than a comment.** Every type the switch above accepts has to fit in
	// `TweenGoal::Start`, which is one `CFrame` wide; a wider interpolable type
	// added without widening that is a write past the end of a member, which is
	// the kind of fault that lands somewhere else entirely.
	static_assert(
		sizeof(core::CFrame) >= sizeof(core::UDim2) && sizeof(core::CFrame) >= sizeof(core::Rect) &&
			sizeof(core::CFrame) >= sizeof(core::Vector3) && sizeof(core::CFrame) >= sizeof(core::Color3) &&
			sizeof(core::CFrame) >= sizeof(double) && sizeof(core::CFrame) >= sizeof(int64_t),
		"a tween goal must fit in the buffer TweenGoal holds"
	);

	bool Interpolate(ecs::PropertyType type, const void *start, const void *goal, float alpha, void *out) {
		// One switch over the same closed list `Interpolable` answers for. The
		// two are next to each other because they must agree, and the default
		// answer on both sides is "no".
		switch (type) {
		case ecs::PropertyType::Int32:
			// **Rounded rather than truncated.** A truncating tween arrives one
			// short of its goal and stays there, which reads as an off-by-one in
			// whatever the property drives.
			Write(
				out,
				static_cast<int32_t>(std::llround(Blend(Read<int32_t>(start), Read<int32_t>(goal), alpha)))
			);
			return true;
		case ecs::PropertyType::Int64:
			Write(
				out,
				static_cast<int64_t>(std::llround(Blend(
					static_cast<double>(Read<int64_t>(start)), static_cast<double>(Read<int64_t>(goal)), alpha
				)))
			);
			return true;
		case ecs::PropertyType::Float:
			Write(out, static_cast<float>(Blend(Read<float>(start), Read<float>(goal), alpha)));
			return true;
		case ecs::PropertyType::Double:
			Write(out, Blend(Read<double>(start), Read<double>(goal), alpha));
			return true;
		case ecs::PropertyType::Vector2:
			Write(out, Read<core::Vector2>(start).Lerp(Read<core::Vector2>(goal), alpha));
			return true;
		case ecs::PropertyType::Vector3:
			Write(out, Read<core::Vector3>(start).Lerp(Read<core::Vector3>(goal), alpha));
			return true;
		case ecs::PropertyType::Color3:
			Write(out, Read<core::Color3>(start).Lerp(Read<core::Color3>(goal), alpha));
			return true;
		case ecs::PropertyType::CFrame:
			// **`Lerp` and not `NLerp`, which is the reverse of what a renderer
			// wants.** `CFrame.hpp` says which to reach for: `NLerp` is for
			// endpoints a tick apart, where the constant-rate error vanishes.
			// A tween's endpoints are a whole animation apart and its whole
			// point is that the *rate* follows the curve it was given, so the
			// `acos` is what is being paid for.
			Write(out, Read<core::CFrame>(start).Lerp(Read<core::CFrame>(goal), alpha));
			return true;
		case ecs::PropertyType::UDim: {
			// No `UDim::Lerp` in `core`, unlike its two-axis form. Written out
			// here rather than added there: a `UDim` is two floats and the
			// engine has no other caller for the operation.
			const core::UDim from = Read<core::UDim>(start);
			const core::UDim to = Read<core::UDim>(goal);
			Write(
				out,
				core::UDim(
					static_cast<float>(Blend(from.Scale, to.Scale, alpha)),
					static_cast<float>(Blend(from.Offset, to.Offset, alpha))
				)
			);
			return true;
		}
		case ecs::PropertyType::UDim2:
			Write(out, Read<core::UDim2>(start).Lerp(Read<core::UDim2>(goal), alpha));
			return true;
		case ecs::PropertyType::Rect: {
			const core::Rect from = Read<core::Rect>(start);
			const core::Rect to = Read<core::Rect>(goal);
			Write(out, core::Rect(from.Min.Lerp(to.Min, alpha), from.Max.Lerp(to.Max, alpha)));
			return true;
		}
		default:
			return false;
		}
	}

	ecs::Entity TweenTable::Create(
		ecs::Store &store,
		ecs::Entity target,
		const core::TweenInfo &info,
		std::vector<TweenGoal> goals,
		std::vector<ecs::Entity> &dropped
	) {
		if (Records.size() >= MAXIMUM) {
			// At the cap, the oldest *finished* record is reclaimed — see
			// `MAXIMUM` for why a finished one, and why refusing is the right
			// answer when every record is still live.
			const auto stale = std::find_if(Records.begin(), Records.end(), [](const Record &record) {
				return record.State == TweenState::Completed || record.State == TweenState::Cancelled;
			});
			if (stale == Records.end()) {
				return ecs::NULL_ENTITY;
			}

			dropped.push_back(stale->Tween);
			Records.erase(stale);
		}

		// Unnamed: a name is for a thing a person or a save file has to be able
		// to point at, and nothing points at a tween.
		//
		// **Predicted on a replica and authoritative anywhere else.** A tween's
		// entity carries no component, is never sent and never crosses a world,
		// so which range it comes from is invisible — but a replica *refuses* an
		// authoritative mint, and a client animating its own interface is the
		// ordinary case rather than an edge one. The reserved range is exactly
		// what it is for.
		Record record;
		record.Tween = store.AdoptOnly() ? store.CreatePredicted() : store.Create();
		if (record.Tween == ecs::NULL_ENTITY) {
			// The index range is exhausted, which `Store::Create` has already
			// said out loud. Nothing to hold a record against.
			return ecs::NULL_ENTITY;
		}

		record.Target = target;
		record.Info = info;
		record.Goals = std::move(goals);

		// Appended, so the vector stays in creation order — which is the order
		// `Advance` walks and therefore the order two `Completed` handlers run
		// in.
		Records.push_back(std::move(record));
		return Records.back().Tween;
	}

	bool TweenTable::Play(ecs::Store &store, ecs::Entity tween) {
		Record *record = Find(tween);
		if (record == nullptr || !store.Alive(record->Target)) {
			return false;
		}

		// A paused tween resumes where it stopped. Anything else starts from the
		// beginning, which means capturing the start values now — see the header
		// for why that is at `Play` and not at `Create`.
		if (record->State != TweenState::Paused) {
			record->Elapsed = 0.0;
			for (TweenGoal &goal : record->Goals) {
				// A goal whose property cannot be read leaves its start buffer
				// as it was. The write would fail the same way, so the tween is
				// inert for that one property rather than for all of them.
				(void)store.GetProperty(record->Target, goal.Property, goal.Start, goal.Size);
			}
		}

		record->State = TweenState::Playing;
		return true;
	}

	bool TweenTable::Pause(ecs::Entity tween) {
		Record *record = Find(tween);

		// Only a playing tween can pause. Pausing a finished one would leave a
		// state `Play` reads as "resume from the end".
		if (record == nullptr || record->State != TweenState::Playing) {
			return false;
		}

		record->State = TweenState::Paused;
		return true;
	}

	bool TweenTable::Cancel(ecs::Entity tween) {
		Record *record = Find(tween);
		if (record == nullptr) {
			return false;
		}

		// Stopped and rewound, and the properties left where they are — see the
		// header. `Completed` does not fire: a script that asked for a stop is
		// not asking to be told the tween arrived.
		record->State = TweenState::Cancelled;
		record->Elapsed = 0.0;
		return true;
	}

	bool TweenTable::Known(ecs::Entity tween) const {
		return Find(tween) != nullptr;
	}

	bool TweenTable::Driving(ecs::Entity target) const {
		for (const Record &record : Records) {
			if (record.Target == target && record.State == TweenState::Playing) {
				return true;
			}
		}
		return false;
	}

	size_t TweenTable::CancelFor(ecs::Entity target) {
		size_t stopped = 0;
		for (Record &record : Records) {
			if (record.Target != target || record.State != TweenState::Playing) {
				continue;
			}
			record.State = TweenState::Cancelled;
			record.Elapsed = 0.0;
			stopped++;
		}
		return stopped;
	}

	TweenState TweenTable::StateOf(ecs::Entity tween) const {
		const Record *record = Find(tween);

		// A handle to a record that has been reclaimed reads as cancelled, which
		// is what it is: stopped, and never going to fire.
		return record != nullptr ? record->State : TweenState::Cancelled;
	}

	void TweenTable::Advance(
		ecs::Store &store, float delta, std::vector<ecs::Entity> &completed, std::vector<ecs::Entity> &dropped
	) {
		// **One pass in creation order**, and both out-lists are appended in
		// that order too — which is what makes two tweens finishing on one tick
		// fire in the order the scripts made them.
		size_t kept = 0;
		for (size_t index = 0; index < Records.size(); index++) {
			Record &record = Records[index];

			// A record whose target has gone is dropped, wherever it had got to.
			// Not an error and not a leak: an instance destroyed mid-tween is
			// ordinary, and the entry would otherwise sit in the table writing
			// to a dead row for the life of the world.
			if (!store.Alive(record.Target)) {
				dropped.push_back(record.Tween);
				continue;
			}

			if (record.State == TweenState::Playing) {
				record.Elapsed += static_cast<double>(delta);

				const Phase phase = PhaseOf(record.Info, record.Elapsed);
				Apply(store, record, record.Info.Evaluate(phase.Alpha));

				if (phase.Finished) {
					// Kept rather than dropped, so the handle can play it again
					// — which is what `MAXIMUM` reclaims when it has to.
					record.State = TweenState::Completed;
					completed.push_back(record.Tween);
				}
			}

			// Compacted as the walk goes rather than erased inside it, so no
			// index moves under the loop.
			if (kept != index) {
				Records[kept] = std::move(record);
			}
			kept++;
		}

		Records.resize(kept);
	}

	TweenTable::Record *TweenTable::Find(ecs::Entity tween) {
		// A linear walk, and it stays one while the cap is a thousand: this is
		// reached once per `Play`, `Pause` or `Cancel`, which are script calls
		// rather than per-tick work.
		for (Record &record : Records) {
			if (record.Tween == tween) {
				return &record;
			}
		}
		return nullptr;
	}

	const TweenTable::Record *TweenTable::Find(ecs::Entity tween) const {
		return const_cast<TweenTable *>(this)->Find(tween);
	}

	void TweenTable::Apply(ecs::Store &store, const Record &record, float alpha) {
		// Each goal, in the name order `Create` sorted them into — see
		// `Advance`, which states why the order between two goals is observable.
		for (const TweenGoal &goal : record.Goals) {
			alignas(alignof(core::CFrame)) std::byte bytes[sizeof(core::CFrame)];
			if (!Interpolate(goal.Type, goal.Start, goal.Goal, alpha, bytes)) {
				continue;
			}

			// A write that fails is left alone rather than reported: a property
			// that stopped existing under a running tween is the same non-event
			// as a target that was never a `Part`, and there is nowhere at a
			// barrier to report it to.
			(void)store.SetProperty(record.Target, goal.Property, bytes, goal.Size);
		}
	}

	core::EasingStyle EasingStyleOf(core::Name member) {
		static const struct {
			const char *Name;
			core::EasingStyle Style;
		} STYLES[] = {
			{"Linear", core::EasingStyle::Linear},
			{"Quad", core::EasingStyle::Quad},
			{"Cubic", core::EasingStyle::Cubic},
			{"Quart", core::EasingStyle::Quart},
			{"Quint", core::EasingStyle::Quint},
			{"Sine", core::EasingStyle::Sine},
			{"Exponential", core::EasingStyle::Exponential},
			{"Circular", core::EasingStyle::Circular},
			{"Back", core::EasingStyle::Back},
			{"Elastic", core::EasingStyle::Elastic},
			{"Bounce", core::EasingStyle::Bounce},
		};

		for (const auto &entry : STYLES) {
			if (member == core::Name(entry.Name)) {
				return entry.Style;
			}
		}
		return core::EasingStyle::Linear;
	}

	core::Name NameOf(core::EasingStyle style) {
		static const char *NAMES[] = {
			"Linear",
			"Quad",
			"Cubic",
			"Quart",
			"Quint",
			"Sine",
			"Exponential",
			"Circular",
			"Back",
			"Elastic",
			"Bounce",
		};
		return core::Name(NAMES[static_cast<size_t>(style)]);
	}

	core::EasingDirection EasingDirectionOf(core::Name member) {
		if (member == core::Name("In")) {
			return core::EasingDirection::In;
		}
		if (member == core::Name("InOut")) {
			return core::EasingDirection::InOut;
		}
		return core::EasingDirection::Out;
	}

	core::Name NameOf(core::EasingDirection direction) {
		static const char *NAMES[] = {"In", "Out", "InOut"};
		return core::Name(NAMES[static_cast<size_t>(direction)]);
	}
}
