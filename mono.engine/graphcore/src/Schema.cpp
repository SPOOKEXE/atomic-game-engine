#include "Validation.hpp"

#include <engine/graphcore/Schema.hpp>

#include <new>
#include <unordered_map>
#include <unordered_set>

namespace engine::graphcore {

	const char *Describe(CompileStatus status) {
		switch (status) {
		case CompileStatus::Ok:
			return "ok";
		case CompileStatus::LimitExceeded:
			return "graph admission limit exceeded";
		case CompileStatus::InvalidIdentity:
			return "identity is empty or contains a null byte";
		case CompileStatus::DuplicateSchema:
			return "schema identity is duplicated";
		case CompileStatus::DuplicateNode:
			return "node identity is duplicated";
		case CompileStatus::DuplicatePort:
			return "directional port identity is duplicated";
		case CompileStatus::InvalidDirection:
			return "port direction is invalid";
		case CompileStatus::InvalidBypass:
			return "bypass route is missing, duplicated or incompatible";
		case CompileStatus::UnknownSchema:
			return "node names an unknown schema";
		case CompileStatus::UnknownNode:
			return "connection names an unknown node";
		case CompileStatus::UnknownPort:
			return "connection names a missing or wrong-direction port";
		case CompileStatus::TypeMismatch:
			return "connection port types are incompatible";
		case CompileStatus::TooManyConnections:
			return "input connection limit exceeded";
		case CompileStatus::DuplicateConnection:
			return "exact connection is duplicated";
		case CompileStatus::MissingInput:
			return "required input is disconnected";
		case CompileStatus::InvalidDependency:
			return "dependency endpoint or ordering policy is invalid";
		case CompileStatus::Cycle:
			return "graph contains a dependency cycle";
		case CompileStatus::AllocationFailed:
			return "graph allocation refused";
		}
		return "unknown graph status";
	}

	TypeCompatibility ComparePortTypes(std::string_view source, std::string_view destination) {
		if (source.empty() || destination.empty()) return TypeCompatibility::Incompatible;
		if (source == destination) return TypeCompatibility::Exact;
		if (destination == "any") return TypeCompatibility::DestinationAny;
		if (source == "any") return TypeCompatibility::SourceAny;
		return TypeCompatibility::Incompatible;
	}

	namespace detail {
		CompileStatus
		Refuse(Diagnostic &diagnostic, CompileStatus status, std::string_view node, std::string_view port) {
			diagnostic = {};
			diagnostic.Status = status;
			diagnostic.Node.assign(node.substr(0, 128));
			diagnostic.Port.assign(port.substr(0, 128));
			return status;
		}

		CompileStatus CountIdentity(
			std::string_view text,
			size_t &bytes,
			const Limits &limits,
			Diagnostic &diagnostic,
			std::string_view node,
			std::string_view port
		) {
			if (text.empty()) return Refuse(diagnostic, CompileStatus::InvalidIdentity, node, port);
			if (text.size() > limits.MaximumIdentityBytes - bytes)
				return Refuse(diagnostic, CompileStatus::LimitExceeded, node, port);
			if (text.find('\0') != std::string_view::npos)
				return Refuse(diagnostic, CompileStatus::InvalidIdentity, node, port);
			bytes += text.size();
			return CompileStatus::Ok;
		}

		CompileStatus CheckSchemaBudgets(
			std::span<const NodeSchema> schemas, const Limits &limits, size_t &bytes, Diagnostic &diagnostic
		) {
			if (schemas.size() > limits.MaximumSchemas || schemas.size() > NO_NODE)
				return Refuse(diagnostic, CompileStatus::LimitExceeded);
			size_t ports = 0;
			for (const NodeSchema &schema : schemas) {
				if (schema.Ports.size() > limits.MaximumPorts - ports || schema.Ports.size() > NO_NODE ||
					schema.BypassMappings.size() > schema.Ports.size())
					return Refuse(diagnostic, CompileStatus::LimitExceeded, schema.Id);
				ports += schema.Ports.size();
				if (CountIdentity(schema.Id, bytes, limits, diagnostic, schema.Id) != CompileStatus::Ok)
					return diagnostic.Status;
				for (const PortSchema &port : schema.Ports) {
					if (CountIdentity(port.Id, bytes, limits, diagnostic, schema.Id, port.Id) !=
							CompileStatus::Ok ||
						CountIdentity(port.ValueType, bytes, limits, diagnostic, schema.Id, port.Id) !=
							CompileStatus::Ok)
						return diagnostic.Status;
				}
				for (const BypassMapping &mapping : schema.BypassMappings) {
					if (CountIdentity(mapping.Input, bytes, limits, diagnostic, schema.Id, mapping.Input) !=
							CompileStatus::Ok ||
						CountIdentity(mapping.Output, bytes, limits, diagnostic, schema.Id, mapping.Output) !=
							CompileStatus::Ok)
						return diagnostic.Status;
				}
			}
			return CompileStatus::Ok;
		}
	}

	CompileStatus
	ValidateSchemas(std::span<const NodeSchema> schemas, const Limits &limits, Diagnostic &diagnostic) {
		try {
			size_t bytes = 0;
			if (detail::CheckSchemaBudgets(schemas, limits, bytes, diagnostic) != CompileStatus::Ok)
				return diagnostic.Status;
			std::unordered_set<std::string_view> names;
			for (const NodeSchema &schema : schemas) {
				if (!names.insert(schema.Id).second)
					return detail::Refuse(diagnostic, CompileStatus::DuplicateSchema, schema.Id);
				std::unordered_map<std::string_view, const PortSchema *> inputs;
				std::unordered_map<std::string_view, const PortSchema *> outputs;
				for (const PortSchema &port : schema.Ports) {
					if (port.Direction != PortDirection::Input && port.Direction != PortDirection::Output)
						return detail::Refuse(
							diagnostic, CompileStatus::InvalidDirection, schema.Id, port.Id
						);
					auto &ports = port.Direction == PortDirection::Input ? inputs : outputs;
					if (!ports.emplace(port.Id, &port).second)
						return detail::Refuse(diagnostic, CompileStatus::DuplicatePort, schema.Id, port.Id);
				}
				std::unordered_set<std::string_view> bypassOutputs;
				for (const BypassMapping &mapping : schema.BypassMappings) {
					const auto input = inputs.find(mapping.Input);
					const auto output = outputs.find(mapping.Output);
					if (input == inputs.end() || output == outputs.end() ||
						!bypassOutputs.insert(mapping.Output).second ||
						ComparePortTypes(input->second->ValueType, output->second->ValueType) ==
							TypeCompatibility::Incompatible)
						return detail::Refuse(
							diagnostic, CompileStatus::InvalidBypass, schema.Id, mapping.Output
						);
				}
			}
			diagnostic = {};
			return CompileStatus::Ok;
		} catch (const std::bad_alloc &) {
			diagnostic = {};
			diagnostic.Status = CompileStatus::AllocationFailed;
			return diagnostic.Status;
		}
	}
}
