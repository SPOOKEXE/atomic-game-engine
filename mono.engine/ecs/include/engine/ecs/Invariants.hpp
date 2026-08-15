#pragma once

// The rules `TypeDescriptor` can only write down, asked of every type that is
// actually registered.
//
// `DescribeType` carries two warnings - a raw writer must not see padding, and
// must not see a `core::Name` - and `Schema.hpp` carries a third about a
// hand-built layout. All three were documentation: the storage is type-erased
// by the time anything could check them, and the compiler is gone. AGENTS.md
// rule 6 says a rule the build does not check is documentation, and this is
// what stops these three being that.
//
// **It runs over the registry rather than over a list.** A test naming the
// components it cares about is a list that goes stale the day somebody adds the
// thirty-ninth one, and the components that have gone wrong here - a limb
// carrying eight indeterminate bytes into every save, a source container whose
// name crossed as an interning index - were all types nobody thought to name.
// So the sweep asks `Components::Count()` and every answer is included by
// default.
//
// What a binary is swept for is what that binary registered, so a module's own
// suite covers its own components and a suite in a program that links
// everything covers the lot. Both are worth having: the first says which module
// broke it, the second says nothing escaped.
//
// @tier L3 · shared

#include <engine/core/Name.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace engine::ecs {

	// One registered type failing one rule.
	//
	// @since v0.15
	struct ComponentComplaint {
		// The component's registered name, which is what a file carries and
		// therefore what a person fixing this needs to find it by.
		core::Name Component;

		// What is wrong, phrased as the rule that was broken and what to do
		// about it. Read straight into a test failure, so it has to stand on
		// its own without the reader having this header open.
		std::string Rule;
	};

	// Every registered type against every rule this can check.
	//
	// Ordered by component id, so two runs of one binary produce the same list
	// and a diff between them is a change rather than a reshuffle.
	//
	// Costs one construct, one write and one round trip per registered type, so
	// it is a test's to call and not a tick's.
	//
	// @return The complaints, empty when the registry is clean.
	// @since v0.15
	std::vector<ComponentComplaint> AuditComponents();

	// The same sweep, kept to the components whose registered name starts with
	// `prefix`.
	//
	// One module's suite wants its own components and not the ones another
	// module registered into the same binary, and `"scene."` is how a component
	// says which module it belongs to. An empty prefix is every component,
	// which is what a program linking everything should ask for.
	//
	// @param prefix The registered-name prefix to keep.
	// @return The complaints about those components, empty when they are clean.
	// @since v0.15
	std::vector<ComponentComplaint> AuditComponents(std::string_view prefix);

	// Every complaint as one block of text, one to a line.
	//
	// For a test that wants the whole verdict in its failure message rather
	// than the first line of it. Empty when there are no complaints, so it
	// reads as the thing to compare against `""`.
	//
	// @param complaints What `AuditComponents` returned.
	// @return The complaints, one per line, or an empty string.
	// @since v0.15
	std::string Describe(const std::vector<ComponentComplaint> &complaints);

	// Whether this build can check the padding rule.
	//
	// False on a toolchain without `__builtin_clear_padding`, where
	// `AuditComponents` still checks everything else. A caller that wants the
	// padding rule enforced somewhere should assert on this in one place rather
	// than let every suite quietly pass without it.
	//
	// @return `true` when the padding rule is among the ones being checked.
	// @since v0.15
	bool AuditChecksPadding();

	// One declared property failing one rule.
	//
	// @since v0.15
	struct PropertyComplaint {
		// The class the property is declared on. An inherited property is
		// reported against every class exposing it, because a getter that needs
		// a component only some of them carry is a defect of the pairing rather
		// than of either half.
		core::Name Class;

		// The property's name, as a script spells it.
		core::Name Property;

		// What is wrong and what to do about it.
		std::string Rule;
	};

	// Every declared property against every rule this can check.
	//
	// **The class table's half of `AuditComponents`, and it is here for the
	// same reason.** `PropertyDescriptor` documents which of its fields have to
	// agree with which - a read-only property needs no setter, an `Enum` names a
	// set and nothing else does, a write has to reach the bytes through
	// `GetMutable` or `replication` never sends what a script wrote - and until
	// this ran, every one of those was a sentence in a header.
	//
	// The round-trip half needs somewhere to put an instance, so a scratch world
	// is built and thrown away.
	//
	// @return The complaints, empty when the class table is clean.
	// @since v0.15
	std::vector<PropertyComplaint> AuditProperties();

	// Every complaint as one block of text, one to a line.
	//
	// @param complaints What `AuditProperties` returned.
	// @return The complaints, one per line, or an empty string.
	// @since v0.15
	std::string Describe(const std::vector<PropertyComplaint> &complaints);
}
