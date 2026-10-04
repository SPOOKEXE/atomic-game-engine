#pragma once
#include <engine/imagegraphexport/GraphFileHost.hpp>
namespace engine::imagegraphexport {
	struct GraphImageCacheLayoutObservation;
	// Only an admitted encoded source that the codec cannot decode may be skipped by Directory Search.
	enum class RasterFailure { Refused, SourceDecodeFailure };
	bool CaptureGraphRaster(
		const engine::imagegraph::HostNodeInvocation &,
		std::span<const GraphFileGrant>,
		const engine::assets::ContentPolicy &,
		engine::imagegraph::HostNodeCapture &,
		std::string &,
		RasterFailure *classification = nullptr,
		std::span<const GraphImageCacheLayoutObservation> caches = {}
	);
}
