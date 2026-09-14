#include <engine/core/Name.hpp>
#include <engine/ecs/Attributes.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/physics/Clock.hpp>
#include <engine/physics/Contacts.hpp>
#include <engine/physics/PhysicsWorld.hpp>
#include <engine/physics/Query.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Constraints.hpp>
#include <engine/scene/Controls.hpp>
#include <engine/scene/EditableImage.hpp>
#include <engine/scene/Enums.hpp>
#include <engine/scene/Input.hpp>
#include <engine/scene/LocalLight.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/Skinning.hpp>
#include <engine/scene/Sunlight.hpp>
#include <engine/scene/SurfaceTable.hpp>
#include <engine/script/DataCaptureBridge.hpp>
#include <engine/script/DataCaptureDriver.hpp>
#include <engine/script/DataLifecycleBridge.hpp>
#include <engine/script/DataSceneService.hpp>
#include <engine/script/EventNarratives.hpp>
#include <engine/script/ScriptCall.hpp>
#include <engine/script/ServiceSurface.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <memory>
#include <numbers>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace engine::script {
	namespace {
		ScriptValue String(std::string_view text) {
			ScriptValue value(ValueTag::String);
			value.Text = text;
			return value;
		}

		ScriptValue Number(double number) {
			ScriptValue value(ValueTag::Number);
			value.Number = number;
			return value;
		}
		ScriptValue CFrame(const core::CFrame &frame) {
			ScriptValue value(ValueTag::CFrame);
			value.Frame = frame;
			return value;
		}

		ScriptValue Boolean(bool boolean) {
			ScriptValue value(boolean ? ValueTag::True : ValueTag::False);
			value.Boolean = boolean;
			return value;
		}

		ScriptValue Frame(const core::CFrame &frame) {
			ScriptValue value(ValueTag::CFrame);
			value.Frame = frame;
			return value;
		}

		ScriptValue Vector(const core::Vector3 &vector) {
			ScriptValue value(ValueTag::Vector3);
			value.Vector = vector;
			return value;
		}

		bool Finite(const core::Vector3 &value) {
			return std::isfinite(value.X) && std::isfinite(value.Y) && std::isfinite(value.Z);
		}
		bool DataSceneUtf8(std::string_view value) {
			for (size_t index = 0; index < value.size();) {
				const uint8_t first = static_cast<uint8_t>(value[index++]);
				if (first < 0x80) continue;
				const unsigned extra = first >= 0xC2 && first <= 0xDF	? 1
									   : first >= 0xE0 && first <= 0xEF ? 2
									   : first >= 0xF0 && first <= 0xF4 ? 3
																		: 4;
				if (extra == 4 || index + extra > value.size()) return false;
				uint32_t codepoint = first & ((1u << (7 - extra)) - 1u);
				for (unsigned part = 0; part < extra; ++part) {
					const uint8_t next = static_cast<uint8_t>(value[index++]);
					if ((next & 0xC0u) != 0x80u) return false;
					codepoint = (codepoint << 6u) | (next & 0x3Fu);
				}
				if ((extra == 1 && codepoint < 0x80) || (extra == 2 && codepoint < 0x800) ||
					(extra == 3 && codepoint < 0x10000) || codepoint > 0x10FFFF ||
					(codepoint >= 0xD800 && codepoint <= 0xDFFF))
					return false;
			}
			return true;
		}
		bool Finite(const core::CFrame &value) {
			const double length = std::hypot(
				std::hypot(static_cast<double>(value.QuaternionX), value.QuaternionY),
				std::hypot(static_cast<double>(value.QuaternionZ), value.QuaternionW)
			);
			return Finite(value.Position) && std::isfinite(length) && std::abs(length - 1.0) <= 0.001;
		}

		struct ProjectedBounds {
			bool Available = false;
			bool Intersects = false;
			bool Clipped = false;
			const char *Reason = "outside_camera_frustum";
			double Left = 0.0;
			double Top = 0.0;
			double Right = 0.0;
			double Bottom = 0.0;
		};

		ProjectedBounds ProjectBounds(
			const core::CFrame &cameraFromWorld,
			const scene::Camera &camera,
			const scene::Transform &object,
			const scene::Bounds &bounds
		) {
			ProjectedBounds result;
			if (camera.ImageWidth == 0 || camera.ImageHeight == 0) {
				result.Reason = "camera_has_no_explicit_image_size";
				return result;
			}
			const double tangent = std::tan(static_cast<double>(camera.FieldOfViewRadians) * 0.5);
			const double aspect = static_cast<double>(camera.ImageWidth) / camera.ImageHeight;
			if (!Finite(object.Frame) || !std::isfinite(tangent) || tangent <= 0.0f ||
				!Finite(bounds.HalfExtent) || bounds.HalfExtent.X < 0.0f || bounds.HalfExtent.Y < 0.0f ||
				bounds.HalfExtent.Z < 0.0f) {
				result.Reason = "invalid_authored_spatial_data";
				return result;
			}
			std::array<core::Vector3, 8> points;
			for (size_t index = 0; index < points.size(); ++index) {
				const core::Vector3 local{
					(index & 1) == 0 ? -bounds.HalfExtent.X : bounds.HalfExtent.X,
					(index & 2) == 0 ? -bounds.HalfExtent.Y : bounds.HalfExtent.Y,
					(index & 4) == 0 ? -bounds.HalfExtent.Z : bounds.HalfExtent.Z,
				};
				points[index] = cameraFromWorld.PointToWorldSpace(object.Frame.PointToWorldSpace(local));
				if (!Finite(points[index])) {
					result.Reason = "invalid_authored_spatial_data";
					return result;
				}
			}
			std::vector<core::Vector3> clipped;
			const auto accept = [&](core::Vector3 point) {
				const float depth = -point.Z;
				if (depth < camera.NearPlane || depth > camera.FarPlane) return;
				clipped.push_back(point);
			};
			for (const auto point : points)
				accept(point);
			constexpr std::array<std::array<size_t, 2>, 12> EDGES{{
				{{0, 1}},
				{{0, 2}},
				{{0, 4}},
				{{1, 3}},
				{{1, 5}},
				{{2, 3}},
				{{2, 6}},
				{{3, 7}},
				{{4, 5}},
				{{4, 6}},
				{{5, 7}},
				{{6, 7}},
			}};
			for (const auto edge : EDGES) {
				core::Vector3 first = points[edge[0]];
				core::Vector3 second = points[edge[1]];
				float firstDepth = -first.Z;
				float secondDepth = -second.Z;
				const auto clip = [&](float plane, bool keepGreater) {
					const bool firstInside = keepGreater ? firstDepth >= plane : firstDepth <= plane;
					const bool secondInside = keepGreater ? secondDepth >= plane : secondDepth <= plane;
					if (firstInside == secondInside) return firstInside;
					const double denominator = static_cast<double>(secondDepth) - firstDepth;
					const double fraction = (static_cast<double>(plane) - firstDepth) / denominator;
					const auto lerp = [&](float left, float right) -> std::optional<float> {
						const double value =
							static_cast<double>(left) + (static_cast<double>(right) - left) * fraction;
						if (!std::isfinite(value) || value < -std::numeric_limits<float>::max() ||
							value > std::numeric_limits<float>::max())
							return std::nullopt;
						return static_cast<float>(value);
					};
					const auto x = lerp(first.X, second.X), y = lerp(first.Y, second.Y),
							   z = lerp(first.Z, second.Z);
					if (!std::isfinite(denominator) || denominator == 0.0 || !std::isfinite(fraction) || !x ||
						!y || !z)
						return false;
					const core::Vector3 point{*x, *y, *z};
					if (!firstInside)
						first = point;
					else
						second = point;
					firstDepth = -first.Z;
					secondDepth = -second.Z;
					return true;
				};
				if (!clip(camera.NearPlane, true) || !clip(camera.FarPlane, false)) continue;
				clipped.push_back(first);
				clipped.push_back(second);
			}
			if (clipped.empty()) {
				result.Reason = "outside_camera_depth_range";
				return result;
			}
			double left = std::numeric_limits<double>::infinity();
			double top = std::numeric_limits<double>::infinity();
			double right = -std::numeric_limits<double>::infinity();
			double bottom = -std::numeric_limits<double>::infinity();
			std::vector<std::pair<double, double>> pixels;
			for (const core::Vector3 point : clipped) {
				const double depth = -static_cast<double>(point.Z);
				if (!std::isfinite(depth) || depth <= 0.0) {
					result.Reason = "invalid_authored_spatial_data";
					return result;
				}
				const double x = static_cast<double>(point.X) / (depth * tangent * aspect);
				const double y = static_cast<double>(point.Y) / (depth * tangent);
				if (!std::isfinite(x) || !std::isfinite(y)) {
					result.Reason = "invalid_authored_spatial_data";
					return result;
				}
				const double pixelX = (x + 1.0) * 0.5 * camera.ImageWidth;
				const double pixelY = (1.0 - y) * 0.5 * camera.ImageHeight;
				if (!std::isfinite(pixelX) || !std::isfinite(pixelY)) {
					result.Reason = "invalid_authored_spatial_data";
					return result;
				}
				left = std::min(left, pixelX);
				right = std::max(right, pixelX);
				top = std::min(top, pixelY);
				bottom = std::max(bottom, pixelY);
				pixels.emplace_back(pixelX, pixelY);
			}
			std::sort(pixels.begin(), pixels.end());
			const auto cross = [](const auto &a, const auto &b, const auto &c) {
				return (b.first - a.first) * (c.second - a.second) -
					   (b.second - a.second) * (c.first - a.first);
			};
			std::vector<std::pair<double, double>> hull;
			for (const auto &point : pixels) {
				while (hull.size() > 1 && cross(hull[hull.size() - 2], hull.back(), point) <= 0.0)
					hull.pop_back();
				hull.push_back(point);
			}
			const size_t lower = hull.size();
			for (size_t index = pixels.size(); index-- > 0;) {
				const auto point = pixels[index];
				while (hull.size() > lower && cross(hull[hull.size() - 2], hull.back(), point) <= 0.0)
					hull.pop_back();
				hull.push_back(point);
			}
			if (hull.size() > 1) hull.pop_back();
			const auto inside = [&](const auto point) {
				bool positive = false, negative = false;
				for (size_t index = 0; index < hull.size(); ++index) {
					const double value = cross(hull[index], hull[(index + 1) % hull.size()], point);
					positive = positive || value > 0.0;
					negative = negative || value < 0.0;
				}
				return !(positive && negative);
			};
			const auto orientation = [](const auto &a, const auto &b, const auto &c) {
				return (b.first - a.first) * (c.second - a.second) -
					   (b.second - a.second) * (c.first - a.first);
			};
			const auto segments = [&](const auto &a, const auto &b, const auto &c, const auto &d) {
				const double abC = orientation(a, b, c), abD = orientation(a, b, d);
				const double cdA = orientation(c, d, a), cdB = orientation(c, d, b);
				return ((abC > 0.0) != (abD > 0.0)) && ((cdA > 0.0) != (cdB > 0.0));
			};
			const std::array<std::pair<double, double>, 4> image{{
				{0.0, 0.0},
				{static_cast<double>(camera.ImageWidth), 0.0},
				{static_cast<double>(camera.ImageWidth), static_cast<double>(camera.ImageHeight)},
				{0.0, static_cast<double>(camera.ImageHeight)},
			}};
			result.Intersects = std::any_of(
									hull.begin(),
									hull.end(),
									[&](const auto point) {
										return point.first > 0.0 && point.first < camera.ImageWidth &&
											   point.second > 0.0 && point.second < camera.ImageHeight;
									}
								) ||
								std::any_of(image.begin(), image.end(), inside);
			for (size_t edge = 0; !result.Intersects && edge < hull.size(); ++edge)
				for (size_t imageEdge = 0; imageEdge < image.size(); ++imageEdge)
					if (segments(
							hull[edge],
							hull[(edge + 1) % hull.size()],
							image[imageEdge],
							image[(imageEdge + 1) % image.size()]
						))
						result.Intersects = true;
			if (!result.Intersects) return result;
			result.Clipped =
				left < 0.0 || top < 0.0 || right > camera.ImageWidth || bottom > camera.ImageHeight;
			result.Left = std::clamp(left, 0.0, static_cast<double>(camera.ImageWidth));
			result.Top = std::clamp(top, 0.0, static_cast<double>(camera.ImageHeight));
			result.Right = std::clamp(right, 0.0, static_cast<double>(camera.ImageWidth));
			result.Bottom = std::clamp(bottom, 0.0, static_cast<double>(camera.ImageHeight));
			result.Available = result.Right > result.Left && result.Bottom > result.Top;
			if (!result.Available) {
				result.Intersects = false;
				result.Reason = "outside_camera_frustum";
			} else {
				result.Reason = "";
			}
			return result;
		}

		ScriptValue Colour(const core::Color3 &colour) {
			ScriptValue value(ValueTag::Color3);
			value.Colour = colour;
			return value;
		}

		const char *LightKindName(scene::LightKind kind) {
			switch (kind) {
			case scene::LightKind::Point:
				return "point";
			case scene::LightKind::Spot:
				return "spot";
			case scene::LightKind::Surface:
				return "surface";
			}
			return "unknown";
		}

		const char *CameraModeName(scene::CameraMode mode) {
			switch (mode) {
			case scene::CameraMode::Classic:
				return "classic";
			case scene::CameraMode::LockFirstPerson:
				return "lock_first_person";
			case scene::CameraMode::ShiftLock:
				return "shift_lock";
			case scene::CameraMode::Scriptable:
				return "scriptable";
			}
			return "unknown";
		}

		const char *ConstraintMotionName(scene::ConstraintMotion motion) {
			switch (motion) {
			case scene::ConstraintMotion::Locked:
				return "locked";
			case scene::ConstraintMotion::Limited:
				return "limited";
			case scene::ConstraintMotion::Free:
				return "free";
			}
			return "unknown";
		}

		ScriptValue Map(std::vector<std::pair<std::string, ScriptValue>> entries) {
			ScriptValue value(ValueTag::Map);
			value.Entries = std::move(entries);
			return value;
		}

		ScriptValue Array(std::vector<ScriptValue> items) {
			ScriptValue value(ValueTag::Array);
			value.Items = std::move(items);
			return value;
		}

		bool StableId(const ecs::Store &store, ecs::Entity entity, std::string &out);

		std::string Decimal(uint64_t value) {
			std::array<char, 32> text{};
			const auto converted = std::to_chars(text.data(), text.data() + text.size(), value);
			return std::string(text.data(), converted.ptr);
		}

		const ScriptValue *Field(const ScriptValue &value, std::string_view name) {
			if (value.Tag != ValueTag::Map) return nullptr;
			for (const auto &[key, item] : value.Entries)
				if (key == name) return &item;
			return nullptr;
		}

		bool
		BoundedStringField(const ScriptValue &value, std::string_view name, size_t limit, std::string &out) {
			const ScriptValue *field = Field(value, name);
			if (field == nullptr || field->Tag != ValueTag::String || field->Text.empty() ||
				field->Text.size() > limit || field->Text.find('\0') != std::string::npos)
				return false;
			out = field->Text;
			return true;
		}

		bool HasOnlyFields(const ScriptValue &value, std::initializer_list<std::string_view> allowed) {
			if (value.Tag != ValueTag::Map) return false;
			for (size_t index = 0; index < value.Entries.size(); index++) {
				const std::string &key = value.Entries[index].first;
				if (key.find('\0') != std::string::npos ||
					std::find(allowed.begin(), allowed.end(), key) == allowed.end())
					return false;
				for (size_t previous = 0; previous < index; previous++)
					if (value.Entries[previous].first == key) return false;
			}
			return true;
		}

		bool Ticket(std::string_view text, uint64_t &out) {
			if (text.empty() || text.size() > 20 || text.find('\0') != std::string_view::npos) return false;
			const auto converted = std::from_chars(text.data(), text.data() + text.size(), out);
			return converted.ec == std::errc{} && converted.ptr == text.data() + text.size();
		}

		constexpr size_t MAX_EVENT_NARRATIVE_STRING_BYTES = 4'096;
		constexpr size_t MAX_EVENT_NARRATIVE_IDS = 64;
		constexpr size_t MAX_EVENT_NARRATIVE_TRANSPORT_BYTES = 64u * 1024u;

		bool JsonStringBytes(std::string_view value, size_t &bytes) {
			if (bytes > MAX_EVENT_NARRATIVE_TRANSPORT_BYTES - 2) return false;
			bytes += 2;
			for (const unsigned char character : value) {
				const size_t added = character < 0x20 ? 6 : (character == '"' || character == '\\' ? 2 : 1);
				if (bytes > MAX_EVENT_NARRATIVE_TRANSPORT_BYTES - added) return false;
				bytes += added;
			}
			return true;
		}

		bool JsonBytes(const ScriptValue &value, size_t &bytes) {
			auto add = [&bytes](size_t amount) {
				if (bytes > MAX_EVENT_NARRATIVE_TRANSPORT_BYTES - amount) return false;
				bytes += amount;
				return true;
			};
			switch (value.Tag) {
			case ValueTag::Nil:
				return add(4);
			case ValueTag::False:
			case ValueTag::True:
				return add(5);
			case ValueTag::Number:
				return add(32);
			case ValueTag::String:
				return JsonStringBytes(value.Text, bytes);
			case ValueTag::Array:
				if (!add(2)) return false;
				for (size_t index = 0; index < value.Items.size(); index++)
					if ((index != 0 && !add(1)) || !JsonBytes(value.Items[index], bytes)) return false;
				return true;
			case ValueTag::Map:
				if (!add(2)) return false;
				for (size_t index = 0; index < value.Entries.size(); index++)
					if ((index != 0 && !add(1)) || !JsonStringBytes(value.Entries[index].first, bytes) ||
						!add(1) || !JsonBytes(value.Entries[index].second, bytes))
						return false;
				return true;
			default:
				return false;
			}
		}

		bool EventNarrativeTime(const ScriptValue &record, std::string_view name, uint64_t &out) {
			const ScriptValue *field = Field(record, name);
			if (field == nullptr || field->Tag != ValueTag::String || field->Text.empty() ||
				field->Text.size() > 20 || field->Text.find('\0') != std::string::npos)
				return false;
			uint64_t value = 0;
			for (const char character : field->Text) {
				if (character < '0' || character > '9') return false;
				const uint64_t digit = static_cast<uint64_t>(character - '0');
				if (value > (std::numeric_limits<uint64_t>::max() - digit) / 10) return false;
				value = value * 10 + digit;
			}
			out = value;
			return true;
		}

		bool EventNarrativeState(std::string_view value) {
			return value == "observed" || value == "known" || value == "hidden" ||
				   value == "simulator_hidden" || value == "inferred" || value == "generated" ||
				   value == "user_specified" || value == "script_declared" || value == "unknown" ||
				   value == "unavailable";
		}

		bool EventNarrativeKind(std::string_view value) {
			return value == "observation" || value == "flashback" || value == "prediction";
		}

		bool
		EventNarrativeIds(const ScriptValue &record, std::string_view name, std::vector<std::string> &out) {
			const ScriptValue *field = Field(record, name);
			if (field == nullptr ||
				(field->Tag != ValueTag::Array && !(field->Tag == ValueTag::Map && field->Entries.empty())) ||
				field->Items.size() > MAX_EVENT_NARRATIVE_IDS)
				return false;
			std::unordered_set<std::string> unique;
			out.clear();
			out.reserve(field->Items.size());
			for (const ScriptValue &item : field->Items) {
				if (item.Tag != ValueTag::String || item.Text.empty() ||
					item.Text.size() > MAX_DATA_SCENE_ID_BYTES || item.Text.find('\0') != std::string::npos ||
					!unique.insert(item.Text).second)
					return false;
				out.push_back(item.Text);
			}
			std::sort(out.begin(), out.end());
			return true;
		}

		bool EventNarrativeConfidence(
			const ScriptValue &record, std::string_view name, double &out, bool &present
		) {
			const ScriptValue *field = Field(record, name);
			present = field != nullptr && field->Tag != ValueTag::Nil;
			if (!present) return true;
			if (field->Tag != ValueTag::Number || !std::isfinite(field->Number) || field->Number < 0.0 ||
				field->Number > 1.0)
				return false;
			out = field->Number;
			return true;
		}

		DataSceneResult InvalidEventNarratives(std::string_view reason) {
			return {
				"invalid_argument",
				Map({{"status", String("invalid_event_narratives")}, {"reason", String(reason)}})
			};
		}

		DataSceneResult ValidateEventNarratives(const ScriptValue &bundle, ScriptValue &canonical) {
			if (!HasOnlyFields(bundle, {"version", "records"}))
				return InvalidEventNarratives("invalid_bundle_fields");
			const ScriptValue *version = Field(bundle, "version");
			const ScriptValue *records = Field(bundle, "records");
			if (version == nullptr || version->Tag != ValueTag::Number ||
				version->Number != EVENT_NARRATIVE_SCHEMA_VERSION || records == nullptr ||
				(records->Tag != ValueTag::Array &&
				 !(records->Tag == ValueTag::Map && records->Entries.empty())) ||
				records->Items.size() > MAX_EVENT_NARRATIVES)
				return InvalidEventNarratives("invalid_bundle");

			std::vector<ScriptValue> output;
			std::vector<std::vector<std::byte>> unique;
			output.reserve(records->Items.size());
			unique.reserve(records->Items.size());
			for (const ScriptValue &record : records->Items) {
				if (!HasOnlyFields(
						record,
						{"event_time_ns",
						 "narration_time_ns",
						 "subject_id",
						 "speaker_id",
						 "text",
						 "knowledge_state",
						 "belief",
						 "certainty",
						 "evidence_ids",
						 "provenance_ids",
						 "temporal_reference",
						 "unavailable_reason"}
					))
					return InvalidEventNarratives("invalid_record_fields");
				uint64_t eventTime = 0;
				uint64_t narrationTime = 0;
				std::string subject;
				std::string speaker;
				std::string text;
				std::string knowledge;
				std::string temporal;
				std::vector<std::string> evidence;
				std::vector<std::string> provenance;
				double belief = 0.0;
				double certainty = 0.0;
				bool hasBelief = false;
				bool hasCertainty = false;
				if (!EventNarrativeTime(record, "event_time_ns", eventTime) ||
					!EventNarrativeTime(record, "narration_time_ns", narrationTime))
					return InvalidEventNarratives("invalid_time");
				if (!BoundedStringField(record, "subject_id", MAX_DATA_SCENE_ID_BYTES, subject) ||
					!BoundedStringField(record, "speaker_id", MAX_DATA_SCENE_ID_BYTES, speaker) ||
					!BoundedStringField(record, "text", MAX_EVENT_NARRATIVE_STRING_BYTES, text) ||
					!BoundedStringField(record, "knowledge_state", 32, knowledge) ||
					!BoundedStringField(record, "temporal_reference", 16, temporal) ||
					!EventNarrativeState(knowledge) || !EventNarrativeKind(temporal))
					return InvalidEventNarratives("invalid_text_or_kind");
				if (!EventNarrativeIds(record, "evidence_ids", evidence) ||
					!EventNarrativeIds(record, "provenance_ids", provenance) || provenance.empty())
					return InvalidEventNarratives("invalid_ids");
				if (!EventNarrativeConfidence(record, "belief", belief, hasBelief) ||
					!EventNarrativeConfidence(record, "certainty", certainty, hasCertainty))
					return InvalidEventNarratives("invalid_confidence");
				const bool confident =
					knowledge == "observed" || knowledge == "known" || knowledge == "inferred";
				const bool evidenced = confident || knowledge == "generated" ||
									   knowledge == "user_specified" || knowledge == "script_declared";
				const bool hidden = knowledge == "hidden" || knowledge == "simulator_hidden" ||
									knowledge == "unknown" || knowledge == "unavailable";
				if ((confident && (!hasBelief || !hasCertainty)) ||
					(!confident && (hasBelief || hasCertainty)) || (evidenced && evidence.empty()) ||
					(hidden && !evidence.empty()) ||
					((temporal == "prediction") ? eventTime < narrationTime : eventTime > narrationTime))
					return InvalidEventNarratives("inconsistent_record");
				const ScriptValue *reason = Field(record, "unavailable_reason");
				std::string unavailable;
				const bool hasReason = reason != nullptr && reason->Tag != ValueTag::Nil;
				if (hasReason && (reason->Tag != ValueTag::String || reason->Text.empty() ||
								  reason->Text.size() > MAX_EVENT_NARRATIVE_STRING_BYTES ||
								  reason->Text.find('\0') != std::string::npos))
					return InvalidEventNarratives("invalid_unavailable_reason");
				if ((knowledge == "unavailable") != hasReason)
					return InvalidEventNarratives("unavailable_reason_mismatch");
				if (hasReason) unavailable = reason->Text;

				ScriptValue item = Map({
					{"event_time_ns", String(Decimal(eventTime))},
					{"narration_time_ns", String(Decimal(narrationTime))},
					{"subject_id", String(subject)},
					{"speaker_id", String(speaker)},
					{"text", String(text)},
					{"knowledge_state", String(knowledge)},
					{"belief", hasBelief ? Number(belief) : ScriptValue(ValueTag::Nil)},
					{"certainty", hasCertainty ? Number(certainty) : ScriptValue(ValueTag::Nil)},
				});
				std::vector<ScriptValue> evidenceValues;
				std::vector<ScriptValue> provenanceValues;
				for (const std::string &id : evidence)
					evidenceValues.push_back(String(id));
				for (const std::string &id : provenance)
					provenanceValues.push_back(String(id));
				item.Entries.push_back({"evidence_ids", Array(std::move(evidenceValues))});
				item.Entries.push_back({"provenance_ids", Array(std::move(provenanceValues))});
				item.Entries.push_back({"temporal_reference", String(temporal)});
				item.Entries.push_back(
					{"unavailable_reason", hasReason ? String(unavailable) : ScriptValue(ValueTag::Nil)}
				);
				std::vector<std::byte> encoded;
				ScriptValue encodedItem = item;
				if (Encode(encodedItem, encoded) != CodecStatus::Ok ||
					std::find(unique.begin(), unique.end(), encoded) != unique.end())
					return InvalidEventNarratives("duplicate_or_oversized_record");
				unique.push_back(std::move(encoded));
				output.push_back(std::move(item));
			}
			canonical = Map(
				{{"version", Number(EVENT_NARRATIVE_SCHEMA_VERSION)}, {"records", Array(std::move(output))}}
			);
			std::vector<std::byte> encoded;
			ScriptValue checked = canonical;
			if (Encode(checked, encoded) != CodecStatus::Ok)
				return InvalidEventNarratives("payload_too_large");
			ScriptValue response = canonical;
			response.Entries.push_back({"status", String("ok")});
			response.Entries.push_back({"schema_version", String("event-narrative/v1")});
			size_t transportBytes = 0;
			if (!JsonBytes(response, transportBytes))
				return InvalidEventNarratives("transport_payload_too_large");
			return {
				"ok",
				Map(
					{{"status", String("ok")},
					 {"schema_version", String("event-narrative/v1")},
					 {"record_count", Number(records->Items.size())}}
				)
			};
		}

		bool VectorField(const ScriptValue &value, std::string_view name, core::Vector3 &out) {
			const ScriptValue *field = Field(value, name);
			if (field == nullptr || field->Tag != ValueTag::Vector3 || !std::isfinite(field->Vector.X) ||
				!std::isfinite(field->Vector.Y) || !std::isfinite(field->Vector.Z))
				return false;
			out = field->Vector;
			return true;
		}

		ScriptValue QueryIds(
			const ecs::Store &store,
			std::span<const ecs::Entity> entities,
			size_t count,
			bool overflowed,
			std::string_view provenance = "physics_exact_collider_query"
		) {
			std::vector<ScriptValue> records;
			records.reserve(count);
			for (size_t index = 0; index < count; index++) {
				std::string id;
				if (StableId(store, entities[index], id)) records.push_back(String(id));
			}
			return Map({
				{"status", String("ok")},
				{"provenance", String(provenance)},
				{"ids", Array(std::move(records))},
				{"overflowed", Boolean(overflowed)},
			});
		}

		DataSceneResult QueryAabb(const ecs::Store &store, const ScriptValue &value) {
			core::Vector3 minimum;
			core::Vector3 maximum;
			if (!HasOnlyFields(value, {"minimum", "maximum"}) || !VectorField(value, "minimum", minimum) ||
				!VectorField(value, "maximum", maximum) || minimum.X >= maximum.X || minimum.Y >= maximum.Y ||
				minimum.Z >= maximum.Z)
				return {"invalid_argument", Map({{"status", String("invalid_aabb_query")}})};
			std::array<ecs::Entity, 64> found;
			const spatial::QueryResult result =
				physics::OverlapBox(store, core::AABB{minimum, maximum}, spatial::LayerMask::All(), found);
			return {"ok", QueryIds(store, found, result.Written, result.Overflowed)};
		}

		DataSceneResult QueryObb(const ecs::Store &store, const ScriptValue &value) {
			const ScriptValue *frame = Field(value, "frame");
			core::Vector3 halfExtent;
			if (!HasOnlyFields(value, {"frame", "half_extent"}) || frame == nullptr ||
				frame->Tag != ValueTag::CFrame || !VectorField(value, "half_extent", halfExtent) ||
				!(halfExtent.X > 0.0f) || !(halfExtent.Y > 0.0f) || !(halfExtent.Z > 0.0f))
				return {"invalid_argument", Map({{"status", String("invalid_obb_query")}})};
			std::array<ecs::Entity, 64> found;
			const spatial::QueryResult result = physics::OverlapOrientedBox(
				store, frame->Frame, halfExtent, spatial::LayerMask::All(), found
			);
			return {
				"ok",
				QueryIds(store, found, result.Written, result.Overflowed, "physics_exact_oriented_box_query")
			};
		}

		DataSceneResult QueryRay(const ecs::Store &store, const ScriptValue &value) {
			core::Vector3 origin;
			core::Vector3 direction;
			const ScriptValue *distance = Field(value, "max_distance_metres");
			if (!HasOnlyFields(value, {"origin", "direction", "max_distance_metres"}) ||
				!VectorField(value, "origin", origin) || !VectorField(value, "direction", direction) ||
				distance == nullptr || distance->Tag != ValueTag::Number ||
				!std::isfinite(distance->Number) || distance->Number <= 0.0 || distance->Number > 100'000.0)
				return {"invalid_argument", Map({{"status", String("invalid_raycast_query")}})};
			const auto hit =
				physics::Raycast(store, core::Ray{origin, direction}, static_cast<float>(distance->Number));
			if (!hit) return {"ok", Map({{"status", String("ok")}, {"hit", Boolean(false)}})};
			std::string id;
			const bool hasId = StableId(store, hit->Owner, id);
			return {
				"ok",
				Map({
					{"status", String("ok")},
					{"provenance", String("physics_exact_collider_raycast")},
					{"hit", Boolean(true)},
					{"id", String(hasId ? id : "")},
					{"has_stable_id", Boolean(hasId)},
					{"distance_metres", Number(hit->Distance)},
					{"position_metres", Vector(hit->Position)},
					{"normal", Vector(hit->Normal)},
				})
			};
		}

		DataSceneResult CaptureUnavailable() {
			return {
				"unsupported",
				Map({
					{"status", String("capability_unsupported")},
					{"feature", String("render_capture")},
					{"channels", Array({})},
					{"reason", String("no runtime-scoped render capture bridge installed")},
				})
			};
		}

		DataSceneResult CaptureChannels(const std::shared_ptr<DataCaptureBridge> &bridge) {
			if (bridge == nullptr) return CaptureUnavailable();
			const DataCaptureBridgeCapabilities capabilities = bridge->Capabilities();
			std::vector<ScriptValue> channels;
			channels.reserve(capabilities.Channels.size());
			for (const std::string &channel : capabilities.Channels)
				channels.push_back(String(channel));
			return {
				capabilities.Available ? "ok" : "unsupported",
				Map({
					{"status", String(capabilities.Available ? "ok" : "capability_unsupported")},
					{"channels", Array(std::move(channels))},
					{"reason", String(capabilities.Detail)},
				})
			};
		}

		DataSceneResult QueueCapture(
			const std::shared_ptr<DataCaptureBridge> &bridge,
			std::string_view worldName,
			const ScriptValue &value
		) {
			if (bridge == nullptr) return CaptureUnavailable();
			DataCaptureBridgeRequest request;
			if (worldName.empty() || worldName.size() > 256 || value.Tag != ValueTag::Map ||
				!BoundedStringField(value, "snapshot_id", 256, request.SnapshotId) ||
				!BoundedStringField(value, "pipeline", 256, request.Pipeline) ||
				!BoundedStringField(value, "capture_node", 256, request.CaptureNode) ||
				!BoundedStringField(value, "temporal_history", 32, request.TemporalHistory)) {
				return {"invalid_argument", Map({{"status", String("invalid_capture_request")}})};
			}
			request.InstanceId = worldName;
			const ScriptValue *slot = Field(value, "view_slot");
			if (slot == nullptr || slot->Tag != ValueTag::Number || !std::isfinite(slot->Number) ||
				!(slot->Number >= 0.0) ||
				slot->Number > static_cast<double>(std::numeric_limits<uint32_t>::max()) ||
				static_cast<double>(static_cast<uint64_t>(slot->Number)) != slot->Number) {
				return {"invalid_argument", Map({{"status", String("invalid_capture_request")}})};
			}
			request.ViewSlot = static_cast<uint64_t>(slot->Number);
			const ScriptValue *channels = Field(value, "channels");
			if (channels == nullptr || channels->Tag != ValueTag::Array || channels->Items.empty() ||
				channels->Items.size() > 12)
				return {"invalid_argument", Map({{"status", String("invalid_capture_request")}})};
			request.Channels.reserve(channels->Items.size());
			for (const ScriptValue &channel : channels->Items) {
				if (channel.Tag != ValueTag::String || channel.Text.empty() || channel.Text.size() > 64)
					return {"invalid_argument", Map({{"status", String("invalid_capture_request")}})};
				if (std::find(request.Channels.begin(), request.Channels.end(), channel.Text) !=
					request.Channels.end())
					return {"invalid_argument", Map({{"status", String("invalid_capture_request")}})};
				request.Channels.push_back(channel.Text);
			}
			uint64_t ticket = 0;
			std::string detail;
			if (!bridge->Queue(worldName, request, ticket, detail))
				return {
					"rejected", Map({{"status", String("capture_rejected")}, {"reason", String(detail)}})
				};
			return {
				"ok",
				Map({
					{"status", String("queued")},
					{"ticket", String(Decimal(ticket))},
					{"detail", String(detail)},
				})
			};
		}

		// DataSceneOptions is plain, copied script data. The current bridge captures
		// only its selected view, so CameraId is the explicit current_view token
		// rather than a misleading arbitrary Instance reference.
		ScriptValue NewCaptureOptions() {
			return Map({
				{"SchemaVersion", String("data-scene-options/v1")},
				{"Channels", Array({String("rgb_linear_hdr")})},
				{"CameraId", String("current_view")},
				{"Pipeline", String("Default PBR")},
				{"CaptureNode", String("data-capture")},
				{"ViewSlot", Number(0)},
				{"TemporalHistory", String("preserve")},
				{"StorageProfile", String("lossless")},
				{"Output", String("raw_planes")},
				{"IncludeSceneData", Boolean(false)},
				{"IncludeExactMasks", Boolean(false)},
				{"CoordinateSpace", String("world_camera_image")},
				{"NoiseMode", String("none")},
				{"NoiseSeed", Number(0)},
			});
		}

		bool OptionText(
			const ScriptValue &options, std::string_view name, std::string_view expected, std::string &out
		) {
			return BoundedStringField(options, name, 256, out) && out == expected;
		}

		DataSceneResult QueueCaptureBundle(
			const std::shared_ptr<DataCaptureBridge> &bridge,
			std::string_view worldName,
			std::string_view snapshotId,
			const ScriptValue &options
		) {
			if (snapshotId.empty() || snapshotId.size() > 256 ||
				snapshotId.find('\0') != std::string_view::npos || !DataSceneUtf8(snapshotId) ||
				options.Tag != ValueTag::Map ||
				!HasOnlyFields(
					options,
					{"SchemaVersion",
					 "Channels",
					 "CameraId",
					 "Pipeline",
					 "CaptureNode",
					 "ViewSlot",
					 "TemporalHistory",
					 "StorageProfile",
					 "Output",
					 "IncludeSceneData",
					 "IncludeExactMasks",
					 "CoordinateSpace",
					 "NoiseMode",
					 "NoiseSeed"}
				))
				return {"invalid_argument", Map({{"status", String("invalid_data_scene_options")}})};

			std::string schema, cameraId, pipeline, captureNode, history, storage, output, coordinateSpace,
				noiseMode;
			if (!OptionText(options, "SchemaVersion", "data-scene-options/v1", schema) ||
				!OptionText(options, "CameraId", "current_view", cameraId) ||
				!BoundedStringField(options, "Pipeline", 256, pipeline) ||
				!BoundedStringField(options, "CaptureNode", 256, captureNode) ||
				!OptionText(options, "TemporalHistory", "preserve", history) ||
				!OptionText(options, "StorageProfile", "lossless", storage) ||
				!OptionText(options, "Output", "raw_planes", output) ||
				!OptionText(options, "CoordinateSpace", "world_camera_image", coordinateSpace) ||
				!OptionText(options, "NoiseMode", "none", noiseMode))
				return {"invalid_argument", Map({{"status", String("invalid_data_scene_options")}})};

			const ScriptValue *channels = Field(options, "Channels");
			const ScriptValue *slot = Field(options, "ViewSlot");
			const ScriptValue *sceneData = Field(options, "IncludeSceneData");
			const ScriptValue *exactMasks = Field(options, "IncludeExactMasks");
			const ScriptValue *noiseSeed = Field(options, "NoiseSeed");
			if (channels == nullptr || channels->Tag != ValueTag::Array || channels->Items.empty() ||
				channels->Items.size() > 12 || slot == nullptr || slot->Tag != ValueTag::Number ||
				!std::isfinite(slot->Number) || slot->Number < 0.0 ||
				slot->Number > static_cast<double>(std::numeric_limits<uint32_t>::max()) ||
				static_cast<double>(static_cast<uint64_t>(slot->Number)) != slot->Number ||
				sceneData == nullptr ||
				(sceneData->Tag != ValueTag::True && sceneData->Tag != ValueTag::False) ||
				exactMasks == nullptr ||
				(exactMasks->Tag != ValueTag::True && exactMasks->Tag != ValueTag::False) ||
				noiseSeed == nullptr || noiseSeed->Tag != ValueTag::Number || noiseSeed->Number != 0.0)
				return {"invalid_argument", Map({{"status", String("invalid_data_scene_options")}})};

			if (sceneData->Boolean)
				return {
					"unsupported",
					Map({
						{"status", String("unsupported_data_scene_options")},
						{"reason", String("bundle scene sidecar is not captured by the renderer")},
					})
				};
			if (exactMasks->Boolean)
				return {
					"unsupported",
					Map({
						{"status", String("unsupported_data_scene_options")},
						{"reason", String("bundle exact mask assembly is not implemented")},
					})
				};

			std::vector<ScriptValue> copiedChannels;
			copiedChannels.reserve(channels->Items.size());
			bool hasSecondSurfaceDepth = false;
			bool hasSecondSurfaceValidity = false;
			for (const ScriptValue &channel : channels->Items) {
				if (channel.Tag != ValueTag::String || channel.Text.empty() || channel.Text.size() > 64 ||
					std::any_of(
						copiedChannels.begin(), copiedChannels.end(), [&](const ScriptValue &existing) {
							return existing.Text == channel.Text;
						}
					))
					return {"invalid_argument", Map({{"status", String("invalid_data_scene_options")}})};
				hasSecondSurfaceDepth = hasSecondSurfaceDepth || channel.Text == "second_surface_depth";
				hasSecondSurfaceValidity =
					hasSecondSurfaceValidity || channel.Text == "second_surface_validity";
				copiedChannels.push_back(String(channel.Text));
			}
			if (hasSecondSurfaceDepth != hasSecondSurfaceValidity)
				return {"invalid_argument", Map({{"status", String("invalid_data_scene_options")}})};

			// Build a separate tree before queuing. The bridge retains only this copied
			// request, never the reusable script-side options table.
			const ScriptValue request = Map({
				{"snapshot_id", String(snapshotId)},
				{"pipeline", String(pipeline)},
				{"capture_node", String(captureNode)},
				{"view_slot", Number(slot->Number)},
				{"channels", Array(std::move(copiedChannels))},
				{"temporal_history", String(history)},
			});
			DataSceneResult queued = QueueCapture(bridge, worldName, request);
			if (queued.Value.Tag == ValueTag::Map && queued.Status == std::string_view("ok"))
				queued.Value.Entries.push_back({"options", options});
			return queued;
		}

		ScriptValue Matrix(const std::array<double, 16> &matrix) {
			std::vector<ScriptValue> values;
			values.reserve(matrix.size());
			for (double value : matrix)
				values.push_back(Number(value));
			return Array(std::move(values));
		}

		DataSceneResult PollCapture(
			const std::shared_ptr<DataCaptureBridge> &bridge,
			std::string_view worldName,
			std::string_view text
		) {
			if (bridge == nullptr) return CaptureUnavailable();
			uint64_t ticket = 0;
			if (!Ticket(text, ticket))
				return {"invalid_argument", Map({{"status", String("invalid_capture_ticket")}})};
			DataCaptureBridgePoll poll;
			std::string detail;
			if (!bridge->Poll(worldName, ticket, poll, detail))
				return {
					"unknown", Map({{"status", String("unknown_capture_ticket")}, {"reason", String(detail)}})
				};
			std::vector<ScriptValue> planes;
			planes.reserve(poll.Planes.size());
			for (const DataCaptureBridgePlane &plane : poll.Planes) {
				planes.push_back(Map({
					{"channel", String(plane.Channel)},
					{"status", String(plane.Status)},
					{"resource", String(plane.Resource)},
					{"source_resource", String(plane.SourceResource)},
					{"hash_algorithm", String(plane.HashAlgorithm)},
					{"hash", String(plane.Hash)},
					{"width", Number(plane.Width)},
					{"height", Number(plane.Height)},
					{"row_stride", Number(plane.RowStride)},
					{"byte_size", Number(plane.ByteSize)},
					{"scalar", String(plane.Scalar)},
					{"color_space", String(plane.ColourSpace)},
					{"origin", String(plane.Origin)},
					{"packing", String(plane.Packing)},
					{"provenance", String(plane.Provenance)},
				}));
			}
			std::vector<std::pair<std::string, ScriptValue>> entries{
				{"status", String(poll.Status)},
				{"snapshot_id", String(poll.SnapshotId)},
				{"capture_frame", String(Decimal(poll.CaptureFrame))},
				{"planes", Array(std::move(planes))},
				{"detail", String(detail)},
			};
			std::vector<ScriptValue> labels;
			labels.reserve(poll.ObjectLabels.size());
			for (const DataCaptureBridgeObjectLabel &label : poll.ObjectLabels) {
				labels.push_back(
					Map({{"label", Number(label.Label)}, {"stable_id", String(label.StableId)}})
				);
			}
			entries.emplace_back("object_labels", Array(std::move(labels)));
			std::vector<ScriptValue> semanticLabels;
			semanticLabels.reserve(poll.SemanticLabels.size());
			for (const DataCaptureBridgeObjectLabel &label : poll.SemanticLabels)
				semanticLabels.push_back(
					Map({{"label", Number(label.Label)}, {"stable_id", String(label.StableId)}})
				);
			entries.emplace_back("semantic_labels", Array(std::move(semanticLabels)));
			std::vector<ScriptValue> partLabels;
			partLabels.reserve(poll.PartLabels.size());
			for (const DataCaptureBridgeObjectLabel &label : poll.PartLabels)
				partLabels.push_back(
					Map({{"label", Number(label.Label)}, {"stable_id", String(label.StableId)}})
				);
			entries.emplace_back("part_labels", Array(std::move(partLabels)));
			if (poll.HasCamera) {
				std::vector<std::pair<std::string, ScriptValue>> camera{
					{"world_from_camera", Matrix(poll.WorldFromCamera)},
					{"vertical_fov_radians", Number(poll.VerticalFieldOfViewRadians)},
					{"near_metres", Number(poll.NearMetres)},
					{"far_metres", Number(poll.FarMetres)},
					{"crop",
					 Array(
						 {Number(poll.CropLeft),
						  Number(poll.CropTop),
						  Number(poll.CropWidth),
						  Number(poll.CropHeight)}
					 )},
					{"crop_convention", String(poll.CropConvention)},
					{"lens_distortion_available", Boolean(poll.LensDistortionAvailable)},
					{"lens_distortion_reason", String(poll.LensDistortionReason)},
					{"jitter_available", Boolean(poll.JitterAvailable)},
					{"jitter_policy", String(poll.JitterPolicy)},
					{"coordinate_convention", String(poll.CoordinateConvention)},
				};
				if (poll.HasProjection) camera.emplace_back("projection", Matrix(poll.Projection));
				entries.emplace_back("camera", Map(std::move(camera)));
			}
			return {"ok", Map(std::move(entries))};
		}

		DataSceneResult CancelCapture(
			const std::shared_ptr<DataCaptureBridge> &bridge,
			std::string_view worldName,
			std::string_view text
		) {
			if (bridge == nullptr) return CaptureUnavailable();
			uint64_t ticket = 0;
			if (!Ticket(text, ticket))
				return {"invalid_argument", Map({{"status", String("invalid_capture_ticket")}})};
			bridge->Cancel(worldName, ticket);
			return {
				"ok", Map({{"status", String("cancellation_requested")}, {"ticket", String(Decimal(ticket))}})
			};
		}

		void ReadCaptureBuffer(ScriptCall &call) {
			const auto bridge = call.DataCapture();
			if (bridge == nullptr) {
				call.Raise("render capture is unavailable");
			}
			uint64_t ticket = 0;
			if (!Ticket(call.AsString(0), ticket)) call.Raise("invalid capture ticket");
			const std::string resource = call.AsString(1);
			const double requestedOffset = call.AsNumber(2);
			const double requestedMaximum = call.AsNumber(3);
			if (!std::isfinite(requestedOffset) || !std::isfinite(requestedMaximum) ||
				requestedOffset < 0.0 || requestedMaximum <= 0.0 ||
				requestedOffset > static_cast<double>(std::numeric_limits<size_t>::max()) ||
				requestedMaximum > 64.0 * 1024.0 * 1024.0 ||
				static_cast<double>(static_cast<size_t>(requestedOffset)) != requestedOffset ||
				static_cast<double>(static_cast<size_t>(requestedMaximum)) != requestedMaximum) {
				call.Raise("invalid capture byte range");
			}
			std::vector<std::byte> bytes;
			std::string detail;
			if (!bridge->ReadPlane(
					call.World().Name(),
					ticket,
					resource,
					static_cast<size_t>(requestedOffset),
					static_cast<size_t>(requestedMaximum),
					bytes,
					detail
				)) {
				call.Raise((std::string("capture bytes unavailable: ") + detail).c_str());
			}
			call.ReturnBytes(bytes);
		}

		DataSceneResult ReleaseCapture(
			const std::shared_ptr<DataCaptureBridge> &bridge,
			std::string_view worldName,
			std::string_view text
		) {
			if (bridge == nullptr) return CaptureUnavailable();
			uint64_t ticket = 0;
			if (!Ticket(text, ticket))
				return {"invalid_argument", Map({{"status", String("invalid_capture_ticket")}})};
			std::string detail;
			if (!bridge->Release(worldName, ticket, detail))
				return {
					"rejected",
					Map({{"status", String("capture_release_rejected")}, {"reason", String(detail)}})
				};
			return {"ok", Map({{"status", String("released")}, {"ticket", String(Decimal(ticket))}})};
		}

		DataSceneResult LifecycleUnavailable() {
			return {
				"unsupported",
				Map({{"status", String("capability_unsupported")}, {"feature", String("lifecycle")}})
			};
		}

		DataSceneResult QueueLifecycle(
			const std::shared_ptr<DataLifecycleBridge> &bridge,
			std::string_view worldName,
			const ScriptValue &value
		) {
			if (bridge == nullptr) return LifecycleUnavailable();
			DataLifecycleBridgeRequest request;
			if (value.Tag != ValueTag::Map ||
				!BoundedStringField(value, "operation", 32, request.Operation) || worldName.empty() ||
				worldName.size() > 256 || worldName.find('\0') != std::string_view::npos)
				return {"invalid_argument", Map({{"status", String("invalid_lifecycle_request")}})};
			request.InstanceId = worldName;
			const bool inspect = request.Operation == "inspect";
			const bool known = inspect || request.Operation == "pause" || request.Operation == "resume" ||
							   request.Operation == "step" || request.Operation == "snapshot" ||
							   request.Operation == "checkpoint" || request.Operation == "restore" ||
							   request.Operation == "render_only";
			if (!known) return {"invalid_argument", Map({{"status", String("invalid_lifecycle_request")}})};
			if (request.Operation == "inspect" && !HasOnlyFields(value, {"operation"}))
				return {"invalid_argument", Map({{"status", String("invalid_lifecycle_request")}})};
			if (request.Operation == "pause" && !HasOnlyFields(
													value,
													{"operation",
													 "operation_id",
													 "expected_tick",
													 "expected_world_epoch",
													 "expected_world_version",
													 "scope"}
												))
				return {"invalid_argument", Map({{"status", String("invalid_lifecycle_request")}})};
			if ((request.Operation == "resume" || request.Operation == "snapshot" ||
				 request.Operation == "checkpoint") &&
				!HasOnlyFields(
					value,
					{"operation",
					 "operation_id",
					 "expected_tick",
					 "expected_world_epoch",
					 "expected_world_version"}
				))
				return {"invalid_argument", Map({{"status", String("invalid_lifecycle_request")}})};
			if (request.Operation == "restore" && !HasOnlyFields(
													  value,
													  {"operation",
													   "operation_id",
													   "expected_tick",
													   "expected_world_epoch",
													   "expected_world_version",
													   "checkpoint_id"}
												  ))
				return {"invalid_argument", Map({{"status", String("invalid_lifecycle_request")}})};
			if (request.Operation == "step" && !HasOnlyFields(
												   value,
												   {"operation",
													"operation_id",
													"expected_tick",
													"expected_world_epoch",
													"expected_world_version",
													"dt_ns"}
											   ))
				return {"invalid_argument", Map({{"status", String("invalid_lifecycle_request")}})};
			if (request.Operation == "render_only" && !HasOnlyFields(
														  value,
														  {"operation",
														   "operation_id",
														   "expected_tick",
														   "expected_world_epoch",
														   "expected_world_version",
														   "snapshot_id",
														   "temporal_history"}
													  ))
				return {"invalid_argument", Map({{"status", String("invalid_lifecycle_request")}})};
			if (const ScriptValue *scope = Field(value, "scope"); scope != nullptr) {
				if (scope->Tag != ValueTag::String ||
					(scope->Text != "all_systems" && scope->Text != "physics_only"))
					return {"invalid_argument", Map({{"status", String("invalid_lifecycle_request")}})};
				request.Scope = scope->Text;
			}
			if (const ScriptValue *snapshot = Field(value, "checkpoint_id"); snapshot != nullptr) {
				if (snapshot->Tag != ValueTag::String || snapshot->Text.empty() ||
					snapshot->Text.size() > 256 || snapshot->Text.find('\0') != std::string::npos)
					return {"invalid_argument", Map({{"status", String("invalid_lifecycle_request")}})};
				request.CheckpointId = snapshot->Text;
			}
			if (request.Operation == "render_only") {
				if (!BoundedStringField(value, "snapshot_id", 256, request.SnapshotId) ||
					!BoundedStringField(value, "temporal_history", 16, request.TemporalHistory) ||
					request.TemporalHistory != "preserve")
					return {"invalid_argument", Map({{"status", String("invalid_lifecycle_request")}})};
			}
			auto count = [&](std::string_view name, uint64_t &out) {
				const ScriptValue *field = Field(value, name);
				return field != nullptr && field->Tag == ValueTag::String && Ticket(field->Text, out);
			};
			if (!known ||
				(!inspect && (!BoundedStringField(value, "operation_id", 128, request.OperationId) ||
							  !count("expected_tick", request.ExpectedTick) ||
							  !count("expected_world_epoch", request.ExpectedWorldEpoch) ||
							  !count("expected_world_version", request.ExpectedWorldVersion))))
				return {"invalid_argument", Map({{"status", String("invalid_lifecycle_request")}})};
			if (request.Operation == "step") {
				const ScriptValue *interval = Field(value, "dt_ns");
				uint64_t denominator = 0;
				if (interval == nullptr || !HasOnlyFields(*interval, {"numerator", "denominator"}))
					return {"invalid_argument", Map({{"status", String("invalid_lifecycle_request")}})};
				const auto intervalCount = [&](std::string_view name, uint64_t &out) {
					const ScriptValue *field = Field(*interval, name);
					return field != nullptr && field->Tag == ValueTag::String && Ticket(field->Text, out);
				};
				if (!intervalCount("numerator", request.DtNumeratorNanoseconds) ||
					!intervalCount("denominator", denominator) || request.DtNumeratorNanoseconds == 0 ||
					denominator == 0 || denominator > std::numeric_limits<uint32_t>::max())
					return {"invalid_argument", Map({{"status", String("invalid_lifecycle_request")}})};
				request.DtDenominator = static_cast<uint32_t>(denominator);
			}
			uint64_t ticket = 0;
			std::string detail;
			if (!bridge->Queue(worldName, request, ticket, detail))
				return {
					"rejected", Map({{"status", String("lifecycle_rejected")}, {"reason", String(detail)}})
				};
			return {"ok", Map({{"status", String("queued")}, {"ticket", String(Decimal(ticket))}})};
		}

		DataSceneResult PollLifecycle(
			const std::shared_ptr<DataLifecycleBridge> &bridge,
			std::string_view worldName,
			std::string_view text
		) {
			if (bridge == nullptr) return LifecycleUnavailable();
			uint64_t ticket = 0;
			if (!Ticket(text, ticket))
				return {"invalid_argument", Map({{"status", String("invalid_lifecycle_ticket")}})};
			DataLifecycleBridgeReply reply;
			std::string detail;
			if (!bridge->Poll(worldName, ticket, reply, detail))
				return {
					"unknown",
					Map({{"status", String("unknown_lifecycle_ticket")}, {"reason", String(detail)}})
				};
			return {
				"ok",
				Map(
					{{"status", String(reply.Status)},
					 {"detail", String(reply.Detail)},
					 {"instance_id", String(reply.InstanceId)},
					 {"snapshot_id", String(reply.SnapshotId)},
					 {"checkpoint_id", String(reply.CheckpointId)},
					 {"tick", String(reply.Tick)},
					 {"time_nanoseconds", String(reply.TimeNanoseconds)},
					 {"version", String(reply.Version)},
					 {"epoch", String(reply.Epoch)},
					 {"operation_id", String(reply.OperationId)},
					 {"temporal_history", String(reply.TemporalHistory)}}
				)
			};
		}

		DataSceneResult ReleaseLifecycle(
			const std::shared_ptr<DataLifecycleBridge> &bridge,
			std::string_view worldName,
			std::string_view text
		) {
			if (bridge == nullptr) return LifecycleUnavailable();
			uint64_t ticket = 0;
			std::string detail;
			if (!Ticket(text, ticket))
				return {"invalid_argument", Map({{"status", String("invalid_lifecycle_ticket")}})};
			if (!bridge->Release(worldName, ticket, detail))
				return {
					"rejected",
					Map({{"status", String("lifecycle_release_rejected")}, {"reason", String(detail)}})
				};
			return {"ok", Map({{"status", String("released")}})};
		}

		bool StableId(const ecs::Store &store, ecs::Entity entity, std::string &out) {
			ecs::AttributeValue value;
			if (!ecs::GetAttribute(store, entity, core::Name(DATA_SCENE_ID_ATTRIBUTE), value) ||
				value.Type != ecs::PropertyType::String || value.String.empty()) {
				return false;
			}
			out = value.String;
			return true;
		}

		ScriptValue EntityRecord(
			const ecs::Store &store,
			ecs::Entity entity,
			std::string_view id,
			const std::unordered_map<uint64_t, std::string> &ids,
			const physics::PhysicsWorld *physicsWorld
		) {
			const ecs::ClassInfo &classInfo = ecs::Classes::Describe(store.ClassOf(entity));
			std::vector<std::pair<std::string, ScriptValue>> entries{
				{"id", String(id)},
				{"name", String(store.InstanceNameOf(entity).Text())},
				{"class", String(classInfo.Name.Text())},
			};
			if (const auto parent = ids.find(store.ParentOf(entity).Id); parent != ids.end()) {
				entries.emplace_back("parent_id", String(parent->second));
			}
			if (const auto *transform = store.Get<scene::Transform>(entity); transform != nullptr) {
				entries.emplace_back("transform", Frame(transform->Frame));
				entries.emplace_back("world_from_object", Frame(transform->Frame));
				const auto *parent = store.Get<scene::Transform>(store.ParentOf(entity));
				const core::CFrame relative =
					parent == nullptr ? core::CFrame{} : parent->Frame.Inverse() * transform->Frame;
				const bool relativeAvailable = parent != nullptr && Finite(transform->Frame) &&
											   Finite(parent->Frame) && Finite(relative);
				entries.emplace_back(
					"parent_from_object",
					!relativeAvailable ? Map({
											 {"available", Boolean(false)},
											 {"frame", ScriptValue{}},
											 {"source", String("derived_from_world_transforms")},
											 {"reason", String("parent_transform_unavailable_or_invalid")},
										 })
									   : Map({
											 {"available", Boolean(true)},
											 {"frame", Frame(relative)},
											 {"source", String("derived_from_world_transforms")},
											 {"reason", String("")},
										 })
				);
			}
			if (const auto *bounds = store.Get<scene::Bounds>(entity); bounds != nullptr) {
				entries.emplace_back("size_metres", Vector(bounds->HalfExtent * 2.0f));
			}
			if (const auto *visual = store.Get<scene::Visual>(entity); visual != nullptr) {
				entries.emplace_back(
					"material",
					Map({
						{"mesh", String(visual->Mesh.Text())},
						{"tint", Colour(visual->Tint)},
						{"transparency", Number(visual->Transparency)},
						{"visible", Boolean(visual->Visible)},
						{"casts_shadow", Boolean(visual->CastShadow)},
					})
				);
			}
			if (const auto *surface = store.Get<scene::Surface>(entity); surface != nullptr) {
				std::vector<std::pair<std::string, ScriptValue>> material{
					{"name", String(surface->Material.Text())}
				};
				if (const auto *table = store.Resource<scene::SurfaceTable>(); table != nullptr) {
					if (const auto *properties = table->Find(surface->Material); properties != nullptr) {
						material.emplace_back("friction", Number(properties->Friction));
						material.emplace_back("restitution", Number(properties->Restitution));
					}
				}
				entries.emplace_back("surface_material", Map(std::move(material)));
			}
			if (const auto *appearance = store.Get<scene::SurfaceAppearance>(entity); appearance != nullptr) {
				entries.emplace_back(
					"pbr",
					Map({
						{"base_color_map", String(appearance->ColourMap.Text())},
						{"normal_map", String(appearance->NormalMap.Text())},
						{"roughness_map", String(appearance->RoughnessMap.Text())},
						{"metalness_map", String(appearance->MetalnessMap.Text())},
						{"emissive_map", String(appearance->EmissiveMap.Text())},
						{"base_color", Colour(appearance->Colour)},
						{"emissive_tint", Colour(appearance->EmissiveTint)},
						{"emissive_strength", Number(appearance->EmissiveStrength)},
						{"alpha_cutoff", Number(appearance->AlphaCutoff)},
					})
				);
			}
			if (const auto *light = store.Get<scene::Light>(entity); light != nullptr) {
				entries.emplace_back(
					"light",
					Map({
						{"kind", String(LightKindName(light->Kind))},
						{"color", Colour(light->Colour)},
						{"brightness", Number(light->Brightness)},
						{"range_metres", Number(light->Range)},
						{"angle_degrees", Number(light->Angle)},
						{"enabled", Boolean(light->Enabled)},
						{"shadows", Boolean(light->Shadows)},
					})
				);
			}
			if (const auto *skeleton = store.Get<scene::Skeleton>(entity); skeleton != nullptr) {
				entries.emplace_back(
					"skeleton",
					Map({
						{"rig", String(skeleton->Rig.Text())},
						{"joint_count", Number(skeleton->JointCount)},
						{"pose_scale", Number(skeleton->PoseScale)},
					})
				);
			}
			if (const auto *bone = store.Get<scene::Bone>(entity); bone != nullptr) {
				entries.emplace_back(
					"bone",
					Map({
						{"joint", Number(bone->Joint)},
						{"parent_joint", Number(bone->ParentJoint)},
						{"rest", Frame(bone->Rest)},
						{"transform", Frame(bone->Transform)},
						{"inverse_bind", Frame(bone->InverseBind)},
						{"world_frame", Frame(bone->WorldFrame)},
					})
				);
			}
			if (const auto *body = store.Get<scene::RigidBody>(entity); body != nullptr) {
				const bool sleeping = physicsWorld != nullptr && physicsWorld->Sleeping(entity);
				const bool simulated = store.Has<scene::Simulated>(entity);
				const bool assemblyAvailable = physicsWorld != nullptr;
				const ecs::Entity root =
					physicsWorld == nullptr ? ecs::NULL_ENTITY : physicsWorld->RigidAssemblyRoot(entity);
				const auto found = ids.find(root.Id);
				const bool hasAssembly = root != ecs::NULL_ENTITY;
				const bool rootHasStableId = hasAssembly && found != ids.end();
				const bool awake =
					assemblyAvailable && simulated && body->Kind == scene::BodyKind::Dynamic && !sleeping;
				const char *sleepState = !simulated || body->Kind != scene::BodyKind::Dynamic
											 ? "not_applicable"
										 : physicsWorld == nullptr ? "unavailable"
										 : sleeping				   ? "sleeping"
																   : "awake";
				std::vector<std::pair<std::string, ScriptValue>> physics{
					{"mass_kg", Number(body->Mass)},
					{"linear_damping_per_second", Number(body->LinearDamping)},
					{"angular_damping_per_second", Number(body->AngularDamping)},
					{"body_kind", String(scene::Describe(body->Kind))},
					{"simulated", Boolean(simulated)},
					{"awake", Boolean(awake)},
					{"sleeping", Boolean(sleeping)},
					{"sleep_state", String(sleepState)},
					{"assembly_available", Boolean(assemblyAvailable)},
					{"has_rigid_assembly", Boolean(hasAssembly)},
					{"assembly_root_has_stable_id", Boolean(rootHasStableId)},
					{"assembly_root_id", String(rootHasStableId ? found->second : "")},
					{"units",
					 Map({
						 {"mass", String("kg")},
						 {"linear_velocity", String("m/s")},
						 {"angular_velocity", String("rad/s")},
						 {"impulse", String("N*s")},
						 {"force", String("N")},
						 {"torque", String("N*m")},
					 })},
				};
				entries.emplace_back("physics", Map(std::move(physics)));
			}
			if (const auto *motion = store.Get<scene::Motion>(entity); motion != nullptr) {
				entries.emplace_back(
					"motion",
					Map({
						{"linear_mps", Vector(motion->Linear)},
						{"angular_radps", Vector(motion->Angular)},
					})
				);
			}
			if (const auto *body = store.Get<scene::RigidBody>(entity); body != nullptr) {
				entries.emplace_back(
					"rigid_body",
					Map({
						{"mass_kg", Number(body->Mass)},
						{"linear_damping_fraction_per_second", Number(body->LinearDamping)},
						{"angular_damping_fraction_per_second", Number(body->AngularDamping)},
						{"simulated", Boolean(store.Has<scene::Simulated>(entity))},
					})
				);
			}
			std::vector<ScriptValue> joints;
			if (const auto *constraint = store.Get<scene::Constraint>(entity); constraint != nullptr) {
				const auto stable = [&](ecs::Entity target) {
					const auto found = ids.find(target.Id);
					return String(found == ids.end() ? "" : found->second);
				};
				std::vector<ScriptValue> linearMotion;
				std::vector<ScriptValue> angularMotion;
				std::vector<ScriptValue> linearLower;
				std::vector<ScriptValue> linearUpper;
				std::vector<ScriptValue> angularLower;
				std::vector<ScriptValue> angularUpper;
				for (size_t axis = 0; axis < scene::CONSTRAINT_AXES; ++axis) {
					linearMotion.push_back(String(ConstraintMotionName(constraint->Linear[axis])));
					angularMotion.push_back(String(ConstraintMotionName(constraint->Angular[axis])));
					linearLower.push_back(Number(constraint->LinearLower[axis]));
					linearUpper.push_back(Number(constraint->LinearUpper[axis]));
					angularLower.push_back(Number(constraint->AngularLower[axis]));
					angularUpper.push_back(Number(constraint->AngularUpper[axis]));
				}
				joints.push_back(Map({
					{"kind", String("constraint")},
					{"attachment0_id", stable(constraint->Attachment0)},
					{"attachment1_id", stable(constraint->Attachment1)},
					{"attachment0_has_stable_id", Boolean(ids.contains(constraint->Attachment0.Id))},
					{"attachment1_has_stable_id", Boolean(ids.contains(constraint->Attachment1.Id))},
					{"enabled", Boolean(constraint->Enabled)},
					{"physics_support", String("authored_only")},
					{"target", Frame(constraint->Target)},
					{"linear_motion", Array(std::move(linearMotion))},
					{"angular_motion", Array(std::move(angularMotion))},
					{"linear_lower_metres", Array(std::move(linearLower))},
					{"linear_upper_metres", Array(std::move(linearUpper))},
					{"angular_lower_radians", Array(std::move(angularLower))},
					{"angular_upper_radians", Array(std::move(angularUpper))},
					{"stiffness_n_per_m", Number(constraint->Stiffness)},
					{"damping_newton_seconds_per_metre", Number(constraint->Damping)},
					{"max_force_n", Number(constraint->MaxForce)},
					{"max_torque_nm", Number(constraint->MaxTorque)},
					{"units",
					 Map({
						 {"linear", String("m")},
						 {"angular", String("rad")},
						 {"stiffness", String("N/m")},
						 {"damping", String("N*s/m")},
						 {"force", String("N")},
						 {"torque", String("N*m")},
					 })},
				}));
			}
			if (const auto *joint = store.Get<scene::JointInstance>(entity); joint != nullptr) {
				const auto stable = [&](ecs::Entity target) {
					const auto found = ids.find(target.Id);
					return String(found == ids.end() ? "" : found->second);
				};
				joints.push_back(Map({
					{"kind", String("joint_instance")},
					{"part0_id", stable(joint->Part0)},
					{"part1_id", stable(joint->Part1)},
					{"part0_has_stable_id", Boolean(ids.contains(joint->Part0.Id))},
					{"part1_has_stable_id", Boolean(ids.contains(joint->Part1.Id))},
					{"enabled", Boolean(joint->Enabled)},
					{"physics_support", String("rigid")},
					{"c0", Frame(joint->C0)},
					{"c1", Frame(joint->C1)},
				}));
			}
			if (const auto *joint = store.Get<scene::WeldConstraint>(entity); joint != nullptr) {
				const auto stable = [&](ecs::Entity target) {
					const auto found = ids.find(target.Id);
					return String(found == ids.end() ? "" : found->second);
				};
				joints.push_back(Map({
					{"kind", String("weld_constraint")},
					{"part0_id", stable(joint->Part0)},
					{"part1_id", stable(joint->Part1)},
					{"part0_has_stable_id", Boolean(ids.contains(joint->Part0.Id))},
					{"part1_has_stable_id", Boolean(ids.contains(joint->Part1.Id))},
					{"enabled", Boolean(joint->Enabled)},
					{"physics_support", String("rigid")},
				}));
			}
			if (!joints.empty()) {
				entries.emplace_back("joints", Array(std::move(joints)));
			}
			if (const auto *humanoid = store.Get<scene::Humanoid>(entity); humanoid != nullptr) {
				const auto root = ids.find(humanoid->RootPart.Id);
				entries.emplace_back(
					"humanoid_controller",
					Map({
						{"root_part_id", String(root == ids.end() ? "" : root->second)},
						{"root_part_has_stable_id", Boolean(root != ids.end())},
						{"move_direction_world", Vector(humanoid->MoveDirection)},
						{"walk_speed_mps", Number(humanoid->WalkSpeed)},
						{"jump_speed_mps", Number(humanoid->JumpSpeed)},
						{"height_metres", Number(humanoid->Height)},
						{"ground_tolerance_metres", Number(humanoid->GroundTolerance)},
						{"health", Number(humanoid->Health)},
						{"max_health", Number(humanoid->MaxHealth)},
						{"grounded", Boolean(humanoid->Grounded)},
						{"jump_requested", Boolean(humanoid->JumpRequested)},
						{"enabled", Boolean(humanoid->Enabled)},
						{"auto_rotate", Boolean(humanoid->AutoRotate)},
						{"units",
						 Map({
							 {"linear_speed", String("m/s")},
							 {"distance", String("m")},
							 {"move_direction", String("unitless_world_space")},
						 })},
					})
				);
			}
			return Map(std::move(entries));
		}

		struct ObservationRecords {
			ScriptValue Values;
			size_t OmittedUnidentifiedEndpoints = 0;
			size_t OmittedResourceLimit = 0;
		};

		ObservationRecords ContactRecords(
			const physics::PhysicsWorld &world, const std::unordered_map<uint64_t, std::string> &ids
		) {
			std::vector<ScriptValue> records;
			records.reserve(std::min(world.Manifolds().size(), MAX_DATA_SCENE_ENTITIES));
			size_t omittedUnidentifiedEndpoints = 0;
			size_t omittedResourceLimit = 0;
			for (const physics::ContactManifold &manifold : world.Manifolds()) {
				const auto first = ids.find(manifold.A.Id);
				const auto second = ids.find(manifold.B.Id);
				if (first == ids.end() || second == ids.end()) {
					omittedUnidentifiedEndpoints++;
					continue;
				}
				if (records.size() == MAX_DATA_SCENE_ENTITIES) {
					omittedResourceLimit++;
					continue;
				}
				core::Vector3 tangent0;
				core::Vector3 tangent1;
				physics::ContactTangentBasis(manifold.Normal, tangent0, tangent1);
				std::vector<ScriptValue> points;
				points.reserve(manifold.PointCount);
				for (size_t index = 0; index < manifold.PointCount; index++) {
					const physics::ContactPoint &point = manifold.Points[index];
					physics::ContactImpulse key;
					key.A = manifold.A;
					key.B = manifold.B;
					key.Feature = point.Feature;
					const auto found =
						std::lower_bound(world.Impulses().begin(), world.Impulses().end(), key);
					const physics::ContactImpulse *impulse =
						found != world.Impulses().end() && !(key < *found) ? &*found : nullptr;
					const float normalImpulse = impulse == nullptr ? 0.0f : impulse->Normal;
					const float tangentImpulse0 = impulse == nullptr ? 0.0f : impulse->Tangent[0];
					const float tangentImpulse1 = impulse == nullptr ? 0.0f : impulse->Tangent[1];
					std::vector<std::pair<std::string, ScriptValue>> pointRecord{
						{"position_metres", Vector(point.Position)},
						{"penetration_metres", Number(point.Penetration)},
						{"separation_metres", Number(point.Separation)},
						{"feature", String(Decimal(point.Feature))},
						{"normal_impulse_newton_seconds", Number(normalImpulse)},
						{"friction_impulse_newton_seconds",
						 Map({
							 {"tangent0", Number(tangentImpulse0)},
							 {"tangent1", Number(tangentImpulse1)},
							 {"tangent0_world_direction", Vector(tangent0)},
							 {"tangent1_world_direction", Vector(tangent1)},
						 })},
						{"world_impulse_newton_seconds",
						 Vector(
							 manifold.Normal * normalImpulse + tangent0 * tangentImpulse0 +
							 tangent1 * tangentImpulse1
						 )},
						{"world_impulse_applied_to", String("b_from_a")},
						{"impulse_provenance",
						 String(impulse == nullptr ? "no_completed_solver_impulse" : "solver_accumulated")},
					};
					points.push_back(Map(std::move(pointRecord)));
				}
				records.push_back(Map({
					{"a_id", String(first->second)},
					{"b_id", String(second->second)},
					{"a_has_stable_id", Boolean(true)},
					{"b_has_stable_id", Boolean(true)},
					{"normal", Vector(manifold.Normal)},
					{"trigger", Boolean(manifold.Trigger)},
					{"points", Array(std::move(points))},
				}));
			}
			return {Array(std::move(records)), omittedUnidentifiedEndpoints, omittedResourceLimit};
		}

		const char *ContactPhaseName(physics::ContactPhase phase) {
			switch (phase) {
			case physics::ContactPhase::Began:
				return "began";
			case physics::ContactPhase::Persisted:
				return "persisted";
			case physics::ContactPhase::Ended:
				return "ended";
			}
			return "unknown";
		}

		ObservationRecords ContactEventRecords(
			const physics::PhysicsWorld &world, const std::unordered_map<uint64_t, std::string> &ids
		) {
			std::vector<ScriptValue> records;
			records.reserve(std::min(world.Events().size(), MAX_DATA_SCENE_ENTITIES));
			size_t omittedUnidentifiedEndpoints = 0;
			size_t omittedResourceLimit = 0;
			for (const physics::ContactEvent &event : world.Events()) {
				const auto first = ids.find(event.A.Id);
				const auto second = ids.find(event.B.Id);
				if (first == ids.end() || second == ids.end()) {
					omittedUnidentifiedEndpoints++;
					continue;
				}
				if (records.size() == MAX_DATA_SCENE_ENTITIES) {
					omittedResourceLimit++;
					continue;
				}
				records.push_back(Map({
					{"a_id", String(first->second)},
					{"b_id", String(second->second)},
					{"a_has_stable_id", Boolean(true)},
					{"b_has_stable_id", Boolean(true)},
					{"phase", String(ContactPhaseName(event.Phase))},
				}));
			}
			return {Array(std::move(records)), omittedUnidentifiedEndpoints, omittedResourceLimit};
		}

		void ServiceCapabilities(ScriptCall &call) {
			DataSceneResult result = GetCapabilities(call.World());
			const auto bridge = call.DataCapture();
			const bool available = bridge != nullptr && bridge->Capabilities().Available;
			for (auto &[name, value] : result.Value.Entries)
				if (name == "render_capture") value = Boolean(available);
			call.ReturnValue(std::move(result.Value));
		}
		void ServiceSnapshot(ScriptCall &call) {
			size_t limit = MAX_DATA_SCENE_ENTITIES;
			if (call.Arguments() > 0) {
				const double requested = call.AsNumber(0);
				if (!(requested >= 0.0) || requested > static_cast<double>(MAX_DATA_SCENE_ENTITIES) ||
					static_cast<double>(static_cast<size_t>(requested)) != requested) {
					call.ReturnValue(Map({{"status", String("invalid_limit")}}));
					return;
				}
				limit = static_cast<size_t>(requested);
			}
			call.ReturnValue(GetSceneSnapshot(call.World(), limit).Value);
		}
		void ServiceCamera(ScriptCall &call) {
			size_t limit = 0;
			if (call.Arguments() > 1) {
				const double requested = call.AsNumber(1);
				if (!(requested >= 0.0) || requested > static_cast<double>(MAX_CAMERA_OBJECT_OBSERVATIONS) ||
					static_cast<double>(static_cast<size_t>(requested)) != requested) {
					call.ReturnValue(Map({{"status", String("invalid_limit")}}));
					return;
				}
				limit = static_cast<size_t>(requested);
			}
			call.ReturnValue(GetCameraRenderingData(call.World(), call.AsInstance(0), limit).Value);
		}
		void ServiceImage(ScriptCall &call) {
			call.ReturnValue(GetEditableImageMetadata(call.World(), call.AsInstance(0)).Value);
		}
		void ServiceChannels(ScriptCall &call) {
			call.ReturnValue(CaptureChannels(call.DataCapture()).Value);
		}
		void ServiceCapture(ScriptCall &call) {
			ScriptValue request;
			CodecStatus status = CodecStatus::Ok;
			if (!call.ReadValue(0, request, status)) {
				call.ReturnValue(
					Map({{"status", String("invalid_capture_request")}, {"reason", String(Describe(status))}})
				);
				return;
			}
			call.ReturnValue(QueueCapture(call.DataCapture(), call.World().Name(), request).Value);
		}
		void ServiceCreateOptions(ScriptCall &call) {
			call.ReturnValue(NewCaptureOptions());
		}
		void ServiceCaptureBundle(ScriptCall &call) {
			ScriptValue options;
			CodecStatus status = CodecStatus::Ok;
			if (!call.ReadValue(1, options, status)) {
				call.ReturnValue(Map(
					{{"status", String("invalid_data_scene_options")}, {"reason", String(Describe(status))}}
				));
				return;
			}
			call.ReturnValue(
				QueueCaptureBundle(call.DataCapture(), call.World().Name(), call.AsString(0), options).Value
			);
		}
		void ServicePollCapture(ScriptCall &call) {
			call.ReturnValue(PollCapture(call.DataCapture(), call.World().Name(), call.AsString(0)).Value);
		}
		void ServiceCancelCapture(ScriptCall &call) {
			call.ReturnValue(CancelCapture(call.DataCapture(), call.World().Name(), call.AsString(0)).Value);
		}
		void ServiceCaptureBuffer(ScriptCall &call) {
			ReadCaptureBuffer(call);
		}
		void ServiceReleaseCapture(ScriptCall &call) {
			call.ReturnValue(ReleaseCapture(call.DataCapture(), call.World().Name(), call.AsString(0)).Value);
		}
		void ServiceSetCaptureDriver(ScriptCall &call) {
			if (call.IsNil(0)) {
				if (auto *previous = call.World().ResourceMutable<DataCaptureDriver>();
					previous != nullptr && previous->Callback.Valid())
					call.ReleaseHostCallback(previous->Callback);
				call.World().RemoveResource<DataCaptureDriver>();
				call.ReturnValue(Map({{"status", String("released")}}));
				return;
			}
			const HostCallback replacement = call.RetainHostCallback(0);
			if (!replacement.Valid()) {
				call.ReturnValue(Map({{"status", String("invalid_driver")}}));
				return;
			}
			if (auto *previous = call.World().ResourceMutable<DataCaptureDriver>();
				previous != nullptr && previous->Callback.Valid())
				call.ReleaseHostCallback(previous->Callback);
			call.World().SetResource(DataCaptureDriver{replacement});
			call.ReturnValue(Map({{"status", String("registered")}}));
		}
		void ServiceRequestLifecycle(ScriptCall &call) {
			ScriptValue request;
			CodecStatus status = CodecStatus::Ok;
			if (!call.ReadValue(0, request, status)) {
				call.ReturnValue(Map({{"status", String("invalid_lifecycle_request")}}));
				return;
			}
			call.ReturnValue(QueueLifecycle(call.DataLifecycle(), call.World().Name(), request).Value);
		}
		void ServicePollLifecycle(ScriptCall &call) {
			call.ReturnValue(
				PollLifecycle(call.DataLifecycle(), call.World().Name(), call.AsString(0)).Value
			);
		}
		void ServiceReleaseLifecycle(ScriptCall &call) {
			call.ReturnValue(
				ReleaseLifecycle(call.DataLifecycle(), call.World().Name(), call.AsString(0)).Value
			);
		}
		void ServiceResources(ScriptCall &call) {
			call.ReturnValue(GetResources(call.World()).Value);
		}
		void ServiceSetEventNarratives(ScriptCall &call) {
			ScriptValue bundle;
			CodecStatus status = CodecStatus::Ok;
			if (!call.ReadValue(0, bundle, status)) {
				call.ReturnValue(InvalidEventNarratives("invalid_codec_value").Value);
				return;
			}
			call.ReturnValue(SetEventNarratives(call.World(), bundle).Value);
		}
		void ServiceEventNarratives(ScriptCall &call) {
			call.ReturnValue(GetEventNarratives(call.World()).Value);
		}
		void ServiceRaycast(ScriptCall &call) {
			ScriptValue request;
			CodecStatus status = CodecStatus::Ok;
			if (!call.ReadValue(0, request, status)) {
				call.ReturnValue(Map({{"status", String("invalid_raycast_query")}}));
				return;
			}
			core::Vector3 origin;
			core::Vector3 direction;
			const ScriptValue *distance = Field(request, "max_distance_metres");
			if (!HasOnlyFields(request, {"origin", "direction", "max_distance_metres"}) ||
				!VectorField(request, "origin", origin) || !VectorField(request, "direction", direction) ||
				distance == nullptr || distance->Tag != ValueTag::Number) {
				call.ReturnValue(Map({{"status", String("invalid_raycast_query")}}));
				return;
			}
			call.ReturnValue(
				Raycast(call.World(), {origin, direction, static_cast<float>(distance->Number)}).Value
			);
		}
		void ServiceAabb(ScriptCall &call) {
			ScriptValue request;
			CodecStatus status = CodecStatus::Ok;
			if (!call.ReadValue(0, request, status)) {
				call.ReturnValue(Map({{"status", String("invalid_aabb_query")}}));
				return;
			}
			core::Vector3 minimum;
			core::Vector3 maximum;
			if (!HasOnlyFields(request, {"minimum", "maximum"}) ||
				!VectorField(request, "minimum", minimum) || !VectorField(request, "maximum", maximum)) {
				call.ReturnValue(Map({{"status", String("invalid_aabb_query")}}));
				return;
			}
			call.ReturnValue(OverlapAABB(call.World(), {minimum, maximum}).Value);
		}
		void ServiceObb(ScriptCall &call) {
			ScriptValue request;
			CodecStatus status = CodecStatus::Ok;
			if (!call.ReadValue(0, request, status)) {
				call.ReturnValue(Map({{"status", String("invalid_obb_query")}}));
				return;
			}
			const ScriptValue *frame = Field(request, "frame");
			core::Vector3 halfExtent;
			if (!HasOnlyFields(request, {"frame", "half_extent"}) || frame == nullptr ||
				frame->Tag != ValueTag::CFrame || !VectorField(request, "half_extent", halfExtent)) {
				call.ReturnValue(Map({{"status", String("invalid_obb_query")}}));
				return;
			}
			call.ReturnValue(OverlapOBB(call.World(), {frame->Frame, halfExtent}).Value);
		}

		void ServiceColliderBev(ScriptCall &call) {
			ScriptValue value;
			CodecStatus status = CodecStatus::Ok;
			if (!call.ReadValue(0, value, status)) {
				call.ReturnValue(Map({{"status", String("invalid_collider_bev")}}));
				return;
			}
			const auto number = [](const ScriptValue *field, float &out) {
				if (field == nullptr || field->Tag != ValueTag::Number || !std::isfinite(field->Number) ||
					field->Number < -std::numeric_limits<float>::max() ||
					field->Number > std::numeric_limits<float>::max())
					return false;
				out = static_cast<float>(field->Number);
				return std::isfinite(out);
			};
			const auto dimension = [](const ScriptValue *field, uint8_t &out) {
				if (field == nullptr || field->Tag != ValueTag::Number || !std::isfinite(field->Number) ||
					field->Number != std::floor(field->Number) || field->Number < 1.0 || field->Number > 8.0)
					return false;
				out = static_cast<uint8_t>(field->Number);
				return true;
			};
			const ScriptValue *bounds = Field(value, "xz_bounds_metres");
			if (!HasOnlyFields(
					value, {"xz_bounds_metres", "y_minimum_metres", "y_maximum_metres", "rows", "columns"}
				) ||
				bounds == nullptr || !HasOnlyFields(*bounds, {"minimum", "maximum"})) {
				call.ReturnValue(Map({{"status", String("invalid_collider_bev")}}));
				return;
			}
			DataSceneColliderBevRequest request;
			const auto boundsField = [](const ScriptValue &map,
										std::string_view name) -> const ScriptValue * {
				for (const auto &[key, field] : map.Entries)
					if (key == name) return &field;
				return nullptr;
			};
			const auto boundsPair = [&](std::string_view name, float &x, float &z) {
				const ScriptValue *pair = boundsField(*bounds, name);
				if (pair == nullptr || pair->Tag != ValueTag::Array || pair->Items.size() != 2) return false;
				return number(&pair->Items[0], x) && number(&pair->Items[1], z);
			};
			if (!boundsPair("minimum", request.MinimumXMetres, request.MinimumZMetres) ||
				!boundsPair("maximum", request.MaximumXMetres, request.MaximumZMetres) ||
				!number(Field(value, "y_minimum_metres"), request.MinimumYMetres) ||
				!number(Field(value, "y_maximum_metres"), request.MaximumYMetres) ||
				!dimension(Field(value, "rows"), request.Rows) ||
				!dimension(Field(value, "columns"), request.Columns)) {
				call.ReturnValue(Map({{"status", String("invalid_collider_bev")}}));
				return;
			}
			call.ReturnValue(ColliderBev(call.World(), request).Value);
		}

		constexpr std::array<ServiceMethod, 23> DATA_SCENE_METHODS{{
			{"GetCapabilities", ServiceCapabilities},
			{"GetSceneSnapshot", ServiceSnapshot},
			{"GetCameraRenderingData", ServiceCamera},
			{"GetEditableImageMetadata", ServiceImage},
			{"GetCaptureChannels", ServiceChannels},
			{"Capture", ServiceCapture},
			{"CreateOptions", ServiceCreateOptions},
			{"CaptureBundle", ServiceCaptureBundle},
			{"PollCapture", ServicePollCapture},
			{"CancelCapture", ServiceCancelCapture},
			{"GetCaptureBuffer", ServiceCaptureBuffer},
			{"ReleaseCapture", ServiceReleaseCapture},
			{"SetCaptureDriver", ServiceSetCaptureDriver},
			{"RequestLifecycle", ServiceRequestLifecycle},
			{"PollLifecycle", ServicePollLifecycle},
			{"ReleaseLifecycle", ServiceReleaseLifecycle},
			{"GetResources", ServiceResources},
			{"SetEventNarratives", ServiceSetEventNarratives},
			{"GetEventNarratives", ServiceEventNarratives},
			{"Raycast", ServiceRaycast},
			{"OverlapAABB", ServiceAabb},
			{"OverlapOBB", ServiceObb},
			{"GetColliderBev", ServiceColliderBev},
		}};
	}

	DataSceneResult GetCapabilities(const ecs::Store &store) {
		const bool physicsPrepared = ecs::Components::Find(core::Name("physics.PhysicsWorld")).IsValid() &&
									 store.Resource<physics::PhysicsWorld>() != nullptr;
		return {
			"ok",
			Map({
				{"status", String("ok")},
				{"schema_version", String("data-scene/v1")},
				{"scene_snapshot", Boolean(true)},
				{"lighting_source_metadata", Boolean(true)},
				{"lighting_contribution", Boolean(false)},
				{"lighting_contribution_reason",
				 String("requires a selected rendered view and per-pixel evidence")},
				{"camera_metadata", Boolean(true)},
				{"camera_metadata_schema_version", String("camera-rendering-data/v1")},
				{"physics_observation_schema_version", String("physics-observation/v1")},
				{"event_narratives", Boolean(true)},
				{"event_narrative_schema_version", String("event-narrative/v1")},
				{"event_narratives_script_declared", Boolean(true)},
				{"max_event_narratives", Number(MAX_EVENT_NARRATIVES)},
				{"editable_image_rgba8", Boolean(true)},
				{"spatial_queries", Boolean(true)},
				{"spatial_query_kinds",
				 Array({String("raycast"), String("aabb_overlap"), String("obb_overlap")})},
				{"max_raycast_distance_metres", Number(100'000.0)},
				{"physics_contacts", Boolean(physicsPrepared)},
				{"physics_contact_impulses", Boolean(physicsPrepared)},
				{"physics_sleep_and_assemblies", Boolean(physicsPrepared)},
				{"physics_joints", Boolean(true)},
				{"physics_forces", Boolean(false)},
				{"physics_torques", Boolean(false)},
				{"physics_force_reason", String("engine has no persistent force accumulator")},
				{"physics_torque_reason", String("engine has no persistent torque accumulator")},
				{"controller_fields", Boolean(true)},
				{"durable_resources", Boolean(false)},
				{"render_capture", Boolean(false)},
				{"checkpoint", Boolean(false)},
			})
		};
	}

	DataSceneResult GetSceneSnapshot(ecs::Store &store, size_t limit) {
		if (limit > MAX_DATA_SCENE_ENTITIES)
			return {
				"resource_limit",
				Map({
					{"status", String("resource_limit")},
					{"limit", Number(MAX_DATA_SCENE_ENTITIES)},
				})
			};
		std::vector<std::pair<ecs::Entity, std::string>> selected;
		std::unordered_map<uint64_t, std::string> ids;
		std::unordered_set<std::string> seen;
		enum class SnapshotFault { None, Limit, IdTooLong, Duplicate };
		SnapshotFault fault = SnapshotFault::None;
		std::string duplicateId;
		size_t unlabelled = 0;
		store.Each<const ecs::InstanceName>([&](ecs::Entity entity, const ecs::InstanceName &) {
			if (fault != SnapshotFault::None) return;
			std::string id;
			if (!StableId(store, entity, id)) {
				unlabelled++;
				return;
			}
			if (id.size() > MAX_DATA_SCENE_ID_BYTES) {
				fault = SnapshotFault::IdTooLong;
				duplicateId = id;
				return;
			}
			if (selected.size() == limit) {
				fault = SnapshotFault::Limit;
				return;
			}
			if (!seen.emplace(id).second) {
				fault = SnapshotFault::Duplicate;
				duplicateId = id;
				return;
			}
			ids.emplace(entity.Id, id);
			selected.emplace_back(entity, std::move(id));
		});
		if (fault != SnapshotFault::None) {
			const char *status = fault == SnapshotFault::Limit		 ? "resource_limit"
								 : fault == SnapshotFault::IdTooLong ? "invalid_identity"
																	 : "identity_conflict";
			std::vector<std::pair<std::string, ScriptValue>> result{{"status", String(status)}};
			if (!duplicateId.empty()) result.emplace_back("id", String(duplicateId));
			return {status, Map(std::move(result))};
		}
		std::sort(selected.begin(), selected.end(), [](const auto &left, const auto &right) {
			return left.second < right.second;
		});
		const physics::PhysicsWorld *physicsWorld = nullptr;
		if (ecs::Components::Find(core::Name("physics.PhysicsWorld")).IsValid())
			physicsWorld = store.Resource<physics::PhysicsWorld>();
		std::vector<ScriptValue> entities;
		entities.reserve(selected.size());
		for (const auto &[entity, id] : selected)
			entities.push_back(EntityRecord(store, entity, id, ids, physicsWorld));
		const scene::WorldLighting lighting = scene::LightingOf(store);
		const ecs::Entity lightingService =
			scene::ServiceOf(store, ecs::Classes::Find(core::Name("Lighting")));
		const bool hasLightingService =
			store.Get<scene::LightingServiceComponent>(lightingService) != nullptr;
		const bool hasSunOverride = store.Resource<scene::Sun>() != nullptr;
		const char *const serviceSource = hasLightingService ? "lighting_service" : "engine_defaults";
		const char *const directionSource = hasSunOverride		 ? "sun_resource_override"
											: hasLightingService ? "lighting_service_solar_arc"
																 : "engine_default";
		const char *const ambientSource = hasSunOverride	   ? "sun_resource_override"
										  : hasLightingService ? "lighting_service"
															   : "engine_default";
		std::vector<ScriptValue> localLights;
		localLights.reserve(selected.size());
		size_t omittedUnidentifiedLights = 0;
		store.Each<const scene::Light>([&](ecs::Entity entity, const scene::Light &) {
			std::string ignored;
			if (!StableId(store, entity, ignored)) omittedUnidentifiedLights++;
		});
		for (const auto &[entity, id] : selected) {
			const auto *authored = store.Get<scene::Light>(entity);
			if (authored == nullptr) continue;
			scene::ResolvedLocalLight resolved;
			const scene::LocalLightRejection rejection =
				scene::ResolveLocalLight(store, entity, *authored, resolved);
			const bool resolvedDirectionAvailable =
				rejection == scene::LocalLightRejection::None && authored->Kind != scene::LightKind::Point;
			localLights.push_back(Map({
				{"id", String(id)},
				{"kind", String(LightKindName(authored->Kind))},
				{"authored_color_rgb", Colour(authored->Colour)},
				{"authored_brightness_renderer_relative", Number(authored->Brightness)},
				{"authored_range_metres", Number(authored->Range)},
				{"authored_angle_degrees", Number(authored->Angle)},
				{"authored_face", String(scene::Describe(authored->Face))},
				{"authored_enabled", Boolean(authored->Enabled)},
				{"authored_shadows_requested", Boolean(authored->Shadows)},
				{"source_stage_eligible", Boolean(rejection == scene::LocalLightRejection::None)},
				{"source_stage_rejection", String(scene::Describe(rejection))},
				{"resolved_position_world_metres",
				 rejection == scene::LocalLightRejection::None ? Vector(resolved.Position) : ScriptValue{}},
				{"resolved_direction_world",
				 resolvedDirectionAvailable ? Vector(resolved.Direction) : ScriptValue{}},
				{"resolved_direction_available", Boolean(resolvedDirectionAvailable)},
				{"resolved_direction_reason",
				 String(
					 resolvedDirectionAvailable					 ? ""
					 : authored->Kind == scene::LightKind::Point ? "point_is_omnidirectional"
																 : scene::Describe(rejection)
				 )},
				{"renderer_rgb",
				 rejection == scene::LocalLightRejection::None ? Colour(resolved.Colour) : ScriptValue{}},
				{"resolved_range_metres",
				 rejection == scene::LocalLightRejection::None ? Number(resolved.Range) : ScriptValue{}},
				{"cone_cosine",
				 rejection == scene::LocalLightRejection::None ? Number(resolved.ConeCosine) : ScriptValue{}},
			}));
		}
		const size_t identifiedLocalLightCount = localLights.size();
		const ecs::WorldTime time = store.Time();
		std::vector<std::pair<std::string, ScriptValue>> result{
			{"status", String("ok")},
			{"schema_version", String("data-scene/v1")},
			{"tick", String(Decimal(time.Tick))},
			{"elapsed_seconds", Number(time.Elapsed)},
			{"fixed_delta_seconds", Number(time.Delta)},
			{"coverage", String("explicitly_identified_subset")},
			{"unlabelled_instances", Number(unlabelled)},
			{"entities", Array(std::move(entities))},
			{"lighting_observation",
			 Map({
				 {"schema_version", String("lighting-observation/v1")},
				 {"resolved_global",
				  Map({
					  {"direction_world_towards", Vector(lighting.Direction)},
					  {"ambient_rgb", Colour(lighting.Ambient)},
					  {"outdoor_ambient_rgb", Colour(lighting.OutdoorAmbient)},
					  {"direct_rgb", Colour(lighting.Direct)},
					  {"fog_color_rgb", Colour(lighting.FogColor)},
					  {"fog_start_metres", Number(lighting.FogStart)},
					  {"fog_end_metres", Number(lighting.FogEnd)},
					  {"provenance",
					   Map({
						   {"lighting_service", String(serviceSource)},
						   {"sun_override", String(hasSunOverride ? "sun_resource_override" : "none")},
						   {"direction", String(directionSource)},
						   {"ambient", String(ambientSource)},
						   {"outdoor_ambient", String(serviceSource)},
						   {"direct", String(serviceSource)},
						   {"fog", String(serviceSource)},
					   })},
				  })},
				 {"local_lights", Array(std::move(localLights))},
				 {"local_light_coverage",
				  String("identified_source_rows_before_portal_copies_and_camera_cap")},
				 {"identified_local_light_count", Number(identifiedLocalLightCount)},
				 {"omitted_unidentified_local_light_count", Number(omittedUnidentifiedLights)},
				 {"view_selection",
				  Map({{"available", Boolean(false)}, {"reason", String("no selected view")}})},
				 {"portal_copies",
				  Map(
					  {{"available", Boolean(false)},
					   {"reason", String("view-derived portal transport is not observed")}}
				  )},
				 {"per_pixel_contribution",
				  Map(
					  {{"available", Boolean(false)}, {"reason", String("requires rendered pixel evidence")}}
				  )},
				 {"shadow_factor",
				  Map(
					  {{"available", Boolean(false)}, {"reason", String("requires rendered shadow evidence")}}
				  )},
				 {"shadow_caster",
				  Map(
					  {{"available", Boolean(false)}, {"reason", String("requires rendered shadow evidence")}}
				  )},
				 {"shadow_receiver",
				  Map(
					  {{"available", Boolean(false)}, {"reason", String("requires rendered shadow evidence")}}
				  )},
				 {"photometric_units",
				  Map(
					  {{"available", Boolean(false)},
					   {"reason", String("authored brightness is renderer-relative")}}
				  )},
			 })},
		};
		if (const auto *clock = physics::PhysicsClockOf(store); clock != nullptr) {
			result.emplace_back(
				"physics_clock",
				Map({
					{"paused", Boolean(clock->Paused)},
					{"rate_hz", Number(clock->Rate)},
					{"steps", String(Decimal(clock->Steps))},
					{"dropped_steps", String(Decimal(clock->DroppedSteps))},
				})
			);
		}
		const ObservationRecords contacts = physicsWorld == nullptr ? ObservationRecords{Array({}), 0, 0}
																	: ContactRecords(*physicsWorld, ids);
		const ObservationRecords events = physicsWorld == nullptr ? ObservationRecords{Array({}), 0, 0}
																  : ContactEventRecords(*physicsWorld, ids);
		result.emplace_back("contacts", contacts.Values);
		result.emplace_back("contact_events", events.Values);
		result.emplace_back(
			"physics_observations",
			Map({
				{"schema_version", String("physics-observation/v1")},
				{"world_prepared", Boolean(physicsWorld != nullptr)},
				{"contacts",
				 Map({
					 {"available", Boolean(physicsWorld != nullptr)},
					 {"reason", String(physicsWorld == nullptr ? "physics world is not prepared" : "")},
					 {"coverage",
					  String(
						  contacts.OmittedResourceLimit == 0
							  ? "identified_endpoints_in_explicit_subset"
							  : "identified_endpoints_in_explicit_subset_capped"
					  )},
					 {"omitted_unidentified_endpoints", Number(contacts.OmittedUnidentifiedEndpoints)},
					 {"omitted_resource_limit", Number(contacts.OmittedResourceLimit)},
				 })},
				{"contact_events",
				 Map({
					 {"available", Boolean(physicsWorld != nullptr)},
					 {"reason", String(physicsWorld == nullptr ? "physics world is not prepared" : "")},
					 {"coverage",
					  String(
						  events.OmittedResourceLimit == 0 ? "identified_endpoints_in_explicit_subset"
														   : "identified_endpoints_in_explicit_subset_capped"
					  )},
					 {"omitted_unidentified_endpoints", Number(events.OmittedUnidentifiedEndpoints)},
					 {"omitted_resource_limit", Number(events.OmittedResourceLimit)},
				 })},
				{"forces",
				 Map({
					 {"available", Boolean(false)},
					 {"reason", String("engine has no persistent force accumulator")},
				 })},
				{"torques",
				 Map({
					 {"available", Boolean(false)},
					 {"reason", String("engine has no persistent torque accumulator")},
				 })},
				{"impulses",
				 Map({
					 {"available", Boolean(physicsWorld != nullptr)},
					 {"reason", String(physicsWorld == nullptr ? "physics world is not prepared" : "")},
					 {"source", String("most_recent_completed_solver")},
					 {"units", String("N*s")},
					 {"coverage", String("identified_real_contacts_only")},
					 {"speculative_rows", String("zero_contact_impulses_only")},
					 {"skipped_manifolds", String("excluded")},
				 })},
			})
		);
		if (const auto *controller = store.Resource<scene::CameraController>(); controller != nullptr) {
			result.emplace_back(
				"camera_controller",
				Map({
					{"mode", String(CameraModeName(controller->Mode))},
					{"enabled", Boolean(controller->Enabled)},
					{"angles_radians", Array({Number(controller->Angles.X), Number(controller->Angles.Y)})},
					{"distance_metres", Number(controller->Distance)},
					{"minimum_distance_metres", Number(controller->MinimumDistance)},
					{"maximum_distance_metres", Number(controller->MaximumDistance)},
					{"occluded_distance_metres", Number(controller->OccludedDistance)},
					{"head_height_metres", Number(controller->HeadHeight)},
					{"shoulder_offset_metres", Number(controller->ShoulderOffset)},
					{"units",
					 Map({
						 {"distance", String("m")},
						 {"angle", String("rad")},
					 })},
				})
			);
		}
		if (const auto *controllers = store.Resource<scene::ControllerState>(); controllers != nullptr) {
			std::vector<ScriptValue> slots;
			for (size_t index = 0; index < scene::MAX_CONTROLLERS; index++) {
				const scene::ControllerSlot &slot = controllers->Slots[index];
				std::vector<ScriptValue> axes;
				for (float axis : slot.Axes)
					axes.push_back(Number(axis));
				slots.push_back(Map({
					{"index", Number(index)},
					{"connected", Boolean(slot.Connected)},
					{"mapped", Boolean(slot.Mapped)},
					{"buttons", Number(slot.Buttons)},
					{"pressed_buttons", Number(slot.PressedButtons)},
					{"axes", Array(std::move(axes))},
				}));
			}
			result.emplace_back("controllers", Array(std::move(slots)));
		}
		return {"ok", Map(std::move(result))};
	}

	DataSceneResult GetCameraRenderingData(ecs::Store &store, ecs::Entity entity, size_t observationLimit) {
		if (observationLimit > MAX_CAMERA_OBJECT_OBSERVATIONS)
			return {
				"resource_limit",
				Map({{"status", String("resource_limit")}, {"limit", Number(MAX_CAMERA_OBJECT_OBSERVATIONS)}})
			};
		const auto *camera = store.Get<scene::Camera>(entity);
		const auto *transform = store.Get<scene::Transform>(entity);
		if (camera == nullptr || transform == nullptr)
			return {"invalid_argument", Map({{"status", String("invalid_camera")}})};
		core::CFrame worldFromCamera = transform->Frame;
		const double rotationLength = std::hypot(
			std::hypot(static_cast<double>(worldFromCamera.QuaternionX), worldFromCamera.QuaternionY),
			std::hypot(static_cast<double>(worldFromCamera.QuaternionZ), worldFromCamera.QuaternionW)
		);
		if (!std::isfinite(worldFromCamera.Position.X) || !std::isfinite(worldFromCamera.Position.Y) ||
			!std::isfinite(worldFromCamera.Position.Z) || !std::isfinite(rotationLength) ||
			std::abs(rotationLength - 1.0) > 0.001 || !std::isfinite(camera->FieldOfViewRadians) ||
			camera->FieldOfViewRadians <= 0.0f || camera->FieldOfViewRadians >= std::numbers::pi_v<float> ||
			!std::isfinite(camera->NearPlane) || !std::isfinite(camera->FarPlane) ||
			camera->NearPlane <= 0.0f || camera->FarPlane <= camera->NearPlane ||
			((camera->ImageWidth == 0) != (camera->ImageHeight == 0))) {
			return {"invalid_argument", Map({{"status", String("invalid_camera_calibration")}})};
		}
		worldFromCamera.QuaternionX = static_cast<float>(worldFromCamera.QuaternionX / rotationLength);
		worldFromCamera.QuaternionY = static_cast<float>(worldFromCamera.QuaternionY / rotationLength);
		worldFromCamera.QuaternionZ = static_cast<float>(worldFromCamera.QuaternionZ / rotationLength);
		worldFromCamera.QuaternionW = static_cast<float>(worldFromCamera.QuaternionW / rotationLength);
		std::string id;
		if (!StableId(store, entity, id))
			return {"identity_required", Map({{"status", String("identity_required")}})};
		const bool hasRequestedResolution = camera->ImageWidth != 0;
		std::vector<std::pair<std::string, ecs::Entity>> observed;
		std::unordered_set<std::string> observedIds;
		bool duplicateObservationId = false;
		bool invalidObservationId = false;
		bool observationOverflow = false;
		if (observationLimit != 0)
			store.Each<const scene::Transform, const scene::Bounds>(
				[&](ecs::Entity candidate, const scene::Transform &, const scene::Bounds &) {
					if (duplicateObservationId || invalidObservationId || observationOverflow) return;
					std::string candidateId;
					if (!StableId(store, candidate, candidateId)) return;
					if (candidateId.size() > MAX_DATA_SCENE_ID_BYTES ||
						candidateId.find('\0') != std::string::npos || !DataSceneUtf8(candidateId)) {
						invalidObservationId = true;
						return;
					}
					if (observed.size() == observationLimit) {
						observationOverflow = true;
						return;
					}
					if (!observedIds.emplace(candidateId).second) {
						duplicateObservationId = true;
						return;
					}
					observed.emplace_back(std::move(candidateId), candidate);
				}
			);
		if (invalidObservationId) return {"invalid_identity", Map({{"status", String("invalid_identity")}})};
		if (duplicateObservationId)
			return {"identity_conflict", Map({{"status", String("identity_conflict")}})};
		if (observationOverflow)
			return {
				"resource_limit",
				Map({{"status", String("resource_limit")}, {"limit", Number(MAX_CAMERA_OBJECT_OBSERVATIONS)}})
			};
		std::sort(observed.begin(), observed.end(), [](const auto &left, const auto &right) {
			return left.first < right.first;
		});
		std::vector<ScriptValue> observations;
		observations.reserve(observed.size());
		const core::CFrame cameraFromWorld = worldFromCamera.Inverse();
		for (const auto &[objectId, objectEntity] : observed) {
			const auto *object = store.Get<scene::Transform>(objectEntity);
			const auto *bounds = store.Get<scene::Bounds>(objectEntity);
			const auto *visual = store.Get<scene::Visual>(objectEntity);
			if (!Finite(object->Frame) || !Finite(bounds->HalfExtent) || bounds->HalfExtent.X < 0.0f ||
				bounds->HalfExtent.Y < 0.0f || bounds->HalfExtent.Z < 0.0f)
				return {"invalid_spatial_data", Map({{"status", String("invalid_spatial_data")}})};
			const core::CFrame cameraFromObject = cameraFromWorld * object->Frame;
			const core::Vector3 size = bounds->HalfExtent * 2.0f;
			if (!Finite(cameraFromObject) || !Finite(size))
				return {"invalid_spatial_data", Map({{"status", String("invalid_spatial_data")}})};
			const ProjectedBounds projected = ProjectBounds(cameraFromWorld, *camera, *object, *bounds);
			if (std::string_view(projected.Reason) == "invalid_authored_spatial_data")
				return {"invalid_spatial_data", Map({{"status", String("invalid_spatial_data")}})};
			observations.push_back(Map({
				{"id", String(objectId)},
				{"camera_from_object", Frame(cameraFromObject)},
				{"size_metres", Vector(size)},
				{"authored_visible",
				 Map({
					 {"available", Boolean(visual != nullptr)},
					 {"value", visual == nullptr ? ScriptValue{} : Boolean(visual->Visible)},
					 {"reason", String(visual == nullptr ? "visual_component_unavailable" : "")},
				 })},
				{"projected_bounds",
				 Map({
					 {"available", Boolean(projected.Available)},
					 {"xyxy_pixels",
					  projected.Available ? Array(
												{Number(projected.Left),
												 Number(projected.Top),
												 Number(projected.Right),
												 Number(projected.Bottom)}
											)
										  : ScriptValue{}},
					 {"clipped_to_image", projected.Available ? Boolean(projected.Clipped) : ScriptValue{}},
					 {"source", String("projected_scene_bounds")},
					 {"reason", String(projected.Reason)},
				 })},
				{"frustum_intersection",
				 Map({
					 {"available", Boolean(hasRequestedResolution)},
					 {"value", hasRequestedResolution ? Boolean(projected.Intersects) : ScriptValue{}},
					 {"reason", String(hasRequestedResolution ? "" : "camera_has_no_explicit_image_size")},
				 })},
				{"occlusion",
				 Map({
					 {"available", Boolean(false)},
					 {"reason", String("requires_capture_visibility_evidence")},
				 })},
			}));
		}
		return {
			"ok",
			Map({
				{"status", String("ok")},
				{"id", String(id)},
				{"world_from_camera", Frame(worldFromCamera)},
				{"camera_from_world", Frame(worldFromCamera.Inverse())},
				{"vertical_fov_radians", Number(camera->FieldOfViewRadians)},
				{"near_metres", Number(camera->NearPlane)},
				{"far_metres", Number(camera->FarPlane)},
				{"requested_width", Number(camera->ImageWidth)},
				{"requested_height", Number(camera->ImageHeight)},
				{"requested_resolution_available", Boolean(hasRequestedResolution)},
				{"requested_resolution_reason",
				 String(
					 hasRequestedResolution ? "explicit_camera_size" : "host_viewport_resolves_at_capture"
				 )},
				{"projection_available", Boolean(false)},
				{"projection_reason", String("exact_projection_available_after_render_capture")},
				{"object_observations", Array(std::move(observations))},
				{"crop",
				 Map({
					 {"left", Number(0.0)},
					 {"top", Number(0.0)},
					 {"width", Number(1.0)},
					 {"height", Number(1.0)},
					 {"convention", String("normalized_full_view_left_top_width_height")},
				 })},
				{"lens_distortion",
				 Map({
					 {"available", Boolean(false)},
					 {"model", String("unavailable")},
					 {"reason", String("camera_component_has_no_lens_distortion_model")},
				 })},
				{"temporal_jitter",
				 Map({
					 {"available", Boolean(false)},
					 {"policy", String("unavailable")},
					 {"reason", String("resolved_only_by_render_pipeline")},
				 })},
				{"units",
				 Map({
					 {"world", String("metres")},
					 {"camera", String("metres")},
					 {"angle", String("radians")},
					 {"image", String("pixels")},
				 })},
				{"world_axes", String("right_handed_y_up")},
				{"camera_axes", String("x_right_y_up_negative_z_forward")},
				{"matrix_layout", String("column_major")},
				{"clip_depth_range", String("zero_to_one")},
				{"intrinsics_source", String("scene_camera_component")},
				{"extrinsics_source", String("scene_transform")},
				{"coordinate_convention",
				 String("right_handed_y_up_camera_negative_z_clip_y_up_depth_zero_to_one_column_major")},
			})
		};
	}

	DataSceneResult GetEditableImageMetadata(const ecs::Store &store, ecs::Entity entity) {
		const auto *image = store.Get<scene::EditableImage>(entity);
		if (image == nullptr)
			return {"invalid_argument", Map({{"status", String("invalid_editable_image")}})};
		if (image->Width == 0 || image->Height == 0 || image->Width > scene::MAXIMUM_EDITABLE_IMAGE_PIXELS ||
			image->Height > scene::MAXIMUM_EDITABLE_IMAGE_PIXELS ||
			static_cast<uint64_t>(image->Width) * image->Height > scene::MAXIMUM_EDITABLE_IMAGE_PIXELS) {
			return {"invalid_data", Map({{"status", String("invalid_rgba8_dimensions")}})};
		}
		const size_t expected = static_cast<size_t>(image->Width) * image->Height * 4;
		if (image->Pixels.size() != expected)
			return {"invalid_data", Map({{"status", String("invalid_rgba8_storage")}})};
		return {
			"ok",
			Map({
				{"status", String("ok")},
				{"width", Number(image->Width)},
				{"height", Number(image->Height)},
				{"byte_length", Number(expected)},
				{"format", String("rgba8")},
				{"row_order", String("top_left")},
				{"color_space", String("linear")},
				{"alpha", String("straight")},
				{"copy_semantics", String("copied")},
				{"revision", Number(image->Revision)},
			})
		};
	}

	DataSceneResult GetCaptureChannels(const ecs::Store &) {
		return CaptureUnavailable();
	}

	DataSceneResult GetCaptureChannels(const ecs::Store &, const std::shared_ptr<DataCaptureBridge> &bridge) {
		return CaptureChannels(bridge);
	}

	DataSceneResult GetResources(const ecs::Store &) {
		return {
			"unsupported",
			Map({
				{"status", String("capability_unsupported")},
				{"feature", String("durable_resources")},
				{"resources", Array({})},
				{"reason", String("this service has no durable resource owner")},
			})
		};
	}

	DataSceneResult
	GetAudioObservationCapabilities(const std::shared_ptr<DataAudioObservationBridge> &bridge) {
		if (!bridge)
			return {
				"ok",
				Map({
					{"status", String("ok")},
					{"audio_observation", Boolean(false)},
					{"audio_observation_schema_version", String(AUDIO_OBSERVATION_SCHEMA)},
					{"audio_observation_reason", String("audio observation bridge is not installed")},
				})
			};
		const DataAudioObservationBridgeCapabilities capabilities = bridge->Capabilities();
		return {
			"ok",
			Map({
				{"status", String("ok")},
				{"audio_observation", Boolean(capabilities.Available)},
				{"audio_observation_schema_version", String(AUDIO_OBSERVATION_SCHEMA)},
				{"audio_observation_maximum_frames", Number(capabilities.MaximumFrames)},
				{"audio_observation_reason", String(capabilities.Detail)},
			})
		};
	}

	DataSceneResult SetEventNarratives(ecs::Store &store, const ScriptValue &bundle) {
		ScriptValue canonical;
		DataSceneResult result = ValidateEventNarratives(bundle, canonical);
		if (std::string_view(result.Status) != "ok") return result;
		store.SetResource(EventNarratives{std::move(canonical)});
		return result;
	}

	bool CanonicalEventNarratives(const ScriptValue &bundle, ScriptValue &canonical) {
		return std::string_view(ValidateEventNarratives(bundle, canonical).Status) == "ok";
	}

	DataSceneResult GetEventNarratives(const ecs::Store &store) {
		const auto *narratives = store.Resource<EventNarratives>();
		if (narratives == nullptr)
			return {
				"unavailable",
				Map({
					{"status", String("unavailable")},
					{"schema_version", String("event-narrative/v1")},
					{"version", Number(EVENT_NARRATIVE_SCHEMA_VERSION)},
					{"records", Array({})},
					{"unavailable_reason", String("no_script_declared_narratives")},
				})
			};
		ScriptValue result = narratives->Bundle;
		ScriptValue canonical;
		if (!CanonicalEventNarratives(result, canonical))
			return {"invalid_data", Map({{"status", String("invalid_event_narratives")}})};
		result = std::move(canonical);
		result.Entries.push_back({"status", String("ok")});
		result.Entries.push_back({"schema_version", String("event-narrative/v1")});
		return {"ok", std::move(result)};
	}

	DataSceneResult Raycast(const ecs::Store &store, const DataSceneRaycastRequest &request) {
		const double length = std::hypot(
			static_cast<double>(request.Direction.X),
			static_cast<double>(request.Direction.Y),
			static_cast<double>(request.Direction.Z)
		);
		if (!std::isfinite(request.Origin.X) || !std::isfinite(request.Origin.Y) ||
			!std::isfinite(request.Origin.Z) || !std::isfinite(request.Direction.X) ||
			!std::isfinite(request.Direction.Y) || !std::isfinite(request.Direction.Z) ||
			!std::isfinite(request.MaxDistanceMetres) || !std::isfinite(length) || length <= 0.0 ||
			request.MaxDistanceMetres <= 0.0f || request.MaxDistanceMetres > 100'000.0f)
			return {"invalid_argument", Map({{"status", String("invalid_raycast_query")}})};
		const core::Vector3 direction{
			static_cast<float>(static_cast<double>(request.Direction.X) / length),
			static_cast<float>(static_cast<double>(request.Direction.Y) / length),
			static_cast<float>(static_cast<double>(request.Direction.Z) / length),
		};
		return QueryRay(
			store,
			Map(
				{{"origin", Vector(request.Origin)},
				 {"direction", Vector(direction)},
				 {"max_distance_metres", Number(request.MaxDistanceMetres)}}
			)
		);
	}

	DataSceneResult OverlapAABB(const ecs::Store &store, const DataSceneAabbRequest &request) {
		return QueryAabb(
			store, Map({{"minimum", Vector(request.Minimum)}, {"maximum", Vector(request.Maximum)}})
		);
	}

	DataSceneResult OverlapOBB(const ecs::Store &store, const DataSceneObbRequest &request) {
		core::CFrame frame = request.Frame;
		const double length = std::hypot(
			std::hypot(
				static_cast<double>(frame.QuaternionX),
				static_cast<double>(frame.QuaternionY),
				static_cast<double>(frame.QuaternionZ)
			),
			static_cast<double>(frame.QuaternionW)
		);
		if (!std::isfinite(frame.Position.X) || !std::isfinite(frame.Position.Y) ||
			!std::isfinite(frame.Position.Z) || !std::isfinite(frame.QuaternionX) ||
			!std::isfinite(frame.QuaternionY) || !std::isfinite(frame.QuaternionZ) ||
			!std::isfinite(frame.QuaternionW) || !std::isfinite(length) || length <= 0.0)
			return {"invalid_argument", Map({{"status", String("invalid_obb_query")}})};
		frame.QuaternionX = static_cast<float>(static_cast<double>(frame.QuaternionX) / length);
		frame.QuaternionY = static_cast<float>(static_cast<double>(frame.QuaternionY) / length);
		frame.QuaternionZ = static_cast<float>(static_cast<double>(frame.QuaternionZ) / length);
		frame.QuaternionW = static_cast<float>(static_cast<double>(frame.QuaternionW) / length);
		return QueryObb(store, Map({{"frame", CFrame(frame)}, {"half_extent", Vector(request.HalfExtent)}}));
	}

	DataSceneResult ColliderBev(ecs::Store &store, const DataSceneColliderBevRequest &request) {
		constexpr size_t MAXIMUM_CELLS = 64;
		if (request.Rows == 0 || request.Rows > 8 || request.Columns == 0 || request.Columns > 8 ||
			static_cast<size_t>(request.Rows) * request.Columns > MAXIMUM_CELLS ||
			!std::isfinite(request.MinimumXMetres) || !std::isfinite(request.MinimumZMetres) ||
			!std::isfinite(request.MaximumXMetres) || !std::isfinite(request.MaximumZMetres) ||
			!std::isfinite(request.MinimumYMetres) || !std::isfinite(request.MaximumYMetres) ||
			request.MinimumXMetres >= request.MaximumXMetres ||
			request.MinimumZMetres >= request.MaximumZMetres ||
			request.MinimumYMetres >= request.MaximumYMetres) {
			return {"invalid_argument", Map({{"status", String("invalid_collider_bev")}})};
		}

		const auto boundary = [](float minimum, float maximum, uint8_t index, uint8_t count) {
			if (index == 0) return minimum;
			if (index == count) return maximum;
			return static_cast<float>(
				static_cast<double>(minimum) +
				(static_cast<double>(maximum) - minimum) * static_cast<double>(index) / count
			);
		};
		const auto hasExtent = [](float minimum, float maximum) {
			return static_cast<float>((static_cast<double>(maximum) - minimum) * 0.5) > 0.0f;
		};
		if (!hasExtent(request.MinimumYMetres, request.MaximumYMetres))
			return {"invalid_argument", Map({{"status", String("invalid_collider_bev")}})};
		const size_t cellCount = static_cast<size_t>(request.Rows) * request.Columns;
		std::array<core::AABB, MAXIMUM_CELLS> probes;
		for (uint8_t row = 0; row < request.Rows; ++row) {
			const float minimumZ =
				boundary(request.MinimumZMetres, request.MaximumZMetres, row, request.Rows);
			const float maximumZ =
				boundary(request.MinimumZMetres, request.MaximumZMetres, row + 1, request.Rows);
			for (uint8_t column = 0; column < request.Columns; ++column) {
				const float minimumX =
					boundary(request.MinimumXMetres, request.MaximumXMetres, column, request.Columns);
				const float maximumX =
					boundary(request.MinimumXMetres, request.MaximumXMetres, column + 1, request.Columns);
				if (!std::isfinite(minimumX) || !std::isfinite(maximumX) || !std::isfinite(minimumZ) ||
					!std::isfinite(maximumZ) || minimumX >= maximumX || minimumZ >= maximumZ ||
					!hasExtent(minimumX, maximumX) || !hasExtent(minimumZ, maximumZ)) {
					return {"invalid_argument", Map({{"status", String("invalid_collider_bev")}})};
				}
				probes[static_cast<size_t>(row) * request.Columns + column] = {
					{minimumX, request.MinimumYMetres, minimumZ},
					{maximumX, request.MaximumYMetres, maximumZ},
				};
			}
		}

		std::unordered_map<std::string, size_t> identities;
		store.EachEntity([&](ecs::Entity entity) {
			std::string id;
			if (StableId(store, entity, id) && id.size() <= MAX_DATA_SCENE_ID_BYTES &&
				id.find('\0') == std::string::npos && DataSceneUtf8(id))
				identities[id]++;
		});

		std::array<physics::ColliderOccupancy, MAXIMUM_CELLS> occupancy;
		physics::ColliderOccupancyBatch(
			store, std::span{probes}.first(cellCount), std::span{occupancy}.first(cellCount)
		);
		const auto reason = [](physics::ColliderOccupancy::Reason value) -> const char * {
			switch (value) {
			case physics::ColliderOccupancy::Reason::None:
				return "";
			case physics::ColliderOccupancy::Reason::PhysicsUnprepared:
				return "physics_unprepared";
			case physics::ColliderOccupancy::Reason::CandidateOverflow:
				return "candidate_overflow";
			case physics::ColliderOccupancy::Reason::BakedGeometryUncertain:
				return "baked_geometry_uncertain";
			case physics::ColliderOccupancy::Reason::PhysicsStale:
				return "physics_stale";
			case physics::ColliderOccupancy::Reason::InvalidProbe:
				return "invalid_probe";
			}
			return "unknown";
		};

		std::vector<ScriptValue> cells;
		cells.reserve(cellCount);
		for (uint8_t row = 0; row < request.Rows; ++row) {
			for (uint8_t column = 0; column < request.Columns; ++column) {
				const size_t index = static_cast<size_t>(row) * request.Columns + column;
				const physics::ColliderOccupancy &answer = occupancy[index];
				// A positive broadphase witness cannot make an incomplete cell usable:
				// callers need one three-state grid where overflow and baked geometry
				// never masquerade as a settled collider-contact answer.
				const bool knownEmpty = answer.Available && answer.Complete && !answer.OverlapFound;
				const bool occupied = answer.Available && answer.Complete && answer.OverlapFound;
				const char *cellReason = answer.Why == physics::ColliderOccupancy::Reason::None
											 ? (answer.Complete ? "" : "incomplete")
											 : reason(answer.Why);
				ScriptValue witness = ScriptValue{};
				bool witnessAvailable = false;
				if (occupied && answer.WitnessAvailable) {
					std::string id;
					if (StableId(store, answer.Witness, id) && identities[id] == 1) {
						witness = String(id);
						witnessAvailable = true;
					}
				}
				cells.push_back(Map({
					{"row", Number(row)},
					{"column", Number(column)},
					{"minimum_metres",
					 Array(
						 {Number(probes[index].Minimum.X),
						  Number(probes[index].Minimum.Y),
						  Number(probes[index].Minimum.Z)}
					 )},
					{"maximum_metres",
					 Array(
						 {Number(probes[index].Maximum.X),
						  Number(probes[index].Maximum.Y),
						  Number(probes[index].Maximum.Z)}
					 )},
					{"state",
					 String(
						 occupied	  ? "occupied"
						 : knownEmpty ? "empty"
									  : "unknown"
					 )},
					{"occupied",
					 occupied	  ? Boolean(true)
					 : knownEmpty ? Boolean(false)
								  : ScriptValue{}},
					{"complete", Boolean(answer.Complete)},
					{"reason", *cellReason == '\0' ? ScriptValue{} : String(cellReason)},
					{"witness_id", std::move(witness)},
					{"witness_identity_available", Boolean(witnessAvailable)},
				}));
			}
		}
		return {
			"ok",
			Map({
				{"status", String("ok")},
				{"schema_version", String("collider-bev/v1")},
				{"row_order", String("z_major_then_x")},
				{"rows", Number(request.Rows)},
				{"columns", Number(request.Columns)},
				{"xz_bounds_metres",
				 Map({
					 {"minimum", Array({Number(request.MinimumXMetres), Number(request.MinimumZMetres)})},
					 {"maximum", Array({Number(request.MaximumXMetres), Number(request.MaximumZMetres)})},
				 })},
				{"y_minimum_metres", Number(request.MinimumYMetres)},
				{"y_maximum_metres", Number(request.MaximumYMetres)},
				{"cells", Array(std::move(cells))},
			})
		};
	}

	const ServiceSurface &DataSceneServiceSurface() {
		static const ServiceSurface SURFACE = [] {
			ServiceSurface surface;
			surface.Name = "DataSceneService";
			surface.Methods = DATA_SCENE_METHODS;
			return surface;
		}();
		return SURFACE;
	}
}
