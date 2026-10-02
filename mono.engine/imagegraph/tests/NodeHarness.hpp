#pragma once

// Runs one catalogue executor on explicit images and values, filling unset inputs from the catalogue
// defaults the way graph evaluation does.

#include "../src/NodeExecutors.hpp"

#include <engine/imagegraph/Catalogue.hpp>

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace imagegraph_test {
	using engine::imagegraph::Image;
	using engine::imagegraph::Value;

	struct NodeRun {
		bool Ok = false;
		engine::imagegraph::Status Code = engine::imagegraph::Status::Ok;
		std::string Message;
		std::string Port;
		std::vector<std::pair<std::string, Image>> Images;
		std::vector<engine::imagegraph::AuthoredValue> Values;

		const Image &Output(std::string_view id = "surface_out") const {
			for (const auto &[port, image] : Images)
				if (port == id) return image;
			static const Image EMPTY;
			return EMPTY;
		}
		const Value *OutputValue(std::string_view id) const {
			for (const auto &value : Values)
				if (value.Port == id) return &value.Data;
			return nullptr;
		}
	};

	inline NodeRun RunNode(
		std::string_view type,
		std::initializer_list<std::pair<std::string_view, const Image *>> images,
		std::initializer_list<std::pair<std::string_view, Value>> values = {},
		uint64_t tick = 0,
		uint64_t seed = 0,
		const engine::imagegraph::TimelineSettings *timeline = nullptr
	) {
		using namespace engine::imagegraph;
		NodeRun run;
		const CatalogueEntry *entry = FindCatalogueEntry(type);
		const detail::Executor executor = detail::FindExecutor(type);
		if (!entry || !executor) {
			run.Message = "unknown type or executor";
			return run;
		}
		Node node{"node", std::string(type), "", {}, {}};
		EvaluationRequest request;
		request.Tick = tick;
		request.Seed = seed;
		detail::NodeContext context(node, *entry, request);
		context.Timeline = timeline;
		context.ByteBudget = Limits::MaximumEvaluationBytes;
		for (const auto &[port, image] : images)
			context.Images.emplace_back(port, image);
		for (const CatalogueInput &input : entry->Inputs) {
			// Runtime-only inputs such as Any take a given value as a linked source would supply it.
			if (!IsAuthoredValueType(input.Type)) {
				for (const auto &[port, value] : values)
					if (port == input.Id) context.Values.emplace_back(input.Id, value);
				continue;
			}
			const auto given = std::find_if(values.begin(), values.end(), [&](const auto &item) {
				return item.first == input.Id;
			});
			if (given != values.end())
				context.Values.emplace_back(input.Id, given->second);
			else if (std::optional<Value> fallback = CatalogueDefault(input)) {
				context.Values.emplace_back(input.Id, *fallback);
				context.CatalogueDefaultInputs.emplace_back(input.Id);
			}
		}
		context.InputProvenanceResolved = true;
		run.Ok = executor(context) && context.FailureCode == Status::Ok;
		run.Code = context.FailureCode;
		run.Message = context.FailureMessage;
		run.Port = context.FailurePort;
		run.Images = std::move(context.OutputImages);
		run.Values = std::move(context.OutputValues);
		return run;
	}

	inline Image MakeImage(uint32_t width, uint32_t height, std::vector<uint8_t> pixels) {
		return Image{width, height, std::move(pixels), 0};
	}
}
