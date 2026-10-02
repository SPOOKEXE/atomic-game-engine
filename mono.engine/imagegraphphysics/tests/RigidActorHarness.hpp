#pragma once
#include "../../imagegraph/src/NodeExecutors.hpp"
#include "../../imagegraph/src/SourceRigidFracture.hpp"

#include <engine/imagegraph/SourceBuiltinRandom.hpp>
#include <engine/imagegraphphysics/RigidReplay.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <stdexcept>
namespace engine::imagegraph::detail {
	std::span<const ExecutorEntry> SourceRigidExecutors();
}
using namespace engine::imagegraph;
using namespace engine::imagegraph::detail;
struct Harness {
	EvaluationBudget Budget{Limits::MaximumEvaluationBytes};
	AllocationReservation Charge;
	RigidReplayState State;
	engine::imagegraphphysics::RigidProvider Provider;
	EvaluationRequest Request;
	Document DocumentData;
	std::vector<SourceBuiltinRandomDraw> NextDraws;
	std::vector<std::pair<std::string, Image>> Images;
	std::vector<AuthoredValue> Owner{
		{"dimension", Vector2{32, 32}},
		{"dimension_unit", EnumValue{0}},
		{"simulation_scale", 16.},
		{"strength", 0.},
		{"use_wall", false}
	};
	std::vector<std::pair<std::string_view, const Value *>> OwnerValues;
	Harness() {
		Request.RigidProvider = &Provider;
		Request.RigidPlaying = true;
		Request.RigidFrameProgress = true;
		for (auto &v : Owner)
			OwnerValues.emplace_back(v.Port, &v.Data);
		Charge = std::move(*Budget.Reserve(0));
	}
	std::vector<AuthoredValue>
	Run(std::string type,
		std::string id,
		std::initializer_list<AuthoredValue> inputs = {},
		const Image *image = nullptr) {
		Node node{id, type, "owned", {}, {}};
		auto *entry = FindCatalogueEntry(type);
		if (!entry) throw std::runtime_error("unknown catalogue " + type);
		NodeContext context(node, *entry, Request, Budget);
		context.ByteBudget = Limits::MaximumEvaluationBytes;
		context.EvaluationDocument = &DocumentData;
		context.EffectiveInputLinks = DocumentData.Links;
		context.CurrentRigid = &State;
		context.CurrentRigidCharge = &Charge;
		context.InlineOwnerType = "pc.rigid_group_inline";
		context.InlineOwnerId = "owner";
		context.InlineOwnerValues = OwnerValues;
		for (const auto &input : entry->Inputs) {
			auto found =
				std::find_if(inputs.begin(), inputs.end(), [&](const auto &v) { return v.Port == input.Id; });
			if (found != inputs.end())
				context.Values.emplace_back(input.Id, found->Data);
			else if (auto value = CatalogueDefault(input))
				context.Values.emplace_back(input.Id, *value);
		}
		for (const auto &input : inputs)
			if (std::none_of(entry->Inputs.begin(), entry->Inputs.end(), [&](const auto &i) {
					return i.Id == input.Port;
				}))
				context.Values.emplace_back(input.Port, input.Data);
		if (image) context.Images.emplace_back("texture", image);
		SourceBuiltinRandomCapture capture;

		if (!NextDraws.empty()) {
			capture.Authored = node;
			capture.Tick = Request.Tick;
			capture.Draws = NextDraws;
			for (const auto &[port, value] : context.Values)
				capture.Inputs.push_back({std::string(port), value});
			for (const auto &[port, value] : context.Images)
				capture.InputImages.push_back({std::string(port), *value});
			Request.BuiltinRandomCaptures = std::span{&capture, 1};
		}
		for (const auto &candidate : SourceRigidExecutors())
			if (candidate.Type == type) {
				if (!candidate.Run(context)) throw std::runtime_error(type + ": " + context.FailureMessage);
				Images = std::move(context.OutputImages);
				Request.BuiltinRandomCaptures = {};
				return std::move(context.OutputValues);
			}
		throw std::runtime_error("unknown draftexecutor " + type);
	}
	SourceRigidSnapshot Snapshot() {
		SourceRigidSnapshot s;
		Diagnostic d;
		if (Provider.Replay(
				State.Owners.at(0).History, Request.Tick, std::nullopt, Limits::MaximumEvaluationBytes, s, d
			) != Status::Ok)
			throw std::runtime_error(d.Message);
		return s;
	}
};
