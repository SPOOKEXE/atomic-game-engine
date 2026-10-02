#pragma once

// Executor tables, one per documentation family. NodeExecutors.cpp merges them.

#include "../NodeExecutors.hpp"

#include <span>

namespace engine::imagegraph::detail {
	std::span<const ExecutorEntry> AudioExecutors();
	std::span<const ExecutorEntry> AudioFileExecutors();
	std::span<const ExecutorEntry> ArrayExecutors();
	std::span<const ExecutorEntry> ArrayStructureExecutors();
	std::span<const ExecutorEntry> ArrayEditExecutors();
	bool CollectSourceArray(NodeContext &context);
	std::span<const ExecutorEntry> FilterExecutors();
	std::span<const ExecutorEntry> GenerateExecutors();
	std::span<const ExecutorEntry> GradientExecutors();
	std::span<const ExecutorEntry> MatrixExecutors();
	std::span<const ExecutorEntry> CurveExecutors();
	std::span<const ExecutorEntry> ValueExecutors();
	std::span<const ExecutorEntry> VectorExecutors();
	std::span<const ExecutorEntry> OutlineExecutors();
	std::span<const ExecutorEntry> BlurExecutors();
	std::span<const ExecutorEntry> TransformExecutors();
	std::span<const ExecutorEntry> PathExecutors();
	std::span<const ExecutorEntry> PointExecutors();
	std::span<const ExecutorEntry> MeshExecutors();
}
