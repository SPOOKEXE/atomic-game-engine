#pragma once
#include <engine/imagegraph/Document.hpp>

#include <cmath>
#include <iomanip>
#include <istream>
#include <ostream>

namespace engine::imagegraph::detail {
	inline bool ValidSourceBridge(const SourcePathBridgeData2D &bridge, size_t &count) {
		if (bridge.LineCount > Limits::MaximumArrayElements || count > Limits::MaximumArrayElements ||
			bridge.Lines.size() > Limits::MaximumArrayElements - count)
			return false;
		count += bridge.Lines.size();
		for (const auto &line : bridge.Lines) {
			const size_t anchors = line.Anchors.size();
			const size_t segments = anchors ? anchors - 1 : 0;
			if (!anchors || anchors > Limits::MaximumPathAnchors || line.Accumulated.size() != segments ||
				!std::isfinite(line.Length) || line.Length < 0)
				return false;
			for (size_t size : {anchors, line.Controls.size(), line.Accumulated.size()}) {
				if (size > Limits::MaximumArrayElements - count) return false;
				count += size;
			}
			for (const auto &point : line.Anchors)
				if (!std::isfinite(point.X) || !std::isfinite(point.Y) || !std::isfinite(point.Z))
					return false;
			for (const auto &control : line.Controls)
				for (double value : control)
					if (!std::isfinite(value)) return false;
			double previous = 0;
			for (double length : line.Accumulated) {
				if (!std::isfinite(length) || length < previous) return false;
				previous = length;
			}
			if (line.Length != previous) return false;
		}
		return true;
	}
	template <bool retained> uint64_t SourceBridgeBytes(const SourcePathBridgeData2D &bridge) {
		uint64_t bytes = sizeof(bridge) + (retained ? bridge.Lines.capacity() : bridge.Lines.size()) *
											  sizeof(SourcePathBridgeLine2D);
		for (const auto &line : bridge.Lines)
			bytes +=
				(retained ? line.Anchors.capacity() : line.Anchors.size()) * sizeof(Vector3) +
				(retained ? line.Controls.capacity() : line.Controls.size()) * sizeof(std::array<double, 4>) +
				(retained ? line.Accumulated.capacity() : line.Accumulated.size()) * sizeof(double);
		return bytes;
	}
	inline void WriteSourceBridge(std::ostream &stream, const SourcePathBridgeData2D &bridge) {
		stream << bridge.LineCount << ' ' << bridge.Smooth << ' ' << bridge.Lines.size();
		for (const auto &line : bridge.Lines) {
			stream << ' ' << line.Anchors.size() << ' ' << line.Controls.size() << ' '
				   << line.Accumulated.size() << ' ' << std::setprecision(17) << line.Length;
			for (const auto &point : line.Anchors)
				stream << ' ' << point.X << ' ' << point.Y << ' ' << point.Z;
			for (const auto &control : line.Controls)
				for (double value : control)
					stream << ' ' << value;
			for (double value : line.Accumulated)
				stream << ' ' << value;
		}
	}
	template <class Admit>
	bool ReadSourceBridge(std::istream &stream, SourcePathBridgeData2D &bridge, Admit &admit) {
		unsigned smooth = 0;
		size_t rows = 0;
		if (!(stream >> bridge.LineCount >> smooth >> rows) || smooth > 1 ||
			bridge.LineCount > Limits::MaximumArrayElements || rows > Limits::MaximumArrayElements ||
			!admit(rows * sizeof(SourcePathBridgeLine2D)))
			return false;
		bridge.Smooth = bool(smooth);
		bridge.Lines.reserve(rows);
		size_t count = rows;
		for (size_t row = 0; row < rows; ++row) {
			size_t anchors = 0, controls = 0, lengths = 0;
			SourcePathBridgeLine2D line;
			if (!(stream >> anchors >> controls >> lengths >> line.Length)) return false;
			for (size_t size : {anchors, controls, lengths}) {
				if (size > Limits::MaximumArrayElements - count) return false;
				count += size;
			}
			if (!anchors || anchors > Limits::MaximumPathAnchors || lengths != anchors - 1 ||
				!admit(
					anchors * sizeof(Vector3) + controls * sizeof(std::array<double, 4>) +
					lengths * sizeof(double)
				))
				return false;
			line.Anchors.resize(anchors);
			line.Controls.resize(controls);
			line.Accumulated.resize(lengths);
			for (auto &point : line.Anchors)
				if (!(stream >> point.X >> point.Y >> point.Z)) return false;
			for (auto &control : line.Controls)
				for (double &value : control)
					if (!(stream >> value)) return false;
			for (double &value : line.Accumulated)
				if (!(stream >> value)) return false;
			bridge.Lines.push_back(std::move(line));
		}
		size_t validation = 1;
		return ValidSourceBridge(bridge, validation);
	}
}
