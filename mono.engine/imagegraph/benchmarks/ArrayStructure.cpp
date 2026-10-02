// Diagnostic executor profile. Model and canonical verification stay outside captured frames.
#include "../src/NodeExecutors.hpp"

#include <engine/core/FrameGraph.hpp>
#include <engine/core/HeapProfile.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/testing/Bench.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

TEST_SUITE_ID("engine.imagegraph.bench.array-structure")
namespace {
	using namespace engine::imagegraph;
	using engine::core::FrameGraph;
	using engine::core::HeapProfile;
	using engine::core::Metrics;
	// Independent canonical tree. Every represented leaf has a complete type-tagged byte record.
	struct Tree {
		std::string Leaf;
		std::vector<Tree> Children;
		bool Array = false;
	};
	void Word(std::string &text, uint64_t word) {
		for (size_t byte = 0; byte < 8; ++byte)
			text.push_back(char((word >> (byte * 8)) & 255));
	}
	Tree Number(double value) {
		Tree result{"d"};
		Word(result.Leaf, std::bit_cast<uint64_t>(value));
		return result;
	}
	Tree Text(std::string_view value) {
		Tree result{"s"};
		Word(result.Leaf, value.size());
		result.Leaf.append(value);
		return result;
	}
	Tree ImageLeaf(const Image &image) {
		Tree result{"image"};
		Word(result.Leaf, image.Width);
		Word(result.Leaf, image.Height);
		Word(result.Leaf, uint64_t(image.Format));
		Word(result.Leaf, image.Hash);
		Word(result.Leaf, image.Pixels.size());
		for (auto byte : image.Pixels)
			result.Leaf.push_back(char(byte));
		return result;
	}
	Tree Row(std::vector<Tree> children) {
		return {{}, std::move(children), true};
	}
	Tree Element(const ElementValue &value) {
		return std::visit(
			[](const auto &leaf) -> Tree {
				using T = std::decay_t<decltype(leaf)>;
				if constexpr (std::is_same_v<T, double>)
					return Number(leaf);
				else if constexpr (std::is_same_v<T, std::string>)
					return Text(leaf);
				else if constexpr (std::is_same_v<T, int64_t>) {
					Tree result{"i"};
					Word(result.Leaf, uint64_t(leaf));
					return result;
				} else if constexpr (std::is_same_v<T, bool>)
					return {leaf ? "b1" : "b0"};
				else if constexpr (std::is_same_v<T, Colour>) {
					Tree result{"c"};
					result.Leaf += char(leaf.Red);
					result.Leaf += char(leaf.Green);
					result.Leaf += char(leaf.Blue);
					result.Leaf += char(leaf.Alpha);
					return result;
				} else if constexpr (std::is_same_v<T, Vector2>)
					return Row({Number(leaf.X), Number(leaf.Y)});
				else
					throw std::runtime_error(
						"array profile canonical leaf not represented by fixture contract"
					);
			},
			value
		);
	}
	Tree Item(const SourceArrayItem &item) {
		return std::visit(
			[](const auto &data) -> Tree {
				using T = std::decay_t<decltype(data)>;
				if constexpr (std::is_same_v<T, ElementValue>)
					return Element(data);
				else if constexpr (std::is_same_v<T, Image>)
					return ImageLeaf(data);
				else {
					std::vector<Tree> children;
					for (const auto &child : data)
						children.push_back(Item(child));
					return Row(std::move(children));
				}
			},
			item.Data
		);
	}
	Tree Array(const ArrayValue &array) {
		std::vector<Tree> children;
		if (!array.Items.empty())
			for (const auto &item : array.Items)
				children.push_back(Item(item));
		else if (!array.Nested.empty())
			for (const auto &row : array.Nested) {
				std::vector<Tree> members;
				for (const auto &leaf : row)
					members.push_back(Element(leaf));
				children.push_back(Row(std::move(members)));
			}
		else
			for (const auto &leaf : array.Elements)
				children.push_back(Element(leaf));
		return Row(std::move(children));
	}
	Tree ImageItem(const ImageArray &array, const ImageArrayItem &item) {
		if (const auto *index = std::get_if<size_t>(&item.Data)) {
			if (*index >= array.Images.size()) throw std::runtime_error("invalid image reference");
			return ImageLeaf(array.Images[*index]);
		}
		std::vector<Tree> children;
		for (const auto &child : std::get<std::vector<ImageArrayItem>>(item.Data))
			children.push_back(ImageItem(array, child));
		return Row(std::move(children));
	}
	void Encode(const Tree &tree, std::string &record) {
		record += tree.Array ? 'a' : 'l';
		Word(record, tree.Array ? tree.Children.size() : tree.Leaf.size());
		if (tree.Array)
			for (const auto &child : tree.Children)
				Encode(child, record);
		else
			record += tree.Leaf;
	}
	std::string Canonical(const Tree &tree) {
		std::string result;
		Encode(tree, result);
		return result;
	}
	uint64_t Hash(std::string_view bytes) {
		uint64_t hash = 14695981039346656037ULL;
		for (unsigned char byte : bytes)
			hash = (hash ^ byte) * 1099511628211ULL;
		return hash;
	}
	void FlattenLeaves(const Tree &tree, std::vector<Tree> &leaves) {
		if (!tree.Array)
			leaves.push_back(tree);
		else
			for (const auto &child : tree.Children)
				FlattenLeaves(child, leaves);
	}
	struct Fixture {
		std::string Type, InputPort, OutputPort;
		ArrayValue Input;
		int64_t Mode = 0;
		std::string Before, Expected;
	};
	std::vector<Fixture> Fixtures() {
		constexpr std::array types{
			"pc.array_reverse",
			"pc.array_copy",
			"pc.array_trim",
			"pc.array_shift",
			"pc.array_partition",
			"pc.array_flattern",
			"pc.array_transpose",
			"pc.array_length"
		};
		std::vector<Fixture> fixtures;
		for (int extent : {16, 256})
			for (int family = 0; family < 3; ++family) {
				ArrayValue input{family == 1 ? ValueType::Vector2 : ValueType::Any, {}};
				for (int index = 0; index < extent; ++index) {
					if (family == 1)
						input.Elements.push_back(Vector2{double(index), double(-index) - 0.5});
					else {
						std::vector<SourceArrayItem> members;
						if (family == 2)
							members.push_back(
								{Image{2, 1, {uint8_t(index), 2, 3, 255, 4, 5, 6, 255}, uint64_t(index)}}
							);
						else
							members.push_back({ElementValue{double(index) + 0.25}});
						members.push_back({ElementValue{std::string("row-") + std::to_string(index)}});
						members.push_back({ElementValue{bool(index % 2)}});
						members.push_back({ElementValue{int64_t(index)}});
						input.Items.push_back({std::move(members)});
					}
				}
				const Tree original = Array(input);
				for (size_t operation = 0; operation < types.size(); ++operation) {
					Fixture fixture{
						types[operation],
						operation == 5 || operation == 6 ? "array_in" : "array",
						operation == 5	 ? "flattened_array"
						: operation == 6 ? "transposed_array"
						: operation == 7 ? "size"
										 : "array",
						input,
						family
					};
					fixture.Before = Canonical(original);
					Tree expected = Row({});
					const auto &rows = original.Children;
					switch (operation) {
					case 0:
						expected.Children.assign(rows.rbegin(), rows.rend());
						break;
					case 1:
						expected.Children = {
							rows[rows.size() - 2], rows[rows.size() - 1], Number(0), Number(0), Number(0)
						};
						break;
					case 2:
						expected.Children.assign(rows.begin() + 1, rows.end() - 1);
						break;
					case 3:
						if (family == 0) {
							expected.Children.push_back(rows.back());
							expected.Children.insert(expected.Children.end(), rows.begin(), rows.end() - 1);
						} else {
							if (family == 1) expected.Children.push_back(Number(0));
							expected.Children.insert(expected.Children.end(), rows.begin(), rows.end() - 1);
						}
						break;
					case 4: {
						const size_t rowCount = family % 2 ? 8 : rows.size() / 4;
						const size_t width = rows.size() / rowCount;
						for (size_t row = 0; row < rowCount; ++row)
							expected.Children.push_back(
								Row(std::vector<Tree>(
									rows.begin() + row * width, rows.begin() + (row + 1) * width
								))
							);
						break;
					}
					case 5:
						FlattenLeaves(original, expected.Children);
						break;
					case 6:
						for (size_t column = 0; column < rows.front().Children.size(); ++column) {
							std::vector<Tree> members;
							for (const auto &row : rows)
								members.push_back(row.Children[column]);
							expected.Children.push_back(Row(std::move(members)));
						}
						break;
					case 7:
						expected = {"i"};
						Word(expected.Leaf, rows.size());
						break;
					}
					fixture.Expected = Canonical(expected);
					fixtures.push_back(std::move(fixture));
				}
			}
		return fixtures;
	}
	struct Span {
		std::string Name;
		engine::core::FrameSpan Data;
	};
	struct Reading {
		size_t FixtureIndex = 0, Call = 0;
		uint64_t InputHash = 0, OutputHash = 0;
		float Frame = 0, Unmarked = 0;
		engine::core::HeapTotals Before, After;
		std::vector<Span> Spans;
		std::vector<engine::core::Counter> Counters;
	};
	struct Profile {
		std::vector<Fixture> Cases = Fixtures();
		std::vector<Reading> Readings;
		size_t Calls = 0;
		Profile() {
			Readings.reserve(Cases.size() * 13);
			for (const auto &fixture : Cases)
				if (!detail::FindExecutor(fixture.Type))
					throw std::runtime_error("array profile needs reviewed executor candidate");
		}
		~Profile() {
			for (const auto &reading : Readings) {
				std::printf(
					"# array-profile fixture=%zu call=%zu phase=%s input_fnv=%llu output_fnv=%llu "
					"frame_ms=%.6f unmarked_ms=%.6f heap_compiled=%d allocated_bytes=%llu "
					"allocated_blocks=%llu live_bytes=%lld live_blocks=%lld peak_bytes=%lld "
					"overhead_bytes=%lld\n",
					reading.FixtureIndex,
					reading.Call,
					reading.Call <= 8 ? "warm" : "measured",
					(unsigned long long)reading.InputHash,
					(unsigned long long)reading.OutputHash,
					reading.Frame,
					reading.Unmarked,
					HeapProfile::IsCompiledIn(),
					(unsigned long long)(reading.After.TotalBytes - reading.Before.TotalBytes),
					(unsigned long long)(reading.After.TotalBlocks - reading.Before.TotalBlocks),
					(long long)reading.After.LiveBytes,
					(long long)reading.After.LiveBlocks,
					(long long)reading.After.PeakBytes,
					(long long)reading.After.OverheadBytes
				);
				for (size_t index = 0; index < reading.Spans.size(); ++index) {
					const auto &span = reading.Spans[index];
					std::printf(
						"# array-span fixture=%zu call=%zu index=%zu parent=%u depth=%u start_ms=%.6f "
						"inclusive_ms=%.6f self_ms=%.6f idle_ms=%.6f reported=%d name=%s\n",
						reading.FixtureIndex,
						reading.Call,
						index,
						span.Data.Parent,
						span.Data.Depth,
						span.Data.StartMilliseconds,
						span.Data.Milliseconds,
						span.Data.SelfMilliseconds,
						span.Data.IdleMilliseconds,
						span.Data.Reported,
						span.Name.c_str()
					);
				}
				for (const auto &counter : reading.Counters)
					std::printf(
						"# array-counter fixture=%zu call=%zu name=%s value=%.0f samples=%u\n",
						reading.FixtureIndex,
						reading.Call,
						counter.Name.Text().data(),
						counter.Value,
						counter.Samples
					);
			}
		}
		void Measure() {
			if (Calls >= 13)
				throw std::runtime_error("array profile permits eight warmups and at most five samples");
			for (size_t index = 0; index < Cases.size(); ++index) {
				const auto &fixture = Cases[index];
				if (Canonical(Array(fixture.Input)) != fixture.Before)
					throw std::runtime_error("array source ownership changed before dispatch");
				const auto *entry = FindCatalogueEntry(fixture.Type);
				if (!entry) throw std::runtime_error("array catalogue missing");
				Node node{"profile-array", fixture.Type, "", {}, {}};
				EvaluationRequest request;
				detail::NodeContext context(node, *entry, request);
				context.ByteBudget = Limits::MaximumEvaluationBytes;
				for (const auto &input : entry->Inputs)
					if (input.Id == fixture.InputPort)
						context.Values.emplace_back(input.Id, fixture.Input);
					else if (auto fallback = CatalogueDefault(input))
						context.Values.emplace_back(input.Id, std::move(*fallback));
				const auto set = [&](std::string_view port, Value value) {
					for (auto &[name, data] : context.Values)
						if (name == port) {
							data = std::move(value);
							return;
						}
					throw std::runtime_error("array control absent");
				};
				if (fixture.Type == "pc.array_copy") {
					set("starting_index", int64_t(-2));
					set("size", int64_t(5));
				}
				if (fixture.Type == "pc.array_trim") {
					set("trim_start", int64_t(1));
					set("trim_end", int64_t(1));
				}
				if (fixture.Type == "pc.array_shift") {
					set("shift", int64_t(1));
					set("overflow", EnumValue{fixture.Mode});
				}
				if (fixture.Type == "pc.array_partition") {
					set("type", EnumValue{fixture.Mode % 2});
					set("length", int64_t(fixture.Mode % 2 ? 8 : 4));
				}
				if (fixture.Type == "pc.array_flattern") set("depth", int64_t(0));
				const uint64_t inputPayloadBytes = sizeof(Value) + detail::PayloadOwnedBytes(fixture.Input);
				Reading reading;
				reading.FixtureIndex = index;
				reading.Call = Calls + 1;
				reading.InputHash = Hash(fixture.Before);
				Metrics::Drain();
				struct Restore {
					bool Enabled = FrameGraph::IsEnabled();
					~Restore() {
						FrameGraph::SetEnabled(Enabled);
					}
				} restore;
				FrameGraph::SetEnabled(true);
				reading.Before = HeapProfile::Totals();
				FrameGraph::BeginFrame();
				bool ok = false;
				try {
					ENGINE_PROFILE("imagegraph.array_structure.dispatch");
					Metrics::Count("imagegraph.array_structure.dispatches", 1);
					Metrics::Count("imagegraph.array_structure.input_value_payload_bytes", inputPayloadBytes);
					ok = detail::FindExecutor(fixture.Type)(context);
				} catch (...) {
					FrameGraph::EndFrame();
					FrameGraph::SetEnabled(restore.Enabled);
					throw;
				}
				FrameGraph::EndFrame();
				reading.After = HeapProfile::Totals();
				reading.Frame = FrameGraph::FrameMilliseconds();
				reading.Unmarked = FrameGraph::UnmarkedMilliseconds();
				if (FrameGraph::Dropped() || reading.After.DroppedScopes != reading.Before.DroppedScopes)
					throw std::runtime_error("array profile lost spans");
				const auto spans = FrameGraph::Spans();
				size_t owners = 0;
				for (size_t spanIndex = 0; spanIndex < spans.size(); ++spanIndex) {
					const auto &span = spans[spanIndex];
					if (span.Reported) throw std::runtime_error("unexpected reported array work");
					if (span.Parent == FrameGraph::NO_PARENT) {
						if (span.Name != "imagegraph.array_structure.dispatch" || span.Depth != 0)
							throw std::runtime_error("array owner absent");
						++owners;
					} else if (span.Parent >= spanIndex || span.Depth != spans[span.Parent].Depth + 1)
						throw std::runtime_error("array hierarchy malformed");
					reading.Spans.push_back({std::string(span.Name), span});
				}
				FrameGraph::SetEnabled(restore.Enabled);
				if (owners != 1 || !ok || context.FailureCode != Status::Ok)
					throw std::runtime_error("array dispatch failed");
				reading.Counters = Metrics::Drain();
				Tree actual;
				bool found = false;
				for (const auto &value : context.OutputValues)
					if (value.Port == fixture.OutputPort) {
						if (!detail::ValidRuntimeValue(value.Data))
							throw std::runtime_error("array output carrier invalid");
						if (const auto *array = std::get_if<ArrayValue>(&value.Data))
							actual = Array(*array);
						else if (const auto *size = std::get_if<int64_t>(&value.Data)) {
							actual = {"i"};
							Word(actual.Leaf, uint64_t(*size));
						} else
							throw std::runtime_error("unexpected array profile output type");
						found = true;
					}
				for (const auto &[port, array] : context.OutputImageArrays)
					if (port == fixture.OutputPort) {
						std::vector<Tree> members;
						for (const auto &item : array.Items)
							members.push_back(ImageItem(array, item));
						actual = Row(std::move(members));
						found = true;
					}
				const std::string canonical = Canonical(actual);
				if (!found || canonical != fixture.Expected)
					throw std::runtime_error("array profile complete output mismatch");
				if (Canonical(Array(fixture.Input)) != fixture.Before)
					throw std::runtime_error("array source ownership changed after dispatch");
				for (const auto &[port, value] : context.Values)
					if (port == fixture.InputPort) {
						const auto *input = std::get_if<ArrayValue>(&value);
						if (!input || Canonical(Array(*input)) != fixture.Before)
							throw std::runtime_error("array executor mutated owned context input");
					}
				const auto counter = [&](std::string_view name) {
					for (const auto &value : reading.Counters)
						if (value.Name.Text() == name) return value.Value;
					throw std::runtime_error("array profile missing boundary counter");
				};
				uint64_t publishedBytes = 0;
				for (const auto &value : context.OutputValues)
					publishedBytes += detail::ValuePayloadBytes(value.Data);
				if (counter("imagegraph.array_structure.dispatches") != 1 ||
					counter("imagegraph.array_structure.input_value_payload_bytes") != inputPayloadBytes ||
					counter("imagegraph.value.outputs") != 1 ||
					counter("imagegraph.value.output_payload_bytes") != publishedBytes)
					throw std::runtime_error("array actual publication counters disagree");
				reading.OutputHash = Hash(canonical);
				Readings.push_back(std::move(reading));
			}
			++Calls;
			engine::testing::Consume(Calls);
		}
	};
}
BENCH("Diagnostic eight array executors, mixed rows packed tuples and owned images", 1) {
	static Profile profile;
	profile.Measure();
}
