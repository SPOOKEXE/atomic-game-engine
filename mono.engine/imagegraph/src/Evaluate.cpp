#include "Pixels.hpp"

#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/imagegraph/Document.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <optional>

namespace engine::imagegraph {
	namespace {
		bool EvaluationFailure(Diagnostic &diagnostic, std::string_view node, std::string_view message) {
			diagnostic = {std::string(node.substr(0, 128)), std::string(message.substr(0, 4096))};
			return false;
		}
		std::pair<uint32_t, uint32_t> Extent(const Operation &operation, const Image *input) {
			return std::visit(
				[&](const auto &value) -> std::pair<uint32_t, uint32_t> {
					using T = std::decay_t<decltype(value)>;
					if constexpr (std::is_same_v<T, Solid> || std::is_same_v<T, Resize> ||
								  std::is_same_v<T, Crop> || std::is_same_v<T, Transform>)
						return {value.Width, value.Height};
					else
						return {input->Width, input->Height};
				},
				operation
			);
		}
		uint64_t WorkPerPixel(const Operation &operation) {
			if (std::holds_alternative<Blend>(operation)) return 2;
			if (const auto *resize = std::get_if<Resize>(&operation))
				return resize->Filter == Sampling::Bilinear ? 4 : 1;
			if (const auto *transform = std::get_if<Transform>(&operation))
				return transform->Filter == Sampling::Bilinear ? 4 : 1;
			return 1;
		}
		// Count declared work and retained results before any resolver or pixel allocation.
		// Source extents remain unknown until decoded and are checked by the execution gate.
		bool Preflight(
			const Document &document,
			const Plan &plan,
			const std::vector<uint8_t> &needed,
			Diagnostic &diagnostic
		) {
			std::vector<std::pair<uint32_t, uint32_t>> extents(document.Nodes.size());
			size_t retained = 0;
			uint64_t work = 0;
			for (size_t index : plan.Order) {
				if (!needed[index] || std::holds_alternative<Source>(document.Nodes[index].Value)) continue;
				Image input;
				if (!plan.Inputs[index].empty()) {
					const auto [width, height] = extents[plan.Inputs[index][0]];
					input.Width = width;
					input.Height = height;
				}
				const auto extent = Extent(document.Nodes[index].Value, &input);
				extents[index] = extent;
				const auto [width, height] = extent;
				const size_t bytes = static_cast<size_t>(width) * height * 4;
				const uint64_t pixels = uint64_t(width) * height * WorkPerPixel(document.Nodes[index].Value);
				if (bytes > Limits::MaximumRetainedBytes - retained)
					return EvaluationFailure(
						diagnostic, document.Nodes[index].Id, "retained image budget exceeded"
					);
				if (pixels > Limits::MaximumPixelWork - work)
					return EvaluationFailure(
						diagnostic, document.Nodes[index].Id, "pixel work budget exceeded"
					);
				retained += bytes;
				work += pixels;
			}
			return true;
		}
		void Compose(const Operation &operation, const Image *input, const Image *foreground, Image &output) {
			std::visit(
				[&](const auto &value) {
					using T = std::decay_t<decltype(value)>;
					double cosine = 1, sine = 0;
					if constexpr (std::is_same_v<T, Transform>) {
						const double radians =
							std::remainder(value.Degrees, 360.0) * std::numbers::pi / 180.0;
						cosine = std::cos(radians);
						sine = std::sin(radians);
					}
					for (uint32_t y = 0; y < output.Height; y++)
						for (uint32_t x = 0; x < output.Width; x++) {
							detail::Pixel pixel{};
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
								pixel = detail::Over(
									detail::At(*input, x, y), detail::At(*foreground, x, y), value.Opacity
								);
							}
							detail::Put(output, x, y, pixel);
						}
				},
				operation
			);
		}
	}
	bool Evaluate(
		const Document &document,
		const Plan &plan,
		std::string_view output,
		const SourceResolver &sources,
		Image &out,
		Diagnostic &diagnostic
	) {
		ENGINE_PROFILE("imagegraph 2d evaluation");
		Plan checked;
		if (!Compile(document, checked, diagnostic)) return false;
		if (checked != plan)
			return EvaluationFailure(diagnostic, {}, "compile plan does not match the document");
		size_t selected = 0;
		if (output.empty()) {
			if (document.Outputs.size() != 1)
				return EvaluationFailure(diagnostic, {}, "select one named output explicitly");
		} else {
			const auto found =
				std::find_if(document.Outputs.begin(), document.Outputs.end(), [&](const Output &binding) {
					return binding.Name == output;
				});
			if (found == document.Outputs.end())
				return EvaluationFailure(diagnostic, {}, "selected output does not exist");
			selected = static_cast<size_t>(found - document.Outputs.begin());
		}
		std::string_view activeNode;
		try {
			const size_t target = plan.Outputs[selected];
			std::vector<uint8_t> needed(document.Nodes.size());
			needed[target] = 1;
			for (auto node = plan.Order.rbegin(); node != plan.Order.rend(); ++node)
				if (needed[*node])
					for (size_t input : plan.Inputs[*node])
						needed[input] = 1;
			if (!Preflight(document, plan, needed, diagnostic)) return false;
			std::vector<std::optional<Image>> results(document.Nodes.size());
			size_t retained = 0;
			uint64_t work = 0;
			for (size_t index : plan.Order) {
				if (!needed[index]) continue;
				const Node &node = document.Nodes[index];
				activeNode = node.Id;
				Image image;
				if (const auto *source = std::get_if<Source>(&node.Value)) {
					if (!sources)
						return EvaluationFailure(diagnostic, node.Id, "source resolver is unavailable");
					// Reserve the resolver's complete bounded allowance before it may allocate.
					if (retained > Limits::MaximumRetainedBytes - Limits::MaximumImageBytes)
						return EvaluationFailure(
							diagnostic, node.Id, "source reservation exceeds retained image budget"
						);
					std::string failure;
					if (!sources(source->Path, image, failure))
						return EvaluationFailure(
							diagnostic, node.Id, failure.empty() ? "source is unavailable" : failure
						);
					if (!image.IsValid())
						return EvaluationFailure(
							diagnostic, node.Id, "source dimensions and RGBA8 bytes are inconsistent"
						);
					core::Metrics::Count("imagegraph.source_decoded_bytes", image.Pixels.size());
				} else {
					const Image *input =
						plan.Inputs[index].empty() ? nullptr : &*results[plan.Inputs[index][0]];
					const Image *foreground =
						plan.Inputs[index].size() == 2 ? &*results[plan.Inputs[index][1]] : nullptr;
					if (foreground &&
						(input->Width != foreground->Width || input->Height != foreground->Height))
						return EvaluationFailure(
							diagnostic, node.Id, "blend inputs require matching canvas dimensions"
						);
					const auto [width, height] = Extent(node.Value, input);
					const size_t bytes = static_cast<size_t>(width) * height * 4;
					if (bytes > Limits::MaximumRetainedBytes - retained)
						return EvaluationFailure(diagnostic, node.Id, "retained image budget exceeded");
					const uint64_t pixels = uint64_t(width) * height * WorkPerPixel(node.Value);
					if (pixels > Limits::MaximumPixelWork - work)
						return EvaluationFailure(diagnostic, node.Id, "pixel work budget exceeded");
					image.Width = width;
					image.Height = height;
					image.Pixels.resize(bytes);
					core::Metrics::Count("imagegraph.allocated_pixel_bytes", bytes);
					Compose(node.Value, input, foreground, image);
				}
				const uint64_t pixels = uint64_t(image.Width) * image.Height * WorkPerPixel(node.Value);
				if (pixels > Limits::MaximumPixelWork - work)
					return EvaluationFailure(diagnostic, node.Id, "pixel work budget exceeded");
				work += pixels;
				retained += image.Pixels.size();
				results[index] = std::move(image);
				core::Metrics::Count("imagegraph.evaluated_nodes", 1);
			}
			out = std::move(*results[target]);
			diagnostic = {};
			return true;
		} catch (const std::exception &error) {
			return EvaluationFailure(diagnostic, activeNode, error.what());
		}
	}
}
