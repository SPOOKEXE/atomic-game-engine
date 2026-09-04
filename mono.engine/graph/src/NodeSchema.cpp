#include <engine/graph/NodeSchema.hpp>

#include <algorithm>
#include <exception>
#include <sstream>
#include <unordered_set>

namespace engine::graph {

	namespace {
		const NodePortSchema *
		FindSchemaPort(const NodeSchema &schema, std::string_view id, NodePortDirection direction) {
			const auto found =
				std::find_if(schema.Ports.begin(), schema.Ports.end(), [&](const NodePortSchema &port) {
					return port.Id == id && port.Direction == direction;
				});
			return found == schema.Ports.end() ? nullptr : &*found;
		}

		const NodeValue *FindValue(const NodeValues &values, std::string_view id) {
			const auto found = values.find(std::string(id));
			return found == values.end() ? nullptr : &found->second;
		}

		bool WriteReason(std::string *reason, std::string_view message) {
			if (reason != nullptr) {
				*reason = message;
			}
			return false;
		}

		bool ValueMatches(std::string_view type, const NodeValue &value) {
			if (type == VALUE_TYPE_ANY) {
				return !ValueTypeOf(value).empty();
			}
			return ValueTypeOf(value) == type;
		}

		const NodeValue *Input(const NodeValues &inputs, std::string_view id) {
			return FindValue(inputs, id);
		}

		bool Boolean(const NodeValues &inputs, std::string_view id, bool &out, std::string &reason) {
			const NodeValue *value = Input(inputs, id);
			if (value == nullptr || !std::holds_alternative<bool>(*value)) {
				reason = "input '" + std::string(id) + "' must be boolean";
				return false;
			}
			out = std::get<bool>(*value);
			return true;
		}

		bool Number(const NodeValues &inputs, std::string_view id, double &out, std::string &reason) {
			const NodeValue *value = Input(inputs, id);
			if (value == nullptr || !std::holds_alternative<double>(*value)) {
				reason = "input '" + std::string(id) + "' must be number";
				return false;
			}
			out = std::get<double>(*value);
			return true;
		}

		bool
		LogicalAnd(const NodeSchema &, const NodeValues &inputs, NodeValues &outputs, std::string &reason) {
			bool left = false;
			bool right = false;
			if (!Boolean(inputs, "left", left, reason) || !Boolean(inputs, "right", right, reason)) {
				return false;
			}
			outputs["value"] = left && right;
			return true;
		}

		bool
		LogicalOr(const NodeSchema &, const NodeValues &inputs, NodeValues &outputs, std::string &reason) {
			bool left = false;
			bool right = false;
			if (!Boolean(inputs, "left", left, reason) || !Boolean(inputs, "right", right, reason)) {
				return false;
			}
			outputs["value"] = left || right;
			return true;
		}

		bool
		LogicalNot(const NodeSchema &, const NodeValues &inputs, NodeValues &outputs, std::string &reason) {
			bool value = false;
			if (!Boolean(inputs, "value", value, reason)) {
				return false;
			}
			outputs["result"] = !value;
			return true;
		}

		bool Switch(const NodeSchema &, const NodeValues &inputs, NodeValues &outputs, std::string &reason) {
			bool condition = false;
			if (!Boolean(inputs, "condition", condition, reason)) {
				return false;
			}
			const NodeValue *value = Input(inputs, condition ? "true" : "false");
			if (value == nullptr || std::holds_alternative<std::monostate>(*value)) {
				reason = "switch requires the selected value";
				return false;
			}
			outputs["value"] = *value;
			return true;
		}

		bool Add(const NodeSchema &, const NodeValues &inputs, NodeValues &outputs, std::string &reason) {
			double left = 0.0;
			double right = 0.0;
			if (!Number(inputs, "left", left, reason) || !Number(inputs, "right", right, reason)) {
				return false;
			}
			outputs["value"] = left + right;
			return true;
		}

		bool
		Multiply(const NodeSchema &, const NodeValues &inputs, NodeValues &outputs, std::string &reason) {
			double left = 0.0;
			double right = 0.0;
			if (!Number(inputs, "left", left, reason) || !Number(inputs, "right", right, reason)) {
				return false;
			}
			outputs["value"] = left * right;
			return true;
		}

		bool Clamp(const NodeSchema &, const NodeValues &inputs, NodeValues &outputs, std::string &reason) {
			double value = 0.0;
			double minimum = 0.0;
			double maximum = 0.0;
			if (!Number(inputs, "value", value, reason) || !Number(inputs, "min", minimum, reason) ||
				!Number(inputs, "max", maximum, reason)) {
				return false;
			}
			if (minimum > maximum) {
				reason = "clamp minimum exceeds maximum";
				return false;
			}
			outputs["value"] = std::clamp(value, minimum, maximum);
			return true;
		}

		bool Remap(const NodeSchema &, const NodeValues &inputs, NodeValues &outputs, std::string &reason) {
			double value = 0.0;
			double sourceMinimum = 0.0;
			double sourceMaximum = 0.0;
			double destinationMinimum = 0.0;
			double destinationMaximum = 0.0;
			if (!Number(inputs, "value", value, reason) ||
				!Number(inputs, "sourceMin", sourceMinimum, reason) ||
				!Number(inputs, "sourceMax", sourceMaximum, reason) ||
				!Number(inputs, "targetMin", destinationMinimum, reason) ||
				!Number(inputs, "targetMax", destinationMaximum, reason)) {
				return false;
			}
			if (sourceMinimum == sourceMaximum) {
				reason = "remap source range is empty";
				return false;
			}
			const double progress = (value - sourceMinimum) / (sourceMaximum - sourceMinimum);
			outputs["value"] = destinationMinimum + progress * (destinationMaximum - destinationMinimum);
			return true;
		}

		bool Equal(const NodeSchema &, const NodeValues &inputs, NodeValues &outputs, std::string &reason) {
			const NodeValue *left = Input(inputs, "left");
			const NodeValue *right = Input(inputs, "right");
			if (left == nullptr || right == nullptr || std::holds_alternative<std::monostate>(*left) ||
				std::holds_alternative<std::monostate>(*right)) {
				reason = "equal requires two values";
				return false;
			}
			outputs["value"] = *left == *right;
			return true;
		}

		bool
		GreaterThan(const NodeSchema &, const NodeValues &inputs, NodeValues &outputs, std::string &reason) {
			double left = 0.0;
			double right = 0.0;
			if (!Number(inputs, "left", left, reason) || !Number(inputs, "right", right, reason)) {
				return false;
			}
			outputs["value"] = left > right;
			return true;
		}

		bool Branch(const NodeSchema &, const NodeValues &inputs, NodeValues &outputs, std::string &reason) {
			bool condition = false;
			if (!Boolean(inputs, "condition", condition, reason)) {
				return false;
			}
			const NodeValue *value = Input(inputs, "value");
			if (value == nullptr || std::holds_alternative<std::monostate>(*value)) {
				reason = "branch requires a value";
				return false;
			}
			outputs[condition ? "true" : "false"] = *value;
			return true;
		}

		bool Merge(const NodeSchema &, const NodeValues &inputs, NodeValues &outputs, std::string &reason) {
			const NodeValue *first = Input(inputs, "first");
			const NodeValue *second = Input(inputs, "second");
			if (first != nullptr && !std::holds_alternative<std::monostate>(*first)) {
				outputs["value"] = *first;
				return true;
			}
			if (second != nullptr && !std::holds_alternative<std::monostate>(*second)) {
				outputs["value"] = *second;
				return true;
			}
			reason = "merge requires at least one value";
			return false;
		}

		bool Reroute(const NodeSchema &, const NodeValues &inputs, NodeValues &outputs, std::string &reason) {
			const NodeValue *value = Input(inputs, "value");
			if (value == nullptr || std::holds_alternative<std::monostate>(*value)) {
				reason = "reroute requires a value";
				return false;
			}
			outputs["value"] = *value;
			return true;
		}

		bool ConstantNumber(
			const NodeSchema &, const NodeValues &inputs, NodeValues &outputs, std::string &reason
		) {
			double value = 0.0;
			if (!Number(inputs, "value", value, reason)) {
				return false;
			}
			outputs["value"] = value;
			return true;
		}

		bool ConstantBoolean(
			const NodeSchema &, const NodeValues &inputs, NodeValues &outputs, std::string &reason
		) {
			bool value = false;
			if (!Boolean(inputs, "value", value, reason)) {
				return false;
			}
			outputs["value"] = value;
			return true;
		}

		bool ConstantString(
			const NodeSchema &, const NodeValues &inputs, NodeValues &outputs, std::string &reason
		) {
			const NodeValue *value = Input(inputs, "value");
			if (value == nullptr || !std::holds_alternative<std::string>(*value)) {
				reason = "input 'value' must be string";
				return false;
			}
			outputs["value"] = std::get<std::string>(*value);
			return true;
		}

		bool
		ToString(const NodeSchema &, const NodeValues &inputs, NodeValues &outputs, std::string &reason) {
			const NodeValue *value = Input(inputs, "value");
			if (value == nullptr || std::holds_alternative<std::monostate>(*value)) {
				reason = "to string requires a value";
				return false;
			}
			if (const double *number = std::get_if<double>(value); number != nullptr) {
				std::ostringstream stream;
				stream << *number;
				outputs["value"] = stream.str();
				return true;
			}
			if (const bool *boolean = std::get_if<bool>(value); boolean != nullptr) {
				outputs["value"] = *boolean ? "true" : "false";
				return true;
			}
			if (const std::string *string = std::get_if<std::string>(value); string != nullptr) {
				outputs["value"] = *string;
				return true;
			}
			reason = "to string supports number, boolean, and string values";
			return false;
		}

		NodePortSchema InputPort(std::string id, std::string valueType, NodeValue defaultValue = {}) {
			return {
				.Id = std::move(id),
				.ValueType = std::move(valueType),
				.DefaultValue = std::move(defaultValue)
			};
		}

		NodePortSchema OutputPort(std::string id, std::string valueType, bool optional = false) {
			return {
				.Id = std::move(id),
				.ValueType = std::move(valueType),
				.Direction = NodePortDirection::Output,
				.Optional = optional,
			};
		}

		NodeSchema Schema(
			std::string id,
			std::string title,
			std::vector<NodePortSchema> ports,
			std::vector<NodeBypassMapping> bypassMappings,
			NodeEvaluator evaluator
		) {
			return {
				.Id = std::move(id),
				.Title = std::move(title),
				.Ports = std::move(ports),
				.BypassMappings = std::move(bypassMappings),
				.Evaluate = std::move(evaluator),
			};
		}
	}

	std::string_view ValueTypeOf(const NodeValue &value) {
		if (std::holds_alternative<double>(value)) {
			return VALUE_TYPE_NUMBER;
		}
		if (std::holds_alternative<bool>(value)) {
			return VALUE_TYPE_BOOLEAN;
		}
		if (std::holds_alternative<std::string>(value)) {
			return VALUE_TYPE_STRING;
		}
		if (const auto *opaque = std::get_if<NodeOpaqueValue>(&value); opaque != nullptr) {
			return opaque->ValueType;
		}
		return {};
	}

	bool HasBuiltinValueRepresentation(std::string_view valueType) {
		return valueType == VALUE_TYPE_ANY || valueType == VALUE_TYPE_BOOLEAN ||
			   valueType == VALUE_TYPE_NUMBER || valueType == VALUE_TYPE_STRING;
	}

	NodeTypeCompatibility ComparePortTypes(std::string_view source, std::string_view destination) {
		if (source == destination) {
			return NodeTypeCompatibility::Exact;
		}
		if (destination == VALUE_TYPE_ANY) {
			return NodeTypeCompatibility::DestinationAny;
		}
		if (source == VALUE_TYPE_ANY) {
			return NodeTypeCompatibility::SourceAny;
		}
		return NodeTypeCompatibility::Incompatible;
	}

	bool ArePortTypesCompatible(std::string_view source, std::string_view destination) {
		return ComparePortTypes(source, destination) != NodeTypeCompatibility::Incompatible;
	}

	NodeSchemaStatus ValidateNodeSchema(const NodeSchema &schema, std::string *reason) {
		if (schema.Id.empty()) {
			WriteReason(reason, "schema id is empty");
			return NodeSchemaStatus::EmptySchemaId;
		}

		std::unordered_set<std::string> ids;
		for (const NodePortSchema &port : schema.Ports) {
			if (port.Id.empty()) {
				WriteReason(reason, "port id is empty");
				return NodeSchemaStatus::EmptyPortId;
			}
			if (port.ValueType.empty()) {
				WriteReason(reason, "port '" + port.Id + "' has no value type");
				return NodeSchemaStatus::EmptyValueType;
			}
			const std::string key = std::to_string(static_cast<uint8_t>(port.Direction)) + ":" + port.Id;
			if (!ids.insert(key).second) {
				WriteReason(reason, "port '" + port.Id + "' is duplicated");
				return NodeSchemaStatus::DuplicatePortId;
			}
			if (port.Direction == NodePortDirection::Input &&
				!std::holds_alternative<std::monostate>(port.DefaultValue) &&
				!ValueMatches(port.ValueType, port.DefaultValue)) {
				WriteReason(reason, "input '" + port.Id + "' has a mismatched default value");
				return NodeSchemaStatus::EmptyValueType;
			}
		}

		std::unordered_set<std::string> bypassOutputs;
		for (const NodeBypassMapping &mapping : schema.BypassMappings) {
			const NodePortSchema *input = FindSchemaPort(schema, mapping.Input, NodePortDirection::Input);
			if (input == nullptr) {
				WriteReason(reason, "bypass input '" + mapping.Input + "' is not declared");
				return NodeSchemaStatus::UnknownBypassInput;
			}
			const NodePortSchema *output = FindSchemaPort(schema, mapping.Output, NodePortDirection::Output);
			if (output == nullptr) {
				WriteReason(reason, "bypass output '" + mapping.Output + "' is not declared");
				return NodeSchemaStatus::UnknownBypassOutput;
			}
			if (!ArePortTypesCompatible(input->ValueType, output->ValueType)) {
				WriteReason(reason, "bypass mapping has incompatible port types");
				return NodeSchemaStatus::InvalidBypassType;
			}
			if (!bypassOutputs.insert(mapping.Output).second) {
				WriteReason(reason, "bypass output '" + mapping.Output + "' is mapped twice");
				return NodeSchemaStatus::DuplicateBypassOutput;
			}
		}
		return NodeSchemaStatus::Ok;
	}

	NodeEvaluationStatus EvaluateNode(
		const NodeSchema &schema, const NodeValues &inputs, NodeValues &outputs, std::string *reason
	) {
		if (ValidateNodeSchema(schema, reason) != NodeSchemaStatus::Ok) {
			return NodeEvaluationStatus::EvaluatorFailed;
		}

		NodeValues resolved = inputs;
		for (const auto &input : resolved) {
			if (FindSchemaPort(schema, input.first, NodePortDirection::Input) == nullptr) {
				WriteReason(reason, "input '" + input.first + "' is not declared");
				return NodeEvaluationStatus::UnknownInput;
			}
		}
		for (const NodePortSchema &port : schema.Ports) {
			if (port.Direction != NodePortDirection::Input) {
				continue;
			}
			const NodeValue *value = FindValue(resolved, port.Id);
			if (value == nullptr) {
				if (!std::holds_alternative<std::monostate>(port.DefaultValue)) {
					resolved[port.Id] = port.DefaultValue;
					continue;
				}
				if (port.Optional) {
					continue;
				}
				WriteReason(reason, "missing input '" + port.Id + "'");
				return NodeEvaluationStatus::MissingInput;
			}
			if (!ValueMatches(port.ValueType, *value)) {
				WriteReason(reason, "input '" + port.Id + "' does not match type '" + port.ValueType + "'");
				return NodeEvaluationStatus::InputTypeMismatch;
			}
		}

		outputs.clear();
		std::string evaluatorReason;
		if (!schema.Evaluate || !schema.Evaluate(schema, resolved, outputs, evaluatorReason)) {
			WriteReason(reason, evaluatorReason.empty() ? "node evaluator failed" : evaluatorReason);
			return NodeEvaluationStatus::EvaluatorFailed;
		}
		for (const auto &output : outputs) {
			if (FindSchemaPort(schema, output.first, NodePortDirection::Output) == nullptr) {
				WriteReason(reason, "output '" + output.first + "' is not declared");
				return NodeEvaluationStatus::UnknownOutput;
			}
		}

		for (const NodePortSchema &port : schema.Ports) {
			if (port.Direction != NodePortDirection::Output) {
				continue;
			}
			const NodeValue *value = FindValue(outputs, port.Id);
			if (value == nullptr) {
				if (port.Optional) {
					continue;
				}
				WriteReason(reason, "missing output '" + port.Id + "'");
				return NodeEvaluationStatus::OutputTypeMismatch;
			}
			if (!ValueMatches(port.ValueType, *value)) {
				WriteReason(reason, "output '" + port.Id + "' does not match type '" + port.ValueType + "'");
				return NodeEvaluationStatus::OutputTypeMismatch;
			}
		}
		return NodeEvaluationStatus::Ok;
	}

	NodeSchemaStatus NodeSchemaRegistry::Add(NodeSchema schema, std::string *reason) {
		const NodeSchemaStatus status = ValidateNodeSchema(schema, reason);
		if (status != NodeSchemaStatus::Ok) {
			return status;
		}
		if (Indices.contains(schema.Id)) {
			WriteReason(reason, "schema '" + schema.Id + "' is duplicated");
			return NodeSchemaStatus::DuplicateSchemaId;
		}
		Indices.emplace(schema.Id, Schemas.size());
		Schemas.push_back(std::move(schema));
		return NodeSchemaStatus::Ok;
	}

	const NodeSchema *NodeSchemaRegistry::Find(std::string_view id) const {
		const auto found = Indices.find(std::string(id));
		return found == Indices.end() ? nullptr : &Schemas[found->second];
	}

	const NodeSchemaRegistry &BuiltinNodeSchemas() {
		static const NodeSchemaRegistry registry = [] {
			NodeSchemaRegistry result;
			const auto add = [&](NodeSchema schema) {
				std::string reason;
				const NodeSchemaStatus status = result.Add(std::move(schema), &reason);
				if (status != NodeSchemaStatus::Ok) {
					std::terminate();
				}
			};

			add(Schema(
				"logic.And",
				"And",
				{InputPort("left", "boolean"), InputPort("right", "boolean"), OutputPort("value", "boolean")},
				{{"left", "value"}},
				LogicalAnd
			));
			add(Schema(
				"logic.Or",
				"Or",
				{InputPort("left", "boolean"), InputPort("right", "boolean"), OutputPort("value", "boolean")},
				{{"left", "value"}},
				LogicalOr
			));
			add(Schema(
				"logic.Not",
				"Not",
				{InputPort("value", "boolean"), OutputPort("result", "boolean")},
				{{"value", "result"}},
				LogicalNot
			));
			NodePortSchema trueValue = InputPort("true", "any");
			trueValue.Optional = true;
			NodePortSchema falseValue = InputPort("false", "any");
			falseValue.Optional = true;
			add(Schema(
				"logic.Switch",
				"Switch",
				{InputPort("condition", "boolean"),
				 std::move(trueValue),
				 std::move(falseValue),
				 OutputPort("value", "any")},
				{{"false", "value"}},
				Switch
			));
			add(Schema(
				"math.Add",
				"Add",
				{InputPort("left", "number"), InputPort("right", "number"), OutputPort("value", "number")},
				{{"left", "value"}},
				Add
			));
			add(Schema(
				"math.Multiply",
				"Multiply",
				{InputPort("left", "number"), InputPort("right", "number"), OutputPort("value", "number")},
				{{"left", "value"}},
				Multiply
			));
			add(Schema(
				"math.Clamp",
				"Clamp",
				{InputPort("value", "number"),
				 InputPort("min", "number", 0.0),
				 InputPort("max", "number", 1.0),
				 OutputPort("value", "number")},
				{{"value", "value"}},
				Clamp
			));
			add(Schema(
				"math.Remap",
				"Remap",
				{InputPort("value", "number"),
				 InputPort("sourceMin", "number", 0.0),
				 InputPort("sourceMax", "number", 1.0),
				 InputPort("targetMin", "number", 0.0),
				 InputPort("targetMax", "number", 1.0),
				 OutputPort("value", "number")},
				{{"value", "value"}},
				Remap
			));
			add(Schema(
				"compare.Equal",
				"Equal",
				{InputPort("left", "any"), InputPort("right", "any"), OutputPort("value", "boolean")},
				{},
				Equal
			));
			add(Schema(
				"compare.GreaterThan",
				"Greater Than",
				{InputPort("left", "number"), InputPort("right", "number"), OutputPort("value", "boolean")},
				{},
				GreaterThan
			));
			add(Schema(
				"flow.Branch",
				"Branch",
				{InputPort("condition", "boolean"),
				 InputPort("value", "any"),
				 OutputPort("true", "any", true),
				 OutputPort("false", "any", true)},
				{},
				Branch
			));
			NodePortSchema first = InputPort("first", "any");
			first.Optional = true;
			NodePortSchema second = InputPort("second", "any");
			second.Optional = true;
			add(Schema(
				"flow.Merge",
				"Merge",
				{std::move(first), std::move(second), OutputPort("value", "any")},
				{{"first", "value"}},
				Merge
			));
			add(Schema(
				"flow.Reroute",
				"Reroute",
				{InputPort("value", "any"), OutputPort("value", "any")},
				{{"value", "value"}},
				Reroute
			));
			add(Schema(
				"value.Number",
				"Number",
				{InputPort("value", "number", 0.0), OutputPort("value", "number")},
				{{"value", "value"}},
				ConstantNumber
			));
			add(Schema(
				"value.Boolean",
				"Boolean",
				{InputPort("value", "boolean", false), OutputPort("value", "boolean")},
				{{"value", "value"}},
				ConstantBoolean
			));
			add(Schema(
				"value.String",
				"String",
				{InputPort("value", "string", std::string{}), OutputPort("value", "string")},
				{{"value", "value"}},
				ConstantString
			));
			add(Schema(
				"convert.ToString",
				"To String",
				{InputPort("value", "any"), OutputPort("value", "string")},
				{},
				ToString
			));
			return result;
		}();
		return registry;
	}
}
