#include "SourceParticle2DReplay.hpp"

#include "../NodeExecutors.hpp"
#include "../ParticlePayload.hpp"

#include <engine/core/Profiling.hpp>

#include <algorithm>
#include <bit>
#include <climits>
#include <cmath>
#include <limits>
#include <new>

namespace engine::imagegraph::detail {
	namespace {
		bool Fail(NodeContext &c, const char *text) {
			return c.Fail(Status::InvalidValue, text, "data");
		}
		struct Writer {
			BufferValue Data;
			bool Valid = true;
			void Word(uint64_t v) {
				for (unsigned shift = 0; shift < 64; shift += 8)
					Data.Bytes.push_back(uint8_t(v >> shift));
			}
			void Real(double v) {
				Valid &= std::isfinite(v);
				Word(std::bit_cast<uint64_t>(v));
			}
			void History(const std::vector<double> &v) {
				Word(v.size());
				for (double x : v)
					Real(x);
			}
		};
		struct Reader {
			const std::vector<uint8_t> &Data;
			size_t Offset = 0;
			bool Word(uint64_t &v) {
				if (Offset > Data.size() || Data.size() - Offset < 8) return false;
				v = 0;
				for (unsigned shift = 0; shift < 64; shift += 8)
					v |= uint64_t(Data[Offset++]) << shift;
				return true;
			}
			bool Real(double &v) {
				uint64_t bits;
				if (!Word(bits)) return false;
				v = std::bit_cast<double>(bits);
				return std::isfinite(v);
			}
			template <class T> bool Unsigned(T &v) {
				uint64_t x;
				if (!Word(x) || x > std::numeric_limits<T>::max()) return false;
				v = T(x);
				return true;
			}
			bool Integer(int &v) {
				uint64_t bits;
				if (!Word(bits)) return false;
				const auto x = std::bit_cast<int64_t>(bits);
				if (x < INT_MIN || x > INT_MAX) return false;
				v = int(x);
				return true;
			}
			bool Flag(bool &v) {
				uint64_t x;
				if (!Word(x) || x > 1) return false;
				v = x != 0;
				return true;
			}
			bool History(std::vector<double> &v) {
				uint64_t count;
				if (!Word(count) || count > Limits::MaximumRangeFrames + 1 ||
					count > (Data.size() - Offset) / 8)
					return false;
				v.resize(count);
				for (auto &x : v)
					if (!Real(x)) return false;
				return true;
			}
			bool Done() const {
				return Offset == Data.size();
			}
		};
		bool Fields(const StructValue &value, std::initializer_list<std::string_view> names) {
			if (!value.Data || value.Data->Fields.size() != names.size()) return false;
			size_t index = 0;
			for (auto name : names)
				if (value.Data->Fields[index++].first != name) return false;
			return true;
		}
		bool Noone(const Value &v) {
			const auto *n = std::get_if<int64_t>(&v);
			return n && *n == -4;
		}
		std::optional<BufferValue> SlotNumbers(const SourceParticle2DSlot &s) {
			Writer w;
			for (double x : s.Start)
				w.Real(x);
			for (double x : s.Previous)
				w.Real(x);
			for (double x : s.Draw)
				w.Real(x);
			for (double x : s.Velocity)
				w.Real(x);
			for (double x : s.InitialVelocity)
				w.Real(x);
			for (double x : s.BaseScale)
				w.Real(x);
			for (double x : s.DrawScale)
				w.Real(x);
			for (double x : s.PathRange)
				w.Real(x);
			w.Real(s.DrawRotation);
			w.Real(s.Life);
			w.Real(s.LifeTotal);
			w.Real(s.BaseRotation);
			w.Real(s.RotationSpeed);
			w.Real(s.TargetAngle);
			w.Real(s.SnapRotation);
			w.Real(s.Alpha);
			w.Real(s.Acceleration);
			w.Real(s.Friction);
			w.Real(s.GravityX);
			w.Real(s.GravityY);
			w.Real(s.Turning);
			w.Real(s.TurnScale);
			w.Real(s.GroundY);
			w.Real(s.Bounce);
			w.Real(s.GroundFriction);
			w.Real(s.AnimationSpeed);
			w.Real(s.PathDeviation);
			w.Real(s.Direction);
			w.Real(s.DirectionSpeed);
			w.Word(s.Seed);
			w.Word(s.Blend);
			w.Word(s.HistoryIndex);
			w.Word(s.TrailLife);
			w.Word(std::bit_cast<uint64_t>(int64_t(s.SpriteSelection)));
			w.Word(std::bit_cast<uint64_t>(int64_t(s.AnimationEnd)));
			w.Word(std::bit_cast<uint64_t>(int64_t(s.RotationType)));
			w.Word(std::bit_cast<uint64_t>(int64_t(s.RenderType)));
			w.Word(std::bit_cast<uint64_t>(int64_t(s.Wrap)));
			w.Word(s.PreviousDefined);
			w.Word(s.DrawDefined);
			w.Word(s.Physics);
			w.Word(s.Ground);
			w.Word(s.Wiggles);
			w.Word(s.RotateByDirection);
			w.Word(s.StretchAnimation);
			w.Word(s.PathLoop);
			w.Word(bool(s.AtlasRect));
			if (s.AtlasRect)
				for (double x : {s.AtlasRect->X, s.AtlasRect->Y, s.AtlasRect->Z, s.AtlasRect->W})
					w.Real(x);
			w.History(s.ScaleXHistory);
			w.History(s.ScaleYHistory);
			w.History(s.AlphaHistory);
			w.Word(s.BlendHistory.size());
			for (auto x : s.BlendHistory)
				w.Word(x);
			if (!w.Valid) return std::nullopt;
			return std::move(w.Data);
		}
		bool SlotNumbers(const BufferValue &v, SourceParticle2DSlot &s) {
			Reader r{v.Bytes};
			for (auto &x : s.Start)
				if (!r.Real(x)) return false;
			for (auto &x : s.Previous)
				if (!r.Real(x)) return false;
			for (auto &x : s.Draw)
				if (!r.Real(x)) return false;
			for (auto &x : s.Velocity)
				if (!r.Real(x)) return false;
			for (auto &x : s.InitialVelocity)
				if (!r.Real(x)) return false;
			for (auto &x : s.BaseScale)
				if (!r.Real(x)) return false;
			for (auto &x : s.DrawScale)
				if (!r.Real(x)) return false;
			for (auto &x : s.PathRange)
				if (!r.Real(x)) return false;
			if (!r.Real(s.DrawRotation)) return false;
			if (!r.Real(s.Life)) return false;
			if (!r.Real(s.LifeTotal)) return false;
			if (!r.Real(s.BaseRotation)) return false;
			if (!r.Real(s.RotationSpeed)) return false;
			if (!r.Real(s.TargetAngle)) return false;
			if (!r.Real(s.SnapRotation)) return false;
			if (!r.Real(s.Alpha)) return false;
			if (!r.Real(s.Acceleration)) return false;
			if (!r.Real(s.Friction)) return false;
			if (!r.Real(s.GravityX)) return false;
			if (!r.Real(s.GravityY)) return false;
			if (!r.Real(s.Turning)) return false;
			if (!r.Real(s.TurnScale)) return false;
			if (!r.Real(s.GroundY)) return false;
			if (!r.Real(s.Bounce)) return false;
			if (!r.Real(s.GroundFriction)) return false;
			if (!r.Real(s.AnimationSpeed)) return false;
			if (!r.Real(s.PathDeviation)) return false;
			if (!r.Real(s.Direction)) return false;
			if (!r.Real(s.DirectionSpeed)) return false;
			if (!r.Unsigned(s.Seed)) return false;
			if (!r.Unsigned(s.Blend)) return false;
			if (!r.Unsigned(s.HistoryIndex)) return false;
			if (!r.Unsigned(s.TrailLife)) return false;
			if (!r.Integer(s.SpriteSelection)) return false;
			if (!r.Integer(s.AnimationEnd)) return false;
			if (!r.Integer(s.RotationType)) return false;
			if (!r.Integer(s.RenderType)) return false;
			if (!r.Integer(s.Wrap)) return false;
			if (!r.Flag(s.PreviousDefined)) return false;
			if (!r.Flag(s.DrawDefined)) return false;
			if (!r.Flag(s.Physics)) return false;
			if (!r.Flag(s.Ground)) return false;
			if (!r.Flag(s.Wiggles)) return false;
			if (!r.Flag(s.RotateByDirection)) return false;
			if (!r.Flag(s.StretchAnimation)) return false;
			if (!r.Flag(s.PathLoop)) return false;
			bool atlas;
			if (!r.Flag(atlas)) return false;
			if (atlas) {
				Vector4 x;
				if (!r.Real(x.X) || !r.Real(x.Y) || !r.Real(x.Z) || !r.Real(x.W)) return false;
				s.AtlasRect = x;
			}
			if (!r.History(s.ScaleXHistory) || !r.History(s.ScaleYHistory) || !r.History(s.AlphaHistory))
				return false;
			uint64_t count;
			if (!r.Word(count) || count > Limits::MaximumRangeFrames ||
				count > (v.Bytes.size() - r.Offset) / 8)
				return false;
			s.BlendHistory.resize(count);
			for (auto &x : s.BlendHistory)
				if (!r.Unsigned(x)) return false;
			return r.Done();
		}
		uint64_t StateBytes(const SourceParticle2DState &s) {
			uint64_t bytes = sizeof(s) + s.Slots.capacity() * sizeof(SourceParticle2DSlot);
			for (const auto &curve : s.Curves)
				bytes = MeshAddBytes(bytes, curve.capacity() * sizeof(double));
			for (const auto &slot : s.Slots) {
				bytes = MeshAddBytes(bytes, ParticleDataBytes<true>(slot.Data));
				for (const auto *v : {&slot.ScaleXHistory, &slot.ScaleYHistory, &slot.AlphaHistory})
					bytes = MeshAddBytes(bytes, v->capacity() * sizeof(double));
				bytes = MeshAddBytes(bytes, slot.BlendHistory.capacity() * sizeof(uint32_t));
				bytes = MeshAddBytes(bytes, RetainedPayloadBytes(slot.LifetimeColour));
				if (slot.FollowPath) bytes = MeshAddBytes(bytes, RetainedPayloadBytes(*slot.FollowPath));
			}
			return bytes;
		}
	}
	bool EncodeSourceParticle2DReceipt(
		NodeContext &c, const SourceParticle2DState &s, StructValue &output, AllocationReservation &charge
	) try {
		ENGINE_PROFILE("imagegraph.particle2d.encode_receipt");
		if (!ValidateSourceParticle2DState(c, s)) return false;
		const uint64_t estimate = MeshAddBytes(
			MeshAddBytes(
				MeshAddBytes(StateBytes(s), s.LastSurface ? s.LastSurface->Pixels.capacity() : 0),
				MeshAddBytes(StateBytes(s), s.LastSurface ? s.LastSurface->Pixels.capacity() : 0)
			),
			s.Slots.size() * 1024 + 65536
		);
		auto work = c.ReserveWorkspace(estimate);
		if (!work) return false;
		StructValue candidate;
		auto &root = candidate.Data.emplace();
		root.Fields.reserve(6);
		Writer metadata;
		metadata.Word(s.Seed);
		metadata.Word(s.SpawnTotal);
		metadata.Word(s.Runner);
		metadata.Word(s.SpawnIndex);
		metadata.Word(std::bit_cast<uint64_t>(s.Frame));
		metadata.Word(s.Initialized);
		ArrayValue slots{ValueType::Any, {}};
		slots.Elements.reserve(s.Slots.size());
		for (const auto &slot : s.Slots) {
			StructValue stored;
			auto &fields = stored.Data.emplace().Fields;
			fields.reserve(4);
			ParticleValue particle;
			particle.Data.emplace() = slot.Data;
			fields.emplace_back("data", std::move(particle));
			fields.emplace_back("colour", slot.LifetimeColour);
			fields.emplace_back("path", slot.FollowPath ? Value{*slot.FollowPath} : Value{int64_t{-4}});
			auto numbers = SlotNumbers(slot);
			if (!numbers) return Fail(c, "Particle receipt numeric state is nonfinite");
			fields.emplace_back("numeric", std::move(*numbers));
			slots.Elements.emplace_back(std::move(stored));
		}
		Writer curves;
		for (const auto &curve : s.Curves)
			curves.History(curve);
		Writer wiggles;
		for (const auto &map : s.WiggleMaps)
			for (auto value : map)
				wiggles.Real(value);
		for (auto value : s.WiggleAmplitudes)
			wiggles.Real(value);
		if (!curves.Valid || !wiggles.Valid)
			return Fail(c, "Particle receipt curve or wiggle state is nonfinite");
		root.Fields.emplace_back("version", int64_t{1});
		root.Fields.emplace_back("numeric", std::move(metadata.Data));
		root.Fields.emplace_back("slots", std::move(slots));
		root.Fields.emplace_back("curves", std::move(curves.Data));
		root.Fields.emplace_back("wiggles", std::move(wiggles.Data));
		root.Fields.emplace_back(
			"surface", s.LastSurface ? Value{SurfaceValue{*s.LastSurface}} : Value{int64_t{-4}}
		);
		const bool valid = ValidStructPayload(candidate);
		if (!valid) return Fail(c, "Particle receipt payload is malformed");
		const uint64_t retained = RetainedPayloadBytes(candidate);
		if (!work->Resize(retained))
			return c.Fail(Status::LimitExceeded, "Particle receipt capacity exceeds budget", "data");
		output = std::move(candidate);
		charge = std::move(*work);
		return true;
	} catch (const std::bad_alloc &) {
		return c.Fail(Status::LimitExceeded, "Particle receipt allocation failed", "data");
	}
	bool DecodeSourceParticle2DReceipt(
		NodeContext &c, const StructValue &input, SourceParticle2DState &output
	) try {
		ENGINE_PROFILE("imagegraph.particle2d.decode_receipt");
		if (!Fields(input, {"version", "numeric", "slots", "curves", "wiggles", "surface"}))
			return Fail(c, "Particle receipt schema differs from version one");
		const auto &fields = input.Data->Fields;
		const auto *version = std::get_if<int64_t>(&fields[0].second);
		const auto *metadata = std::get_if<BufferValue>(&fields[1].second);
		const auto *slots = std::get_if<ArrayValue>(&fields[2].second);
		const auto *curves = std::get_if<BufferValue>(&fields[3].second);
		const auto *wiggles = std::get_if<BufferValue>(&fields[4].second);
		const auto *surface = std::get_if<SurfaceValue>(&fields[5].second);
		if (!version || *version != 1 || !metadata || !slots || !curves || !wiggles ||
			(!surface && !Noone(fields[5].second)) || slots->ElementType != ValueType::Any ||
			!slots->Nested.empty() || !slots->Items.empty() || slots->Elements.empty() ||
			slots->Elements.size() > 4096)
			return Fail(c, "Particle receipt field types are malformed");
		const bool valid = ValidStructPayload(input);
		if (!valid) return Fail(c, "Particle receipt payload is malformed");
		const uint64_t estimate = MeshAddBytes(
			sizeof(SourceParticle2DState) + slots->Elements.size() * sizeof(SourceParticle2DSlot),
			MeshAddBytes(MeshAddBytes(RetainedPayloadBytes(input), RetainedPayloadBytes(input)), 65536)
		);
		auto reservation = c.ReserveWorkspace(estimate);
		if (!reservation) return false;
		SourceParticle2DState candidate;
		Reader meta{metadata->Bytes};
		uint64_t frame;
		if (!meta.Unsigned(candidate.Seed) || !meta.Word(candidate.SpawnTotal) ||
			!meta.Unsigned(candidate.Runner) || !meta.Unsigned(candidate.SpawnIndex) || !meta.Word(frame) ||
			!meta.Flag(candidate.Initialized) || !meta.Done())
			return Fail(c, "Particle receipt metadata is malformed");
		candidate.Frame = std::bit_cast<int64_t>(frame);
		if (!candidate.Initialized || candidate.Frame < -1 ||
			candidate.Frame > int64_t(Limits::MaximumTick) || candidate.Runner >= slots->Elements.size() ||
			candidate.SpawnIndex >= slots->Elements.size())
			return Fail(c, "Particle receipt metadata bounds are malformed");
		candidate.Slots.resize(slots->Elements.size());
		for (size_t index = 0; index < slots->Elements.size(); ++index) {
			const auto *stored = std::get_if<StructValue>(&slots->Elements[index]);
			if (!stored || !Fields(*stored, {"data", "colour", "path", "numeric"}))
				return Fail(c, "Particle receipt slot schema is malformed");
			const auto &f = stored->Data->Fields;
			const auto *data = std::get_if<ParticleValue>(&f[0].second);
			const auto *colour = std::get_if<Gradient>(&f[1].second);
			const auto *path = std::get_if<Path2D>(&f[2].second);
			const auto *numbers = std::get_if<BufferValue>(&f[3].second);
			if (!data || !data->Data || !colour || !numbers || (!path && !Noone(f[2].second)))
				return Fail(c, "Particle receipt slot field types are malformed");
			auto &slot = candidate.Slots[index];
			slot.Data = *data->Data;
			slot.LifetimeColour = *colour;
			if (path) slot.FollowPath = *path;
			if (!SlotNumbers(*numbers, slot))
				return Fail(c, "Particle receipt slot numeric state is malformed");
		}
		Reader curveReader{curves->Bytes};
		for (auto &curve : candidate.Curves)
			if (!curveReader.History(curve)) return Fail(c, "Particle receipt curve map is malformed");
		if (!curveReader.Done()) return Fail(c, "Particle receipt curve buffer has trailing bytes");
		Reader wiggleReader{wiggles->Bytes};
		for (auto &map : candidate.WiggleMaps)
			for (auto &value : map)
				if (!wiggleReader.Real(value)) return Fail(c, "Particle receipt wiggle map is malformed");
		for (auto &value : candidate.WiggleAmplitudes)
			if (!wiggleReader.Real(value)) return Fail(c, "Particle receipt wiggle amplitude is malformed");
		if (!wiggleReader.Done()) return Fail(c, "Particle receipt wiggle buffer has trailing bytes");
		if (surface) candidate.LastSurface = surface->Data;
		if (!ValidateSourceParticle2DState(c, candidate)) return false;
		uint64_t retained = StateBytes(candidate);
		if (surface) retained = MeshAddBytes(retained, surface->Data.Pixels.capacity());
		if (!reservation->Resize(retained))
			return c.Fail(Status::LimitExceeded, "Particle replay state capacity exceeds budget", "data");
		for (auto &slot : candidate.Slots) {
			uint64_t bytes = ParticleDataBytes<true>(slot.Data);
			bytes = MeshAddBytes(bytes, RetainedPayloadBytes(slot.LifetimeColour));
			if (slot.FollowPath) bytes = MeshAddBytes(bytes, RetainedPayloadBytes(*slot.FollowPath));
			for (const auto *history : {&slot.ScaleXHistory, &slot.ScaleYHistory, &slot.AlphaHistory})
				bytes = MeshAddBytes(bytes, history->capacity() * sizeof(double));
			bytes = MeshAddBytes(bytes, slot.BlendHistory.capacity() * sizeof(uint32_t));
			auto token = reservation->Split(bytes);
			if (!token)
				return c.Fail(Status::LimitExceeded, "Particle slot ownership exceeds reservation", "data");
			slot.Charge = std::move(*token);
		}
		if (candidate.LastSurface) {
			auto token = reservation->Split(candidate.LastSurface->Pixels.capacity());
			if (!token)
				return c.Fail(
					Status::LimitExceeded, "Particle surface ownership exceeds reservation", "data"
				);
			candidate.LastSurfaceCharge = std::move(*token);
		}
		candidate.Charge = std::move(*reservation);
		output = std::move(candidate);
		return true;
	} catch (const std::bad_alloc &) {
		return c.Fail(Status::LimitExceeded, "Particle replay allocation failed", "data");
	}
}
