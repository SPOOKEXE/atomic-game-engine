#pragma once

#include <engine/core/Paths.hpp>
#include <engine/graph/PipelineDocument.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

namespace engine::render::test {

	// Load the same staged recipe that a runnable demo selects.
	inline graph::PipelineDocument DemoPipelineAsset(std::string_view name) {
		const std::filesystem::path path =
			core::Paths::Base().parent_path() / "assets" / "examples" / "pipelines" / std::string(name);
		std::ifstream input(path, std::ios::binary);
		REQUIRE(input);
		const std::string text(std::istreambuf_iterator<char>(input), {});
		graph::PipelineDocument document;
		core::Name offender;
		REQUIRE(graph::Read(text, document, offender) == graph::PipelineDocumentStatus::Ok);
		return document;
	}
}
