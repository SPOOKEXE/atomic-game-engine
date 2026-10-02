#include "PixelBuilderPayload.hpp"
#include "ValuePayload.hpp"

#include <algorithm>

namespace engine::imagegraph::detail {
	Status RasterizePixelBuilder(
		const DynamicSurfaceValue &value,
		Vector2 dimension,
		Image &image,
		Diagnostic &diagnostic,
		uint64_t maximumBytes
	) {
		if (!value.Data || !ValidPixelBuilderPayload(value)) {
			diagnostic = {
				Status::InvalidValue,
				{},
				"dynamic_builder",
				"Dynamic Builder requires a valid owned source recipe"
			};
			return diagnostic.Code;
		}
		if (!std::isfinite(dimension.X) || !std::isfinite(dimension.Y) ||
			std::abs(dimension.X) > Limits::MaximumDimension ||
			std::abs(dimension.Y) > Limits::MaximumDimension) {
			diagnostic = {
				Status::InvalidValue,
				value.Data->OwnerNodeId,
				"dimension",
				"Dynamic Builder dimensions must be finite and bounded"
			};
			return diagnostic.Code;
		}
		const auto &data = *value.Data;
		const uint64_t retained = PixelBuilderStorageBytes(value, true);
		if (retained > maximumBytes / 2) {
			diagnostic = {
				Status::LimitExceeded,
				value.Data->OwnerNodeId,
				"dynamic_builder",
				"Dynamic Builder source clone exceeds the operation byte budget"
			};
			return diagnostic.Code;
		}
		Document document = data.Authored;
		auto owner = std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const Node &node) {
			return node.Id == data.OwnerNodeId;
		});
		if (owner == document.Nodes.end()) {
			diagnostic = {
				Status::InvalidValue,
				value.Data->OwnerNodeId,
				"dynamic_builder",
				"Dynamic Builder owner is missing"
			};
			return diagnostic.Code;
		}
		const auto set = [&](std::string_view port, Value replacement) {
			const auto found =
				std::find_if(owner->Values.begin(), owner->Values.end(), [&](const AuthoredValue &value) {
					return value.Port == port;
				});
			if (found == owner->Values.end())
				owner->Values.push_back({std::string(port), std::move(replacement)});
			else
				found->Data = std::move(replacement);
		};
		set("dimension", dimension);
		set("dimension_unit", EnumValue{0});
		std::erase_if(document.Links, [&](const Link &link) {
			return link.ToNode == data.OwnerNodeId &&
				   (link.ToPort == "dimension" || link.ToPort == "dimension_unit");
		});
		std::erase_if(document.Keyframes, [&](const Keyframe &key) {
			return key.NodeId == data.OwnerNodeId &&
				   (key.Port == "dimension" || key.Port == "dimension_unit");
		});
		std::erase_if(document.Tracks, [&](const AnimationTrack &track) {
			return track.NodeId == data.OwnerNodeId &&
				   (track.Port == "dimension" || track.Port == "dimension_unit");
		});
		std::erase_if(owner->SourceAnimatedInputs, [](const std::string &port) {
			return port == "dimension" || port == "dimension_unit";
		});
		for (const std::string port : {"dimension", "dimension_unit"}) {
			if (!owner->InstanceBase.empty() &&
				std::find(owner->InstanceOverrides.begin(), owner->InstanceOverrides.end(), port) ==
					owner->InstanceOverrides.end())
				owner->InstanceOverrides.push_back(port);
			if (port == "dimension" &&
				std::find(owner->SourceStaticInputs.begin(), owner->SourceStaticInputs.end(), port) ==
					owner->SourceStaticInputs.end())
				owner->SourceStaticInputs.push_back(port);
		}
		// Select the builder's pixels directly; dynamic output creation remains an owned side result.
		document.Outputs = {{"__pixel_builder_raster", data.OwnerNodeId, "surface_out"}};
		Plan plan;
		const Status compiled = Compile(document, plan, diagnostic);
		if (compiled != Status::Ok) return compiled;
		std::vector<std::string_view> cacheActions;
		cacheActions.reserve(data.SimulationCacheCaptures.size());
		for (const auto &node : data.SimulationCacheCaptures)
			cacheActions.emplace_back(node);
		EvaluationRequest request;
		request.Tick = data.Tick;
		request.Seed = data.Seed;
		request.Subframe = data.Subframe;
		request.NegativeFrame = data.NegativeFrame;
		request.DataReplay = data.DataHistory ? &*data.DataHistory : nullptr;
		request.SliceStackReplay = data.SliceStack ? &*data.SliceStack : nullptr;
		request.SimulationAuthoringRevision = data.SimulationAuthoringRevision;
		request.GroupReplay = data.Groups ? &data.Groups->Replay : nullptr;
		request.GroupAuthoringRevision = data.GroupAuthoringRevision;
		request.MaximumImageDimension = data.MaximumImageDimension;
		request.PixelBuilderCirclePrecision = data.CirclePrecision;
		request.RequireSourceGpuRasterCoverage = data.RequireSourceGpuRasterCoverage;
		request.ResetSurfaceReplay = data.ResetSurfaceReplay;
		request.SimulationCacheCaptures = cacheActions;
		request.AudioFrames = data.AudioFrames;
		request.AudioClips = data.AudioClips;
		request.ImageSources = data.ImageSources;
		request.HostCaptures = data.HostCaptures;
		request.PcxObservations = data.PcxObservations;
		request.ProjectName = data.ProjectName;
		request.RandomEntropy = data.Entropy;
		request.SimulationReplay = data.Simulation ? &*data.Simulation : nullptr;
		request.SurfaceReplay = data.Surfaces ? &*data.Surfaces : nullptr;
		request.RandomReplay = data.Random ? &*data.Random : nullptr;
		return Evaluate(
			document, plan, "__pixel_builder_raster", request, image, diagnostic, maximumBytes - retained
		);
	}
}
