#pragma once

// arch-waiver public-header: forward graph API. Editors and execution hosts
// consume this durable typed-node contract.

// Typed node declarations and the built-in node set.
//
// The canvas owns presentation. This file owns the durable node contract that
// both an editor and an execution host use: stable port ids, connection rules,
// defaults, bypass routes, and device-free evaluators.
//
// @tier L9 · shared

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace engine::graph {

	// Stable, serialized value type identifiers.
	inline constexpr std::string_view VALUE_TYPE_ANY = "any";
	inline constexpr std::string_view VALUE_TYPE_BOOLEAN = "boolean";
	inline constexpr std::string_view VALUE_TYPE_IMAGE = "image";
	inline constexpr std::string_view VALUE_TYPE_NUMBER = "number";
	inline constexpr std::string_view VALUE_TYPE_STRING = "string";

	// A serialized payload for a schema type the scalar node set does not own.
	// It stays copyable and device-free, so an image or a custom node-set value
	// can cross an execution boundary without a host pointer.
	struct NodeOpaqueValue {
		std::string ValueType;
		std::vector<std::byte> Bytes;

		auto operator<=>(const NodeOpaqueValue &) const = default;
	};

	// Values the built-in logical and scalar nodes can execute without a host
	// adapter, plus opaque serialized values for image and custom node sets.
	using NodeValue = std::variant<std::monostate, double, bool, std::string, NodeOpaqueValue>;
	using NodeValues = std::unordered_map<std::string, NodeValue>;

	// Whether a port receives or produces a value.
	enum class NodePortDirection : uint8_t { Input, Output };

	// The authored state used when a node temporarily passes a declared input to
	// a declared output instead of evaluating.
	enum class NodeBypassMode : uint8_t { None, Bypass };

	// How two schema port types relate before there is an actual value.
	enum class NodeTypeCompatibility : uint8_t {
		Exact,
		DestinationAny,
		SourceAny,
		Incompatible,
	};

	// A stable named input or output. Zero means an input may have unlimited
	// links. Outputs are fan-out by default and ignore this field.
	struct NodePortSchema {
		std::string Id;
		std::string ValueType;
		NodePortDirection Direction = NodePortDirection::Input;
		size_t MaxConnections = 1;
		NodeValue DefaultValue{};
		bool Optional = false;
	};

	// A safe route a node may use while it is bypassed.
	struct NodeBypassMapping {
		std::string Input;
		std::string Output;
	};

	// Why a schema or schema registry entry was refused.
	enum class NodeSchemaStatus : uint8_t {
		Ok,
		EmptySchemaId,
		EmptyPortId,
		EmptyValueType,
		DuplicatePortId,
		DuplicateSchemaId,
		UnknownBypassInput,
		UnknownBypassOutput,
		InvalidBypassType,
		DuplicateBypassOutput,
	};

	// Why evaluation did not yield a usable output set.
	enum class NodeEvaluationStatus : uint8_t {
		Ok,
		MissingInput,
		UnknownInput,
		InputTypeMismatch,
		UnknownOutput,
		OutputTypeMismatch,
		EvaluatorFailed,
	};

	struct NodeSchema;
	using NodeEvaluator =
		std::function<bool(const NodeSchema &, const NodeValues &, NodeValues &, std::string &)>;

	// The durable declaration for one node type.
	struct NodeSchema {
		std::string Id;
		std::string Title;
		std::vector<NodePortSchema> Ports;
		std::vector<NodeBypassMapping> BypassMappings;
		NodeEvaluator Evaluate;
	};

	// Returns a stable type identifier for a concrete built-in value. A missing
	// value has no type and is represented by an empty view.
	std::string_view ValueTypeOf(const NodeValue &value);

	// Checks whether a type has a built-in scalar payload representation.
	bool HasBuiltinValueRepresentation(std::string_view valueType);

	// Performs the connection-time type check. A source `any` is accepted but
	// must be checked against the actual value by the execution runtime.
	NodeTypeCompatibility ComparePortTypes(std::string_view source, std::string_view destination);

	// Whether `ComparePortTypes` permits a connection.
	bool ArePortTypesCompatible(std::string_view source, std::string_view destination);

	// Validates all port ids and bypass routes in a schema.
	NodeSchemaStatus ValidateNodeSchema(const NodeSchema &schema, std::string *reason = nullptr);

	// Runs a schema evaluator after resolving declared defaults and validating
	// typed inputs and outputs. Values on undeclared ports are refused. The
	// caller owns the result map.
	NodeEvaluationStatus EvaluateNode(
		const NodeSchema &schema, const NodeValues &inputs, NodeValues &outputs, std::string *reason = nullptr
	);

	// A registry of node types. Entries are copied so a node set can be built
	// from local declarations then safely handed to a graph runtime.
	class NodeSchemaRegistry {
	  public:
		NodeSchemaStatus Add(NodeSchema schema, std::string *reason = nullptr);

		const NodeSchema *Find(std::string_view id) const;

		std::span<const NodeSchema> All() const {
			return Schemas;
		}

		size_t Count() const {
			return Schemas.size();
		}

	  private:
		std::vector<NodeSchema> Schemas;
		std::unordered_map<std::string, size_t> Indices;
	};

	// The engine-supplied basic logical, scalar, comparison, flow, and explicit
	// conversion nodes. The registry remains immutable after first use.
	const NodeSchemaRegistry &BuiltinNodeSchemas();
}
