#include "SourceNineSlice.hpp"

#include "PixelBuilderPayload.hpp"
#include "TimelineDrivers.hpp"
#include "nodes/Processor.hpp"

namespace engine::imagegraph::detail {
	Status RasterizeSourceNineSlice(
		const DynamicSurfaceValue &value,
		Vector2 dimension,
		std::array<double, 4> tint,
		Image &result,
		Diagnostic &diagnostic,
		uint64_t maximumBytes
	) {
		if (!value.Data || !value.Data->NineSlice || !ValidPixelBuilderPayload(value)) {
			diagnostic = {
				Status::InvalidValue,
				{},
				"dyna_surf",
				"Nine Slice requires a valid owned original-image recipe"
			};
			return diagnostic.Code;
		}
		const auto &data = *value.Data;
		if (data.NineSlice->Cold) {
			diagnostic = {
				Status::UnsupportedExecution,
				data.OwnerNodeId,
				"dyna_surf",
				"cold Nine Slice owns no getter surface"
			};
			return diagnostic.Code;
		}
		if (data.RequireSourceGpuRasterCoverage) {
			diagnostic = {
				Status::UnsupportedExecution,
				data.OwnerNodeId,
				"dyna_surf",
				"Nine Slice exact GPU part coverage requires a licensed renderer observation"
			};
			return diagnostic.Code;
		}
		for (double component : {dimension.X, dimension.Y})
			if (!std::isfinite(component) || component < 0 || component > data.MaximumImageDimension) {
				diagnostic = {
					Status::LimitExceeded,
					data.OwnerNodeId,
					"dimension",
					"Nine Slice rerender dimensions must be finite positive bounded values"
				};
				return diagnostic.Code;
			}
		for (double component : tint)
			if (!std::isfinite(component) || component < 0 || component > 1) {
				diagnostic = {
					Status::InvalidValue,
					data.OwnerNodeId,
					"color",
					"Nine Slice draw tint must be finite normalized values"
				};
				return diagnostic.Code;
			}
		const auto width = uint32_t(std::max(1., DriverRoundHalfEven(dimension.X))),
				   height = uint32_t(std::max(1., DriverRoundHalfEven(dimension.Y)));
		const uint64_t imageBytes = uint64_t(width) * height * 4,
					   retained = PixelBuilderStorageBytes(value, true);
		const uint64_t overhead = sizeof(NodeContext) + sizeof(Node) + data.OwnerNodeId.capacity() * 3 + 512;
		if (retained > maximumBytes || result.Pixels.capacity() > maximumBytes - retained ||
			overhead > maximumBytes - retained - result.Pixels.capacity() ||
			imageBytes > maximumBytes - retained - result.Pixels.capacity() - overhead) {
			diagnostic = {
				Status::LimitExceeded,
				data.OwnerNodeId,
				"dyna_surf",
				"Nine Slice retained recipe, previous output and rerender exceed byte budget"
			};
			return diagnostic.Code;
		}
		const auto *entry = FindCatalogueEntry("pc.9_slice");
		if (!entry) {
			diagnostic = {
				Status::InvalidValue, data.OwnerNodeId, "dyna_surf", "Nine Slice catalogue is absent"
			};
			return diagnostic.Code;
		}
		Node node{data.OwnerNodeId, "pc.9_slice", "", {}, {}};
		EvaluationRequest request;
		NodeContext context(node, *entry, request);
		context.ByteBudget = maximumBytes;
		auto reservation = context.ReserveWorkspace(
			retained + result.Pixels.capacity() + overhead + imageBytes, "dyna_surf"
		);
		Image prepared{width, height, {}, 0};
		if (!reservation ||
			!StageSourceNineSlice(context, *data.NineSlice, dimension, prepared, tint, false)) {
			diagnostic = {context.FailureCode, data.OwnerNodeId, context.FailurePort, context.FailureMessage};
			return diagnostic.Code;
		}
		prepared.Pixels.resize(imageBytes);
		if (!StageSourceNineSlice(context, *data.NineSlice, dimension, prepared, tint)) {
			diagnostic = {context.FailureCode, data.OwnerNodeId, context.FailurePort, context.FailureMessage};
			return diagnostic.Code;
		}
		result = std::move(prepared);
		diagnostic = {};
		return Status::Ok;
	}
}
