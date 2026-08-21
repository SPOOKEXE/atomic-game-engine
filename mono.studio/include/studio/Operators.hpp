#pragma once

// Every command the editor offers, as data rather than as call sites.
//
// **`MCP.md` §3's argument, taken at face value.** A studio operator - a name, a
// description, a poll saying whether it can run, and something that runs it - is
// an MCP tool descriptor field for field. Written once as a table, it has three
// consumers rather than three implementations:
//
//   - the **command palette**, which walks it and filters,
//   - the **menus**, which ask an operator's poll instead of writing the
//     condition out again at the call site,
//   - the **MCP surface**, which is the one this table is shaped for and the one
//     `control/Tools.cpp` still writes by hand.
//
// The poll is the part worth having. Availability was previously a condition
// written inline at each menu item - `!Selection.empty()` appears three times in
// `Interface.cpp` alone, with nothing comparing them - so an action reachable
// from a menu and a shortcut had two answers to "may this run now" and no
// mechanism that would notice them disagreeing.
//
// ## Why it does not replace `Keybinds`
//
// It could, and doing so would touch a tested subsystem - the binding table, its
// file persistence and its scopes - for no gain the palette can see. So the
// division is: **`Keybinds` owns what key runs a command, this owns what the
// commands are**, joined by `Action`.
//
// Two tables joined by an enum is exactly the drift this repository warns about,
// so the join is checked rather than trusted: `tests/Operators.cpp` asserts every
// `Action` has exactly one operator. A command added to one and not the other
// fails the suite rather than going missing from the palette.
//
// ## Why a reason and not just a bool
//
// An agent - and a person reading a greyed-out menu item - discovers that an
// action was unavailable by attempting it and reading a failure. A poll that
// carries *why* turns that into something readable before acting, which removes
// a whole class of wasted turns. It costs one string.
//
// @tier L12 · client

#include <functional>
#include <string>
#include <string_view>
#include <studio/Keybinds.hpp>
#include <vector>

namespace studio {

	// Whether an operator can run right now, and if not, what is missing.
	//
	// @since v0.7
	struct Availability {
		// Whether `Operator::Run` would do anything.
		bool Ready = false;

		// Why not, in the words a person would use - "nothing is selected", not
		// "precondition failed". Empty when `Ready`.
		std::string Reason;

		// The available answer.
		//
		// @return An `Availability` that is ready.
		static Availability Yes() {
			return Availability{true, {}};
		}

		// The unavailable answer, with its reason.
		//
		// @param reason What is missing.
		// @return An `Availability` that is not ready.
		static Availability No(std::string reason) {
			return Availability{false, std::move(reason)};
		}
	};

	// One thing the editor can be asked to do.
	//
	// @since v0.7
	struct Operator {
		// Which command this is. Also how a binding is found - see
		// `Keybinds::Of`.
		Action Id = Action::Count;

		// What to call it in the palette and the menus.
		std::string_view Name;

		// One line, for the palette's second column.
		std::string_view Description;

		// Whether it can run, and why not. **Never null**: an operator that is
		// always available says so rather than leaving this empty, because a
		// null poll is a fourth answer to a question that already has three.
		std::function<Availability()> Poll;

		// Doing it. Called from outside `Universe::Enter`, like every other
		// action in this program.
		std::function<void()> Run;
	};

	// The registry, and the palette's filtering over it.
	//
	// @since v0.7
	class OperatorTable {
	  public:
		// Registers one operator.
		//
		// **Refuses a second operator for one `Action`**, rather than replacing
		// or appending. Two operators on one id is the drift this table exists
		// to make impossible, and silently keeping the last one registered would
		// hide it behind whichever translation unit initialised second.
		//
		// @param op The operator.
		// @return `true` when it was added.
		bool Add(Operator op);

		// Finds the operator for a command.
		//
		// @param id Which command.
		// @return The operator, or null when nothing registered one.
		const Operator *Find(Action id) const;

		// Asks whether a command can run, without needing its operator.
		//
		// **The one call a menu item makes.** An unregistered command is not
		// ready and says so, rather than defaulting to enabled - a menu item
		// that is clickable and does nothing is worse than one that is greyed.
		//
		// @param id Which command.
		// @return Its availability.
		Availability Available(Action id) const;

		// Runs a command, if it can run.
		//
		// @param id Which command.
		// @return `true` when it ran.
		bool Run(Action id) const;

		// Everything registered, in registration order.
		//
		// @return The operators.
		const std::vector<Operator> &All() const {
			return Registered;
		}

		// The operators matching a palette query, best first.
		//
		// **Ranked by the same `FuzzyMatch` the explorer and the properties
		// panel filter with**, because three different notions of "matches what
		// I typed" in one program is three things to learn. An empty query
		// matches everything, in registration order.
		//
		// Unavailable operators are **included**, so that the palette can show
		// them greyed with their reason. Filtering them out would answer "why
		// can I not find Delete" with silence.
		//
		// @param query What was typed.
		// @return Pointers into `All`, stable until the next `Add`.
		std::vector<const Operator *> Matching(std::string_view query) const;

	  private:
		std::vector<Operator> Registered;
	};
}
