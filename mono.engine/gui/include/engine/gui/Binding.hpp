#pragma once

// Declarative GUI values resolved before layout.
//
// A binding owns its result instead of writing an authored property. Layout and
// compile read that result through the named target, so evaluating a binding
// never invokes a script callback or overwrites what an author entered.
//
// @tier L7 · shared

#include <engine/core/Name.hpp>
#include <engine/ecs/Entity.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace engine::ecs {
	class Store;
}

namespace engine::gui {

	// Why a binding could not publish a value.  It remains derived state so an
	// authored fallback cannot hide an invalid source from Studio or tests.
	enum class BindingFailure : uint8_t {
		None,
		InvalidSourcePath,
		MissingSource,
		MissingAttribute,
		TypeMismatch,
		UnsupportedTarget,
		OutputTooLong,
	};

	// An authored attribute-to-property binding. It is a child of its target.
	struct Binding {
		// Relative path to the source instance.
		std::string SourcePath;
		// Source attribute to read.
		core::Name Attribute;
		// Bound target property name.
		core::Name Target{"Text"};
		// Value used when the binding cannot resolve.
		std::string Fallback;
	};

	// The derived result of a binding evaluation.
	struct BindingOutput {
		// Last resolved text value.
		std::string Value;
		// Whether Value came from a valid source.
		bool Valid = false;
		// Reason the last evaluation failed.
		BindingFailure Failure = BindingFailure::None;
		// Revision of the source used for Value.
		uint64_t SourceRevision = 0;
		// Number of evaluations performed.
		uint64_t EvaluationCount = 0;
	};

	// The resolved source and exact attribute version used by one binding. This
	// is local derived state, never an authored reference or serialized row.
	struct BindingDependency {
		// Resolved source instance.
		ecs::Entity Source = ecs::NULL_ENTITY;
		// Revision of the binding configuration.
		uint64_t ConfigurationStamp = 0;
		// Revision of Source's read attribute.
		uint64_t SourceRevision = 0;
	};

	// Registers the binding components under stable schema names.
	void RegisterBindingComponents();

	// Resolves every binding without running scripts or mutating target fields.
	// Returns the number whose published output changed.
	size_t EvaluateBindings(ecs::Store &store);

	// Returns the resolved String target for an instance, or `fallback` when no
	// valid binding targets it.
	std::string_view BoundText(const ecs::Store &store, ecs::Entity instance, std::string_view fallback);
}
