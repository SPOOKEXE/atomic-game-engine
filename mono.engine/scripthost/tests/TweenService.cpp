// What a tween does, in both languages and on the neutral table underneath.
//
// **Two halves, and they answer different questions.** The neutral cases drive
// `TweenTable` directly, because the cap, the repeat maths and the reclaim
// policy are decisions with no script in them - reaching them through a VM would
// mean a thousand `Instance.new` calls to test an integer. The parity cases run
// the same tween in Luau and in JavaScript and compare the answers, because that
// is the property the whole service layer exists to buy: one `TweenTable`, one
// set of orders, two bindings that must not disagree about either.
//
// The answer crosses as `workspace.Name` for `ScriptCall.cpp`'s reason:
// `Runtime::Run` reports whether a chunk ran and not what it evaluated to, and
// the property surface is the one channel this suite is not testing.

#include <engine/ecs/Store.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/script/Tweens.hpp>
#include <engine/scripthost/Runtime.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

TEST_SUITE_ID("engine.scripthost.tweenservice")

using engine::core::EasingDirection;
using engine::core::EasingStyle;
using engine::core::TweenInfo;
using engine::ecs::Entity;
using engine::ecs::NULL_ENTITY;
using engine::ecs::Store;
using engine::script::Interpolable;
using engine::script::Interpolate;
using engine::script::Language;
using engine::script::MakeRuntime;
using engine::script::TweenGoal;
using engine::script::TweenState;
using engine::script::TweenTable;

namespace {
	const std::vector<Language> LANGUAGES = {Language::Luau, Language::JavaScript};

	Store Fresh(const char *name) {
		engine::scene::EnsureClassTree();
		engine::scene::RegisterSceneComponents();
		return Store(name);
	}

	// `a:b(...)` in Luau and `a.b(...)` in JavaScript - `ScriptCall.cpp`'s
	// helper, which is the whole of what differs between most halves below.
	std::string Send(Language language, std::string_view receiver, std::string_view call) {
		return std::string(receiver) + (language == Language::Luau ? ":" : ".") + std::string(call) + "\n";
	}

	// `local x = ...` and `let x = ...`.
	std::string Let(Language language, std::string_view name, std::string_view value) {
		return (language == Language::Luau ? "local " : "let ") + std::string(name) + " = " +
			   std::string(value) + "\n";
	}

	// Writes one value where the test can read it.
	std::string Say(Language language, std::string_view expression) {
		const std::string text = language == Language::Luau ? "tostring(" : "String(";
		return "workspace.Name = " + text + std::string(expression) + ")\n";
	}

	// A function body, which is the one construct the two languages spell
	// entirely differently.
	std::string Does(Language language, std::string_view body) {
		return language == Language::Luau ? "function()\n" + std::string(body) + "end)\n"
										  : "function () {\n" + std::string(body) + "})\n";
	}

	// A heartbeat connection, which is how a case does something *between*
	// beats - a script cannot run in the middle of the harness's loop.
	std::string EachBeat(Language language, std::string_view body) {
		return "RunService.Heartbeat" + std::string(language == Language::Luau ? ":" : ".") + "Connect(" +
			   Does(language, body);
	}

	// Runs a chunk, pumps `beats` heartbeats, and hands back what `Say` wrote.
	//
	// **The beat is the runtime's alone and not the world's**, unlike the debris
	// suite's: a tween integrates the delta it is handed and never reads the
	// tick, so advancing the store here would test nothing this does not.
	std::string Answer(Language language, const std::string &source, int beats = 0) {
		Store store = Fresh("tweenservice");
		const auto runtime = MakeRuntime(store, language);
		REQUIRE(runtime != nullptr);

		// Held before the chunk runs, because the chunk renames it.
		const Entity workspace = engine::scene::WorkspaceOf(store);
		REQUIRE(workspace != NULL_ENTITY);

		INFO(source);
		const bool ok = runtime->Run(source.c_str());
		INFO(runtime->LastError());
		REQUIRE(ok);

		for (int beat = 0; beat < beats; beat++) {
			const bool beaten = runtime->Heartbeat(1.0f / 60.0f);
			INFO(runtime->LastError());
			REQUIRE(beaten);
		}

		return std::string(store.InstanceNameOf(workspace).Text());
	}

	// One parity case: what to run, how long to beat, and what both languages
	// must say.
	struct ParityCase {
		const char *Name;
		std::function<std::string(Language)> Body;
		const char *Expected;
		int Beats = 0;
	};

	void Check(const std::vector<ParityCase> &cases) {
		for (const ParityCase &probe : cases) {
			INFO(probe.Name);

			const std::string luau = Answer(Language::Luau, probe.Body(Language::Luau), probe.Beats);
			const std::string javascript =
				Answer(Language::JavaScript, probe.Body(Language::JavaScript), probe.Beats);

			INFO(luau);
			INFO(javascript);
			CHECK(luau == javascript);
			CHECK(luau == std::string(probe.Expected));
		}
	}

	// A part and a one-second linear tween on its `Position`, which almost every
	// case below starts with.
	std::string ALinearTween(
		Language language,
		std::string_view info = "TweenInfo.new(1, Enum.EasingStyle.Linear, Enum.EasingDirection.In)"
	) {
		return Let(language, "part", "Instance.new('Part')") +
			   Let(language,
				   "tween",
				   "TweenService" + std::string(language == Language::Luau ? ":" : ".") + "Create(part, " +
					   std::string(info) + ", { Position" + (language == Language::Luau ? " = " : ": ") +
					   "Vector3.new(0, 10, 0) })");
	}
}

// --- the easing curve, which is the whole service before anything moves -------

TEST_CASE("GetValue answers the same curve in both languages", "[scripting][tween]") {
	// **The first thing to get right and the easiest thing to check.** Every
	// case here is a value that is exact in a float, so the assertion is an
	// equality rather than a tolerance - `Linear` and the polynomial family at
	// a half are quarters and eighths, and a curve that has drifted shows up as
	// a different string rather than as a rounding argument.
	//
	// `core::TweenInfo` already has its own suite over all eleven styles; what
	// this adds is that the *binding* reaches it, in both languages, with the
	// enums resolved the same way.
	const auto value = [](Language language, const char *alpha, const char *style, const char *direction) {
		return Say(
			language,
			"TweenService" + std::string(language == Language::Luau ? ":" : ".") + "GetValue(" + alpha +
				", Enum.EasingStyle." + style + ", Enum.EasingDirection." + direction + ")"
		);
	};

	Check({
		{"linear at zero", [&](Language l) { return value(l, "0", "Linear", "In"); }, "0"},
		{"linear half way", [&](Language l) { return value(l, "0.5", "Linear", "In"); }, "0.5"},
		{"linear at one", [&](Language l) { return value(l, "1", "Linear", "In"); }, "1"},

		{"quad at zero", [&](Language l) { return value(l, "0", "Quad", "In"); }, "0"},
		{"quad half way in", [&](Language l) { return value(l, "0.5", "Quad", "In"); }, "0.25"},
		{"quad half way out", [&](Language l) { return value(l, "0.5", "Quad", "Out"); }, "0.75"},
		{"quad at one", [&](Language l) { return value(l, "1", "Quad", "In"); }, "1"},

		// The third style is here because `InOut` is the one direction that is
		// not derived from the other two by a single subtraction - it joins two
		// halves, and the join is what a mistake shows up in.
		{"cubic half way in", [&](Language l) { return value(l, "0.5", "Cubic", "In"); }, "0.125"},
		{"cubic half way in and out", [&](Language l) { return value(l, "0.5", "Cubic", "InOut"); }, "0.5"},

		// **Past the end is the end**, which `TweenInfo::Ease` clamps for and
		// this pins from the outside: an elastic curve extrapolated past one
		// grows without bound, and the symptom would be a part flung out of the
		// world rather than a bad alpha.
		{"past the end is the end", [&](Language l) { return value(l, "4", "Quad", "In"); }, "1"},
	});
}

// --- a tween that actually runs ----------------------------------------------

TEST_CASE("a tween reaches its goal and completes", "[scripting][tween]") {
	// **Sixty beats of a one-second tween, which is the arithmetic the whole
	// thing rests on.** The delta is the fixed tick delta and nothing here reads
	// a clock, so this is the same number of beats on every machine - which is
	// the point of the delta being what it is.
	Check({
		{"the goal is reached exactly",
		 [](Language language) {
			 return ALinearTween(language) +
					("tween.Completed" + std::string(language == Language::Luau ? ":" : ".") + "Connect(") +
					Does(language, Say(language, "part.Position.Y")) + Send(language, "tween", "Play()");
		 },
		 "10",
		 60},

		// **Half way is half way**, which is what says the curve is being
		// integrated rather than snapped at the end. Rounded to a tenth,
		// because thirty ticks of a float delta is 0.50000002 seconds and the
		// two languages print that differently.
		{"half way through it is half way there",
		 [](Language language) {
			 const std::string floor = language == Language::Luau ? "math.floor" : "Math.floor";
			 return ALinearTween(language) + Send(language, "tween", "Play()") + Let(language, "beats", "0") +
					EachBeat(
						language,
						"beats += 1\n"
						"if " +
							std::string(
								language == Language::Luau ? "beats == 30 then " : "(beats === 30) "
							) +
							Say(language, floor + "(part.Position.Y * 10 + 0.5)") +
							(language == Language::Luau ? "end\n" : "")
					);
		 },
		 "50",
		 30},

		// **A tween that has not been played does not move**, which is the
		// state every tween starts in and the one a scene builds ahead of time.
		{"an unplayed tween moves nothing",
		 [](Language language) { return ALinearTween(language) + Say(language, "part.Position.Y"); },
		 "0",
		 60},
	});
}

TEST_CASE("a cancelled tween stops and does not complete", "[scripting][tween]") {
	// **Two facts and they are not the same one.** A cancel that fired
	// `Completed` would tell a script the tween arrived when it did not; a
	// cancel that left the tween running would be a stop nobody can see.
	Check({
		{"nothing completes",
		 [](Language language) {
			 return ALinearTween(language) + Let(language, "completions", "0") +
					("tween.Completed" + std::string(language == Language::Luau ? ":" : ".") + "Connect(") +
					Does(language, "completions += 1\n") + Send(language, "tween", "Play()") +
					Let(language, "beats", "0") +
					EachBeat(
						language,
						"beats += 1\n"
						"if " +
							std::string(language == Language::Luau ? "beats == 5 then " : "(beats === 5) ") +
							Send(language, "tween", "Cancel()") +
							(language == Language::Luau ? "end\n" : "") + "if " +
							std::string(
								language == Language::Luau ? "beats == 90 then " : "(beats === 90) "
							) +
							Say(language, "completions") + (language == Language::Luau ? "end\n" : "")
					);
		 },
		 "0",
		 90},

		{"and it stops where it was",
		 [](Language language) {
			 const std::string same =
				 language == Language::Luau ? "part.Position.Y == frozen" : "part.Position.Y === frozen";
			 return ALinearTween(language) + Send(language, "tween", "Play()") + Let(language, "beats", "0") +
					Let(language, "frozen", "-1") +
					EachBeat(
						language,
						"beats += 1\n"
						"if " +
							std::string(
								language == Language::Luau ? "beats == 5 then\n" : "(beats === 5) {\n"
							) +
							Send(language, "tween", "Cancel()") + "frozen = part.Position.Y\n" +
							(language == Language::Luau ? "end\n" : "}\n") + "if " +
							std::string(
								language == Language::Luau ? "beats == 90 then " : "(beats === 90) "
							) +
							Say(language, same) + (language == Language::Luau ? "end\n" : "")
					);
		 },
		 "true",
		 90},
	});
}

TEST_CASE("a tween whose instance is destroyed mid-flight is dropped", "[scripting][tween]") {
	// **Ordinary, and neither a crash nor a leak.** An instance destroyed while
	// something is animating it is what happens every time a game kills
	// something mid-effect; the record is taken out of the table wherever it had
	// got to, and `Completed` does not fire because the tween did not arrive.
	//
	// The heartbeats themselves are the other half of the assertion: `Answer`
	// requires every one of them to succeed, so a write into a dead row would
	// fail this case rather than this suite noticing later.
	Check({
		{"nothing completes and nothing raises",
		 [](Language language) {
			 return ALinearTween(language) + Let(language, "completions", "0") +
					("tween.Completed" + std::string(language == Language::Luau ? ":" : ".") + "Connect(") +
					Does(language, "completions += 1\n") + Send(language, "tween", "Play()") +
					Let(language, "beats", "0") +
					EachBeat(
						language,
						"beats += 1\n"
						"if " +
							std::string(language == Language::Luau ? "beats == 5 then " : "(beats === 5) ") +
							Send(language, "part", "Destroy()") +
							(language == Language::Luau ? "end\n" : "") + "if " +
							std::string(
								language == Language::Luau ? "beats == 90 then " : "(beats === 90) "
							) +
							Say(language, "completions") + (language == Language::Luau ? "end\n" : "")
					);
		 },
		 "0",
		 90},
	});
}

TEST_CASE("a goal a tween cannot drive is refused by name", "[scripting][tween]") {
	// **By name, which is the difference between a minute and an afternoon.** A
	// tween pointed at a `Bool` has nothing to interpolate through, and the
	// alternative to refusing is a tween that runs for its whole duration and
	// moves nothing - which reads as a broken engine rather than as a scene
	// asking for something that does not mean anything.
	struct Refusal {
		const char *Goal;
		const char *Named;
	};

	const std::vector<Refusal> PROBES = {
		// A `Bool`, which has no midpoint.
		{"{ Anchored = true }", "Anchored"},

		// A property of no class at all, which is a typo.
		{"{ Nonsense = 1 }", "Nonsense"},
	};

	for (const Language language : LANGUAGES) {
		for (const Refusal &probe : PROBES) {
			Store store = Fresh("tween_refusal");
			const auto runtime = MakeRuntime(store, language);
			REQUIRE(runtime != nullptr);

			std::string goal(probe.Goal);
			if (language == Language::JavaScript) {
				// The one thing an object literal spells differently.
				goal = goal.replace(goal.find(" = "), 3, ": ");
			}

			const std::string source =
				Let(language, "part", "Instance.new('Part')") +
				Send(language, "TweenService", "Create(part, TweenInfo.new(1), " + goal + ")");

			INFO(source);
			CHECK_FALSE(runtime->Run(source.c_str()));
			INFO(runtime->LastError());
			CHECK(runtime->LastError().find(probe.Named) != std::string::npos);
		}
	}
}

// --- the table underneath ----------------------------------------------------

TEST_CASE("only a type with a midpoint may be tweened", "[scripting][tween]") {
	// **`Interpolable` and `Interpolate` are one decision written twice**, and
	// they are next to each other in the file for that reason. This is what
	// says they still agree: a type the first accepts and the second refuses is
	// a goal that passes `Create` and then quietly does nothing.
	using engine::ecs::PropertyType;

	for (const PropertyType type :
		 {PropertyType::Opaque,		   PropertyType::Bool,		  PropertyType::Int32,
		  PropertyType::Int64,		   PropertyType::Float,		  PropertyType::Double,
		  PropertyType::Name,		   PropertyType::Enum,		  PropertyType::String,
		  PropertyType::Vector3,	   PropertyType::Color3,	  PropertyType::CFrame,
		  PropertyType::Vector2,	   PropertyType::UDim,		  PropertyType::UDim2,
		  PropertyType::Rect,		   PropertyType::NumberRange, PropertyType::NumberSequence,
		  PropertyType::ColorSequence, PropertyType::Reference}) {
		alignas(alignof(engine::core::CFrame)) std::byte start[sizeof(engine::core::CFrame)]{};
		alignas(alignof(engine::core::CFrame)) std::byte goal[sizeof(engine::core::CFrame)]{};
		alignas(alignof(engine::core::CFrame)) std::byte out[sizeof(engine::core::CFrame)]{};

		INFO(engine::ecs::Describe(type));
		CHECK(Interpolable(type) == Interpolate(type, start, goal, 0.5f, out));
	}

	// The midpoint of a scalar and of a vector, which is the arithmetic every
	// running tween is doing.
	const float from = 0.0f;
	const float to = 10.0f;
	float blended = -1.0f;
	REQUIRE(Interpolate(engine::ecs::PropertyType::Float, &from, &to, 0.25f, &blended));
	CHECK(blended == 2.5f);

	const engine::core::Vector3 left(0.0f, 0.0f, 0.0f);
	const engine::core::Vector3 right(0.0f, 8.0f, 0.0f);
	engine::core::Vector3 middle;
	REQUIRE(Interpolate(engine::ecs::PropertyType::Vector3, &left, &right, 0.5f, &middle));
	CHECK(middle.Y == 4.0f);
}

TEST_CASE("a tween's own timeline honours the delay, the repeat and the reversal", "[scripting][tween]") {
	// **Driven against the table rather than through a VM**, because what is
	// being asserted is *when* a tween is finished and that is a pure function
	// of `TweenInfo` and a number of seconds. Goals are empty: this case is
	// about the clock, and `Interpolate` has its own.
	Store store("tween_timeline");
	const Entity target = store.Create();

	struct Probe {
		const char *Name;
		TweenInfo Info;

		// How many half-second steps before it is finished.
		int Steps;
	};

	const std::vector<Probe> PROBES = {
		{"a plain second", TweenInfo(1.0f), 2},

		// A delay is *before* the pass, so the two add up.
		{"a second after a delayed half",
		 TweenInfo(1.0f, EasingStyle::Linear, EasingDirection::In, 0, false, 0.5f),
		 3},

		// `RepeatCount` is extra passes, so one repeat is two seconds.
		{"one repeat is two passes", TweenInfo(1.0f, EasingStyle::Linear, EasingDirection::In, 1), 4},

		// Reversing runs the pass backwards afterwards, which doubles it.
		{"reversing doubles a pass", TweenInfo(1.0f, EasingStyle::Linear, EasingDirection::In, 0, true), 4},
	};

	for (const Probe &probe : PROBES) {
		INFO(probe.Name);

		TweenTable table;
		std::vector<Entity> dropped;
		const Entity tween = table.Create(store, target, probe.Info, {}, dropped);
		REQUIRE(tween != NULL_ENTITY);
		REQUIRE(table.Play(store, tween));

		for (int step = 1; step <= probe.Steps; step++) {
			std::vector<Entity> completed;
			table.Advance(store, 0.5f, completed, dropped);

			INFO(step);
			// **The step before the last must not have finished**, which is the
			// half of this that catches a timeline that is merely too short.
			CHECK((step == probe.Steps) == (table.StateOf(tween) == TweenState::Completed));
			CHECK((step == probe.Steps) == !completed.empty());
		}
	}

	// **An endless tween never completes**, which is what `-1` means and is the
	// one case where "not finished yet" has to stay true forever.
	TweenTable endless;
	std::vector<Entity> dropped;
	const Entity forever = endless.Create(
		store, target, TweenInfo(1.0f, EasingStyle::Linear, EasingDirection::In, -1), {}, dropped
	);
	REQUIRE(endless.Play(store, forever));
	for (int step = 0; step < 20; step++) {
		std::vector<Entity> completed;
		endless.Advance(store, 0.5f, completed, dropped);
		CHECK(completed.empty());
	}
	CHECK(endless.StateOf(forever) == TweenState::Playing);
}

TEST_CASE("the table is capped, and it reclaims a finished tween before it refuses", "[scripting][tween]") {
	// **A handle is not a lifetime**, which is the whole argument for the cap:
	// a script can make a tween in a loop and drop every one of them, and
	// nothing here can tell an unplayed tween somebody still holds from one
	// nobody does.
	//
	// The two halves are the policy: a finished tween is taken back, and a table
	// of live ones refuses rather than evicting something that is still running.
	Store store("tween_cap");
	const Entity target = store.Create();

	TweenTable table;
	std::vector<Entity> dropped;

	Entity first = NULL_ENTITY;
	for (size_t made = 0; made < TweenTable::MAXIMUM; made++) {
		const Entity tween = table.Create(store, target, TweenInfo(1.0f), {}, dropped);
		REQUIRE(tween != NULL_ENTITY);
		if (made == 0) {
			first = tween;
		}
	}

	CHECK(table.Count() == TweenTable::MAXIMUM);
	CHECK(dropped.empty());

	// Every record is idle, which counts as live: nothing has had its chance.
	CHECK(table.Create(store, target, TweenInfo(1.0f), {}, dropped) == NULL_ENTITY);
	CHECK(dropped.empty());

	// One finished record is all it takes, and the oldest is the one that goes.
	REQUIRE(table.Cancel(first));
	CHECK(table.Create(store, target, TweenInfo(1.0f), {}, dropped) != NULL_ENTITY);
	REQUIRE(dropped.size() == 1);
	CHECK(dropped.front() == first);

	// **And the reclaimed handle answers**, rather than naming a record that
	// has gone. `Play` on it is `false` and its state is the stopped one.
	CHECK_FALSE(table.Known(first));
	CHECK(table.StateOf(first) == TweenState::Cancelled);
	CHECK_FALSE(table.Play(store, first));
}

TEST_CASE("a tween writes its goals in property-name order", "[scripting][tween]") {
	// **Two goals of one instance may project onto one component**, so which of
	// them lands last is observable - `Position` and `CFrame` both write
	// `Transform`. The order is sorted by spelling in both bindings, so a Luau
	// table's hash order cannot decide what a scene looks like.
	//
	// Asserted through the table rather than a VM: what is being pinned is that
	// `Advance` walks the vector it was handed in order, and the sort itself is
	// in the two `ReadGoals`.
	Store store = Fresh("tween_order");

	const Entity part = store.CreateInstance(engine::scene::PartClass(), "orderly");
	REQUIRE(part != NULL_ENTITY);

	TweenGoal cframe;
	cframe.Property = engine::core::Name("CFrame");
	cframe.Type = engine::ecs::PropertyType::CFrame;
	cframe.Size = sizeof(engine::core::CFrame);
	const engine::core::CFrame target(engine::core::Vector3(1.0f, 2.0f, 3.0f));
	std::memcpy(cframe.Goal, &target, sizeof(target));

	TweenGoal position;
	position.Property = engine::core::Name("Position");
	position.Type = engine::ecs::PropertyType::Vector3;
	position.Size = sizeof(engine::core::Vector3);
	const engine::core::Vector3 elsewhere(9.0f, 9.0f, 9.0f);
	std::memcpy(position.Goal, &elsewhere, sizeof(elsewhere));

	// `CFrame` sorts before `Position`, so the second write is the one that
	// survives - which is what a scene naming both should see.
	TweenTable table;
	std::vector<Entity> dropped;
	const Entity tween = table.Create(store, part, TweenInfo(1.0f), {cframe, position}, dropped);
	REQUIRE(tween != NULL_ENTITY);
	REQUIRE(table.Play(store, tween));

	std::vector<Entity> completed;
	table.Advance(store, 1.0f, completed, dropped);
	REQUIRE(completed.size() == 1);

	engine::core::Vector3 landed;
	REQUIRE(store.GetProperty(part, engine::core::Name("Position"), &landed, sizeof(landed)));
	CHECK(landed.X == 9.0f);
}
