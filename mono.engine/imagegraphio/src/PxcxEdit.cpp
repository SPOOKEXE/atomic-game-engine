#include <engine/imagegraph/Catalogue.hpp>
#include <engine/imagegraph/SourceAnimatorCapture.hpp>
#include <engine/imagegraphio/PxcxEdit.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <nlohmann/json.hpp>
#include <set>
#include <type_traits>
#include <unordered_set>

namespace engine::imagegraphio {
	namespace edit_detail {
		using namespace imagegraph;
		using Json = nlohmann::ordered_json;

		std::optional<Json> RetainedJson(std::string_view text, bool &ambiguous) {
			std::vector<std::unordered_set<std::string>> objects;
			try {
				return Json::parse(
					text.begin(), text.end(), [&](int depth, Json::parse_event_t event, Json &item) {
						if (depth > int(bake::PxcxLimits::MaximumJsonDepth)) ambiguous = true;
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
				if (!value.Nested.empty() || value.Elements.size() > Limits::MaximumArrayElements ||
					value.ElementType > ValueType::Vector2)
					return false;
				for (const auto &leaf : value.Elements)
					if (!std::visit([&](const auto &item) { return Bounded(item, budget); }, leaf))
						return false;
				return true;
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
				if (!original.is_array()) return std::nullopt;
				Json result = Json::array();
				for (size_t index = 0; index < value.Elements.size(); index++) {
					const Json empty;
					const Json &old = index < original.size() ? original[index] : empty;
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
					else
						return "curve";
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
					} else
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

		Json *SourceNode(Json &root, std::string_view id) {
			for (auto &node : root["nodes"])
				if (node.value("id", std::string{}) == id) return &node;
			return nullptr;
		}
		Node *NativeNode(Document &document, std::string_view id) {
			for (auto &node : document.Nodes)
				if (node.Id == id) return &node;
			return nullptr;
		}
		Json *FixedInput(Json &source, const CatalogueEntry &entry, std::string_view port) {
			const auto *input = FindCatalogueInput(entry, port);
			if (!input || input->SourceIndex < 0 || !source.contains("inputs") ||
				!source["inputs"].is_array() || size_t(input->SourceIndex) >= source["inputs"].size())
				return nullptr;
			Json &record = source["inputs"][input->SourceIndex];
			return record.is_object() ? &record : nullptr;
		}
		std::optional<uint8_t> Side(std::string_view name) {
			if (name == "linear") return 0;
			if (name == "bezier") return 1;
			if (name == "cut") return 2;
			return std::nullopt;
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
			const auto *schema = FindCatalogueInput(entry, operation.Port);
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
						const auto *entry = FindCatalogueEntry(node->Type);
						if (!entry)
							return Reject(
								diagnostic,
								"PXC edit has no reversible catalogue mapping",
								operation.NodeId,
								operation.Port
							);
						Json *input = FixedInput(*source, *entry, operation.Port);
						if (!input || !input->contains("r") || input->contains("from_node") ||
							input->contains("global_key") || input->contains("global_use"))
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
							if (value == node->Values.end())
								return Reject(
									diagnostic,
									"PXC fixed value was not projected",
									operation.NodeId,
									operation.Port
								);
							if (value->Data == operation.Data) return true;
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
							value->Data = Value(operation.Data);
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
		if (intended != projected.Graph)
			return Reject(diagnostic, "PXC edited projection changed unsupported or ambiguous semantics");
		out = std::move(bytes);
		return true;
	}
}
