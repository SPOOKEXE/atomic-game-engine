// The instance methods a UI author reaches for, written once for both VMs.
//
// **`ScriptMethods.cpp`'s table, split by what a method has to reach.** The rows
// there name the class tree and the store and nothing else; these four name
// `engine::gui` or drive a `TweenTable`, and each carries a policy worth stating
// on its own rather than one line in a list of thirty-odd.
//
// ## The hit test is the compile's answer read back
//
// `gui/AGENTS.md` refuses a second traversal that re-decides what is on top,
// because the compiled list and the hit test disagreeing reads as "clicking is
// off by a bit" and is close to undebuggable from the outside. So
// `GetGuiObjectsAtPosition` does not walk the tree deciding an order.
// `gui::ElementsAt` collects the elements whose rectangle and clip contain the
// point and sorts them by `Resolved::Order`, which is the paint position
// `Compiled::Rebuild` already wrote down.
//
// **`PlayerGui` is the receiver and the search is scoped to it**, which is
// Roblox's shape and is load-bearing rather than cosmetic here: `Layout` resolves
// every collector in the world, including the `StarterGui` template and every
// *other* player's copy, so an unscoped answer would hand one player the
// rectangles of somebody else's interface.
//
// ## A tween on a `GuiObject` goes through `TweenService` and never beside it
//
// The three `Tween*` methods are Roblox's shorthand for a tween somebody would
// otherwise build with `TweenService:Create`, and that is exactly what they are
// here: a `TweenGoal` per property, `TweenTable::Create`, `TweenTable::Play`.
// There is no second interpolator, no second easing table and no second drain -
// `TweenTable::Advance` steps these at the head of the barrier with everything
// else, so a scene animating a panel and a part stays in one clock.
//
// **The property type comes from the target's own descriptor**, through
// `ScriptCall::ReadProperty`, so none of the three names a datatype. That is what
// makes them honest on the flat instance table: the neutral methods are installed
// on every instance, and `TweenSize` on something whose `Size` is a `Vector3`
// tweens a `Vector3` rather than refusing a call it could have served.
//
// **Roblox's argument order is kept, easing *direction* before *style*.** It is
// the opposite of `TweenInfo.new` and it is what every script written elsewhere
// passes; correcting it here would silently swap two arguments of the same type.
//
// @tier L9 · shared

#include <engine/ecs/Classes.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/Input.hpp>
#include <engine/gui/Typing.hpp>
#include <engine/script/ScriptCall.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace engine::script {

	namespace {
		using core::Name;
		using ecs::Entity;

		// `playerGui:GetGuiObjectsAtPosition(x, y)`
		//
		// **Every `GuiObject` under the point and not only the one that takes
		// input**, which is where this differs from `gui::Pick`: a decorative
		// `Frame` is transparent to a click and is still under the pointer, and a
		// script asking what is there wants the panel as well as the button on
		// it.
		//
		// Answers an empty list for anything that is not a container of laid-out
		// elements, which is what a `Part` and an empty `PlayerGui` both are.
		void GetGuiObjectsAtPosition(ScriptCall &call) {
			const core::Vector2 point{
				static_cast<float>(call.AsNumber(0)),
				static_cast<float>(call.AsNumber(1)),
			};

			std::vector<Entity> found;
			gui::ElementsAt(call.World(), call.Subject(), point, found);
			call.ReturnInstances(found);
		}

		// `guiButton:EmulateClick()`
		//
		// **A signal delivery and not pointer injection.** A test or a scripted
		// tutorial wants to activate the button's authored behaviour without
		// changing the pointer capture, hover state, or focus that belongs to a
		// physical device event. The router and this method therefore meet at the
		// same `GuiActivated` signal, which keeps `Activated` and
		// `MouseButton1Click` aliases in one route.
		void EmulateClick(ScriptCall &call) {
			const ecs::ClassId button = ecs::Classes::Find(Name("GuiButton"));
			if (!button.IsValid() || !call.World().IsA(call.Subject(), button)) {
				call.Raise("EmulateClick needs a GuiButton");
			}

			const std::string error = call.DispatchSignal(SignalKind::GuiActivated, call.Subject());
			if (!error.empty()) {
				call.Raise(error.c_str());
			}
		}

		void RequireGuiObject(ScriptCall &call, const char *method) {
			if (call.World().Get<gui::Element>(call.Subject()) == nullptr) {
				call.Raise((std::string(method) + " needs a GuiObject").c_str());
			}
		}

		void RequireTextBox(ScriptCall &call, const char *method) {
			if (call.World().Get<gui::Entry>(call.Subject()) == nullptr) {
				call.Raise((std::string(method) + " needs a TextBox").c_str());
			}
		}

		void RequireFocusedTextBox(ScriptCall &call, const char *method) {
			RequireTextBox(call, method);
			if (gui::FocusedTextBox(call.World()) != call.Subject()) {
				call.Raise((std::string(method) + " needs a focused TextBox").c_str());
			}
		}

		void RequireScrollingFrame(ScriptCall &call, const char *method) {
			if (call.World().Get<gui::Scrolling>(call.Subject()) == nullptr) {
				call.Raise((std::string(method) + " needs a ScrollingFrame").c_str());
			}
		}

		void RequireDragDetector(ScriptCall &call, const char *method) {
			if (call.World().Get<gui::DragDetector>(call.Subject()) == nullptr) {
				call.Raise((std::string(method) + " needs a UIDragDetector").c_str());
			}
		}

		void RaiseIfHandlerFailed(ScriptCall &call, const std::string &error) {
			if (!error.empty()) {
				call.Raise(error.c_str());
			}
		}

		core::Vector2 VirtualPointerPosition(const ecs::Store &store, Entity instance) {
			const gui::Resolved *resolved = store.Get<gui::Resolved>(instance);
			if (resolved == nullptr) {
				return {};
			}
			return resolved->AbsolutePosition + resolved->AbsoluteSize * 0.5f;
		}

		core::Vector2 VirtualPosition(ScriptCall &call) {
			return core::Vector2{
				static_cast<float>(call.AsNumber(0)),
				static_cast<float>(call.AsNumber(1)),
			};
		}

		core::Vector2 VirtualDelta(ScriptCall &call) {
			return core::Vector2{
				static_cast<float>(call.AsNumber(2)),
				static_cast<float>(call.AsNumber(3)),
			};
		}

		// `guiObject:VirtualHover()`
		//
		// A virtual event has no device position. The laid-out centre is the one
		// stable canvas point to offer handlers, with `(0, 0)` for an unlaid item.
		void VirtualHover(ScriptCall &call) {
			RequireGuiObject(call, "VirtualHover");
			const core::Vector2 point = VirtualPointerPosition(call.World(), call.Subject());
			RaiseIfHandlerFailed(
				call, call.DispatchPointerSignal(SignalKind::GuiMouseEnter, call.Subject(), point.X, point.Y)
			);
		}

		// `guiObject:VirtualUnhover()`
		void VirtualUnhover(ScriptCall &call) {
			RequireGuiObject(call, "VirtualUnhover");
			const core::Vector2 point = VirtualPointerPosition(call.World(), call.Subject());
			RaiseIfHandlerFailed(
				call, call.DispatchPointerSignal(SignalKind::GuiMouseLeave, call.Subject(), point.X, point.Y)
			);
		}

		// `guiObject:VirtualMove(x, y)`
		void VirtualMove(ScriptCall &call) {
			RequireGuiObject(call, "VirtualMove");
			const core::Vector2 point = VirtualPosition(call);
			RaiseIfHandlerFailed(
				call, call.DispatchPointerSignal(SignalKind::GuiMouseMoved, call.Subject(), point.X, point.Y)
			);
		}

		// `textBox:VirtualFocus()`
		void VirtualFocus(ScriptCall &call) {
			RequireTextBox(call, "VirtualFocus");
			const Entity previous = gui::FocusedTextBox(call.World());
			if (!gui::Focus(call.World(), call.Subject())) {
				return;
			}
			if (previous != ecs::NULL_ENTITY) {
				RaiseIfHandlerFailed(call, call.DispatchFocusLost(previous, false));
			}
			RaiseIfHandlerFailed(call, call.DispatchSignal(SignalKind::GuiFocused, call.Subject()));
		}

		// `textBox:VirtualUnfocus()`
		void VirtualUnfocus(ScriptCall &call) {
			RequireTextBox(call, "VirtualUnfocus");
			if (gui::FocusedTextBox(call.World()) != call.Subject() ||
				!gui::Focus(call.World(), ecs::NULL_ENTITY)) {
				return;
			}
			RaiseIfHandlerFailed(call, call.DispatchFocusLost(call.Subject(), false));
		}

		// `textBox:VirtualText(text)`
		//
		// The focused-box guard keeps a receiver from typing into some other box
		// through the shared keyboard route.
		void VirtualText(ScriptCall &call) {
			RequireFocusedTextBox(call, "VirtualText");
			const std::string text = call.AsString(0);
			(void)gui::Type(call.World(), gui::Typing{.Text = text});
		}

		// `textBox:VirtualSubmit()`
		void VirtualSubmit(ScriptCall &call) {
			RequireFocusedTextBox(call, "VirtualSubmit");
			const gui::TypeResult result = gui::Type(call.World(), gui::Typing{.Submit = true});
			if (result.Released) {
				RaiseIfHandlerFailed(call, call.DispatchFocusLost(call.Subject(), true));
			}
		}

		// `scrollingFrame:VirtualScroll(notches)`
		void VirtualScroll(ScriptCall &call) {
			RequireScrollingFrame(call, "VirtualScroll");
			(void)gui::Scroll(call.World(), call.Subject(), static_cast<float>(call.AsNumber(0)));
		}

		void VirtualDrag(ScriptCall &call, SignalKind kind, const char *method, bool hasDelta) {
			RequireDragDetector(call, method);
			const core::Vector2 point = VirtualPosition(call);
			const core::Vector2 delta = hasDelta ? VirtualDelta(call) : core::Vector2{};
			RaiseIfHandlerFailed(
				call, call.DispatchDragSignal(kind, call.Subject(), point.X, point.Y, delta.X, delta.Y)
			);
		}

		void VirtualDragBegin(ScriptCall &call) {
			VirtualDrag(call, SignalKind::GuiDragBegan, "VirtualDragBegin", false);
		}

		void VirtualDragContinue(ScriptCall &call) {
			VirtualDrag(call, SignalKind::GuiDragContinue, "VirtualDragContinue", true);
		}

		void VirtualDragEnd(ScriptCall &call) {
			VirtualDrag(call, SignalKind::GuiDragEnded, "VirtualDragEnd", true);
		}

		void RequireGuiButton(ScriptCall &call, const char *method) {
			const ecs::ClassId button = ecs::Classes::Find(Name("GuiButton"));
			if (!button.IsValid() || !call.World().IsA(call.Subject(), button)) {
				call.Raise((std::string(method) + " needs a GuiButton").c_str());
			}
		}

		void VirtualLeftHold(ScriptCall &call) {
			RequireGuiButton(call, "VirtualLeftHold");
			RaiseIfHandlerFailed(call, call.DispatchSignal(SignalKind::GuiInputBegan, call.Subject()));
			RaiseIfHandlerFailed(call, call.DispatchSignal(SignalKind::GuiMouseButton1Down, call.Subject()));
		}

		void VirtualLeftRelease(ScriptCall &call) {
			RequireGuiButton(call, "VirtualLeftRelease");
			RaiseIfHandlerFailed(call, call.DispatchSignal(SignalKind::GuiInputEnded, call.Subject()));
			RaiseIfHandlerFailed(call, call.DispatchSignal(SignalKind::GuiMouseButton1Up, call.Subject()));
		}

		// `guiButton:VirtualLeftClick()`
		//
		// Match the primary router sequence: down, up, then activation. The final
		// signal also carries Roblox's `MouseButton1Click` alias.
		void VirtualLeftClick(ScriptCall &call) {
			VirtualLeftHold(call);
			VirtualLeftRelease(call);
			RaiseIfHandlerFailed(call, call.DispatchSignal(SignalKind::GuiActivated, call.Subject()));
		}

		void VirtualRightHold(ScriptCall &call) {
			RequireGuiButton(call, "VirtualRightHold");
			RaiseIfHandlerFailed(call, call.DispatchSignal(SignalKind::GuiMouseButton2Down, call.Subject()));
		}

		void VirtualRightRelease(ScriptCall &call) {
			RequireGuiButton(call, "VirtualRightRelease");
			RaiseIfHandlerFailed(call, call.DispatchSignal(SignalKind::GuiMouseButton2Up, call.Subject()));
		}

		void VirtualRightClick(ScriptCall &call) {
			VirtualRightHold(call);
			VirtualRightRelease(call);
			RaiseIfHandlerFailed(call, call.DispatchSignal(SignalKind::GuiMouseButton2Click, call.Subject()));
		}

		// --- the three tweens ---------------------------------------------------

		// Where each of Roblox's trailing five arguments sits, per method.
		//
		// **A record rather than five literals per body**, because the only thing
		// that differs between the three is how many goals come first - and three
		// copies of "the callback is argument five, unless it is argument six" is
		// exactly the pair that gets edited singly.
		struct TweenArguments {
			// How many goal values lead the argument list: one, or two for
			// `TweenSizeAndPosition`.
			size_t Goals;

			size_t Direction() const {
				return Goals;
			}
			size_t Style() const {
				return Goals + 1;
			}
			size_t Seconds() const {
				return Goals + 2;
			}
			size_t Override() const {
				return Goals + 3;
			}
			size_t Callback() const {
				return Goals + 4;
			}
		};

		// Roblox's defaults for the four optional arguments, which are not the
		// ones `TweenInfo.new` uses.
		//
		// `TweenInfo`'s own defaults are `Quad`/`Out` over one second; these are
		// the same curve and the same second, spelled here because a reader
		// comparing the two surfaces should be able to see that they agree rather
		// than take it on trust.
		//@{
		constexpr float DEFAULT_SECONDS = 1.0f;
		constexpr const char *DEFAULT_STYLE = "Quad";
		constexpr const char *DEFAULT_DIRECTION = "Out";
		//@}

		// One goal, aimed at a value the argument list carries.
		//
		// Refuses by *name* for the reason `TweenService:Create` does: a tween
		// that runs for its whole duration and moves nothing reads as a broken
		// engine rather than as a scene asking for something that has no meaning.
		TweenGoal AimGoal(ScriptCall &call, Entity target, const char *property, size_t index) {
			const ecs::PropertyDescriptor *descriptor = ScriptableProperty(call.World(), target, property);
			if (descriptor == nullptr) {
				call.Raise((std::string(property) + " is not a property of this instance").c_str());
			}
			if (!descriptor->Writable) {
				call.Raise((std::string(property) + " cannot be assigned").c_str());
			}
			if (!Interpolable(descriptor->Type)) {
				call.Raise((std::string(property) + " is a " + ecs::Describe(descriptor->Type) +
							", which has no midpoint to interpolate through")
							   .c_str());
			}

			TweenGoal goal;
			goal.Property = descriptor->Name;
			goal.Type = descriptor->Type;
			goal.Size = descriptor->Size;

			if (!call.ReadProperty(index, descriptor->Type, descriptor->EnumName, goal.Goal)) {
				call.Raise((std::string("expected a ") + ecs::Describe(descriptor->Type) + " for " + property)
							   .c_str());
			}
			return goal;
		}

		// The curve the four optional arguments describe.
		core::TweenInfo ReadCurve(ScriptCall &call, const TweenArguments &where) {
			Name direction(DEFAULT_DIRECTION);
			if (!call.IsNil(where.Direction()) &&
				!call.ReadEnum(where.Direction(), Name("EasingDirection"), direction)) {
				call.Raise("expected an Enum.EasingDirection");
			}

			Name style(DEFAULT_STYLE);
			if (!call.IsNil(where.Style()) && !call.ReadEnum(where.Style(), Name("EasingStyle"), style)) {
				call.Raise("expected an Enum.EasingStyle");
			}

			core::TweenInfo info;
			info.Time = call.IsNil(where.Seconds()) ? DEFAULT_SECONDS
													: static_cast<float>(call.AsNumber(where.Seconds()));
			info.Style = EasingStyleOf(style);
			info.Direction = EasingDirectionOf(direction);
			return info;
		}

		// Builds the tween, plays it, and answers whether it will run.
		//
		// **`override` decides, and it decides before anything is built.** Roblox
		// answers `false` for a call that was refused because something else is
		// already animating the object, and a caller reading that answer is the
		// whole reason the flag exists - so a refusal must not leave a record in
		// the table for the cap to reclaim later.
		//
		// **The callback is a `:Once` on the tween's `Completed`**, and it is
		// handed nothing. Roblox passes an `Enum.TweenStatus`; this engine has no
		// such enum and `tween.Completed` is already argument-less for that
		// reason, so inventing one for this one surface would be a value that has
		// to change the day the enum arrives.
		void PlayGoals(ScriptCall &call, const TweenArguments &where, std::vector<TweenGoal> goals) {
			const Entity target = call.Subject();
			TweenTable &table = call.Tweens();

			// **Read before the refusal**, so a wrong easing argument is an error
			// in both the overridden and the refused case. A validation order that
			// depended on what else was running would make a script's own bug
			// intermittent.
			const core::TweenInfo info = ReadCurve(call, where);

			const bool takeOver = call.OptionalBoolean(where.Override(), false);
			if (!takeOver && table.Driving(target)) {
				call.ReturnBoolean(false);
				return;
			}
			table.CancelFor(target);

			// Sorted by spelling, which is `TweenService:Create`'s rule and is
			// why it matters here too: `TweenSizeAndPosition` writes two goals,
			// and two properties of one instance may project onto one component.
			std::sort(goals.begin(), goals.end(), [](const TweenGoal &left, const TweenGoal &right) {
				return left.Property.Text() < right.Property.Text();
			});

			std::vector<Entity> dropped;
			const Entity tween = table.Create(call.World(), target, info, std::move(goals), dropped);

			// The connections go before the row does - only a VM knows what a
			// `CallbackRef` means, which is why the release is a request.
			for (const Entity stale : dropped) {
				call.ForgetSubject(stale);
				call.World().Destroy(stale);
			}

			if (tween == ecs::NULL_ENTITY) {
				// **False rather than a raise**, which is where this departs from
				// `TweenService:Create`. That method answers a handle and has
				// nothing else to say; this one already answers "will it play",
				// and a world holding a thousand live tweens is the honest "no".
				call.ReturnBoolean(false);
				return;
			}

			if (!call.IsNil(where.Callback())) {
				call.ConnectOnce(SignalKind::TweenCompleted, tween, call.RetainCallback(where.Callback()));
			}

			call.ReturnBoolean(table.Play(call.World(), tween));
		}

		// `guiObject:TweenPosition(endPosition, direction, style, seconds, override, callback)`
		void TweenPosition(ScriptCall &call) {
			constexpr TweenArguments WHERE{1};
			PlayGoals(call, WHERE, {AimGoal(call, call.Subject(), "Position", 0)});
		}

		// `guiObject:TweenSize(endSize, direction, style, seconds, override, callback)`
		void TweenSize(ScriptCall &call) {
			constexpr TweenArguments WHERE{1};
			PlayGoals(call, WHERE, {AimGoal(call, call.Subject(), "Size", 0)});
		}

		// `guiObject:TweenSizeAndPosition(endSize, endPosition, direction, style, seconds, override,
		// callback)`
		//
		// **Size first, which is Roblox's order and the opposite of the method's
		// own name read as a list.** Getting it the other way round is a call that
		// typechecks, runs, and animates the wrong pair of numbers.
		void TweenSizeAndPosition(ScriptCall &call) {
			constexpr TweenArguments WHERE{2};
			PlayGoals(
				call,
				WHERE,
				{AimGoal(call, call.Subject(), "Size", 0), AimGoal(call, call.Subject(), "Position", 1)}
			);
		}

		// **`SetTopbarTransparency` and `GetTopbarTransparency` are deliberately
		// absent, and the reason is that there is no topbar.** `gui::Screen::
		// TopInset` is zero and its comment says why - this engine reserves no
		// strip at the top of the canvas, so the pair would read and write a
		// number nothing draws. Roblox's `PlayerGui` has them because Roblox has a
		// bar; a setter that stored a float for a surface that does not exist is
		// the member `SoundService.cpp` and `HttpService.cpp` each refuse a list
		// of, on the rule that an absent member is better than one that does
		// nothing. What closing it needs is a topbar: a non-zero `TopInset` that
		// something paints, and then this pair is the transparency of that paint.

		constexpr std::array<InstanceMethod, 22> GUI_METHODS{{
			{"GetGuiObjectsAtPosition", GetGuiObjectsAtPosition},
			{"EmulateClick", EmulateClick},
			{"VirtualHover", VirtualHover},
			{"VirtualUnhover", VirtualUnhover},
			{"VirtualMove", VirtualMove},
			{"VirtualFocus", VirtualFocus},
			{"VirtualUnfocus", VirtualUnfocus},
			{"VirtualText", VirtualText},
			{"VirtualSubmit", VirtualSubmit},
			{"VirtualScroll", VirtualScroll},
			{"VirtualDragBegin", VirtualDragBegin},
			{"VirtualDragContinue", VirtualDragContinue},
			{"VirtualDragEnd", VirtualDragEnd},
			{"VirtualLeftClick", VirtualLeftClick},
			{"VirtualLeftHold", VirtualLeftHold},
			{"VirtualLeftRelease", VirtualLeftRelease},
			{"VirtualRightClick", VirtualRightClick},
			{"VirtualRightHold", VirtualRightHold},
			{"VirtualRightRelease", VirtualRightRelease},

			{"TweenPosition", TweenPosition},
			{"TweenSize", TweenSize},
			{"TweenSizeAndPosition", TweenSizeAndPosition},
		}};
	}

	std::span<const InstanceMethod> GuiInstanceMethods() {
		return GUI_METHODS;
	}
}
