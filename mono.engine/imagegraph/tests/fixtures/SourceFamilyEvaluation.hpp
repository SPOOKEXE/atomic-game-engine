#pragma once

// Fixed headless workloads with analytical expected outputs. No external source parity is claimed.
#include "AudioWindowObservation.hpp"

#include <engine/imagegraph/Document.hpp>
#include <engine/imagegraph/WavPreview.hpp>
#include <engine/imagegraph/WavTimelinePresentation.hpp>

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <locale>
#include <map>
#include <memory>
#include <numbers>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace engine::imagegraph::testing {
	inline uint64_t HashBytes(uint64_t hash, uint64_t word, size_t bytes) {
		for (size_t index = 0; index < bytes; index++) {
			hash ^= (word >> (8 * index)) & 255;
			hash *= 1099511628211ULL;
		}
		return hash;
	}

	struct SourceFamilyFixture {
		enum class Family {
			Audio,
			Gradient,
			Solid,
			Matrix,
			WavControls,
			ScalarMath,
			Curve,
			Vector,
			HdrDirectional,
			HdrZoom,
			CubeSmall,
			CubeLarge,
			WavTimeline,
			CylinderSmall,
			CylinderProfile,
			ConeDefault8,
			ConeBounded104,
			TorusDefault16x8,
			TorusSmooth31x22,
			UVSphereDefault8x16,
			UVSphereSmooth31x22,
			AudioWindow4096,
			AudioWindow65536,
			IcosphereDefault1,
			IcosphereSmooth3
		};
		static_assert(static_cast<int>(Family::CylinderProfile) == 14);
		static_assert(static_cast<int>(Family::ConeDefault8) == 15);
		static_assert(static_cast<int>(Family::ConeBounded104) == 16);
		static_assert(static_cast<int>(Family::TorusDefault16x8) == 17);
		static_assert(static_cast<int>(Family::TorusSmooth31x22) == 18);
		static_assert(static_cast<int>(Family::UVSphereDefault8x16) == 19);
		static_assert(static_cast<int>(Family::UVSphereSmooth31x22) == 20);
		static_assert(static_cast<int>(Family::AudioWindow4096) == 21);
		static_assert(static_cast<int>(Family::AudioWindow65536) == 22);
		std::unique_ptr<AudioWindowObservationFixture> AudioObservation;
		std::array<AudioWindowPresentation, 4> AudioWindows;
		static_assert(static_cast<int>(Family::IcosphereDefault1) == 23);
		static_assert(static_cast<int>(Family::IcosphereSmooth3) == 24);
		Family Kind;
		Document Authored;
		Plan Compiled;
		std::vector<AudioCaptureFrame> Captures;
		std::vector<AudioClipSource> Clips;
		std::vector<RequestImageSource> ImageSources;
		Image HdrOutput;
		WavPreviewControls Controls;
		std::array<WavTimelinePresentation, 3> Waveforms;
		Diagnostic Failure;
		EvaluatedValue ValueOutput;
		EvaluatedValue ControlOutput;
		EvaluatedValue CreatorOutput;
		ImageArray ImageOutput;
		uint64_t InputHash = 14695981039346656037ULL;
		uint64_t OutputHash = 0;

		explicit SourceFamilyFixture(Family family) : Kind(family) {
			if (IsAudioWindow()) {
				AudioObservation = std::make_unique<AudioWindowObservationFixture>(
					family == Family::AudioWindow4096 ? 4096 : 65536
				);
				InputHash = AudioObservation->InputHash;
				Evaluate();
				OutputHash = Verify();
				Evaluate();
				if (Verify() != OutputHash) Fail("audio observation replay");
				return;
			}
			Authored.FormatVersion = 7;
			if (IsIcosphere()) {
				Authored.FormatVersion = 9;
				Authored.Nodes = {
					{"material", "pc.3_d_material", "", {}, {{"diffuse", .25}}},
					{"sphere",
					 "pc.3_d_mesh_sphere_ico",
					 "",
					 {},
					 {{"position", Vector3{2, 3, 4}},
					  {"anchor", Vector3{.1, .2, .3}},
					  {"rotation", Quaternion{.1, .2, .3, .4}},
					  {"scale", Vector3{2, -3, .5}}}},
					{"transform",
					 "pc.3_d_transform",
					 "",
					 {},
					 {{"position", Vector3{-1, 2, 3}},
					  {"anchor", Vector3{4, 5, 6}},
					  {"rotation", Quaternion{0, 0, 1, 0}},
					  {"scale", Vector3{-1, 2, 4}}}},
					{"get", "pc.3_d_get_data", "", {}, {}}
				};
				if (family == Family::IcosphereSmooth3) {
					Authored.Nodes[1].Values.insert(
						Authored.Nodes[1].Values.end(), {{"subdivision", int64_t{3}}, {"smooth_normal", true}}
					);
				}
				Authored.Links = {
					{"material", "material", "sphere", "material"},
					{"sphere", "mesh", "transform", "mesh"},
					{"transform", "mesh", "get", "mesh"}
				};
				Authored.Outputs = {
					{"output", "transform", "mesh"},
					{"control", "get", "position"},
					{"creator", "material", "material"}
				};
			} else if (IsUVSphere()) {
				Authored.FormatVersion = 9;
				Authored.Nodes = {
					{"material", "pc.3_d_material", "", {}, {{"diffuse", .25}}},
					{"sphere",
					 "pc.3_d_mesh_sphere_uv",
					 "",
					 {},
					 {{"position", Vector3{2, 3, 4}},
					  {"anchor", Vector3{.1, .2, .3}},
					  {"rotation", Quaternion{.1, .2, .3, .4}},
					  {"scale", Vector3{2, -3, .5}}}},
					{"transform",
					 "pc.3_d_transform",
					 "",
					 {},
					 {{"position", Vector3{-1, 2, 3}},
					  {"anchor", Vector3{4, 5, 6}},
					  {"rotation", Quaternion{0, 0, 1, 0}},
					  {"scale", Vector3{-1, 2, 4}}}},
					{"get", "pc.3_d_get_data", "", {}, {}}
				};
				if (family == Family::UVSphereSmooth31x22) {
					// 682 source cells retain 4,092 ordered vertices and 1,364 duplicated cell edges.
					Authored.Nodes[1].Values.insert(
						Authored.Nodes[1].Values.end(),
						{{"horizontal_slices", int64_t{31}},
						 {"vertical_slices", int64_t{22}},
						 {"smooth_normal", true},
						 {"projection", EnumValue{1}}}
					);
				}
				Authored.Links = {
					{"material", "material", "sphere", "material"},
					{"sphere", "mesh", "transform", "mesh"},
					{"transform", "mesh", "get", "mesh"}
				};
				Authored.Outputs = {
					{"output", "transform", "mesh"},
					{"control", "get", "position"},
					{"creator", "material", "material"}
				};
			} else if (IsTorus()) {
				Authored.FormatVersion = 9;
				Authored.Nodes = {
					{"material", "pc.3_d_material", "", {}, {{"diffuse", .25}}},
					{"torus",
					 "pc.3_d_mesh_torus",
					 "",
					 {},
					 {{"position", Vector3{2, 3, 4}},
					  {"anchor", Vector3{.1, .2, .3}},
					  {"rotation", Quaternion{.1, .2, .3, .4}},
					  {"scale", Vector3{2, -3, .5}}}},
					{"transform",
					 "pc.3_d_transform",
					 "",
					 {},
					 {{"position", Vector3{-1, 2, 3}},
					  {"anchor", Vector3{4, 5, 6}},
					  {"rotation", Quaternion{0, 0, 1, 0}},
					  {"scale", Vector3{-1, 2, 4}}}},
					{"get", "pc.3_d_get_data", "", {}, {}}
				};
				if (family == Family::TorusSmooth31x22) {
					// 682 cells yields 4,092 vertices and 2,728 duplicated cell edges.
					Authored.Nodes[1].Values.insert(
						Authored.Nodes[1].Values.end(),
						{{"toroidal_slices", int64_t{31}},
						 {"poloidal_slices", int64_t{22}},
						 {"smooth_normal", true},
						 {"toroidal_angle", .5},
						 {"poloidal_angle", 13.0},
						 {"twist", .25}}
					);
				}
				Authored.Links = {
					{"material", "material", "torus", "material"},
					{"torus", "mesh", "transform", "mesh"},
					{"transform", "mesh", "get", "mesh"}
				};
				Authored.Outputs = {
					{"output", "transform", "mesh"},
					{"control", "get", "position"},
					{"creator", "material", "material"}
				};
			} else if (IsCone()) {
				Authored.FormatVersion = 9;
				const bool large = family == Family::ConeBounded104;
				Authored.Nodes = {
					{"bottom", "pc.3_d_material", "", {}, {{"diffuse", .25}}},
					{"side", "pc.3_d_material", "", {}, {{"diffuse", .75}}},
					{"cone",
					 "pc.3_d_mesh_cone",
					 "",
					 {},
					 {{"position", Vector3{2, 3, 4}},
					  {"anchor", Vector3{.1, .2, .3}},
					  {"rotation", Quaternion{.1, .2, .3, .4}},
					  {"scale", Vector3{2, -3, .5}}}},
					{"transform",
					 "pc.3_d_transform",
					 "",
					 {},
					 {{"position", Vector3{-1, 2, 3}},
					  {"anchor", Vector3{4, 5, 6}},
					  {"rotation", Quaternion{0, 0, 1, 0}},
					  {"scale", Vector3{-1, 2, 4}}}},
					{"get", "pc.3_d_get_data", "", {}, {}}
				};
				if (large) {
					// 104 sides yields 624 vertices and 208 edges, below the 4,096-element cap.
					Authored.Nodes[2].Values.push_back({"side", int64_t{104}});
					Authored.Nodes[2].Values.push_back({"smooth_side", true});
				}
				Authored.Links = {
					{"bottom", "material", "cone", "material_bottom"},
					{"side", "material", "cone", "material_side"},
					{"cone", "mesh", "transform", "mesh"},
					{"transform", "mesh", "get", "mesh"}
				};
				Authored.Outputs = {
					{"output", "transform", "mesh"},
					{"control", "get", "position"},
					{"creator", "side", "material"}
				};
			} else if (IsCylinder()) {
				Authored.FormatVersion = 9;
				Authored.Nodes = {
					{"material", "pc.3_d_material", "", {}, {{"diffuse", .25}}},
					{"cylinder",
					 "pc.3_d_mesh_cylinder",
					 "",
					 {},
					 {{"side", int64_t(family == Family::CylinderSmall ? 8 : 16)},
					  {"segments", int64_t(family == Family::CylinderSmall ? 1 : 12)},
					  {"end_caps", true},
					  {"smooth_side", family == Family::CylinderProfile},
					  {"position", Vector3{1, 2, 3}},
					  {"anchor", Vector3{-1, -2, -3}},
					  {"rotation", Quaternion{.5, .5, .5, .5}},
					  {"scale", Vector3{-1, 2, .5}}}},
					{"transform",
					 "pc.3_d_transform",
					 "",
					 {},
					 {{"position", Vector3{-1, 2, 3}},
					  {"anchor", Vector3{.25, .5, .75}},
					  {"rotation", Quaternion{0, 0, 1, 0}},
					  {"scale", Vector3{2, 3, 4}}}},
					{"get", "pc.3_d_get_data", "", {}, {}}
				};
				if (family == Family::CylinderProfile) {
					Curve profile;
					profile.Header = {0, 1, 0, 0, 1, 0};
					profile.Anchors = {{{0, 0, 0, .5, 0, 0}}, {{0, 0, 1, 1.5, 0, 0}}};
					Authored.Nodes[1].Values.push_back({"profile", std::move(profile)});
				}
				for (const auto port : {"material_side", "material_top", "material_bottom"})
					Authored.Links.push_back({"material", "material", "cylinder", port});
				Authored.Links.push_back({"cylinder", "mesh", "transform", "mesh"});
				Authored.Links.push_back({"transform", "mesh", "get", "mesh"});
				Authored.Outputs = {
					{"output", "transform", "mesh"},
					{"control", "get", "position"},
					{"creator", "material", "material"}
				};
			} else if (IsCube()) {
				Authored.FormatVersion = 9;
				const double subdivisions = family == Family::CubeSmall ? 1 : 10;
				Authored.Nodes = {
					{"material",
					 "pc.3_d_material",
					 "",
					 {},
					 {{"diffuse", .25},
					  {"interpolation", EnumValue{0}},
					  {"scale", Vector2{1, 1}},
					  {"shift", Vector2{0, 0}},
					  {"specular", 0.0},
					  {"shininess", 1.0},
					  {"metal", false},
					  {"reflectance", 0.0},
					  {"normal_strength", 1.0},
					  {"metalic_mapped", false},
					  {"roughness_mapped", false},
					  {"metalic", 0.0},
					  {"roughness", 1.0}},
					 {}},
					{"cube",
					 "pc.3_d_mesh_cube",
					 "",
					 {},
					 {{"subdivision", Vector3{subdivisions, subdivisions, subdivisions}},
					  {"material_mode", EnumValue{1}},
					  {"taper", 0.0},
					  {"position", Vector3{1, 2, 3}},
					  {"anchor", Vector3{-1, -2, -3}},
					  {"scale", Vector3{-1, 2, .5}}},
					 {}},
					{"transform",
					 "pc.3_d_transform",
					 "",
					 {},
					 {{"position", Vector3{-1, 2, 3}},
					  {"anchor", Vector3{.25, .5, .75}},
					  {"scale", Vector3{2, 3, 4}}},
					 {}},
					{"get", "pc.3_d_get_data", "", {}, {}, {}}
				};
				for (const auto port :
					 {"material",
					  "material_bottom",
					  "material_left",
					  "material_right",
					  "material_back",
					  "material_front"})
					Authored.Links.push_back({"material", "material", "cube", port});
				Authored.Links.push_back({"cube", "mesh", "transform", "mesh"});
				Authored.Links.push_back({"transform", "mesh", "get", "mesh"});
				Authored.Outputs = {
					{"output", "transform", "mesh"},
					{"control", "get", "position"},
					{"creator", "material", "material"}
				};
			} else if (IsHdr()) {
				Authored.FormatVersion = 9;
				const auto inputFormat = family == Family::HdrDirectional ? SurfaceFormat::RGBA16Float
																		  : SurfaceFormat::RGBA32Float;
				const auto layout = CheckedSurfaceLayout(128, 128, inputFormat, Limits::MaximumOutputBytes);
				if (!layout) Fail("HDR input layout");
				Image input{128, 128, std::vector<uint8_t>(layout->Bytes), 0, inputFormat};
				for (uint32_t y = 0; y < 128; ++y)
					for (uint32_t x = 0; x < 128; ++x)
						if (!StoreSurfacePixel(input, x, y, {2.00099, -.5002475, .25012375, 1}))
							Fail("HDR input pixel");
				ImageSources.push_back({"hdr", std::move(input)});
				Authored.Nodes = {
					{"capture", "image.captured", "", {}, {{"source_id", std::string{"hdr"}}}, {}},
					{"gaussian",
					 "pc.blur",
					 "",
					 {},
					 {{"size", 1.0},
					  {"gamma_correction", false},
					  {"attribute_color_depth", EnumValue{family == Family::HdrDirectional ? 5 : 4}}},
					 {}},
					{"motion",
					 family == Family::HdrDirectional ? "pc.blur_directional" : "pc.blur_zoom",
					 "",
					 {},
					 {},
					 {}}
				};
				if (family == Family::HdrDirectional)
					Authored.Nodes.back().Values = {
						{"strength", 8.0},
						{"resolution", .0625},
						{"direction", 0.0},
						{"fade_distance", false},
						{"gamma_correction", false},
						{"smooth_blur", 0.0},
						{"colorize", EnumValue{0}},
						{"attribute_color_depth", EnumValue{3}}
					};
				else
					Authored.Nodes.back().Values = {
						{"strength", 8.0},
						{"samples", int64_t{8}},
						{"center", Vector2{64, 64}},
						{"zoom_origin", EnumValue{1}},
						{"mode", EnumValue{0}},
						{"fade", false},
						{"gamma_correction", false},
						{"colorize", EnumValue{0}},
						{"attribute_color_depth", EnumValue{3}}
					};
				Authored.Links = {
					{"capture", "image", "gaussian", "surface_in"},
					{"gaussian", "surface_out", "motion", "surface_in"}
				};
				Authored.Outputs = {{"output", "motion", "surface_out"}};
			} else if (family == Family::Audio) {
				Authored.Timeline = TimelineSettings{1, 0, 0, "loop", 30};
				Authored.Nodes = {
					{"capture", "image.audio_recording", "", {}, {{"source_id", std::string{"stereo"}}}, {}},
					{"window",
					 "pc.audio_window",
					 "",
					 {},
					 {{"width", int64_t{1024}}, {"cursor_location", EnumValue{0}}, {"step", int64_t{1}}},
					 {}},
					{"fft", "pc.fft", "", {}, {{"preprocess_function", EnumValue{0}}}, {}}
				};
				Authored.Links = {
					{"capture", "audio", "window", "audio_data"}, {"window", "bit_array", "fft", "data"}
				};
				Authored.Outputs = {{"output", "fft", "array"}};
				// Audio Window excludes its clamped end packet; one trailing sample keeps the window at 1024.
				Captures = {
					{"stereo", 0, {}, 48000, {std::vector<double>(1025, 1), std::vector<double>(1025, 2)}}
				};
			} else if (family == Family::Gradient) {
				ArrayValue palette{ValueType::Colour, {Colour{0, 0, 0, 255}, Colour{255, 255, 255, 255}}};
				ArrayValue ratios{ValueType::Scalar, {}};
				for (size_t index = 0; index < 1024; index++)
					ratios.Elements.emplace_back(index % 2 ? .5 : 0.0);
				Authored.Nodes = {
					{"palette", "pc.gradient_palette", "", {}, {{"palette", palette}}, {}},
					{"sample", "pc.gradient_sample", "", {}, {{"type", EnumValue{1}}, {"ratio", ratios}}, {}}
				};
				Authored.Links = {{"palette", "gradient", "sample", "gradient"}};
				Authored.Outputs = {{"output", "sample", "colors"}};
			} else if (family == Family::Matrix) {
				Authored.FormatVersion = 8;
				MatrixValue identity{32, 32, std::vector<double>(1024, 0)};
				MatrixValue numbers{32, 32, std::vector<double>(1024, 0)};
				for (size_t index = 0; index < 1024; index++) {
					identity.Values[index] = index / 32 == index % 32 ? 1 : 0;
					numbers.Values[index] = static_cast<double>(index);
				}
				Authored.Nodes = {
					{"product",
					 "pc.matrix_math",
					 "",
					 {},
					 {{"matrix_1", identity}, {"matrix_2", numbers}, {"operation", EnumValue{5}}},
					 {}}
				};
				Authored.Outputs = {{"output", "product", "matrix"}};
			} else if (family == Family::Vector) {
				Authored.FormatVersion = 8;
				ArrayValue x{ValueType::Scalar, {}}, y{ValueType::Scalar, {}};
				for (size_t index = 0; index < 1024; index++) {
					const double k = static_cast<double>(index + 1);
					x.Elements.emplace_back(3 * k);
					y.Elements.emplace_back(4 * k);
				}
				Authored.Nodes = {
					{"creator", "pc.vector2", "", {}, {{"x", x}, {"y", y}}, {}},
					{"multiply",
					 "pc.vector_math",
					 "",
					 {},
					 {{"type", EnumValue{2}}, {"dimension", int64_t{2}}, {"scalar_b", true}, {"b", 2.0}},
					 {}},
					{"length",
					 "pc.vector_math",
					 "",
					 {},
					 {{"type", EnumValue{6}}, {"dimension", int64_t{2}}, {"scalar_b", true}, {"b", 0.0}},
					 {}}
				};
				Authored.Links = {
					{"creator", "vector", "multiply", "a"}, {"multiply", "result", "length", "a"}
				};
				Authored.Outputs = {
					{"output", "length", "result"},
					{"control", "multiply", "result"},
					{"creator", "creator", "vector"}
				};
			} else if (family == Family::ScalarMath) {
				Authored.FormatVersion = 8;
				ArrayValue operands{ValueType::Scalar, {}};
				for (size_t index = 0; index < 4096; index++)
					operands.Elements.emplace_back(static_cast<double>(index) / 4);
				Authored.Nodes = {
					{"multiply",
					 "pc.math",
					 "",
					 {},
					 {{"type", EnumValue{2}}, {"a", operands}, {"b", 2.0}},
					 {}},
					{"compare", "pc.compare", "", {}, {{"type", EnumValue{3}}, {"b", 1024.0}}, {}}
				};
				Authored.Links = {{"multiply", "result", "compare", "a"}};
				Authored.Outputs = {{"output", "compare", "result"}, {"control", "multiply", "result"}};
			} else if (family == Family::Curve) {
				Authored.FormatVersion = 8;
				ArrayValue progress{ValueType::Scalar, {}};
				for (size_t index = 0; index < 256; index++)
					progress.Elements.emplace_back(static_cast<double>(index % 65) / 64);
				Authored.Nodes = {
					{"function",
					 "pc.curve_function",
					 "",
					 {},
					 {{"type", EnumValue{2}}, {"resolution", int64_t{64}}},
					 {}},
					{"sample",
					 "pc.anim_curve",
					 "",
					 {},
					 {{"progress", progress}, {"minimum", 10.0}, {"maximum", 20.0}},
					 {}}
				};
				Authored.Links = {{"function", "curve", "sample", "curve"}};
				Authored.Outputs = {{"output", "sample", "curve"}, {"control", "function", "curve"}};
			} else if (family == Family::WavTimeline) {
				Authored.FormatVersion = 9;
				Authored.Nodes = {
					{"path",
					 "pc.string_merge",
					 "",
					 {},
					 {},
					 {{"text_0", ValueType::Text, Value{std::string("a.wav")}}}},
					{"wav", "pc.wav_file_read", "", {}, {{"path", std::string("unused.wav")}, {"mono", true}}}
				};
				Authored.Links = {{"path", "text", "wav", "path"}};
				Authored.Outputs = {{"output", "wav", "data"}};
				Authored.Keyframes = {
					{"path", "text_0", 0, std::string("a.wav"), "step"},
					{"path", "text_0", 2, std::string("b.wav"), "step"}
				};
				for (int file = 0; file < 2; ++file) {
					AudioBit clip;
					clip.SampleRate = 128;
					clip.Channels.resize(2);
					for (size_t index = 0; index < 256; ++index) {
						const double value = double(index % 32) / 32 * (file == 0 ? 1 : -1);
						clip.Channels[0].push_back(value);
						clip.Channels[1].push_back(-value);
					}
					Clips.push_back({file == 0 ? "a.wav" : "b.wav", std::move(clip)});
				}
			} else if (family == Family::WavControls) {
				Authored.FormatVersion = 8;
				Authored.Nodes = {
					{"wav", "pc.wav_file_read", "", {}, {{"path", std::string{"bounded.wav"}}}, {}},
					{"gain", "pc.number", "", {}, {{"value", .25}}, {}}
				};
				Authored.Links = {{"gain", "number", "wav", "attribute_preview_gain"}};
				Authored.Keyframes = {
					{"gain", "value", 0, .25, "linear"}, {"gain", "value", 10, .75, "step"}
				};
				Authored.Outputs = {{"output", "wav", "data"}};
				Clips = {
					{"bounded.wav", {std::vector<double>(Limits::MaximumAudioClipSamples, .25), 48000, {}}}
				};
			} else {
				Node batch{"batch", "value.array", "", {}, {}, {}};
				for (size_t index = 0; index < 8; index++) {
					const std::string id = "solid" + std::to_string(index);
					Authored.Nodes.push_back(
						{id,
						 "pc.solid",
						 "",
						 {},
						 {{"dimension_unit", EnumValue{0}},
						  {"dimension", Vector2{256, 256}},
						  {"color", Colour{static_cast<uint8_t>(index * 16), 32, 64, 255}}},
						 {}}
					);
					batch.DynamicInputs.push_back({id, ValueType::Image, std::nullopt});
					Authored.Links.push_back({id, "surface_out", "batch", id});
				}
				Authored.Nodes.push_back(std::move(batch));
				Authored.Nodes.push_back({"invert", "pc.invert", "", {}, {}, {}});
				Authored.Links.push_back({"batch", "array", "invert", "surface_in"});
				Authored.Outputs = {{"output", "invert", "surface_out"}};
			}
			if (family == Family::Vector || family == Family::WavTimeline || IsHdr() || IsCube() ||
				IsCylinder() || IsCone() || IsTorus() || IsUVSphere() || IsIcosphere()) {
				Document persisted;
				if (Read(Write(Authored), persisted, Failure) != Status::Ok) Fail("persisted vector graph");
				Authored = std::move(persisted);
			}
			for (unsigned char byte : Write(Authored))
				InputHash = HashBytes(InputHash, byte, 1);
			for (const auto &frame : Captures) {
				for (unsigned char byte : frame.SourceId)
					InputHash = HashBytes(InputHash, byte, 1);
				InputHash = HashBytes(InputHash, frame.Tick, 8);
				InputHash = HashBytes(InputHash, std::bit_cast<uint64_t>(frame.SampleRate), 8);
				for (const auto &channel : frame.Channels)
					for (double sample : channel)
						InputHash = HashBytes(InputHash, std::bit_cast<uint64_t>(sample), 8);
			}
			for (const auto &source : ImageSources) {
				InputHash = HashBytes(InputHash, static_cast<uint8_t>(source.Data.Format), 1);
				InputHash = HashBytes(InputHash, source.Data.Width, 4);
				InputHash = HashBytes(InputHash, source.Data.Height, 4);
				for (uint8_t byte : source.Data.Pixels)
					InputHash = HashBytes(InputHash, byte, 1);
			}
			for (const auto &clip : Clips) {
				for (unsigned char byte : clip.SourceId)
					InputHash = HashBytes(InputHash, byte, 1);
				InputHash = HashBytes(InputHash, std::bit_cast<uint64_t>(clip.Data.SampleRate), 8);
				for (double sample : clip.Data.Samples)
					InputHash = HashBytes(InputHash, std::bit_cast<uint64_t>(sample), 8);
			}
			if (Kind == Family::WavTimeline) InputHash = WavTimelineInputHash();
			if (Compile(Authored, Compiled, Failure) != Status::Ok) Fail("compile");
			Evaluate();
			OutputHash = Verify();
			Evaluate();
			if (Verify() != OutputHash) Fail("replay");
		}

		bool IsAudioWindow() const {
			return Kind == Family::AudioWindow4096 || Kind == Family::AudioWindow65536;
		}
		bool IsCylinder() const {
			return Kind == Family::CylinderSmall || Kind == Family::CylinderProfile;
		}
		bool IsIcosphere() const {
			return Kind == Family::IcosphereDefault1 || Kind == Family::IcosphereSmooth3;
		}
		bool IsUVSphere() const {
			return Kind == Family::UVSphereDefault8x16 || Kind == Family::UVSphereSmooth31x22;
		}
		bool IsTorus() const {
			return Kind == Family::TorusDefault16x8 || Kind == Family::TorusSmooth31x22;
		}
		bool IsCone() const {
			return Kind == Family::ConeDefault8 || Kind == Family::ConeBounded104;
		}
		bool IsCube() const {
			return Kind == Family::CubeSmall || Kind == Family::CubeLarge;
		}
		bool IsHdr() const {
			return Kind == Family::HdrDirectional || Kind == Family::HdrZoom;
		}

		[[noreturn]] void Fail(const char *stage) const {
			throw std::runtime_error(
				std::string("source-family fixture ") + stage +
				" family=" + std::to_string(static_cast<int>(Kind)) + " node=" + Failure.NodeId +
				" port=" + Failure.Port + ": " + Failure.Message
			);
		}
		static constexpr std::array<FrameTime, 4> AudioWindowClocks{
			{{64, .5, false}, {64, .5, true}, {0, .5, false}, {0, .5, true}}
		};
		void Evaluate() {
			if (IsAudioWindow()) {
				// Replace the first retained waveform, then restore it so every signed result remains
				// observable.
				for (size_t operation = 0; operation < 5; ++operation) {
					const size_t target = operation == 0 ? 0 : operation - 1;
					AudioObservation->Select(
						operation == 0 || target >= 2 ? 1 : 0, AudioWindowClocks[target]
					);
					uint64_t others = 0;
					for (size_t j = 0; j < AudioWindows.size(); ++j)
						if (j != target) others += AudioWindows[j].Points.capacity() * sizeof(Vector2);
					if (ResolveAudioWindowPresentation(
							AudioObservation->Doc,
							AudioObservation->Compiled,
							"window",
							AudioObservation->Request,
							Limits::MaximumEvaluationBytes - others,
							AudioWindows[target],
							Failure
						) != Status::Ok)
						Fail("audio observation resolve");
				}
				return;
			}
			EvaluationRequest request;
			request.AudioFrames = Captures;
			request.ImageSources = ImageSources;
			if (IsHdr()) {
				if (engine::imagegraph::Evaluate(Authored, Compiled, "output", request, HdrOutput, Failure) !=
					Status::Ok)
					Fail("HDR evaluate");
				return;
			}
			if (Kind == Family::WavTimeline) {
				request.AudioClips = Clips;
				for (size_t index = 0; index < Waveforms.size(); ++index) {
					if (!SetFrameTime(request, WavTimelineFrames()[index])) Fail("waveform clock");
					uint64_t others = 0;
					for (size_t other = 0; other < Waveforms.size(); ++other)
						if (other != index) others += Waveforms[other].Points.capacity() * sizeof(Vector2);
					if (others > Limits::MaximumEvaluationBytes ||
						ResolveWavTimelinePresentation(
							Authored,
							Compiled,
							"wav",
							request,
							16,
							Limits::MaximumEvaluationBytes - others,
							Waveforms[index],
							Failure
						) != Status::Ok)
						Fail("waveform observation");
				}
				return;
			}
			if (Kind == Family::WavControls) {
				request.AudioClips = Clips;
				request.Tick = 5;
				request.Subframe = .5;
				if (ResolveWavPreviewControls(Authored, "wav", request, Controls, Failure) != Status::Ok)
					Fail("controls");
				return;
			}
			// This row measures compilation as well as matrix execution, rather than plan reuse.
			if (Kind == Family::Matrix && Compile(Authored, Compiled, Failure) != Status::Ok) Fail("compile");
			const Status status =
				Kind == Family::Solid
					? EvaluateArray(Authored, Compiled, "output", request, ImageOutput, Failure)
					: EvaluateValue(Authored, Compiled, "output", request, ValueOutput, Failure);
			if (status != Status::Ok) Fail("evaluate");
			// Selected control outputs add evaluation traversals within the owner frame.
			if ((Kind == Family::ScalarMath || Kind == Family::Curve || Kind == Family::Vector || IsCube() ||
				 IsCylinder() || IsCone() || IsTorus() || IsUVSphere() || IsIcosphere()) &&
				EvaluateValue(Authored, Compiled, "control", request, ControlOutput, Failure) != Status::Ok)
				Fail("control evaluate");
			if ((Kind == Family::Vector || IsCube() || IsCylinder() || IsCone() || IsTorus() ||
				 IsUVSphere() || IsIcosphere()) &&
				EvaluateValue(Authored, Compiled, "creator", request, CreatorOutput, Failure) != Status::Ok)
				Fail("creator evaluate");
		}

		static std::array<FrameTime, 3> WavTimelineFrames() {
			return {{{1, .25, true}, {1, .25, false}, {2, .5, false}}};
		}
		uint64_t WavTimelineInputHash() const {
			uint64_t hash = 14695981039346656037ULL;
			for (unsigned char byte : Write(Authored))
				hash = HashBytes(hash, byte, 1);
			for (const auto &clip : Clips) {
				for (unsigned char byte : clip.SourceId)
					hash = HashBytes(hash, byte, 1);
				hash = HashBytes(hash, clip.SourceId.size(), 8);
				hash = HashBytes(hash, std::bit_cast<uint64_t>(clip.Data.SampleRate), 8);
				hash = HashBytes(hash, clip.Data.Samples.size(), 8);
				for (double value : clip.Data.Samples)
					hash = HashBytes(hash, std::bit_cast<uint64_t>(value), 8);
				hash = HashBytes(hash, clip.Data.Channels.size(), 8);
				for (const auto &channel : clip.Data.Channels) {
					hash = HashBytes(hash, channel.size(), 8);
					for (double value : channel)
						hash = HashBytes(hash, std::bit_cast<uint64_t>(value), 8);
				}
			}
			for (const auto frame : WavTimelineFrames()) {
				hash = HashBytes(hash, frame.Tick, 8);
				hash = HashBytes(hash, std::bit_cast<uint64_t>(frame.Subframe), 8);
				hash = HashBytes(hash, frame.NegativeFrame, 1);
			}
			return hash;
		}
		uint64_t VerifyWavTimeline() const {
			if (WavTimelineInputHash() != InputHash) Fail("waveform inputs mutated");
			uint64_t hash = 14695981039346656037ULL;
			const std::array<double, 3> progress{0, .0390625, .078125};
			for (size_t observation = 0; observation < Waveforms.size(); ++observation) {
				const auto &value = Waveforms[observation];
				if (value.Points.size() != 33 || value.Channels != 2 || value.Duration != 2 ||
					value.Progress != progress[observation])
					Fail("waveform metadata");
				hash = HashBytes(hash, value.Points.size(), 8);
				hash = HashBytes(hash, value.Channels, 8);
				hash = HashBytes(hash, std::bit_cast<uint64_t>(value.Duration), 8);
				hash = HashBytes(hash, std::bit_cast<uint64_t>(value.Progress), 8);
				for (size_t point = 0; point < 33; ++point) {
					const double expected =
						(point == 32 ? 31. / 32 : double(point % 4) / 4) * (observation == 2 ? -1 : 1);
					if (value.Points[point] != Vector2{double(point), expected} ||
						std::signbit(value.Points[point].Y) != std::signbit(expected))
						Fail("waveform point");
					hash = HashBytes(hash, std::bit_cast<uint64_t>(value.Points[point].X), 8);
					hash = HashBytes(hash, std::bit_cast<uint64_t>(value.Points[point].Y), 8);
				}
			}
			return hash;
		}
		uint64_t VerifyCube() const {
			uint64_t hash = 14695981039346656037ULL;
			const auto number = [&](double value) {
				if (!std::isfinite(value)) Fail("Cube nonfinite payload");
				hash = HashBytes(hash, std::bit_cast<uint64_t>(value), 8);
			};
			const auto vector = [&](Vector3 value) {
				number(value.X);
				number(value.Y);
				number(value.Z);
			};
			const auto &mesh = std::get<MeshValue3D>(ValueOutput.Data);
			if (!mesh.Data) Fail("Cube missing backing");
			const auto &data = *mesh.Data;
			const size_t n = Kind == Family::CubeSmall ? 1 : 10;
			if (data.Parts.size() != 6 || data.Edges.size() != 12 || data.Materials.size() != 6 ||
				data.LocalTransforms.size() != 2)
				Fail("Cube complete shape");
			const std::array<MeshTransform3D, 2> transforms{
				{{{-1, 2, 3}, {.25, .5, .75}, {}, {2, 3, 4}}, {{1, 2, 3}, {-1, -2, -3}, {}, {-1, 2, .5}}}
			};
			if (std::get<Vector3>(ControlOutput.Data) != transforms[0].Position)
				Fail("Cube GetData position");
			vector(std::get<Vector3>(ControlOutput.Data));
			for (size_t index = 0; index < 2; ++index) {
				const auto &value = data.LocalTransforms[index];
				if (value != transforms[index]) Fail("Cube local transform chain");
				vector(value.Position);
				vector(value.Anchor);
				vector(value.Scale);
				for (double v : {value.Rotation.X, value.Rotation.Y, value.Rotation.Z, value.Rotation.W})
					number(v);
			}
			// Literal face planes and UV triangle order, not the executor's corner interpolation.
			const std::array<Vector3, 6> normals{
				{{0, 0, 1}, {0, 0, -1}, {-1, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, -1, 0}}
			};
			const std::array<std::array<Vector2, 6>, 6> uvOrder{
				{{{{1, 1}, {0, 0}, {0, 1}, {1, 1}, {1, 0}, {0, 0}}},
				 {{{1, 1}, {0, 1}, {0, 0}, {1, 1}, {0, 0}, {1, 0}}},
				 {{{1, 0}, {0, 1}, {0, 0}, {1, 0}, {1, 1}, {0, 1}}},
				 {{{0, 0}, {1, 0}, {1, 1}, {0, 0}, {1, 1}, {0, 1}}},
				 {{{1, 0}, {0, 1}, {0, 0}, {1, 0}, {1, 1}, {0, 1}}},
				 {{{0, 0}, {1, 0}, {1, 1}, {0, 0}, {1, 1}, {0, 1}}}}
			};
			for (size_t face = 0; face < 6; ++face) {
				const auto &part = data.Parts[face];
				if (part.MaterialIndex != face || part.Vertices.size() != 6 * n * n) Fail("Cube part shape");
				hash = HashBytes(hash, part.MaterialIndex, 4);
				for (size_t index = 0; index < part.Vertices.size(); ++index) {
					const auto &v = part.Vertices[index];
					const size_t cell = index / 6;
					const Vector2 uv{
						(double(cell / n) + uvOrder[face][index % 6].X) / double(n),
						(double(cell % n) + uvOrder[face][index % 6].Y) / double(n)
					};
					const std::array<Vector3, 6> expected{
						{{.5 - uv.X, .5 - uv.Y, .5},
						 {.5 - uv.X, .5 - uv.Y, -.5},
						 {-.5, .5 - uv.X, .5 - uv.Y},
						 {.5, -.5 + uv.X, .5 - uv.Y},
						 {.5 - uv.X, .5, .5 - uv.Y},
						 {-.5 + uv.X, -.5, .5 - uv.Y}}
					};
					if (v.Position != expected[face] || v.Normal != normals[face] || v.UV != uv ||
						v.Tint != Colour{255, 255, 255, 255})
						Fail("Cube literal geometry UV normal tint");
					vector(v.Position);
					vector(v.Normal);
					number(v.UV.X);
					number(v.UV.Y);
					for (uint8_t byte : {v.Tint.Red, v.Tint.Green, v.Tint.Blue, v.Tint.Alpha})
						hash = HashBytes(hash, byte, 1);
				}
			}
			const std::array<MeshEdge3D, 12> edges{
				{{{-.5, -.5, -.5}, {.5, -.5, -.5}},
				 {{.5, -.5, -.5}, {.5, .5, -.5}},
				 {{.5, .5, -.5}, {-.5, .5, -.5}},
				 {{-.5, .5, -.5}, {-.5, -.5, -.5}},
				 {{-.5, -.5, .5}, {.5, -.5, .5}},
				 {{.5, -.5, .5}, {.5, .5, .5}},
				 {{.5, .5, .5}, {-.5, .5, .5}},
				 {{-.5, .5, .5}, {-.5, -.5, .5}},
				 {{-.5, -.5, -.5}, {-.5, -.5, .5}},
				 {{.5, -.5, -.5}, {.5, -.5, .5}},
				 {{-.5, .5, -.5}, {-.5, .5, .5}},
				 {{.5, .5, -.5}, {.5, .5, .5}}}
			};
			for (size_t index = 0; index < 12; ++index) {
				if (data.Edges[index] != edges[index]) Fail("Cube twelve literal edges");
				vector(data.Edges[index].From);
				vector(data.Edges[index].To);
			}
			const auto &created = std::get<MaterialValue3D>(CreatorOutput.Data);
			MaterialData3D expectedMaterial;
			expectedMaterial.Diffuse = .25;
			expectedMaterial.Surface = Image{1, 1, {0, 0, 0, 0}};
			expectedMaterial.Surface->Hash = SurfaceHash(*expectedMaterial.Surface);
			// Pinned Material source uses a 32x32 minimum property map without linked textures.
			expectedMaterial.PropertiesMap = Image{32, 32, std::vector<uint8_t>(32 * 32 * 4, 0)};
			for (size_t pixel = 0; pixel < 32 * 32; ++pixel)
				expectedMaterial.PropertiesMap->Pixels[pixel * 4 + 3] = 255;
			expectedMaterial.PropertiesMap->Hash = SurfaceHash(*expectedMaterial.PropertiesMap);
			if (created.Get() != expectedMaterial) Fail("Cube literal Material descriptor");
			const auto materialHash = [&](const MaterialValue3D &material) {
				const auto &m = material.Get();
				for (double value :
					 {m.TextureScale.X,
					  m.TextureScale.Y,
					  m.TextureShift.X,
					  m.TextureShift.Y,
					  m.TextureFilter,
					  m.Diffuse,
					  m.Specular,
					  m.Shininess,
					  m.Reflectance,
					  m.NormalStrength,
					  m.MetallicRange.X,
					  m.MetallicRange.Y,
					  m.RoughnessRange.X,
					  m.RoughnessRange.Y})
					number(value);
				for (bool value : {m.Metal, m.MetallicMapped, m.RoughnessMapped})
					hash = HashBytes(hash, value, 1);
				for (const auto *image : {&m.Surface, &m.Normal, &m.PropertiesMap}) {
					hash = HashBytes(hash, image->has_value(), 1);
					if (!*image) continue;
					const auto &value = **image;
					hash = HashBytes(hash, static_cast<uint8_t>(value.Format), 1);
					hash = HashBytes(hash, value.Width, 4);
					hash = HashBytes(hash, value.Height, 4);
					hash = HashBytes(hash, value.Hash, 8);
					for (uint8_t byte : value.Pixels)
						hash = HashBytes(hash, byte, 1);
				}
			};
			materialHash(created);
			for (const auto &material : data.Materials) {
				if (material != created) Fail("Cube six owned material clones");
				materialHash(material);
			}
			return hash;
		}

		uint64_t VerifyIcosphere() const {
			uint64_t hash = 14695981039346656037ULL;
			const auto number = [&](double value) {
				if (!std::isfinite(value)) Fail("Icosphere nonfinite payload");
				hash = HashBytes(hash, std::bit_cast<uint64_t>(value), 8);
			};
			const auto vector = [&](Vector3 value) {
				number(value.X);
				number(value.Y);
				number(value.Z);
			};
			const auto near = [](double actual, double expected) {
				return std::abs(actual - expected) <= 1e-12;
			};
			const auto same = [&](Vector3 a, Vector3 b) {
				return near(a.X, b.X) && near(a.Y, b.Y) && near(a.Z, b.Z);
			};

			const bool smooth = Kind == Family::IcosphereSmooth3;
			const uint32_t level = smooth ? 3 : 1;
			const size_t count = smooth ? 3840 : 240;
			const auto &mesh = std::get<MeshValue3D>(ValueOutput.Data);
			if (!mesh.Data) Fail("Icosphere missing backing");
			const auto &data = *mesh.Data;
			if (data.Parts.size() != 1 || data.Materials.size() != 1 || data.LocalTransforms.size() != 2 ||
				data.Parts[0].MaterialIndex != 0 || data.Parts[0].Vertices.size() != count ||
				data.Edges.size() != count)
				Fail("Icosphere complete source shape");
			const std::array<MeshTransform3D, 2> transforms{
				{{{-1, 2, 3}, {4, 5, 6}, {0, 0, 1, 0}, {-1, 2, 4}},
				 {{2, 3, 4}, {.1, .2, .3}, {.1, .2, .3, .4}, {2, -3, .5}}}
			};
			if (std::get<Vector3>(ControlOutput.Data) != transforms[0].Position)
				Fail("Icosphere GetData control");
			vector(std::get<Vector3>(ControlOutput.Data));
			for (size_t i = 0; i < transforms.size(); ++i) {
				const auto &actual = data.LocalTransforms[i];
				if (actual != transforms[i]) Fail("Icosphere local transform order");
				vector(actual.Position);
				vector(actual.Anchor);
				vector(actual.Scale);
				for (double value :
					 {actual.Rotation.X, actual.Rotation.Y, actual.Rotation.Z, actual.Rotation.W})
					number(value);
			}

			struct Reference {
				Vector3 P;
				bool Old;
				std::vector<size_t> Adjacent;
			};
			std::vector<Reference> points;
			const auto normal = [](Vector3 p) {
				const double d = std::sqrt(p.X * p.X + p.Y * p.Y + p.Z * p.Z);
				return d ? Vector3{p.X / d, p.Y / d, p.Z / d} : p;
			};
			const double b = 2 / (1 + std::sqrt(5.0));
			for (Vector3 p : std::array<Vector3, 13>{
					 {{1, 1, 1},
					  {0, b, -1},
					  {b, 1, 0},
					  {-b, 1, 0},
					  {0, b, 1},
					  {0, -b, 1},
					  {-1, 0, b},
					  {0, -b, -1},
					  {1, 0, -b},
					  {1, 0, b},
					  {-1, 0, -b},
					  {b, -1, 0},
					  {-b, -1, 0}}
				 }) {
				p = normal(p);
				points.push_back({{p.X / 2, p.Y / 2, p.Z / 2}, true, {}});
			}
			using Triangle = std::array<size_t, 3>;
			std::vector<Triangle> triangles{{3, 1, 2},	{2, 4, 3},	 {6, 4, 5},	  {5, 4, 9},  {8, 1, 7},
											{7, 1, 10}, {12, 5, 11}, {11, 7, 12}, {10, 3, 6}, {6, 12, 10},
											{9, 2, 8},	{8, 11, 9},	 {3, 4, 6},	  {9, 4, 2},  {10, 1, 3},
											{2, 1, 8},	{12, 7, 10}, {8, 7, 11},  {6, 5, 12}, {11, 5, 9}};
			for (uint32_t stage = 0; stage < level; ++stage) {
				// Unlike the kernel's endpoint keys, this oracle uses source coordinate strings.
				std::map<std::string, size_t> pool;
				std::vector<Triangle> replacement;
				const auto middle = [&](size_t first, size_t second) {
					const auto a = points[first].P, c = points[second].P;
					const Vector3 p{(a.X + c.X) / 2, (a.Y + c.Y) / 2, (a.Z + c.Z) / 2};
					std::ostringstream key;
					key.imbue(std::locale::classic());
					for (double coordinate : {p.X, p.Y, p.Z}) {
						if (coordinate == std::trunc(coordinate))
							key << int64_t(coordinate);
						else
							key << std::fixed << std::setprecision(2) << coordinate;
						key << ',';
					}
					const auto found = pool.find(key.str());
					if (found != pool.end()) return found->second;
					const size_t index = points.size();
					points.push_back({p, false, {}});
					pool.emplace(key.str(), index);
					return index;
				};
				for (const auto &triangle : triangles) {
					const auto a = triangle[0], c = triangle[1], d = triangle[2];
					const auto ac = middle(a, c), cd = middle(c, d), da = middle(d, a);
					points[a].Adjacent.insert(points[a].Adjacent.end(), {ac, da});
					points[c].Adjacent.insert(points[c].Adjacent.end(), {ac, cd});
					points[d].Adjacent.insert(points[d].Adjacent.end(), {cd, da});
					replacement.insert(
						replacement.end(), {{a, ac, da}, {ac, c, cd}, {da, cd, d}, {ac, cd, da}}
					);
				}
				for (const auto &triangle : replacement)
					for (size_t id : triangle) {
						auto &vertex = points[id];
						if (vertex.Old && !vertex.Adjacent.empty()) {
							const double valence = double(vertex.Adjacent.size()) / 2,
										 beta = 3 / (5 * valence);
							std::array<double, 3> sums{};
							for (size_t neighbor : vertex.Adjacent) {
								const auto p = points[neighbor].P;
								sums[0] += p.X;
								sums[1] += p.Y;
								sums[2] += p.Z;
							}
							const auto p = vertex.P;
							vertex.P = {
								sums[0] * .5 * beta + p.X * (1 - valence * beta),
								sums[1] * .5 * beta + p.Y * (1 - valence * beta),
								sums[2] * .5 * beta + p.Z * (1 - valence * beta)
							};
						}
						vertex.Old = true;
						vertex.Adjacent.clear();
					}
				triangles = std::move(replacement);
			}
			hash = HashBytes(hash, data.Parts[0].MaterialIndex, 4);
			hash = HashBytes(hash, data.Parts[0].Vertices.size(), 8);
			hash = HashBytes(hash, data.Edges.size(), 8);
			const auto angle = [](double x, double y) {
				double a = std::atan2(-y, x) * 180 / std::numbers::pi;
				return a < 0 ? a + 360 : a;
			};
			for (size_t i = 0; i < triangles.size(); ++i) {
				const std::array<Vector3, 3> p{
					points[triangles[i][0]].P, points[triangles[i][1]].P, points[triangles[i][2]].P
				};
				const std::array<double, 3> down{p[2].X - p[0].X, p[2].Y - p[0].Y, p[2].Z - p[0].Z},
					across{p[1].X - p[0].X, p[1].Y - p[0].Y, p[1].Z - p[0].Z};
				std::array<double, 3> det{};
				for (size_t axis = 0; axis < 3; ++axis)
					det[axis] = down[(axis + 1) % 3] * across[(axis + 2) % 3] -
								down[(axis + 2) % 3] * across[(axis + 1) % 3];
				for (size_t j = 0; j < 3; ++j) {
					const auto &actual = data.Parts[0].Vertices[i * 3 + j];
					double va = std::fmod(angle(p[j].X, p[j].Z) + 90, 360);
					if (va > 180) va = 360 - va;
					const Vector3 expectedNormal = smooth ? normal(p[j]) : Vector3{det[0], det[1], det[2]};
					if (!same(actual.Position, p[j]) || !same(actual.Normal, expectedNormal) ||
						!near(actual.UV.X, angle(p[j].X, p[j].Y) / 360) || !near(actual.UV.Y, va / 180) ||
						actual.Tint != Colour{255, 255, 255, 255})
						Fail("Icosphere all ordered geometry normals UV tint");
					vector(actual.Position);
					vector(actual.Normal);
					number(actual.UV.X);
					number(actual.UV.Y);
					for (uint8_t byte :
						 {actual.Tint.Red, actual.Tint.Green, actual.Tint.Blue, actual.Tint.Alpha})
						hash = HashBytes(hash, byte, 1);
				}
				const std::array<MeshEdge3D, 3> expected{{{p[0], p[1]}, {p[0], p[2]}, {p[2], p[1]}}};
				for (size_t j = 0; j < 3; ++j) {
					const auto &edge = data.Edges[i * 3 + j];
					if (!same(edge.From, expected[j].From) || !same(edge.To, expected[j].To))
						Fail("Icosphere every ordered duplicate edge");
					vector(edge.From);
					vector(edge.To);
				}
			}
			const auto materialHash = [&](const MaterialValue3D &material) {
				const auto &value = material.Get();
				for (double field :
					 {value.TextureScale.X,
					  value.TextureScale.Y,
					  value.TextureShift.X,
					  value.TextureShift.Y,
					  value.TextureFilter,
					  value.Diffuse,
					  value.Specular,
					  value.Shininess,
					  value.Reflectance,
					  value.NormalStrength,
					  value.MetallicRange.X,
					  value.MetallicRange.Y,
					  value.RoughnessRange.X,
					  value.RoughnessRange.Y})
					number(field);
				for (bool field : {value.Metal, value.MetallicMapped, value.RoughnessMapped})
					hash = HashBytes(hash, field, 1);
				for (const auto *image : {&value.Surface, &value.Normal, &value.PropertiesMap}) {
					hash = HashBytes(hash, image->has_value(), 1);
					if (!*image) continue;
					const auto &pixels = **image;
					hash = HashBytes(hash, static_cast<uint8_t>(pixels.Format), 1);
					hash = HashBytes(hash, pixels.Width, 4);
					hash = HashBytes(hash, pixels.Height, 4);
					hash = HashBytes(hash, pixels.Hash, 8);
					for (uint8_t byte : pixels.Pixels)
						hash = HashBytes(hash, byte, 1);
				}
			};
			const auto expectedMaterial = [&](double diffuse) {
				MaterialData3D expected;
				expected.Diffuse = diffuse;
				expected.Surface = Image{1, 1, {0, 0, 0, 0}};
				expected.Surface->Hash = SurfaceHash(*expected.Surface);
				expected.PropertiesMap = Image{32, 32, std::vector<uint8_t>(32 * 32 * 4, 0)};
				for (size_t pixel = 0; pixel < 32 * 32; ++pixel)
					expected.PropertiesMap->Pixels[pixel * 4 + 3] = 255;
				expected.PropertiesMap->Hash = SurfaceHash(*expected.PropertiesMap);
				return expected;
			};

			const auto &creator = std::get<MaterialValue3D>(CreatorOutput.Data);
			const auto &material = data.Materials[0];
			if (creator.Get() != expectedMaterial(.25) || material.Get() != expectedMaterial(.25) ||
				material.Data.operator->() == creator.Data.operator->() ||
				material.Get().Surface->Pixels.data() == creator.Get().Surface->Pixels.data() ||
				material.Get().PropertiesMap->Pixels.data() == creator.Get().PropertiesMap->Pixels.data())
				Fail("Icosphere complete independently owned material descriptor");
			materialHash(material);
			materialHash(creator);
			return hash;
		}

		uint64_t VerifyUVSphere() const {
			uint64_t hash = 14695981039346656037ULL;
			const auto number = [&](double value) {
				if (!std::isfinite(value)) Fail("UV Sphere nonfinite payload");
				hash = HashBytes(hash, std::bit_cast<uint64_t>(value), 8);
			};
			const auto vector = [&](Vector3 value) {
				number(value.X);
				number(value.Y);
				number(value.Z);
			};
			const auto near = [](double actual, double expected) {
				return std::abs(actual - expected) <= 1e-11;
			};
			const auto same = [&](Vector3 a, Vector3 b) {
				return near(a.X, b.X) && near(a.Y, b.Y) && near(a.Z, b.Z);
			};

			const bool smooth = Kind == Family::UVSphereSmooth31x22;
			const size_t h = smooth ? 31 : 8, v = smooth ? 22 : 16;
			const int projection = smooth ? 1 : 0;
			const auto &mesh = std::get<MeshValue3D>(ValueOutput.Data);
			if (!mesh.Data) Fail("UV Sphere missing backing");
			const auto &data = *mesh.Data;
			if (data.Parts.size() != 1 || data.Materials.size() != 1 || data.LocalTransforms.size() != 2 ||
				data.Parts[0].MaterialIndex != 0 || data.Parts[0].Vertices.size() != 6 * h * v ||
				data.Edges.size() != 2 * h * v)
				Fail("UV Sphere complete source shape");
			const std::array<MeshTransform3D, 2> transforms{
				{{{-1, 2, 3}, {4, 5, 6}, {0, 0, 1, 0}, {-1, 2, 4}},
				 {{2, 3, 4}, {.1, .2, .3}, {.1, .2, .3, .4}, {2, -3, .5}}}
			};
			if (std::get<Vector3>(ControlOutput.Data) != transforms[0].Position)
				Fail("UV Sphere GetData control");
			vector(std::get<Vector3>(ControlOutput.Data));
			for (size_t i = 0; i < transforms.size(); ++i) {
				const auto &actual = data.LocalTransforms[i];
				if (actual != transforms[i]) Fail("UV Sphere local transform order");
				vector(actual.Position);
				vector(actual.Anchor);
				vector(actual.Scale);
				for (double value :
					 {actual.Rotation.X, actual.Rotation.Y, actual.Rotation.Z, actual.Rotation.W})
					number(value);
			}

			// Independent spherical coordinates retain positive Y and unsnapped degree trig.
			const auto point = [](double longitude, double latitude) {
				const double phi = longitude * std::numbers::pi / 180,
							 theta = latitude * std::numbers::pi / 180;
				return Vector3{
					.5 * std::cos(theta) * std::cos(phi),
					.5 * std::cos(theta) * std::sin(phi),
					.5 * std::sin(theta)
				};
			};
			const auto tex = [&](double latitude) {
				return projection == 0 ? (1 - std::sin(latitude * std::numbers::pi / 180)) / 2
									   : (90 - latitude) / 180;
			};
			hash = HashBytes(hash, data.Parts[0].MaterialIndex, 4);
			hash = HashBytes(hash, data.Parts[0].Vertices.size(), 8);
			hash = HashBytes(hash, data.Edges.size(), 8);
			for (size_t i = 0; i < v; ++i)
				for (size_t j = 0; j < h; ++j) {
					const double a = 360 * double(i) / v, b = 360 * double(i + 1) / v,
								 c = 90 - 180 * double(j) / h, d = 90 - 180 * double(j + 1) / h;
					const std::array<Vector3, 4> p{point(a, c), point(b, c), point(a, d), point(b, d)};
					std::array<Vector3, 4> n = p;
					if (!smooth) {
						const std::array<double, 3> down{p[2].X - p[0].X, p[2].Y - p[0].Y, p[2].Z - p[0].Z},
							across{p[1].X - p[0].X, p[1].Y - p[0].Y, p[1].Z - p[0].Z};
						std::array<double, 3> determinant{};
						for (size_t k = 0; k < 3; ++k)
							determinant[k] = down[(k + 1) % 3] * across[(k + 2) % 3] -
											 down[(k + 2) % 3] * across[(k + 1) % 3];
						const double magnitude = std::hypot(determinant[0], determinant[1], determinant[2]);
						if (!(magnitude > 0) || !std::isfinite(magnitude))
							Fail("UV Sphere oracle degenerate normal");
						n.fill(
							{determinant[0] / magnitude,
							 determinant[1] / magnitude,
							 determinant[2] / magnitude}
						);
					}
					const std::array<Vector2, 4> uv{
						{{a / 360, tex(c)}, {b / 360, tex(c)}, {a / 360, tex(d)}, {b / 360, tex(d)}}
					};
					constexpr std::array<size_t, 6> order{0, 1, 2, 1, 3, 2};
					for (size_t k = 0; k < order.size(); ++k) {
						const auto &actual = data.Parts[0].Vertices[(i * h + j) * 6 + k];
						const auto index = order[k];
						if (!same(actual.Position, p[index]) || !same(actual.Normal, n[index]) ||
							!near(actual.UV.X, uv[index].X) || !near(actual.UV.Y, uv[index].Y) ||
							actual.Tint != Colour{255, 255, 255, 255})
							Fail("UV Sphere all ordered geometry normal UV tint");
						vector(actual.Position);
						vector(actual.Normal);
						number(actual.UV.X);
						number(actual.UV.Y);
						for (uint8_t byte :
							 {actual.Tint.Red, actual.Tint.Green, actual.Tint.Blue, actual.Tint.Alpha})
							hash = HashBytes(hash, byte, 1);
					}
					const std::array<MeshEdge3D, 2> expected{{{p[0], p[1]}, {p[0], p[2]}}};
					for (size_t k = 0; k < 2; ++k) {
						const auto &actual = data.Edges[(i * h + j) * 2 + k];
						if (!same(actual.From, expected[k].From) || !same(actual.To, expected[k].To))
							Fail("UV Sphere all ordered edge endpoints");
						vector(actual.From);
						vector(actual.To);
					}
				}
			const auto materialHash = [&](const MaterialValue3D &material) {
				const auto &value = material.Get();
				for (double field :
					 {value.TextureScale.X,
					  value.TextureScale.Y,
					  value.TextureShift.X,
					  value.TextureShift.Y,
					  value.TextureFilter,
					  value.Diffuse,
					  value.Specular,
					  value.Shininess,
					  value.Reflectance,
					  value.NormalStrength,
					  value.MetallicRange.X,
					  value.MetallicRange.Y,
					  value.RoughnessRange.X,
					  value.RoughnessRange.Y})
					number(field);
				for (bool field : {value.Metal, value.MetallicMapped, value.RoughnessMapped})
					hash = HashBytes(hash, field, 1);
				for (const auto *image : {&value.Surface, &value.Normal, &value.PropertiesMap}) {
					hash = HashBytes(hash, image->has_value(), 1);
					if (!*image) continue;
					const auto &pixels = **image;
					hash = HashBytes(hash, static_cast<uint8_t>(pixels.Format), 1);
					hash = HashBytes(hash, pixels.Width, 4);
					hash = HashBytes(hash, pixels.Height, 4);
					hash = HashBytes(hash, pixels.Hash, 8);
					for (uint8_t byte : pixels.Pixels)
						hash = HashBytes(hash, byte, 1);
				}
			};
			const auto expectedMaterial = [&](double diffuse) {
				MaterialData3D expected;
				expected.Diffuse = diffuse;
				expected.Surface = Image{1, 1, {0, 0, 0, 0}};
				expected.Surface->Hash = SurfaceHash(*expected.Surface);
				expected.PropertiesMap = Image{32, 32, std::vector<uint8_t>(32 * 32 * 4, 0)};
				for (size_t pixel = 0; pixel < 32 * 32; ++pixel)
					expected.PropertiesMap->Pixels[pixel * 4 + 3] = 255;
				expected.PropertiesMap->Hash = SurfaceHash(*expected.PropertiesMap);
				return expected;
			};

			const auto &creator = std::get<MaterialValue3D>(CreatorOutput.Data);
			const auto &material = data.Materials[0];
			if (creator.Get() != expectedMaterial(.25) || material.Get() != expectedMaterial(.25) ||
				material.Data.operator->() == creator.Data.operator->() ||
				material.Get().Surface->Pixels.data() == creator.Get().Surface->Pixels.data() ||
				material.Get().PropertiesMap->Pixels.data() == creator.Get().PropertiesMap->Pixels.data())
				Fail("UV Sphere complete independently owned material descriptor");
			materialHash(material);
			materialHash(creator);
			return hash;
		}

		uint64_t VerifyTorus() const {
			uint64_t hash = 14695981039346656037ULL;
			const auto number = [&](double value) {
				if (!std::isfinite(value)) Fail("Torus nonfinite payload");
				hash = HashBytes(hash, std::bit_cast<uint64_t>(value), 8);
			};
			const auto vector = [&](Vector3 value) {
				number(value.X);
				number(value.Y);
				number(value.Z);
			};
			const auto near = [](double actual, double expected) {
				return std::abs(actual - expected) <= 1e-11;
			};
			const auto same = [&](Vector3 a, Vector3 b) {
				return near(a.X, b.X) && near(a.Y, b.Y) && near(a.Z, b.Z);
			};

			const bool smooth = Kind == Family::TorusSmooth31x22;
			const size_t nt = smooth ? 31 : 16, np = smooth ? 22 : 8;
			const double major = 1, minor = .2, angleT = smooth ? .5 : 0, angleP = smooth ? 13 : 0,
						 twist = smooth ? .25 : 0;
			const auto &mesh = std::get<MeshValue3D>(ValueOutput.Data);
			if (!mesh.Data) Fail("Torus missing backing");
			const auto &data = *mesh.Data;
			if (data.Parts.size() != 1 || data.Materials.size() != 1 || data.LocalTransforms.size() != 2 ||
				data.Parts[0].MaterialIndex != 0 || data.Parts[0].Vertices.size() != 6 * nt * np ||
				data.Edges.size() != 4 * nt * np)
				Fail("Torus complete shape");
			const std::array<MeshTransform3D, 2> transforms{
				{{{-1, 2, 3}, {4, 5, 6}, {0, 0, 1, 0}, {-1, 2, 4}},
				 {{2, 3, 4}, {.1, .2, .3}, {.1, .2, .3, .4}, {2, -3, .5}}}
			};
			if (std::get<Vector3>(ControlOutput.Data) != transforms[0].Position)
				Fail("Torus GetData control");
			vector(std::get<Vector3>(ControlOutput.Data));
			for (size_t i = 0; i < transforms.size(); ++i) {
				const auto &actual = data.LocalTransforms[i];
				if (actual != transforms[i]) Fail("Torus local transform order");
				vector(actual.Position);
				vector(actual.Anchor);
				vector(actual.Scale);
				for (double value :
					 {actual.Rotation.X, actual.Rotation.Y, actual.Rotation.Z, actual.Rotation.W})
					number(value);
			}

			// Snap individual lengthdir components before products, using neighboring-integer distances.
			const auto snap = [](double value) {
				const double lower = std::floor(value), upper = lower + 1;
				if (value - lower <= .0001) return lower;
				if (upper - value <= .0001) return upper;
				return value;
			};
			const auto center = [&](double turn) {
				const double angle = (360 * turn + angleT) * std::numbers::pi / 180;
				return Vector3{major * snap(std::cos(angle)), major * snap(-std::sin(angle)), 0};
			};
			const auto point = [&](double turn, double phase) {
				const double angle = (360 * turn + angleT) * std::numbers::pi / 180;
				const double cross = phase * std::numbers::pi / 180;
				const double radial = major + snap(minor * std::cos(cross));
				return Vector3{
					radial * snap(std::cos(angle)),
					radial * snap(-std::sin(angle)),
					snap(-minor * std::sin(cross))
				};
			};
			hash = HashBytes(hash, data.Parts[0].MaterialIndex, 4);
			hash = HashBytes(hash, data.Parts[0].Vertices.size(), 8);
			hash = HashBytes(hash, data.Edges.size(), 8);
			for (size_t i = 0; i < nt; ++i)
				for (size_t j = 0; j < np; ++j) {
					const double t0 = double(i) / nt, t1 = double(i + 1) / nt,
								 p0 = 360 * double(j) / np + angleP, p1 = 360 * double(j + 1) / np + angleP;
					// The right edge has a constant twist offset for every cell, not an accumulated turn.
					const std::array<Vector3, 4> p{
						point(t0, p0),
						point(t1, p0 + 360 * twist / np),
						point(t1, p1 + 360 * twist / np),
						point(t0, p1)
					};
					const std::array<Vector3, 4> c{center(t0), center(t1), center(t1), center(t0)};
					std::array<Vector3, 4> normals{};
					if (smooth)
						for (size_t k = 0; k < 4; ++k)
							normals[k] = {p[k].X - c[k].X, p[k].Y - c[k].Y, p[k].Z};
					else {
						Vector3 average{};
						for (const auto &v : p) {
							average.X += v.X / 4;
							average.Y += v.Y / 4;
							average.Z += v.Z / 4;
						}
						normals.fill(
							{average.X - (c[0].X + c[1].X) / 2, average.Y - (c[0].Y + c[1].Y) / 2, average.Z}
						);
					}
					const std::array<Vector2, 4> uv{
						{{1 - t0, 1 - double(j) / np},
						 {1 - t1, 1 - double(j) / np},
						 {1 - t1, 1 - double(j + 1) / np},
						 {1 - t0, 1 - double(j + 1) / np}}
					};
					constexpr std::array<size_t, 6> order{0, 2, 1, 0, 3, 2};
					for (size_t k = 0; k < order.size(); ++k) {
						const auto &v = data.Parts[0].Vertices[(i * np + j) * 6 + k];
						const auto index = order[k];
						if (!same(v.Position, p[index]) || !same(v.Normal, normals[index]) ||
							!near(v.UV.X, uv[index].X) || !near(v.UV.Y, uv[index].Y) ||
							v.Tint != Colour{255, 255, 255, 255})
							Fail("Torus all ordered vertex fields");
						vector(v.Position);
						vector(v.Normal);
						number(v.UV.X);
						number(v.UV.Y);
						for (uint8_t byte : {v.Tint.Red, v.Tint.Green, v.Tint.Blue, v.Tint.Alpha})
							hash = HashBytes(hash, byte, 1);
					}
					for (size_t k = 0; k < 4; ++k) {
						const auto &edge = data.Edges[(i * np + j) * 4 + k];
						if (!same(edge.From, p[k]) || !same(edge.To, p[(k + 1) % 4]))
							Fail("Torus all ordered edges");
						vector(edge.From);
						vector(edge.To);
					}
				}
			const auto materialHash = [&](const MaterialValue3D &material) {
				const auto &value = material.Get();
				for (double field :
					 {value.TextureScale.X,
					  value.TextureScale.Y,
					  value.TextureShift.X,
					  value.TextureShift.Y,
					  value.TextureFilter,
					  value.Diffuse,
					  value.Specular,
					  value.Shininess,
					  value.Reflectance,
					  value.NormalStrength,
					  value.MetallicRange.X,
					  value.MetallicRange.Y,
					  value.RoughnessRange.X,
					  value.RoughnessRange.Y})
					number(field);
				for (bool field : {value.Metal, value.MetallicMapped, value.RoughnessMapped})
					hash = HashBytes(hash, field, 1);
				for (const auto *image : {&value.Surface, &value.Normal, &value.PropertiesMap}) {
					hash = HashBytes(hash, image->has_value(), 1);
					if (!*image) continue;
					const auto &pixels = **image;
					hash = HashBytes(hash, static_cast<uint8_t>(pixels.Format), 1);
					hash = HashBytes(hash, pixels.Width, 4);
					hash = HashBytes(hash, pixels.Height, 4);
					hash = HashBytes(hash, pixels.Hash, 8);
					for (uint8_t byte : pixels.Pixels)
						hash = HashBytes(hash, byte, 1);
				}
			};
			const auto expectedMaterial = [&](double diffuse) {
				MaterialData3D expected;
				expected.Diffuse = diffuse;
				expected.Surface = Image{1, 1, {0, 0, 0, 0}};
				expected.Surface->Hash = SurfaceHash(*expected.Surface);
				expected.PropertiesMap = Image{32, 32, std::vector<uint8_t>(32 * 32 * 4, 0)};
				for (size_t pixel = 0; pixel < 32 * 32; ++pixel)
					expected.PropertiesMap->Pixels[pixel * 4 + 3] = 255;
				expected.PropertiesMap->Hash = SurfaceHash(*expected.PropertiesMap);
				return expected;
			};

			const auto &creator = std::get<MaterialValue3D>(CreatorOutput.Data);
			const auto &material = data.Materials[0];
			if (creator.Get() != expectedMaterial(.25) || material.Get() != expectedMaterial(.25) ||
				material.Data.operator->() == creator.Data.operator->() ||
				material.Get().Surface->Pixels.data() == creator.Get().Surface->Pixels.data() ||
				material.Get().PropertiesMap->Pixels.data() == creator.Get().PropertiesMap->Pixels.data())
				Fail("Torus complete independently owned material descriptor");
			materialHash(material);
			materialHash(creator);
			return hash;
		}

		uint64_t VerifyCone() const {
			uint64_t hash = 14695981039346656037ULL;
			const auto number = [&](double value) {
				if (!std::isfinite(value)) Fail("Cone nonfinite payload");
				hash = HashBytes(hash, std::bit_cast<uint64_t>(value), 8);
			};
			const auto vector = [&](Vector3 value) {
				number(value.X);
				number(value.Y);
				number(value.Z);
			};
			const auto near = [](double actual, double expected) {
				return std::abs(actual - expected) <= 1e-11;
			};
			const auto same = [&](Vector3 a, Vector3 b) {
				return near(a.X, b.X) && near(a.Y, b.Y) && near(a.Z, b.Z);
			};
			const bool smooth = Kind == Family::ConeBounded104;
			const size_t sides = smooth ? 104 : 8;
			const auto &mesh = std::get<MeshValue3D>(ValueOutput.Data);
			if (!mesh.Data) Fail("Cone missing backing");
			const auto &data = *mesh.Data;
			if (data.Parts.size() != 2 || data.Edges.size() != 2 * sides || data.Materials.size() != 2 ||
				data.LocalTransforms.size() != 2)
				Fail("Cone complete shape");
			const std::array<MeshTransform3D, 2> transforms{
				{{{-1, 2, 3}, {4, 5, 6}, {0, 0, 1, 0}, {-1, 2, 4}},
				 {{2, 3, 4}, {.1, .2, .3}, {.1, .2, .3, .4}, {2, -3, .5}}}
			};
			if (std::get<Vector3>(ControlOutput.Data) != transforms[0].Position) Fail("Cone GetData control");
			vector(std::get<Vector3>(ControlOutput.Data));
			for (size_t i = 0; i < transforms.size(); ++i) {
				const auto &actual = data.LocalTransforms[i];
				if (actual != transforms[i]) Fail("Cone local transform order");
				vector(actual.Position);
				vector(actual.Anchor);
				vector(actual.Scale);
				for (double value :
					 {actual.Rotation.X, actual.Rotation.Y, actual.Rotation.Z, actual.Rotation.W})
					number(value);
			}
			// Derive source angular coordinates directly. Do not call executor helpers or reuse its arrays.
			const auto direction = [](double length, double turn) {
				const double angle = 2 * std::numbers::pi * turn;
				const auto snap = [](double value) {
					const double nearest = std::round(value);
					return std::abs(value - nearest) <= .0001 ? nearest : value;
				};
				return Vector2{snap(length * std::cos(angle)), snap(-length * std::sin(angle))};
			};
			constexpr double nz = .25 / (.25 + 1);
			for (size_t partIndex = 0; partIndex < 2; ++partIndex) {
				const auto &part = data.Parts[partIndex];
				if (part.MaterialIndex != partIndex || part.Vertices.size() != 3 * sides)
					Fail("Cone ordered part and material index");
				hash = HashBytes(hash, part.MaterialIndex, 4);
				hash = HashBytes(hash, part.Vertices.size(), 8);
				for (size_t side = 0; side < sides; ++side) {
					const double u0 = double(side) / double(sides), u1 = double(side + 1) / double(sides);
					const auto a = direction(.5, u0), b = direction(.5, u1),
							   middle = direction(1, (u0 + u1) / 2);
					const Vector3 a0{a.X, a.Y, -.5}, b0{b.X, b.Y, -.5};
					std::array<MeshVertex3D, 3> expected{};
					if (partIndex == 0) {
						expected = {
							{{{0, 0, -.5}, {0, 0, -1}, {.5, .5}},
							 {b0, {0, 0, -1}, {.5 + b.X, .5 + b.Y}},
							 {a0, {0, 0, -1}, {.5 + a.X, .5 + a.Y}}}
						};
					} else {
						const auto na = smooth ? direction(1, u0) : middle;
						const auto nb = smooth ? direction(1, u1) : middle;
						expected = {
							{{{0, 0, .5}, {middle.X, middle.Y, nz}, {(u0 + u1) / 2, 0}},
							 {a0, {na.X, na.Y, nz}, {u0, 1}},
							 {b0, {nb.X, nb.Y, nz}, {u1, 1}}}
						};
					}
					for (size_t corner = 0; corner < expected.size(); ++corner) {
						const auto &actual = part.Vertices[3 * side + corner];
						const auto &wanted = expected[corner];
						if (!same(actual.Position, wanted.Position) || !same(actual.Normal, wanted.Normal) ||
							!near(actual.UV.X, wanted.UV.X) || !near(actual.UV.Y, wanted.UV.Y) ||
							actual.Tint != Colour{255, 255, 255, 255})
							Fail("Cone all ordered vertex fields");
						vector(actual.Position);
						vector(actual.Normal);
						number(actual.UV.X);
						number(actual.UV.Y);
						for (uint8_t byte :
							 {actual.Tint.Red, actual.Tint.Green, actual.Tint.Blue, actual.Tint.Alpha})
							hash = HashBytes(hash, byte, 1);
					}
				}
			}
			for (size_t side = 0; side < sides; ++side) {
				const double u0 = double(side) / double(sides), u1 = double(side + 1) / double(sides);
				const auto a = direction(.5, u0), b = direction(.5, u1);
				const std::array<MeshEdge3D, 2> expected{
					{{{a.X, a.Y, -.5}, {b.X, b.Y, -.5}}, {{a.X, a.Y, -.5}, {0, 0, .5}}}
				};
				for (size_t edge = 0; edge < expected.size(); ++edge) {
					const size_t index = edge == 0 ? side : sides + side;
					const auto &actual = data.Edges[index];
					if (!same(actual.From, expected[edge].From) || !same(actual.To, expected[edge].To))
						Fail("Cone all ordered edge endpoints");
					vector(actual.From);
					vector(actual.To);
				}
			}
			const auto materialHash = [&](const MaterialValue3D &material) {
				const auto &value = material.Get();
				for (double field :
					 {value.TextureScale.X,
					  value.TextureScale.Y,
					  value.TextureShift.X,
					  value.TextureShift.Y,
					  value.TextureFilter,
					  value.Diffuse,
					  value.Specular,
					  value.Shininess,
					  value.Reflectance,
					  value.NormalStrength,
					  value.MetallicRange.X,
					  value.MetallicRange.Y,
					  value.RoughnessRange.X,
					  value.RoughnessRange.Y})
					number(field);
				for (bool field : {value.Metal, value.MetallicMapped, value.RoughnessMapped})
					hash = HashBytes(hash, field, 1);
				for (const auto *image : {&value.Surface, &value.Normal, &value.PropertiesMap}) {
					hash = HashBytes(hash, image->has_value(), 1);
					if (!*image) continue;
					const auto &pixels = **image;
					hash = HashBytes(hash, static_cast<uint8_t>(pixels.Format), 1);
					hash = HashBytes(hash, pixels.Width, 4);
					hash = HashBytes(hash, pixels.Height, 4);
					hash = HashBytes(hash, pixels.Hash, 8);
					for (uint8_t byte : pixels.Pixels)
						hash = HashBytes(hash, byte, 1);
				}
			};
			const auto expectedMaterial = [&](double diffuse) {
				MaterialData3D expected;
				expected.Diffuse = diffuse;
				expected.Surface = Image{1, 1, {0, 0, 0, 0}};
				expected.Surface->Hash = SurfaceHash(*expected.Surface);
				expected.PropertiesMap = Image{32, 32, std::vector<uint8_t>(32 * 32 * 4, 0)};
				for (size_t pixel = 0; pixel < 32 * 32; ++pixel)
					expected.PropertiesMap->Pixels[pixel * 4 + 3] = 255;
				expected.PropertiesMap->Hash = SurfaceHash(*expected.PropertiesMap);
				return expected;
			};
			const auto &creator = std::get<MaterialValue3D>(CreatorOutput.Data);
			if (creator.Get() != expectedMaterial(.75) || data.Materials[0].Get() != expectedMaterial(.25) ||
				data.Materials[1].Get() != expectedMaterial(.75) ||
				data.Materials[0].Data.operator->() == data.Materials[1].Data.operator->() ||
				data.Materials[1].Data.operator->() == creator.Data.operator->())
				Fail("Cone ordered owned material descriptors");
			for (size_t i = 0; i < data.Materials.size(); ++i) {
				const auto &material = data.Materials[i];
				if (!material.Get().Surface || !material.Get().PropertiesMap)
					Fail("Cone material image fields");
				if (i == 1 &&
					(material.Get().Surface->Pixels.data() == creator.Get().Surface->Pixels.data() ||
					 material.Get().PropertiesMap->Pixels.data() ==
						 creator.Get().PropertiesMap->Pixels.data()))
					Fail("Cone retained material clone storage");
				materialHash(material);
			}
			materialHash(creator);
			vector(std::get<Vector3>(ControlOutput.Data));
			return hash;
		}

		uint64_t VerifyCylinder() const {
			uint64_t hash = 14695981039346656037ULL;
			const auto number = [&](double value) {
				if (!std::isfinite(value)) Fail("Cylinder nonfinite payload");
				hash = HashBytes(hash, std::bit_cast<uint64_t>(value), 8);
			};
			const auto vector = [&](Vector3 value) {
				number(value.X);
				number(value.Y);
				number(value.Z);
			};
			const auto near = [](double actual, double expected) {
				return std::abs(actual - expected) <= 1e-11;
			};
			const auto same = [&](Vector3 a, Vector3 b) {
				return near(a.X, b.X) && near(a.Y, b.Y) && near(a.Z, b.Z);
			};
			const bool shaped = Kind == Family::CylinderProfile;
			const size_t sides = shaped ? 16 : 8, segments = shaped ? 12 : 1;
			const auto &mesh = std::get<MeshValue3D>(ValueOutput.Data);
			if (!mesh.Data) Fail("Cylinder missing backing");
			const auto &data = *mesh.Data;
			if (data.Parts.size() != 3 || data.Edges.size() != 2 * sides * (segments + 1) ||
				data.Materials.size() != 3 || data.LocalTransforms.size() != 2)
				Fail("Cylinder complete shape");
			const std::array<MeshTransform3D, 2> transforms{
				{{{-1, 2, 3}, {.25, .5, .75}, {0, 0, 1, 0}, {2, 3, 4}},
				 {{1, 2, 3}, {-1, -2, -3}, {.5, .5, .5, .5}, {-1, 2, .5}}}
			};
			if (std::get<Vector3>(ControlOutput.Data) != transforms[0].Position)
				Fail("Cylinder GetData control");
			vector(std::get<Vector3>(ControlOutput.Data));
			for (size_t i = 0; i < 2; ++i) {
				const auto &t = data.LocalTransforms[i];
				if (t != transforms[i]) Fail("Cylinder local chain");
				vector(t.Position);
				vector(t.Anchor);
				vector(t.Scale);
				for (double v : {t.Rotation.X, t.Rotation.Y, t.Rotation.Z, t.Rotation.W})
					number(v);
			}
			// Independent cylindrical coordinates and linear radius law, without executor/sampler helpers.
			const auto point = [&](size_t side, size_t level) {
				const double angle = 2 * std::numbers::pi * double(side) / double(sides),
							 radius = shaped ? .25 + .5 * double(level) / double(segments) : .5;
				return Vector3{
					radius * std::cos(angle),
					-radius * std::sin(angle),
					-.5 + double(level) / double(segments)
				};
			};
			const double slope = shaped ? 1.0 / double(segments) : 0,
						 normalLength = std::sqrt(1 + slope * slope);
			for (size_t partIndex = 0; partIndex < 3; ++partIndex) {
				const auto &part = data.Parts[partIndex];
				const size_t count = partIndex == 0 ? 6 * sides * segments : 3 * sides;
				if (part.MaterialIndex != partIndex || part.Vertices.size() != count)
					Fail("Cylinder part count/material");
				hash = HashBytes(hash, part.MaterialIndex, 4);
				hash = HashBytes(hash, part.Vertices.size(), 8);
				for (size_t index = 0; index < count; ++index) {
					Vector3 position{}, normal{};
					Vector2 uv{};
					if (partIndex == 0) {
						const size_t cell = index / 6, side = cell / segments, level = cell % segments,
									 corner = index % 6;
						constexpr std::array<size_t, 6> right{0, 0, 1, 0, 1, 1}, upper{1, 0, 1, 0, 0, 1};
						const size_t endpoint = side + right[corner], vertical = level + upper[corner];
						position = point(endpoint, vertical);
						const double v0 = double(level) * (1.0 / double(segments));
						uv = {
							double(endpoint) / double(sides),
							v0 + (upper[corner] ? 1.0 / double(segments) : 0)
						};
						position.Z = -.5 + uv.Y;
						const double angle = 2 * std::numbers::pi *
											 (shaped ? double(endpoint) : double(side) + .5) / double(sides);
						normal = {
							std::cos(angle) / normalLength,
							-std::sin(angle) / normalLength,
							slope / normalLength
						};
					} else {
						const size_t side = index / 3, corner = index % 3,
									 level = partIndex == 1 ? segments : 0;
						normal = {0, 0, partIndex == 1 ? 1.0 : -1.0};
						if (corner == 0) {
							position = {0, 0, partIndex == 1 ? .5 : -.5};
							uv = {.5, .5};
						} else {
							const size_t endpoint = side + (partIndex == 1 ? (corner == 2) : (corner == 1));
							position = point(endpoint, level);
							const double angle = 2 * std::numbers::pi * double(endpoint) / double(sides);
							uv = {.5 + .5 * std::cos(angle), .5 - .5 * std::sin(angle)};
						}
					}
					const auto &v = part.Vertices[index];
					if (!same(v.Position, position) || !same(v.Normal, normal) || !near(v.UV.X, uv.X) ||
						!near(v.UV.Y, uv.Y) || v.Tint != Colour{255, 255, 255, 255})
						Fail("Cylinder all-point analytical fields");
					vector(v.Position);
					vector(v.Normal);
					number(v.UV.X);
					number(v.UV.Y);
					for (uint8_t byte : {v.Tint.Red, v.Tint.Green, v.Tint.Blue, v.Tint.Alpha})
						hash = HashBytes(hash, byte, 1);
				}
			}
			for (size_t i = 0; i < data.Edges.size(); ++i) {
				MeshEdge3D expected;
				if (i < 2 * sides) {
					const size_t side = i / 2, level = i % 2 == 0 ? segments : 0;
					expected = {point(side, level), point(side + 1, level)};
				} else {
					const size_t index = i - 2 * sides, cell = index / 2, side = cell / segments + index % 2,
								 level = cell % segments;
					expected = {point(side, level), point(side, level + 1)};
					const double v0 = double(level) * (1.0 / double(segments));
					expected.From.Z = -.5 + v0;
					expected.To.Z = -.5 + v0 + 1.0 / double(segments);
				}
				if (!same(data.Edges[i].From, expected.From) || !same(data.Edges[i].To, expected.To))
					Fail("Cylinder all-edge analytical fields");
				vector(data.Edges[i].From);
				vector(data.Edges[i].To);
			}
			const auto &created = std::get<MaterialValue3D>(CreatorOutput.Data);
			MaterialData3D expected;
			expected.Diffuse = .25;
			expected.Surface = Image{1, 1, {0, 0, 0, 0}};
			expected.Surface->Hash = SurfaceHash(*expected.Surface);
			expected.PropertiesMap = Image{32, 32, std::vector<uint8_t>(32 * 32 * 4, 0)};
			for (size_t i = 0; i < 32 * 32; ++i)
				expected.PropertiesMap->Pixels[i * 4 + 3] = 255;
			expected.PropertiesMap->Hash = SurfaceHash(*expected.PropertiesMap);
			if (created.Get() != expected) Fail("Cylinder complete Material descriptor");
			const auto materialHash = [&](const MaterialValue3D &material) {
				const auto &m = material.Get();
				for (double value :
					 {m.TextureScale.X,
					  m.TextureScale.Y,
					  m.TextureShift.X,
					  m.TextureShift.Y,
					  m.TextureFilter,
					  m.Diffuse,
					  m.Specular,
					  m.Shininess,
					  m.Reflectance,
					  m.NormalStrength,
					  m.MetallicRange.X,
					  m.MetallicRange.Y,
					  m.RoughnessRange.X,
					  m.RoughnessRange.Y})
					number(value);
				for (bool value : {m.Metal, m.MetallicMapped, m.RoughnessMapped})
					hash = HashBytes(hash, value, 1);
				for (const auto *image : {&m.Surface, &m.Normal, &m.PropertiesMap}) {
					hash = HashBytes(hash, image->has_value(), 1);
					if (!*image) continue;
					const auto &v = **image;
					hash = HashBytes(hash, static_cast<uint8_t>(v.Format), 1);
					hash = HashBytes(hash, v.Width, 4);
					hash = HashBytes(hash, v.Height, 4);
					hash = HashBytes(hash, v.Hash, 8);
					for (uint8_t byte : v.Pixels)
						hash = HashBytes(hash, byte, 1);
				}
			};
			materialHash(created);
			for (const auto &material : data.Materials) {
				if (material != created || material.Data.operator->() == created.Data.operator->() ||
					material.Get().Surface->Pixels.data() == created.Get().Surface->Pixels.data())
					Fail("Cylinder independent owned Material clone");
				materialHash(material);
			}
			return hash;
		}

		uint64_t Verify() const {
			uint64_t hash = 14695981039346656037ULL;
			if (IsAudioWindow()) {
				AudioObservation->VerifyInputs();
				for (size_t i = 0; i < AudioWindows.size(); ++i) {
					const size_t clock = i;
					hash = HashBytes(
						hash,
						AudioObservation->Verify(
							AudioWindows[i], clock >= 2 ? 1 : 0, AudioWindowClocks[clock]
						),
						8
					);
				}
				return hash;
			}
			if (Kind == Family::WavTimeline) return VerifyWavTimeline();
			if (IsCube()) return VerifyCube();
			if (IsCylinder()) return VerifyCylinder();
			if (IsCone()) return VerifyCone();
			if (IsTorus()) return VerifyTorus();
			if (IsIcosphere()) return VerifyIcosphere();
			if (IsUVSphere()) return VerifyUVSphere();
			if (IsHdr()) {
				const auto inputFormat = ImageSources.front().Data.Format;
				const auto outputFormat =
					Kind == Family::HdrDirectional ? SurfaceFormat::RGBA32Float : SurfaceFormat::RGBA16Float;
				const auto layout = CheckedSurfaceLayout(128, 128, outputFormat, Limits::MaximumOutputBytes);
				if (!layout || HdrOutput.Width != 128 || HdrOutput.Height != 128 ||
					HdrOutput.Format != outputFormat || HdrOutput.Pixels.size() != layout->Bytes)
					Fail("HDR output shape and format");
				// Size1 gives one unit-weight tap per Gaussian pass; its alpha seed attenuates RGB.
				// Near half rounding midpoints, pass attenuation and conversion order affect the final bytes.
				// Quantize after each input-format pass, then convert to the selected output format.
				// Constant-field Directional/Zoom with fade/gamma off preserves that colour at all17 taps.
				SurfacePixel expected;
				if (!LoadSurfacePixel(ImageSources.front().Data, 0, 0, expected)) Fail("HDR input decode");
				for (size_t pass = 0; pass < 3; ++pass) {
					const auto format = pass < 2 ? inputFormat : outputFormat;
					if (pass < 2)
						for (size_t channel = 0; channel < 3; ++channel)
							expected[channel] /= 1 + 1e-5;
					const auto quantizedLayout =
						CheckedSurfaceLayout(1, 1, format, Limits::MaximumOutputBytes);
					if (!quantizedLayout) Fail("HDR oracle layout");
					Image quantized{1, 1, std::vector<uint8_t>(quantizedLayout->Bytes), 0, format};
					if (!StoreSurfacePixel(quantized, 0, 0, expected) ||
						!LoadSurfacePixel(quantized, 0, 0, expected))
						Fail("HDR oracle quantization");
				}
				for (uint32_t y = 0; y < 128; ++y)
					for (uint32_t x = 0; x < 128; ++x) {
						SurfacePixel actual;
						if (!LoadSurfacePixel(HdrOutput, x, y, actual)) Fail("HDR output pixel");
						for (size_t channel = 0; channel < 4; ++channel)
							if (!std::isfinite(actual[channel]) ||
								std::abs(actual[channel] - expected[channel]) > 1e-6)
								Fail("HDR analytical colour");
					}
				hash = HashBytes(hash, static_cast<uint8_t>(HdrOutput.Format), 1);
				hash = HashBytes(hash, HdrOutput.Width, 4);
				hash = HashBytes(hash, HdrOutput.Height, 4);
				for (uint8_t byte : HdrOutput.Pixels)
					hash = HashBytes(hash, byte, 1);
			} else if (Kind == Family::Vector) {
				const auto &vectors = std::get<ArrayValue>(CreatorOutput.Data);
				const auto &products = std::get<ArrayValue>(ControlOutput.Data);
				const auto &lengths = std::get<ArrayValue>(ValueOutput.Data);
				if (vectors.ElementType != ValueType::Vector2 || vectors.Elements.size() != 1024 ||
					!vectors.Nested.empty() || products.ElementType != ValueType::Scalar ||
					!products.Elements.empty() || products.Nested.size() != 1024 ||
					lengths.ElementType != ValueType::Scalar || lengths.Elements.size() != 1024 ||
					!lengths.Nested.empty())
					Fail("vector workflow shape");
				for (size_t index = 0; index < 1024; index++) {
					const double k = static_cast<double>(index + 1);
					const auto &vector = std::get<Vector2>(vectors.Elements[index]);
					if (vector != Vector2{3 * k, 4 * k} || products.Nested[index].size() != 2)
						Fail("vector creator or product shape");
					const double x = std::get<double>(products.Nested[index][0]);
					const double y = std::get<double>(products.Nested[index][1]);
					const double length = std::get<double>(lengths.Elements[index]);
					if (x != 6 * k || y != 8 * k || length != 10 * k) Fail("vector product or length");
					for (double value : {vector.X, vector.Y, x, y, length})
						hash = HashBytes(hash, std::bit_cast<uint64_t>(value), 8);
				}
			} else if (Kind == Family::ScalarMath) {
				const auto &numbers = std::get<ArrayValue>(ControlOutput.Data);
				const auto &booleans = std::get<ArrayValue>(ValueOutput.Data);
				if (numbers.ElementType != ValueType::Scalar || !numbers.Nested.empty() ||
					numbers.Elements.size() != 4096 || booleans.ElementType != ValueType::Boolean ||
					!booleans.Nested.empty() || booleans.Elements.size() != 4096)
					Fail("math/compare shape");
				for (size_t index = 0; index < 4096; index++) {
					const double number = std::get<double>(numbers.Elements[index]);
					const bool boolean = std::get<bool>(booleans.Elements[index]);
					if (number != static_cast<double>(index) / 2 || boolean != (index >= 2048))
						Fail("math/compare values");
					hash = HashBytes(hash, std::bit_cast<uint64_t>(number), 8);
					hash = HashBytes(hash, boolean, 1);
				}
			} else if (Kind == Family::Curve) {
				const auto &curve = std::get<engine::imagegraph::Curve>(ControlOutput.Data);
				if (curve.Header != std::array<double, 6>{0, 1, 0, 0, 1, 0} || curve.Anchors.size() != 65)
					Fail("curve control shape");
				for (double value : curve.Header)
					hash = HashBytes(hash, std::bit_cast<uint64_t>(value), 8);
				for (size_t index = 0; index < 65; index++) {
					const double x = static_cast<double>(index) / 64;
					const std::array<double, 6> expected{0, 0, x, std::abs(2 * x - 1), 0, 0};
					if (curve.Anchors[index] != expected) Fail("curve control anchors");
					for (double value : curve.Anchors[index])
						hash = HashBytes(hash, std::bit_cast<uint64_t>(value), 8);
				}
				const auto &samples = std::get<ArrayValue>(ValueOutput.Data);
				if (samples.ElementType != ValueType::Scalar || !samples.Nested.empty() ||
					samples.Elements.size() != 256)
					Fail("curve sample shape");
				for (size_t index = 0; index < 256; index++) {
					const double value = std::get<double>(samples.Elements[index]);
					const double x = static_cast<double>(index % 65) / 64;
					if (value != 10 + 10 * std::abs(2 * x - 1)) Fail("curve samples");
					hash = HashBytes(hash, std::bit_cast<uint64_t>(value), 8);
				}
			} else if (Kind == Family::Matrix) {
				const auto &matrix = std::get<MatrixValue>(ValueOutput.Data);
				if (matrix.Columns != 32 || matrix.Rows != 32 || matrix.Values.size() != 1024)
					Fail("matrix shape");
				for (size_t index = 0; index < matrix.Values.size(); index++) {
					if (matrix.Values[index] != static_cast<double>(index)) Fail("matrix product");
					hash = HashBytes(hash, std::bit_cast<uint64_t>(matrix.Values[index]), 8);
				}
			} else if (Kind == Family::WavControls) {
				if (Controls.SourceId != "bounded.wav" || !Controls.Play || Controls.Gain != .525 ||
					Controls.ShiftSeconds != 0 || Controls.SampleRate != 48000 ||
					Controls.Frames != Limits::MaximumAudioClipSamples)
					Fail("WAV controls");
				for (unsigned char byte : Controls.SourceId)
					hash = HashBytes(hash, byte, 1);
				hash = HashBytes(hash, Controls.Play, 1);
				hash = HashBytes(hash, std::bit_cast<uint64_t>(Controls.Gain), 8);
				hash = HashBytes(hash, std::bit_cast<uint64_t>(Controls.ShiftSeconds), 8);
				hash = HashBytes(hash, Controls.SampleRate, 4);
				hash = HashBytes(hash, Controls.Frames, 8);
			} else if (Kind == Family::Solid) {
				if (ImageOutput.Images.size() != 8 || ImageOutput.Items.size() != 8) Fail("image count");
				for (size_t index = 0; index < 8; index++) {
					const auto &image = ImageOutput.Images[index];
					if (std::get<size_t>(ImageOutput.Items[index].Data) != index || image.Width != 256 ||
						image.Height != 256 || image.Pixels.size() != 256 * 256 * 4)
						Fail("image shape");
					const uint8_t expected[]{static_cast<uint8_t>(255 - index * 16), 223, 191, 255};
					for (size_t byte = 0; byte < image.Pixels.size(); byte++) {
						if (image.Pixels[byte] != expected[byte % 4]) Fail("image pixels");
						hash = HashBytes(hash, image.Pixels[byte], 1);
					}
				}
			} else {
				const auto &array = std::get<ArrayValue>(ValueOutput.Data);
				if (Kind == Family::Audio) {
					if (array.ElementType != ValueType::Scalar || !array.Elements.empty() ||
						array.Nested.size() != 2)
						Fail("audio shape");
					for (size_t channel = 0; channel < 2; channel++) {
						if (array.Nested[channel].size() != 513) Fail("FFT bins");
						for (size_t bin = 0; bin < 513; bin++) {
							const double value = std::get<double>(array.Nested[channel][bin]);
							if (value != (bin == 512 ? 1024.0 * (channel + 1) : 0.0)) Fail("FFT magnitude");
							hash = HashBytes(hash, std::bit_cast<uint64_t>(value), 8);
						}
					}
				} else {
					if (array.ElementType != ValueType::Colour || !array.Nested.empty() ||
						array.Elements.size() != 1024)
						Fail("gradient shape");
					for (size_t index = 0; index < 1024; index++) {
						const uint8_t level = index % 2 ? 128 : 0;
						const Colour expected{level, level, level, 255};
						if (std::get<Colour>(array.Elements[index]) != expected) Fail("gradient colour");
						for (uint8_t byte : {level, level, level, uint8_t{255}})
							hash = HashBytes(hash, byte, 1);
					}
				}
			}
			return hash;
		}
	};
}
