#pragma once

// Device-free node declarations shared by domain compilers and authoring tools.
// Defaults, values and evaluators remain in the domain that consumes the schema.
// @tier L2 · shared

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine::graphcore {

	// Dense declaration index, valid only for one compiled graph.
	using NodeIndex = uint32_t;

	// Sentinel used when a refusal has no dense node location.
	inline constexpr NodeIndex NO_NODE = std::numeric_limits<NodeIndex>::max();

	// Whether a port receives or produces domain data.
	enum class PortDirection : uint8_t { Input, Output };

	// Connection-time compatibility; wildcard values still require runtime checks.
	enum class TypeCompatibility : uint8_t { Exact, DestinationAny, SourceAny, Incompatible };

	// Why a declaration or compilation was refused.
	enum class CompileStatus : uint8_t {
		Ok,
		LimitExceeded,
		InvalidIdentity,
		DuplicateSchema,
		DuplicateNode,
		DuplicatePort,
		InvalidDirection,
		InvalidBypass,
		UnknownSchema,
		UnknownNode,
		UnknownPort,
		TypeMismatch,
		TooManyConnections,
		DuplicateConnection,
		MissingInput,
		InvalidDependency,
		Cycle,
		AllocationFailed,
	};

	// Explicit admission bounds; zero accepts no entries of that kind.
	struct Limits {
		// Largest node declaration count.
		size_t MaximumNodes = 0;
		// Largest authored connection or dependency count, including duplicates.
		size_t MaximumConnections = 0;
		// Largest supplied schema count.
		size_t MaximumSchemas = 0;
		// Largest total port count across all supplied schemas.
		size_t MaximumPorts = 0;
		// Largest combined byte count of every supplied identity and type token.
		size_t MaximumIdentityBytes = 0;
	};

	// Bounded refusal location and an optional closed cycle path of dense nodes.
	struct Diagnostic {
		// Machine-readable result of the last operation.
		CompileStatus Status = CompileStatus::Ok;
		// Node or schema identity, truncated to 128 bytes.
		std::string Node{};
		// Port identity, truncated to 128 bytes.
		std::string Port{};
		// Lowest blocked declaration on cycle refusal, including blocked downstream nodes.
		NodeIndex Blocked = NO_NODE;
		// Directed cycle with its first node repeated at the end.
		std::vector<NodeIndex> Cycle{};
	};

	// One named input or output, without an executable payload representation.
	struct PortSchema {
		// Durable node-local identity; unique within its direction.
		std::string Id{};
		// Exact nonempty type token; only `any` is a wildcard.
		std::string ValueType{};
		// Which side of a connection accepts this port.
		PortDirection Direction = PortDirection::Input;
		// Input connection cap, with zero unlimited; ignored for outputs.
		size_t MaximumConnections = 1;
		// Whether an input may remain disconnected; ignored for outputs.
		bool Optional = false;
	};

	// Declared forwarding route used by a domain's bypass evaluator.
	struct BypassMapping {
		// Input port supplying the forwarded value.
		std::string Input{};
		// Output port receiving the forwarded value.
		std::string Output{};
	};

	// Pure declaration of one domain-owned node kind.
	struct NodeSchema {
		// Durable node-kind identity.
		std::string Id{};
		// Ports in authored declaration order.
		std::vector<PortSchema> Ports{};
		// Typed bypass routes; each output may appear at most once.
		std::vector<BypassMapping> BypassMappings{};
	};

	// Stable diagnostic text for a refusal code.
	const char *Describe(CompileStatus status);

	// Compares exact type tokens and the explicit `any` wildcard. Empty tokens refuse.
	TypeCompatibility ComparePortTypes(std::string_view source, std::string_view destination);

	// Validates bounded schema declarations, directional identities and bypass routes.
	// No declaration is installed or modified; the diagnostic is reset on success.
	CompileStatus
	ValidateSchemas(std::span<const NodeSchema> schemas, const Limits &limits, Diagnostic &diagnostic);
}
