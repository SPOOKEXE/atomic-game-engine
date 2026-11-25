#pragma once

// angview's own registry. Port geometry with the Dark theme and zoom 1, for a
// node at (x, y): inputs at (x, y + 40 + 20 * row), outputs at (x + 160, same).

#include <engine/angview/View.hpp>

#include <catch2/catch_test_macros.hpp>

namespace engine::angview::tests {

	inline ang::Registry MakeRegistry() {
		ang::Registry registry = ang::Registry::WithScalars();
		const auto noop = [](ang::EvalContext &) { return ang::NodeResult::Success(); };
		registry.DefineNode(
			{.Id = "math.number",
			 .Category = "math",
			 .Label = "Number",
			 .Outputs = {{.Name = "value", .Type = "number"}},
			 .Evaluate = noop}
		);
		registry.DefineNode(
			{.Id = "math.add",
			 .Category = "math",
			 .Label = "Add",
			 .Inputs = {{.Name = "a", .Type = "number"}, {.Name = "b", .Type = "number"}},
			 .Outputs = {{.Name = "sum", .Type = "number"}},
			 .Evaluate = noop}
		);
		registry.DefineNode(
			{.Id = "text.upper",
			 .Category = "text",
			 .Label = "Upper",
			 .Inputs = {{.Name = "text", .Type = "string"}},
			 .Outputs = {{.Name = "text", .Type = "string"}},
			 .Evaluate = noop}
		);
		registry.DefineNode(
			{.Id = "math.total",
			 .Category = "math",
			 .Label = "Total",
			 .Inputs = {{.Name = "values", .Type = "number", .MaximumLinks = 0}},
			 .Outputs = {{.Name = "sum", .Type = "number"}},
			 .Evaluate = noop}
		);
		return registry;
	}

	inline void Add(ang::Graph &graph, std::string id, std::string def, core::Vector2 position) {
		REQUIRE(
			graph
				.Apply(ang::edit::AddNode{{.Id = std::move(id), .Def = std::move(def), .Position = position}})
				.Ok
		);
	}

	inline core::Vector2 InputPort(core::Vector2 node, int row) {
		return {node.X, node.Y + 40.0f + 20.0f * static_cast<float>(row)};
	}

	inline core::Vector2 OutputPort(core::Vector2 node, int row) {
		return {node.X + 160.0f, node.Y + 40.0f + 20.0f * static_cast<float>(row)};
	}

	// Press, move in steps, release.
	inline void Drag(
		View &view,
		core::Vector2 from,
		core::Vector2 to,
		PointerButton button = PointerButton::Left,
		Modifiers modifiers = {}
	) {
		view.PointerDown(button, from, modifiers);
		view.PointerMove({(from.X + to.X) * 0.5f, (from.Y + to.Y) * 0.5f}, modifiers);
		view.PointerMove(to, modifiers);
		view.PointerUp(button, to, modifiers);
	}

	inline bool HasText(const DrawList &list, std::string_view text) {
		for (const DrawCommand &command : list) {
			if (command.Kind == DrawKind::Text && command.Text.find(text) != std::string::npos) {
				return true;
			}
		}
		return false;
	}
}
