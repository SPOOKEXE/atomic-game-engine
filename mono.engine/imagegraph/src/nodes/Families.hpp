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
	std::span<const ExecutorEntry> ArrayNumericExecutors();
	std::span<const ExecutorEntry> RandomExecutors();
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
	std::span<const ExecutorEntry> SourceErodeExecutors();
	std::span<const ExecutorEntry> TransformExecutors();
	std::span<const ExecutorEntry> PathExecutors();
	std::span<const ExecutorEntry> PointExecutors();
	std::span<const ExecutorEntry> MeshExecutors();
	std::span<const ExecutorEntry> MeshModifyExecutors();
	std::span<const ExecutorEntry> SourceMesh2DExecutors();
	std::span<const ExecutorEntry> Source2DExecutors();
	std::span<const ExecutorEntry> SourceAmbientOcclusionExecutors();
	std::span<const ExecutorEntry> SourceSimpleShapeExecutors();
	std::span<const ExecutorEntry> SourceTextExecutors();
	std::span<const ExecutorEntry> SourcePcxExecutors();
	std::span<const ExecutorEntry> SourceAnimationExecutors();
	std::span<const ExecutorEntry> SourceRoutingExecutors();
	std::span<const ExecutorEntry> SourceSwitchExecutors();
	std::span<const ExecutorEntry> SourceMiscExecutors();
	std::span<const ExecutorEntry> SourceSequenceAnimationExecutors();
	std::span<const ExecutorEntry> SourceCacheValueExecutors();
	std::span<const ExecutorEntry> SourceCacheResultsExecutors();
	std::span<const ExecutorEntry> SourceFrameCacheExecutors();
	std::span<const ExecutorEntry> SceneExecutors();
	std::span<const ExecutorEntry> SourceSdfExecutors();
	std::span<const ExecutorEntry> SimulationExecutors();
	std::span<const ExecutorEntry> SourceRigidExecutors();
	std::span<const ExecutorEntry> SourceValueExecutors();
	std::span<const ExecutorEntry> SourceConversionExecutors();
	std::span<const ExecutorEntry> SourceColourFilterExecutors();
	std::span<const ExecutorEntry> SourceDataExecutors();
	std::span<const ExecutorEntry> SourceMatrixExecutors();
	std::span<const ExecutorEntry> SourcePathExecutors();
	std::span<const ExecutorEntry> SourcePathComposeExecutors();
	std::span<const ExecutorEntry> SourcePathModifierExecutors();
	std::span<const ExecutorEntry> SourcePathGeometryExecutors();
	std::span<const ExecutorEntry> SourcePathShiftExecutors();
	std::span<const ExecutorEntry> SourcePathWeightExecutors();
	std::span<const ExecutorEntry> SourceQuaternionLookAtExecutors();
	std::span<const ExecutorEntry> SourcePointsExecutors();
	std::span<const ExecutorEntry> SourcePointDataExecutors();
	std::span<const ExecutorEntry> SourceSpatialPointsExecutors();
	std::span<const ExecutorEntry> SourceSpatialShapeExecutors();
	std::span<const ExecutorEntry> SourceTileExecutors();
	std::span<const ExecutorEntry> SourceSpriteStackExecutors();
	std::span<const ExecutorEntry> SourceNormalMapExecutors();
	std::span<const ExecutorEntry> SourceBevelExecutors();
	std::span<const ExecutorEntry> SourcePixelBevelExecutors();
	std::span<const ExecutorEntry> SourceVolumeProjectionExecutors();
	std::span<const ExecutorEntry> SourceCylinderProjectionExecutors();
	std::span<const ExecutorEntry> SourceHeightmapProjectionExecutors();
	std::span<const ExecutorEntry> SourceAtlasExecutors();
	std::span<const ExecutorEntry> SourceAtlasPixelExecutors();
	std::span<const ExecutorEntry> SourcePaletteExecutors();
	std::span<const ExecutorEntry> SourceSurfaceDataExecutors();
	std::span<const ExecutorEntry> SourceSurfaceBufferExecutors();
	std::span<const ExecutorEntry> HostExecutors();
	// Reuse exact authored/time/control/image validation for opaque source observations.
	bool ReplayRecordedHostOutputs(NodeContext &context);
	std::span<const ExecutorEntry> TriggerExecutors();
	std::span<const ExecutorEntry> TemporalExecutors();
}
