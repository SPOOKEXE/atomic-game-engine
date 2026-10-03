#include "PixelBuilderPayload.hpp"
#include "SourceRandom.hpp"
#include "Utf8TextOps.hpp"
#include "ValuePayload.hpp"
#include "nodes/ArraySource.hpp"
#include "nodes/SourceJson.hpp"

#include <engine/core/Profiling.hpp>
#include <engine/imagegraph/PcxExpression.hpp>

#include <charconv>
#include <numbers>
#include <unordered_map>

namespace engine::imagegraph {
	namespace {
		using namespace detail;
		struct Runtime {
			const PcxExecutionContext &Context;
			Diagnostic Error;
			PcxExecutionResult Result;
			std::unordered_map<std::string, Value> Variables;
			uint64_t Work = 0, Bytes = 0;
			SourceRandom Random;
			PcxDrawBlend ActiveBlend = PcxDrawBlend::SourceNormal;
			bool Fail(std::string text, Status status = Status::InvalidValue) {
				if (Error.Code == Status::Ok) Error = {status, {}, "equation", std::move(text)};
				return false;
			}
			bool Step() {
				return ++Work <= Context.MaximumWork ||
					   Fail("PCX execution work budget exceeded", Status::LimitExceeded);
			}
			bool Admit(const Value &value) {
				if (const auto *number = std::get_if<double>(&value); number && !std::isfinite(*number))
					return Fail(
						"PCX produced a source non-finite number without a native numeric carrier",
						Status::UnsupportedExecution
					);
				const auto bytes = ValueClonePayloadBytes(value);
				if (!bytes || *bytes > Context.MaximumBytes - std::min(Bytes, Context.MaximumBytes))
					return Fail("PCX value allocation budget exceeded", Status::LimitExceeded);
				Bytes += *bytes;
				return true;
			}
			static bool Numeric(const Value &value) {
				return std::holds_alternative<double>(value) || std::holds_alternative<int64_t>(value) ||
					   std::holds_alternative<bool>(value) || std::holds_alternative<EnumValue>(value) ||
					   std::holds_alternative<Colour>(value);
			}
			// GML stores colours and enum selections as integers, while native sockets retain typed carriers.
			static Value SourceNumber(const Value &value) {
				if (const auto *v = std::get_if<EnumValue>(&value)) return v->Value;
				if (const auto *v = std::get_if<Colour>(&value))
					return int64_t(
						uint32_t(v->Red) | (uint32_t(v->Green) << 8) | (uint32_t(v->Blue) << 16) |
						(uint32_t(v->Alpha) << 24)
					);
				return value;
			}
			static double Number(const Value &value) {
				if (const auto *v = std::get_if<double>(&value)) return *v;
				if (const auto *v = std::get_if<int64_t>(&value)) return double(*v);
				if (const auto *v = std::get_if<bool>(&value)) return double(*v);
				if (std::holds_alternative<EnumValue>(value) || std::holds_alternative<Colour>(value))
					return double(std::get<int64_t>(SourceNumber(value)));
				return 0;
			}
			// Source vector and quaternion values are GML arrays. Projection is local to each VM
			// operation, so socket types remain intact and transient copies obey the VM byte limit.
			const ArrayValue *AsArray(const Value &value, std::optional<Value> &projection) {
				if (const auto *array = std::get_if<ArrayValue>(&value)) return array;
				ArrayValue array{ValueType::Scalar, {}};
				if (const auto *v = std::get_if<Vector2>(&value))
					array.Elements = {v->X, v->Y};
				else if (const auto *v = std::get_if<Vector3>(&value))
					array.Elements = {v->X, v->Y, v->Z};
				else if (const auto *v = std::get_if<Vector4>(&value))
					array.Elements = {v->X, v->Y, v->Z, v->W};
				else if (const auto *v = std::get_if<Quaternion>(&value))
					array.Elements = {v->X, v->Y, v->Z, v->W};
				else
					return nullptr;
				projection = std::move(array);
				if (!Admit(*projection)) return nullptr;
				return &std::get<ArrayValue>(*projection);
			}

			std::string String(const Value &value, size_t depth = 1) {
				if (depth > 64) {
					Fail("PCX string conversion depth exceeded", Status::LimitExceeded);
					return {};
				}
				if (const auto *text = std::get_if<std::string>(&value)) return *text;
				if (Numeric(value)) {
					char text[512];
					const auto numeric = SourceNumber(value);
					const auto *integer = std::get_if<int64_t>(&numeric);
					const double number = Number(value);
					const auto converted = integer ? std::to_chars(text, text + sizeof(text), *integer)
												   : std::to_chars(
														 text,
														 text + sizeof(text),
														 number,
														 std::chars_format::fixed,
														 std::floor(number) == number ? 0 : 2
													 );
					if (converted.ec != std::errc{}) {
						Fail("PCX numeric string exceeds its conversion bound", Status::LimitExceeded);
						return {};
					}
					return std::string(text, converted.ptr);
				}
				if (std::holds_alternative<UndefinedValue>(value)) return "undefined";
				std::optional<Value> projection;
				if (const auto *array = AsArray(value, projection)) {
					std::string text = "[";
					const auto items = source_array::FromValues(*array);
					for (size_t i = 0; i < items.size(); ++i) {
						if (i) text += ", ";
						text += String(Item(items[i]), depth + 1);
						if (text.size() > Limits::MaximumTextBytes) {
							Fail("PCX string exceeds text budget", Status::LimitExceeded);
							return {};
						}
					}
					return text + "]";
				}
				Fail("PCX string conversion received an unsupported value");
				return {};
			}
			static Value Item(const SourceArrayItem &entry) {
				return std::visit(
					[](const auto &child) -> Value {
						using T = std::decay_t<decltype(child)>;
						if constexpr (std::is_same_v<T, ElementValue>)
							return std::visit([](const auto &leaf) -> Value { return leaf; }, child);
						else if constexpr (std::is_same_v<T, Image>)
							return SurfaceValue{child};
						else {
							ArrayValue array{ValueType::Any, {}};
							array.Items = child;
							return array;
						}
					},
					entry.Data
				);
			}
			static SourceArrayItem ArrayItem(Value value) {
				if (auto *array = std::get_if<ArrayValue>(&value))
					return {source_array::FromValues(std::move(*array))};
				return {*ArrayElement(std::move(value))};
			}
			Value Index(const Value &value, double index) {
				if (!std::isfinite(index) || std::abs(index) > double(Limits::MaximumArrayElements))
					return double{0};
				int64_t position = int64_t(index);
				std::optional<Value> projection;
				if (const auto *array = AsArray(value, projection)) {
					const auto items = source_array::FromValues(*array);
					if (position < 0) position += int64_t(items.size());
					return position >= 0 && uint64_t(position) < items.size() ? Item(items[size_t(position)])
																			  : Value{double{0}};
				}
				if (const auto *text = std::get_if<std::string>(&value)) {
					size_t size = 0;
					if (CountText(*text, size) != TextOpStatus::Ok) {
						Fail("PCX string indexing requires UTF-8");
						return std::string{};
					}
					if (position < 0) position += int64_t(size);
					if (position < 0 || uint64_t(position) >= size) return std::string{};
					const auto start = ByteOffset(*text, size_t(position)),
							   end = ByteOffset(*text, size_t(position) + 1);
					return text->substr(start, end - start);
				}
				return double{0};
			}
			static uint64_t Bits(double value) {
				if (!std::isfinite(value) || std::abs(value) >= 0x1p63) return 0;
				return uint64_t(int64_t(value));
			}
			// Authored rounding is independent of the host thread's floating-point rounding mode.
			static double RoundEven(double value) {
				if (!std::isfinite(value) || std::abs(value) >= 0x1p52) return value;
				const double lower = std::floor(value), fraction = value - lower;
				if (fraction < .5) return lower;
				if (fraction > .5) return lower + 1;
				return std::fmod(lower, 2.) == 0 ? lower : lower + 1;
			}
			Value BitNot(const Value &value, size_t depth = 1) {
				if (depth > 64 || !Step()) {
					Fail("PCX unary nesting exceeded", Status::LimitExceeded);
					return double{0};
				}
				std::optional<Value> projection;
				const auto *array = AsArray(value, projection);
				if (Error.Code != Status::Ok) return double{0};
				if (!array) return double(int64_t(~Bits(Number(value))));
				const auto items = source_array::FromValues(*array);
				if (items.size() > Context.MaximumBytes / sizeof(SourceArrayItem)) {
					Fail("PCX unary array exceeds byte budget", Status::LimitExceeded);
					return double{0};
				}
				ArrayValue output{ValueType::Any, {}};
				output.Items.reserve(items.size());
				for (const auto &item : items) {
					if (Error.Code != Status::Ok) break;
					output.Items.push_back(ArrayItem(BitNot(Item(item), depth + 1)));
				}
				return output;
			}
			Value Binary(std::string_view op, const Value &a, const Value &b, size_t depth = 1) {
				if (depth > 64 || !Step()) {
					Fail("PCX arithmetic nesting exceeded", Status::LimitExceeded);
					return double{0};
				}
				std::optional<Value> projectedA, projectedB;
				const auto *aa = AsArray(a, projectedA), *bb = AsArray(b, projectedB);
				if (Error.Code != Status::Ok) return double{0};
				if (aa || bb) {
					const auto x = aa ? source_array::FromValues(*aa) : source_array::Items{},
							   y = bb ? source_array::FromValues(*bb) : source_array::Items{};
					const size_t count = std::max(x.size(), y.size());
					if (count > Context.MaximumBytes / sizeof(SourceArrayItem)) {
						Fail("PCX array arithmetic exceeds byte budget", Status::LimitExceeded);
						return double{0};
					}
					ArrayValue output{ValueType::Any, {}};
					output.Items.reserve(count);
					for (size_t i = 0; i < count && Error.Code == Status::Ok; ++i)
						output.Items.push_back(ArrayItem(Binary(
							op,
							aa ? (i < x.size() ? Item(x[i]) : Value{double{0}}) : a,
							bb ? (i < y.size() ? Item(y[i]) : Value{double{0}}) : b,
							depth + 1
						)));
					return output;
				}
				if ((op == "+" || op == "+=") &&
					(std::holds_alternative<std::string>(a) || std::holds_alternative<std::string>(b))) {
					auto x = String(a), y = String(b);
					if (y.size() > Limits::MaximumTextBytes - std::min(x.size(), Limits::MaximumTextBytes)) {
						Fail("PCX concatenation exceeds text budget", Status::LimitExceeded);
						return std::string{};
					}
					return x + y;
				}
				if (!Numeric(a) || !Numeric(b))
					return op == ".." ? Value{ArrayValue{ValueType::Scalar, {}}} : Value{double{0}};
				const double x = Number(a), y = Number(b);
				if (op == "+" || op == "+=") return x + y;
				if (op == "-" || op == "-=" || op == "negsub") return x - y;
				if (op == "*" || op == "*=") return x * y;
				if (op == "/" || op == "/=") return y ? x / y : 0.;
				if (op == "%") return y ? std::fmod(x, y) : 0.;
				if (op == "**" || op == "$") return std::pow(x, y);
				if (op == "&") return double(int64_t(Bits(x) & Bits(y)));
				if (op == "|") return double(int64_t(Bits(x) | Bits(y)));
				if (op == "^") return double(int64_t(Bits(x) ^ Bits(y)));
				if (op == "<<") return double(int64_t(Bits(x) << (Bits(y) & 63)));
				if (op == ">>") return double(int64_t(Bits(x)) >> (Bits(y) & 63));
				if (op == "==") return x == y;
				if (op == "!=" || op == "<>") return x != y;
				if (op == "<") return x < y;
				if (op == ">") return x > y;
				if (op == "<=") return x <= y;
				if (op == ">=") return x >= y;
				if (op == "..") return Range(std::floor(std::abs(y - x)) + 1, x, y < x ? -1 : 1);
				Fail("unknown PCX binary operator");
				return double{0};
			}
			Value Range(double count, double start, double step) {
				if (!std::isfinite(count) || count < 0 || count > Limits::MaximumArrayElements ||
					count > Context.MaximumBytes / sizeof(ElementValue)) {
					Fail("PCX range exceeds element or byte budget", Status::LimitExceeded);
					return double{0};
				}
				ArrayValue array{ValueType::Scalar, {}};
				array.Elements.reserve(size_t(count));
				for (size_t i = 0; i < size_t(count); ++i) {
					if (!Step()) break;
					array.Elements.push_back(start + double(i) * step);
				}
				return array;
			}
			double Unit() {
				return Random.Range(0, 1);
			}
			Value Function(std::string_view name, const std::vector<Value> &a) {
				const auto n = [&](size_t i, double fallback = 0) {
					return i < a.size() ? Number(a[i]) : fallback;
				};
				const auto v = [&](size_t i) -> const Value & {
					static const Value zero{double{0}};
					return i < a.size() ? a[i] : zero;
				};
				if (a.size() > 16) {
					Fail("PCX function exceeds source argument count");
					return double{0};
				}
				if (name == "abs") return std::abs(n(0));
				if (name == "round") return RoundEven(n(0));
				if (name == "ceil") return std::ceil(n(0));
				if (name == "floor") return std::floor(n(0));
				if (name == "fract") return n(0) - std::trunc(n(0));
				if (name == "sign") return double((n(0) > 0) - (n(0) < 0));
				if (name == "min") return std::min(n(0), n(1));
				if (name == "max") return std::max(n(0), n(1));
				if (name == "clamp") return std::max(n(1), std::min(n(2, 1), n(0)));
				if (name == "lerp") return n(0) + (n(1) - n(0)) * n(2);
				if (name == "sin") return std::sin(n(0));
				if (name == "cos") return std::cos(n(0));
				if (name == "tan") return std::tan(n(0));
				if (name == "dsin") return std::sin(n(0) * std::numbers::pi / 180);
				if (name == "dcos") return std::cos(n(0) * std::numbers::pi / 180);
				if (name == "dtan") return std::tan(n(0) * std::numbers::pi / 180);
				if (name == "arcsin") return std::asin(n(0));
				if (name == "arccos") return std::acos(n(0));
				if (name == "arctan") return std::atan(n(0));
				if (name == "arctan2") return std::atan2(n(0), n(1));
				if (name == "darcsin") return std::asin(n(0)) * 180 / std::numbers::pi;
				if (name == "darccos") return std::acos(n(0)) * 180 / std::numbers::pi;
				if (name == "darctan") return std::atan(n(0)) * 180 / std::numbers::pi;
				if (name == "darctan2") return std::atan2(n(0), n(1)) * 180 / std::numbers::pi;
				if (name == "random") return Random.Range(n(0), n(1, 1));
				if (name == "irandom") {
					const auto lo = std::ceil(std::min(n(0), n(1, 1))),
							   hi = std::floor(std::max(n(0), n(1, 1)));
					return lo + std::floor(Unit() * (hi - lo + 1));
				}
				if (name == "wiggle") {
					const double octaves = n(2, 1), frequency = n(1);
					if (octaves < 1 || octaves > 32 || frequency == 0) {
						Fail("PCX wiggle octave/frequency is invalid");
						return double{0};
					}
					double amp = std::pow(2., octaves - 1) / (std::pow(2., octaves) - 1), value = 0,
						   time = n(0),
						   scale = (Context.Timeline ? double(Context.Timeline->Frames) : 1) / frequency;
					for (size_t i = 0; i < size_t(octaves); ++i) {
						const double pos = n(3) + time * scale, integral = std::floor(pos),
									 fraction = pos - std::trunc(pos);
						Random = SourceRandom(uint32_t(Bits(integral)));
						const auto first = Unit(), second = Unit();
						value += (first + (second - first) * fraction * fraction * (3 - 2 * fraction)) * amp;
						amp *= .5;
						time *= 2;
					}
					return value;
				}
				if (name == "string") return String(v(0));
				if (name == "number") {
					if (Numeric(v(0))) return SourceNumber(v(0));
					const auto *text = std::get_if<std::string>(&v(0));
					if (!text) return double{0};
					double x = 0;
					auto view = std::string_view(*text);
					while (!view.empty() && (view.front() == ' ' || view.front() == '\t'))
						view.remove_prefix(1);
					while (!view.empty() && (view.back() == ' ' || view.back() == '\t'))
						view.remove_suffix(1);
					if (!view.empty() && view.front() == '+') view.remove_prefix(1);
					const auto parsed = std::from_chars(view.data(), view.data() + view.size(), x);
					return parsed.ec == std::errc{} && parsed.ptr == view.data() + view.size() &&
								   std::isfinite(x)
							   ? x
							   : 0.;
				}
				if (name == "chr") {
					const double code = n(0);
					if (code < 0 || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) {
						Fail("PCX chr code point is invalid");
						return std::string{};
					}
					std::string result;
					JsonCursor::Utf8(result, uint32_t(code));
					return result;
				}
				if (name == "ord") {
					const auto *text = std::get_if<std::string>(&v(0));
					if (!text || text->empty()) return double{0};
					size_t characters = 0;
					if (CountText(*text, characters) != TextOpStatus::Ok) {
						Fail("PCX ord requires UTF-8");
						return double{0};
					}
					uint32_t code = uint8_t((*text)[0]);
					if (code < 128) return double(code);
					const size_t bytes = ByteOffset(*text, 1);
					code &= bytes == 2 ? 31 : bytes == 3 ? 15 : 7;
					for (size_t i = 1; i < bytes; ++i)
						code = (code << 6) | (uint8_t((*text)[i]) & 63);
					return double(code);
				}
				if (name == "range") return Range(n(0), n(1), n(2, 1));
				if (name == "length") {
					std::optional<Value> projection;
					if (const auto *array = AsArray(v(0), projection))
						return double(source_array::FromValues(*array).size());
					if (const auto *text = std::get_if<std::string>(&v(0))) {
						size_t characters = 0;
						if (CountText(*text, characters) != TextOpStatus::Ok)
							Fail("PCX length requires UTF-8");
						return double(characters);
					}
					return double{0};
				}
				if (name.starts_with("surface_get_")) {
					const auto *surface = std::get_if<SurfaceValue>(&v(0));
					const auto *builder = std::get_if<DynamicSurfaceValue>(&v(0));
					const auto dimension = builder && builder->Data
											   ? builder->Data->BaseDimension
											   : Vector2{
													 surface ? double(surface->Data.Width) : 0.,
													 surface ? double(surface->Data.Height) : 0.
												 };
					if (name == "surface_get_dimension") return dimension;
					if (name == "surface_get_width") return dimension.X;
					if (name == "surface_get_height") return dimension.Y;
				}
				if (name == "color_hex") {
					const auto text = String(v(0));
					size_t characters = 0;
					if (CountText(text, characters) != TextOpStatus::Ok) {
						Fail("PCX hex colour requires UTF-8");
						return double{0};
					}
					if (characters < 6) return double{0};
					const auto pair = [&](size_t offset) {
						int value = 0;
						for (size_t i = offset; i < offset + 2; ++i) {
							const auto character = text.substr(
								ByteOffset(text, i), ByteOffset(text, i + 1) - ByteOffset(text, i)
							);
							int digit = -1;
							if (character.size() == 1) {
								char c = character[0];
								if (c >= 'a' && c <= 'f') c -= 'a' - 'A';
								if (c >= '0' && c <= '9')
									digit = c - '0';
								else if (c >= 'A' && c <= 'F')
									digit = c - 'A' + 10;
							}
							value = value * 16 + digit;
						}
						return value;
					};
					const int64_t red = pair(0), green = pair(2), blue = pair(4);
					if (characters >= 8)
						return double(red + green * 256 + blue * 65536 + int64_t(pair(6)) * 16777216);
					return double(
						std::clamp<int64_t>(red, 0, 255) + std::clamp<int64_t>(green, 0, 255) * 256 +
						std::clamp<int64_t>(blue, 0, 255) * 65536
					);
				}
				if (name == "color_rgb" || name == "color_hsv") {
					double r = n(0), g = n(1), b = n(2);
					if (name == "color_hsv") {
						double h = std::fmod(r / 255, 1.);
						if (h < 0) h += 1;
						const double s = std::clamp(g / 255, 0., 1.), value = std::clamp(b / 255, 0., 1.),
									 f = h * 6 - std::floor(h * 6), p = value * (1 - s),
									 q = value * (1 - f * s), t = value * (1 - (1 - f) * s);
						switch (int(h * 6)) {
						case 0:
							r = value;
							g = t;
							b = p;
							break;
						case 1:
							r = q;
							g = value;
							b = p;
							break;
						case 2:
							r = p;
							g = value;
							b = t;
							break;
						case 3:
							r = p;
							g = q;
							b = value;
							break;
						case 4:
							r = t;
							g = p;
							b = value;
							break;
						default:
							r = value;
							g = p;
							b = q;
							break;
						}
						r *= 255;
						g *= 255;
						b *= 255;
					}
					return double(
						uint32_t(RoundEven(std::clamp(r, 0., 255.))) |
						(uint32_t(RoundEven(std::clamp(g, 0., 255.))) << 8) |
						(uint32_t(RoundEven(std::clamp(b, 0., 255.))) << 16)
					);
				}
				if (name == "print") {
					const auto text = String(v(0));
					if (text.size() > 65536 || Result.Messages.size() >= 256) {
						Fail("PCX print budget exceeded", Status::LimitExceeded);
						return double{0};
					}
					if (!Admit(Value{text})) return double{0};
					Result.Messages.push_back({text, n(1) != 0});
					return double{0};
				}
				if (name == "draw") {
					const auto *surface = std::get_if<SurfaceValue>(&v(0));
					SurfaceValue rendered;
					const auto *builder = std::get_if<DynamicSurfaceValue>(&v(0));
					double scaleX = n(3, 1), scaleY = n(4, 1);
					const bool nineSlice = builder && builder->Data && builder->Data->NineSlice;
					if (nineSlice && Context.Request.RequireSourceGpuRasterCoverage) {
						Error = {
							Status::UnsupportedExecution,
							builder->Data->OwnerNodeId,
							"dyna_surf",
							"Nine Slice exact GPU draw coverage requires a licensed renderer observation"
						};
						return false;
					}
					const auto color = Bits(n(6, 0xffffff));
					const double alpha = std::clamp(n(7, 1), 0., 1.);
					if (builder && builder->Data) {
						const Vector2 requested{
							builder->Data->BaseDimension.X * scaleX, builder->Data->BaseDimension.Y * scaleY
						};
						Diagnostic diagnostic;
						const auto status =
							nineSlice ? RasterizeSourceNineSlice(
											*builder,
											requested,
											{double(color & 255) / 255,
											 double((color >> 8) & 255) / 255,
											 double((color >> 16) & 255) / 255,
											 alpha},
											rendered.Data,
											diagnostic,
											Context.MaximumBytes - std::min(Bytes, Context.MaximumBytes)
										)
									  : RasterizePixelBuilder(
											*builder,
											requested,
											rendered.Data,
											diagnostic,
											Context.MaximumBytes,
											Context.Request.RigidProvider
										);
						if (status != Status::Ok) {
							Error = std::move(diagnostic);
							return false;
						}
						if (nineSlice) ActiveBlend = PcxDrawBlend::SourceNormal;
						surface = &rendered;
						scaleX = 1;
						scaleY = 1;
					}
					if (!Context.Target) {
						Fail("PCX draw requires an explicit owned target", Status::UnsupportedExecution);
						return false;
					}
					if (!surface) return true;
					const auto &source = surface->Data;
					auto &target = *Context.Target;
					const double sx = scaleX, sy = scaleY, angle = n(5) * std::numbers::pi / 180;
					if (sx == 0 || sy == 0) return true;
					const double cosine = std::cos(angle), sine = std::sin(angle);
					for (uint32_t y = 0; y < target.Height && Error.Code == Status::Ok; ++y)
						for (uint32_t x = 0; x < target.Width; ++x) {
							if (!Step()) break;
							const double dx = double(x) + .5 - n(1), dy = double(y) + .5 - n(2),
										 u = (cosine * dx - sine * dy) / sx,
										 w = (sine * dx + cosine * dy) / sy;
							if (u < 0 || w < 0 || u >= source.Width || w >= source.Height) continue;
							SurfacePixel p{}, previous{};
							LoadSurfacePixel(source, uint32_t(u), uint32_t(w), p);
							if (DescribeSurfaceFormat(source.Format)->Channels == 1)
								p = {p[0], p[0], p[0], 1};
							LoadSurfacePixel(target, x, y, previous);
							if (!nineSlice) {
								p[0] *= double(color & 255) / 255;
								p[1] *= double((color >> 8) & 255) / 255;
								p[2] *= double((color >> 16) & 255) / 255;
								p[3] *= alpha;
							}
							const double sourceAlpha = p[3];
							if (ActiveBlend == PcxDrawBlend::SourceNormal) {
								for (size_t c = 0; c < 4; ++c)
									p[c] = p[c] * sourceAlpha + previous[c] * (1 - sourceAlpha);
							} else if (ActiveBlend == PcxDrawBlend::SourceAlphaAdd) {
								for (size_t c = 0; c < 3; ++c)
									p[c] = p[c] + previous[c] * (1 - sourceAlpha);
								p[3] += previous[3];
							}
							StoreSurfacePixel(target, x, y, p);
						}
					return true;
				}
				Fail("PCX function name is unknown: " + std::string(name));
				return double{0};
			}
			Value Name(std::string_view name, size_t depth) {
				std::string canonical;
				if (name.starts_with("Project.") || name.starts_with("Program.") ||
					name.starts_with("Device.")) {
					const auto second = name.find('.', name.find('.') + 1);
					if (second != std::string_view::npos) {
						canonical = name.substr(0, second);
						name = canonical;
					}
				}
				if (auto it = Variables.find(std::string(name)); it != Variables.end()) {
					if (const auto *tree = std::get_if<PcxExpressionValue>(&it->second)) {
						if (!Admit(*tree)) return double{0};
						const auto nested = *tree;
						for (const auto &[key, value] : nested.Data->Bindings) {
							if (!Admit(value)) return double{0};
							Variables.insert_or_assign(key, value);
						}
						return Eval(nested, nested.Data->Root, depth + 1);
					}
					return it->second;
				}
				if (name.starts_with("self.")) {
					const auto found = Variables.find("node_values");
					const auto *structure =
						found == Variables.end() ? nullptr : std::get_if<StructValue>(&found->second);
					const auto end = name.find('.', 5);
					const auto key = name.substr(5, end == std::string_view::npos ? end : end - 5);
					if (structure && structure->Data)
						for (const auto &[field, value] : structure->Data->Fields)
							if (field == key) return value;
					return UndefinedValue{};
				}
				const double frame = (double(Context.Request.Tick) + Context.Request.Subframe) *
									 (Context.Request.NegativeFrame ? -1 : 1),
							 frames = Context.Timeline ? double(Context.Timeline->Frames) : 1,
							 fps = Context.Timeline ? Context.Timeline->FramesPerSecond : 30;
				if (name == "Project.frame") return frame;
				if (name == "Project.frameTotal") return frames;
				if (name == "Project.FPS") return fps;
				if (name == "Project.time") return frame / fps;
				if (name == "Project.progress") {
					if (frames <= 1) {
						Fail(
							"PCX Project.progress divides by zero for a one-frame project",
							Status::UnsupportedExecution
						);
						return double{0};
					}
					return frame / (frames - 1);
				}
				if (name == "Project.name")
					return std::string(
						Context.ProjectName.empty() ? Context.Request.ProjectName : Context.ProjectName
					);
				if (name == "Project.dimension") return Context.ProjectDimension;
				if (Context.Names) {
					Value resolved;
					Diagnostic diagnostic;
					if (Context.Names->Resolve(name, resolved, diagnostic)) {
						if (!diagnostic.Message.empty()) {
							if (Result.Messages.size() >= 256 || diagnostic.Message.size() > 65536) {
								Fail("PCX warning budget exceeded", Status::LimitExceeded);
								return UndefinedValue{};
							}
							Result.Messages.push_back({std::move(diagnostic.Message), true});
						}
						return resolved;
					}
					if (diagnostic.Code != Status::Ok) {
						Error = std::move(diagnostic);
						return UndefinedValue{};
					}
				}
				constexpr std::string_view observations[]{
					"Program.time",
					"Device.timeSecond",
					"Device.timeMinute",
					"Device.timeHour",
					"Device.timeDay",
					"Device.timeDayInWeek",
					"Device.timeMonth",
					"Device.timeYear"
				};
				if ((name.starts_with("Project.") || name.starts_with("Program.") ||
					 name.starts_with("Device.")) &&
					std::find(std::begin(observations), std::end(observations), name) ==
						std::end(observations)) {
					if (Result.Messages.size() >= 256) {
						Fail("PCX warning count exceeds bound", Status::LimitExceeded);
						return double{0};
					}
					Result.Messages.push_back(
						{"Variable " + std::string(name.substr(name.find('.') + 1)) + " not found.", true}
					);
					return double{0};
				}
				if (name.starts_with("Device.") || name.starts_with("Program.") ||
					name.find('.') != std::string_view::npos) {
					Fail(
						"PCX named observation requires an explicit parameter: " + std::string(name),
						Status::UnsupportedExecution
					);
					return UndefinedValue{};
				}
				return std::string(name);
			}
			bool Assign(const PcxExpressionValue &expression, uint32_t index, Value value, size_t depth) {
				if (Error.Code != Status::Ok) return false;
				const auto &node = expression.Data->Instructions[index];
				if (node.Operation == "name") {
					const auto *name = std::get_if<std::string>(&node.Literal);
					if (!name) return Fail("PCX assignment name is invalid");
					Variables.insert_or_assign(*name, std::move(value));
					return true;
				}
				if (node.Operation == "index" && node.Arguments.size() == 2) {
					uint32_t ownerIndex = node.Arguments[0];
					Eval(expression, ownerIndex, depth + 1);
					if (Error.Code != Status::Ok) return false;
					while (expression.Data->Instructions[ownerIndex].Operation == "=" ||
						   expression.Data->Instructions[ownerIndex].Operation == "default") {
						if (expression.Data->Instructions[ownerIndex].Arguments.size() != 2)
							return Fail("PCX array assignment owner is invalid");
						ownerIndex = expression.Data->Instructions[ownerIndex].Arguments[0];
					}
					const auto &owner = expression.Data->Instructions[ownerIndex];
					if (owner.Operation != "name" || !std::holds_alternative<std::string>(owner.Literal))
						return Fail("PCX array assignment needs a named array");
					auto [stored, inserted] =
						Variables.try_emplace(std::get<std::string>(owner.Literal), UndefinedValue{});
					(void)inserted;
					auto &storage = stored->second;
					std::optional<Value> projection;
					const auto *sourceArray = AsArray(storage, projection);
					if (Error.Code != Status::Ok) return false;
					if (projection) storage = std::move(*projection);
					auto *array = sourceArray ? std::get_if<ArrayValue>(&storage) : nullptr;
					if (!array) return true;
					const auto indexValue = Eval(expression, node.Arguments[1], depth + 1);
					if (!Numeric(indexValue) || Number(indexValue) < 0) return true;
					const double raw = Number(indexValue);
					if (raw >= Limits::MaximumArrayElements)
						return Fail("PCX array assignment index exceeds budget", Status::LimitExceeded);
					auto items = source_array::FromValues(*array);
					size_t position = size_t(raw);
					if (position >= Context.MaximumBytes / sizeof(SourceArrayItem))
						return Fail("PCX array assignment exceeds bytes", Status::LimitExceeded);
					while (items.size() <= position)
						items.push_back({ElementValue{double{0}}});
					items[position] = ArrayItem(std::move(value));
					*array = ArrayValue{ValueType::Any, {}};
					array->Items = std::move(items);
					return true;
				}
				return Fail("PCX assignment target is not a variable");
			}
			Value Eval(const PcxExpressionValue &expression, uint32_t index, size_t depth = 1) {
				if (Error.Code != Status::Ok) return double{0};
				if (depth > 128 || !Step()) {
					Fail("PCX evaluation nesting exceeds budget", Status::LimitExceeded);
					return double{0};
				}
				const auto &node = expression.Data->Instructions[index];
				Value result = double{0};
				if (node.Operation == "literal")
					result = node.Literal;
				else if (node.Operation == "name") {
					const auto *name = std::get_if<std::string>(&node.Literal);
					if (!name) {
						Fail("PCX instruction name is invalid");
						return result;
					}
					result = Name(*name, depth);
				} else if (node.Operation == "default") {
					if (node.Arguments.size() != 2) {
						Fail("PCX default needs name and value");
						return result;
					}
					const auto &target = expression.Data->Instructions[node.Arguments[0]];
					if (target.Operation != "name" || !std::holds_alternative<std::string>(target.Literal)) {
						Fail("PCX default target must be a name");
						return result;
					}
					auto fallback = Eval(expression, node.Arguments[1], depth + 1);
					const auto &key = std::get<std::string>(target.Literal);
					if (!Variables.contains(key)) Variables.emplace(key, std::move(fallback));
					result = Variables.at(key);
				} else if (node.Operation == "sequence") {
					for (auto child : node.Arguments) {
						result = Eval(expression, child, depth + 1);
						if (Error.Code != Status::Ok) break;
					}
				} else if (node.Operation == "if") {
					if (node.Arguments.size() != 3) {
						Fail("PCX if needs three tree operands");
						return result;
					}
					result = Eval(
						expression,
						node.Arguments[Number(Eval(expression, node.Arguments[0], depth + 1)) ? 1 : 2],
						depth + 1
					);
				} else if (node.Operation == "for_each") {
					if (node.Arguments.size() != 2 || !std::holds_alternative<std::string>(node.Literal)) {
						Fail("PCX foreach needs array and body");
						return result;
					}
					const auto source = Eval(expression, node.Arguments[0], depth + 1);
					std::optional<Value> projection;
					const auto *array = AsArray(source, projection);
					if (Error.Code != Status::Ok) return result;
					if (!array) {
						Fail("PCX foreach requires an array");
						return result;
					}
					const auto &binding = std::get<std::string>(node.Literal);
					const auto separator = binding.find('\n');
					if (separator == std::string::npos) {
						Fail("PCX foreach binding is invalid");
						return result;
					}
					const auto indexName = binding.substr(0, separator),
							   valueName = binding.substr(separator + 1);
					const auto items = source_array::FromValues(*array);
					for (size_t i = 0; i < items.size() && Error.Code == Status::Ok; ++i) {
						if (!indexName.empty()) Variables.insert_or_assign(indexName, double(i));
						Variables.insert_or_assign(valueName, Item(items[i]));
						result = Eval(expression, node.Arguments[1], depth + 1);
					}
				} else if (node.Operation == "for") {
					if (node.Arguments.size() != 4) {
						Fail("PCX for needs init test increment action");
						return result;
					}
					Eval(expression, node.Arguments[0], depth + 1);
					while (Error.Code == Status::Ok &&
						   Number(Eval(expression, node.Arguments[1], depth + 1))) {
						result = Eval(expression, node.Arguments[3], depth + 1);
						Eval(expression, node.Arguments[2], depth + 1);
					}
				} else if (node.Operation == "array") {
					ArrayValue array{ValueType::Any, {}};
					for (auto child : node.Arguments) {
						array.Items.push_back(ArrayItem(Eval(expression, child, depth + 1)));
						if (Error.Code != Status::Ok) break;
					}
					result = std::move(array);
				} else if (node.Operation.starts_with("call:")) {
					std::vector<Value> arguments;
					arguments.reserve(node.Arguments.size());
					for (auto child : node.Arguments) {
						arguments.push_back(Eval(expression, child, depth + 1));
						if (Error.Code != Status::Ok) break;
					}
					if (Error.Code == Status::Ok)
						result = Function(std::string_view(node.Operation).substr(5), arguments);
				} else if (node.Operation == "neg" || node.Operation == "~") {
					if (node.Arguments.size() != 1) {
						Fail("PCX unary needs one operand");
						return result;
					}
					auto value = Eval(expression, node.Arguments[0], depth + 1);
					result = node.Operation == "neg" ? Binary("-", double{0}, value) : BitNot(value);
				} else {
					if (node.Arguments.size() != 2) {
						Fail("PCX binary needs two operands");
						return result;
					}
					auto a = node.Operation == "=" ? Value{double{0}}
												   : Eval(expression, node.Arguments[0], depth + 1),
						 b = Eval(expression, node.Arguments[1], depth + 1);
					if (node.Operation == "index")
						result = Numeric(b) ? Index(a, Number(b)) : Value{double{0}};
					else if (node.Operation == "=") {
						result = b;
						if (!Assign(expression, node.Arguments[0], b, depth)) return result;
						const auto &target = expression.Data->Instructions[node.Arguments[0]];
						if (target.Operation == "index" && target.Arguments.size() == 2) {
							auto owner = target.Arguments[0];
							while (expression.Data->Instructions[owner].Operation == "=" ||
								   expression.Data->Instructions[owner].Operation == "default")
								owner = expression.Data->Instructions[owner].Arguments[0];
							if (expression.Data->Instructions[owner].Operation == "name")
								result = Variables[std::get<std::string>(expression.Data->Instructions[owner]
																			 .Literal)];
						}
					} else {
						result = Binary(node.Operation, a, b);
						if (node.Operation == "+=" || node.Operation == "-=" || node.Operation == "*=" ||
							node.Operation == "/=")
							Assign(expression, node.Arguments[0], result, depth);
					}
				}
				if (Error.Code == Status::Ok) Admit(result);
				return result;
			}
		};
	}
	Status ExecutePcxExpression(
		const PcxExpressionValue &expression,
		const PcxExecutionContext &context,
		PcxExecutionResult &output,
		Diagnostic &diagnostic
	) {
		ENGINE_PROFILE("imagegraph.pcx.execute");
		if (!detail::ValidPcxPayload(expression) || !context.MaximumWork ||
			context.MaximumWork > 64'000'000 || !context.MaximumBytes ||
			context.MaximumBytes > Limits::MaximumEvaluationBytes ||
			context.Parameters.size() > Limits::MaximumLinks ||
			(context.Target &&
			 (!ValidSurfaceLayout(*context.Target, Limits::MaximumDimension, Limits::MaximumOutputBytes) ||
			  !FiniteSurfaceSamples(*context.Target)))) {
			diagnostic = {
				Status::InvalidValue, {}, "equation", "PCX program or execution limits are invalid"
			};
			return diagnostic.Code;
		}
		for (const auto &instruction : expression.Data->Instructions) {
			const auto &operation = instruction.Operation;
			const size_t arguments = instruction.Arguments.size();
			const bool name = operation == "name";
			if ((name && (!std::holds_alternative<std::string>(instruction.Literal) || arguments)) ||
				(operation == "literal" && arguments) || ((operation == "if") && arguments != 3) ||
				((operation == "for") && arguments != 4) ||
				((operation == "neg" || operation == "~") && arguments != 1) ||
				((operation == "for_each") &&
				 (arguments != 2 || !std::holds_alternative<std::string>(instruction.Literal)))) {
				diagnostic = {Status::InvalidValue, {}, "equation", "PCX instruction operands are invalid"};
				return diagnostic.Code;
			}
		}
		Runtime runtime{
			context, {}, {}, {}, 0, 0, SourceRandom(uint32_t(context.Request.Seed)), context.DrawBlend
		};
		for (const auto &[key, value] : expression.Data->Bindings) {
			if (!runtime.Admit(value)) break;
			runtime.Variables.insert_or_assign(key, value);
		}
		if (context.Request.PcxObservations.size() > 8 ||
			context.Request.ProjectName.size() > Limits::MaximumTextBytes) {
			diagnostic = {
				Status::LimitExceeded, {}, "equation", "PCX observation snapshot exceeds its bound"
			};
			return diagnostic.Code;
		}
		for (const auto &observation : context.Request.PcxObservations) {
			constexpr std::string_view names[]{
				"Program.time",
				"Device.timeSecond",
				"Device.timeMinute",
				"Device.timeHour",
				"Device.timeDay",
				"Device.timeDayInWeek",
				"Device.timeMonth",
				"Device.timeYear"
			};
			if (std::find(std::begin(names), std::end(names), observation.Port) == std::end(names) ||
				!Runtime::Numeric(observation.Data) || !runtime.Admit(observation.Data)) {
				diagnostic = {
					Status::InvalidValue,
					{},
					observation.Port,
					"PCX observations require named finite numeric values"
				};
				return diagnostic.Code;
			}
			if (runtime.Variables.contains(observation.Port)) {
				diagnostic = {
					Status::DuplicateId, {}, observation.Port, "PCX observation names are duplicated"
				};
				return diagnostic.Code;
			}
			runtime.Variables.emplace(observation.Port, observation.Data);
		}
		for (const auto &value : context.Parameters) {
			if (!runtime.Admit(value.Data)) break;
			runtime.Variables.insert_or_assign(value.Port, value.Data);
		}
		if (runtime.Error.Code == Status::Ok)
			runtime.Result.Data = runtime.Eval(expression, expression.Data->Root);
		if (runtime.Error.Code != Status::Ok) {
			diagnostic = std::move(runtime.Error);
			return diagnostic.Code;
		}
		output = std::move(runtime.Result);
		diagnostic = {};
		return Status::Ok;
	}
}
