#include "SourceCameraNodes.hpp"

#include "Families.hpp"
#include "Processor.hpp"

#include <engine/core/Metrics.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace engine::imagegraph::detail {
	namespace {
		struct CameraLayer {
			size_t Group = 0;
			std::string_view Element, Positioning, Position, Oversample, Parallax, Depth;
		};
		struct PreparedCameraLayer {
			const Image *Source = nullptr;
			std::string_view Port;
			double OriginX = 0, OriginY = 0, BokehDistance = 0;
			int64_t Mode = 0;
		};
		struct CameraPlan {
			uint32_t Width = 0, Height = 0;
			SurfaceFormat Format = SurfaceFormat::RGBA8Unorm;
			bool Filtered = true;
			double Zoom = 1.0;
			uint64_t OutputBytes = 0, SampleWork = 0;
			std::vector<PreparedCameraLayer> Layers;
			AllocationReservation Workspace;
		};
		constexpr uint64_t MAXIMUM_CAMERA_SAMPLE_WORK = 64'000'000;
		bool FiniteControls(
			NodeContext &context,
			const CameraLayer &layer,
			Vector2 &position,
			Vector2 &parallax,
			double &depth,
			int64_t &positioning,
			int64_t &oversample,
			Vector2 referenceDimension
		);

		bool CameraLayers(NodeContext &context, std::vector<CameraLayer> &layers, size_t capacity) {
			if (context.Authored.DynamicInputs.size() > Limits::MaximumDynamicInputsPerNode)
				return context.Fail(Status::LimitExceeded, "camera exceeds the dynamic input limit");
			layers.reserve(capacity);
			for (const DynamicInput &input : context.Authored.DynamicInputs) {
				size_t group = 0;
				const CatalogueInput *slot = FindDynamicTemplate(context.Entry, input.Id, group);
				if (!slot || slot->Type != input.Type)
					return context.Fail(
						Status::InvalidValue, "camera has an invalid dynamic input", input.Id
					);
				if (group >= Limits::MaximumDynamicInputsPerNode / 6)
					return context.Fail(
						Status::LimitExceeded, "camera layer index exceeds its bound", input.Id
					);
				auto found = std::find_if(layers.begin(), layers.end(), [group](const CameraLayer &layer) {
					return layer.Group == group;
				});
				if (found == layers.end()) {
					layers.push_back({});
					found = std::prev(layers.end());
					found->Group = group;
				}
				std::string_view *member = nullptr;
				if (slot->Id == "element")
					member = &found->Element;
				else if (slot->Id == "positioning")
					member = &found->Positioning;
				else if (slot->Id == "position")
					member = &found->Position;
				else if (slot->Id == "oversample")
					member = &found->Oversample;
				else if (slot->Id == "parallax")
					member = &found->Parallax;
				else if (slot->Id == "depth")
					member = &found->Depth;
				if (!member || !member->empty())
					return context.Fail(
						Status::InvalidValue, "camera has a duplicate dynamic input", input.Id
					);
				*member = input.Id;
			}
			std::sort(layers.begin(), layers.end(), [](const CameraLayer &a, const CameraLayer &b) {
				return a.Group < b.Group;
			});
			for (size_t index = 0; index < layers.size(); ++index) {
				const CameraLayer &layer = layers[index];
				if (layer.Group != index)
					return context.Fail(Status::InvalidValue, "camera layer groups must be contiguous");
				if (layer.Element.empty() || layer.Positioning.empty() || layer.Position.empty() ||
					layer.Oversample.empty() || layer.Parallax.empty() || layer.Depth.empty())
					return context.Fail(Status::InvalidValue, "camera layer has incomplete inputs");
			}
			return true;
		}

		bool LayerVisibility(NodeContext &context, size_t count, std::vector<uint8_t> &visible) {
			visible.assign(count, 1);
			const AuthoredValue *property = nullptr;
			for (const AuthoredValue &candidate : context.Authored.SourceProperties) {
				if (candidate.Port != "layer_visible") continue;
				if (property)
					return context.Fail(
						Status::InvalidValue, "camera duplicates layer visibility", "layer_visible"
					);
				property = &candidate;
			}
			if (!property) return true;
			const auto *array = std::get_if<ArrayValue>(&property->Data);
			if (!array || !array->Nested.empty() || !array->Items.empty() || array->Elements.size() != count)
				return context.Fail(
					Status::InvalidValue, "camera layer visibility has invalid shape", "layer_visible"
				);
			for (size_t index = 0; index < array->Elements.size(); ++index) {
				const auto *flag = std::get_if<bool>(&array->Elements[index]);
				if (!flag)
					return context.Fail(
						Status::InvalidValue, "camera layer visibility must contain booleans", "layer_visible"
					);
				visible[index] = *flag ? 1 : 0;
			}
			return true;
		}

		bool PrepareCamera(NodeContext &context, CameraPlan &plan) {
			uint32_t referenceWidth = 0, referenceHeight = 0;
			if (!ResolveDimension(context, "scene_size", referenceWidth, referenceHeight)) return false;
			if (!ResolveDimension(context, "camera_size", plan.Width, plan.Height)) return false;
			const auto *depthEntry = FindCatalogueInput(context.Entry, "attribute_color_depth");
			if (!depthEntry)
				return context.Fail(Status::UnsupportedExecution, "camera color-depth schema is absent");
			if (depthEntry->Default.empty() && !context.Find("attribute_color_depth"))
				return context.Fail(
					Status::UnsupportedExecution,
					"camera color-depth default is unresolved",
					"attribute_color_depth"
				);
			const auto format = ResolveProcessorSurfaceFormat(context, nullptr);
			if (!format) return false;
			plan.Format = *format;
			if (context.Request.MaximumImageDimension == 0 ||
				context.Request.MaximumImageDimension > Limits::MaximumDimension ||
				plan.Width > context.Request.MaximumImageDimension ||
				plan.Height > context.Request.MaximumImageDimension)
				return context.Fail(
					Status::LimitExceeded, "camera dimensions exceed the request budget", "camera_size"
				);
			const auto outputLayout =
				CheckedSurfaceLayout(plan.Width, plan.Height, plan.Format, Limits::MaximumOutputBytes);
			if (!outputLayout)
				return context.Fail(
					Status::LimitExceeded, "camera output exceeds byte bounds", "surface_out"
				);
			plan.OutputBytes = outputLayout->Bytes;
			const size_t layerCapacity =
				std::min(context.Authored.DynamicInputs.size(), Limits::MaximumDynamicInputsPerNode / 6);
			const uint64_t scratchBytes =
				layerCapacity * (sizeof(CameraLayer) + sizeof(PreparedCameraLayer) + sizeof(uint8_t));
			auto workspace = context.ReserveWorkspace(scratchBytes, "layers");
			if (!workspace) return false;
			plan.Workspace = std::move(*workspace);
			std::vector<CameraLayer> layers;
			if (!CameraLayers(context, layers, layerCapacity)) return false;
			std::vector<uint8_t> visible;
			if (!LayerVisibility(context, layers.size(), visible)) return false;
			plan.Layers.reserve(layerCapacity);
			const Vector2 referenceDimension{double(referenceWidth), double(referenceHeight)};
			const Vector2 focus =
				UnitVector(context, "focus_center", referenceDimension.X, referenceDimension.Y);
			const double zoom = context.Scalar("zoom", 1.0);
			plan.Zoom = zoom;
			const bool dof = context.Boolean("depth_of_field", false);
			const double focalDistance = context.Scalar("focal_distance", 0.0);
			const double focalRange = context.Scalar("focal_range", 0.0);
			const double defocus = context.Scalar("defocus", 1.0);
			plan.Filtered = context.InheritedInterpolation != 1 && context.InheritedInterpolation != 6;
			if (context.FailureCode != Status::Ok) return false;
			if (!std::isfinite(focus.X) || !std::isfinite(focus.Y) || !std::isfinite(zoom) ||
				!std::isfinite(focalDistance) || !std::isfinite(focalRange) || !std::isfinite(defocus))
				return context.Fail(Status::InvalidValue, "camera controls must be finite");
			if (focus.X < double(std::numeric_limits<int32_t>::min()) ||
				focus.X > double(std::numeric_limits<int32_t>::max()) ||
				focus.Y < double(std::numeric_limits<int32_t>::min()) ||
				focus.Y > double(std::numeric_limits<int32_t>::max()))
				return context.Fail(
					Status::LimitExceeded, "camera focus exceeds source integer range", "focus_center"
				);
			const double cameraX = std::nearbyint(focus.X), cameraY = std::nearbyint(focus.Y);
			const uint64_t pixels = uint64_t(plan.Width) * plan.Height;
			for (size_t index = 0; index < layers.size(); ++index) {
				if (!visible[index]) continue;
				const CameraLayer &layer = layers[index];
				const Image *source = context.Input(layer.Element);
				if (!source) continue;
				if (!ValidSurfaceLayout(*source, Limits::MaximumDimension, Limits::MaximumEvaluationBytes))
					return context.Fail(
						Status::InvalidValue, "camera layer surface has an invalid layout", layer.Element
					);
				Vector2 position{}, parallax{};
				double depth = 0.0;
				int64_t positioning = 0, mode = 0;
				if (!FiniteControls(
						context, layer, position, parallax, depth, positioning, mode, referenceDimension
					))
					return false;
				const double shiftX = position.X + parallax.X * cameraX;
				const double shiftY = position.Y + parallax.Y * cameraY;
				const double bokehDistance =
					dof ? std::max(std::abs(depth - focalDistance) - focalRange, 0.0) *
							  std::tanh(std::max(std::abs(depth - focalDistance) - focalRange, 0.0) / 10.0) *
							  defocus
						: 0.0;
				PreparedCameraLayer prepared{
					source,
					layer.Position,
					(positioning == 0 ? cameraX - shiftX : -shiftX) / source->Width,
					(positioning == 0 ? cameraY - shiftY : -shiftY) / source->Height,
					bokehDistance,
					mode
				};
				if (!std::isfinite(prepared.OriginX) || !std::isfinite(prepared.OriginY) ||
					!std::isfinite(prepared.BokehDistance))
					return context.Fail(
						Status::InvalidValue, "camera layer transform exceeds numeric range", layer.Position
					);
				const uint64_t taps = bokehDistance == 0.0 ? 1 : 400;
				if (pixels > MAXIMUM_CAMERA_SAMPLE_WORK / taps ||
					pixels * taps > MAXIMUM_CAMERA_SAMPLE_WORK - plan.SampleWork)
					return context.Fail(
						Status::LimitExceeded, "camera sample work exceeds its bounded profile", layer.Depth
					);
				plan.SampleWork += pixels * taps;
				plan.Layers.push_back(prepared);
			}
			return true;
		}

		bool FiniteControls(
			NodeContext &context,
			const CameraLayer &layer,
			Vector2 &position,
			Vector2 &parallax,
			double &depth,
			int64_t &positioning,
			int64_t &oversample,
			Vector2 referenceDimension
		) {
			position = UnitVector(context, layer.Position, referenceDimension.X, referenceDimension.Y);
			parallax = context.Vec2(layer.Parallax);
			depth = context.Scalar(layer.Depth);
			positioning = context.SourceChoice(layer.Positioning, 0);
			oversample = context.SourceChoice(layer.Oversample, 0);
			if (context.FailureCode != Status::Ok) return false;
			if (!std::isfinite(position.X) || !std::isfinite(position.Y) || !std::isfinite(parallax.X) ||
				!std::isfinite(parallax.Y) || !std::isfinite(depth))
				return context.Fail(
					Status::InvalidValue, "camera layer controls must be finite", layer.Position
				);
			if (positioning < 0 || positioning > 1)
				return context.Fail(
					Status::InvalidValue, "camera positioning choice is invalid", layer.Positioning
				);
			if (oversample < 0 || oversample > 3)
				return context.Fail(
					Status::InvalidValue, "camera oversample choice is invalid", layer.Oversample
				);
			return true;
		}

		Rgba SampleCamera(const Image &image, double u, double v, int64_t mode, bool filtered) {
			if (!std::isfinite(u) || !std::isfinite(v)) return {};
			const bool insideX = u >= 0.0 && u <= 1.0;
			const bool insideY = v >= 0.0 && v <= 1.0;
			if (!insideX || !insideY) {
				if (mode == 0 || (mode == 2 && !insideY) || (mode == 3 && !insideX) || mode < 0 || mode > 3)
					return {};
				u -= std::floor(u);
				v -= std::floor(v);
			}
			return filtered ? BilinearClamp(image, u, v) : SampleNearest(image, u, v);
		}

		Rgba BokehSample(
			const Image &image,
			double u,
			double v,
			double radius,
			int64_t mode,
			bool filtered,
			uint64_t &sampleTaps
		) {
			if (radius == 0.0) {
				++sampleTaps;
				const Rgba direct = SampleCamera(image, u, v, mode, filtered);
				const std::array<double, 3> premultiplied{
					direct[0] * direct[3], direct[1] * direct[3], direct[2] * direct[3]
				};
				return {
					premultiplied[0], premultiplied[1], premultiplied[2], direct[3] * direct[3] * direct[3]
				};
			}

			constexpr double GOLDEN_ANGLE = 2.39996323;
			constexpr double CONTRAST = 150.0;
			constexpr double SMOOTH = 2.0;
			const double cosine = std::cos(GOLDEN_ANGLE), sine = std::sin(GOLDEN_ANGLE);
			const double aspect = double(image.Height) / image.Width;
			double angleX = 0.0, angleY = radius * 0.01 / 20.0;
			double reciprocal = 1.0;
			std::array<double, 3> numerator{}, weight{};
			double alpha = 0.0;
			for (int sample = 0; sample < 400; ++sample) {
				++sampleTaps;
				reciprocal += 1.0 / reciprocal;
				const double nextX = cosine * angleX - sine * angleY;
				const double nextY = sine * angleX + cosine * angleY;
				angleX = nextX;
				angleY = nextY;
				const double offset = reciprocal - 1.0;
				const Rgba sampled =
					SampleCamera(image, u + aspect * offset * angleX, v + offset * angleY, mode, filtered);
				const std::array<double, 3> colour{
					sampled[0] * sampled[3], sampled[1] * sampled[3], sampled[2] * sampled[3]
				};
				std::array<double, 3> bokeh{};
				for (size_t channel = 0; channel < 3; ++channel) {
					bokeh[channel] = SMOOTH + std::pow(colour[channel], 9.0) * CONTRAST;
					numerator[channel] += colour[channel] * bokeh[channel];
					weight[channel] += bokeh[channel];
				}
				alpha += sampled[3] * (bokeh[0] + bokeh[1] + bokeh[2]) / 3.0;
			}
			const double meanWeight = (weight[0] + weight[1] + weight[2]) / 3.0;
			if (!(meanWeight > 0.0) || !std::isfinite(meanWeight)) return {};
			const double alphaRatio = alpha / meanWeight;
			return {
				numerator[0] / weight[0],
				numerator[1] / weight[1],
				numerator[2] / weight[2],
				alphaRatio * alphaRatio * alphaRatio
			};
		}

		Rgba Composite(Rgba back, Rgba front) {
			const double alpha = front[3] + back[3] * (1.0 - front[3]);
			if (!(alpha > 0.0)) return {};
			Rgba result{};
			for (size_t channel = 0; channel < 3; ++channel)
				result[channel] =
					(back[channel] * back[3] * (1.0 - front[3]) + front[channel] * front[3]) / alpha;
			result[3] = alpha;
			return result;
		}
	}

	bool Camera(NodeContext &context) {
		ENGINE_PROFILE("imagegraph.camera.render");
		struct CameraMetrics {
			uint64_t SampleTaps = 0, OutputBytes = 0;
			~CameraMetrics() {
				core::Metrics::Count("imagegraph.camera.sample_taps", double(SampleTaps));
				core::Metrics::Count("imagegraph.camera.output_bytes", double(OutputBytes));
			}
		} metrics;
		CameraPlan plan;
		if (!PrepareCamera(context, plan)) return false;
		Image *output = context.NewImage("surface_out", plan.Width, plan.Height, plan.Format);
		if (!output) return false;
		for (const PreparedCameraLayer &layer : plan.Layers) {
			for (uint32_t y = 0; y < plan.Height; ++y)
				for (uint32_t x = 0; x < plan.Width; ++x) {
					const double uvX = (double(x) + 0.5) / plan.Width;
					const double uvY = (double(y) + 0.5) / plan.Height;
					const double sourceX =
						layer.OriginX + (uvX - 0.5) * (double(plan.Width) / layer.Source->Width) * plan.Zoom;
					const double sourceY = layer.OriginY + (uvY - 0.5) *
															   (double(plan.Height) / layer.Source->Height) *
															   plan.Zoom;
					const double u = sourceX;
					const double v = sourceY;
					const Rgba front = BokehSample(
						*layer.Source,
						u,
						v,
						layer.BokehDistance,
						layer.Mode,
						plan.Filtered,
						metrics.SampleTaps
					);
					const Rgba back = ReadPixel(*output, x, y);
					if (!WritePixel(*output, x, y, Composite(back, front)))
						return context.Fail(
							Status::InvalidValue, "camera output sample exceeds surface range", "surface_out"
						);
					metrics.OutputBytes += DescribeSurfaceFormat(plan.Format)->BytesPerPixel;
				}
		}
		return true;
	}

	bool AdmitSourceCamera(NodeContext &context, uint64_t &batchWork, uint64_t &batchBytes) {
		CameraPlan plan;
		if (!PrepareCamera(context, plan)) return false;
		if (plan.SampleWork > MAXIMUM_CAMERA_SAMPLE_WORK - batchWork)
			return context.Fail(
				Status::LimitExceeded, "camera batch exceeds its bounded sample work", "surface_out"
			);
		if (plan.OutputBytes > Limits::MaximumEvaluationBytes - batchBytes)
			return context.Fail(
				Status::LimitExceeded, "camera batch output exceeds its retained byte budget", "surface_out"
			);
		batchWork += plan.SampleWork;
		batchBytes += plan.OutputBytes;
		return true;
	}
}
