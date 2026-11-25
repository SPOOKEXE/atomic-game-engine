#pragma once

// Data types and node definitions a host makes available to its graphs.
//
// There is no global registry. Studio, the client and each script context own
// their own, so a script cannot change the node types the engine registered.
// @tier L3 · shared

#include <engine/ang/Value.hpp>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine::ang {

	class Graph;
	struct NodeInstance;

	// Whether a port receives or produces values.
	enum class Direction : uint8_t { Input, Output };

	// How a node's Evaluate is scheduled. The graph's Domain limits which are legal.
	enum class ExecMode : uint8_t {
		// Runs on the calling thread in wave order.
		Sync,
		// Runs beside the other Parallel nodes of its wave through Jobs::For.
		Parallel,
		// Runs on an AsyncPool worker; Run never waits for it. Tool and Frame only.
		Async,
	};

	// Whether the same inputs always give the same outputs. Only Pure results are cached.
	enum class Purity : uint8_t { Pure, Impure };

	// Who runs a graph, which decides the modes it may use. Chosen by the host.
	enum class Domain : uint8_t {
		// Simulation. Sync and Parallel only, so a tick starts and finishes.
		Tick,
		// Presentation. Async allowed for work that may land frames late.
		Frame,
		// Editors and offline tools. Async on CPU workers.
		Tool,
	};

	const char *DomainName(Domain domain);
	std::optional<Domain> DomainFromName(std::string_view name);

	// An implicit conversion a link may pass through.
	struct Conversion {
		TypeId To{};
		std::function<Value(const Value &)> Convert{};
	};

	// Short inspector text for a value, shown by views on a wire or port.
	using PreviewFn = std::function<std::string(const Value &)>;

	// One registered data type.
	struct DataType {
		TypeId Id{};
		std::string Label{};
		// Wire colour hint for views. No UI type lives here.
		core::Color3 Wire{1.0f, 1.0f, 1.0f};
		std::vector<Conversion> ConvertsTo{};
		// Resident bytes of one value, for memory accounting. Null uses Value::Bytes.
		std::function<size_t(const Value &)> Bytes{};
		PreviewFn Preview{};
	};

	// One input or output of a node definition.
	struct PortDef {
		// Durable within its node type and direction.
		std::string Name{};
		// Concrete type, or ignored when Generic is set.
		TypeId Type{};
		Direction Dir = Direction::Input;
		// An unlinked, unsupplied input without a Default is allowed when true.
		bool Optional = false;
		// Input link cap; 0 is unlimited. Outputs always fan out.
		uint16_t MaximumLinks = 1;
		// Used when the input is unlinked and the run supplies nothing.
		Value Default{};
		// Generic name such as "T". The first link resolves it for every port
		// on the node that names the same generic.
		std::string Generic{};
		// Types the generic may resolve to; empty accepts any registered type.
		std::vector<TypeId> Constraints{};
	};

	// An editable setting on a node. Widget is a hint ("slider", "color"), never widget code.
	struct ParamDef {
		std::string Name{};
		TypeId Type{};
		Value Default{};
		std::string Label{};
		std::string Widget{};
		double Minimum = 0.0;
		double Maximum = 0.0;
	};

	// Forwarding used when a node is bypassed: the input's value appears on the output.
	struct BypassRoute {
		std::string Input{};
		std::string Output{};
	};

	class EvalContext;

	// What one evaluation reported.
	struct NodeResult {
		bool Ok = true;
		std::string Note{};

		static NodeResult Success() {
			return {};
		}
		static NodeResult Failure(std::string note) {
			return {false, std::move(note)};
		}
	};

	using EvaluateFn = std::function<NodeResult(EvalContext &)>;

	// Upgrades a saved node from `fromVersion` to `fromVersion + 1`. False refuses.
	using MigrateFn = std::function<bool(NodeInstance &, uint32_t fromVersion)>;

	// One node type.
	struct NodeDef {
		// Durable, e.g. "image.blur".
		std::string Id{};
		uint32_t Version = 1;
		std::string Category{};
		std::string Label{};
		std::vector<PortDef> Inputs{};
		std::vector<PortDef> Outputs{};
		std::vector<ParamDef> Params{};
		ExecMode Mode = ExecMode::Sync;
		Purity Pure = Purity::Pure;
		// Null evaluates nothing; outputs stay empty and the node reports Skipped.
		EvaluateFn Evaluate{};
		MigrateFn Migrate{};
		std::vector<BypassRoute> Bypass{};

		const PortDef *FindInput(std::string_view name) const;
		const PortDef *FindOutput(std::string_view name) const;
		const ParamDef *FindParam(std::string_view name) const;
	};

	// One port a subgraph exposes, and the inner node port it maps to.
	struct SubgraphPort {
		PortDef Port{};
		std::string Node{};
		std::string NodePort{};
	};

	// A graph used as a node. Compile flattens it; recursion is refused.
	struct SubgraphDef {
		std::string Id{};
		std::string Label{};
		std::string Category{};
		std::shared_ptr<const Graph> Body{};
		std::vector<SubgraphPort> Inputs{};
		std::vector<SubgraphPort> Outputs{};
	};

	// Why a definition was refused.
	struct RegistryResult {
		bool Ok = true;
		std::string Error{};
	};

	// How a link between two types is satisfied.
	struct LinkCheck {
		bool Ok = false;
		// Set when the link passes through a conversion.
		const Conversion *Via = nullptr;
	};

	// The host's set of types and node definitions.
	class Registry {
	  public:
		// Starts with only the `any` wildcard.
		Registry();

		// The scalar types every Value constructor uses, with numeric conversions:
		// bool, int, number, vector2, vector3, vector4, color3, string, name.
		static Registry WithScalars();

		// Refuses an empty id, a duplicate id or a redefinition of `any`.
		RegistryResult DefineType(DataType type);
		// Refuses duplicates, empty or repeated port names and unknown port types.
		RegistryResult DefineNode(NodeDef node);
		// Refuses an id already used by a node or subgraph, or a missing body.
		RegistryResult DefineSubgraph(SubgraphDef subgraph);

		const DataType *FindType(std::string_view id) const;
		std::shared_ptr<const NodeDef> FindNode(std::string_view id) const;
		const SubgraphDef *FindSubgraph(std::string_view id) const;

		// Definitions in the order they were added, for palettes and search.
		std::span<const std::shared_ptr<const NodeDef>> Nodes() const {
			return NodeList;
		}
		std::span<const DataType> Types() const {
			return TypeList;
		}

		// Compatibility of a link from `from` to `to`: exact, the `any` wildcard,
		// or one registered conversion. An unregistered type connects to nothing.
		LinkCheck CheckLink(std::string_view from, std::string_view to) const;

		// Resident bytes of a value, using the type's Bytes hook when present.
		size_t BytesOf(const Value &value) const;

	  private:
		std::vector<DataType> TypeList{};
		std::vector<std::shared_ptr<const NodeDef>> NodeList{};
		std::vector<SubgraphDef> SubgraphList{};
		std::unordered_map<std::string, size_t> TypeIndex{};
		std::unordered_map<std::string, size_t> NodeIndex{};
		std::unordered_map<std::string, size_t> SubgraphIndex{};
	};

	// Reads a type name from an older document. `data.ANY` becomes `any`.
	TypeId NormaliseType(std::string_view text);
}
