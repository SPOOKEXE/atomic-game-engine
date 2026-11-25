#include "Execution.hpp"
#include "Patterns.hpp"
#include "Pixels.hpp"

#include <engine/ang/Runner.hpp>
#include <engine/core/Float16.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/imagegraph/Document.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <optional>

namespace engine::imagegraph {
	namespace {
		using detail::Buffer;
		using detail::Pixel;

		bool EvaluationFailure(Diagnostic &diagnostic, std::string_view node, std::string_view message) {
			diagnostic = {std::string(node.substr(0, 128)), std::string(message.substr(0, 4096))};
			return false;
		}
		// Count declared work and peak live results before any resolver or pixel allocation.
		// Source extents remain unknown until decoded and are checked by the execution gate.
		bool Preflight(
			const Document &document,
			const Plan &plan,
			const std::vector<uint8_t> &needed,
			size_t target,
			Diagnostic &diagnostic
		) {
			const size_t bytesPerPixel = detail::StorageBytes(document.Storage);
			std::vector<ImageExtent> extents(document.Nodes.size());
			uint64_t work = 0;
			for (size_t index : plan.Order) {
				if (!needed[index] || std::holds_alternative<Source>(document.Nodes[index].Value)) continue;
				const size_t extentInput = detail::ExtentInput(document.Nodes[index].Value);
				const ImageExtent input = plan.Inputs[index].size() <= extentInput
											  ? ImageExtent{}
											  : extents[plan.Inputs[index][extentInput]];
				const auto extent = detail::Extent(document.Nodes[index].Value, input, bytesPerPixel);
				extents[index] = extent;
				const uint64_t pixels = uint64_t(extent.Width) * extent.Height *
										detail::WorkPerPixel(document.Nodes[index].Value);
				if (pixels > Limits::MaximumPixelWork - work)
					return EvaluationFailure(
						diagnostic, document.Nodes[index].Id, "pixel work budget exceeded"
					);
				work += pixels;
			}
			if (detail::LiveBytes(plan, detail::LiveOrder(plan, target, extents), extents) >
				Limits::MaximumLiveBytes)
				return EvaluationFailure(diagnostic, document.Nodes[target].Id, "live image budget exceeded");
			return true;
		}

		double Unit255(double value) {
			return value / 255.0;
		}
		double Factor(const Pixel &pixel, Channel channel) {
			if (channel == Channel::Luminance)
				return (0.2126 * pixel[0] + 0.7152 * pixel[1] + 0.0722 * pixel[2]) / 255.0;
			return pixel[static_cast<size_t>(channel)] / 255.0;
		}
		// Normalized position of a generator pixel, or the Vector input's red and green.
		std::array<double, 2> Position(const Buffer &output, const Buffer *vector, uint32_t x, uint32_t y) {
			const double u = (x + 0.5) / output.Width, v = (y + 0.5) / output.Height;
			if (vector == nullptr) return {u, v};
			const Pixel pixel = detail::At(
				*vector,
				static_cast<int64_t>(std::floor(u * vector->Width)),
				static_cast<int64_t>(std::floor(v * vector->Height)),
				true
			);
			return {Unit255(pixel[0]), Unit255(pixel[1])};
		}
		Pixel Grey(double value) {
			const double scaled = std::clamp(value, 0.0, 1.0) * 255.0;
			return {scaled, scaled, scaled, 255.0};
		}
		double BlendChannel(BlendMode mode, double back, double front) {
			switch (mode) {
			case BlendMode::Normal:
				return front;
			case BlendMode::Multiply:
				return back * front;
			case BlendMode::Screen:
				return back + front - back * front;
			case BlendMode::Overlay:
				return back <= 0.5 ? 2 * back * front : 1 - 2 * (1 - back) * (1 - front);
			case BlendMode::Add:
				return std::min(back + front, 1.0);
			case BlendMode::Subtract:
				return std::max(back - front, 0.0);
			case BlendMode::SoftLight: {
				if (front <= 0.5) return back - (1 - 2 * front) * back * (1 - back);
				const double d = back <= 0.25 ? ((16 * back - 12) * back + 4) * back : std::sqrt(back);
				return back + (2 * front - 1) * (d - back);
			}
			case BlendMode::Difference:
				return std::abs(back - front);
			}
			return front;
		}
		// Separable blend composited with straight alpha (W3C compositing). Normal mode
		// is exactly Over, so unmasked v1 and v2 blends keep their bytes.
		Pixel Mixed(const Pixel &back, const Pixel &front, double opacity, BlendMode mode) {
			if (mode == BlendMode::Normal) return detail::Over(back, front, opacity);
			const double source = front[3] / 255.0 * opacity;
			const double backdrop = back[3] / 255.0;
			const double alpha = source + backdrop * (1 - source);
			if (alpha == 0) return {};
			Pixel pixel{};
			for (size_t channel = 0; channel < 3; channel++) {
				const double cb = back[channel] / 255.0, cs = front[channel] / 255.0;
				const double blended = BlendChannel(mode, cb, cs);
				const double premultiplied =
					(1 - source) * backdrop * cb + (1 - backdrop) * source * cs + source * backdrop * blended;
				pixel[channel] = premultiplied / alpha * 255.0;
			}
			pixel[3] = alpha * 255.0;
			return pixel;
		}
		double Arithmetic(MathOperation operation, double a, double b) {
			switch (operation) {
			case MathOperation::Add:
				return a + b;
			case MathOperation::Subtract:
				return a - b;
			case MathOperation::Multiply:
				return a * b;
			case MathOperation::Divide:
				return b == 0 ? 0 : a / b;
			case MathOperation::Minimum:
				return std::min(a, b);
			case MathOperation::Maximum:
				return std::max(a, b);
			case MathOperation::Power:
				return std::pow(std::max(a, 0.0), b);
			}
			return a;
		}
		Pixel RampAt(const Ramp &ramp, double factor) {
			const auto &stops = ramp.Stops;
			const auto colour = [](const RampStop &stop) {
				return Pixel{
					double(stop.Colour[0]),
					double(stop.Colour[1]),
					double(stop.Colour[2]),
					double(stop.Colour[3])
				};
			};
			if (factor <= stops.front().Position) return colour(stops.front());
			if (factor >= stops.back().Position) return colour(stops.back());
			size_t upper = 1;
			while (upper < stops.size() && stops[upper].Position <= factor)
				++upper;
			const auto &low = stops[upper - 1], &high = stops[upper];
			if (ramp.Interpolation == RampInterpolation::Constant) return colour(low);
			double t = (factor - low.Position) / (high.Position - low.Position);
			if (ramp.Interpolation == RampInterpolation::Smooth) t = t * t * (3 - 2 * t);
			Pixel pixel{};
			for (size_t channel = 0; channel < 4; ++channel)
				pixel[channel] = detail::Lerp(low.Colour[channel], high.Colour[channel], t);
			return pixel;
		}
		// Premultiplied gaussian; edges clamp. Sigma is radius / 3 and taps reach ceil(radius).
		void BlurInto(const Buffer &input, double radius, Buffer &output) {
			const auto reach = static_cast<int64_t>(std::ceil(radius));
			if (reach == 0) {
				output.Data = input.Data;
				return;
			}
			const double sigma = radius / 3;
			std::vector<double> weights(size_t(reach) * 2 + 1);
			double total = 0;
			for (int64_t offset = -reach; offset <= reach; ++offset)
				total += weights[size_t(offset + reach)] =
					std::exp(-double(offset * offset) / (2 * sigma * sigma));
			for (double &weight : weights)
				weight /= total;
			const uint32_t width = input.Width, height = input.Height;
			std::vector<float> premultiplied(size_t(width) * height * 4), across(premultiplied.size());
			for (uint32_t y = 0; y < height; ++y)
				for (uint32_t x = 0; x < width; ++x) {
					const Pixel pixel = detail::At(input, x, y);
					float *target = &premultiplied[(size_t(y) * width + x) * 4];
					for (size_t channel = 0; channel < 3; ++channel)
						target[channel] = static_cast<float>(pixel[channel] * pixel[3] / 255.0);
					target[3] = static_cast<float>(pixel[3]);
				}
			const auto pass = [&](const std::vector<float> &from, std::vector<float> &to, bool horizontal) {
				for (uint32_t y = 0; y < height; ++y)
					for (uint32_t x = 0; x < width; ++x) {
						std::array<double, 4> sum{};
						for (int64_t offset = -reach; offset <= reach; ++offset) {
							const int64_t sx = horizontal ? std::clamp<int64_t>(x + offset, 0, width - 1) : x;
							const int64_t sy =
								horizontal ? y : std::clamp<int64_t>(y + offset, 0, height - 1);
							const float *source = &from[(size_t(sy) * width + size_t(sx)) * 4];
							const double weight = weights[size_t(offset + reach)];
							for (size_t channel = 0; channel < 4; ++channel)
								sum[channel] += source[channel] * weight;
						}
						float *target = &to[(size_t(y) * width + x) * 4];
						for (size_t channel = 0; channel < 4; ++channel)
							target[channel] = static_cast<float>(sum[channel]);
					}
			};
			pass(premultiplied, across, true);
			pass(across, premultiplied, false);
			for (uint32_t y = 0; y < height; ++y)
				for (uint32_t x = 0; x < width; ++x) {
					const float *source = &premultiplied[(size_t(y) * width + x) * 4];
					Pixel pixel{};
					pixel[3] = source[3];
					if (source[3] > 0)
						for (size_t channel = 0; channel < 3; ++channel)
							pixel[channel] = source[channel] * 255.0 / source[3];
					detail::Put(output, x, y, pixel);
				}
		}
		bool MeshInto(
			const MeshAttribute &node, const MeshResolver &meshes, Buffer &output, Diagnostic &diagnostic
		) {
			if (!meshes) return EvaluationFailure(diagnostic, {}, "mesh resolver is unavailable");
			MeshAttributeData data;
			std::string failure;
			if (!meshes(node, data, failure))
				return EvaluationFailure(diagnostic, {}, failure.empty() ? "mesh is unavailable" : failure);
			std::vector<float> rgba;
			if (!RasteriseMeshAttribute(node, data, rgba, diagnostic)) return false;
			for (size_t index = 0; index < rgba.size(); ++index)
				detail::Store(output, index, rgba[index] * 255.0);
			return true;
		}
		// Every connected input already has storage; inputs[n] is null when absent.
		void Compose(
			const Operation &operation,
			const std::array<const Buffer *, Limits::MaximumInputs> &inputs,
			Buffer &output
		) {
			const Buffer *input = inputs[0];
			std::visit(
				[&](const auto &value) {
					using T = std::decay_t<decltype(value)>;
					if constexpr (std::is_same_v<T, Blur>) {
						BlurInto(*input, value.Radius, output);
						return;
					}
					double cosine = 1, sine = 0;
					if constexpr (std::is_same_v<T, Transform>) {
						const double radians =
							std::remainder(value.Degrees, 360.0) * std::numbers::pi / 180.0;
						cosine = std::cos(radians);
						sine = std::sin(radians);
					}
					for (uint32_t y = 0; y < output.Height; y++)
						for (uint32_t x = 0; x < output.Width; x++) {
							Pixel pixel{};
							if constexpr (std::is_same_v<T, Solid>) {
								for (size_t channel = 0; channel < 4; channel++)
									pixel[channel] = value.Colour[channel];
							} else if constexpr (std::is_same_v<T, Resize>) {
								pixel = detail::Sample(
									*input,
									(x + 0.5) * input->Width / value.Width,
									(y + 0.5) * input->Height / value.Height,
									value.Filter,
									true
								);
							} else if constexpr (std::is_same_v<T, Crop>) {
								pixel = detail::At(*input, int64_t(value.X) + x, int64_t(value.Y) + y);
							} else if constexpr (std::is_same_v<T, Transform>) {
								const double relativeX = x + 0.5 - value.PivotX - value.TranslateX;
								const double relativeY = y + 0.5 - value.PivotY - value.TranslateY;
								const double sourceX =
									value.PivotX + (cosine * relativeX + sine * relativeY) / value.ScaleX;
								const double sourceY =
									value.PivotY + (-sine * relativeX + cosine * relativeY) / value.ScaleY;
								pixel = detail::Sample(*input, sourceX, sourceY, value.Filter);
							} else if constexpr (std::is_same_v<T, Flip>) {
								pixel = detail::At(
									*input,
									value.Horizontal ? input->Width - 1 - x : x,
									value.Vertical ? input->Height - 1 - y : y
								);
							} else if constexpr (std::is_same_v<T, Blend>) {
								// Preparation admitted matching canvases.
								double opacity = value.Opacity;
								if (inputs[2]) opacity *= Unit255(detail::At(*inputs[2], x, y)[0]);
								pixel = Mixed(
									detail::At(*input, x, y),
									detail::At(*inputs[1], x, y),
									opacity,
									value.Mode
								);
							} else if constexpr (std::is_same_v<T, Noise>) {
								const auto [u, v] = Position(output, input, x, y);
								pixel = Grey(detail::NoiseAt(value, u * value.Scale, v * value.Scale));
							} else if constexpr (std::is_same_v<T, Voronoi>) {
								const auto [u, v] = Position(output, input, x, y);
								const auto cells = detail::Cells(
									u * value.Scale,
									v * value.Scale,
									value.Randomness,
									value.Seed,
									value.Feature == VoronoiFeature::Edge
								);
								switch (value.Feature) {
								case VoronoiFeature::F1:
									pixel = Grey(cells.F1);
									break;
								case VoronoiFeature::F2:
									pixel = Grey(cells.F2);
									break;
								case VoronoiFeature::F2MinusF1:
									pixel = Grey(cells.F2 - cells.F1);
									break;
								case VoronoiFeature::Edge:
									pixel = Grey(cells.Edge);
									break;
								case VoronoiFeature::Cell:
									pixel = Grey(cells.Cell);
									break;
								}
							} else if constexpr (std::is_same_v<T, Gradient>) {
								const auto [u, v] = Position(output, input, x, y);
								const double dx = value.EndX - value.StartX, dy = value.EndY - value.StartY;
								const double length = dx * dx + dy * dy;
								const double t =
									value.Type == GradientType::Linear
										? ((u - value.StartX) * dx + (v - value.StartY) * dy) / length
										: std::hypot(u - value.StartX, v - value.StartY) / std::sqrt(length);
								pixel = Grey(t);
							} else if constexpr (std::is_same_v<T, Wave>) {
								const auto [u, v] = Position(output, input, x, y);
								pixel = Grey(detail::WaveAt(value, u, v));
							} else if constexpr (std::is_same_v<T, Ramp>) {
								pixel = RampAt(value, Factor(detail::At(*input, x, y), value.Factor));
							} else if constexpr (std::is_same_v<T, Levels>) {
								pixel = detail::At(*input, x, y);
								for (size_t channel = 0; channel < 4; ++channel) {
									if (!(value.Channels & (1u << channel))) continue;
									double t = (Unit255(pixel[channel]) - value.InputMinimum) /
											   (value.InputMaximum - value.InputMinimum);
									if (value.Clamp) t = std::clamp(t, 0.0, 1.0);
									if (t > 0) t = std::pow(t, 1 / value.Gamma);
									pixel[channel] = (value.OutputMinimum +
													  t * (value.OutputMaximum - value.OutputMinimum)) *
													 255.0;
								}
							} else if constexpr (std::is_same_v<T, Math>) {
								pixel = detail::At(*input, x, y);
								const Pixel other = inputs[1] ? detail::At(*inputs[1], x, y) : Pixel{};
								for (size_t channel = 0; channel < 4; ++channel) {
									if (!(value.Channels & (1u << channel))) continue;
									const double b =
										inputs[1] ? Unit255(other[channel]) : value.Value[channel];
									pixel[channel] =
										Arithmetic(value.Operation, Unit255(pixel[channel]), b) * 255.0;
								}
							} else if constexpr (std::is_same_v<T, Channels>) {
								std::array<Pixel, Limits::MaximumInputs> read{};
								for (size_t index = 0; index < inputs.size(); ++index)
									if (inputs[index]) read[index] = detail::At(*inputs[index], x, y);
								for (size_t channel = 0; channel < 4; ++channel) {
									const auto &pick = value.Picks[channel];
									pixel[channel] = pick.Input == -1 ? 0.0
													 : pick.Input == -2
														 ? 255.0
														 : read[size_t(pick.Input)][pick.Channel];
								}
							} else if constexpr (std::is_same_v<T, Invert>) {
								pixel = detail::At(*input, x, y);
								for (size_t channel = 0; channel < 4; ++channel)
									if (value.Channels & (1u << channel))
										pixel[channel] = 255.0 - pixel[channel];
							} else if constexpr (std::is_same_v<T, NormalMap>) {
								const auto height = [&](int64_t sx, int64_t sy) {
									return Factor(detail::At(*input, sx, sy, true), value.Height);
								};
								const double dx = (height(int64_t(x) + 1, y) - height(int64_t(x) - 1, y)) *
												  0.5 * value.Strength;
								const double dy = (height(x, int64_t(y) + 1) - height(x, int64_t(y) - 1)) *
												  0.5 * value.Strength;
								// Image rows run down while tangent-space +Y runs up.
								std::array<double, 3> normal{-dx, value.FlipY ? -dy : dy, 1};
								const double length = std::sqrt(
									normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]
								);
								for (size_t channel = 0; channel < 3; ++channel)
									pixel[channel] = (normal[channel] / length * 0.5 + 0.5) * 255.0;
								pixel[3] = 255.0;
							} else if constexpr (std::is_same_v<T, Warp>) {
								const Pixel vector = detail::At(*inputs[1], x, y);
								const double sx = std::clamp(
									x + 0.5 + (Unit255(vector[0]) * 2 - 1) * value.Strength,
									0.5,
									input->Width - 0.5
								);
								const double sy = std::clamp(
									y + 0.5 + (Unit255(vector[1]) * 2 - 1) * value.Strength,
									0.5,
									input->Height - 0.5
								);
								pixel = detail::Sample(*input, sx, sy, value.Filter, true);
							} else if constexpr (std::is_same_v<T, Lookup>) {
								const Pixel vector = detail::At(*inputs[1], x, y);
								if (vector[3] > 0)
									pixel = detail::Lookup(
										*input,
										Unit255(vector[0]) * value.ScaleX + value.OffsetX,
										Unit255(vector[1]) * value.ScaleY + value.OffsetY,
										value
									);
							}
							detail::Put(output, x, y, pixel);
						}
				},
				operation
			);
		}
		Image Publish(const Buffer &buffer, OutputFormat format, OutputSpace space) {
			Image image{buffer.Width, buffer.Height, {}, space, format};
			const size_t pixels = size_t(buffer.Width) * buffer.Height;
			image.Pixels.resize(pixels * BytesPerPixel(format));
			const auto half = [&](size_t target, uint16_t value) {
				const uint16_t bits = core::EncodeFloat16(static_cast<float>(value / 65535.0));
				image.Pixels[target] = std::byte(bits & 0xff);
				image.Pixels[target + 1] = std::byte(bits >> 8);
			};
			for (size_t pixel = 0; pixel < pixels; ++pixel) {
				const uint16_t *source = &buffer.Data[pixel * 4];
				switch (format) {
				case OutputFormat::RGBA8:
					for (size_t channel = 0; channel < 4; ++channel)
						image.Pixels[pixel * 4 + channel] = std::byte(
							static_cast<uint8_t>((uint32_t(source[channel]) * 255 + 32767) / 65535)
						);
					break;
				case OutputFormat::R16F:
					half(pixel * 2, source[0]);
					break;
				case OutputFormat::RGBA16F:
					for (size_t channel = 0; channel < 4; ++channel)
						half(pixel * 8 + channel * 2, source[channel]);
					break;
				}
			}
			return image;
		}
	}

	bool EvaluateWith(
		const Document &authored,
		const Plan &plan,
		std::string_view output,
		const Resolvers &resolvers,
		Image &out,
		Diagnostic &diagnostic
	) {
		ENGINE_PROFILE("imagegraph 2d evaluation");
		Document resolved;
		const Document *document = &authored;
		if (!authored.Parameters.empty() || !authored.Bindings.empty()) {
			if (!ResolveInputs(authored, {}, resolved, diagnostic)) return false;
			document = &resolved;
		}
		Plan checked;
		if (!Compile(*document, checked, diagnostic)) return false;
		if (checked != plan)
			return EvaluationFailure(diagnostic, {}, "compile plan does not match the document");
		size_t selected = 0;
		if (output.empty()) {
			if (document->Outputs.size() != 1)
				return EvaluationFailure(diagnostic, {}, "select one named output explicitly");
		} else {
			const auto found =
				std::find_if(document->Outputs.begin(), document->Outputs.end(), [&](const Output &binding) {
					return binding.Name == output;
				});
			if (found == document->Outputs.end())
				return EvaluationFailure(diagnostic, {}, "selected output does not exist");
			selected = static_cast<size_t>(found - document->Outputs.begin());
		}
		const bool wide = document->Storage == Precision::Wide;
		std::string_view activeNode;
		try {
			const size_t target = plan.Outputs[selected];
			std::vector<uint8_t> needed(document->Nodes.size());
			needed[target] = 1;
			for (auto node = plan.Order.rbegin(); node != plan.Order.rend(); ++node)
				if (needed[*node])
					for (size_t input : plan.Inputs[*node])
						needed[input] = 1;
			if (!Preflight(*document, plan, needed, target, diagnostic)) return false;
			std::vector<std::optional<Buffer>> results(document->Nodes.size());
			std::vector<SourceExtent> extents;
			size_t retainedSources = 0;
			// Decode only reachable sources first. Exact metadata then admits the whole
			// execution before allocating any derived pixel buffer, just as GPU hosts do.
			for (size_t index : plan.Order) {
				if (!needed[index]) continue;
				const auto &node = document->Nodes[index];
				const auto *source = std::get_if<Source>(&node.Value);
				if (!source) continue;
				activeNode = node.Id;
				if (!resolvers.Sources)
					return EvaluationFailure(diagnostic, node.Id, "source resolver is unavailable");
				if (retainedSources > Limits::MaximumRetainedBytes - Limits::MaximumImageBytes)
					return EvaluationFailure(
						diagnostic, node.Id, "source reservation exceeds retained image budget"
					);
				Image image;
				std::string failure;
				bool sourceResolved;
				{
					ENGINE_PROFILE("imagegraph source resolution");
					sourceResolved = resolvers.Sources(*source, image, failure);
				}
				if (!sourceResolved)
					return EvaluationFailure(
						diagnostic, node.Id, failure.empty() ? "source is unavailable" : failure
					);
				if (!image.IsValid() || image.Format != OutputFormat::RGBA8)
					return EvaluationFailure(
						diagnostic, node.Id, "source dimensions and RGBA8 bytes are inconsistent"
					);
				retainedSources += image.Pixels.size();
				extents.push_back({node.Id, image.Width, image.Height});
				core::Metrics::Count("imagegraph.source_decoded_bytes", image.Pixels.size());
				Buffer buffer{image.Width, image.Height, wide, {}};
				buffer.Data.resize(image.Pixels.size());
				for (size_t offset = 0; offset < image.Pixels.size(); ++offset)
					buffer.Data[offset] =
						static_cast<uint16_t>(std::to_integer<uint8_t>(image.Pixels[offset]) * 257);
				results[index] = std::move(buffer);
			}
			ExecutionPlan execution;
			if (!Prepare(*document, plan, output, extents, execution, diagnostic)) return false;
			// The needed nodes run as an ang graph: one definition per document
			// node whose Evaluate is that node's kernel, image buffers as blobs and
			// the plan's peak as the runner budget, so consumed intermediates are
			// evicted as the run goes.
			ang::Registry types;
			types.DefineType({.Id = "image", .Bytes = [](const ang::Value &value) {
								  const ang::Blob *blob = value.Get<ang::Blob>();
								  return blob == nullptr ? size_t{0} : blob->Bytes;
							  }});
			ang::Graph graph;
			Diagnostic kernelFailure;
			const std::vector<size_t> &members = execution.Order;
			for (const size_t index : members) {
				const auto &node = document->Nodes[index];
				ang::NodeDef def{
					.Id = "imagegraph/" + node.Id, .Outputs = {{.Name = "image", .Type = "image"}}
				};
				for (size_t slot = 0; slot < plan.Inputs[index].size(); ++slot) {
					def.Inputs.push_back({.Name = "in" + std::to_string(slot), .Type = "image"});
				}
				// Runs after the previous node in the order, which makes every wave one
				// node: breadth-first waves would hold every branch's images at once.
				def.Inputs.push_back({.Name = "after", .Type = "image", .Optional = true});
				// Each node's identity in the blob is its operation and position in
				// the plan, which is all a pure kernel's output depends on here.
				const uint64_t identity = ang::HashMix(Fingerprint(node.Value), index) | 1u;
				def.Evaluate = [&, index, identity](ang::EvalContext &context) {
					const auto publish = [&](std::shared_ptr<Buffer> buffer) {
						const size_t bytes = buffer->Data.size() * sizeof(uint16_t);
						context.SetOutput(
							"image",
							ang::Value::FromBlob(
								"image", {.Data = buffer, .Bytes = bytes, .Identity = identity}
							)
						);
						return ang::NodeResult::Success();
					};
					if (results[index]) {
						return publish(std::make_shared<Buffer>(std::move(*results[index])));
					}
					const auto &extent = execution.Extents[index];
					auto buffer = std::make_shared<Buffer>(Buffer{extent.Width, extent.Height, wide, {}});
					{
						ENGINE_PROFILE("imagegraph pixel allocation");
						buffer->Data.resize(size_t(extent.Width) * extent.Height * 4);
						core::Metrics::Count("imagegraph.allocated_pixel_bytes", extent.Bytes);
					}
					const auto &value = document->Nodes[index].Value;
					if (const auto *mesh = std::get_if<MeshAttribute>(&value)) {
						if (!MeshInto(*mesh, resolvers.Meshes, *buffer, kernelFailure)) {
							return ang::NodeResult::Failure(kernelFailure.Message);
						}
					} else {
						std::array<const Buffer *, Limits::MaximumInputs> inputs{};
						for (size_t slot = 0; slot < plan.Inputs[index].size(); ++slot) {
							const ang::Blob *blob =
								context.Input("in" + std::to_string(slot)).Get<ang::Blob>();
							if (blob == nullptr) {
								return ang::NodeResult::Failure("input image is missing");
							}
							inputs[slot] = static_cast<const Buffer *>(blob->Data.get());
						}
						ENGINE_PROFILE("imagegraph node compose");
						Compose(value, inputs, *buffer);
					}
					core::Metrics::Count("imagegraph.evaluated_nodes", 1);
					return publish(std::move(buffer));
				};
				types.DefineNode(std::move(def));
				graph.Apply(ang::edit::AddNode{{.Id = node.Id, .Def = "imagegraph/" + node.Id}});
			}
			for (const size_t index : members) {
				for (size_t slot = 0; slot < plan.Inputs[index].size(); ++slot) {
					graph.Apply(
						ang::edit::AddLink{
							{document->Nodes[plan.Inputs[index][slot]].Id,
							 "image",
							 document->Nodes[index].Id,
							 "in" + std::to_string(slot)}
						}
					);
				}
			}
			for (size_t position = 1; position < members.size(); ++position)
				graph.Apply(
					ang::edit::AddLink{
						{document->Nodes[members[position - 1]].Id,
						 "image",
						 document->Nodes[members[position]].Id,
						 "after"}
					}
				);
			ang::Plan compiled;
			const ang::CompileResult compiledResult =
				ang::Compile(graph, types, {.Runs = ang::Domain::Tool}, compiled);
			if (!compiledResult.Ok)
				return EvaluationFailure(diagnostic, {}, compiledResult.Diagnostics.front().Message);
			// Host buffers hold sixteen bits per channel at either precision.
			const size_t budget = execution.LiveBytes * (wide ? 1 : 2);
			ang::Runner runner(std::move(compiled), {.BudgetBytes = budget});
			const ang::RunReport report = runner.Run();
			for (size_t index = 0; index < report.Nodes.size(); ++index) {
				const ang::NodeReport &entry = report.Nodes[index];
				if (entry.Status == ang::NodeStatus::Ran || entry.Status == ang::NodeStatus::Cached) continue;
				const std::string &id = runner.CurrentPlan().Nodes[index].Id;
				if (!kernelFailure.Message.empty())
					return EvaluationFailure(diagnostic, id, kernelFailure.Message);
				return EvaluationFailure(
					diagnostic, id, entry.Note.empty() ? ang::Describe(entry.Status) : entry.Note
				);
			}
			const ang::Value *final = runner.Output(document->Nodes[target].Id, "image");
			if (final == nullptr)
				return EvaluationFailure(diagnostic, document->Nodes[target].Id, "output image is missing");
			results[target] = *static_cast<const Buffer *>(final->Get<ang::Blob>()->Data.get());
			const auto &selection = document->Outputs[selected];
			out = Publish(*results[target], selection.Format, selection.Space);
			diagnostic = {};
			return true;
		} catch (const std::exception &error) {
			return EvaluationFailure(diagnostic, activeNode, error.what());
		}
	}
	bool EvaluateTyped(
		const Document &document,
		const Plan &plan,
		std::string_view output,
		const TypedSourceResolver &sources,
		Image &out,
		Diagnostic &diagnostic
	) {
		return EvaluateWith(document, plan, output, {sources, {}}, out, diagnostic);
	}
	bool Evaluate(
		const Document &document,
		const Plan &plan,
		std::string_view output,
		const SourceResolver &sources,
		Image &out,
		Diagnostic &diagnostic
	) {
		TypedSourceResolver typed;
		if (sources)
			typed = [&](const Source &source, Image &image, std::string &failure) {
				if (source.Interpretation != SourceInterpretation::Colour) {
					failure = "data sources require a typed source resolver";
					return false;
				}
				return sources(source.Path, image, failure);
			};
		return EvaluateTyped(document, plan, output, typed, out, diagnostic);
	}
}
