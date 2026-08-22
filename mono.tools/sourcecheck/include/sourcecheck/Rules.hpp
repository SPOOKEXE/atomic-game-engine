#pragma once

// The four architecture rules that live in source text rather than in the
// target graph.
//
// `mono.tools/architecture/CheckTargetGraph.cmake` checks the rules that are
// visible in CMake's own output - the module set, the tiers, the link sets and
// the layer heights. These four are not visible there, and `docs/CODE_ARCH.md`
// §11 listed all four as convention until v0.19:
//
// | Rule | Root `AGENTS.md` | What this decides |
// |---|---|---|
// | `ecs-copy` | rule 2 | a long-lived object holding data the store owns |
// | `world-pointer` | rule 3 | a type marked as crossing that reaches a pointer |
// | `name-id` | rule 4 | `core::Name::Id()` reaching a serialiser |
// | `public-header` | §3 | a header in `include/` nothing outside the module includes |
//
// **Each of them says what it does not catch, and those sentences are the
// point.** A check whose limits are not written down is read as a proof, and
// three of these four are heuristics over declarations. The fixtures under
// `tests/fixtures/` are the other half of the same honesty: every rule has an
// input that must fail with a named message and an input that must pass, for
// `CheckTargetGraph.cmake`'s reason - a walker over an expectation it cannot
// parse reports success, and so would a scanner over a tree it cannot read.
//
// @tier L0 · shared

#include <cstddef>
#include <sourcecheck/Source.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace sourcecheck {

	// What a finding's `// arch-waiver` comment, if any, said about it.
	enum class State : uint8_t {
		// No waiver. Fails the build for a gating rule.
		Open,

		// Waived with a reason. The design is deliberate and the reason says so.
		Waived,

		// Waived with a reason beginning `known violation`.
		//
		// **A third state, because without it a rule can only gate by lying.** A
		// violation everybody agrees is one and nobody can fix this afternoon
		// leaves two choices: an ordinary waiver, which files it under
		// "deliberate" and hides it, or a permanently red check, which is the one
		// people learn to skip. This is the third - printed on every run, counted
		// apart from the waived, and not fatal, so a *new* copy still cannot land
		// while the old one stays in front of whoever reads the output.
		//
		// Nothing in the tree carries one at v0.19. `docs/ARCH_REVIEW.md` A4 was
		// this rule's first finding and was fixed while the rule was being
		// written, which is the outcome this state exists to make optional rather
		// than mandatory.
		Known,
	};

	// One thing wrong with one declaration, phrased for somebody reading a
	// build log.
	struct Finding {
		// `ecs-copy`, `world-pointer`, `name-id` or `public-header`.
		std::string Rule;

		// Relative to the scanned root.
		std::string Path;

		// One-based. The declaration, not the record that holds it.
		size_t Line = 0;

		// One sentence, naming what was found and why it is a problem.
		std::string Message;

		// Whether a waiver covers it, and which kind.
		State Status = State::Open;

		// The waiver's reason, when there is one.
		std::string Reason;
	};

	// What one scan found, and what it had to work with.
	struct Report {
		// In rule order, then file order.
		std::vector<Finding> Findings;

		// First-party files read.
		size_t Scanned = 0;

		// Distinct types passed to `ecs::Components::Register`.
		size_t Components = 0;

		// Records marked `// arch-crossing`.
		size_t Crossings = 0;

		// Headers under an `include/` directory.
		size_t PublicHeaders = 0;
	};

	// A module does not keep a private copy of data the ECS owns.
	//
	// **Catches** a record that declares a function - so it is an object that
	// lives across frames rather than an argument list - holding, by value or
	// inside a container, either a type registered with
	// `ecs::Components::Register` or an enumeration declared beside a registered
	// component and used as one of its fields. The second half is what found
	// `docs/ARCH_REVIEW.md` A4 - the client's copy of `scene::InputState`'s mouse
	// behaviour was an enumeration, not a component, so a rule matching whole
	// components would have missed the instance it was written from. That copy is
	// gone at v0.19 and the fixture that stands in for it is not.
	//
	// **Does not catch** a copy whose type is a primitive. `InputState` carries
	// `MouseIconEnabled` as a `bool`, and a `bool` beside it somewhere else
	// carries no identity for a scanner to match on. It also does not catch a
	// copy reached through a macro or a `using` alias, and it does not know how
	// long a holder actually lives - the function test is a proxy for that, and
	// the nine waivers in this tree are where the proxy is wrong.
	//
	// @param tree The scanned tree.
	// @return Findings, waived or open.
	std::vector<Finding> CheckEcsCopy(const Tree &tree);

	// Nothing crossing a world boundary is a pointer.
	//
	// **Catches** a pointer, a reference, a `unique_ptr`, `shared_ptr`,
	// `weak_ptr`, `span`, `string_view`, `function`, `reference_wrapper` or
	// `any`, anywhere in the transitive field closure of a record marked
	// `// arch-crossing`. The marker is a comment above the declaration rather
	// than a list in a build file, so a type that starts crossing declares that
	// where it is defined.
	//
	// **Does not catch** a type that crosses without the marker. That is the
	// price of not having a front end, and it is why the marker sits in
	// `world/Bus.hpp` beside the paragraph that already says the same thing in
	// prose. It also stops at a type it cannot resolve to a declaration in the
	// tree - a vendor type in a message would be invisible, which is one more
	// reason `VENDOR_PUBLIC` is rationed.
	//
	// @param tree The scanned tree.
	// @return Findings, waived or open.
	std::vector<Finding> CheckWorldPointer(const Tree &tree);

	// A name crosses boundaries. A number does not.
	//
	// **Catches** `Id()` inside the arguments of a call whose name begins
	// `Write`, `Encode`, `Serialise`, `Serialize`, `Emit` or `Put`; a
	// `sizeof(...Name...)` inside one of those, which is the object-representation
	// write that produced the `client::DrawList` failure at v0.7; and
	// `Name::FromId` fed from a `Read` call, which is the same violation on the
	// way back in.
	//
	// **The compiler already owns half of this rule.** `core::Name` has no
	// implicit conversion to an integer - its only conversion operator is an
	// `explicit operator bool` - so `WriteUInt32(name)` does not compile and
	// never has. What is left is the spelling somebody reaches for when it does
	// not: `.Id()`, written out, which is what this reads.
	//
	// **Does not catch** an id that reaches a writer through a variable, a
	// struct field or another function. The type system would catch those too if
	// `Id()` returned something a writer refused, and it returns a `uint32_t`
	// instead: changing that would touch all one hundred and sixty-nine call
	// sites, and all but a handful of them are the map keys, sorts and
	// comparisons the dense integer exists to make cheap. So this is the direct
	// path only, and the direct path is the one somebody writes by accident.
	//
	// @param tree The scanned tree.
	// @return Findings, waived or open.
	std::vector<Finding> CheckNameId(const Tree &tree);

	// A public header is one somebody outside the module includes.
	//
	// **Catches** a header under a module's `include/` that no file outside that
	// module includes, and that the module's own `app/` does not include either -
	// a program consumes its own surface from its `main.cpp`, and counting that
	// is what keeps every program's headers from being reported as private.
	//
	// **Does not fail the build, ever.** Root `AGENTS.md` warns that an unwired
	// subsystem is not dead code: `IssueContentGrant` and the viewpoint pair are
	// deliberate forward API, complete and frozen with nothing calling them, and
	// decision 16 says that is a state rather than a stage. So this reports, and
	// `// arch-waiver public-header: <reason>` anywhere in a header takes it off
	// the list with the reason in the file rather than in a build script.
	//
	// **Does not catch** a header that one other module includes once and should
	// not, which is the other half of §3's judgement and is not mechanical.
	//
	// @param tree The scanned tree.
	// @return Findings, waived or open.
	std::vector<Finding> CheckPublicHeader(const Tree &tree);

	// Every rule, in the order `docs/CODE_ARCH.md` §11 lists them.
	//
	// @param tree The scanned tree.
	// @return The findings and what the scan had to work with.
	Report Check(const Tree &tree);

	// Whether an open finding for a rule fails the build.
	//
	// `public-header` does not, and says why above.
	//
	// @param rule The rule name.
	// @return `true` when an open finding is fatal.
	bool Gating(std::string_view rule);

	// The rule names, in §11's order.
	//
	// @return Four names.
	std::vector<std::string> RuleNames();
}
