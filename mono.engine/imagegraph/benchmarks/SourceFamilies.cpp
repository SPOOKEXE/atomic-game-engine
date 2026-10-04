// Evaluation frames retain complete span hierarchy. Verification and reporting occur outside the frame.
#include "../tests/fixtures/SourceFamilyEvaluation.hpp"

#include <engine/core/FrameGraph.hpp>
#include <engine/core/HeapProfile.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/testing/Bench.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.imagegraph.bench.source-families")

namespace {
	using engine::core::FrameGraph;
	using engine::core::HeapProfile;
	using engine::core::Metrics;
	using Fixture = engine::imagegraph::testing::SourceFamilyFixture;
	struct SpanRecord {
		std::string Name;
		engine::core::FrameSpan Span;
	};
	struct Reading {
		float OwnerMilliseconds = 0;
		std::vector<SpanRecord> Spans;
		std::vector<engine::core::Counter> Counters;
		engine::core::HeapTotals HeapBefore, HeapAfter;
		std::vector<engine::core::HeapNodeView> HeapNodesBefore, HeapNodesAfter;
	};
	struct ProfileFixture {
		Fixture Graph;
		std::array<Reading, 13> Readings{};
		size_t Count = 0;
		explicit ProfileFixture(Fixture::Family family) : Graph(family) {}
		~ProfileFixture() {
			for (size_t call = 0; call < Count; call++) {
				const auto &record = Readings[call];
				std::printf(
					"# source-profile family=%d call=%zu input_fnv=%llu output_fnv=%llu owner_ms=%.6f "
					"spans=%zu heap_compiled=%d allocated_bytes=%llu allocated_blocks=%llu live_bytes=%lld "
					"live_blocks=%lld peak_bytes=%lld overhead_bytes=%lld\n",
					static_cast<int>(Graph.Kind),
					call + 1,
					static_cast<unsigned long long>(Graph.InputHash),
					static_cast<unsigned long long>(Graph.OutputHash),
					record.OwnerMilliseconds,
					record.Spans.size(),
					HeapProfile::IsCompiledIn(),
					static_cast<unsigned long long>(
						record.HeapAfter.TotalBytes - record.HeapBefore.TotalBytes
					),
					static_cast<unsigned long long>(
						record.HeapAfter.TotalBlocks - record.HeapBefore.TotalBlocks
					),
					static_cast<long long>(record.HeapAfter.LiveBytes),
					static_cast<long long>(record.HeapAfter.LiveBlocks),
					static_cast<long long>(record.HeapAfter.PeakBytes),
					static_cast<long long>(record.HeapAfter.OverheadBytes)
				);
				for (size_t index = 0; index < record.Spans.size(); index++) {
					const auto &span = record.Spans[index];
					std::printf(
						"# source-span family=%d call=%zu index=%zu parent=%u depth=%u start_ms=%.6f "
						"inclusive_ms=%.6f self_ms=%.6f idle_ms=%.6f reported=%d name=%s\n",
						static_cast<int>(Graph.Kind),
						call + 1,
						index,
						span.Span.Parent,
						span.Span.Depth,
						span.Span.StartMilliseconds,
						span.Span.Milliseconds,
						span.Span.SelfMilliseconds,
						span.Span.IdleMilliseconds,
						span.Span.Reported,
						span.Name.c_str()
					);
				}
				for (const auto &counter : record.Counters)
					std::printf(
						"# source-counter family=%d call=%zu name=%s value=%.0f samples=%u\n",
						static_cast<int>(Graph.Kind),
						call + 1,
						counter.Name.Text().data(),
						counter.Value,
						counter.Samples
					);
				for (size_t index = 0; index < record.HeapNodesAfter.size(); index++) {
					const auto &after = record.HeapNodesAfter[index];
					const auto before = index < record.HeapNodesBefore.size() ? record.HeapNodesBefore[index]
																			  : engine::core::HeapNodeView{};
					if (after.TotalBytes == before.TotalBytes && after.LiveBytes == before.LiveBytes &&
						after.TotalBlocks == before.TotalBlocks)
						continue;
					std::printf(
						"# source-heap family=%d call=%zu index=%zu parent=%u depth=%u allocated_bytes=%llu "
						"allocated_blocks=%llu live_bytes=%lld live_blocks=%lld peak_bytes=%lld name=%.*s\n",
						static_cast<int>(Graph.Kind),
						call + 1,
						index,
						after.Parent,
						after.Depth,
						static_cast<unsigned long long>(after.TotalBytes - before.TotalBytes),
						static_cast<unsigned long long>(after.TotalBlocks - before.TotalBlocks),
						static_cast<long long>(after.LiveBytes),
						static_cast<long long>(after.LiveBlocks),
						static_cast<long long>(after.PeakBytes),
						static_cast<int>(after.Name.size()),
						after.Name.data()
					);
				}
			}
		}
		void Measure() {
			if (Count == Readings.size())
				throw std::runtime_error(
					"source-family benchmark supports at most five samples after eight warmups"
				);
			struct Restore {
				bool Enabled = FrameGraph::IsEnabled();
				~Restore() {
					FrameGraph::SetEnabled(Enabled);
				}
			} restore;
			Metrics::Drain();
			FrameGraph::SetEnabled(true);
			if (Graph.Kind == Fixture::Family::WavTimeline && Graph.WavTimelineInputHash() != Graph.InputHash)
				Graph.Fail("waveform before-input hash");
			if (Graph.IsAudioWindow()) Graph.AudioObservation->VerifyInputs();
			Reading &record = Readings[Count];
			// Snapshot storage belongs to the recorder, outside the measured allocation interval.
			record.HeapNodesBefore.reserve(HeapProfile::MAXIMUM_NODES);
			record.HeapNodesAfter.reserve(HeapProfile::MAXIMUM_NODES);
			for (uint32_t index = 0; index < HeapProfile::NodeCount(); index++)
				record.HeapNodesBefore.push_back(HeapProfile::Node(index));
			record.HeapBefore = HeapProfile::Totals();
			FrameGraph::BeginFrame();
			try {
				Graph.Evaluate();
			} catch (...) {
				// Close exceptional evaluations before restoring the frame collector setting.
				FrameGraph::EndFrame();
				throw;
			}
			FrameGraph::EndFrame();
			record.HeapAfter = HeapProfile::Totals();
			for (uint32_t index = 0; index < HeapProfile::NodeCount(); index++)
				record.HeapNodesAfter.push_back(HeapProfile::Node(index));
			uint64_t taggedBytes = 0;
			uint64_t taggedBlocks = 0;
			for (size_t index = 0; index < record.HeapNodesAfter.size(); index++) {
				const auto &after = record.HeapNodesAfter[index];
				const auto before = index < record.HeapNodesBefore.size() ? record.HeapNodesBefore[index]
																		  : engine::core::HeapNodeView{};
				taggedBytes += after.TotalBytes - before.TotalBytes;
				taggedBlocks += after.TotalBlocks - before.TotalBlocks;
			}
			if (taggedBytes != record.HeapAfter.TotalBytes - record.HeapBefore.TotalBytes ||
				taggedBlocks != record.HeapAfter.TotalBlocks - record.HeapBefore.TotalBlocks)
				throw std::runtime_error("source-family exclusive heap tags disagree with process totals");
			record.OwnerMilliseconds = FrameGraph::FrameMilliseconds();
			if (FrameGraph::Dropped() || record.HeapAfter.DroppedScopes != record.HeapBefore.DroppedScopes)
				throw std::runtime_error("source-family profile dropped frame or heap scopes");
			const auto required = [&](std::string_view name) {
				return std::any_of(
					FrameGraph::Spans().begin(), FrameGraph::Spans().end(), [&](const auto &span) {
						return span.Name == name;
					}
				);
			};
			// A full mono clip requires eight MiB; control resolution must stay below that
			// even with compilation and animated-input temporaries included.
			if (Graph.Kind == Fixture::Family::WavControls && HeapProfile::IsCompiledIn() &&
				record.HeapAfter.TotalBytes - record.HeapBefore.TotalBytes >=
					engine::imagegraph::Limits::MaximumAudioClipSamples * sizeof(double))
				throw std::runtime_error("WAV control resolution materialized clip-sized storage");
			if (!(Graph.IsAudioWindow()						   ? required("imagegraph.audio_window.observe")
				  : Graph.Kind == Fixture::Family::WavTimeline ? required("imagegraph.wav_timeline.observe")
															   : required("imagegraph.evaluate")) ||
				!required("imagegraph.processor") ||
				(Graph.Kind == Fixture::Family::WavTimeline &&
				 !required("imagegraph.wav_timeline.geometry")) ||
				(Graph.Kind == Fixture::Family::Audio && !required("imagegraph.node.audio")) ||
				(Graph.Kind == Fixture::Family::Gradient && !required("imagegraph.node.gradient")) ||
				((Graph.Kind == Fixture::Family::ScalarMath || Graph.Kind == Fixture::Family::Curve ||
				  Graph.Kind == Fixture::Family::Vector) &&
				 !required("imagegraph.node.other")) ||
				((Graph.IsCube() || Graph.IsCylinder() || Graph.IsCone() || Graph.IsTorus() ||
				  Graph.IsUVSphere() || Graph.IsIcosphere()) &&
				 (!required("imagegraph.node.other") || !required("imagegraph.mesh.material") ||
				  !required(
					  Graph.IsCube()	   ? "imagegraph.mesh.cube"
					  : Graph.IsCylinder() ? "imagegraph.mesh.cylinder"
					  : Graph.IsCone()	   ? "imagegraph.mesh.cone"
					  : Graph.IsTorus()	   ? "imagegraph.mesh.torus"
					  : Graph.IsUVSphere() ? "imagegraph.mesh.sphere_uv"
										   : "imagegraph.mesh.sphere_ico"
				  ) ||
				  !required("imagegraph.mesh.transform") || !required("imagegraph.mesh.get_data"))) ||
				(Graph.IsHdr() &&
				 (!required("imagegraph.node.other") || !required("imagegraph.surface.scratch") ||
				  !required("imagegraph.image.allocate"))) ||
				(Graph.IsSourceImage() &&
				 (!required("imagegraph.source." + std::string(Graph.SourceNodeName())) ||
				  !required("imagegraph.image.allocate"))) ||
				(Graph.FilterProfile &&
				 (!required(
					  Graph.FilterProfile->Variant < 3 ? "imagegraph.source.kuwahara"
													   : "imagegraph.source.blobify"
				  ) ||
				  !required("imagegraph.surface.scratch") || !required("imagegraph.image.allocate"))) ||
				(Graph.Kind == Fixture::Family::BitmapTextEightRows &&
				 (!required("imagegraph.bitmap_font") || !required("imagegraph.text.prepare_batch") ||
				  !required("imagegraph.text.prepare_row") || !required("imagegraph.text.admit_batch") ||
				  !required("imagegraph.text") || !required("imagegraph.image.allocate"))) ||
				(Graph.Kind == Fixture::Family::Solid &&
				 (!required("imagegraph.node.generate") || !required("imagegraph.node.filter") ||
				  !required("imagegraph.image.allocate")))) {
				std::string message = "source-family profile missed a required evaluation phase family=" +
									  std::to_string(static_cast<int>(Graph.Kind)) + " observed=";
				// Bound failure diagnostics independently of any unexpected profiler cardinality.
				size_t count = 0;
				for (const auto &span : FrameGraph::Spans()) {
					if (count++ == 128) {
						message += "[truncated]";
						break;
					}
					message += std::string(span.Name) + ";";
				}
				throw std::runtime_error(message);
			}
			for (const auto &span : FrameGraph::Spans())
				record.Spans.push_back({std::string(span.Name), span});
			record.Counters = Metrics::Drain();
			if (Graph.FilterProfile) {
				const auto value = [&](std::string_view name) {
					for (const auto &counter : record.Counters)
						if (counter.Name.Text() == name) return counter.Value;
					throw std::runtime_error("filter profile missing actual boundary counter");
				};
				if (value("imagegraph.node.executions") <
						engine::imagegraph::testing::SourceFilterWorkload::Rows ||
					value("imagegraph.image.allocations") <
						engine::imagegraph::testing::SourceFilterWorkload::Rows ||
					value("imagegraph.image.allocated_payload_bytes") <
						engine::imagegraph::testing::SourceFilterWorkload::Rows * 16 * 16 * 4)
					throw std::runtime_error("filter profile misses executed rows or output bytes");
			}

			if (Graph.IsAudioWindow()) {
				size_t owners = 0, geometry = 0;
				for (size_t i = 0; i < FrameGraph::Spans().size(); ++i) {
					const auto &span = FrameGraph::Spans()[i];
					if (span.Reported) throw std::runtime_error("audio observation reported span");
					if (span.Parent == FrameGraph::NO_PARENT) {
						if (span.Name != "imagegraph.audio_window.observe" || span.Depth != 0)
							throw std::runtime_error("audio observation whole owner");
						++owners;
					} else if (span.Parent >= i || span.Depth != FrameGraph::Spans()[span.Parent].Depth + 1)
						throw std::runtime_error("audio observation hierarchy");
					if (span.Name == "imagegraph.audio_window.geometry") ++geometry;
				}
				const auto counter = [&](std::string_view name) {
					for (const auto &c : record.Counters)
						if (c.Name.Text() == name) return c.Value;
					throw std::runtime_error("audio observation counter missing");
				};
				if (owners != 5 || geometry != 2 || counter("imagegraph.audio_window.allocations") != 2 ||
					counter("imagegraph.audio_window.geometry_cache_hits") != 3 ||
					counter("imagegraph.audio_window.allocated_payload_bytes") !=
						2 * Graph.AudioWindows[0].Points.capacity() * sizeof(engine::imagegraph::Vector2) ||
					counter("imagegraph.audio_window.snapshot_retained_payload_bytes") <= 0)
					throw std::runtime_error("audio observation operations mismatch");
			}
			if (Graph.Kind == Fixture::Family::WavTimeline) {
				const auto &spans = FrameGraph::Spans();
				size_t owners = 0;
				for (size_t index = 0; index < spans.size(); ++index) {
					const auto &span = spans[index];
					if (span.Reported) throw std::runtime_error("waveform profile unexpected reported span");
					if (span.Parent == FrameGraph::NO_PARENT) {
						if (span.Name != "imagegraph.wav_timeline.observe" || span.Depth != 0)
							throw std::runtime_error("waveform profile missing complete observation owner");
						++owners;
					} else if (span.Parent >= index || span.Depth != spans[span.Parent].Depth + 1)
						throw std::runtime_error("waveform profile malformed hierarchy");
				}
				if (owners != 3) throw std::runtime_error("waveform profile missed signed observations");
				const auto counterValue = [&](std::string_view name) {
					const auto found = std::find_if(
						record.Counters.begin(), record.Counters.end(), [&](const auto &counter) {
							return counter.Name.Text() == name;
						}
					);
					if (found == record.Counters.end())
						throw std::runtime_error("waveform profile missed actual point counter");
					return found->Value;
				};
				uint64_t bytes = 0;
				for (const auto &waveform : Graph.Waveforms)
					bytes += waveform.Points.capacity() * sizeof(engine::imagegraph::Vector2);
				if (counterValue("imagegraph.wav_timeline.allocated_payload_bytes") != bytes ||
					counterValue("imagegraph.wav_timeline.allocations") != 3)
					throw std::runtime_error("waveform actual point byte/operation counters mismatch");
			}
			if (Graph.IsHdr()) {
				const auto &spans = FrameGraph::Spans();
				if (spans.empty() || spans.front().Name != "imagegraph.evaluate" ||
					spans.front().Parent != FrameGraph::NO_PARENT || spans.front().Depth != 0)
					throw std::runtime_error("HDR blur capture missing whole evaluation owner");
				for (size_t index = 0; index < spans.size(); ++index) {
					const auto &span = spans[index];
					if (span.Reported ||
						(index != 0 && (span.Parent >= index || span.Depth != spans[span.Parent].Depth + 1)))
						throw std::runtime_error("HDR blur capture malformed hierarchy");
				}
				const auto counterValue = [&](std::string_view name) {
					const auto found = std::find_if(
						record.Counters.begin(), record.Counters.end(), [&](const auto &counter) {
							return counter.Name.Text() == name;
						}
					);
					if (found == record.Counters.end())
						throw std::runtime_error("HDR blur profile missed payload counter");
					return found->Value;
				};
				const uint64_t inputBytes = Graph.ImageSources.front().Data.Pixels.size();
				const uint64_t outputBytes = Graph.HdrOutput.Pixels.size();
				// Two Gaussian input-format passes, one motion scratch and two selected-format outputs.
				if (counterValue("imagegraph.surface.scratch.allocated_payload_bytes") !=
						2 * inputBytes + outputBytes ||
					counterValue("imagegraph.surface.scratch.allocations") != 3 ||
					counterValue("imagegraph.image.allocated_payload_bytes") != 2 * outputBytes ||
					counterValue("imagegraph.image.allocations") != 2)
					throw std::runtime_error("HDR blur actual payload/operation counters mismatch");
			}
			if (Graph.IsSourceImage()) {
				const auto counterValue = [&](std::string_view name) {
					const auto found = std::find_if(
						record.Counters.begin(), record.Counters.end(), [&](const auto &counter) {
							return counter.Name.Text() == name;
						}
					);
					if (found == record.Counters.end())
						throw std::runtime_error(
							"source image profile missed an allocation boundary counter"
						);
					return found->Value;
				};
				if (counterValue("imagegraph.image.allocated_payload_bytes") <
						Graph.SourceOutput.Pixels.size() ||
					counterValue("imagegraph.image.allocations") < 1)
					throw std::runtime_error("source image profile missed real output payload allocations");
			}
			if (Graph.IsCube() || Graph.IsCylinder() || Graph.IsCone() || Graph.IsTorus() ||
				Graph.IsUVSphere() || Graph.IsIcosphere()) {
				const std::string label = Graph.IsCube()	   ? "Cube"
										  : Graph.IsCylinder() ? "Cylinder"
										  : Graph.IsCone()	   ? "Cone"
										  : Graph.IsTorus()	   ? "Torus"
										  : Graph.IsUVSphere() ? "UV Sphere"
															   : "Icosphere";
				size_t owners = 0;
				const auto &spans = FrameGraph::Spans();
				for (size_t index = 0; index < spans.size(); ++index) {
					const auto &span = spans[index];
					if (span.Reported)
						throw std::runtime_error(label + " profile unexpected reported worker span");
					if (span.Parent == FrameGraph::NO_PARENT) {
						if (span.Name != "imagegraph.evaluate" || span.Depth != 0)
							throw std::runtime_error(label + " profile missing evaluation owner");
						++owners;
					} else if (span.Parent >= index || span.Depth != spans[span.Parent].Depth + 1)
						throw std::runtime_error(label + " profile malformed hierarchy");
				}
				if (owners != 3)
					throw std::runtime_error(label + " profile did not cover all selected output traversals");
				for (const auto name :
					 {"imagegraph.node.executions",
					  "imagegraph.value.outputs",
					  "imagegraph.value.output_payload_bytes"}) {
					const auto found = std::find_if(
						record.Counters.begin(), record.Counters.end(), [&](const auto &counter) {
							return counter.Name.Text() == name;
						}
					);
					if (found == record.Counters.end() || !(found->Value > 0))
						throw std::runtime_error(label + " profile missing actual value boundary counter");
					if (Graph.IsIcosphere() &&
						((std::string_view(name) == "imagegraph.node.executions" && found->Value != 8) ||
						 (std::string_view(name) == "imagegraph.value.outputs" && found->Value != 11)))
						throw std::runtime_error("Icosphere profile traversal boundary counts mismatch");
				}
			}
			if (Graph.Verify() != Graph.OutputHash) Graph.Fail("measured hash");
			Count++;
			engine::testing::Consume(Graph.OutputHash);
		}
	};
}

BENCH("Captured stereo 1024-sample Window to FFT", 1) {
	static ProfileFixture fixture(Fixture::Family::Audio);
	fixture.Measure();
}
BENCH("Palette gradient to 1024 colour samples", 1) {
	static ProfileFixture fixture(Fixture::Family::Gradient);
	fixture.Measure();
}
BENCH("Eight 256x256 Solid images to invert processor batch", 1) {
	static ProfileFixture fixture(Fixture::Family::Solid);
	fixture.Measure();
}

BENCH("Compile and evaluate 32x32 matrix identity product", 1) {
	static ProfileFixture fixture(Fixture::Family::Matrix);
	fixture.Measure();
}
BENCH("Resolve animated WAV controls over 1048576 samples without Data", 1) {
	static ProfileFixture fixture(Fixture::Family::WavControls);
	fixture.Measure();
}

BENCH("4096 scalar Multiply to Compare, numeric and boolean output traversals", 1) {
	static ProfileFixture fixture(Fixture::Family::ScalarMath);
	fixture.Measure();
}
BENCH("65-anchor triangle CurveFn to 256 samples, control and sample output traversals", 1) {
	static ProfileFixture fixture(Fixture::Family::Curve);
	fixture.Measure();
}

BENCH("1024 persisted Vector2 creators to Multiply and Length, three output traversals", 1) {
	static ProfileFixture fixture(Fixture::Family::Vector);
	fixture.Measure();
}

BENCH("128x128 captured RGBA16F Gaussian size1 to RGBA32F Directional 17 taps", 1) {
	static ProfileFixture fixture(Fixture::Family::HdrDirectional);
	fixture.Measure();
}
BENCH("128x128 captured RGBA32F Gaussian size1 to RGBA16F Zoom 17 taps", 1) {
	static ProfileFixture fixture(Fixture::Family::HdrZoom);
	fixture.Measure();
}

BENCH("Persisted Material to Cube1x1x1 Transform GetData, mesh/position/material traversals", 1) {
	static ProfileFixture fixture(Fixture::Family::CubeSmall);
	fixture.Measure();
}
BENCH("Persisted Material to Cube10x10x10 Transform GetData, mesh/position/material traversals", 1) {
	static ProfileFixture fixture(Fixture::Family::CubeLarge);
	fixture.Measure();
}

BENCH("Observe linked persisted stereo WAV timeline at negative and positive fractional frames", 1) {
	static ProfileFixture fixture(Fixture::Family::WavTimeline);
	fixture.Measure();
}

BENCH("Persisted Material to Cylinder8x1 Transform GetData, mesh/position/material traversals", 1) {
	static ProfileFixture fixture(Fixture::Family::CylinderSmall);
	fixture.Measure();
}
BENCH(
	"Persisted Material to Cylinder16x12 smooth profile Transform GetData, mesh/position/material traversals",
	1
) {
	static ProfileFixture fixture(Fixture::Family::CylinderProfile);
	fixture.Measure();
}

BENCH("Persisted Material to Cone8 default Transform GetData, mesh/position/material traversals", 1) {
	static ProfileFixture fixture(Fixture::Family::ConeDefault8);
	fixture.Measure();
}
BENCH("Persisted Material to Cone104 smooth Transform GetData, mesh/position/material traversals", 1) {
	static ProfileFixture fixture(Fixture::Family::ConeBounded104);
	fixture.Measure();
}

BENCH("Persisted Material to Torus16x8 default Transform GetData, mesh/position/material traversals", 1) {
	static ProfileFixture fixture(Fixture::Family::TorusDefault16x8);
	fixture.Measure();
}
BENCH(
	"Persisted Material to Torus31x22 smooth twisted Transform GetData, mesh/position/material traversals", 1
) {
	static ProfileFixture fixture(Fixture::Family::TorusSmooth31x22);
	fixture.Measure();
}

BENCH("Persisted Material to UVSphere8x16 default Transform GetData, mesh/position/material traversals", 1) {
	static ProfileFixture fixture(Fixture::Family::UVSphereDefault8x16);
	fixture.Measure();
}
BENCH(
	"Persisted Material to UVSphere31x22 smooth equirectangular Transform GetData, mesh/position/material "
	"traversals",
	1
) {
	static ProfileFixture fixture(Fixture::Family::UVSphereSmooth31x22);
	fixture.Measure();
}

BENCH("Audio Window observer 4096-packet stereo, signed clocks and source replacement", 1) {
	static ProfileFixture fixture(Fixture::Family::AudioWindow4096);
	fixture.Measure();
}
BENCH("Audio Window observer 65536-packet stereo, signed clocks and source replacement", 1) {
	static ProfileFixture fixture(Fixture::Family::AudioWindow65536);
	fixture.Measure();
}

BENCH("Persisted Material Icosphere default level1 flat Transform GetData, three traversals", 1) {
	static ProfileFixture fixture(Fixture::Family::IcosphereDefault1);
	fixture.Measure();
}
BENCH("Persisted Material Icosphere bounded level3 smooth Transform GetData, three traversals", 1) {
	static ProfileFixture fixture(Fixture::Family::IcosphereSmooth3);
	fixture.Measure();
}

BENCH("128x128 authored Julia one-iteration field", 1) {
	static ProfileFixture fixture(Fixture::Family::Julia128);
	fixture.Measure();
}
BENCH("128x128 authored Gabor fixed 25-cell kernel field, density-zero baseline", 1) {
	static ProfileFixture fixture(Fixture::Family::Gabor128);
	fixture.Measure();
}
BENCH("128x128 authored Julia eight-iteration field, matching pinned fixture", 1) {
	static ProfileFixture fixture(Fixture::Family::Julia128EightIterations);
	fixture.Measure();
}
BENCH("128x128 authored Gabor seeded default-density 25-cell field", 1) {
	static ProfileFixture fixture(Fixture::Family::Gabor128Seeded);
	fixture.Measure();
}
BENCH("128x128 authored Herringbone height field", 1) {
	static ProfileFixture fixture(Fixture::Family::Herringbone128);
	fixture.Measure();
}
BENCH("128x128 authored Honeycomb four-octave field", 1) {
	static ProfileFixture fixture(Fixture::Family::Honeycomb128);
	fixture.Measure();
}
BENCH("80x80 authored Heightmap three-output projection, normal selected, voxel-work bounded", 1) {
	static ProfileFixture fixture(Fixture::Family::Heightmap80);
	fixture.Measure();
}

BENCH("128x128 authored Flow detail 1..8, eight iterations and nine-step admission bound", 1) {
	static ProfileFixture fixture(Fixture::Family::Flow128DefaultDetail);
	fixture.Measure();
}
BENCH("64x64 authored Bubble seed 17 default density 0.5, 32 samples per pixel", 1) {
	static ProfileFixture fixture(Fixture::Family::Bubble64SeededDefaultDensity);
	fixture.Measure();
}

BENCH("Persisted Bitmap Font to eight Text rows, 5888 literal glyphs", 1) {
	static ProfileFixture fixture(Fixture::Family::BitmapTextEightRows);
	fixture.Measure();
}

BENCH("Kuwahara16FourRows pinned source CPU filter", 1) {
	static ProfileFixture fixture(Fixture::Family::Kuwahara16FourRows);
	fixture.Measure();
}

BENCH("KuwaharaAnisotropic16FourRows pinned source CPU filter", 1) {
	static ProfileFixture fixture(Fixture::Family::KuwaharaAnisotropic16FourRows);
	fixture.Measure();
}

BENCH("KuwaharaGeneralized16FourRows pinned source CPU filter", 1) {
	static ProfileFixture fixture(Fixture::Family::KuwaharaGeneralized16FourRows);
	fixture.Measure();
}

BENCH("BlobCircle16FourRows pinned source CPU filter", 1) {
	static ProfileFixture fixture(Fixture::Family::BlobCircle16FourRows);
	fixture.Measure();
}

BENCH("BlobDiamond16FourRows pinned source CPU filter", 1) {
	static ProfileFixture fixture(Fixture::Family::BlobDiamond16FourRows);
	fixture.Measure();
}

BENCH("BlobSquare16FourRows pinned source CPU filter", 1) {
	static ProfileFixture fixture(Fixture::Family::BlobSquare16FourRows);
	fixture.Measure();
}

BENCH("BlobCircleDistance16FourRows pinned source CPU filter", 1) {
	static ProfileFixture fixture(Fixture::Family::BlobCircleDistance16FourRows);
	fixture.Measure();
}

BENCH("BlobDiamondDistance16FourRows pinned source CPU filter", 1) {
	static ProfileFixture fixture(Fixture::Family::BlobDiamondDistance16FourRows);
	fixture.Measure();
}

BENCH("BlobSquareDistance16FourRows pinned source CPU filter", 1) {
	static ProfileFixture fixture(Fixture::Family::BlobSquareDistance16FourRows);
	fixture.Measure();
}
