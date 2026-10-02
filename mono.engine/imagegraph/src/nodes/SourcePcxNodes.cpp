#include "../ValuePayload.hpp"
#include "Families.hpp"

#include <engine/imagegraph/HostCapture.hpp>
#include <engine/imagegraph/PcxExpression.hpp>

namespace engine::imagegraph::detail {
	namespace {
		std::string_view Text(const NodeContext &context, std::string_view id) {
			const auto *value = context.Find(id);
			const auto *text = value ? std::get_if<std::string>(value) : nullptr;
			return text ? std::string_view(*text) : std::string_view{};
		}
		bool Publish(NodeContext &context, PcxExpressionValue &&tree, std::string_view port = "pcx") {
			if (!ValidPcxPayload(tree))
				return context.Fail(Status::LimitExceeded, "PCX tree exceeds its bounded payload", port);
			if (!context.ReserveOutput(RetainedPayloadBytes(tree), port)) return false;
			context.SetValue(port, std::move(tree));
			return context.FailureCode == Status::Ok;
		}
		bool Arguments(NodeContext &context, PcxExpressionValue &tree) {
			for (const auto &input : context.Authored.DynamicInputs) {
				size_t group = 0;
				const auto *part = FindDynamicTemplate(context.Entry, input.Id, group);
				if (!part || part->Id != "argument_name") continue;
				const auto name = Text(context, input.Id);
				if (name.empty()) continue;
				const Value *value = nullptr;
				const Image *image = nullptr;
				for (const auto &candidate : context.Authored.DynamicInputs) {
					size_t index = 0;
					const auto *field = FindDynamicTemplate(context.Entry, candidate.Id, index);
					if (field && index == group && field->Id == "argument_value") {
						value = context.Find(candidate.Id);
						image = context.Input(candidate.Id);
					}
				}
				Value owned = image ? Value{SurfaceValue{*image}} : value ? *value : Value{double{0}};
				if (!ValidRuntimeValue(owned))
					return context.Fail(Status::InvalidValue, "PCX argument payload is invalid", input.Id);
				auto &bindings = tree.Data->Bindings;
				const auto found = std::find_if(bindings.begin(), bindings.end(), [&](const auto &pair) {
					return pair.first == name;
				});
				if (found == bindings.end())
					bindings.emplace_back(std::string(name), std::move(owned));
				else
					found->second = std::move(owned);
			}
			return true;
		}
		bool GlobalScope(NodeContext &) {
			return true;
		}
		bool GlobalVariable(NodeContext &context) {
			const auto name = Text(context, "globalvar");
			Value value = double{0};
			Diagnostic diagnostic;
			if (context.PcxNames && !context.PcxNames->Resolve(name, value, diagnostic) &&
				diagnostic.Code != Status::Ok)
				return context.Fail(diagnostic.Code, diagnostic.Message, "globalvar");
			if (!context.ReserveOutput(RetainedPayloadBytes(value), "value")) return false;
			context.SetValue("value", std::move(value));
			return context.FailureCode == Status::Ok;
		}
		bool Equation(NodeContext &context) {
			const auto source = Text(context, "equation");
			uint64_t bytes =
				source.size() * 8 + 4096 * sizeof(PcxInstruction) + 4 * Limits::MaximumArrayBytes;
			for (const auto &[port, value] : context.Values)
				bytes += port.size() + RetainedPayloadBytes(value);
			for (const auto &[port, value] : context.ValueViews)
				if (value) bytes += port.size() + RetainedPayloadBytes(*value);
			for (const auto &[port, image] : context.Images)
				if (image) bytes += port.size() + image->Pixels.size();
			auto scratch = context.ReserveWorkspace(bytes, "equation");
			if (!scratch) return false;
			PcxExpressionValue tree;
			Diagnostic diagnostic;
			if (CompilePcxExpression(source, tree, diagnostic) != Status::Ok)
				return context.Fail(diagnostic.Code, diagnostic.Message, "equation");
			if (!Arguments(context, tree)) return false;
			if (context.Authored.Type == "pc.pcx_equation")
				return Publish(context, std::move(tree), "result");
			PcxExecutionContext execution{
				context.Request,
				{},
				context.Timeline,
				{double(context.Project.SurfaceWidth), double(context.Project.SurfaceHeight)},
				context.Request.ProjectName
			};
			execution.Names = context.PcxNames;
			execution.MaximumBytes = std::min(context.AvailableBytes(), Limits::MaximumArrayBytes);
			PcxExecutionResult result;
			if (ExecutePcxExpression(tree, execution, result, diagnostic) != Status::Ok)
				return context.Fail(diagnostic.Code, diagnostic.Message, "equation");
			if (!result.Messages.empty()) {
				uint64_t bytes = 2 * result.Messages.size() * sizeof(PcxMessage);
				for (const auto &message : result.Messages)
					bytes += message.Text.size();
				auto charge = context.ReserveWorkspace(bytes, "equation");
				if (!charge || !context.PcxControlCharge.Merge(std::move(*charge))) return false;
				for (auto &message : result.Messages)
					context.PcxControlMessages.push_back(std::move(message));
			}
			if (!context.ReserveOutput(RetainedPayloadBytes(result.Data), "result")) return false;
			context.SetValue("result", std::move(result.Data));
			return context.FailureCode == Status::Ok;
		}
		struct Builder {
			NodeContext &Context;
			PcxExpressionValue Tree;
			Builder(NodeContext &context) : Context(context) {
				Tree.Data.emplace();
			}
			uint32_t
			Add(std::string operation, std::vector<uint32_t> children = {}, Value literal = double{0}) {
				auto &nodes = Tree.Data->Instructions;
				if (nodes.size() >= 4096) {
					Context.Fail(Status::LimitExceeded, "PCX tree instruction budget exceeded");
					return 0;
				}
				nodes.push_back({std::move(operation), std::move(literal), std::move(children)});
				return uint32_t(nodes.size() - 1);
			}
			uint32_t Input(std::string_view port) {
				const auto *value = Context.Find(port);
				if (const auto *source = value ? std::get_if<PcxExpressionValue>(value) : nullptr) {
					if (!ValidPcxPayload(*source)) {
						Context.Fail(Status::InvalidValue, "PCX child tree is invalid", port);
						return 0;
					}
					const auto base = Tree.Data->Instructions.size();
					if (source->Data->Instructions.size() > 4096 - base) {
						Context.Fail(
							Status::LimitExceeded, "PCX child tree exceeds instruction budget", port
						);
						return 0;
					}
					for (const auto &node : source->Data->Instructions) {
						auto copy = node;
						for (auto &child : copy.Arguments)
							child += uint32_t(base);
						Tree.Data->Instructions.push_back(std::move(copy));
					}
					for (const auto &binding : source->Data->Bindings)
						Tree.Data->Bindings.push_back(binding);
					return uint32_t(base) + source->Data->Root;
				}
				if (const auto *image = Context.Input(port)) return Add("literal", {}, SurfaceValue{*image});
				return Add("literal", {}, value ? *value : Value{double{0}});
			}
		};
		bool Tree(NodeContext &context) {
			uint64_t bytes = 4096 * sizeof(PcxInstruction) + 4 * Limits::MaximumArrayBytes;
			for (const auto &[port, value] : context.Values)
				bytes += RetainedPayloadBytes(value);
			for (const auto &[port, value] : context.ValueViews)
				if (value) bytes += RetainedPayloadBytes(*value);
			for (const auto &[port, image] : context.Images)
				if (image) bytes += port.size() + image->Pixels.size();
			auto scratch = context.ReserveWorkspace(bytes, "pcx");
			if (!scratch) return false;
			Builder builder(context);
			const auto type = context.Authored.Type;
			uint32_t root = 0;
			if (type == "pc.pcx_var" || type == "pc.pcx_fn_var") {
				const auto name = type == "pc.pcx_var" ? Text(context, "name")
													   : std::string_view(context.Authored.SourceDisplayName);
				if (name.empty() && type == "pc.pcx_fn_var")
					return context.Fail(
						Status::InvalidValue, "PCX Fn Variable requires its serialized source display name"
					);
				const auto left = builder.Add("name", {}, std::string(name)),
						   right = builder.Input(type == "pc.pcx_var" ? "value" : "default_value");
				root = builder.Add(type == "pc.pcx_var" ? "=" : "default", {left, right});
			} else if (type == "pc.pcx_array_get" || type == "pc.pcx_array_set") {
				const auto array = builder.Input("array"), index = builder.Input("index"),
						   get = builder.Add("index", {array, index});
				root = type == "pc.pcx_array_get" ? get : builder.Add("=", {get, builder.Input("value")});
			} else if (type == "pc.pcx_condition")
				root = builder.Add(
					"if", {builder.Input("condition"), builder.Input("true"), builder.Input("false")}
				);
			else if (type == "pc.pcx_fn_random")
				root = builder.Add(
					context.Boolean("integer") ? "call:irandom" : "call:random",
					{builder.Input("min"), builder.Input("max")}
				);
			else if (type == "pc.pcx_fn_surface_width" || type == "pc.pcx_fn_surface_height")
				root = builder.Add(
					type == "pc.pcx_fn_surface_width" ? "call:surface_get_width" : "call:surface_get_height",
					{builder.Input("surface")}
				);
			else if (type == "pc.pcx_fn_math") {
				static constexpr std::string_view operations[]{
					"+",		   "-",			  "*",		   "/",
					"**",		   "%",			  "call:abs",  "",
					"call:round",  "call:floor",  "call:ceil", "",
					"call:sin",	   "call:cos",	  "call:tan",  "call:arcsin",
					"call:arccos", "call:arctan", "",		   "call:min",
					"call:max",	   "call:clamp",  "",		   "call:lerp"
				};
				const auto index = context.Integer("operator");
				if (index < 0 || index >= 24 || operations[size_t(index)].empty())
					return context.Fail(Status::InvalidValue, "PCX Math operator is invalid", "operator");
				const auto operation = operations[size_t(index)];
				std::vector<uint32_t> args{builder.Input("x"), builder.Input("y")};
				if (operation.starts_with("call:")) args.push_back(builder.Input("z"));
				root = builder.Add(std::string(operation), std::move(args));
			} else
				return context.Fail(Status::UnsupportedExecution, "PCX builder type is unavailable");
			builder.Tree.Data->Root = root;
			if (context.FailureCode != Status::Ok) return false;
			return Publish(context, std::move(builder.Tree));
		}
	}
	std::span<const ExecutorEntry> SourcePcxExecutors() {
		static constexpr ExecutorEntry entries[]{
			{"pc.global_scope", GlobalScope, false},
			{"pc.globalvar", GlobalVariable, true},
			{"pc.equation", Equation, true},
			{"pc.pcx_equation", Equation, true},
			{"pc.pcx_array_get", Tree, true},
			{"pc.pcx_array_set", Tree, true},
			{"pc.pcx_condition", Tree, true},
			{"pc.pcx_fn_math", Tree, true},
			{"pc.pcx_fn_random", Tree, true},
			{"pc.pcx_fn_surface_width", Tree, true},
			{"pc.pcx_fn_surface_height", Tree, true},
			{"pc.pcx_fn_var", Tree, true},
			{"pc.pcx_var", Tree, true}
		};
		return entries;
	}
}
