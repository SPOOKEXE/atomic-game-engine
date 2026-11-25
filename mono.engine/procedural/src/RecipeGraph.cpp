#include "Encoding.hpp"
#include "RegionInternal.hpp"

#include <engine/ang/Runner.hpp>

#include <algorithm>
#include <new>

namespace engine::procedural::detail {

	namespace {
		constexpr const char *RUN_TYPE = "procedural.run";

		RegionRun *RunOf(ang::EvalContext &context) {
			const ang::Blob *blob = context.Input("run").Get<ang::Blob>();
			return blob == nullptr ? nullptr : static_cast<RegionRun *>(const_cast<void *>(blob->Data.get()));
		}

		// One definition per authored operation kind, with the ports of its
		// graphcore schema and an unlinked "run" input the request supplies.
		ang::Registry RecipeRegistry() {
			ang::Registry types;
			types.DefineType({.Id = RUN_TYPE});
			for (const auto &schema : ForestVillageNodeSchemas()) {
				for (const auto &port : schema.Ports)
					if (types.FindType(port.ValueType) == nullptr) types.DefineType({.Id = port.ValueType});
				const auto stage = static_cast<Stage>(KindIndex(schema.Id));
				ang::NodeDef def{.Id = schema.Id};
				def.Inputs.push_back({.Name = "run", .Type = RUN_TYPE});
				for (const auto &port : schema.Ports) {
					if (port.Direction == graphcore::PortDirection::Input)
						def.Inputs.push_back({.Name = port.Id, .Type = port.ValueType});
					else
						def.Outputs.push_back(
							{.Name = port.Id, .Type = port.ValueType, .Dir = ang::Direction::Output}
						);
				}
				// Stages write the shared run; outputs only carry the order downstream.
				def.Pure = ang::Purity::Impure;
				def.Evaluate = [stage, outputs = def.Outputs](ang::EvalContext &context) {
					RegionRun *run = RunOf(context);
					if (run == nullptr) return ang::NodeResult::Failure("run is missing");
					try {
						if (RunStage(stage, *run) != Status::Ok)
							return ang::NodeResult::Failure(run->Failure.Field);
					} catch (const std::bad_alloc &) {
						Refuse(run->Failure, Status::AllocationFailed);
						run->Code = Status::AllocationFailed;
						return ang::NodeResult::Failure({});
					}
					for (const auto &port : outputs)
						context.SetOutput(port.Name, context.Input("run"));
					return ang::NodeResult::Success();
				};
				types.DefineNode(std::move(def));
			}
			return types;
		}
	}

	Status
	CompileRecipeGraph(const Recipe &recipe, std::shared_ptr<const ang::Plan> &out, Diagnostic &diagnostic) {
		static const ang::Registry types = RecipeRegistry();
		std::vector<const OperationNode *> ordered;
		for (const OperationNode &node : recipe.Nodes)
			ordered.push_back(&node);
		std::sort(ordered.begin(), ordered.end(), [](const OperationNode *left, const OperationNode *right) {
			return StableNameLess(left->Id, right->Id);
		});
		ang::Graph graph;
		for (const OperationNode *node : ordered)
			if (!graph.Apply(ang::edit::AddNode{{.Id = node->Id, .Def = node->Kind}}).Ok)
				return Refuse(diagnostic, Status::InvalidRecipe, node->Id);
		for (const OperationLink &link : recipe.Links)
			if (!graph.Apply(ang::edit::AddLink{{link.FromNode, link.FromPort, link.ToNode, link.ToPort}}).Ok)
				return Refuse(diagnostic, Status::InvalidRecipe, "recipe.graph");
		auto plan = std::make_shared<ang::Plan>();
		const ang::CompileResult result = ang::Compile(
			graph,
			types,
			{.Runs = ang::Domain::Tick,
			 .MaximumNodes = KindCount,
			 .MaximumLinks = 13,
			 .MaximumPorts = 19 + KindCount,
			 .MaximumIdentityBytes = 8192},
			*plan
		);
		if (!result.Ok) {
			const ang::Diagnostic &first = result.Diagnostics.front();
			return Refuse(
				diagnostic,
				first.GraphStatus == graphcore::CompileStatus::AllocationFailed ? Status::AllocationFailed
																				: Status::InvalidRecipe,
				first.Node.empty() ? "recipe.graph" : first.Node
			);
		}
		out = std::move(plan);
		return Status::Ok;
	}

	Status RunRecipeGraph(const ang::Plan &plan, RegionRun &run) {
		const ang::Value token =
			ang::Value::FromBlob(RUN_TYPE, {.Data = std::shared_ptr<const void>(&run, [](const void *) {})});
		ang::RunInputs inputs;
		for (const ang::PlannedNode &node : plan.Nodes)
			inputs[node.Id]["run"] = token;
		ang::Runner runner(plan);
		const ang::RunReport report = runner.Run(inputs);
		if (run.Code != Status::Ok) return run.Code;
		for (const ang::NodeReport &node : report.Nodes)
			if (node.Status != ang::NodeStatus::Ran) {
				run.Code = Refuse(run.Failure, Status::InvalidRecipe, "recipe.stages");
				return run.Code;
			}
		return Status::Ok;
	}
}
