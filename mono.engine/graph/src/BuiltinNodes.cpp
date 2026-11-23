#include <engine/ang/Runner.hpp>
#include <engine/graph/BuiltinNodes.hpp>

#include <algorithm>
#include <sstream>
#include <string>
#include <utility>

namespace engine::graph {

	namespace {
		using ang::EvalContext;
		using ang::NodeResult;
		using ang::PortDef;
		using ang::Value;

		PortDef In(std::string name, std::string type, Value fallback = {}) {
			return {.Name = std::move(name), .Type = std::move(type), .Default = std::move(fallback)};
		}

		PortDef Optional(std::string name, std::string type) {
			return {.Name = std::move(name), .Type = std::move(type), .Optional = true};
		}

		PortDef Out(std::string name, std::string type) {
			return {.Name = std::move(name), .Type = std::move(type), .Dir = ang::Direction::Output};
		}

		// Typed reads. A linked `any` output can carry any value, so the kind is
		// checked here rather than trusted from the port type.
		bool Boolean(const EvalContext &context, std::string_view port, bool &out, NodeResult &failure) {
			const bool *value = context.Input(port).Get<bool>();
			if (value == nullptr) {
				failure = NodeResult::Failure("input '" + std::string(port) + "' must be boolean");
				return false;
			}
			out = *value;
			return true;
		}

		bool Number(const EvalContext &context, std::string_view port, double &out, NodeResult &failure) {
			const double *value = context.Input(port).Get<double>();
			if (value == nullptr) {
				failure = NodeResult::Failure("input '" + std::string(port) + "' must be number");
				return false;
			}
			out = *value;
			return true;
		}

		NodeResult
		Present(EvalContext &context, std::string_view port, std::string_view output, const char *why) {
			const Value &value = context.Input(port);
			if (value.Empty()) {
				return NodeResult::Failure(why);
			}
			context.SetOutput(output, value);
			return NodeResult::Success();
		}

		template <typename Combine> ang::EvaluateFn Logical(Combine combine) {
			return [combine](EvalContext &context) {
				NodeResult failure;
				bool left = false;
				bool right = false;
				if (!Boolean(context, "left", left, failure) || !Boolean(context, "right", right, failure)) {
					return failure;
				}
				context.SetOutput("value", Value(combine(left, right)));
				return NodeResult::Success();
			};
		}

		template <typename Combine> ang::EvaluateFn Arithmetic(Combine combine) {
			return [combine](EvalContext &context) {
				NodeResult failure;
				double left = 0.0;
				double right = 0.0;
				if (!Number(context, "left", left, failure) || !Number(context, "right", right, failure)) {
					return failure;
				}
				context.SetOutput("value", Value(combine(left, right)));
				return NodeResult::Success();
			};
		}

		ang::NodeDef
		Def(std::string id,
			std::string label,
			std::vector<PortDef> inputs,
			std::vector<PortDef> outputs,
			std::vector<ang::BypassRoute> bypass,
			ang::EvaluateFn evaluate) {
			const std::string category = id.substr(0, id.find('.'));
			return {
				.Id = std::move(id),
				.Category = category,
				.Label = std::move(label),
				.Inputs = std::move(inputs),
				.Outputs = std::move(outputs),
				.Evaluate = std::move(evaluate),
				.Bypass = std::move(bypass),
			};
		}

		std::vector<ang::NodeDef> Definitions() {
			std::vector<ang::NodeDef> nodes;
			nodes.push_back(
				Def("logic.And",
					"And",
					{In("left", "bool"), In("right", "bool")},
					{Out("value", "bool")},
					{{"left", "value"}},
					Logical([](bool left, bool right) { return left && right; }))
			);
			nodes.push_back(
				Def("logic.Or",
					"Or",
					{In("left", "bool"), In("right", "bool")},
					{Out("value", "bool")},
					{{"left", "value"}},
					Logical([](bool left, bool right) { return left || right; }))
			);
			nodes.push_back(
				Def("logic.Not",
					"Not",
					{In("value", "bool")},
					{Out("result", "bool")},
					{{"value", "result"}},
					[](EvalContext &context) {
						NodeResult failure;
						bool value = false;
						if (!Boolean(context, "value", value, failure)) {
							return failure;
						}
						context.SetOutput("result", Value(!value));
						return NodeResult::Success();
					})
			);
			nodes.push_back(Def(
				"logic.Switch",
				"Switch",
				{In("condition", "bool"), Optional("true", "any"), Optional("false", "any")},
				{Out("value", "any")},
				{{"false", "value"}},
				[](EvalContext &context) {
					NodeResult failure;
					bool condition = false;
					if (!Boolean(context, "condition", condition, failure)) {
						return failure;
					}
					return Present(
						context, condition ? "true" : "false", "value", "switch requires the selected value"
					);
				}
			));
			nodes.push_back(
				Def("math.Add",
					"Add",
					{In("left", "number"), In("right", "number")},
					{Out("value", "number")},
					{{"left", "value"}},
					Arithmetic([](double left, double right) { return left + right; }))
			);
			nodes.push_back(
				Def("math.Multiply",
					"Multiply",
					{In("left", "number"), In("right", "number")},
					{Out("value", "number")},
					{{"left", "value"}},
					Arithmetic([](double left, double right) { return left * right; }))
			);
			nodes.push_back(
				Def("math.Clamp",
					"Clamp",
					{In("value", "number"), In("min", "number", Value(0.0)), In("max", "number", Value(1.0))},
					{Out("value", "number")},
					{{"value", "value"}},
					[](EvalContext &context) {
						NodeResult failure;
						double value = 0.0;
						double minimum = 0.0;
						double maximum = 0.0;
						if (!Number(context, "value", value, failure) ||
							!Number(context, "min", minimum, failure) ||
							!Number(context, "max", maximum, failure)) {
							return failure;
						}
						if (minimum > maximum) {
							return NodeResult::Failure("clamp minimum exceeds maximum");
						}
						context.SetOutput("value", Value(std::clamp(value, minimum, maximum)));
						return NodeResult::Success();
					})
			);
			nodes.push_back(
				Def("math.Remap",
					"Remap",
					{In("value", "number"),
					 In("sourceMin", "number", Value(0.0)),
					 In("sourceMax", "number", Value(1.0)),
					 In("targetMin", "number", Value(0.0)),
					 In("targetMax", "number", Value(1.0))},
					{Out("value", "number")},
					{{"value", "value"}},
					[](EvalContext &context) {
						NodeResult failure;
						double value = 0.0;
						double sourceMinimum = 0.0;
						double sourceMaximum = 0.0;
						double targetMinimum = 0.0;
						double targetMaximum = 0.0;
						if (!Number(context, "value", value, failure) ||
							!Number(context, "sourceMin", sourceMinimum, failure) ||
							!Number(context, "sourceMax", sourceMaximum, failure) ||
							!Number(context, "targetMin", targetMinimum, failure) ||
							!Number(context, "targetMax", targetMaximum, failure)) {
							return failure;
						}
						if (sourceMinimum == sourceMaximum) {
							return NodeResult::Failure("remap source range is empty");
						}
						const double progress = (value - sourceMinimum) / (sourceMaximum - sourceMinimum);
						context.SetOutput(
							"value", Value(targetMinimum + progress * (targetMaximum - targetMinimum))
						);
						return NodeResult::Success();
					})
			);
			nodes.push_back(
				Def("compare.Equal",
					"Equal",
					{In("left", "any"), In("right", "any")},
					{Out("value", "bool")},
					{},
					[](EvalContext &context) {
						const Value &left = context.Input("left");
						const Value &right = context.Input("right");
						if (left.Empty() || right.Empty()) {
							return NodeResult::Failure("equal requires two values");
						}
						context.SetOutput("value", Value(left == right));
						return NodeResult::Success();
					})
			);
			nodes.push_back(
				Def("compare.GreaterThan",
					"Greater Than",
					{In("left", "number"), In("right", "number")},
					{Out("value", "bool")},
					{},
					Arithmetic([](double left, double right) { return left > right; }))
			);
			nodes.push_back(Def(
				"flow.Branch",
				"Branch",
				{In("condition", "bool"), In("value", "any")},
				{Out("true", "any"), Out("false", "any")},
				{},
				[](EvalContext &context) {
					NodeResult failure;
					bool condition = false;
					if (!Boolean(context, "condition", condition, failure)) {
						return failure;
					}
					return Present(context, "value", condition ? "true" : "false", "branch requires a value");
				}
			));
			nodes.push_back(
				Def("flow.Merge",
					"Merge",
					{Optional("first", "any"), Optional("second", "any")},
					{Out("value", "any")},
					{{"first", "value"}},
					[](EvalContext &context) {
						const bool first = !context.Input("first").Empty();
						return Present(
							context, first ? "first" : "second", "value", "merge requires at least one value"
						);
					})
			);
			nodes.push_back(
				Def("flow.Reroute",
					"Reroute",
					{In("value", "any")},
					{Out("value", "any")},
					{{"value", "value"}},
					[](EvalContext &context) {
						return Present(context, "value", "value", "reroute requires a value");
					})
			);
			nodes.push_back(
				Def("value.Number",
					"Number",
					{In("value", "number", Value(0.0))},
					{Out("value", "number")},
					{{"value", "value"}},
					[](EvalContext &context) {
						NodeResult failure;
						double value = 0.0;
						if (!Number(context, "value", value, failure)) {
							return failure;
						}
						context.SetOutput("value", Value(value));
						return NodeResult::Success();
					})
			);
			nodes.push_back(
				Def("value.Boolean",
					"Boolean",
					{In("value", "bool", Value(false))},
					{Out("value", "bool")},
					{{"value", "value"}},
					[](EvalContext &context) {
						NodeResult failure;
						bool value = false;
						if (!Boolean(context, "value", value, failure)) {
							return failure;
						}
						context.SetOutput("value", Value(value));
						return NodeResult::Success();
					})
			);
			nodes.push_back(
				Def("value.String",
					"String",
					{In("value", "string", Value(std::string{}))},
					{Out("value", "string")},
					{{"value", "value"}},
					[](EvalContext &context) {
						const std::string *value = context.Input("value").Get<std::string>();
						if (value == nullptr) {
							return NodeResult::Failure("input 'value' must be string");
						}
						context.SetOutput("value", Value(*value));
						return NodeResult::Success();
					})
			);
			nodes.push_back(Def(
				"convert.ToString",
				"To String",
				{In("value", "any")},
				{Out("value", "string")},
				{},
				[](EvalContext &context) {
					const Value &value = context.Input("value");
					if (const double *number = value.Get<double>(); number != nullptr) {
						std::ostringstream stream;
						stream << *number;
						context.SetOutput("value", Value(stream.str()));
					} else if (const bool *boolean = value.Get<bool>(); boolean != nullptr) {
						context.SetOutput("value", Value(*boolean ? "true" : "false"));
					} else if (const std::string *text = value.Get<std::string>(); text != nullptr) {
						context.SetOutput("value", Value(*text));
					} else {
						return NodeResult::Failure("to string supports number, boolean, and string values");
					}
					return NodeResult::Success();
				}
			));
			return nodes;
		}
	}

	ang::RegistryResult RegisterBuiltinNodes(ang::Registry &registry) {
		for (ang::NodeDef &node : Definitions()) {
			ang::RegistryResult result = registry.DefineNode(std::move(node));
			if (!result.Ok) {
				return result;
			}
		}
		return {};
	}
}
