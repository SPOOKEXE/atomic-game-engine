#include "PxcxKeyProvenance.hpp"
#include "TileProperties.hpp"

#include <engine/imagegraph/Catalogue.hpp>
#include <engine/imagegraph/SourceAnimatorCapture.hpp>
#include <engine/imagegraphio/PxcxEdit.hpp>
#include <engine/imagegraphio/PxcxStructureEdit.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <nlohmann/json.hpp>
#include <set>
#include <type_traits>
#include <unordered_set>

namespace engine::imagegraphio {
	namespace edit_detail {
		using namespace imagegraph;
		using Json = nlohmann::ordered_json;
		struct JsonDepthExceeded {};

		std::optional<Json> RetainedJson(std::string_view text, bool &ambiguous) {
			std::vector<std::unordered_set<std::string>> objects;
			try {
				return Json::parse(
					text.begin(), text.end(), [&](int depth, Json::parse_event_t event, Json &item) {
						if (depth > int(bake::PxcxLimits::MaximumJsonDepth)) throw JsonDepthExceeded{};
						if (event == Json::parse_event_t::object_start)
							objects.emplace_back();
						else if (event == Json::parse_event_t::object_end)
							objects.pop_back();
						else if (event == Json::parse_event_t::key &&
								 !objects.back().insert(item.get<std::string>()).second)
							ambiguous = true;
						return true;
					}
				);
			} catch (const JsonDepthExceeded &) {
				ambiguous = true;
				return std::nullopt;
			} catch (const Json::exception &) {
				return std::nullopt;
			}
		}

		bool Reject(
			Diagnostic &diagnostic, std::string reason, std::string_view node = {}, std::string_view port = {}
		) {
			diagnostic = {
				Status::UnsupportedExecution, std::string(node), std::string(port), std::move(reason)
			};
			return false;
		}
		bool Spend(size_t bytes, size_t &budget) {
			if (bytes > budget) return false;
			budget -= bytes;
			return true;
		}
		template <class T> bool Bounded(const T &, size_t &);
		bool
		BoundedItems(const std::vector<SourceArrayItem> &items, size_t depth, size_t &count, size_t &budget) {
			if (depth >= Limits::MaximumArrayDepth || items.size() > Limits::MaximumArrayElements - count ||
				!Spend(items.size() * sizeof(SourceArrayItem), budget))
				return false;
			count += items.size();
			for (const auto &item : items) {
				if (const auto *leaf = std::get_if<ElementValue>(&item.Data)) {
					if (!std::visit([&](const auto &value) { return Bounded(value, budget); }, *leaf))
						return false;
				} else if (const auto *nested = std::get_if<std::vector<SourceArrayItem>>(&item.Data)) {
					if (!BoundedItems(*nested, depth + 1, count, budget)) return false;
				} else
					return false;
			}
			return true;
		}
		template <class T> bool Bounded(const T &value, size_t &budget) {
			if (!Spend(sizeof(T), budget)) return false;
			if constexpr (std::is_same_v<T, double>)
				return std::isfinite(value);
			else if constexpr (std::is_same_v<T, std::string>)
				return value.size() <= Limits::MaximumTextBytes && Spend(value.size(), budget);
			else if constexpr (std::is_same_v<T, Vector2>)
				return std::isfinite(value.X) && std::isfinite(value.Y);
			else if constexpr (std::is_same_v<T, Vector3>)
				return std::isfinite(value.X) && std::isfinite(value.Y) && std::isfinite(value.Z);
			else if constexpr (std::is_same_v<T, Vector4> || std::is_same_v<T, Quaternion>)
				return std::isfinite(value.X) && std::isfinite(value.Y) && std::isfinite(value.Z) &&
					   std::isfinite(value.W);
			else if constexpr (std::is_same_v<T, Curve>) {
				if (value.Anchors.size() > Limits::MaximumCurveAnchors ||
					!Spend(value.Anchors.size() * sizeof(value.Anchors[0]), budget))
					return false;
				for (double number : value.Header)
					if (!std::isfinite(number)) return false;
				for (const auto &anchor : value.Anchors)
					for (double number : anchor)
						if (!std::isfinite(number)) return false;
				return true;
			} else if constexpr (std::is_same_v<T, Gradient>) {
				if (value.Mode > 6 || value.Keys.empty() || value.Keys.size() > Limits::MaximumGradientKeys ||
					!Spend(value.Keys.size() * sizeof(GradientKey), budget))
					return false;
				double previous = -1;
				for (const auto &key : value.Keys) {
					if (!std::isfinite(key.Time) || key.Time < 0 || key.Time > 1 || key.Time < previous)
						return false;
					previous = key.Time;
				}
				return true;
			} else if constexpr (std::is_same_v<T, MatrixValue>) {
				if (!value.Columns || !value.Rows ||
					uint64_t(value.Columns) * value.Rows != value.Values.size() ||
					value.Values.size() > Limits::MaximumArrayElements ||
					!Spend(value.Values.size() * sizeof(double), budget))
					return false;
				return std::all_of(value.Values.begin(), value.Values.end(), [](double number) {
					return std::isfinite(number);
				});
			} else if constexpr (std::is_same_v<T, Area>)
				return std::isfinite(value.CenterX) && std::isfinite(value.CenterY) &&
					   std::isfinite(value.HalfWidth) && std::isfinite(value.HalfHeight) &&
					   value.Shape <= 1 && value.Mode <= 2;
			else if constexpr (std::is_same_v<T, ArrayValue>) {
				if (unsigned(!value.Elements.empty()) + unsigned(!value.Nested.empty()) +
							unsigned(!value.Items.empty()) >
						1 ||
					(value.ElementType > ValueType::Vector2 && value.ElementType != ValueType::Any))
					return false;
				size_t count = 0, arrayBudget = std::min(budget, size_t(Limits::MaximumArrayBytes));
				const size_t initialBudget = arrayBudget;
				const auto valid = [&] {
					if (!value.Items.empty()) return BoundedItems(value.Items, 0, count, arrayBudget);
					if (value.Elements.size() > Limits::MaximumArrayElements ||
						!Spend(value.Elements.size() * sizeof(ElementValue), arrayBudget))
						return false;
					count = value.Elements.size();
					for (const auto &leaf : value.Elements)
						if (!std::visit([&](const auto &item) { return Bounded(item, arrayBudget); }, leaf))
							return false;
					if (value.Nested.size() > Limits::MaximumArrayElements - count ||
						!Spend(value.Nested.size() * sizeof(std::vector<ElementValue>), arrayBudget))
						return false;
					count += value.Nested.size();
					for (const auto &row : value.Nested) {
						if (row.size() > Limits::MaximumArrayElements - count ||
							!Spend(row.size() * sizeof(ElementValue), arrayBudget))
							return false;
						count += row.size();
						for (const auto &leaf : row)
							if (!std::visit(
									[&](const auto &item) { return Bounded(item, arrayBudget); }, leaf
								))
								return false;
					}
					return true;
				}();
				budget -= initialBudget - arrayBudget;
				return valid;
			} else
				return std::is_same_v<T, bool> || std::is_same_v<T, int64_t> ||
					   std::is_same_v<T, EnumValue> || std::is_same_v<T, Colour>;
		}
		bool BoundedValue(const Value &value, size_t &budget) {
			return std::visit([&](const auto &item) { return Bounded(item, budget); }, value);
		}
		uint32_t Packed(const Colour &value) {
			return uint32_t(value.Red) | uint32_t(value.Green) << 8 | uint32_t(value.Blue) << 16 |
				   uint32_t(value.Alpha) << 24;
		}
		Json CurveValue(const Curve &curve) {
			Json result = Json::array();
			for (double number : curve.Header)
				result.push_back(number);
			for (const auto &anchor : curve.Anchors)
				for (double number : anchor)
					result.push_back(number);
			return result;
		}
		template <class T> std::optional<Json> Encode(const T &, const Json &);
		std::optional<Json>
		EncodeItems(const std::vector<SourceArrayItem> &items, const Json &original, size_t depth = 0) {
			if (depth >= Limits::MaximumArrayDepth) return std::nullopt;
			Json result = Json::array();
			for (size_t index = 0; index < items.size(); ++index) {
				const Json empty;
				const auto &old = original.is_array() && index < original.size() ? original[index] : empty;
				std::optional<Json> encoded;
				if (const auto *leaf = std::get_if<ElementValue>(&items[index].Data))
					encoded = std::visit([&](const auto &value) { return Encode(value, old); }, *leaf);
				else if (const auto *nested = std::get_if<std::vector<SourceArrayItem>>(&items[index].Data))
					encoded = EncodeItems(*nested, old, depth + 1);
				if (!encoded) return std::nullopt;
				result.push_back(std::move(*encoded));
			}
			return result;
		}
		template <class T> std::optional<Json> Encode(const T &value, const Json &original) {
			if constexpr (std::is_same_v<T, bool> || std::is_same_v<T, int64_t> ||
						  std::is_same_v<T, double> || std::is_same_v<T, std::string>)
				return Json(value);
			else if constexpr (std::is_same_v<T, EnumValue>)
				return Json(value.Value);
			else if constexpr (std::is_same_v<T, Colour>)
				return Json(Packed(value));
			else if constexpr (std::is_same_v<T, Vector2>)
				return Json::array({value.X, value.Y});
			else if constexpr (std::is_same_v<T, Vector3>)
				return Json::array({value.X, value.Y, value.Z});
			else if constexpr (std::is_same_v<T, Vector4> || std::is_same_v<T, Quaternion>)
				return Json::array({value.X, value.Y, value.Z, value.W});
			else if constexpr (std::is_same_v<T, Area>)
				return Json::array(
					{value.CenterX, value.CenterY, value.HalfWidth, value.HalfHeight, value.Shape, value.Mode}
				);
			else if constexpr (std::is_same_v<T, Curve>)
				return CurveValue(value);
			else if constexpr (std::is_same_v<T, Gradient>) {
				if (!original.is_string()) return std::nullopt;
				bool ambiguous = false;
				auto decoded = RetainedJson(original.template get<std::string>(), ambiguous);
				if (!decoded || ambiguous) return std::nullopt;
				Json gradient = std::move(*decoded);
				if (!gradient.is_object() || !gradient.contains("keys") || !gradient["keys"].is_array() ||
					gradient["keys"].size() != value.Keys.size())
					return std::nullopt;
				gradient["type"] = value.Mode;
				for (size_t index = 0; index < value.Keys.size(); index++) {
					if (!gradient["keys"][index].is_object()) return std::nullopt;
					gradient["keys"][index]["time"] = value.Keys[index].Time;
					gradient["keys"][index]["value"] = Packed(value.Keys[index].Color);
				}
				return Json(gradient.dump());
			} else if constexpr (std::is_same_v<T, MatrixValue>) {
				if (original.is_array()) {
					if (value.Columns != value.Rows || original.size() != value.Values.size())
						return std::nullopt;
					return Json(value.Values);
				}
				if (!original.is_object()) return std::nullopt;
				const auto size = original.find("size"), raw = original.find("raw");
				if (size == original.end() || !size->is_array() || size->size() != 2 ||
					raw == original.end() || !raw->is_array() || !(*size)[0].is_number() ||
					!(*size)[1].is_number())
					return std::nullopt;
				const double cells = (*size)[0].template get<double>() * (*size)[1].template get<double>();
				if (cells != double(raw->size())) return std::nullopt;
				const auto count = original.find("isize");
				if (count != original.end() &&
					(!count->is_number() || count->template get<double>() != cells))
					return std::nullopt;
				Json result = original;
				result["size"] = Json::array({value.Columns, value.Rows});
				result["isize"] = value.Values.size();
				result["raw"] = value.Values;
				return result;
			} else if constexpr (std::is_same_v<T, ArrayValue>) {
				if (!value.Items.empty()) return EncodeItems(value.Items, original);
				Json result = Json::array();
				if (!value.Nested.empty()) {
					for (size_t row = 0; row < value.Nested.size(); ++row) {
						Json encoded = Json::array();
						for (size_t index = 0; index < value.Nested[row].size(); ++index) {
							const Json empty;
							const auto &old = original.is_array() && row < original.size() &&
													  original[row].is_array() && index < original[row].size()
												  ? original[row][index]
												  : empty;
							auto leaf = std::visit(
								[&](const auto &item) { return Encode(item, old); }, value.Nested[row][index]
							);
							if (!leaf) return std::nullopt;
							encoded.push_back(std::move(*leaf));
						}
						result.push_back(std::move(encoded));
					}
					return result;
				}
				for (size_t index = 0; index < value.Elements.size(); index++) {
					const Json empty;
					const Json &old =
						original.is_array() && index < original.size() ? original[index] : empty;
					auto leaf = std::visit(
						[&](const auto &item) { return Encode(item, old); }, value.Elements[index]
					);
					if (!leaf) return std::nullopt;
					result.push_back(std::move(*leaf));
				}
				return result;
			} else
				return std::nullopt;
		}
		std::optional<Json> EncodeValue(const Value &value, const Json &original) {
			return std::visit([&](const auto &item) { return Encode(item, original); }, value);
		}
		std::string DriverName(const KeyframeSourceDriver &driver) {
			return std::visit(
				[](const auto &item) -> std::string {
					using T = std::decay_t<decltype(item)>;
					if constexpr (std::is_same_v<T, KeyframeLinearDriver>)
						return "linear";
					else if constexpr (std::is_same_v<T, KeyframeSineDriver>)
						return "sine";
					else if constexpr (std::is_same_v<T, KeyframeSnapDriver>)
						return "snap";
					else if constexpr (std::is_same_v<T, KeyframeBounceDriver>)
						return "bounce";
					else if constexpr (std::is_same_v<T, KeyframeElasticDriver>)
						return "elastic";
					else if constexpr (std::is_same_v<T, KeyframeCurveDriver>)
						return "curve";
					else
						return {};
				},
				driver
			);
		}
		bool DriverFieldsKnown(const Json &record) {
			if (!record.is_object()) return true;
			const std::string kind = record.value("typ", std::string{});
			for (const auto &[name, value] : record.items()) {
				if (name == "typ") continue;
				const bool known = kind == "linear" ? name == "spd"
								   : kind == "sine"
									   ? (name == "fre" || name == "amp" || name == "phs" || name == "smt")
								   : kind == "snap" ? name == "snp"
								   : (kind == "bounce" || kind == "elastic")
									   ? (name == "amo" || name == "amp" || name == "stp")
								   : kind == "curve" ? name == "crv"
													 : false;
				if (!known) return false;
			}
			return true;
		}

		std::optional<Json>
		DriverValue(const std::optional<KeyframeSourceDriver> &driver, const Json &original) {
			if (!driver) {
				if (!DriverFieldsKnown(original)) return std::nullopt;
				return Json(0);
			}
			if (!ValidKeyframeSourceDriver(*driver)) return std::nullopt;
			const std::string name = DriverName(*driver);
			if (name.empty()) return std::nullopt;
			const bool same = original.is_object() && original.value("typ", std::string{}) == name;
			if (!same && !DriverFieldsKnown(original)) return std::nullopt;
			Json result = same ? original : Json::object();
			result["typ"] = name;
			std::visit(
				[&](const auto &item) {
					using T = std::decay_t<decltype(item)>;
					if constexpr (std::is_same_v<T, KeyframeLinearDriver>)
						result["spd"] = item.Speed;
					else if constexpr (std::is_same_v<T, KeyframeSineDriver>) {
						result["fre"] = item.Frequency;
						result["amp"] = item.Amplitude;
						result["phs"] = item.Phase;
						result["smt"] = item.Smooth;
					} else if constexpr (std::is_same_v<T, KeyframeSnapDriver>)
						result["snp"] = item.Size;
					else if constexpr (std::is_same_v<T, KeyframeBounceDriver> ||
									   std::is_same_v<T, KeyframeElasticDriver>) {
						result["amo"] = item.Amount;
						result["amp"] = item.Spacing;
						result["stp"] = item.Curve;
					} else if constexpr (std::is_same_v<T, KeyframeCurveDriver>)
						result["crv"] = CurveValue(item.Data);
				},
				*driver
			);
			return result;
		}
		bool JsonStringBytes(std::string_view text, size_t &budget) {
			if (!Spend(2, budget)) return false;
			for (unsigned char letter : text) {
				const size_t bytes = letter == '"' || letter == '\\' ? 2
									 : letter < 32 ? (letter == '\b' || letter == '\f' || letter == '\n' ||
															  letter == '\r' || letter == '\t'
														  ? 2
														  : 6)
												   : 1;
				if (!Spend(bytes, budget)) return false;
			}
			return true;
		}
		bool JsonBytes(const Json &value, size_t &budget) {
			if (value.is_string()) return JsonStringBytes(value.get_ref<const std::string &>(), budget);
			if (value.is_object() || value.is_array()) {
				if (!Spend(2 + (value.empty() ? 0 : value.size() - 1), budget)) return false;
				for (const auto &[name, item] : value.items()) {
					if (value.is_object() && (!JsonStringBytes(name, budget) || !Spend(1, budget)))
						return false;
					if (!JsonBytes(item, budget)) return false;
				}
				return true;
			}
			return Spend(value.dump().size(), budget);
		}

		std::string GlobalNodeId(const Json &root) {
			std::string id = "$project-global";
			while (std::any_of(root.at("nodes").begin(), root.at("nodes").end(), [&](const auto &n) {
				return n.value("id", std::string{}) == id;
			}))
				id += "_";
			return id;
		}
		Json *SourceNode(Json &root, std::string_view id) {
			if (id == GlobalNodeId(root)) {
				auto saved = root.find("global");
				if (saved == root.end()) saved = root.find("global_node");
				if (saved != root.end() && saved->is_object()) return &*saved;
			}

			for (auto &node : root["nodes"])
				if (node.value("id", std::string{}) == id) return &node;
			return nullptr;
		}
		Node *NativeNode(Document &document, std::string_view id) {
			for (auto &node : document.Nodes)
				if (node.Id == id) return &node;
			return nullptr;
		}
		const Node *NativeNode(const Document &document, std::string_view id) {
			for (const auto &node : document.Nodes)
				if (node.Id == id) return &node;
			return nullptr;
		}
		const CatalogueInput *EditCatalogueInput(const CatalogueEntry &entry, std::string_view port) {
			if (const auto *input = FindCatalogueInput(entry, port)) return input;
			size_t group;
			return FindDynamicTemplate(entry, port, group);
		}
		Json *FixedInput(Json &source, const CatalogueEntry &entry, std::string_view port) {
			if (entry.Type == "pc.global_scope") {
				if (!source.contains("inputs") || !source["inputs"].is_array()) return nullptr;
				for (auto &record : source["inputs"])
					if (record.is_object() && record.value("global_name", std::string{}) == port)
						return &record;
				return nullptr;
			}
			const auto *input = FindCatalogueInput(entry, port);
			size_t index = 0;
			if (input && input->SourceIndex >= 0)
				index = size_t(input->SourceIndex);
			else {
				size_t group;
				input = FindDynamicTemplate(entry, port, group);
				if (!input || input->SourceIndex < 0 || entry.DynamicGroupLength <= 0) return nullptr;
				if (group > bake::PxcxLimits::MaximumInputsPerNode / size_t(entry.DynamicGroupLength))
					return nullptr;
				index = size_t(entry.DynamicFixedLength) + group * size_t(entry.DynamicGroupLength) +
						size_t(input->SourceIndex);
			}
			if (!source.contains("inputs") || !source["inputs"].is_array() ||
				index >= source["inputs"].size())
				return nullptr;
			Json &record = source["inputs"][index];
			return record.is_object() ? &record : nullptr;
		}
		Json *SourceInput(Json &root, Json &source, const CatalogueEntry &entry, std::string_view port) {
			if (entry.Type != "pc.group_input" || port != "parent_value")
				return FixedInput(source, entry, port);
			const auto groupId = source.find("group"), id = source.find("id");
			if (groupId == source.end() || !groupId->is_string() || id == source.end() || !id->is_string())
				return nullptr;
			auto *group = SourceNode(root, groupId->get_ref<const std::string &>());
			if (!group || !group->contains("attri") || !(*group)["attri"].contains("custom_input_list") ||
				!group->contains("inputs"))
				return nullptr;
			auto &list = (*group)["attri"]["custom_input_list"];
			auto &inputs = (*group)["inputs"];
			if (!list.is_array() || !inputs.is_array()) return nullptr;
			const auto item = std::find(list.begin(), list.end(), *id);
			if (item == list.end() || std::find(item + 1, list.end(), *id) != list.end()) return nullptr;
			const size_t offset = group->value("type", "") == "Node_Group" ? 0 : 4;
			const size_t index = size_t(item - list.begin()) + offset;
			return index < inputs.size() && inputs[index].is_object() ? &inputs[index] : nullptr;
		}
		std::optional<uint8_t> Side(std::string_view name) {
			if (name == "linear") return 0;
			if (name == "bezier") return 1;
			if (name == "cut") return 2;
			return std::nullopt;
		}
		bool AuthoredKeyRecord(
			const Document &document,
			const Node &node,
			std::string_view port,
			Json &record,
			Diagnostic &diagnostic
		) {
			const auto fail = [&](std::string reason) {
				return Reject(diagnostic, std::move(reason), node.Id, port);
			};
			if (record.contains("from_node") || record.value("global_use", false)) return true;
			std::vector<const Keyframe *> keys;
			for (const auto &key : document.Keyframes)
				if (key.NodeId == node.Id && key.Port == port) keys.push_back(&key);
			const bool animated =
				std::find(node.SourceAnimatedInputs.begin(), node.SourceAnimatedInputs.end(), port) !=
				node.SourceAnimatedInputs.end();
			if (keys.empty()) {
				const auto value = std::find_if(node.Values.begin(), node.Values.end(), [&](const auto &v) {
					return v.Port == port;
				});
				if (value == node.Values.end()) return true;
				const Json old =
					record.contains("r") && record["r"].is_object() ? record["r"].value("d", Json{}) : Json{};
				auto encoded = EncodeValue(value->Data, old);
				if (!encoded) return fail("PXC group parent has no inverse raw codec");
				if (!record.contains("r") || !record["r"].is_object()) record["r"] = Json::object();
				record["r"]["d"] = std::move(*encoded);
				record["anim"] = animated;
				return true;
			}
			if (record.contains("r") && record["r"].is_object() &&
				std::any_of(keys.begin(), keys.end(), [](const auto *key) {
					return !key->SourceKeyId.empty() && key->SourceKeyId != detail::CompactSourceKeyId;
				}))
				return fail("PXC source key identity does not belong to this compact archive input");
			if (!animated && keys.size() == 1 && GetFrameTime(*keys.front()) == FrameTime{} &&
				keys.front()->Kind == KeyframeKind::Normal && !keys.front()->SourceDriver &&
				record.contains("r") && record["r"].is_object()) {
				auto encoded = EncodeValue(keys.front()->Data, record["r"].value("d", Json{}));
				if (!encoded) return fail("PXC group parent has no inverse compact codec");
				record["r"]["d"] = std::move(*encoded);
				record["anim"] = false;
				return true;
			}
			const Json previous = record.value("r", Json{});
			if (previous.is_object() && (previous.size() != 1 || !previous.contains("d")))
				return fail("PXC group animation expansion would discard compact source fields");
			const auto sourceTime = [](const Json &candidate) -> std::optional<FrameTime> {
				FrameTime time;
				if (!candidate.is_array() || candidate.size() < 8 || !candidate[0].is_array() ||
					candidate[0].size() < 2 || !candidate[0][1].is_number() ||
					!SplitFrameTime(candidate[0][1].get<double>(), time))
					return std::nullopt;
				return time;
			};
			struct SourceRecord {
				detail::SourceKeyIdText Identity;
				size_t Index = 0;
				bool Consumed = false;
			};
			const size_t recordCount = previous.is_array() ? previous.size() : 0;
			if (recordCount > Limits::MaximumKeyframes ||
				recordCount > Limits::MaximumArrayBytes / sizeof(SourceRecord))
				return fail("PXC source key identity table exceeds its payload limit");
			std::vector<SourceRecord> records;
			records.reserve(recordCount);
			for (size_t index = 0; index < recordCount; ++index) {
				const auto time = sourceTime(previous[index]);
				if (!time) continue;
				const auto kind = previous[index][0][0] == 0 ? KeyframeKind::Normal : KeyframeKind::Adder;
				const auto identity = detail::SourceKeyId(*time, kind, index);
				if (!identity) return fail("PXC source key identity exceeds its byte limit");
				records.push_back({*identity, index, false});
			}
			std::sort(records.begin(), records.end(), [](const auto &a, const auto &b) {
				return a.Identity.View() < b.Identity.View();
			});
			Json expanded = Json::array();
			for (const auto *key : keys) {
				if (key->Interpolation != "source" || !key->Ease || !Side(key->Ease->InType) ||
					!Side(key->Ease->OutType))
					return fail("PXC group parent key has no source easing mapping");
				const double frame = double(FrameTimeToReal(GetFrameTime(*key)));
				FrameTime exact;
				if (!SplitFrameTime(frame, exact) || exact != GetFrameTime(*key))
					return fail("PXC group key clock is not exactly representable");
				Json old;
				std::optional<size_t> recordIndex;
				if (previous.is_array() && !key->SourceKeyId.empty()) {
					const auto found = std::lower_bound(
						records.begin(),
						records.end(),
						key->SourceKeyId,
						[](const auto &record, std::string_view id) { return record.Identity.View() < id; }
					);
					if (found == records.end() || found->Identity.View() != key->SourceKeyId)
						return fail("PXC group source key identity does not belong to this archive input");
					if (found->Consumed) return fail("PXC group source key identity is duplicated");
					found->Consumed = true;
					recordIndex = found->Index;
					old = previous[*recordIndex];
				} else if (!key->SourceKeyId.empty() && key->SourceKeyId != detail::CompactSourceKeyId)
					return fail("PXC group source key identity does not belong to this archive input");
				if (!old.is_array())
					old = Json::array(
						{Json::array({0, frame}),
						 Json{},
						 Json::array({0, 1}),
						 Json::array({0, 0}),
						 0,
						 0,
						 true,
						 0,
						 uint32_t(0xffffff)}
					);
				auto encoded = EncodeValue(key->Data, old[1]);
				auto driver = DriverValue(key->SourceDriver, old[7]);
				if (!encoded || !driver) return fail("PXC group key has no inverse raw codec");
				old[0][0] = key->Kind == KeyframeKind::Normal ? 0 : 1;
				old[0][1] = frame;
				if (key->Kind == KeyframeKind::Adder && old[0].size() < 3) old[0].push_back(0);
				old[1] = std::move(*encoded);
				old[2] = {key->Ease->In.X, key->Ease->In.Y};
				old[3] = {key->Ease->Out.X, key->Ease->Out.Y};
				old[4] = *Side(key->Ease->InType);
				old[5] = *Side(key->Ease->OutType);
				old[7] = std::move(*driver);
				expanded.push_back(std::move(old));
			}
			record["r"] = std::move(expanded);
			record["anim"] = animated;
			const auto track =
				std::find_if(document.Tracks.begin(), document.Tracks.end(), [&](const auto &t) {
					return t.NodeId == node.Id && t.Port == port;
				});
			if (track != document.Tracks.end()) {
				constexpr std::string_view ends[]{"hold", "loop", "ping", "wrap"};
				const auto end = std::find(std::begin(ends), std::end(ends), track->End);
				if (end == std::end(ends)) return fail("PXC group parent has no inverse end mode");
				record["on_end"] = size_t(end - std::begin(ends));
				record["loop_range"] = track->LoopRange;
			}
			return true;
		}
		bool ResetLastKey(
			const PxcxKeyframeDeleteEdit &operation,
			Json &source,
			Json &input,
			const CatalogueEntry &entry,
			Document &intended,
			Node &node,
			std::set<std::tuple<std::string, std::string, uint64_t, double, bool>> &targets,
			size_t &headroom,
			Diagnostic &diagnostic
		) {
			const auto reject = [&](std::string reason) {
				return Reject(diagnostic, std::move(reason), operation.NodeId, operation.Port);
			};
			if (!operation.CaptureTime)
				return reject("PXC last-key deletion requires transient source snapshot semantics");
			const auto *schema = EditCatalogueInput(entry, operation.Port);
			if (!schema) return reject("PXC last-key input is not represented");
			struct Snapshot {
				Json *Input = nullptr;
				const CatalogueInput *Schema = nullptr;
				SourceAnimatorCapture Capture;
				Json Encoded;
			};
			std::array<Snapshot, 2> snapshots{};
			snapshots[0].Input = &input;
			snapshots[0].Schema = schema;
			size_t count = 1;
			if (schema->Type == ValueType::Gradient) {
				const auto *map = FindCatalogueInput(entry, operation.Port + "_map");
				const auto *range = FindCatalogueInput(entry, operation.Port + "_map_range");
				if (map) {
					if (!range || map->SourceIndex < 0 || range->SourceIndex != map->SourceIndex + 1 ||
						range->Type != ValueType::Vector4)
						return reject("PXC gradient hidden range association is unrepresented");
					auto *record = FixedInput(source, entry, range->Id);
					if (record && record->value("anim", false)) {
						if (record->contains("from_node") || record->contains("global_key") ||
							record->contains("global_use"))
							return reject("PXC gradient hidden range capture has unsupported controls");
						snapshots[1].Input = record;
						snapshots[1].Schema = range;
						++count;
					}
				}
			}
			for (size_t index = 0; index < count; ++index) {
				auto &snapshot = snapshots[index];
				const std::string port(snapshot.Schema->Id);
				const auto matches = [&](const Keyframe &key) {
					return key.NodeId == node.Id && key.Port == port;
				};
				const auto begin =
					std::find_if(intended.Keyframes.begin(), intended.Keyframes.end(), matches);
				if (begin == intended.Keyframes.end()) return reject("PXC capture track is missing");
				const auto end = std::find_if(begin, intended.Keyframes.end(), [&](const auto &key) {
					return !matches(key);
				});
				if (std::any_of(end, intended.Keyframes.end(), matches))
					return reject("PXC capture keys are not contiguous");
				const auto &records = (*snapshot.Input)["r"];
				if (!records.is_array() || records.size() != size_t(end - begin))
					return reject("PXC capture source track differs from projection");
				if (index == 0 && (records.size() != 1 || GetFrameTime(*begin) != operation.OriginalTime))
					return reject("PXC original key identity is missing");
				for (size_t row = 0; row < records.size(); ++row) {
					FrameTime time;
					if (!records[row].is_array() || records[row].size() < 8 || !records[row][0].is_array() ||
						records[row][0].size() < 2 || !records[row][0][1].is_number() ||
						!SplitFrameTime(records[row][0][1].get<double>(), time) ||
						time != GetFrameTime(begin[row]) ||
						!targets.emplace(node.Id, "key:" + port, time.Tick, time.Subframe, time.NegativeFrame)
							 .second)
						return reject("PXC capture key identity is ambiguous or already edited");
				}
				if (!targets.emplace(node.Id, "value:" + port, 0, 0, false).second)
					return reject("PXC capture value is already edited");
				if (!Spend(sizeof(SourceAnimatorCapture) + sizeof(AuthoredValue) + port.size(), headroom))
					return reject("PXC captures exceed aggregate fixed payload headroom");
				SourceAnimatorCaptureOptions options;
				options.Time = *operation.CaptureTime;
				options.AvailableOwnedBytes = headroom;
				options.TotalFrames = intended.Timeline ? intended.Timeline->Frames : 1;
				options.Kind = snapshot.Schema->SourceKind == "Trigger" ? SourceAnimatorCaptureKind::Trigger
																		: SourceAnimatorCaptureKind::Value;
				if (!intended.Timeline && begin->SourceDriver &&
					std::holds_alternative<KeyframeSineDriver>(*begin->SourceDriver))
					return reject("PXC sine snapshot requires represented project frame count");
				if (CaptureSourceDisabledAnimatorValue(
						std::span<const Keyframe>(&*begin, size_t(end - begin)),
						options,
						snapshot.Capture,
						diagnostic
					) != Status::Ok)
					return false;
				if (!Spend(snapshot.Capture.ResidentOwnedBytes, headroom))
					return reject("PXC captures exceed aggregate resident payload headroom");
				auto encoded = EncodeValue(snapshot.Capture.Data, records.front()[1]);
				if (!encoded) return reject("PXC capture has no reversible raw value codec");
				snapshot.Encoded = std::move(*encoded);
				if (snapshot.Schema->Type == ValueType::Colour &&
					std::holds_alternative<double>(snapshot.Capture.Data)) {
					const double raw = std::get<double>(snapshot.Capture.Data);
					if (raw < 0 || raw > UINT32_MAX || std::floor(raw) != raw)
						return reject("PXC captured packed colour has no native static representation");
					const auto packed = uint32_t(raw);
					snapshot.Capture.Data = Colour{
						uint8_t(packed), uint8_t(packed >> 8), uint8_t(packed >> 16), uint8_t(packed >> 24)
					};
				}
			}
			if (node.Values.size() + count > Limits::MaximumPropertiesPerNode)
				return reject("PXC capture exceeds native property limit");
			const auto sourceIndex = [&](std::string_view port) {
				if (node.Type == "pc.group_input" && port == "parent_value") return int32_t{-1};
				if (const auto *direct = FindCatalogueInput(entry, port); direct && direct->SourceIndex >= 0)
					return direct->SourceIndex;
				for (const auto &candidate : entry.Inputs)
					if (candidate.SourceIndex >= 0 && port.starts_with(std::string(candidate.Id) + "_"))
						return candidate.SourceIndex;
				return INT32_MAX;
			};
			for (size_t index = 0; index < count; ++index) {
				auto &snapshot = snapshots[index];
				const std::string port(snapshot.Schema->Id);
				(*snapshot.Input)["anim"] = false;
				(*snapshot.Input)["r"] = Json{{"d", std::move(snapshot.Encoded)}};
				const auto first =
					std::find_if(intended.Keyframes.begin(), intended.Keyframes.end(), [&](const auto &key) {
						return key.NodeId == node.Id && key.Port == port;
					});
				const size_t keyIndex = size_t(first - intended.Keyframes.begin());
				const auto payload = ValueClonePayloadBytes(snapshot.Capture.Data);
				const auto textBytes = [](std::string_view text) {
					return std::max(text.size(), std::string{}.capacity()) + 1;
				};
				if (!payload || !Spend(
									*payload + sizeof(Keyframe) + textBytes(node.Id) + textBytes(port) +
										textBytes("source") + 2 * textBytes("linear"),
									headroom
								))
					return reject("PXC captured compact key exceeds aggregate headroom");
				Keyframe fresh;
				fresh.NodeId = std::string(node.Id);
				fresh.Port = std::string(port);
				fresh.Data = Value(snapshot.Capture.Data);
				fresh.Interpolation = "source";
				fresh.Ease = KeyframeEase{};
				std::erase_if(intended.Keyframes, [&](const auto &key) {
					return key.NodeId == node.Id && key.Port == port;
				});
				// The compact source representation reloads as a fresh normal key0; end/range stays local.
				intended.Keyframes.insert(intended.Keyframes.begin() + keyIndex, std::move(fresh));
				// Keep the opaque gradient range's source mode so its static getter stays raw.
				const bool opaqueGradientRange =
					node.Type == "pc.gradient" && snapshot.Schema->Id == "gradient_map_range" &&
					snapshot.Schema->SourceIndex == 16 && snapshot.Schema->SourceKind == "Vec4" &&
					snapshot.Schema->Type == ValueType::Vector4;
				if (HasNativeExecutor(node.Type) || opaqueGradientRange) {
					std::erase(node.SourceAnimatedInputs, port);
					if (std::find(node.SourceStaticInputs.begin(), node.SourceStaticInputs.end(), port) ==
						node.SourceStaticInputs.end()) {
						if (!Spend(
								(node.SourceStaticInputs.size() + 1) * sizeof(std::string) + textBytes(port),
								headroom
							))
							return reject("PXC captured static mode exceeds aggregate headroom");
						node.SourceStaticInputs.reserve(node.SourceStaticInputs.size() + 1);
						const auto before = std::find_if(
							node.SourceStaticInputs.begin(),
							node.SourceStaticInputs.end(),
							[&](const auto &mode) { return sourceIndex(mode) > snapshot.Schema->SourceIndex; }
						);
						node.SourceStaticInputs.insert(before, std::string(port));
					}
				}
				auto stored = std::find_if(node.Values.begin(), node.Values.end(), [&](const auto &value) {
					return value.Port == port;
				});
				if (stored != node.Values.end())
					stored->Data = std::move(snapshot.Capture.Data);
				else {
					const auto before =
						std::find_if(node.Values.begin(), node.Values.end(), [&](const auto &value) {
							return sourceIndex(value.Port) > snapshot.Schema->SourceIndex;
						});
					node.Values.insert(before, {port, std::move(snapshot.Capture.Data)});
				}
			}
			return true;
		}

		template <class T>
		bool ChangeKeySequence(
			const T &operation,
			Json &input,
			Document &intended,
			std::set<std::tuple<std::string, std::string, uint64_t, double, bool>> &targets,
			size_t &sequenceWork,
			Diagnostic &diagnostic
		) {
			const auto reject = [&](std::string reason) {
				return Reject(diagnostic, std::move(reason), operation.NodeId, operation.Port);
			};
			auto &records = input["r"];
			if (!input.value("anim", false) || !records.is_array() || records.empty())
				return reject("PXC key structure requires an animated expanded track");
			// Every operation scans and shifts bounded sequences; batch count alone would permit
			// quadratic work across the maximum-sized track.
			if (!Spend(intended.Keyframes.size() * 8 + records.size() * 3, sequenceWork))
				return reject("PXC key structure exceeds sequence work limit");
			if constexpr (std::is_same_v<T, PxcxKeyframeDeleteEdit>)
				if (records.size() == 1)
					return reject("PXC last-key deletion requires transient source snapshot semantics");
			const size_t resultCount =
				std::is_same_v<T, PxcxKeyframeInsertEdit> ? records.size() + 1 : records.size() - 1;
			for (const auto &track : intended.Tracks)
				if (track.NodeId == operation.NodeId && track.Port == operation.Port &&
					(track.LoopRange < -1 ||
					 (track.LoopRange >= 0 && size_t(track.LoopRange) >= resultCount)))
					return reject("PXC key structure would invalidate source loop range");
			const auto matches = [&](const Keyframe &key) {
				return key.NodeId == operation.NodeId && key.Port == operation.Port;
			};
			auto native = intended.Keyframes.begin();
			FrameTime previous;
			bool first = true;
			for (const auto &record : records) {
				FrameTime time;
				if (!record.is_array() || record.size() < 8 || !record[0].is_array() ||
					record[0].size() < 2 || !record[0][1].is_number() ||
					!SplitFrameTime(record[0][1].template get<double>(), time) ||
					(!first && CompareFrameTime(previous, time) >= 0))
					return reject("PXC key structure requires uniquely ordered source records");
				native = std::find_if(native, intended.Keyframes.end(), matches);
				if (native == intended.Keyframes.end() || GetFrameTime(*native) != time)
					return reject("PXC key structure has no exact native sequence");
				++native;
				previous = time;
				first = false;
			}
			if (std::find_if(native, intended.Keyframes.end(), matches) != intended.Keyframes.end())
				return reject("PXC key structure has no exact native sequence");
			const FrameTime time = [&] {
				if constexpr (std::is_same_v<T, PxcxKeyframeInsertEdit>)
					return GetFrameTime(operation.Replacement);
				else
					return operation.OriginalTime;
			}();
			if (!targets
					 .emplace(
						 operation.NodeId,
						 "key:" + operation.Port,
						 time.Tick,
						 time.Subframe,
						 time.NegativeFrame
					 )
					 .second)
				return reject("PXC edits target the same key twice");
			size_t index = 0;
			for (; index < records.size(); ++index) {
				FrameTime source;
				SplitFrameTime(records[index][0][1].template get<double>(), source);
				if (CompareFrameTime(source, time) >= 0) break;
			}
			auto position =
				std::find_if(intended.Keyframes.begin(), intended.Keyframes.end(), [&](const auto &key) {
					return matches(key) && CompareFrameTime(GetFrameTime(key), time) >= 0;
				});
			if constexpr (std::is_same_v<T, PxcxKeyframeInsertEdit>) {
				if (position != intended.Keyframes.end() && GetFrameTime(*position) == time)
					return reject("PXC key insertion would create an ambiguous timestamp");
				if (intended.Keyframes.size() >= Limits::MaximumKeyframes)
					return reject("PXC key insertion exceeds native keyframe limit");
				if (time.Subframe != 0 ||
					std::any_of(intended.Keyframes.begin(), intended.Keyframes.end(), [&](const auto &key) {
						return matches(key) && key.Subframe != 0;
					}))
					return reject("PXC fractional multi-key source map is unverified");
				FrameTime represented;
				const double sourceFrame = static_cast<double>(FrameTimeToReal(time));
				if (!SplitFrameTime(sourceFrame, represented) || represented != time)
					return reject("PXC key time cannot be represented exactly as a source real");
				auto data = EncodeValue(
					operation.Replacement.Data,
					std::holds_alternative<ArrayValue>(operation.Replacement.Data) ? Json::array() : Json{}
				);
				auto driver = DriverValue(operation.Replacement.SourceDriver, Json(0));
				if (!data || !driver) return reject("PXC inserted key requires an unsupported inverse codec");
				Json marker =
					Json::array({operation.Replacement.Kind == KeyframeKind::Normal ? 0 : 1, sourceFrame});
				if (operation.Replacement.Kind == KeyframeKind::Adder) marker.push_back(0);
				const auto &ease = *operation.Replacement.Ease;
				Json record = Json::array(
					{marker,
					 *data,
					 Json::array({ease.In.X, ease.In.Y}),
					 Json::array({ease.Out.X, ease.Out.Y}),
					 *Side(ease.InType),
					 *Side(ease.OutType),
					 true,
					 *driver,
					 uint32_t(0xffffff)}
				);
				if (position == intended.Keyframes.end()) {
					auto last = std::find_if(intended.Keyframes.rbegin(), intended.Keyframes.rend(), matches);
					position = last.base();
				}
				intended.Keyframes.insert(position, operation.Replacement);
				records.insert(records.begin() + index, std::move(record));
			} else {
				if (position == intended.Keyframes.end() || GetFrameTime(*position) != time)
					return reject("PXC original key identity is missing");
				if (records.size() > 2 &&
					std::any_of(intended.Keyframes.begin(), intended.Keyframes.end(), [&](const auto &key) {
						return matches(key) && &key != &*position && key.Subframe != 0;
					}))
					return reject("PXC fractional multi-key source map is unverified");
				intended.Keyframes.erase(position);
				records.erase(records.begin() + index);
			}
			return true;
		}

	}

	bool WritePxcxEdits(
		const PxcxImport &imported,
		std::span<const std::byte> expectedSource,
		std::span<const PxcxEdit> edits,
		std::vector<std::byte> &out,
		imagegraph::Diagnostic &diagnostic
	) {
		using namespace edit_detail;
		diagnostic = {};
		if (expectedSource.empty() || expectedSource.size() != imported.Source.OriginalBytes.size() ||
			!std::equal(expectedSource.begin(), expectedSource.end(), imported.Source.OriginalBytes.begin()))
			return Reject(diagnostic, "PXC edit source identity is stale");
		if (edits.size() > Limits::MaximumKeyframes)
			return Reject(diagnostic, "PXC edit count exceeds native limit");
		size_t budget = Limits::MaximumArrayBytes;
		for (const PxcxEdit &edit : edits) {
			bool valid = std::visit(
				[&](const auto &operation) {
					using T = std::decay_t<decltype(operation)>;
					if (operation.NodeId.size() > Limits::MaximumTextBytes ||
						!Spend(operation.NodeId.size(), budget))
						return false;
					if constexpr (std::is_same_v<T, PxcxNodePositionEdit>)
						return Bounded(operation.Position, budget);
					else {
						if (operation.Port.size() > Limits::MaximumTextBytes ||
							!Spend(operation.Port.size(), budget))
							return false;
						if constexpr (std::is_same_v<T, PxcxInputValueEdit>)
							return BoundedValue(operation.Data, budget);
						else if constexpr (std::is_same_v<T, PxcxKeyframeDeleteEdit>)
							return ValidFrameTime(operation.OriginalTime) &&
								   (!operation.CaptureTime || (ValidFrameTime(*operation.CaptureTime) &&
															   Spend(sizeof(FrameTime), budget))) &&
								   Spend(sizeof(FrameTime), budget);
						else {
							const auto &key = operation.Replacement;
							if constexpr (std::is_same_v<T, PxcxKeyframeEdit>)
								if (!ValidFrameTime(operation.OriginalTime)) return false;
							if (key.SourceKeyId.size() > Limits::MaximumSourceKeyIdBytes ||
								!Spend(key.SourceKeyId.size(), budget))
								return false;
							if (!ValidFrameTime(GetFrameTime(key)) || key.NodeId != operation.NodeId ||
								key.Port != operation.Port || key.Interpolation != "source" ||
								key.SineDriver ||
								(key.Kind != KeyframeKind::Normal && key.Kind != KeyframeKind::Adder) ||
								!key.Ease)
								return false;
							if (!Side(key.Ease->InType) || !Side(key.Ease->OutType) ||
								!Bounded(key.Ease->In, budget) || !Bounded(key.Ease->Out, budget))
								return false;
							if (key.SourceDriver && !ValidKeyframeSourceDriver(*key.SourceDriver))
								return false;
							if (key.SourceDriver &&
								!std::visit(
									[&](const auto &driver) {
										using D = std::decay_t<decltype(driver)>;
										if constexpr (std::is_same_v<D, KeyframeCurveDriver>)
											return Bounded(driver.Data, budget);
										else
											return Spend(sizeof(D), budget);
									},
									*key.SourceDriver
								))
								return false;
							return BoundedValue(key.Data, budget);
						}
					}
				},
				edit
			);
			if (!valid)
				return Reject(
					diagnostic, "PXC edit payload is invalid, unsupported or exceeds its aggregate limit"
				);
		}
		PxcxImport baseline;
		std::string failure;
		if (!ImportPxcxImageGraph(imported.Source, baseline, failure, imported.Options))
			return Reject(diagnostic, "PXC edit source is not a checked import: " + failure);
		if (baseline.Graph != imported.Graph)
			return Reject(diagnostic, "PXC edit projection has untracked changes");
		bool ambiguous = false;
		auto parsed = RetainedJson(
			std::string_view(baseline.Source.GraphJson.data(), baseline.Source.GraphJson.size() - 1),
			ambiguous
		);
		if (!parsed) return Reject(diagnostic, "PXC edit source JSON cannot be decoded");
		Json root = std::move(*parsed);
		Document intended = baseline.Graph;
		bool changed = false;
		std::set<std::tuple<std::string, std::string, uint64_t, double, bool>> targets;
		size_t sequenceWork = Limits::MaximumKeyframes * 16;
		size_t captureHeadroom = Limits::MaximumArrayBytes;
		for (const PxcxEdit &edit : edits) {
			bool accepted = std::visit(
				[&](const auto &operation) {
					using T = std::decay_t<decltype(operation)>;
					Json *source = SourceNode(root, operation.NodeId);
					Node *node = NativeNode(intended, operation.NodeId);
					if (!source || !node)
						return Reject(diagnostic, "PXC edit node identity is missing", operation.NodeId);
					if constexpr (std::is_same_v<T, PxcxNodePositionEdit>) {
						if (!targets.emplace(operation.NodeId, "xy", 0, 0, false).second)
							return Reject(
								diagnostic, "PXC edits target the same field twice", operation.NodeId
							);
						if (node->Position == operation.Position) return true;
						(*source)["x"] = operation.Position.X;
						(*source)["y"] = operation.Position.Y;
						node->Position = operation.Position;
					} else {
						std::vector<CatalogueInput> globalInputs;
						CatalogueEntry globalEntry{};
						const auto *entry = FindCatalogueEntry(node->Type);
						if (node->Type == "pc.global_scope") {
							globalEntry.Type = node->Type;
							for (const auto &input : node->DynamicInputs) {
								CatalogueInput schema{};
								schema.Id = input.Id;
								schema.Type = input.Type;
								schema.SourceIndex = int32_t(globalInputs.size());
								globalInputs.push_back(schema);
							}
							globalEntry.Inputs = globalInputs;
							entry = &globalEntry;
						}
						if (!entry)
							return Reject(
								diagnostic,
								"PXC edit has no reversible catalogue mapping",
								operation.NodeId,
								operation.Port
							);
						Json *input = SourceInput(root, *source, *entry, operation.Port);
						if (!input || !input->contains("r") || input->contains("from_node") ||
							input->value("global_use", false))
							return Reject(
								diagnostic,
								"PXC edit input has no reversible fixed value",
								operation.NodeId,
								operation.Port
							);
						if constexpr (std::is_same_v<T, PxcxInputValueEdit>) {
							if (!targets.emplace(operation.NodeId, "value:" + operation.Port, 0, 0, false)
									 .second)
								return Reject(
									diagnostic,
									"PXC edits target the same field twice",
									operation.NodeId,
									operation.Port
								);
							if (input->value("anim", false) || !(*input)["r"].is_object() ||
								!(*input)["r"].contains("d"))
								return Reject(
									diagnostic,
									"PXC static edit cannot replace an animated input",
									operation.NodeId,
									operation.Port
								);
							auto value = std::find_if(
								node->Values.begin(), node->Values.end(), [&](const auto &stored) {
									return stored.Port == operation.Port;
								}
							);
							Value *storedData = value != node->Values.end() ? &value->Data : nullptr;
							if (!storedData) {
								const auto dynamic = std::find_if(
									node->DynamicInputs.begin(),
									node->DynamicInputs.end(),
									[&](const auto &input) { return input.Id == operation.Port; }
								);
								if (dynamic != node->DynamicInputs.end() && dynamic->Default)
									storedData = &*dynamic->Default;
							}
							if (!storedData)
								return Reject(
									diagnostic,
									"PXC fixed value was not projected",
									operation.NodeId,
									operation.Port
								);
							if (*storedData == operation.Data) return true;
							auto encoded = EncodeValue(operation.Data, (*input)["r"]["d"]);
							if (!encoded)
								return Reject(
									diagnostic,
									"PXC value inverse codec cannot retain source representation",
									operation.NodeId,
									operation.Port
								);
							(*input)["r"]["d"] = std::move(*encoded);
							const auto compact = std::find_if(
								intended.Keyframes.begin(), intended.Keyframes.end(), [&](const auto &key) {
									return key.NodeId == operation.NodeId && key.Port == operation.Port;
								}
							);
							if (compact != intended.Keyframes.end()) {
								if (compact->Interpolation != "source" ||
									compact->Kind != KeyframeKind::Normal ||
									GetFrameTime(*compact) != FrameTime{} || compact->SourceDriver ||
									std::count_if(
										intended.Keyframes.begin(),
										intended.Keyframes.end(),
										[&](const auto &key) {
											return key.NodeId == operation.NodeId &&
												   key.Port == operation.Port;
										}
									) != 1)
									return Reject(
										diagnostic,
										"PXC compact animator identity differs from source",
										operation.NodeId,
										operation.Port
									);
								const auto bytes = ValueClonePayloadBytes(operation.Data);
								if (!bytes || !Spend(*bytes, budget))
									return Reject(
										diagnostic,
										"PXC compact animator value exceeds aggregate edit headroom",
										operation.NodeId,
										operation.Port
									);
								compact->Data = Value(operation.Data);
							}
							*storedData = Value(operation.Data);
							if (node->Type == "pc.group_input" && operation.Port == "parent_value") {
								const auto junction = std::find_if(
									intended.Junctions.begin(), intended.Junctions.end(), [&](const auto &j) {
										return j.Id == node->Id + "/parent-value" &&
											   j.GroupId == node->GroupId;
									}
								);
								if (junction != intended.Junctions.end()) {
									const auto bytes = ValueClonePayloadBytes(operation.Data);
									if (!bytes || !Spend(*bytes, budget))
										return Reject(
											diagnostic,
											"PXC group default copy exceeds edit headroom",
											node->Id,
											operation.Port
										);
									junction->Default = Value(operation.Data);
								}
							}
						} else if constexpr (std::is_same_v<T, PxcxKeyframeInsertEdit> ||
											 std::is_same_v<T, PxcxKeyframeDeleteEdit>) {
							bool reset = false;
							if constexpr (std::is_same_v<T, PxcxKeyframeDeleteEdit>) {
								if (input->value("anim", false) && (*input)["r"].is_array() &&
									(*input)["r"].size() == 1) {
									if (!Spend(intended.Keyframes.size() * 8 + 3, sequenceWork) ||
										!ResetLastKey(
											operation,
											*source,
											*input,
											*entry,
											intended,
											*node,
											targets,
											captureHeadroom,
											diagnostic
										))
										return false;
									reset = true;
								}
							}
							if (!reset && !ChangeKeySequence(
											  operation, *input, intended, targets, sequenceWork, diagnostic
										  ))
								return false;
						} else {
							const auto &time = operation.OriginalTime;
							if (!targets
									 .emplace(
										 operation.NodeId,
										 "key:" + operation.Port,
										 time.Tick,
										 time.Subframe,
										 time.NegativeFrame
									 )
									 .second)
								return Reject(
									diagnostic,
									"PXC edits target the same key twice",
									operation.NodeId,
									operation.Port
								);
							auto key = std::find_if(
								intended.Keyframes.begin(),
								intended.Keyframes.end(),
								[&](const auto &stored) {
									return stored.NodeId == operation.NodeId &&
										   stored.Port == operation.Port && GetFrameTime(stored) == time;
								}
							);
							if (key == intended.Keyframes.end() || !(*input)["r"].is_array())
								return Reject(
									diagnostic,
									"PXC original key identity is missing",
									operation.NodeId,
									operation.Port
								);
							if (*key == operation.Replacement) return true;
							const auto newTime = GetFrameTime(operation.Replacement);
							FrameTime represented;
							const double sourceFrame = static_cast<double>(FrameTimeToReal(newTime));
							if (!SplitFrameTime(sourceFrame, represented) || represented != newTime)
								return Reject(
									diagnostic,
									"PXC key time cannot be represented exactly as a source real",
									operation.NodeId,
									operation.Port
								);
							if (std::any_of(
									intended.Keyframes.begin(),
									intended.Keyframes.end(),
									[&](const auto &other) {
										return &other != &*key && other.NodeId == operation.NodeId &&
											   other.Port == operation.Port && GetFrameTime(other) == newTime;
									}
								))
								return Reject(
									diagnostic,
									"PXC key edit would create an ambiguous timestamp",
									operation.NodeId,
									operation.Port
								);
							if (std::any_of(
									intended.Keyframes.begin(),
									intended.Keyframes.end(),
									[&](const auto &other) {
										return &other != &*key && other.NodeId == operation.NodeId &&
											   other.Port == operation.Port &&
											   CompareFrameTime(time, GetFrameTime(other)) !=
												   CompareFrameTime(newTime, GetFrameTime(other));
									}
								))
								return Reject(
									diagnostic,
									"PXC key edit would reorder source records",
									operation.NodeId,
									operation.Port
								);
							Json *record = nullptr;
							for (auto &candidate : (*input)["r"]) {
								FrameTime sourceTime;
								if (candidate.is_array() && candidate.size() >= 8 &&
									candidate[0].is_array() && candidate[0].size() >= 2 &&
									candidate[0][1].is_number() &&
									SplitFrameTime(candidate[0][1].template get<double>(), sourceTime) &&
									sourceTime == time) {
									if (record)
										return Reject(
											diagnostic,
											"PXC original timestamp is ambiguous",
											operation.NodeId,
											operation.Port
										);
									record = &candidate;
								}
							}
							if (!record)
								return Reject(
									diagnostic,
									"PXC source key record is missing",
									operation.NodeId,
									operation.Port
								);
							const auto &replacement = operation.Replacement;
							if (replacement.Data != key->Data) {
								auto encoded = EncodeValue(replacement.Data, (*record)[1]);
								if (!encoded)
									return Reject(
										diagnostic,
										"PXC key value inverse codec is unsupported",
										operation.NodeId,
										operation.Port
									);
								(*record)[1] = std::move(*encoded);
							}
							if (replacement.SourceDriver != key->SourceDriver) {
								auto driver = DriverValue(replacement.SourceDriver, (*record)[7]);
								if (!driver)
									return Reject(
										diagnostic,
										"PXC driver edit would discard source metadata",
										operation.NodeId,
										operation.Port
									);
								(*record)[7] = std::move(*driver);
							}
							(*record)[0][0] = replacement.Kind == KeyframeKind::Normal ? 0 : 1;
							(*record)[0][1] = sourceFrame;
							if (replacement.Kind == KeyframeKind::Adder && (*record)[0].size() == 2)
								(*record)[0].push_back(0);
							// Existing expanded marker tails and all unrelated key fields remain untouched.
							(*record)[2] = Json::array({replacement.Ease->In.X, replacement.Ease->In.Y});
							(*record)[3] = Json::array({replacement.Ease->Out.X, replacement.Ease->Out.Y});
							(*record)[4] = *Side(replacement.Ease->InType);
							(*record)[5] = *Side(replacement.Ease->OutType);
							*key = replacement;
						}
					}
					changed = true;
					return true;
				},
				edit
			);
			if (!accepted) return false;
		}
		if (!changed) {
			if (!bake::WritePxcx(baseline.Source, out, failure)) return Reject(diagnostic, failure);
			return true;
		}
		if (ambiguous) return Reject(diagnostic, "PXC modified JSON has duplicate keys or excessive depth");
		size_t jsonBudget = bake::PxcxLimits::MaximumGraphJsonBytes - 1;
		if (!JsonBytes(root, jsonBudget)) return Reject(diagnostic, "PXC edited JSON exceeds its byte limit");
		bake::PxcxArchive candidate = baseline.Source;
		candidate.GraphJson = root.dump();
		candidate.GraphJson.push_back('\0');
		candidate.Nodes.clear();
		candidate.Links.clear();
		std::vector<std::byte> bytes;
		if (!bake::WritePxcx(candidate, bytes, failure))
			return Reject(diagnostic, "PXC edited archive validation failed: " + failure);
		bake::PxcxArchive checked;
		if (!bake::ReadPxcx(bytes, checked, failure)) return Reject(diagnostic, failure);
		PxcxImport projected;
		if (!ImportPxcxImageGraph(checked, projected, failure, imported.Options))
			return Reject(diagnostic, failure);
		// The import's minimum format is derived from retained source metadata, not an edited field.
		intended.FormatVersion = projected.Graph.FormatVersion;
		detail::RebaseKeyProvenance(intended, projected.Graph);
		if (intended != projected.Graph)
			return Reject(diagnostic, "PXC edited projection changed unsupported or ambiguous semantics");
		out = std::move(bytes);
		return true;
	}
	bool WritePxcxStructureEdits(
		const PxcxImport &imported,
		std::span<const std::byte> expectedSource,
		std::span<const PxcxStructureEdit> edits,
		std::vector<std::byte> &out,
		imagegraph::Diagnostic &diagnostic
	) {
		using namespace edit_detail;
		diagnostic = {};
		if (expectedSource.empty() || expectedSource.size() != imported.Source.OriginalBytes.size() ||
			!std::equal(expectedSource.begin(), expectedSource.end(), imported.Source.OriginalBytes.begin()))
			return Reject(diagnostic, "PXC structural edit source identity is stale");
		if (edits.size() > Limits::MaximumKeyframes)
			return Reject(diagnostic, "PXC structural edit count exceeds its limit");
		bake::PxcxArchive original;
		std::string failure;
		if (!bake::ReadPxcx(expectedSource, original, failure)) return Reject(diagnostic, failure);
		if (original.MetadataNumber != 121092)
			return Reject(diagnostic, "PXC structural edit save version is not source-backed");
		if (original.GraphJson != imported.Source.GraphJson ||
			original.MetadataPayload != imported.Source.MetadataPayload ||
			original.MetadataNumber != imported.Source.MetadataNumber ||
			original.MetadataText != imported.Source.MetadataText ||
			original.ThumbnailRgba != imported.Source.ThumbnailRgba ||
			original.HasThumbnailBlock != imported.Source.HasThumbnailBlock)
			return Reject(diagnostic, "PXC structural edit archive has untracked changes");
		PxcxImport baseline;
		if (!ImportPxcxImageGraph(original, baseline, failure, imported.Options))
			return Reject(diagnostic, failure);
		if (baseline.Graph != imported.Graph)
			return Reject(diagnostic, "PXC structural edit projection has untracked changes");
		bool ambiguous = false;
		auto parsed = RetainedJson(
			std::string_view(original.GraphJson.data(), original.GraphJson.size() - 1), ambiguous
		);
		if (!parsed || ambiguous) return Reject(diagnostic, "PXC structural edit JSON is ambiguous");
		Json root = std::move(*parsed);
		const Json initial = root;
		size_t payloadBudget = Limits::MaximumArrayBytes;
		size_t work = bake::PxcxLimits::MaximumLinks * 16;
		auto text = [&](std::string_view value) {
			return !value.empty() && value.size() <= Limits::MaximumTextBytes &&
				   value.find('\0') == std::string_view::npos && Spend(value.size(), payloadBudget);
		};
		auto payload = [&](const std::string &value) -> std::optional<Json> {
			if (!Spend(value.size(), payloadBudget)) return std::nullopt;
			bool invalid = false;
			auto result = RetainedJson(value, invalid);
			return invalid ? std::nullopt : result;
		};
		auto disconnect = [](Json &input) {
			input.erase("from_node");
			input.erase("from_index");
			input.erase("from_tag");
		};
		try {
			for (const auto &edit : edits) {
				if (!Spend(root["nodes"].size(), work))
					return Reject(diagnostic, "PXC structural edit work exceeds its limit");
				const bool accepted = std::visit(
					[&](const auto &operation) {
						using T = std::decay_t<decltype(operation)>;
						if constexpr (std::is_same_v<T, PxcxNodeInsert>) {
							auto record = payload(operation.RecordJson);
							if (!record || !record->is_object() || !record->contains("id") ||
								!(*record)["id"].is_string() ||
								!text((*record)["id"].template get_ref<const std::string &>()) ||
								SourceNode(root, (*record)["id"].template get_ref<const std::string &>()) ||
								root["nodes"].size() >= Limits::MaximumNodes)
								return Reject(
									diagnostic, "PXC inserted source record or identity is invalid"
								);
							root["nodes"].push_back(std::move(*record));
							return true;
						} else {
							if (!text(operation.NodeId))
								return Reject(diagnostic, "PXC structural node identity exceeds its limit");
							if constexpr (std::is_same_v<T, PxcxNodeCreate>) {
								const auto *entry = FindCatalogueSource(operation.SourceType);
								if (!text(operation.SourceType) || !entry ||
									SourceNode(root, operation.NodeId) ||
									!std::isfinite(operation.Position.X) ||
									!std::isfinite(operation.Position.Y) ||
									root["nodes"].size() >= Limits::MaximumNodes)
									return Reject(
										diagnostic,
										"PXC creation requires a known source constructor and fresh identity"
									);
								size_t inputCount =
									entry->DynamicGroupLength > 0 ? size_t(entry->DynamicFixedLength) : 0;
								for (const auto &input : entry->Inputs)
									if (input.SourceIndex >= 0)
										inputCount = std::max(inputCount, size_t(input.SourceIndex) + 1);
								if (inputCount > bake::PxcxLimits::MaximumInputsPerNode ||
									!Spend(inputCount * 2, payloadBudget))
									return Reject(diagnostic, "PXC creation input count exceeds its limit");
								Json inputs = Json::array();
								for (size_t index = 0; index < inputCount; ++index)
									inputs.push_back(Json::object());
								root["nodes"].push_back(
									Json{
										{"id", operation.NodeId},
										{"type", operation.SourceType},
										{"version", 121092},
										{"x", operation.Position.X},
										{"y", operation.Position.Y},
										{"inputs", std::move(inputs)}
									}
								);
								return true;
							} else if constexpr (std::is_same_v<T, PxcxNodeClone>) {
								if (!text(operation.TemplateId) || SourceNode(root, operation.NodeId) ||
									!std::isfinite(operation.Position.X) ||
									!std::isfinite(operation.Position.Y) ||
									root["nodes"].size() >= Limits::MaximumNodes)
									return Reject(diagnostic, "PXC clone identity or position is invalid");
								const Json *source = SourceNode(root, operation.TemplateId);
								if (!source || source->value("type", "") == "Node_Group")
									return Reject(
										diagnostic,
										"PXC group cloning requires explicit remapped source records"
									);
								size_t cloneBudget = payloadBudget;
								if (!JsonBytes(*source, cloneBudget))
									return Reject(diagnostic, "PXC cloned record exceeds payload limit");
								payloadBudget = cloneBudget;
								Json cloned = *source;
								cloned["id"] = operation.NodeId;
								cloned["x"] = operation.Position.X;
								cloned["y"] = operation.Position.Y;
								root["nodes"].push_back(std::move(cloned));
								return true;
							}
							Json *source = SourceNode(root, operation.NodeId);
							if (!source)
								return Reject(
									diagnostic, "PXC structural node identity is missing", operation.NodeId
								);
							if constexpr (std::is_same_v<T, PxcxNodeDelete>) {
								std::set<std::string> removed{operation.NodeId};
								bool expanded = true;
								while (expanded) {
									expanded = false;
									for (const auto &node : root["nodes"]) {
										if (!Spend(1, work))
											return Reject(
												diagnostic, "PXC group deletion exceeds work limit"
											);
										if (node.contains("group") && node["group"].is_string() &&
											removed.contains(node["group"].template get<std::string>()) &&
											!removed.contains(node["id"].template get<std::string>())) {
											if (!operation.IncludeGroupChildren)
												return Reject(
													diagnostic,
													"PXC group deletion requires explicit child deletion"
												);
											expanded |=
												removed.insert(node["id"].template get<std::string>()).second;
										}
									}
								}
								for (auto &node : root["nodes"]) {
									if (removed.contains(node["id"].template get<std::string>())) continue;
									if (node.contains("instanceBase") && node["instanceBase"].is_string() &&
										removed.contains(node["instanceBase"].template get<std::string>()))
										return Reject(
											diagnostic, "PXC deleted node is still an instance base"
										);
									for (auto &input : node["inputs"]) {
										if (!Spend(1, work))
											return Reject(diagnostic, "PXC deletion exceeds work limit");
										if (input.contains("from_node") && input["from_node"].is_string() &&
											removed.contains(input["from_node"].template get<std::string>()))
											disconnect(input);
									}
								}
								auto &nodes = root["nodes"].template get_ref<Json::array_t &>();
								std::erase_if(nodes, [&](const Json &node) {
									return removed.contains(node["id"].get<std::string>());
								});
								return true;
							} else if constexpr (std::is_same_v<T, PxcxLinkEdit>) {
								if (!source->contains("inputs") || !(*source)["inputs"].is_array() ||
									operation.InputIndex >= (*source)["inputs"].size() ||
									!(*source)["inputs"][operation.InputIndex].is_object())
									return Reject(diagnostic, "PXC link destination input is missing");
								Json &input = (*source)["inputs"][operation.InputIndex];
								if (!operation.From) {
									disconnect(input);
									return true;
								}
								if (!text(operation.From->NodeId))
									return Reject(diagnostic, "PXC link source identity is invalid");
								const Json *producer = SourceNode(root, operation.From->NodeId);
								if (!producer) return Reject(diagnostic, "PXC link source node is missing");
								if (const auto *entry = FindCatalogueSource(producer->value("type", ""));
									entry &&
									!(entry->SourceNode == "Node_Array_Split" &&
									  producer->contains("attri") && (*producer)["attri"].is_object() &&
									  (*producer)["attri"].contains("output_amount") &&
									  (*producer)["attri"]["output_amount"].is_number_integer() &&
									  (*producer)["attri"]["output_amount"].template get<int64_t>() >
										  operation.From->OutputIndex &&
									  operation.From->OutputIndex <= Limits::MaximumDynamicOutputsPerNode) &&
									!((entry->SourceNode == "Node_Group" ||
									   entry->SourceNode == "Node_Pixel_Builder") &&
									  producer->contains("attri") && (*producer)["attri"].is_object() &&
									  (*producer)["attri"].contains("custom_output_list") &&
									  (*producer)["attri"]["custom_output_list"].is_array() &&
									  operation.From->OutputIndex <
										  (*producer)["attri"]["custom_output_list"].size() +
											  (entry->SourceNode == "Node_Pixel_Builder" ? 2 : 0)) &&
									!std::any_of(
										entry->Outputs.begin(),
										entry->Outputs.end(),
										[&](const auto &output) {
											return output.SourceIndex >= 0 && uint32_t(output.SourceIndex) ==
																				  operation.From->OutputIndex;
										}
									))
									return Reject(diagnostic, "PXC link source output index is not declared");
								input["from_node"] = operation.From->NodeId;
								input["from_index"] = operation.From->OutputIndex;
								if (operation.From->Tag)
									input["from_tag"] = *operation.From->Tag;
								else
									input.erase("from_tag");
								return true;
							} else if constexpr (std::is_same_v<T, PxcxDynamicInputInsert> ||
												 std::is_same_v<T, PxcxDynamicInputDelete>) {
								const auto *entry = FindCatalogueSource(source->value("type", ""));
								if (!entry || entry->DynamicGroupLength <= 0 ||
									entry->DynamicFixedLength < 0 || !source->contains("inputs") ||
									!(*source)["inputs"].is_array())
									return Reject(
										diagnostic, "PXC dynamic layout has no source-backed mapping"
									);
								auto &inputs = (*source)["inputs"].template get_ref<Json::array_t &>();
								const size_t fixed = entry->DynamicFixedLength,
											 stride = entry->DynamicGroupLength;
								if (inputs.size() < fixed || (inputs.size() - fixed) % stride)
									return Reject(diagnostic, "PXC existing dynamic groups are incomplete");
								const size_t groups = (inputs.size() - fixed) / stride;
								if (operation.GroupIndex > groups || !Spend(inputs.size(), work))
									return Reject(
										diagnostic, "PXC dynamic group position or work is invalid"
									);
								const size_t offset = fixed + size_t(operation.GroupIndex) * stride;
								if constexpr (std::is_same_v<T, PxcxDynamicInputInsert>) {
									auto records = payload(operation.RecordsJson);
									if (!records || !records->is_array() || records->empty() ||
										records->size() % stride ||
										records->size() >
											bake::PxcxLimits::MaximumInputsPerNode - inputs.size() ||
										!std::all_of(
											records->begin(), records->end(), [](const auto &record) {
												return record.is_object();
											}
										))
										return Reject(
											diagnostic,
											"PXC inserted dynamic groups are invalid or exceed limits"
										);
									inputs.insert(inputs.begin() + offset, records->begin(), records->end());
								} else {
									if (!operation.GroupCount ||
										operation.GroupCount > groups - operation.GroupIndex)
										return Reject(diagnostic, "PXC dynamic deletion range is invalid");
									inputs.erase(
										inputs.begin() + offset,
										inputs.begin() + offset + size_t(operation.GroupCount) * stride
									);
								}
								return true;
							} else
								return true;
						}
					},
					edit
				);
				if (!accepted) return false;
			}
		} catch (const Json::exception &) {
			return Reject(diagnostic, "PXC structural source record is malformed");
		}
		if (root == initial) {
			out = original.OriginalBytes;
			return true;
		}
		size_t jsonBudget = bake::PxcxLimits::MaximumGraphJsonBytes - 1;
		if (!JsonBytes(root, jsonBudget))
			return Reject(diagnostic, "PXC structural JSON exceeds its byte limit");
		bake::PxcxArchive candidate = original;
		candidate.GraphJson = root.dump();
		candidate.GraphJson.push_back('\0');
		candidate.Nodes.clear();
		candidate.Links.clear();
		std::vector<std::byte> written;
		if (!bake::WritePxcx(candidate, written, failure)) return Reject(diagnostic, failure);
		bake::PxcxArchive checked;
		if (!bake::ReadPxcx(written, checked, failure)) return Reject(diagnostic, failure);
		PxcxImport projection;
		if (!ImportPxcxImageGraph(checked, projection, failure, imported.Options))
			return Reject(diagnostic, failure);
		out = std::move(written);
		return true;
	}

	bool WritePxcxProjection(
		const PxcxImport &imported,
		const imagegraph::Document &authored,
		imagegraph::FrameTime captureTime,
		std::vector<std::byte> &out,
		imagegraph::Diagnostic &diagnostic
	) {
		using namespace edit_detail;
		std::vector<std::byte> checkedBytes;
		if (!WritePxcxStructureEdits(imported, imported.Source.OriginalBytes, {}, checkedBytes, diagnostic))
			return false;
		if (!ValidFrameTime(captureTime)) return Reject(diagnostic, "PXC projection capture time is invalid");
		Document initial = imported.Graph;
		if (Migrate(initial, diagnostic) != Status::Ok) return false;
		if (authored == initial || authored == imported.Graph) {
			out = imported.Source.OriginalBytes;
			return true;
		}
		if (authored.Nodes.size() > Limits::MaximumNodes || authored.Links.size() > Limits::MaximumLinks ||
			authored.Keyframes.size() > Limits::MaximumKeyframes ||
			authored.Groups.size() > Limits::MaximumGroups ||
			authored.Junctions.size() > Limits::MaximumJunctions)
			return Reject(diagnostic, "PXC authored projection exceeds its structural limits");
		const uint64_t comparisons =
			uint64_t(authored.Nodes.size()) * imported.Source.Nodes.size() +
			uint64_t(authored.Nodes.size()) * authored.Nodes.size() +
			uint64_t(authored.Keyframes.size()) * imported.Graph.Keyframes.size() * 2 +
			uint64_t(authored.Links.size() + imported.Graph.Links.size()) *
				(authored.Links.size() + imported.Graph.Links.size() + authored.Nodes.size());
		if (comparisons > bake::PxcxLimits::MaximumLinks * 16)
			return Reject(diagnostic, "PXC authored projection exceeds its transaction work limit");
		size_t payloadBudget = Limits::MaximumArrayBytes;
		for (const auto &node : authored.Nodes) {
			if (node.DynamicInputs.size() > MaximumDynamicInputsForType(node.Type) ||
				!Spend(
					node.Id.size() + node.Type.size() + node.SourceDisplayName.size() +
						node.SourceInternalName.size(),
					payloadBudget
				))
				return Reject(diagnostic, "PXC authored node exceeds its payload limit");
			if (node.DynamicOutputs.size() > Limits::MaximumDynamicOutputsPerNode)
				return Reject(diagnostic, "PXC authored output count exceeds its limit", node.Id);
			for (const auto &output : node.DynamicOutputs)
				if (!Spend(output.Id.size(), payloadBudget))
					return Reject(diagnostic, "PXC authored outputs exceed the payload budget", node.Id);
			for (const auto &value : node.Values)
				if (!Spend(value.Port.size(), payloadBudget) || !BoundedValue(value.Data, payloadBudget))
					return Reject(
						diagnostic, "PXC authored value exceeds its payload limit", node.Id, value.Port
					);
			for (const auto &property : node.SourceProperties)
				if (!Spend(property.Port.size(), payloadBudget) ||
					!BoundedValue(property.Data, payloadBudget))
					return Reject(
						diagnostic, "PXC source properties exceed payload budget", node.Id, property.Port
					);
			for (const auto &expression : node.SourceInputExpressions)
				if (!Spend(expression.Port.size() + expression.Code.size(), payloadBudget))
					return Reject(
						diagnostic, "PXC source expressions exceed payload budget", node.Id, expression.Port
					);
			for (const auto &input : node.DynamicInputs)
				if (!Spend(input.Id.size() + input.SourceLayerName.size(), payloadBudget) ||
					(input.Default && !BoundedValue(*input.Default, payloadBudget)))
					return Reject(
						diagnostic,
						"PXC authored dynamic values exceed their payload limit",
						node.Id,
						input.Id
					);
		}
		for (const auto &group : authored.Groups) {
			if (group.Ports.size() > Limits::MaximumGroupPorts ||
				!Spend(group.Id.size() + group.Name.size() + group.ParentId.size(), payloadBudget))
				return Reject(diagnostic, "PXC authored group exceeds its budget", group.Id);
			for (const auto &port : group.Ports)
				if (!Spend(
						port.Id.size() + port.JunctionId.size() + port.ControlNodeId.size(), payloadBudget
					))
					return Reject(diagnostic, "PXC group ports exceed their payload budget", group.Id);
		}
		for (const auto &junction : authored.Junctions)
			if (!Spend(junction.Id.size() + junction.GroupId.size(), payloadBudget) ||
				(junction.Default && !BoundedValue(*junction.Default, payloadBudget)))
				return Reject(diagnostic, "PXC authored junction exceeds its budget", junction.Id);
		std::set<std::tuple<std::string_view, std::string_view, std::string_view>> keyIdentities;
		for (const auto &key : authored.Keyframes) {
			if (key.SourceKeyId.size() > Limits::MaximumSourceKeyIdBytes ||
				!Spend(key.SourceKeyId.size(), payloadBudget) || !BoundedValue(key.Data, payloadBudget))
				return Reject(diagnostic, "PXC authored keys exceed their payload limit");
			if (!key.SourceKeyId.empty()) {
				if (!Spend(
						sizeof(std::tuple<std::string_view, std::string_view, std::string_view>) +
							4 * sizeof(void *),
						payloadBudget
					))
					return Reject(diagnostic, "PXC source key identity table exceeds its payload limit");
				if (!keyIdentities.emplace(key.NodeId, key.Port, key.SourceKeyId).second)
					return Reject(
						diagnostic, "PXC authored source key identity is duplicated", key.NodeId, key.Port
					);
			}
		}

		Document desired = authored;
		if (Migrate(desired, diagnostic) != Status::Ok) return false;
		if (desired == initial) {
			out = imported.Source.OriginalBytes;
			return true;
		}
		PxcxImport working = imported;
		bool keyAmbiguous = false;
		auto keyRoot = RetainedJson(
			std::string_view(imported.Source.GraphJson.data(), imported.Source.GraphJson.size() - 1),
			keyAmbiguous
		);
		if (!keyRoot || keyAmbiguous) return Reject(diagnostic, "PXC source key JSON is ambiguous");
		const auto wantedNode = [&](std::string_view id) -> const Node * {
			for (const auto &node : desired.Nodes)
				if (node.Id == id) return &node;
			return nullptr;
		};
		std::vector<PxcxEdit> keys;
		for (const auto &key : imported.Graph.Keyframes) {
			const auto *parentNode = wantedNode(key.NodeId);
			if (!parentNode) continue;
			if (parentNode->Type == "pc.group_input" && key.Port == "parent_value") continue;
			const auto *native = NativeNode(working.Graph, key.NodeId);
			const auto *entry = native ? FindCatalogueEntry(native->Type) : nullptr;
			Json *savedNode = SourceNode(*keyRoot, key.NodeId);
			CatalogueEntry globalEntry{};
			globalEntry.Type = "pc.global_scope";
			if (native && native->Type == "pc.global_scope") entry = &globalEntry;
			Json *savedInput =
				entry && savedNode ? SourceInput(*keyRoot, *savedNode, *entry, key.Port) : nullptr;
			if (savedInput && savedInput->contains("r") && (*savedInput)["r"].is_object()) continue;
			if (savedInput && savedInput->contains("r") && (*savedInput)["r"].is_array() &&
				std::any_of(desired.Keyframes.begin(), desired.Keyframes.end(), [&](const auto &wanted) {
					return wanted.NodeId == key.NodeId && wanted.Port == key.Port;
				}))
				continue;
			const auto wanted =
				std::find_if(desired.Keyframes.begin(), desired.Keyframes.end(), [&](const auto &item) {
					return item.NodeId == key.NodeId && item.Port == key.Port &&
						   GetFrameTime(item) == GetFrameTime(key);
				});
			if (wanted == desired.Keyframes.end())
				keys.emplace_back(
					PxcxKeyframeDeleteEdit{key.NodeId, key.Port, GetFrameTime(key), captureTime}
				);
			else if (*wanted != key)
				keys.emplace_back(PxcxKeyframeEdit{key.NodeId, key.Port, GetFrameTime(key), *wanted});
		}
		for (const auto &key : desired.Keyframes) {
			const auto *node = NativeNode(working.Graph, key.NodeId);
			if (!node) continue;
			if (node->Type == "pc.group_input" && key.Port == "parent_value") continue;
			const auto *entry = FindCatalogueEntry(node->Type);
			CatalogueEntry globalEntry{};
			globalEntry.Type = "pc.global_scope";
			if (node->Type == "pc.global_scope") entry = &globalEntry;
			auto *savedNode = SourceNode(*keyRoot, key.NodeId);
			auto *savedInput =
				entry && savedNode ? SourceInput(*keyRoot, *savedNode, *entry, key.Port) : nullptr;
			if (savedInput && savedInput->contains("r") && (*savedInput)["r"].is_array()) continue;
			const auto old = std::find_if(
				imported.Graph.Keyframes.begin(), imported.Graph.Keyframes.end(), [&](const auto &item) {
					return item.NodeId == key.NodeId && item.Port == key.Port &&
						   GetFrameTime(item) == GetFrameTime(key);
				}
			);
			if (old == imported.Graph.Keyframes.end())
				keys.emplace_back(PxcxKeyframeInsertEdit{key.NodeId, key.Port, key});
		}
		std::string failure;
		if (!keys.empty()) {
			if (!WritePxcxEdits(working, working.Source.OriginalBytes, keys, checkedBytes, diagnostic))
				return false;
			bake::PxcxArchive archive;
			if (!bake::ReadPxcx(checkedBytes, archive, failure) ||
				!ImportPxcxImageGraph(archive, working, failure, imported.Options))
				return Reject(diagnostic, failure);
		}
		bool ambiguous = false;
		auto parsed = RetainedJson(
			std::string_view(working.Source.GraphJson.data(), working.Source.GraphJson.size() - 1), ambiguous
		);
		if (!parsed || ambiguous) return Reject(diagnostic, "PXC projection source JSON is ambiguous");
		Json root = std::move(*parsed);
		const ProjectSettings previousProject = working.Graph.Project.value_or(ProjectSettings{});
		const ProjectSettings project = desired.Project.value_or(ProjectSettings{});
		if (working.Graph.Project.has_value() != desired.Project.has_value() ||
			previousProject.SurfaceWidth != project.SurfaceWidth ||
			previousProject.SurfaceHeight != project.SurfaceHeight ||
			previousProject.Interpolation != project.Interpolation ||
			previousProject.Oversample != project.Oversample || previousProject.Palette != project.Palette ||
			previousProject.ColorDepth != project.ColorDepth ||
			previousProject.Shader3D != project.Shader3D) {
			if (!project.SurfaceWidth || !project.SurfaceHeight ||
				project.SurfaceWidth > Limits::MaximumDimension ||
				project.SurfaceHeight > Limits::MaximumDimension || project.Interpolation < 0 ||
				project.Interpolation > 6 || project.Oversample < 0 || project.Oversample > 12 ||
				project.ColorDepth < 0 || project.ColorDepth > 6 || project.Shader3D < 0 ||
				project.Shader3D > 1 || project.Palette.size() > Limits::MaximumProjectPaletteEntries)
				return Reject(diagnostic, "PXC project attributes exceed source ranges");
			if (!root.contains("attributes")) root["attributes"] = Json::object();
			if (!root["attributes"].is_object())
				return Reject(diagnostic, "PXC project attributes are malformed");
			auto &attributes = root["attributes"];
			// Dimensions make the source project constructor defaults explicit when adding attributes.
			attributes["surface_dimension"] = {project.SurfaceWidth, project.SurfaceHeight};
			if (previousProject.Interpolation != project.Interpolation)
				attributes["interpolate"] = project.Interpolation;
			if (previousProject.Oversample != project.Oversample)
				attributes["oversample"] = project.Oversample;
			if (previousProject.ColorDepth != project.ColorDepth)
				attributes["color_depth"] = project.ColorDepth;
			if (previousProject.Shader3D != project.Shader3D) attributes["shader"] = project.Shader3D;
			if (previousProject.Palette != project.Palette) {
				attributes["palette"] = Json::array();
				for (const auto &colour : project.Palette)
					attributes["palette"].push_back(Packed(colour));
			}
		}
		if (previousProject.AnimationRegions != project.AnimationRegions) {
			if (!ValidProjectAnimationRegions(project))
				return Reject(diagnostic, "PXC animation regions are invalid");
			Json records = Json::array();
			for (size_t index = 0; index < project.AnimationRegions.size(); ++index) {
				const auto &region = project.AnimationRegions[index];
				Json record =
					root.contains("aRegion") && root["aRegion"].is_array() && index < root["aRegion"].size()
						? root["aRegion"][index]
						: Json::object();
				if (!record.is_object()) return Reject(diagnostic, "PXC animation region record is invalid");
				const double first = static_cast<double>(FrameTimeToReal(region.Start));
				const double last = static_cast<double>(FrameTimeToReal(region.End));
				FrameTime firstRoundtrip, lastRoundtrip;
				if (!SplitFrameTime(first, firstRoundtrip) || firstRoundtrip != region.Start ||
					!SplitFrameTime(last, lastRoundtrip) || lastRoundtrip != region.End)
					return Reject(diagnostic, "PXC animation region time loses source precision");
				record["l"] = region.Label;
				record["c"] = Packed(region.Color) & 0xffffffu;
				record["fs"] = first;
				record["fe"] = last;
				records.push_back(std::move(record));
			}
			root["aRegion"] = std::move(records);
		}

		const auto wantedGroup = [&](std::string_view id) {
			return std::any_of(desired.Groups.begin(), desired.Groups.end(), [&](const auto &group) {
				return group.Id == id;
			});
		};
		const auto indexOf =
			[&](const Json &record, std::string_view port, bool output) -> std::optional<size_t> {
			const auto *entry = FindCatalogueSource(record.value("type", ""));
			if (entry) {
				if (output) {
					if (entry->SourceNode == "Node_Array_Split" && port.starts_with("val_")) {
						size_t index = 0;
						const auto parsed =
							std::from_chars(port.data() + 4, port.data() + port.size(), index);
						if (parsed.ec == std::errc{} && parsed.ptr == port.data() + port.size() &&
							index <= Limits::MaximumDynamicOutputsPerNode)
							return index;
					}
					for (const auto &value : entry->Outputs)
						if (value.Id == port && value.SourceIndex >= 0) return size_t(value.SourceIndex);
				} else {
					if (const auto *input = FindCatalogueInput(*entry, port);
						input && input->SourceIndex >= 0)
						return size_t(input->SourceIndex);
					size_t group;
					if (const auto *input = FindDynamicTemplate(*entry, port, group);
						input && input->SourceIndex >= 0 && entry->DynamicGroupLength > 0)
						return size_t(entry->DynamicFixedLength) + group * size_t(entry->DynamicGroupLength) +
							   size_t(input->SourceIndex);
				}
			}
			const std::string_view prefix = output ? "output-" : "input-";
			if (!port.starts_with(prefix)) return std::nullopt;
			size_t index = 0;
			const auto result =
				std::from_chars(port.data() + prefix.size(), port.data() + port.size(), index);
			return result.ec == std::errc{} && result.ptr == port.data() + port.size() ? std::optional(index)
																					   : std::nullopt;
		};
		const auto expressionRecord = [&](Json &record, const Node &node, std::string_view port) {
			const SourceInputExpression *wanted = nullptr, *previous = nullptr;
			for (const auto &e : node.SourceInputExpressions)
				if (e.Port == port) wanted = &e;
			if (auto old = NativeNode(working.Graph, node.Id))
				for (const auto &e : old->SourceInputExpressions)
					if (e.Port == port) previous = &e;
			if (!wanted) {
				if (previous) {
					record.erase("global_key");
					record.erase("global_use");
				}
				return;
			}
			if (previous && *wanted == *previous) return;
			record["global_key"] = wanted->Code;
			record["global_use"] = wanted->Enabled;
		};
		try {
			if (desired.ProjectGlobalNodeId.empty() && !working.Graph.ProjectGlobalNodeId.empty()) {
				if (auto saved = SourceNode(root, working.Graph.ProjectGlobalNodeId))
					(*saved)["inputs"] = Json::array();
			}
			auto &nodes = root["nodes"].get_ref<Json::array_t &>();
			std::erase_if(nodes, [&](const Json &node) {
				const auto id = node["id"].get<std::string>();
				return !wantedNode(id) && !wantedGroup(id);
			});
			for (const auto &node : desired.Nodes) {
				if (node.Type == "pc.global_scope") {
					if (desired.ProjectGlobalNodeId != node.Id || node.Id != GlobalNodeId(root))
						return Reject(
							diagnostic, "PXC global identity must use the source project association", node.Id
						);
					Json *saved = SourceNode(root, node.Id);
					if (!saved) {
						root["global_node"] = {{"inputs", Json::array()}};
						saved = &root["global_node"];
					}
					const auto *old = NativeNode(working.Graph, node.Id);
					Json records = Json::array();
					for (const auto &input : node.DynamicInputs) {
						const DynamicInput *previous = nullptr;
						if (old)
							for (const auto &v : old->DynamicInputs)
								if (v.Id == input.Id) previous = &v;
						Json record = Json::object();
						for (const auto &r : (*saved)["inputs"])
							if (r.value("global_name", std::string{}) == input.Id) record = r;
						if (record.empty() || !previous || previous->Type != input.Type) {
							int type = 0, display = 0;
							switch (input.Type) {
							case ValueType::Integer:
								type = 0;
								break;
							case ValueType::Scalar:
								type = 1;
								break;
							case ValueType::Boolean:
								type = 2;
								break;
							case ValueType::Colour:
								type = 3;
								break;
							case ValueType::Gradient:
								type = 4;
								break;
							case ValueType::Curve:
								type = 6;
								break;
							case ValueType::Text:
								type = 7;
								break;
							case ValueType::Vector2:
								type = 1;
								display = 7;
								break;
							case ValueType::Vector3:
								type = 1;
								display = 8;
								break;
							case ValueType::Vector4:
								type = 1;
								display = 9;
								break;
							case ValueType::Array: {
								auto array =
									input.Default ? std::get_if<ArrayValue>(&*input.Default) : nullptr;
								if (!array || !array->Items.empty() || !array->Nested.empty())
									return Reject(
										diagnostic, "PXC global array shape unsupported", node.Id, input.Id
									);
								if (array->ElementType == ValueType::Colour) {
									type = 3;
									display = 1;
								} else if (array->ElementType == ValueType::Scalar &&
										   array->Elements.size() == 5) {
									type = 1;
									display = 12;
								} else
									return Reject(
										diagnostic, "PXC global array display unsupported", node.Id, input.Id
									);
								break;
							}
							default:
								return Reject(
									diagnostic, "PXC global value type unsupported", node.Id, input.Id
								);
							}
							record["global_type"] = type;
							record["global_disp"] = display;
							if (!record.contains("global_s_range"))
								record["global_s_range"] = Json::array({0, 1});
							if (!record.contains("global_s_step")) record["global_s_step"] = 0.01;
						}
						record["global_name"] = input.Id;
						expressionRecord(record, node, input.Id);
						if (!previous || input.Default != previous->Default || input.Type != previous->Type) {
							if (!input.Default || record.value("anim", false))
								return Reject(
									diagnostic,
									"PXC global animated default cannot be replaced",
									node.Id,
									input.Id
								);
							Json seed =
								record.contains("r") && record["r"].is_object() && record["r"].contains("d")
									? record["r"]["d"]
									: Json{};
							if (std::holds_alternative<ArrayValue>(*input.Default) && seed.is_null())
								seed = Json::array();
							if (auto gradient = std::get_if<Gradient>(&*input.Default);
								gradient && seed.is_null()) {
								Json g = {{"type", gradient->Mode}, {"keys", Json::array()}};
								for (const auto &key : gradient->Keys)
									g["keys"].push_back({{"time", key.Time}, {"value", Packed(key.Color)}});
								seed = g.dump();
							}
							auto value = EncodeValue(*input.Default, seed);
							if (!value)
								return Reject(
									diagnostic, "PXC global default has no codec", node.Id, input.Id
								);
							record["r"] = {{"d", std::move(*value)}};
						}
						records.push_back(std::move(record));
					}
					(*saved)["inputs"] = std::move(records);
					continue;
				}
				Json *source = SourceNode(root, node.Id);
				if (!source) {
					const auto *entry = FindCatalogueEntry(node.Type);
					if (!entry) return Reject(diagnostic, "PXC new node has no source constructor", node.Id);
					root["nodes"].push_back(
						Json{
							{"id", node.Id},
							{"type", entry->SourceNode},
							{"version", 121092},
							{"x", node.Position.X},
							{"y", node.Position.Y},
							{"inputs", Json::array()}
						}
					);
					source = &root["nodes"].back();
				}
				source->at("x") = node.Position.X;
				source->at("y") = node.Position.Y;
				const auto *oldMembership = NativeNode(working.Graph, node.Id);
				if ((oldMembership && oldMembership->SourceDisplayName != node.SourceDisplayName) ||
					(!oldMembership && !node.SourceDisplayName.empty()))
					(*source)["name"] = node.SourceDisplayName;
				if ((oldMembership && oldMembership->SourceInternalName != node.SourceInternalName) ||
					(!oldMembership && !node.SourceInternalName.empty()))
					(*source)["iname"] = node.SourceInternalName;
				if (!oldMembership || oldMembership->GroupId != node.GroupId) {
					if (!node.GroupId.empty())
						(*source)["group"] = node.GroupId;
					else
						source->erase("group");
				}
				const auto *entry = FindCatalogueSource(source->value("type", ""));
				if (entry && node.Type == entry->Type) {
					if (entry->SourceNode == "Node_Array_Split" &&
						(!oldMembership || oldMembership->DynamicOutputs != node.DynamicOutputs)) {
						if (node.DynamicOutputs.size() > Limits::MaximumDynamicOutputsPerNode)
							return Reject(diagnostic, "PXC Array Split output count exceeds limits", node.Id);
						for (size_t index = 0; index < node.DynamicOutputs.size(); ++index)
							if (node.DynamicOutputs[index].Id != "val_" + std::to_string(index + 1) ||
								node.DynamicOutputs[index].Type != ValueType::Any)
								return Reject(
									diagnostic, "PXC Array Split output order or type is unsupported", node.Id
								);
						(*source)["attri"]["output_amount"] = node.DynamicOutputs.size() + 1;
					}
					size_t count = entry->DynamicGroupLength > 0 ? size_t(entry->DynamicFixedLength)
																 : (*source)["inputs"].size();
					for (const auto &input : entry->Inputs)
						if (input.SourceIndex >= 0) count = std::max(count, size_t(input.SourceIndex) + 1);
					if (entry->DynamicGroupLength > 0)
						for (const auto &input : node.DynamicInputs) {
							const auto index = indexOf(*source, input.Id, false);
							if (!index)
								return Reject(
									diagnostic, "PXC dynamic input has no inverse index", node.Id, input.Id
								);
							count = std::max(count, *index + 1);
						}
					if (entry->SourceNode == "Node_Pixel_Builder" && source->contains("attri") &&
						(*source)["attri"].is_object()) {
						auto custom = (*source)["attri"].find("custom_input_list");
						if (custom != (*source)["attri"].end() && custom->is_array())
							count = std::max(count, size_t(4) + custom->size());
					}
					if (count > bake::PxcxLimits::MaximumInputsPerNode)
						return Reject(diagnostic, "PXC projected input count exceeds its limit");
					auto &inputs = (*source)["inputs"].get_ref<Json::array_t &>();
					inputs.resize(count, Json::object());
					const auto *oldDynamic = NativeNode(working.Graph, node.Id);
					for (const auto &input : node.DynamicInputs) {
						const DynamicInput *oldInput = nullptr;
						if (oldDynamic) {
							auto found = std::find_if(
								oldDynamic->DynamicInputs.begin(),
								oldDynamic->DynamicInputs.end(),
								[&](const auto &old) { return old.Id == input.Id; }
							);
							if (found != oldDynamic->DynamicInputs.end()) oldInput = &*found;
						}
						if ((oldInput && oldInput->SourceLayerName != input.SourceLayerName) ||
							(!oldInput && !input.SourceLayerName.empty())) {
							const auto index = indexOf(*source, input.Id, false);
							if (!index || *index >= inputs.size())
								return Reject(
									diagnostic,
									"PXC source layer binding has no inverse socket",
									node.Id,
									input.Id
								);
							auto &record = inputs[*index];
							if (input.SourceLayerName.empty()) {
								if (record.contains("attri") && record["attri"].is_object())
									record["attri"].erase("layerName");
							} else
								record["attri"]["layerName"] = input.SourceLayerName;
						}
						if (!input.Default || (oldDynamic && std::any_of(
																 oldDynamic->DynamicInputs.begin(),
																 oldDynamic->DynamicInputs.end(),
																 [&](const auto &old) {
																	 return old.Id == input.Id &&
																			old.Type == input.Type &&
																			old.Default == input.Default;
																 }
															 )))
							continue;
						const auto index = indexOf(*source, input.Id, false);
						if (!index || *index >= inputs.size())
							return Reject(
								diagnostic, "PXC dynamic default has no inverse index", node.Id, input.Id
							);
						auto &record = inputs[*index];
						if (record.value("anim", false)) continue;
						if (record.contains("r") && (!record["r"].is_object() || !record["r"].contains("d")))
							return Reject(
								diagnostic,
								"PXC dynamic default retains a noncompact animator",
								node.Id,
								input.Id
							);
						Json seed = record.contains("r") ? record["r"]["d"] : Json{};
						if (std::holds_alternative<ArrayValue>(*input.Default) && seed.is_null())
							seed = Json::array();
						auto encoded = EncodeValue(*input.Default, seed);
						if (!encoded)
							return Reject(
								diagnostic, "PXC dynamic default has no inverse codec", node.Id, input.Id
							);
						if (!record.contains("r")) record["r"] = Json::object();
						record["r"]["d"] = std::move(*encoded);
					}
					for (const auto &value : node.Values) {
						if (node.Type == "pc.group_input" && value.Port == "parent_value") continue;
						const auto *oldNode = NativeNode(working.Graph, node.Id);
						if (oldNode &&
							std::any_of(oldNode->Values.begin(), oldNode->Values.end(), [&](const auto &old) {
								return old == value;
							}))
							continue;
						if (node.Type == "pc.bevel" &&
							(value.Port == "height_mapped" || value.Port == "height_map_range")) {
							const auto *height = FindCatalogueInput(*entry, "height");
							if (!height || height->SourceIndex != 1 || height->SourceKind != "Int" ||
								inputs.size() <= 1)
								return Reject(
									diagnostic,
									"PXC Bevel mapped Height has no source slot",
									node.Id,
									value.Port
								);
							Json &record = inputs[1];
							if (!record.is_object() || record.value("anim", false) ||
								record.contains("from_node") ||
								(record.contains("r") &&
								 (!record["r"].is_object() || !record["r"].contains("d"))))
								return Reject(
									diagnostic,
									"PXC Bevel mapped Height needs a static local source value",
									node.Id,
									value.Port
								);
							if (value.Port == "height_mapped") {
								const auto *mapped = std::get_if<bool>(&value.Data);
								if (!mapped)
									return Reject(
										diagnostic,
										"PXC Bevel map toggle needs a boolean",
										node.Id,
										value.Port
									);
								if (!record.contains("attri")) record["attri"] = Json::object();
								if (!record["attri"].is_object())
									return Reject(
										diagnostic,
										"PXC Bevel input attributes are malformed",
										node.Id,
										value.Port
									);
								record["attri"]["mapped"] = *mapped;
							} else {
								const auto *range = FindCatalogueInput(*entry, value.Port);
								const auto toggle = std::find_if(
									node.Values.begin(), node.Values.end(), [](const AuthoredValue &item) {
										return item.Port == "height_mapped";
									}
								);
								const auto *mapped =
									toggle != node.Values.end() ? std::get_if<bool>(&toggle->Data) : nullptr;
								if (!range || range->SourceKind != "MapRange" ||
									range->Type != ValueType::Vector2 || !mapped || !*mapped)
									return Reject(
										diagnostic,
										"PXC Bevel endpoint pair requires its mapped source mode",
										node.Id,
										value.Port
									);
								auto encoded = EncodeValue(value.Data, Json::array());
								if (!encoded || !encoded->is_array() || encoded->size() != 2 ||
									!(*encoded)[0].is_number() || !(*encoded)[1].is_number())
									return Reject(
										diagnostic,
										"PXC Bevel map range needs two numeric endpoints",
										node.Id,
										value.Port
									);
								if (!record.contains("r")) record["r"] = Json::object();
								record["r"]["d"] = std::move(*encoded);
							}
							continue;
						}
						if (node.Type == "pc.export" && value.Port == "framerate_unit") {
							const auto *mode = std::get_if<EnumValue>(&value.Data);
							if (!mode || mode->Value < 0 || mode->Value > 1 || inputs.size() <= 8)
								return Reject(
									diagnostic,
									"PXC export framerate unit has no inverse",
									node.Id,
									value.Port
								);
							inputs[8]["attri"]["unit"] = mode->Value;
							continue;
						}
						const auto *attribute = FindCatalogueInput(*entry, value.Port);
						if (attribute && attribute->SourceIndex < 0 &&
							(value.Port.starts_with("attribute_") || value.Port == "interpolate" ||
							 value.Port == "oversample")) {
							const std::string name =
								value.Port.starts_with("attribute_") ? value.Port.substr(10) : value.Port;
							if (!source->contains("attri")) (*source)["attri"] = Json::object();
							const Json seed =
								(*source)["attri"].contains(name) ? (*source)["attri"][name] : Json{};
							auto encoded = EncodeValue(value.Data, seed);
							if (!encoded)
								return Reject(
									diagnostic,
									"PXC authored attribute has no inverse codec",
									node.Id,
									value.Port
								);
							(*source)["attri"][name] = std::move(*encoded);
							continue;
						}
						const auto index = indexOf(*source, value.Port, false);
						if (!index || *index >= inputs.size())
							return Reject(
								diagnostic, "PXC authored value has no inverse input", node.Id, value.Port
							);
						Json &record = inputs[*index];
						if (record.value("anim", false)) continue;

						if (record.contains("r") && (!record["r"].is_object() || !record["r"].contains("d")))
							return Reject(
								diagnostic,
								"PXC static value retains a noncompact animator",
								node.Id,
								value.Port
							);
						Json seed = record.contains("r") ? record["r"]["d"] : Json{};
						if (std::holds_alternative<ArrayValue>(value.Data) && seed.is_null())
							seed = Json::array();
						auto encoded = EncodeValue(value.Data, seed);
						if (!encoded)
							return Reject(
								diagnostic, "PXC authored value has no inverse codec", node.Id, value.Port
							);
						if (!record.contains("r")) record["r"] = Json::object();
						record["r"]["d"] = std::move(*encoded);
					}
				}
				if (node.Type == "pc.ase_file_read") {
					for (const auto &property : node.SourceProperties) {
						if (property.Port != "layer_visible")
							return Reject(
								diagnostic, "Aseprite source property has no inverse", node.Id, property.Port
							);
						const auto *array = std::get_if<ArrayValue>(&property.Data);
						if (!array || array->ElementType != ValueType::Boolean || !array->Nested.empty() ||
							!array->Items.empty())
							return Reject(
								diagnostic,
								"Aseprite visibility requires a boolean array",
								node.Id,
								property.Port
							);
						const auto encoded = EncodeValue(property.Data, Json::array());
						if (!encoded)
							return Reject(
								diagnostic, "Aseprite visibility has no inverse value", node.Id, property.Port
							);
						(*source)["attri"]["layer_visible"] = *encoded;
					}
					if (oldMembership &&
						std::any_of(
							oldMembership->SourceProperties.begin(),
							oldMembership->SourceProperties.end(),
							[](const auto &v) { return v.Port == "layer_visible"; }
						) &&
						std::none_of(
							node.SourceProperties.begin(), node.SourceProperties.end(), [](const auto &v) {
								return v.Port == "layer_visible";
							}
						))
						(*source)["attri"].erase("layer_visible");
				}
				if (node.Type == "pc.wav_file_read") {
					const bool *checker = nullptr;
					for (const auto &property : node.SourceProperties) {
						if (property.Port != "file_checker") continue;
						const auto *value = std::get_if<bool>(&property.Data);
						if (checker || !value)
							return Reject(
								diagnostic,
								"WAV File Watcher must be one source boolean",
								node.Id,
								property.Port
							);
						checker = value;
					}
					if (checker) {
						if (!source->contains("attri")) (*source)["attri"] = Json::object();
						if (!(*source)["attri"].is_object())
							return Reject(
								diagnostic, "WAV source attributes are malformed", node.Id, "file_checker"
							);
						(*source)["attri"]["file_checker"] = *checker;
					} else if (source->contains("attri") && (*source)["attri"].is_object())
						(*source)["attri"].erase("file_checker");
				}
				if (node.Type == "pc.mesh_warp") {
					const auto *old = NativeNode(working.Graph, node.Id);
					if (old)
						for (const auto &property : old->SourceProperties)
							if (std::none_of(
									node.SourceProperties.begin(),
									node.SourceProperties.end(),
									[&](const auto &v) { return v.Port == property.Port; }
								)) {
								if (property.Port == "pin" || property.Port == "mesh_bound") {
									if (source->contains("attri")) (*source)["attri"].erase(property.Port);
								} else if (property.Port.starts_with("control_point_")) {
									auto index = indexOf(*source, property.Port, false);
									if (!index || *index >= (*source)["inputs"].size())
										return Reject(
											diagnostic,
											"Puppet property has no inverse slot",
											node.Id,
											property.Port
										);
									(*source)["inputs"][*index].erase("r");
								}
							}
					for (const auto &property : node.SourceProperties) {
						if (old && std::any_of(
									   old->SourceProperties.begin(),
									   old->SourceProperties.end(),
									   [&](const auto &v) { return v == property; }
								   ))
							continue;
						if (property.Port == "pin" || property.Port == "mesh_bound") {
							Json seed =
								(*source).contains("attri") && (*source)["attri"].contains(property.Port)
									? (*source)["attri"][property.Port]
									: Json::array();
							auto encoded = EncodeValue(property.Data, seed);
							if (!encoded)
								return Reject(
									diagnostic,
									"Mesh Warp property has no inverse codec",
									node.Id,
									property.Port
								);
							(*source)["attri"][property.Port] = std::move(*encoded);
						} else if (property.Port.starts_with("control_point_")) {
							auto index = indexOf(*source, property.Port, false);
							auto encoded = EncodeValue(property.Data, Json::array());
							if (!index || *index >= (*source)["inputs"].size() || !encoded ||
								!encoded->is_array() || encoded->size() != 7)
								return Reject(
									diagnostic, "Puppet property has no inverse array", node.Id, property.Port
								);
							(*source)["inputs"][*index]["r"] = {{"d", std::move(*encoded)}};
						} else
							return Reject(
								diagnostic,
								"Mesh Warp source property is not whitelisted",
								node.Id,
								property.Port
							);
					}
				}
				if (node.Type == "pc.tile_tileset" || node.Type == "pc.tile_rule" ||
					node.Type == "pc.tile_convert") {
					for (const auto &property : node.SourceProperties) {
						if (auto old = NativeNode(working.Graph, node.Id);
							old && std::any_of(
									   old->SourceProperties.begin(),
									   old->SourceProperties.end(),
									   [&](const auto &p) { return p == property; }
								   ))
							continue;
						if (property.Port != "animatedTiles" && property.Port != "autoterrain" &&
							property.Port != "ruleTiles" && property.Port != "colorList" &&
							property.Port != "colorMap")
							return Reject(
								diagnostic, "tile source property is not whitelisted", node.Id, property.Port
							);
						nlohmann::json encoded;
						const nlohmann::json seed =
							source->contains("attri") && (*source)["attri"].contains(property.Port)
								? (*source)["attri"][property.Port]
								: Json::object();
						const bool valid =
							property.Port == "colorMap"
								? detail::EncodeTileColorMap(
									  property.Data, seed, encoded, Limits::MaximumEvaluationBytes
								  )
								: detail::EncodeTileProperty(
									  property.Data, encoded, Limits::MaximumEvaluationBytes
								  );
						if (!valid)
							return Reject(
								diagnostic,
								"tile source property has no bounded inverse",
								node.Id,
								property.Port
							);
						if (property.Port != "colorMap") detail::MergeTileSourceFields(encoded, seed);
						(*source)["attri"][property.Port] = std::move(encoded);
					}
				}
				if (source->contains("inputs") && (*source)["inputs"].is_array()) {
					std::vector<std::string> ports;
					for (const auto &e : node.SourceInputExpressions)
						ports.push_back(e.Port);
					if (auto old = NativeNode(working.Graph, node.Id))
						for (const auto &e : old->SourceInputExpressions)
							ports.push_back(e.Port);
					for (const auto &port : ports) {
						auto index = indexOf(*source, port, false);
						if (!index || *index >= (*source)["inputs"].size())
							return Reject(diagnostic, "PXC expression has no inverse input", node.Id, port);
						expressionRecord((*source)["inputs"][*index], node, port);
					}
				}
			}
			for (const auto &group : desired.Groups) {
				Json *source = SourceNode(root, group.Id);
				if (!source) {
					root["nodes"].push_back(
						Json{
							{"id", group.Id},
							{"type", "Node_Group"},
							{"version", 121092},
							{"x", 0},
							{"y", 0},
							{"inputs", Json::array()}
						}
					);
					source = &root["nodes"].back();
				}
				const auto old = std::find_if(
					working.Graph.Groups.begin(), working.Graph.Groups.end(), [&](const auto &stored) {
						return stored.Id == group.Id;
					}
				);
				if (old == working.Graph.Groups.end() || old->Ports != group.Ports) {
					Json inputIds = Json::array(), outputIds = Json::array();
					for (const auto &port : group.Ports) {
						const auto *control = NativeNode(desired, port.ControlNodeId);
						const auto *entry = control ? FindCatalogueEntry(control->Type) : nullptr;
						const auto kind =
							port.Direction == PortDirection::Input ? "Node_Group_Input" : "Node_Group_Output";
						if (port.ControlNodeId != port.Id || port.JunctionId != port.Id + "/parent-value" ||
							!entry || entry->SourceNode != kind || control->GroupId != group.Id)
							return Reject(
								diagnostic,
								"PXC group boundary requires canonical source controls",
								group.Id,
								port.Id
							);
						(port.Direction == PortDirection::Input ? inputIds : outputIds).push_back(port.Id);
					}
					(*source)["attri"]["custom_input_list"] = inputIds;
					(*source)["attri"]["custom_output_list"] = outputIds;
					auto &inputs = (*source)["inputs"].get_ref<Json::array_t &>();
					inputs.resize(
						inputIds.size() + (source->value("type", "") == "Node_Pixel_Builder" ? 4 : 0),
						Json::object()
					);
				}
				if (old == working.Graph.Groups.end() || old->Name != group.Name)
					(*source)["name"] = group.Name;
				if (old == working.Graph.Groups.end() || old->ParentId != group.ParentId) {
					if (group.ParentId.empty())
						source->erase("group");
					else
						(*source)["group"] = group.ParentId;
				}
				if (old == working.Graph.Groups.end() || old->ColorDepth != group.ColorDepth ||
					old->Interpolation != group.Interpolation || old->Oversample != group.Oversample) {
					if (!source->contains("attri")) (*source)["attri"] = Json::object();
					(*source)["attri"]["color_depth"] = group.ColorDepth;
					(*source)["attri"]["interpolate"] = group.Interpolation;
					(*source)["attri"]["oversample"] = group.Oversample;
				}
			}
			for (const auto &node : desired.Nodes) {
				if (node.Type != "pc.group_input") continue;
				auto *source = SourceNode(root, node.Id);
				const auto *entry = FindCatalogueEntry(node.Type);
				auto *record = source && entry ? SourceInput(root, *source, *entry, "parent_value") : nullptr;
				if (!record)
					return Reject(
						diagnostic, "PXC group parent source record is missing", node.Id, "parent_value"
					);
				if (!AuthoredKeyRecord(desired, node, "parent_value", *record, diagnostic)) return false;
			}
			// Rebuild each expanded animator once, preserving opaque records by durable provenance
			// through simultaneous moves and value swaps instead of sequential timestamp collisions.
			for (const auto &node : desired.Nodes) {
				const auto *entry = FindCatalogueEntry(node.Type);
				CatalogueEntry globalEntry{};
				globalEntry.Type = "pc.global_scope";
				if (node.Type == "pc.global_scope") entry = &globalEntry;
				auto *source = SourceNode(root, node.Id);
				if (!entry || !source) continue;
				std::set<std::string_view> ports;
				for (const auto &key : desired.Keyframes)
					if (key.NodeId == node.Id &&
						!(node.Type == "pc.group_input" && key.Port == "parent_value"))
						ports.insert(key.Port);
				for (const auto port : ports) {
					auto *record = SourceInput(root, *source, *entry, port);
					if (record && record->contains("r") && (*record)["r"].is_array() &&
						!AuthoredKeyRecord(desired, node, port, *record, diagnostic))
						return false;
				}
			}
			const auto boundaryPort = [&](
										  const Document &document, std::string_view junction
									  ) -> std::pair<const Group *, const GroupPort *> {
				for (const auto &group : document.Groups)
					for (const auto &port : group.Ports)
						if (port.JunctionId == junction) return {&group, &port};
				return {};
			};
			const auto syntheticRoute = [&](const Document &document, const Link &link) {
				const auto input = boundaryPort(document, link.FromNode);
				if (input.second && input.second->Direction == PortDirection::Input &&
					link.ToNode == input.second->ControlNodeId && link.ToPort == "parent_value")
					return true;
				const auto output = boundaryPort(document, link.ToNode);
				return output.second && output.second->Direction == PortDirection::Output &&
					   link.FromNode == output.second->ControlNodeId && link.FromPort == "value";
			};
			const auto endpoint = [&](const Document &document,
									  std::string_view id,
									  std::string_view port,
									  bool output) -> std::pair<Json *, std::optional<size_t>> {
				const auto boundary = boundaryPort(document, id);
				if (boundary.second) {
					if (port != "value" ||
						boundary.second->Direction != (output ? PortDirection::Output : PortDirection::Input))
						return {};
					size_t index = 0;
					for (const auto &candidate : boundary.first->Ports) {
						if (&candidate == boundary.second) break;
						if (candidate.Direction == boundary.second->Direction) ++index;
					}
					auto *owner = SourceNode(root, boundary.first->Id);
					if (owner && owner->value("type", "") == "Node_Pixel_Builder") index += output ? 2 : 4;
					return {owner, index};
				}
				Json *record = SourceNode(root, id);
				return {record, record ? indexOf(*record, port, output) : std::nullopt};
			};
			// Boundary junctions map to their parent's source socket; synthetic routes are reconstructed.

			for (const auto &link : working.Graph.Links) {
				if (std::find(desired.Links.begin(), desired.Links.end(), link) != desired.Links.end())
					continue;
				if (syntheticRoute(working.Graph, link)) continue;
				const auto [target, index] = endpoint(working.Graph, link.ToNode, link.ToPort, false);
				if (!target) continue;
				if (!index || *index >= (*target)["inputs"].size())
					return Reject(
						diagnostic, "PXC removed link has no inverse source input", link.ToNode, link.ToPort
					);
				auto &input = (*target)["inputs"][*index];
				input.erase("from_node");
				input.erase("from_index");
				input.erase("from_tag");
			}
			for (const auto &link : desired.Links) {
				if (std::find(working.Graph.Links.begin(), working.Graph.Links.end(), link) !=
					working.Graph.Links.end())
					continue;
				if (syntheticRoute(desired, link)) continue;
				const auto [target, input] = endpoint(desired, link.ToNode, link.ToPort, false);
				const auto [producer, output] = endpoint(desired, link.FromNode, link.FromPort, true);
				if (!target || !producer)
					return Reject(diagnostic, "PXC new link has no inverse source node");
				if (!input || !output || *input >= (*target)["inputs"].size() || *output > UINT32_MAX)
					return Reject(diagnostic, "PXC new link has no inverse source socket");
				auto &record = (*target)["inputs"][*input];
				record["from_node"] = (*producer)["id"];
				record["from_index"] = *output;
				record.erase("from_tag");
			}
		} catch (const Json::exception &) {
			return Reject(diagnostic, "PXC authored source projection is malformed");
		}
		size_t jsonBudget = bake::PxcxLimits::MaximumGraphJsonBytes - 1;
		if (!JsonBytes(root, jsonBudget))
			return Reject(diagnostic, "PXC authored source JSON exceeds its limit");
		bake::PxcxArchive candidate = working.Source;
		candidate.GraphJson = root.dump();
		candidate.GraphJson.push_back('\0');
		candidate.Nodes.clear();
		candidate.Links.clear();
		std::vector<std::byte> written;
		if (!bake::WritePxcx(candidate, written, failure)) return Reject(diagnostic, failure);
		bake::PxcxArchive checked;
		PxcxImport projected;
		if (!bake::ReadPxcx(written, checked, failure) ||
			!ImportPxcxImageGraph(checked, projected, failure, imported.Options))
			return Reject(diagnostic, failure);
		if (Migrate(projected.Graph, diagnostic) != Status::Ok) return false;
		// New global controls acquire the same compact animator bookkeeping as new source nodes.
		for (auto &node : desired.Nodes)
			if (node.Type == "pc.global_scope") {
				const auto *old = NativeNode(imported.Graph, node.Id);
				const auto *saved = NativeNode(projected.Graph, node.Id);
				if (!old || !saved) continue;
				for (const auto &input : node.DynamicInputs) {
					if (std::any_of(old->DynamicInputs.begin(), old->DynamicInputs.end(), [&](const auto &v) {
							return v.Id == input.Id;
						}))
						continue;
					if (std::find(node.SourceStaticInputs.begin(), node.SourceStaticInputs.end(), input.Id) ==
						node.SourceStaticInputs.end())
						node.SourceStaticInputs.push_back(input.Id);
					for (const auto &key : projected.Graph.Keyframes)
						if (key.NodeId == node.Id && key.Port == input.Id) {
							if (key.Kind != KeyframeKind::Normal || GetFrameTime(key) != FrameTime{} ||
								key.SourceDriver || !input.Default || key.Data != *input.Default)
								return Reject(
									diagnostic, "PXC new global animator changes execution", node.Id, input.Id
								);
							if (std::none_of(
									desired.Keyframes.begin(), desired.Keyframes.end(), [&](const auto &v) {
										return v.NodeId == node.Id && v.Port == input.Id;
									}
								))
								desired.Keyframes.push_back(key);
						}
					for (const auto &track : projected.Graph.Tracks)
						if (track.NodeId == node.Id && track.Port == input.Id &&
							std::none_of(desired.Tracks.begin(), desired.Tracks.end(), [&](const auto &v) {
								return v.NodeId == node.Id && v.Port == input.Id;
							}))
							desired.Tracks.push_back(track);
				}
			}
		// Appending a source socket creates the same compact static animator as a new source node.
		for (auto &node : desired.Nodes) {
			const auto *old = NativeNode(imported.Graph, node.Id);
			const auto *saved = NativeNode(projected.Graph, node.Id);
			if (!old || !saved || node.Type == "pc.global_scope") continue;
			for (const auto &input : node.DynamicInputs) {
				if (std::any_of(
						old->DynamicInputs.begin(),
						old->DynamicInputs.end(),
						[&](const auto &v) { return v.Id == input.Id; }
					) ||
					!input.Default)
					continue;
				for (const auto &key : projected.Graph.Keyframes) {
					if (key.NodeId != node.Id || key.Port != input.Id) continue;
					if (key.Kind != KeyframeKind::Normal || GetFrameTime(key) != FrameTime{} ||
						key.SourceDriver || key.Data != *input.Default)
						return Reject(
							diagnostic,
							"PXC appended socket animator changes authored execution",
							node.Id,
							input.Id
						);
					if (std::none_of(desired.Keyframes.begin(), desired.Keyframes.end(), [&](const auto &v) {
							return v.NodeId == node.Id && v.Port == input.Id;
						}))
						desired.Keyframes.push_back(key);
				}
				for (const auto &track : projected.Graph.Tracks)
					if (track.NodeId == node.Id && track.Port == input.Id &&
						std::none_of(desired.Tracks.begin(), desired.Tracks.end(), [&](const auto &v) {
							return v.NodeId == node.Id && v.Port == input.Id;
						}))
						desired.Tracks.push_back(track);
				if (std::find(saved->SourceStaticInputs.begin(), saved->SourceStaticInputs.end(), input.Id) !=
						saved->SourceStaticInputs.end() &&
					std::find(node.SourceStaticInputs.begin(), node.SourceStaticInputs.end(), input.Id) ==
						node.SourceStaticInputs.end())
					node.SourceStaticInputs.push_back(input.Id);
			}
		}
		// A new native catalogue node has no saved animator yet. A source compact animator adds
		// static bookkeeping, which does not change the authored value or its execution.
		for (auto &node : desired.Nodes) {
			if (NativeNode(imported.Graph, node.Id) || !node.SourceAnimatedInputs.empty()) continue;
			const auto *saved = NativeNode(projected.Graph, node.Id);
			if (!saved || !saved->SourceAnimatedInputs.empty()) continue;
			if (std::any_of(
					desired.Keyframes.begin(),
					desired.Keyframes.end(),
					[&](const auto &key) { return key.NodeId == node.Id; }
				) ||
				std::any_of(desired.Tracks.begin(), desired.Tracks.end(), [&](const auto &track) {
					return track.NodeId == node.Id;
				}))
				continue;
			if (node.SourceStaticInputs.empty()) node.SourceStaticInputs = saved->SourceStaticInputs;
			for (const auto &key : projected.Graph.Keyframes) {
				if (key.NodeId != node.Id) continue;
				if (key.Kind != KeyframeKind::Normal || GetFrameTime(key) != FrameTime{} ||
					key.SourceDriver ||
					(!std::any_of(
						 node.Values.begin(),
						 node.Values.end(),
						 [&](const auto &value) { return value.Port == key.Port && value.Data == key.Data; }
					 ) &&
					 !std::any_of(
						 node.DynamicInputs.begin(), node.DynamicInputs.end(), [&](const auto &input) {
							 return input.Id == key.Port && input.Default && *input.Default == key.Data;
						 }
					 )))
					return Reject(
						diagnostic,
						"PXC new node source bookkeeping changes authored execution",
						node.Id,
						key.Port
					);
				desired.Keyframes.push_back(key);
			}
			for (const auto &track : projected.Graph.Tracks)
				if (track.NodeId == node.Id) desired.Tracks.push_back(track);
		}
		detail::RebaseKeyProvenance(desired, projected.Graph);
		detail::CanonicalizeKeyOrder(desired);
		detail::CanonicalizeKeyOrder(projected.Graph);
		if (projected.Graph != desired) {
			std::string field = "document fields";
			std::string nodeId;
			if (projected.Graph.Nodes.size() != desired.Nodes.size())
				field = "node count";
			else
				for (size_t index = 0; index < desired.Nodes.size(); ++index) {
					const auto &wanted = desired.Nodes[index];
					const auto &actual = projected.Graph.Nodes[index];
					if (actual == wanted) continue;
					nodeId = wanted.Id;
					if (actual.Values != wanted.Values)
						field = "node values";
					else if (actual.DynamicInputs != wanted.DynamicInputs)
						field = "dynamic inputs";
					else if (actual.DynamicOutputs != wanted.DynamicOutputs)
						field = "dynamic outputs";
					else if (actual.SourceStaticInputs != wanted.SourceStaticInputs)
						field = "static source controls";
					else if (actual.SourceAnimatedInputs != wanted.SourceAnimatedInputs)
						field = "animated source controls";
					else if (actual.SourceProperties != wanted.SourceProperties)
						field = "source properties";
					else
						field = "node metadata";
					break;
				}
			if (nodeId.empty()) {
				if (projected.Graph.Keyframes != desired.Keyframes)
					field = "keyframes";
				else if (projected.Graph.Tracks != desired.Tracks)
					field = "tracks";
				else if (projected.Graph.Links != desired.Links)
					field = "links";
				else if (projected.Graph.Project != desired.Project)
					field = "project settings";
			}
			return Reject(diagnostic, "PXC source inverse would change " + field, nodeId);
		}
		Document unchanged = imported.Graph;
		if (Migrate(unchanged, diagnostic) != Status::Ok) return false;
		detail::RebaseKeyProvenance(unchanged, projected.Graph);
		detail::CanonicalizeKeyOrder(unchanged);
		out = desired == unchanged ? imported.Source.OriginalBytes : std::move(written);
		return true;
	}

}
