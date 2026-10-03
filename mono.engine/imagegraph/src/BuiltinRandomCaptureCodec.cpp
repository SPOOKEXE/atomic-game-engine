#include "EvaluationBudget.hpp"
#include "ValueText.hpp"

#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/imagegraph/BuiltinRandomCaptureCodec.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <locale>
#include <ostream>
#include <streambuf>
#include <type_traits>
namespace engine::imagegraph {
	namespace {
		constexpr std::string_view Header = "imagegraph-builtin-random 2\n";
		constexpr std::string_view LegacyHeader = "imagegraph-builtin-random 1\n";
		constexpr std::array<std::string_view, 6> Operations{
			"random", "irandom", "irandom_range", "crand", "random_get_seed", "random_range"
		};

		template <class T> bool DurableLeaf(const T &value, size_t depth);
		bool DurableItem(const SourceArrayItem &item, size_t depth) {
			if (depth > Limits::MaximumArrayDepth) return false;
			if (const auto *leaf = std::get_if<ElementValue>(&item.Data))
				return std::visit([&](const auto &value) { return DurableLeaf(value, depth + 1); }, *leaf);
			if (const auto *children = std::get_if<std::vector<SourceArrayItem>>(&item.Data)) {
				for (const auto &child : *children)
					if (!DurableItem(child, depth + 1)) return false;
				return true;
			}
			return false;
		}
		template <class T> bool DurableLeaf(const T &value, size_t depth) {
			if (depth > Limits::MaximumArrayDepth) return false;
			if constexpr (std::is_same_v<T, ArrayValue>) {
				for (const auto &leaf : value.Elements)
					if (!std::visit([&](const auto &v) { return DurableLeaf(v, depth + 1); }, leaf))
						return false;
				for (const auto &row : value.Nested)
					for (const auto &leaf : row)
						if (!std::visit([&](const auto &v) { return DurableLeaf(v, depth + 1); }, leaf))
							return false;
				for (const auto &item : value.Items)
					if (!DurableItem(item, depth + 1)) return false;
				return true;
			} else if constexpr (std::is_same_v<T, AudioBit>)
				return value.Channels.empty() && value.SampleRate > 0;
			else
				return std::is_same_v<T, bool> || std::is_same_v<T, int64_t> || std::is_same_v<T, double> ||
					   std::is_same_v<T, std::string> || std::is_same_v<T, Colour> ||
					   std::is_same_v<T, Vector2> || std::is_same_v<T, Gradient> || std::is_same_v<T, Area> ||
					   std::is_same_v<T, Curve> || std::is_same_v<T, Vector4> || std::is_same_v<T, Path2D> ||
					   std::is_same_v<T, Vector3> || std::is_same_v<T, Quaternion> ||
					   std::is_same_v<T, EnumValue> || std::is_same_v<T, MatrixValue> ||
					   std::is_same_v<T, PixelBoxValue> || std::is_same_v<T, RigidValue> ||
					   std::is_same_v<T, AtlasValue> || std::is_same_v<T, ParticleValue> ||
					   std::is_same_v<T, TilesetValue>;
		}
		bool Durable(const Value &value) {
			return std::visit([](const auto &leaf) { return DurableLeaf(leaf, 0); }, value);
		}
		class CountBuffer final : public std::streambuf {
			uint64_t Limit;
			std::streamsize xsputn(const char *, std::streamsize n) override {
				if (n < 0 || static_cast<uint64_t>(n) > Limit - Bytes) return 0;
				Bytes += n;
				return n;
			}
			int_type overflow(int_type v) override {
				if (traits_type::eq_int_type(v, traits_type::eof())) return traits_type::not_eof(v);
				if (Bytes == Limit) return traits_type::eof();
				++Bytes;
				return v;
			}

		  public:
			uint64_t Bytes = 0;
			explicit CountBuffer(uint64_t limit) : Limit(limit) {}
		};
		class FixedBuffer final : public std::streambuf {
			std::streamsize xsputn(const char *s, std::streamsize n) override {
				if (n < 0 || n > epptr() - pptr()) return 0;
				std::copy_n(s, n, pptr());
				pbump(static_cast<int>(n));
				return n;
			}
			int_type overflow(int_type v) override {
				if (traits_type::eq_int_type(v, traits_type::eof())) return traits_type::not_eof(v);
				if (pptr() == epptr()) return traits_type::eof();
				*pptr() = traits_type::to_char_type(v);
				pbump(1);
				return v;
			}

		  public:
			explicit FixedBuffer(std::string &text) {
				setp(text.data(), text.data() + text.size());
			}
		};
		struct Writer {
			std::ostream &Out;
			bool Valid = true;
			std::string_view NodeId, Port;
			void Text(std::string_view value) {
				Out << value.size() << ':';
				Out.write(value.data(), value.size());
				Out.put('\n');
			}
			template <class T> void Number(T value) {
				std::array<char, 64> buffer;
				auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
				if (result.ec != std::errc{}) {
					Valid = false;
					return;
				}
				Text({buffer.data(), static_cast<size_t>(result.ptr - buffer.data())});
			}
			void Bool(bool value) {
				Text(value ? "true" : "false");
			}
			void Typed(const Value &value) {
				if (!Valid) return;
				if (!Durable(value)) {
					Valid = false;
					return;
				}
				CountBuffer count(Limits::MaximumEvaluationBytes);
				std::ostream measured(&count);
				measured.imbue(std::locale::classic());
				detail::WriteValueText(measured, value);
				if (!measured) {
					Valid = false;
					return;
				}
				Out << count.Bytes << ':';
				detail::WriteValueText(Out, value);
				Out.put('\n');
			}
			template <class T, class F> void List(const std::vector<T> &values, F write) {
				Number(values.size());
				for (const auto &value : values)
					write(value);
			}
			void Values(const std::vector<AuthoredValue> &values) {
				List(values, [&](const auto &value) {
					Text(value.Port);
					if (Valid) Port = value.Port;
					Typed(value.Data);
				});
			}
			void Strings(const std::vector<std::string> &values) {
				List(values, [&](const auto &value) { Text(value); });
			}
			void NodeData(const Node &n) {
				Text(n.Id);
				Text(n.Type);
				Text(n.GroupId);
				Number(n.Position.X);
				Number(n.Position.Y);
				Values(n.Values);
				List(n.DynamicInputs, [&](const auto &input) {
					Text(input.Id);
					Text(ValueTypeName(input.Type));
					Bool(input.Default.has_value());
					if (input.Default) {
						if (Valid) Port = input.Id;
						Typed(*input.Default);
					}
					Text(input.SourceLayerName);
					Text(input.SourceInputId);
				});
				Text(n.InstanceBase);
				Strings(n.InstanceOverrides);
				Strings(n.SourceAnimatedInputs);
				Strings(n.SourceStaticInputs);
				List(n.DynamicOutputs, [&](const auto &output) {
					Text(output.Id);
					Text(ValueTypeName(output.Type));
				});
				Text(n.SourceDisplayName);
				Text(n.SourceInternalName);
				List(n.SourceInputExpressions, [&](const auto &expression) {
					Text(expression.Port);
					Text(expression.Code);
					Bool(expression.Enabled);
				});
				Values(n.SourceProperties);
			}
			void Capture(const SourceBuiltinRandomCapture &c) {
				Text("capture");
				if (Valid) NodeId = c.Authored.Id;
				NodeData(c.Authored);
				Number(c.ProcessorRow);
				Number(c.Tick);
				Number(c.Subframe);
				Bool(c.NegativeFrame);
				Values(c.Inputs);
				List(c.InputImages, [&](const auto &image) {
					Text(image.Port);
					Number(image.Data.Width);
					Number(image.Data.Height);
					auto info = DescribeSurfaceFormat(image.Data.Format);
					if (!info) {
						Valid = false;
						return;
					}
					Text(info->Name);
					Number(image.Data.Hash);
					Text(
						{reinterpret_cast<const char *>(image.Data.Pixels.data()), image.Data.Pixels.size()}
					);
				});
				List(c.Draws, [&](const auto &draw) {
					auto index = static_cast<size_t>(draw.Operation);
					if (index >= Operations.size()) {
						Valid = false;
						return;
					}
					Text(Operations[index]);
					Number(draw.Lower);
					Number(draw.Upper);
					Number(draw.Result);
				});
			}
			void All(std::span<const SourceBuiltinRandomCapture> captures) {
				Out.write(Header.data(), Header.size());
				Number(captures.size());
				for (const auto &capture : captures)
					Capture(capture);
			}
		};
		struct Reader {
			std::string_view Rest;
			detail::EvaluationBudget &Budget;
			detail::AllocationReservation &Charge;
			Status Failure = Status::Malformed;
			uint8_t Version = 2;
			bool Admit(uint64_t bytes) {
				if (bytes > Budget.Available()) {
					Failure = Status::LimitExceeded;
					return false;
				}
				return Charge.Resize(Charge.Bytes() + bytes);
			}
			bool Token(std::string_view &value) {
				auto colon = Rest.find(':');
				if (colon == std::string_view::npos || colon > 20) return false;
				uint64_t size = 0;
				auto parsed = std::from_chars(Rest.data(), Rest.data() + colon, size);
				if (parsed.ec != std::errc{} || parsed.ptr != Rest.data() + colon ||
					size > Rest.size() - colon - 1)
					return false;
				Rest.remove_prefix(colon + 1);
				if (size == Rest.size() || Rest[size] != '\n') return false;
				value = Rest.substr(0, size);
				Rest.remove_prefix(size + 1);
				return true;
			}
			bool Text(std::string &value, size_t maximum = Limits::MaximumTextBytes) {
				std::string_view token;
				if (!Token(token)) return false;
				if (token.size() > maximum) {
					Failure = Status::LimitExceeded;
					return false;
				}
				if (!Admit(token.size() + 32)) return false;
				value.assign(token);
				return true;
			}
			template <class T> bool Number(T &value) {
				std::string_view token;
				if (!Token(token) || token.size() > 64 || token.empty()) return false;
				auto parsed = std::from_chars(token.data(), token.data() + token.size(), value);
				return parsed.ec == std::errc{} && parsed.ptr == token.data() + token.size();
			}
			bool Bool(bool &value) {
				std::string_view token;
				if (!Token(token)) return false;
				if (token == "true")
					value = true;
				else if (token == "false")
					value = false;
				else
					return false;
				return true;
			}
			bool Typed(Value &value) {
				std::string_view token;
				if (!Token(token)) return false;
				auto status = detail::ReadValueText(token, value, Budget, Charge);
				if (status != Status::Ok) {
					Failure = status;
					return false;
				}
				if (!Durable(value)) {
					Failure = Status::UnsupportedExecution;
					return false;
				}
				return true;
			}
			template <class T, class F> bool List(std::vector<T> &values, size_t maximum, F read) {
				size_t count;
				if (!Number(count)) return false;
				if (count > maximum) {
					Failure = Status::LimitExceeded;
					return false;
				}
				if (count && (!Admit(count * sizeof(T)))) return false;
				values.reserve(count);
				for (size_t i = 0; i < count; ++i) {
					values.emplace_back();
					if (!read(values.back())) return false;
				}
				return true;
			}
			bool Values(std::vector<AuthoredValue> &values) {
				return List(values, Limits::MaximumLinks, [&](auto &value) {
					return Text(value.Port) && Typed(value.Data);
				});
			}
			bool Strings(std::vector<std::string> &values) {
				return List(values, Limits::MaximumArrayElements, [&](auto &value) { return Text(value); });
			}
			bool Type(ValueType &type) {
				std::string_view token;
				if (!Token(token)) return false;
				auto parsed = ParseValueTypeName(token);
				if (!parsed) return false;
				type = *parsed;
				return true;
			}
			bool NodeData(Node &n) {
				if (!Text(n.Id) || !Text(n.Type) || !Text(n.GroupId) || !Number(n.Position.X) ||
					!Number(n.Position.Y) || !Values(n.Values))
					return false;
				if (!List(n.DynamicInputs, MaximumDynamicInputsForNode(n), [&](auto &input) {
						bool present;
						if (!Text(input.Id) || !Type(input.Type) || !Bool(present)) return false;
						if (present) {
							input.Default.emplace();
							if (!Typed(*input.Default)) return false;
						}
						return Text(input.SourceLayerName) &&
							   (Version == 1 || Text(input.SourceInputId, Limits::MaximumSourceInputIdBytes));
					}))
					return false;
				if (!Text(n.InstanceBase) || !Strings(n.InstanceOverrides) ||
					!Strings(n.SourceAnimatedInputs) || !Strings(n.SourceStaticInputs))
					return false;
				if (!List(n.DynamicOutputs, Limits::MaximumDynamicOutputsPerNode, [&](auto &output) {
						return Text(output.Id) && Type(output.Type);
					}))
					return false;
				if (!Text(n.SourceDisplayName) || !Text(n.SourceInternalName)) return false;
				if (!List(
						n.SourceInputExpressions,
						Limits::MaximumSourceInputExpressionsPerNode,
						[&](auto &expression) {
							return Text(expression.Port) && Text(expression.Code) && Bool(expression.Enabled);
						}
					))
					return false;
				return Values(n.SourceProperties);
			}
			bool Capture(SourceBuiltinRandomCapture &c) {
				std::string_view kind;
				if (!Token(kind) || kind != "capture" || !NodeData(c.Authored) || !Number(c.ProcessorRow) ||
					!Number(c.Tick) || !Number(c.Subframe) || !Bool(c.NegativeFrame) || !Values(c.Inputs))
					return false;
				if (!List(c.InputImages, Limits::MaximumLinks, [&](auto &image) {
						if (!Text(image.Port) || !Number(image.Data.Width) || !Number(image.Data.Height))
							return false;
						std::string_view name;
						if (!Token(name)) return false;
						bool found = false;
						for (int i = 0; i <= static_cast<int>(SurfaceFormat::R32Float); ++i) {
							auto info = DescribeSurfaceFormat(static_cast<SurfaceFormat>(i));
							if (info && info->Name == name) {
								image.Data.Format = static_cast<SurfaceFormat>(i);
								found = true;
								break;
							}
						}
						if (!found || !Number(image.Data.Hash)) return false;
						std::string_view pixels;
						if (!Token(pixels)) return false;
						auto layout = CheckedSurfaceLayout(
							image.Data.Width,
							image.Data.Height,
							image.Data.Format,
							Limits::MaximumEvaluationBytes
						);
						if (!layout || image.Data.Width > Limits::MaximumDimension ||
							image.Data.Height > Limits::MaximumDimension || layout->Bytes != pixels.size())
							return false;
						if (!Admit(pixels.size())) return false;
						image.Data.Pixels.assign(
							reinterpret_cast<const uint8_t *>(pixels.data()),
							reinterpret_cast<const uint8_t *>(pixels.data()) + pixels.size()
						);
						return true;
					}))
					return false;
				return List(c.Draws, 65536, [&](auto &draw) {
					std::string_view name;
					if (!Token(name)) return false;
					auto operation = std::find(Operations.begin(), Operations.end(), name);
					if (operation == Operations.end()) return false;
					draw.Operation =
						static_cast<SourceBuiltinRandomOperation>(operation - Operations.begin());
					return Number(draw.Lower) && Number(draw.Upper) && Number(draw.Result);
				});
			}
		};
		Status Fail(Diagnostic &diagnostic, Status status, std::string_view message) {
			diagnostic = {status, {}, {}, std::string(message)};
			return status;
		}
	}

	std::string_view BuiltinRandomOperationName(SourceBuiltinRandomOperation operation) noexcept {
		auto index = static_cast<size_t>(operation);
		return index < Operations.size() ? Operations[index] : std::string_view{};
	}
	std::optional<SourceBuiltinRandomOperation>
	ParseBuiltinRandomOperationName(std::string_view name) noexcept {
		auto found = std::find(Operations.begin(), Operations.end(), name);
		if (found == Operations.end()) return std::nullopt;
		return static_cast<SourceBuiltinRandomOperation>(found - Operations.begin());
	}
	Status WriteBuiltinRandomCapture(
		std::span<const SourceBuiltinRandomCapture> captures,
		std::string &text,
		Diagnostic &diagnostic,
		uint64_t maximumBytes
	) {
		ENGINE_PROFILE("imagegraph builtin random write");
		if (!maximumBytes || maximumBytes > Limits::MaximumEvaluationBytes)
			return Fail(
				diagnostic, Status::LimitExceeded, "Builtin RNG codec byte cap is outside native bounds"
			);
		try {
			if (text.capacity() > maximumBytes)
				return Fail(diagnostic, Status::LimitExceeded, "Builtin RNG prior text exceeds byte cap");
			uint64_t owned = 0;
			auto status =
				ValidateBuiltinRandomCaptures(captures, maximumBytes - text.capacity(), owned, diagnostic);
			if (status != Status::Ok) return status;
			detail::EvaluationBudget budget(maximumBytes);
			auto residency = budget.Reserve(0);
			if (!residency || text.capacity() > budget.Available() || !residency->Resize(text.capacity()))
				return Fail(diagnostic, Status::LimitExceeded, "Builtin RNG previous text exceeds residency");
			// Value grammar printers may clone one array row/leaf. Two full owned charges cover that scratch.
			if (owned > budget.Available() / 2 || !residency->Resize(residency->Bytes() + owned * 2))
				return Fail(
					diagnostic, Status::LimitExceeded, "Builtin RNG writer controls exceed residency"
				);
			CountBuffer measured(budget.Available());
			std::ostream count(&measured);
			count.imbue(std::locale::classic());
			Writer first{count, true, {}, {}};
			first.All(captures);
			if (!first.Valid) {
				diagnostic = {
					Status::UnsupportedExecution,
					std::string(first.NodeId),
					std::string(first.Port),
					"Builtin RNG control has no durable value representation"
				};
				return diagnostic.Code;
			}
			if (!count || measured.Bytes > budget.Available() / 2)
				return Fail(
					diagnostic, Status::LimitExceeded, "Builtin RNG encoded capture exceeds residency"
				);
			if (!residency->Resize(residency->Bytes() + 2 * measured.Bytes + 32))
				return Fail(diagnostic, Status::LimitExceeded, "Builtin RNG encoded text exceeds residency");
			std::string candidate;
			candidate.resize(measured.Bytes);
			FixedBuffer storage(candidate);
			std::ostream out(&storage);
			out.imbue(std::locale::classic());
			Writer second{out, true, {}, {}};
			second.All(captures);
			if (!out || !second.Valid)
				return Fail(diagnostic, Status::InvalidValue, "Builtin RNG encoding failed");
			core::Metrics::Count("imagegraph builtin capture write bytes", candidate.size());
			core::Metrics::Count("imagegraph builtin capture write records", captures.size());
			text = std::move(candidate);
			diagnostic = {};
			return Status::Ok;
		} catch (const std::bad_alloc &) {
			return Fail(diagnostic, Status::LimitExceeded, "Builtin RNG encoding allocation failed");
		}
	}
	Status ReadBuiltinRandomCapture(
		std::string_view text,
		std::vector<SourceBuiltinRandomCapture> &captures,
		Diagnostic &diagnostic,
		uint64_t maximumBytes
	) {
		ENGINE_PROFILE("imagegraph builtin random read");
		if (!maximumBytes || maximumBytes > Limits::MaximumEvaluationBytes || text.size() > maximumBytes)
			return Fail(
				diagnostic, Status::LimitExceeded, "Builtin RNG codec source exceeds native byte cap"
			);
		try {
			if (captures.capacity() > maximumBytes / sizeof(SourceBuiltinRandomCapture))
				return Fail(diagnostic, Status::LimitExceeded, "Builtin RNG prior slots exceed byte cap");
			const uint64_t slots = captures.capacity() * sizeof(SourceBuiltinRandomCapture);
			if (slots > maximumBytes - text.size())
				return Fail(
					diagnostic, Status::LimitExceeded, "Builtin RNG prior slots and source exceed byte cap"
				);
			uint64_t prior = 0;
			auto status = ValidateBuiltinRandomCaptures(
				captures, maximumBytes - text.size() - slots, prior, diagnostic
			);
			if (status != Status::Ok) return status;
			if (slots > maximumBytes - prior)
				return Fail(
					diagnostic, Status::LimitExceeded, "Builtin RNG prior capture slots exceed residency"
				);
			prior += slots;
			detail::EvaluationBudget budget(maximumBytes);
			auto charge = budget.Reserve(0);
			if (prior > budget.Available() || !charge->Resize(prior) || text.size() > budget.Available() ||
				!charge->Resize(prior + text.size()))
				return Fail(diagnostic, Status::LimitExceeded, "Builtin RNG replacement exceeds residency");
			const uint8_t version = text.starts_with(Header) ? 2 : text.starts_with(LegacyHeader) ? 1 : 0;
			if (!version)
				return Fail(
					diagnostic, Status::UnsupportedVersion, "Builtin RNG capture header is unsupported"
				);
			Reader reader{text.substr(Header.size()), budget, *charge, Status::Malformed, version};
			std::vector<SourceBuiltinRandomCapture> candidate;
			if (!reader.List(
					candidate, Limits::MaximumNodes, [&](auto &capture) { return reader.Capture(capture); }
				) ||
				!reader.Rest.empty())
				return Fail(
					diagnostic, reader.Failure, "Builtin RNG capture record is malformed or exceeds admission"
				);
			size_t validationSlots = 0;
			for (const auto &capture : candidate)
				validationSlots =
					std::max(validationSlots, std::max(capture.Inputs.size(), capture.InputImages.size()));
			auto validation = budget.Reserve(validationSlots * sizeof(std::string_view));
			if (!validation)
				return Fail(
					diagnostic, Status::LimitExceeded, "Builtin RNG candidate validation exceeds residency"
				);
			uint64_t owned = 0;
			status = ValidateBuiltinRandomCaptures(candidate, maximumBytes, owned, diagnostic);
			if (status != Status::Ok) return status;
			if (owned > maximumBytes - prior - text.size())
				return Fail(
					diagnostic,
					Status::LimitExceeded,
					"Builtin RNG candidate retained capacity exceeds residency"
				);
			core::Metrics::Count("imagegraph builtin capture read bytes", text.size());
			core::Metrics::Count("imagegraph builtin capture read records", candidate.size());
			captures = std::move(candidate);
			diagnostic = {};
			return Status::Ok;
		} catch (const std::bad_alloc &) {
			return Fail(diagnostic, Status::LimitExceeded, "Builtin RNG decoding allocation failed");
		}
	}
}
