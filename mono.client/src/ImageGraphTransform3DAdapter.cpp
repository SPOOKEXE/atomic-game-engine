#include "ImageGraphTransform3DAdapter.hpp"

#include "ImageGraphSurfaceFormat.hpp"

#include <algorithm>
#include <limits>
#include <new>
#include <utility>

namespace client::detail {
	namespace {
		constexpr uint64_t MAXIMUM_TRANSFORM_HOST_BYTES =
			engine::render::imagegraph::MAXIMUM_TRANSFORM_IMAGE_3D_OUTPUT_BYTES;
		bool AddHostBytes(uint64_t &bytes, uint64_t addition) {
			if (addition > std::numeric_limits<uint64_t>::max() - bytes) return false;
			bytes += addition;
			return true;
		}

		const engine::imagegraph::EvaluationInputValue *
		InputValue(const engine::imagegraph::EvaluationSnapshot &snapshot, std::string_view port) {
			const auto values = snapshot.Values();
			const auto found = std::find_if(values.begin(), values.end(), [&](const auto &value) {
				return value.Port == port;
			});
			return found == values.end() ? nullptr : &*found;
		}

		const engine::imagegraph::EvaluationInputImage *
		InputImage(const engine::imagegraph::EvaluationSnapshot &snapshot, std::string_view port) {
			const auto images = snapshot.Images();
			const auto found = std::find_if(images.begin(), images.end(), [&](const auto &image) {
				return image.Port == port;
			});
			return found == images.end() ? nullptr : &*found;
		}

		bool PreflightTransformSurface(
			const engine::imagegraph::Image *source,
			engine::render::imagegraph::TransformImage3DSurface &destination,
			uint64_t &expectedBytes
		) {
			if (source == nullptr) {
				destination = {};
				expectedBytes = 0;
				return true;
			}
			const auto layout = engine::imagegraph::CheckedSurfaceLayout(
				source->Width,
				source->Height,
				source->Format,
				engine::render::imagegraph::MAXIMUM_TRANSFORM_IMAGE_3D_OUTPUT_BYTES
			);
			const auto format = TextureFormatForSurface(source->Format);
			if (!layout || !format || layout->Bytes != source->Pixels.size() ||
				source->Pixels.capacity() < source->Pixels.size())
				return false;
			expectedBytes = layout->Bytes;
			destination.Width = source->Width;
			destination.Height = source->Height;
			destination.Format = *format;
			return true;
		}

	}
	bool BuildTransformRequest(
		const engine::imagegraph::Document &document,
		const engine::imagegraph::Plan &plan,
		const engine::imagegraph::Node &node,
		uint64_t tick,
		uint64_t seed,
		bool displayColorSpace,
		engine::render::imagegraph::TransformImage3DRequest &outputRequest,
		engine::imagegraph::Diagnostic &diagnostic
	) {
		using namespace engine;
		engine::render::imagegraph::TransformImage3DRequest request;
		imagegraph::EvaluationSnapshot snapshot;
		const imagegraph::Status captured = imagegraph::EvaluateNodeInputs(
			document,
			plan,
			node.Id,
			imagegraph::EvaluationRequest{.Tick = tick, .Seed = seed},
			snapshot,
			diagnostic,
			MAXIMUM_TRANSFORM_HOST_BYTES
		);
		if (captured != imagegraph::Status::Ok) return false;

		const auto *front = InputImage(snapshot, "surface");
		const auto *back = InputImage(snapshot, "back_surface");
		if (front == nullptr) {
			diagnostic = {
				imagegraph::Status::InvalidValue,
				node.Id,
				"surface",
				"Transform Image 3D surface input is missing"
			};
			return false;
		}
		uint64_t frontBytes = 0, backBytes = 0;
		if (!PreflightTransformSurface(&front->Data, request.Front, frontBytes) ||
			!PreflightTransformSurface(back == nullptr ? nullptr : &back->Data, request.Back, backBytes)) {
			diagnostic = {
				imagegraph::Status::LimitExceeded,
				node.Id,
				"surface",
				"Transform Image 3D surface size is invalid or exceeds its host limit"
			};
			return false;
		}
		const auto *positionInput = InputValue(snapshot, "position");
		const auto *anchorInput = InputValue(snapshot, "anchor");
		const auto *rotationInput = InputValue(snapshot, "rotation");
		const auto *scaleInput = InputValue(snapshot, "scale");
		const auto *tilingInput = InputValue(snapshot, "texture_tiling");
		const auto *projectionInput = InputValue(snapshot, "projection");
		const auto *fovInput = InputValue(snapshot, "fov");
		const auto *viewRangeInput = InputValue(snapshot, "view_range");
		const auto *depthRangeInput = InputValue(snapshot, "depth_range");
		const auto *position =
			positionInput ? std::get_if<imagegraph::Vector3>(&positionInput->Data) : nullptr;
		const auto *anchor = anchorInput ? std::get_if<imagegraph::Vector3>(&anchorInput->Data) : nullptr;
		const auto *rotation =
			rotationInput ? std::get_if<imagegraph::Quaternion>(&rotationInput->Data) : nullptr;
		const auto *scale = scaleInput ? std::get_if<imagegraph::Vector3>(&scaleInput->Data) : nullptr;
		const auto *tiling = tilingInput ? std::get_if<imagegraph::Vector2>(&tilingInput->Data) : nullptr;
		const auto *projection =
			projectionInput ? std::get_if<imagegraph::EnumValue>(&projectionInput->Data) : nullptr;
		const auto *fov = fovInput ? std::get_if<double>(&fovInput->Data) : nullptr;
		const auto *viewRange =
			viewRangeInput ? std::get_if<imagegraph::Vector2>(&viewRangeInput->Data) : nullptr;
		const auto *depthRange =
			depthRangeInput ? std::get_if<imagegraph::Vector2>(&depthRangeInput->Data) : nullptr;
		if (!position || !anchor || !rotation || !scale || !tiling || !projection || !fov || !viewRange ||
			!depthRange || (projection->Value != 0 && projection->Value != 1)) {
			diagnostic = {
				imagegraph::Status::InvalidValue, node.Id, {}, "Transform Image 3D controls are invalid"
			};
			return false;
		}
		request.Position = {float(position->X), float(position->Y), float(position->Z)};
		request.Anchor = {float(anchor->X), float(anchor->Y), float(anchor->Z)};
		request.Rotation = {float(rotation->X), float(rotation->Y), float(rotation->Z), float(rotation->W)};
		request.Scale = {float(scale->X), float(scale->Y), float(scale->Z)};
		request.TextureTiling = {float(tiling->X), float(tiling->Y)};
		request.Projection = projection->Value == 0
								 ? render::imagegraph::TransformImage3DProjection::Perspective
								 : render::imagegraph::TransformImage3DProjection::Orthographic;
		request.FieldOfViewDegrees = float(*fov);
		request.ViewRange = {float(viewRange->X), float(viewRange->Y)};
		request.DepthRange = {float(depthRange->X), float(depthRange->Y)};
		request.ColorSpace = displayColorSpace ? render::imagegraph::TransformImage3DColorSpace::Display
											   : render::imagegraph::TransformImage3DColorSpace::Linear;
		if (request.Front.Width > 4096 || request.Front.Height > 4096 ||
			(frontBytes != 0 && backBytes != 0 &&
			 (request.Front.Width != request.Back.Width || request.Front.Height != request.Back.Height))) {
			diagnostic = {
				imagegraph::Status::LimitExceeded,
				node.Id,
				"surface",
				"Transform Image 3D surfaces exceed their native limits"
			};
			return false;
		}
		// Keep control-range validation in the renderer's shared validator without
		// allocating source-sized pixel buffers first.
		render::imagegraph::TransformImage3DRequest controlProbe = request;
		controlProbe.Front.Width = 1;
		controlProbe.Front.Height = 1;
		controlProbe.Front.Pixels.clear();
		try {
			controlProbe.Front.Pixels.resize(assets::BytesPerPixel(controlProbe.Front.Format));
		} catch (const std::bad_alloc &) {
			diagnostic = {
				imagegraph::Status::LimitExceeded,
				node.Id,
				"surface",
				"Transform Image 3D control validation allocation failed"
			};
			return false;
		}
		controlProbe.Back = {};
		if (render::imagegraph::ValidateTransformImage3D(controlProbe) !=
			render::imagegraph::TransformImage3DStatus::Ok) {
			diagnostic = {
				imagegraph::Status::InvalidValue, node.Id, {}, "Transform Image 3D controls are invalid"
			};
			return false;
		}
		uint64_t minimumHostBytes = snapshot.RetainedBytes();
		if (!AddHostBytes(minimumHostBytes, sizeof(request)) || !AddHostBytes(minimumHostBytes, frontBytes) ||
			!AddHostBytes(minimumHostBytes, backBytes) || minimumHostBytes > MAXIMUM_TRANSFORM_HOST_BYTES) {
			diagnostic = {
				imagegraph::Status::LimitExceeded,
				node.Id,
				{},
				"Transform Image 3D snapshot and request exceed the host byte limit"
			};
			return false;
		}
		try {
			request.Front.Pixels.reserve(static_cast<size_t>(frontBytes));
			request.Back.Pixels.reserve(static_cast<size_t>(backBytes));
		} catch (const std::bad_alloc &) {
			diagnostic = {
				imagegraph::Status::LimitExceeded,
				node.Id,
				{},
				"Transform Image 3D host request allocation failed"
			};
			return false;
		}
		uint64_t retainedHostBytes = snapshot.RetainedBytes();
		if (!AddHostBytes(retainedHostBytes, sizeof(request)) ||
			!AddHostBytes(retainedHostBytes, request.Front.Pixels.capacity()) ||
			!AddHostBytes(retainedHostBytes, request.Back.Pixels.capacity()) ||
			retainedHostBytes > MAXIMUM_TRANSFORM_HOST_BYTES) {
			diagnostic = {
				imagegraph::Status::LimitExceeded,
				node.Id,
				{},
				"Transform Image 3D snapshot and request capacities exceed the host byte limit"
			};
			return false;
		}
		const auto copyPixels = [](const imagegraph::Image &image, auto &pixels) {
			for (size_t index = 0; index < image.Pixels.size(); ++index)
				pixels.push_back(static_cast<std::byte>(image.Pixels[index]));
		};
		copyPixels(front->Data, request.Front.Pixels);
		if (back != nullptr) copyPixels(back->Data, request.Back.Pixels);

		if (render::imagegraph::ValidateTransformImage3D(request) !=
			render::imagegraph::TransformImage3DStatus::Ok) {
			diagnostic = {
				imagegraph::Status::InvalidValue, node.Id, {}, "Transform Image 3D inputs are invalid"
			};
			return false;
		}
		outputRequest = std::move(request);
		return true;
	}

}
