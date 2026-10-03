#pragma once
#include <engine/render/ComposerHlsl.hpp>

#include <limits>
#include <optional>
namespace engine::render::hlsl::detail {
	// A replacement vector allocation overlaps the old backing until its noexcept
	// swap.
	inline std::optional<uint64_t> AdmitComposerVectorGrowth(
		uint64_t oldBacking, uint64_t newBacking, uint64_t held, uint64_t scratch, uint64_t maximum
	) {
		if (oldBacking > held || held > maximum || scratch > maximum - held ||
			newBacking > maximum - held - scratch)
			return {};
		return held - oldBacking + newBacking;
	}

	inline uint64_t SourceRetainedBytes(const Source &source) {
		uint64_t bytes = sizeof(source) + source.Vertex.capacity() + 1 + source.Fragment.capacity() + 1 +
						 source.MissingLibraries.capacity() * sizeof(std::string);
		for (const auto &name : source.MissingLibraries)
			bytes += name.capacity() + 1;
		return bytes;
	}
	inline uint64_t ProgramRetainedBytes(const Program &program) {
		uint64_t bytes =
			sizeof(program) +
			(program.Vertex.SpirV.capacity() + program.Fragment.SpirV.capacity()) * sizeof(uint32_t) +
			program.Arguments.capacity() * sizeof(Argument) + program.Members.capacity() * sizeof(Member);
		for (const auto &argument : program.Arguments)
			bytes += argument.Name.capacity() + 1;
		for (const auto &member : program.Members)
			bytes += member.Name.capacity() + 1;
		return bytes;
	}
}
