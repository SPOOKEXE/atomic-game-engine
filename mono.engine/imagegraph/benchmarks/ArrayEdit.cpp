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
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

TEST_SUITE_ID("engine.imagegraph.bench.array-edit")
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
				else if constexpr (std::is_same_v<T, EnumValue>) {
					Tree result{"e"};
					Word(result.Leaf, uint64_t(leaf.Value));
					return result;
				} else if constexpr (std::is_same_v<T, Colour>) {
					Tree result{"c"};
					result.Leaf += char(leaf.Red);
					result.Leaf += char(leaf.Green);
					result.Leaf += char(leaf.Blue);
					result.Leaf += char(leaf.Alpha);
					return result;
				} else if constexpr (std::is_same_v<T, Vector2>)
					return Row({Number(leaf.X), Number(leaf.Y)});
				else if constexpr (std::is_same_v<T, Vector3>)
					return Row({Number(leaf.X), Number(leaf.Y), Number(leaf.Z)});
				else if constexpr (std::is_same_v<T, Vector4> || std::is_same_v<T, Quaternion>)
					return Row({Number(leaf.X), Number(leaf.Y), Number(leaf.Z), Number(leaf.W)});
				else if constexpr (std::is_same_v<T, Area>)
					return Row(
						{Number(leaf.CenterX),
						 Number(leaf.CenterY),
						 Number(leaf.HalfWidth),
						 Number(leaf.HalfHeight),
						 Number(leaf.Shape),
						 Number(leaf.Mode)}
					);
				else if constexpr (std::is_same_v<T, Curve>) {
					std::vector<Tree> components;
					for (double component : leaf.Header)
						components.push_back(Number(component));
					for (const auto &anchor : leaf.Anchors)
						for (double component : anchor)
							components.push_back(Number(component));
					return Row(std::move(components));
				} else
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
	Tree Integer(int64_t value) {
		Tree result{"i"};
		Word(result.Leaf, uint64_t(value));
		return result;
	}
	void NativeElement(const ElementValue &value, std::string &record) {
		Word(record, value.index());
		std::visit(
			[&](const auto &leaf) {
				using T = std::decay_t<decltype(leaf)>;
				if constexpr (std::is_same_v<T, std::string>) {
					Word(record, leaf.capacity());
					Encode(Text(leaf), record);
				} else if constexpr (std::is_same_v<T, Curve>) {
					Word(record, leaf.Anchors.capacity());
					Encode(Element(ElementValue{leaf}), record);
				} else if constexpr (std::is_same_v<T, Vector2>) {
					Word(record, std::bit_cast<uint64_t>(leaf.X));
					Word(record, std::bit_cast<uint64_t>(leaf.Y));
				} else {
					// The native variant tag distinguishes scalar alternatives, including enum and integer.
					const Tree tree = Element(ElementValue{leaf});
					Encode(tree, record);
				}
			},
			value
		);
	}
	void NativeItem(const SourceArrayItem &item, std::string &record) {
		Word(record, item.Data.index());
		std::visit(
			[&](const auto &data) {
				using T = std::decay_t<decltype(data)>;
				if constexpr (std::is_same_v<T, ElementValue>)
					NativeElement(data, record);
				else if constexpr (std::is_same_v<T, Image>) {
					Word(record, data.Pixels.capacity());
					const Tree tree = ImageLeaf(data);
					Word(record, tree.Leaf.size());
					record += tree.Leaf;
				} else {
					Word(record, data.size());
					Word(record, data.capacity());
					for (const auto &child : data)
						NativeItem(child, record);
				}
			},
			item.Data
		);
	}
	void NativeArrayFields(const ArrayValue &data, std::string &record) {
		Word(record, uint64_t(data.ElementType));
		Word(record, data.Elements.size());
		Word(record, data.Elements.capacity());
		for (const auto &leaf : data.Elements)
			NativeElement(leaf, record);
		Word(record, data.Nested.size());
		Word(record, data.Nested.capacity());
		for (const auto &row : data.Nested) {
			Word(record, row.size());
			Word(record, row.capacity());
			for (const auto &leaf : row)
				NativeElement(leaf, record);
		}
		Word(record, data.Items.size());
		Word(record, data.Items.capacity());
		for (const auto &item : data.Items)
			NativeItem(item, record);
	}
	void NativeValue(const Value &value, std::string &record) {
		Word(record, value.index());
		std::visit(
			[&](const auto &data) {
				using T = std::decay_t<decltype(data)>;
				if constexpr (std::is_same_v<T, ArrayValue>) {
					NativeArrayFields(data, record);
				} else
					NativeElement(ElementValue{data}, record);
			},
			value
		);
	}
	std::string NativeInputs(const std::vector<std::pair<std::string_view, Value>> &values) {
		std::string record;
		Word(record, values.size());
		for (const auto &[name, value] : values) {
			Word(record, name.size());
			record.append(name);
			NativeValue(value, record);
		}
		return record;
	}
	std::string NativeOwned(const std::vector<std::pair<std::string, Value>> &values) {
		std::string record;
		Word(record, values.size());
		for (const auto &[name, value] : values) {
			Word(record, name.size());
			record += name;
			NativeValue(value, record);
		}
		return record;
	}
	std::string NativeArray(const ArrayValue &array) {
		std::string record;
		Word(record, Value{ArrayValue{}}.index());
		NativeArrayFields(array, record);
		return record;
	}
	bool SameImageArray(const std::optional<ImageArray> &left, const std::optional<ImageArray> &right) {
		if (left.has_value() != right.has_value()) return false;
		return !left || (left->Images == right->Images && left->Items == right->Items);
	}
	bool MutateItem(SourceArrayItem &item) {
		if (auto *image = std::get_if<Image>(&item.Data)) {
			if (image->Pixels.empty()) return false;
			image->Pixels.front() ^= 255;
			return true;
		}
		if (auto *children = std::get_if<std::vector<SourceArrayItem>>(&item.Data)) {
			for (auto &child : *children)
				if (MutateItem(child)) return true;
			children->push_back({ElementValue{double{999}}});
			return true;
		}
		auto &leaf = std::get<ElementValue>(item.Data);
		if (auto *text = std::get_if<std::string>(&leaf))
			text->append("-mutated");
		else if (auto *curve = std::get_if<Curve>(&leaf); curve && !curve->Anchors.empty())
			curve->Anchors.front()[0] += 999;
		else
			leaf = double{999};
		return true;
	}
	struct Fixture {
		std::string Type, InputPort = "array", OutputPort;
		ArrayValue Input;
		int64_t Mode = 0;
		bool ScalarGet = false;
		Status ExpectedStatus = Status::Ok;
		std::optional<ValueType> ExpectedArrayType;
		std::optional<ImageArray> ImageInput;
		bool ExpectedImageArray = false, OmitOrders = false;
		std::string ExpectedMessage;
		std::vector<std::pair<std::string, Value>> Controls, Dynamic;
		std::string Before, Expected, NativeBefore, NativeControlsBefore, NativeDynamicBefore;
	};
	std::vector<Fixture> Fixtures() {
		constexpr std::array types{
			"pc.array_add",
			"pc.array_get",
			"pc.array_set",
			"pc.array_insert",
			"pc.array_remove",
			"pc.array_find",
			"pc.array_zip",
			"pc.array_get"
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
						members.push_back({std::vector<SourceArrayItem>{{ElementValue{int64_t(index)}}}});
						input.Items.push_back({std::move(members)});
					}
				}
				const Tree original = Array(input);
				const auto &rows = original.Children;
				ArrayValue replacement{ValueType::Any, {}};
				replacement.Items = {
					{ElementValue{std::string("replacement")}},
					{std::vector<SourceArrayItem>{{ElementValue{double{999}}}}}
				};
				const Tree replacementTree = Array(replacement);
				for (size_t operation = 0; operation < types.size(); ++operation) {
					Fixture fixture;
					fixture.Type = types[operation];
					fixture.Input = input;
					fixture.Mode = family;
					fixture.ScalarGet = operation == 7;
					fixture.OutputPort = operation == 0 || operation == 6	? "output"
										 : operation == 1 || operation == 7 ? "value"
										 : operation == 5					? "index"
																			: "array";
					fixture.Before = Canonical(original);
					Tree expected = original;
					switch (operation) {
					case 0:
						fixture.Controls = {{"spread_array", family % 2 == 0}};
						fixture.Dynamic = {{"value_0", replacement}, {"value_1", std::string("tail")}};
						if (family % 2 == 0)
							expected.Children.insert(
								expected.Children.end(),
								replacementTree.Children.begin(),
								replacementTree.Children.end()
							);
						else
							expected.Children.push_back(replacementTree);
						expected.Children.push_back(Text("tail"));
						break;
					case 1:
						fixture.Controls = {
							{"index",
							 ArrayValue{ValueType::Integer, {int64_t{0}, int64_t{-1}, int64_t(extent + 3)}}},
							{"mode", EnumValue{0}},
							{"overflow", EnumValue{family}}
						};
						expected = family == 0	 ? Row({rows.front(), rows.back(), rows.back()})
								   : family == 1 ? Row({rows.front(), rows.back(), rows[3]})
												 : Row({rows.front(), rows[1], rows[rows.size() - 5]});
						break;
					case 2:
						fixture.Controls = {
							{"index",
							 ArrayValue{ValueType::Integer, {int64_t{0}, int64_t{-1}, int64_t(extent + 2)}}},
							{"value", replacement}
						};
						expected.Children.front() = replacementTree.Children.front();
						expected.Children.back() = replacementTree.Children.back();
						expected.Children.push_back(Number(0));
						expected.Children.push_back(Number(0));
						expected.Children.push_back(replacementTree.Children.front());
						break;
					case 3:
						fixture.Controls = {
							{"index", int64_t{-1}}, {"value", replacement}, {"spread_array", family % 2 == 0}
						};
						if (family % 2 == 0)
							expected.Children.insert(
								expected.Children.end() - 1,
								replacementTree.Children.begin(),
								replacementTree.Children.end()
							);
						else
							expected.Children.insert(expected.Children.end() - 1, replacementTree);
						break;
					case 4:
						fixture.Controls = {
							{"type", EnumValue{0}},
							{"index", ArrayValue{ValueType::Integer, {int64_t{1}, int64_t{-1}}}}
						};
						expected.Children.erase(expected.Children.begin() + 1);
						expected.Children.pop_back();
						break;
					case 5:
						if (family == 1)
							fixture.Controls = {
								{"value", Vector2{double(extent - 1), double(-(extent - 1)) - 0.5}}
							};
						else {
							ArrayValue needle{ValueType::Any, {}};
							if (family == 2)
								needle.Items = {{ElementValue{double{-123}}}};
							else
								needle.Items =
									std::get<std::vector<SourceArrayItem>>(input.Items.back().Data);
							fixture.Controls = {{"value", needle}};
						}
						expected = Integer(family == 2 ? -1 : extent - 1);
						break;
					case 6: {
						ArrayValue shorter{ValueType::Scalar, {}};
						for (int index = 0; index < extent - 1; ++index)
							shorter.Elements.push_back(double(index) + 1000);
						fixture.Controls = {{"spread_content", family % 2 == 0}};
						fixture.Dynamic = {
							{"value_0", input}, {"value_1", std::string("scalar")}, {"value_2", shorter}
						};
						expected = Row({});
						for (int index = 0; index < extent - 1; ++index) {
							std::vector<Tree> members =
								family % 2 == 0 ? rows[index].Children : std::vector<Tree>{rows[index]};
							members.push_back(index == 0 ? Text("scalar") : Number(0));
							members.push_back(Number(double(index) + 1000));
							expected.Children.push_back(Row(std::move(members)));
						}
						break;
					}
					case 7:
						fixture.Controls = {
							{"index", int64_t{-1}}, {"mode", EnumValue{0}}, {"overflow", EnumValue{0}}
						};
						expected = rows.back();
						break;
					}
					fixture.Expected = Canonical(expected);
					fixture.NativeBefore = NativeArray(fixture.Input);
					fixture.NativeControlsBefore = NativeOwned(fixture.Controls);
					fixture.NativeDynamicBefore = NativeOwned(fixture.Dynamic);
					fixtures.push_back(std::move(fixture));
				}
			}
		// Unique policy cases use predetermined schedules, not the executor comparator as an oracle.
		for (int extent : {16, 256})
			for (int family = 0; family < 3; ++family)
				for (bool duplicates : {false, true}) {
					Fixture fixture;
					fixture.Type = "pc.array_unique";
					fixture.InputPort = "array_in";
					fixture.OutputPort = "unique_array";
					fixture.Input.ElementType = family == 0	  ? ValueType::Integer
												: family == 1 ? ValueType::Text
															  : ValueType::Any;
					std::vector<Tree> expected;
					for (int index = 0; index < extent; ++index) {
						const int member = duplicates ? index % 4 : index;
						ElementValue leaf =
							family == 0	  ? ElementValue{(int64_t{1} << 53) + member}
							: family == 1 ? ElementValue{std::string("unique-text-") + std::to_string(member)}
							: member % 3 == 0 ? ElementValue{int64_t(member + 100)}
							: member % 3 == 1 ? ElementValue{double(member) + 0.25}
											  : ElementValue{std::string("mixed-") + std::to_string(member)};
						if (family == 2)
							fixture.Input.Items.push_back({leaf});
						else
							fixture.Input.Elements.push_back(leaf);
						if (!duplicates || index < 4) expected.push_back(Element(leaf));
					}
					fixture.Before = Canonical(Array(fixture.Input));
					fixture.Expected = Canonical(Row(std::move(expected)));
					fixture.NativeBefore = NativeArray(fixture.Input);
					fixture.NativeControlsBefore = NativeOwned(fixture.Controls);
					fixture.NativeDynamicBefore = NativeOwned(fixture.Dynamic);
					fixtures.push_back(std::move(fixture));
				}
		for (int special = 0; special < 4; ++special) {
			Fixture fixture;
			fixture.Type = "pc.array_unique";
			fixture.InputPort = "array_in";
			fixture.OutputPort = "unique_array";
			fixture.Input.ElementType = ValueType::Any;
			SourceArrayItem member = special == 1 || special == 2
										 ? SourceArrayItem{Image{2, 1, {1, 2, 3, 255, 4, 5, 6, 255}, 97}}
										 : SourceArrayItem{std::vector<SourceArrayItem>{
											   {ElementValue{std::string("nested")}},
											   {std::vector<SourceArrayItem>{{ElementValue{int64_t{7}}}}}
										   }};
			fixture.Input.Items.push_back(member);
			if (special >= 2) {
				fixture.Input.Items.push_back(member);
				fixture.ExpectedStatus = Status::UnsupportedExecution;
				fixture.ExpectedMessage = special == 2
											  ? "source unique opaque identity comparison is unavailable"
											  : "source unique array reference comparison is unverified";
			}
			fixture.Before = Canonical(Array(fixture.Input));
			fixture.Expected =
				fixture.ExpectedStatus == Status::Ok ? fixture.Before : Canonical(Tree{"diagnostic"});
			fixture.NativeBefore = NativeArray(fixture.Input);
			fixture.NativeControlsBefore = NativeOwned(fixture.Controls);
			fixture.NativeDynamicBefore = NativeOwned(fixture.Dynamic);
			fixtures.push_back(std::move(fixture));
		}
		// Uniform repeats a whole owned source member. Expected trees are constructed
		// independently from the original source shape, never from executor output.
		for (int extent : {16, 256})
			for (int family = 0; family < 3; ++family) {
				Fixture fixture;
				fixture.Type = "pc.array_uniform";
				fixture.InputPort = "data";
				fixture.OutputPort = "array_out";
				fixture.Input.ElementType = family == 1 ? ValueType::Vector2 : ValueType::Any;
				for (int index = 0; index < extent; ++index) {
					if (family == 1)
						fixture.Input.Elements.push_back(Vector2{double(index) + 0.25, -double(index) - 0.5});
					else {
						std::vector<SourceArrayItem> row;
						row.push_back({ElementValue{std::string("uniform-") + std::to_string(index)}});
						row.push_back({ElementValue{(int64_t{1} << 53) + index}});
						row.push_back({ElementValue{bool(index % 2)}});
						if (family == 2)
							row.push_back(
								{Image{2, 1, {uint8_t(index), 2, 3, 255, 4, 5, 6, 255}, uint64_t(index)}}
							);
						else
							row.push_back({std::vector<SourceArrayItem>{}});
						fixture.Input.Items.push_back({std::move(row)});
					}
				}
				const Tree original = Array(fixture.Input);
				fixture.Controls = {{"length", int64_t{3}}};
				fixture.Before = Canonical(original);
				fixture.Expected = Canonical(Row({original, original, original}));
				fixture.NativeBefore = NativeArray(fixture.Input);
				fixture.NativeControlsBefore = NativeOwned(fixture.Controls);
				fixture.NativeDynamicBefore = NativeOwned(fixture.Dynamic);
				fixtures.push_back(std::move(fixture));
			}
		for (int special = 0; special < 4; ++special) {
			Fixture fixture;
			fixture.Type = "pc.array_uniform";
			fixture.InputPort = "data";
			fixture.OutputPort = "array_out";
			fixture.Input.ElementType = special == 0 ? ValueType::Text : ValueType::Any;
			if (special != 3) fixture.ExpectedArrayType = fixture.Input.ElementType;
			const int64_t count = special == 2 ? 0 : special == 3 ? 3 : 16;
			if (special == 2)
				fixture.Input.Items = {
					{ElementValue{std::string("unused")}},
					{std::vector<SourceArrayItem>{{ElementValue{int64_t{9}}}}}
				};
			else if (special == 3)
				fixture.Input.Items = {{Image{2, 1, {11, 12, 13, 255, 14, 15, 16, 255}, 103}}};
			const Tree original = Array(fixture.Input);
			fixture.Controls = {{"length", count}};
			fixture.Before = Canonical(original);
			fixture.Expected = Canonical(Row(std::vector<Tree>(size_t(count), original)));
			fixture.NativeBefore = NativeArray(fixture.Input);
			fixture.NativeControlsBefore = NativeOwned(fixture.Controls);
			fixture.NativeDynamicBefore = NativeOwned(fixture.Dynamic);
			fixtures.push_back(std::move(fixture));
		}
		// Reverse permutation and four-member repetition have fixed expected schedules.
		// Neither schedule consults the executor or its Orders projection.
		for (int extent : {16, 256})
			for (int family = 0; family < 5; ++family)
				for (bool duplicates : {false, true}) {
					Fixture fixture;
					fixture.Type = "pc.array_rearrange";
					fixture.OutputPort = "array";
					const int memberCount = family == 3 && extent == 256 ? 128 : extent;
					fixture.Input.ElementType = family == 0	  ? ValueType::Scalar
												: family == 1 ? ValueType::Text
															  : ValueType::Any;
					for (int index = 0; index < memberCount; ++index) {
						if (family == 0)
							fixture.Input.Elements.push_back(double(index) + 0.125);
						else if (family == 1)
							fixture.Input.Elements.push_back(
								std::string("rearrange-") + std::to_string(index)
							);
						else if (family == 2) {
							ElementValue tuple =
								index % 5 == 0	 ? ElementValue{Vector2{double(index), 2}}
								: index % 5 == 1 ? ElementValue{Vector3{double(index), 2, 3}}
								: index % 5 == 2 ? ElementValue{Vector4{double(index), 2, 3, 4}}
								: index % 5 == 3 ? ElementValue{Quaternion{double(index), 2, 3, 4}}
												 : ElementValue{Area{double(index), 2, 3, 4, 1, 2}};
							fixture.Input.Items.push_back({std::move(tuple)});
						} else if (family == 3) {
							Curve curve;
							curve.Header = {double(index), 1, 2, 3, 4, 5};
							curve.Anchors = {{{6, 7, 8, 9, 10, 11}}, {{12, 13, 14, 15, 16, 17}}};
							fixture.Input.Items.push_back({ElementValue{std::move(curve)}});
						} else {
							fixture.Input.Items.push_back({std::vector<SourceArrayItem>{
								{Image{2, 1, {uint8_t(index), 2, 3, 255, 4, 5, 6, 255}, uint64_t(index)}},
								{ElementValue{std::string("owned-") + std::to_string(index)}},
								{std::vector<SourceArrayItem>{
									{ElementValue{(int64_t{1} << 53) + index}},
									{std::vector<SourceArrayItem>{}}
								}}
							}});
						}
					}
					const Tree original = Array(fixture.Input);
					ArrayValue orders{ValueType::Integer, {}};
					std::vector<Tree> expected;
					for (int position = 0; position < memberCount; ++position) {
						const int selected = duplicates ? position % 4 : memberCount - 1 - position;
						orders.Elements.push_back(int64_t(selected));
						expected.push_back(original.Children[size_t(selected)]);
					}
					fixture.Controls = {{"orders", std::move(orders)}};
					fixture.Before = Canonical(original);
					fixture.Expected = Canonical(Row(std::move(expected)));
					fixture.NativeBefore = NativeArray(fixture.Input);
					fixture.NativeControlsBefore = NativeOwned(fixture.Controls);
					fixture.NativeDynamicBefore = NativeOwned(fixture.Dynamic);
					fixtures.push_back(std::move(fixture));
				}
		for (int special = 0; special < 8; ++special) {
			Fixture fixture;
			fixture.Type = "pc.array_rearrange";
			fixture.OmitOrders = special == 0;
			fixture.OutputPort = "array";
			fixture.Input.ElementType = special == 5 ? ValueType::Text : ValueType::Any;
			if (special < 5)
				fixture.Input.Items = {
					{ElementValue{std::string("kept")}},
					{Image{2, 1, {1, 2, 3, 255, 4, 5, 6, 255}, 127}},
					{std::vector<SourceArrayItem>{}}
				};
			if (special == 1) fixture.Controls = {{"orders", ArrayValue{ValueType::Integer, {}}}};
			if (special == 2) fixture.Controls = {{"orders", int64_t{9}}};
			if (special == 3) fixture.Controls = {{"orders", ArrayValue{ValueType::Integer, {int64_t{2}}}}};
			if (special == 4)
				fixture.Controls = {
					{"orders", ArrayValue{ValueType::Integer, {int64_t{1}, int64_t{-1}, int64_t{3}}}}
				};
			const Tree original = Array(fixture.Input);
			fixture.Before = Canonical(original);
			fixture.Expected =
				special == 4 ? Canonical(Row({original.Children[1], Number(0), Number(0)})) : fixture.Before;
			if (special >= 5 && special < 7) fixture.ExpectedArrayType = fixture.Input.ElementType;
			if (special == 7) {
				fixture.ImageInput = ImageArray{};
				fixture.ExpectedImageArray = true;
			}
			fixture.NativeBefore = NativeArray(fixture.Input);
			fixture.NativeControlsBefore = NativeOwned(fixture.Controls);
			fixture.NativeDynamicBefore = NativeOwned(fixture.Dynamic);
			fixtures.push_back(std::move(fixture));
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
			// Equivalent source shapes must remain distinct native ownership records.
			const ArrayValue packed{ValueType::Vector2, {Vector2{1, 2}}};
			ArrayValue general{ValueType::Any, {}};
			general.Items = {
				{std::vector<SourceArrayItem>{{ElementValue{double{1}}}, {ElementValue{double{2}}}}}
			};
			const ArrayValue legacyRows{ValueType::Scalar, {}, {{1.0, 2.0}}};
			if (Canonical(Array(packed)) != Canonical(Array(general)) ||
				Canonical(Array(legacyRows)) != Canonical(Array(general)) ||
				NativeArray(packed) == NativeArray(general) ||
				NativeArray(legacyRows) == NativeArray(general))
				throw std::runtime_error("native input oracle loses carrier or packed type");
			auto changedType = packed;
			changedType.ElementType = ValueType::Any;
			if (NativeArray(changedType) == NativeArray(packed))
				throw std::runtime_error("native input oracle loses ElementType");
			Readings.reserve(Cases.size() * 13);
			for (const auto &fixture : Cases)
				if (!detail::FindExecutor(fixture.Type))
					throw std::runtime_error("array profile needs reviewed executor candidate");
		}
		~Profile() {
			for (const auto &reading : Readings) {
				std::printf(
					"# array-edit-profile fixture=%zu call=%zu phase=%s input_fnv=%llu output_fnv=%llu "
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
						"# array-edit-span fixture=%zu call=%zu index=%zu parent=%u depth=%u start_ms=%.6f "
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
						"# array-edit-counter fixture=%zu call=%zu name=%s value=%.0f samples=%u\n",
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
				if (NativeArray(fixture.Input) != fixture.NativeBefore ||
					NativeOwned(fixture.Controls) != fixture.NativeControlsBefore ||
					NativeOwned(fixture.Dynamic) != fixture.NativeDynamicBefore ||
					Canonical(Array(fixture.Input)) != fixture.Before)
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
				for (const auto &[port, value] : fixture.Controls) {
					bool replaced = false;
					for (auto &[name, data] : context.Values)
						if (name == port) {
							data = value;
							replaced = true;
							break;
						}
					if (!replaced) context.Values.emplace_back(port, value);
				}
				node.DynamicInputs.reserve(fixture.Dynamic.size());
				for (const auto &[port, value] : fixture.Dynamic)
					node.DynamicInputs.push_back({port, ValueType::Any, std::nullopt});
				for (size_t dynamic = 0; dynamic < fixture.Dynamic.size(); ++dynamic)
					context.Values.emplace_back(
						node.DynamicInputs[dynamic].Id, fixture.Dynamic[dynamic].second
					);
				if (fixture.OmitOrders)
					context.Values.erase(
						std::remove_if(
							context.Values.begin(),
							context.Values.end(),
							[](const auto &input) { return input.first == "orders"; }
						),
						context.Values.end()
					);
				if (fixture.ImageInput) {
					context.Values.erase(
						std::remove_if(
							context.Values.begin(),
							context.Values.end(),
							[&](const auto &input) { return input.first == fixture.InputPort; }
						),
						context.Values.end()
					);
					context.ImageArrays.emplace_back(fixture.InputPort, &*fixture.ImageInput);
				}
				const auto imageInputBefore = fixture.ImageInput;
				const std::string contextBefore = NativeInputs(context.Values);
				const auto nativeValuesBefore = context.Values;
				uint64_t inputPayloadBytes = 0;
				for (const auto &[port, value] : context.Values)
					inputPayloadBytes += detail::ValuePayloadBytes(value);
				Reading reading;
				reading.FixtureIndex = index;
				reading.Call = Calls + 1;
				reading.InputHash = Hash(contextBefore);
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
					ENGINE_PROFILE("imagegraph.array_edit.dispatch");
					Metrics::Count("imagegraph.array_edit.dispatches", 1);
					Metrics::Count("imagegraph.array_edit.resolved_value_payload_bytes", inputPayloadBytes);
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
						if (span.Name != "imagegraph.array_edit.dispatch" || span.Depth != 0)
							throw std::runtime_error("array owner absent");
						++owners;
					} else if (span.Parent >= spanIndex || span.Depth != spans[span.Parent].Depth + 1)
						throw std::runtime_error("array hierarchy malformed");
					reading.Spans.push_back({std::string(span.Name), span});
				}
				FrameGraph::SetEnabled(restore.Enabled);
				if (owners != 1 || ok != (fixture.ExpectedStatus == Status::Ok) ||
					context.FailureCode != fixture.ExpectedStatus)
					throw std::runtime_error("array dispatch failed");
				reading.Counters = Metrics::Drain();
				if (fixture.ExpectedStatus != Status::Ok &&
					(context.FailureMessage != fixture.ExpectedMessage ||
					 context.FailurePort != fixture.InputPort || !context.OutputValues.empty() ||
					 !context.OutputImages.empty() || !context.OutputImageArrays.empty()))
					throw std::runtime_error("Unique diagnostic or unpublished output mismatch");
				Tree actual;
				bool found = false;
				for (const auto &value : context.OutputValues)
					if (value.Port == fixture.OutputPort) {
						if (!detail::ValidRuntimeValue(value.Data))
							throw std::runtime_error("array output carrier invalid");
						if (const auto *array = std::get_if<ArrayValue>(&value.Data)) {
							if (fixture.ExpectedArrayType && array->ElementType != *fixture.ExpectedArrayType)
								throw std::runtime_error("Uniform empty output category changed");
							actual = Array(*array);
						} else if (const auto *size = std::get_if<int64_t>(&value.Data)) {
							actual = {"i"};
							Word(actual.Leaf, uint64_t(*size));
						} else
							throw std::runtime_error("unexpected array profile output type");
						found = true;
					}
				for (const auto &[port, array] : context.OutputImageArrays)
					if (port == fixture.OutputPort) {
						if (fixture.ExpectedArrayType)
							throw std::runtime_error("Uniform value category became image array");
						std::vector<Tree> members;
						for (const auto &item : array.Items)
							members.push_back(ImageItem(array, item));
						actual = Row(std::move(members));
						found = true;
					}
				if (fixture.ExpectedStatus != Status::Ok) {
					actual = Tree{"diagnostic"};
					found = true;
				}
				const std::string canonical = Canonical(actual);
				if (!found || canonical != fixture.Expected ||
					(fixture.ExpectedImageArray && context.OutputImageArrays.size() != 1))
					throw std::runtime_error("array profile complete output mismatch");
				if (NativeArray(fixture.Input) != fixture.NativeBefore ||
					NativeOwned(fixture.Controls) != fixture.NativeControlsBefore ||
					NativeOwned(fixture.Dynamic) != fixture.NativeDynamicBefore ||
					Canonical(Array(fixture.Input)) != fixture.Before)
					throw std::runtime_error("array source ownership changed after dispatch");
				if (context.Values != nativeValuesBefore || NativeInputs(context.Values) != contextBefore)
					throw std::runtime_error("array edit mutated complete resolved inputs");
				const auto counter = [&](std::string_view name, bool absentAllowed = false) {
					for (const auto &value : reading.Counters)
						if (value.Name.Text() == name) return value.Value;
					if (absentAllowed) return double{0};
					throw std::runtime_error("array profile missing required boundary counter");
				};
				uint64_t publishedBytes = 0;
				for (const auto &value : context.OutputValues)
					publishedBytes += detail::ValuePayloadBytes(value.Data);
				if (counter("imagegraph.array_edit.dispatches") != 1 ||
					counter("imagegraph.array_edit.resolved_value_payload_bytes") != inputPayloadBytes ||
					counter("imagegraph.value.outputs", context.OutputValues.empty()) !=
						context.OutputValues.size() ||
					counter("imagegraph.value.output_payload_bytes", context.OutputValues.empty()) !=
						publishedBytes)
					throw std::runtime_error("array actual publication counters disagree");
				// Mutation outside capture proves singleton output owns its payload independently.
				if (fixture.Type == "pc.array_unique" && fixture.Input.Items.size() == 1) {
					if (!context.OutputImageArrays.empty())
						context.OutputImageArrays.front().second.Images.front().Pixels.front() ^= 255;
					else if (!context.OutputValues.empty())
						std::get<ArrayValue>(context.OutputValues.front().Data).Items.front() = {
							ElementValue{double{999}}
						};
				}
				if ((fixture.Type == "pc.array_uniform" || fixture.Type == "pc.array_rearrange") &&
					!actual.Children.empty()) {
					// Mutating the first output member must leave every sibling unchanged.
					if (!context.OutputImageArrays.empty()) {
						auto &output = context.OutputImageArrays.front().second;
						if (!output.Images.empty()) output.Images.front().Pixels.front() ^= 255;
						std::vector<Tree> members;
						for (const auto &item : output.Items)
							members.push_back(ImageItem(output, item));
						for (size_t member = 1; member < members.size(); ++member)
							if (Canonical(members[member]) != Canonical(actual.Children[member]))
								throw std::runtime_error("Uniform output image siblings alias");
					} else {
						auto &output = std::get<ArrayValue>(context.OutputValues.front().Data);
						if (!output.Items.empty())
							MutateItem(output.Items.front());
						else if (!output.Nested.empty())
							output.Nested.front().push_back(std::string("mutated"));
						else if (!output.Elements.empty()) {
							if (auto *text = std::get_if<std::string>(&output.Elements.front()))
								text->append("-mutated");
							else
								output.Elements.front() = double{999};
						}
						const Tree changed = Array(output);
						for (size_t member = 1; member < changed.Children.size(); ++member)
							if (Canonical(changed.Children[member]) != Canonical(actual.Children[member]))
								throw std::runtime_error("Uniform output value siblings alias");
					}
				}
				if (!SameImageArray(fixture.ImageInput, imageInputBefore) ||
					context.Values != nativeValuesBefore || NativeInputs(context.Values) != contextBefore ||
					NativeArray(fixture.Input) != fixture.NativeBefore)
					throw std::runtime_error("array output aliases resolved inputs");
				reading.OutputHash = Hash(canonical);
				Readings.push_back(std::move(reading));
			}
			++Calls;
			engine::testing::Consume(Calls);
		}
	};
}
BENCH("Diagnostic array edit/query, Unique, Uniform and Rearrange, complete owned payloads", 1) {
	static Profile profile;
	profile.Measure();
}
