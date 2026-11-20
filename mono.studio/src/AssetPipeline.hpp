#pragma once

// What the Asset Pipeline panel holds between frames. Defined here rather than
// in `AssetPipeline.cpp` because `Editor` owns it through a `unique_ptr` and its
// destructor needs the complete type.

#include <engine/bakegraph/Document.hpp>
#include <engine/core/Name.hpp>

#include <array>
#include <cstdint>
#include <nodegraph/Editor.hpp>
#include <string>
#include <studio/Editor.hpp>
#include <vector>

namespace studio {
	struct AssetPipelineState {
		enum class Action : uint8_t { Save, Remove, Bake };
		struct Pending {
			Action Kind = Action::Save;
			WorldId World;
			engine::core::Name Pipeline;
			engine::bake::Document Document;
		};

		nodegraph::Graph Graph;
		nodegraph::Canvas Canvas;

		// Which world and pipeline the canvas holds. A change to either reloads.
		WorldId World;
		std::string Open;
		bool Loaded = false;
		bool Dirty = false;

		std::array<char, 96> NewName{};
		std::string Status;
		std::vector<Pending> Queue;
	};
}
