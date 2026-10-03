#include "../SourceMappedInputs.hpp"
#include "Source2DComplexGenerator.hpp"

#include <numbers>

namespace engine::imagegraph::detail {
	namespace {
		struct GaborControl {
			Vector2 Range{};
			const Image *Map = nullptr;
		};
		bool ReadGaborControl(NodeContext &c, std::string_view port, double fallback, GaborControl &control) {
			if (SourceRangeMapped(c, port)) {
				if (!ReadSourceMappedRange(c, port, control.Range)) return false;
				control.Map = c.Input(std::string(port) + "_map");
			} else {
				const double value = c.Scalar(port, fallback);
				control.Range = {value, value};
			}
			return c.FailureCode == Status::Ok;
		}
		double GaborSample(const GaborControl &control, double u, double v) {
			if (!control.Map) return control.Range.X;
			const auto pixel = SampleNearest(*control.Map, u, v);
			return control.Range.X +
				   (control.Range.Y - control.Range.X) * (pixel[0] + pixel[1] + pixel[2]) / 3;
		}
		double GaborFract(double x) {
			return x - std::floor(x);
		}
		Vector2 GaborHash(Vector2 p, double seed) {
			const double reduced = seed - 10000 * std::floor(seed / 10000);
			return {
				GaborFract(
					std::sin((p.X * 127.1324 + p.Y * 311.7874) * (152.6178612 + reduced / 100)) * 43758.5453
				),
				GaborFract(
					std::sin((p.X * 269.8355 + p.Y * 183.3961) * (437.5453123 + reduced / 100)) * 43758.5453
				)
			};
		}
		bool GaborWave(
			NodeContext &c,
			Vector2 p,
			Vector2 augment,
			double seed,
			double density,
			double sharpness,
			double phase,
			double &value
		) {
			if (!std::isfinite(p.X) || !std::isfinite(p.Y))
				return c.Fail(
					Status::UnsupportedExecution, "Gabor coordinates exceed native arithmetic range", "scale"
				);
			const Vector2 integer{std::floor(p.X), std::floor(p.Y)},
				fraction{GaborFract(p.X), GaborFract(p.Y)};
			double numerator = 0, denominator = 0;
			const double frequency = density * 6.283185;
			for (int j = -2; j <= 2; ++j)
				for (int i = -2; i <= 2; ++i) {
					const Vector2 cell{integer.X + i, integer.Y + j};
					const auto hash = GaborHash(cell, seed);
					const Vector2 r{fraction.X - (i + hash.X), fraction.Y - (j + hash.Y)};
					const auto direction = GaborHash({cell.X + augment.X, cell.Y + augment.Y}, seed);
					const Vector2 k{-1 + 2 * direction.X, -1 + 2 * direction.Y};
					const double length = std::sqrt(k.X * k.X + k.Y * k.Y);
					if (!(length > 0) || !std::isfinite(length))
						return c.Fail(
							Status::UnsupportedExecution,
							"Gabor direction normalization is undefined",
							"augment"
						);
					const double distance = r.X * r.X + r.Y * r.Y;
					const double projection = (r.X * k.X + r.Y * k.Y) / length + phase;
					const double weight = std::exp(-sharpness * distance);
					const double wave = std::cos(frequency * projection + phase);
					numerator += weight * wave;
					denominator += weight;
				}
			// Only gabor_wave(...).x is consumed; derivative components are source-inert.
			if (!(denominator > 0) || !std::isfinite(denominator) || !std::isfinite(numerator))
				return c.Fail(
					Status::UnsupportedExecution, "Gabor weighted normalization is undefined", "sharpness"
				);
			value = numerator / denominator;
			return std::isfinite(value) ||
				   c.Fail(Status::UnsupportedExecution, "Gabor normalized sample is nonfinite", "sharpness");
		}
	}
	bool SourceGaborNoise(NodeContext &c) {
		ENGINE_PROFILE("imagegraph.source.gabor_noise");
		const auto format = ResolveProcessorSurfaceFormat(c, nullptr);
		if (!format ||
			!source2d::ComplexBatchAdmission(c, source2d::GABOR_PIXEL_WORK, "surface_out", "dimension"))
			return false;
		source2d::ComplexCanvas canvas;
		if (!source2d::ResolveComplexCanvas(c, canvas)) return false;
		if (!c.Find("seed"))
			return c.Fail(Status::UnsupportedExecution, "Gabor requires an authored source seed", "seed");
		const double seed = c.Scalar("seed");
		const auto augment = c.Vec2("augment", {11, 31}), levelIn = c.Vec2("level_in", {0, 1}),
				   levelOut = c.Vec2("level_out", {0, 1});
		if (levelIn.X == levelIn.Y)
			return c.Fail(Status::UnsupportedExecution, "Gabor Level In divides by zero", "level_in");
		Vector2 position;
		if (!source2d::ReferenceVector(c, "position", canvas.Raw, position)) return false;
		Vector2 scale;
		if (SourceRangeMapped(c, "scale")) {
			if (!ReadSourceMappedRange(c, "scale", scale)) return false;
		} else
			scale = c.Vec2("scale", {4, 4});
		// The shader computes mapped sca but transforms with uniform scale; its sampler has no effect.
		GaborControl density, sharpness, phase;
		if (!ReadGaborControl(c, "density", 2, density) || !ReadGaborControl(c, "sharpness", 4, sharpness) ||
			!ReadGaborControl(c, "phase", 0, phase))
			return false;
		const double rotation = c.Scalar("rotation") * std::numbers::pi / 180;
		const double cosine = std::cos(rotation), sine = std::sin(rotation);
		Image *output = c.NewImage("surface_out", canvas.Width, canvas.Height, *format);
		if (!output) return false;
		for (uint32_t y = 0; y < canvas.Height; ++y)
			for (uint32_t x = 0; x < canvas.Width; ++x) {
				const double u = (x + .5) / canvas.Width, v = (y + .5) / canvas.Height;
				double alpha = 1;
				const auto uv = source2d::GeneratorUv(c, u, v, alpha);
				const Vector2 translated{
					uv.X * (canvas.Raw.X / canvas.Raw.Y) - position.X / canvas.Raw.X,
					uv.Y - position.Y / canvas.Raw.Y
				};
				const Vector2 p{
					(translated.X * cosine - translated.Y * sine) * scale.X,
					(translated.X * sine + translated.Y * cosine) * scale.Y
				};
				double wave = 0;
				if (!GaborWave(
						c,
						p,
						augment,
						seed,
						GaborSample(density, u, v),
						GaborSample(sharpness, u, v),
						GaborSample(phase, u, v) * std::numbers::pi / 180,
						wave
					))
					return false;
				const double level =
					levelOut.X + (levelOut.Y - levelOut.X) * (wave - levelIn.X) / (levelIn.Y - levelIn.X);
				const double value = .5 + .5 * level;
				if (!source2d::StoreComplexPixel(
						c, *output, x, y, {value, value, value, alpha}, "surface_out"
					))
					return false;
			}
		return c.FailureCode == Status::Ok;
	}
}
