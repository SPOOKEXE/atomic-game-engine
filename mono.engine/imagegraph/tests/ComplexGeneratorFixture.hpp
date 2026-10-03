#pragma once

#include "NodeHarness.hpp"

#include <catch2/catch_test_macros.hpp>

namespace complex_generator_test {
	using namespace engine::imagegraph;
	inline Document Graph(std::string type, Vector2 dimension = {4, 4}) {
		Document d;
		d.FormatVersion = 9;
		d.Nodes = {{"generator", type, "", {}, {{"dimension", dimension}, {"dimension_unit", EnumValue{0}}}}};
		if (type == "pc.julia_set")
			d.Nodes[0].Values.push_back({"max_iteration", int64_t{8}});
		else {
			d.Nodes[0].Values.push_back({"seed", 17.0});
			d.Nodes[0].Values.push_back({"attribute_color_depth", EnumValue{3}});
		}
		d.Outputs = {{"out", "generator", type == "pc.julia_set" ? "surface" : "surface_out"}};
		return d;
	}
	inline void Set(Document &d, std::string port, Value value) {
		for (auto &stored : d.Nodes[0].Values)
			if (stored.Port == port) {
				stored.Data = std::move(value);
				return;
			}
		d.Nodes[0].Values.push_back({std::move(port), std::move(value)});
	}
	inline Image Draw(const Document &d) {
		Plan plan;
		Diagnostic diagnostic;
		const auto compiled = Compile(d, plan, diagnostic);
		INFO(diagnostic.NodeId + ":" + diagnostic.Port + ":" + diagnostic.Message);
		REQUIRE(compiled == Status::Ok);
		Image image;
		const auto evaluated = Evaluate(d, plan, "out", {}, image, diagnostic);
		INFO(diagnostic.NodeId + ":" + diagnostic.Port + ":" + diagnostic.Message);
		REQUIRE(evaluated == Status::Ok);
		return image;
	}
	inline void Rows(const Image &image, std::initializer_list<std::initializer_list<int>> expected) {
		REQUIRE(image.Height == expected.size());
		size_t y = 0;
		for (const auto &row : expected) {
			REQUIRE(image.Width == row.size());
			size_t x = 0;
			for (const auto value : row) {
				for (size_t channel = 0; channel < 3; ++channel)
					CHECK(image.Pixels[(y * image.Width + x) * 4 + channel] == value);
				CHECK(image.Pixels[(y * image.Width + x) * 4 + 3] == 255);
				++x;
			}
			++y;
		}
	}
	inline Node Array(std::string id, ValueType type, Value a, Value b) {
		Node n{std::move(id), "pc.array", "", {}, {}, {}};
		n.DynamicInputs = {{"input_0", type, std::move(a)}, {"input_1", type, std::move(b)}};
		return n;
	}
	inline Node Solid(std::string id, Vector2 dimension, Colour colour) {
		return {
			std::move(id),
			"pc.solid",
			"",
			{},
			{{"dimension", dimension}, {"dimension_unit", EnumValue{0}}, {"color", colour}}
		};
	}
	inline void SurfaceRows(Document &d, std::string_view port) {
		d.Nodes.push_back(Solid("first", {2, 1}, {255, 255, 255, 255}));
		d.Nodes.push_back(Solid("second", {3, 2}, {255, 255, 255, 255}));
		Node list{"surfaces", "value.array", "", {}, {}};
		list.DynamicInputs = {
			{"first", ValueType::Image, std::nullopt}, {"second", ValueType::Image, std::nullopt}
		};
		d.Nodes.push_back(std::move(list));
		d.Links.push_back({"first", "surface_out", "surfaces", "first"});
		d.Links.push_back({"second", "surface_out", "surfaces", "second"});
		d.Links.push_back({"surfaces", "array", "generator", std::string(port)});
	}

	inline void Refuse(
		Document d,
		Status status,
		std::string_view port,
		uint64_t budget = Limits::MaximumEvaluationBytes,
		std::string_view message = {}
	) {
		Plan plan;
		Diagnostic diagnostic;
		REQUIRE(Compile(d, plan, diagnostic) == Status::Ok);
		Image output{1, 1, {9, 8, 7, 6}, 0};
		const auto prior = output;
		const auto actual = Evaluate(d, plan, "out", {}, output, diagnostic, budget);
		INFO(diagnostic.NodeId + ":" + diagnostic.Port + ":" + diagnostic.Message);
		CHECK(actual == status);
		if (!port.empty()) CHECK(diagnostic.NodeId == "generator");
		if (!port.empty()) CHECK(diagnostic.Port == port);
		if (!message.empty()) CHECK(diagnostic.Message.find(message) != std::string::npos);
		CHECK(output == prior);
	}
}
