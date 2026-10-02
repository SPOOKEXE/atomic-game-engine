#pragma once
// Shape geometry follows the pinned __shapes.gml constructors and vertex order.
#include <engine/imagegraph/Document.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <vector>
namespace engine::imagegraph::detail::shape2d {
	using Triangle = std::array<Vector2, 3>;
	struct Object {
		bool Rectangle = false;
		std::vector<Triangle> Triangles;
	};
	struct Geometry {
		std::vector<Object> Objects;
		std::vector<Vector2> Segment;
	};
	struct Data {
		std::array<double, 2> scale{}, radRan{0, 360};
		[[maybe_unused]] double side = 16, inner = .5, radius = .5, teeth = 6, teethH = .2, teethT = 0,
								cap = 0, explode = 0, trep = .5, palAng = .5, factor = 3;
	};
	inline double abs(double value) {
		return std::abs(value);
	}
	inline double max(double a, double b) {
		return std::max(a, b);
	}
	inline double min(double a, double b) {
		return std::min(a, b);
	}
	inline double lerp(double a, double b, double t) {
		return a + (b - a) * t;
	}
	inline double dcos(double a) {
		return std::cos(a * std::numbers::pi / 180);
	}
	inline double dsin(double a) {
		return std::sin(a * std::numbers::pi / 180);
	}
	inline double lengthdir_x(double l, double a) {
		return l * dcos(a);
	}
	inline double lengthdir_y(double l, double a) {
		return -l * dsin(a);
	}
	inline double power(double a, double b) {
		return std::pow(a, b);
	}
	inline double triangle_area_points(double x0, double y0, double x1, double y1, double x2, double y2) {
		return std::abs(x0 * (y1 - y2) + x1 * (y2 - y0) + x2 * (y0 - y1)) / 2;
	}
	template <class T> inline std::vector<T> array_merge(std::vector<T> a, const std::vector<T> &b) {
		a.insert(a.end(), b.begin(), b.end());
		return a;
	}
	template <class T> inline void Push(std::vector<T> &v, std::initializer_list<T> values) {
		v.insert(v.end(), values);
	}
	template <class T> inline void Append(std::vector<T> &v, const std::vector<T> &values) {
		v.insert(v.end(), values.begin(), values.end());
	}
	template <class T> inline std::vector<T> Reverse(std::vector<T> values) {
		std::reverse(values.begin(), values.end());
		return values;
	}
	inline Geometry rectangle(const Data &data) {
		std::vector<Triangle> triangles;
		std::vector<Vector2> segment;
		[[maybe_unused]] double w = 0, h = 0;

		w = data.scale[0];
		h = data.scale[1];

		triangles = {
			{Vector2{-w, -h}, Vector2{-w, h}, Vector2{w, -h}},
			{Vector2{w, -h}, Vector2{-w, h}, Vector2{w, h}},
		};
		segment = {Vector2{-w, -h}, Vector2{w, -h}, Vector2{w, h}, Vector2{-w, h}, Vector2{-w, -h}};

		return Geometry{{Object{true, triangles}}, segment};
	}
	inline Geometry diamond(const Data &data) {
		std::vector<Triangle> triangles;
		std::vector<Vector2> segment;
		[[maybe_unused]] double w = 0, h = 0;

		w = data.scale[0];
		h = data.scale[1];

		triangles = {
			{Vector2{0, 0}, Vector2{w, 0}, Vector2{0, -h}},
			{Vector2{0, 0}, Vector2{0, -h}, Vector2{-w, 0}},
			{Vector2{0, 0}, Vector2{-w, 0}, Vector2{0, h}},
			{Vector2{0, 0}, Vector2{0, h}, Vector2{w, 0}},
		};
		segment = {Vector2{w, 0}, Vector2{0, -h}, Vector2{-w, 0}, Vector2{0, h}, Vector2{w, 0}};

		return Geometry{{Object{true, triangles}}, segment};
	}
	inline Geometry trapezoid(const Data &data) {
		std::vector<Triangle> triangles;
		std::vector<Vector2> segment;
		[[maybe_unused]] double w = 0, h = 0, v = 0;

		w = data.scale[0];
		h = data.scale[1];
		v = w * data.trep;

		triangles = {
			{Vector2{-v, -h}, Vector2{-w, h}, Vector2{v, -h}},
			{Vector2{v, -h}, Vector2{-w, h}, Vector2{w, h}},
		};
		segment = {Vector2{-v, -h}, Vector2{v, -h}, Vector2{w, h}, Vector2{-w, h}, Vector2{-v, -h}};

		return Geometry{{Object{true, triangles}}, segment};
	}
	inline Geometry parallelogram(const Data &data) {
		std::vector<Triangle> triangles;
		std::vector<Vector2> segment;
		[[maybe_unused]] double w = 0, h = 0, a = 0, x0 = 0, x1 = 0, x2 = 0, x3 = 0;

		w = data.scale[0];
		h = data.scale[1];
		a = data.palAng;

		x0 = -w, x1 = w;
		x2 = -w, x3 = w;

		if (a > 0) {
			x0 = lerp(w, -w, 1 - abs(a));
			x3 = lerp(-w, w, 1 - abs(a));
		} else {
			x2 = lerp(w, -w, 1 - abs(a));
			x1 = lerp(-w, w, 1 - abs(a));
		}

		triangles = {
			{Vector2{x0, -h}, Vector2{x2, h}, Vector2{x1, -h}},
			{Vector2{x1, -h}, Vector2{x2, h}, Vector2{x3, h}},
		};
		segment = {Vector2{x0, -h}, Vector2{x1, -h}, Vector2{x3, h}, Vector2{x2, h}, Vector2{x0, -h}};

		return Geometry{{Object{true, triangles}}, segment};
	}
	inline Geometry circle(const Data &data) {
		std::vector<Triangle> triangles;
		std::vector<Vector2> segment;
		[[maybe_unused]] double w = 0, h = 0, prec = 0, ang = 0, explode = 0, i = 0, d0 = 0, d5 = 0, d1 = 0,
								dx = 0, dy = 0, x0 = 0, y0 = 0, x1 = 0, y1 = 0;

		w = data.scale[0];
		h = data.scale[1];

		prec = max(3, data.side);
		ang = 360 / prec;
		triangles.resize(prec);
		segment.resize(prec + 1);
		explode = data.explode;

		for (i = 0; i < prec; i++) {
			d0 = (i + 0.) * ang;
			d5 = (i + .5) * ang;
			d1 = (i + 1.) * ang;

			dx = lengthdir_x(explode * w, d5);
			dy = lengthdir_y(explode * h, d5);

			x0 = lengthdir_x(w, d0) + dx;
			y0 = lengthdir_y(h, d0) + dy;
			x1 = lengthdir_x(w, d1) + dx;
			y1 = lengthdir_y(h, d1) + dy;

			triangles[i] = {Vector2{dx, dy}, Vector2{x0, y0}, Vector2{x1, y1}};

			if (i == 0) segment[0] = Vector2{x0, y0};
			segment[i + 1] = Vector2{x1, y1};
		}

		return Geometry{{Object{false, triangles}}, segment};
	}
	inline Geometry ring(const Data &data) {
		std::vector<Triangle> triangles;
		std::vector<Vector2> segment;
		[[maybe_unused]] double w = 0, h = 0, s = 0, ow = 0, oh = 0, iw = 0, ih = 0, an = 0, i = 0, a0 = 0,
								a1 = 0, ix0 = 0, iy0 = 0, ix1 = 0, iy1 = 0, ox0 = 0, oy0 = 0, ox1 = 0,
								oy1 = 0;

		w = data.scale[0];
		h = data.scale[1];

		s = max(3, data.side);
		ow = w;
		oh = h;
		iw = w * data.inner;
		ih = h * data.inner;
		an = 360 / s;

		triangles = {};
		segment = {};

		for (i = 0; i < s; i++) {
			a0 = i * an;
			a1 = a0 + an;

			ix0 = lengthdir_x(iw, a0);
			iy0 = lengthdir_y(ih, a0);
			ix1 = lengthdir_x(iw, a1);
			iy1 = lengthdir_y(ih, a1);

			ox0 = lengthdir_x(ow, a0);
			oy0 = lengthdir_y(oh, a0);
			ox1 = lengthdir_x(ow, a1);
			oy1 = lengthdir_y(oh, a1);

			Push(triangles, {{Vector2{ix0, iy0}, Vector2{ox0, oy0}, Vector2{ox1, oy1}}});
			Push(triangles, {{Vector2{ix0, iy0}, Vector2{ox1, oy1}, Vector2{ix1, iy1}}});

			if (i == 0) Push(segment, {Vector2{ox0, oy0}});
			Push(segment, {Vector2{ox1, oy1}});
		}

		return Geometry{{Object{true, triangles}}, segment};
	}
	inline Geometry arc(const Data &data) {
		std::vector<Triangle> triangles;
		std::vector<Vector2> segment;
		std::vector<Vector2> sgArcI;
		std::vector<Vector2> sgArcO;
		std::vector<Vector2> sgCapI;
		std::vector<Vector2> sgCapO;
		std::array<double, 2> radRan{};
		[[maybe_unused]] double w = 0, h = 0, prec = 0, inner = 0, cap = 0, oa = 0, na = 0, i = 0, ix1 = 0,
								iy1 = 0, nx1 = 0, ny1 = 0, ix0 = 0, iy0 = 0, nx0 = 0, ny0 = 0, cx = 0, cy = 0,
								ox = 0, oy = 0, nx = 0, ny = 0;

		w = data.scale[0];
		h = data.scale[1];

		prec = max(3, data.side);
		inner = data.inner;
		radRan = data.radRan;
		cap = data.cap;
		triangles = {};
		segment = {};

		sgArcI = {};
		sgArcO = {};

		for (i = 0; i <= prec; i++) {
			na = lerp(radRan[0], radRan[1], i / prec);

			ix1 = lengthdir_x(0.5 * inner, na) * w * 2;
			iy1 = lengthdir_y(0.5 * inner, na) * h * 2;

			nx1 = lengthdir_x(0.5, na) * w * 2;
			ny1 = lengthdir_y(0.5, na) * h * 2;

			if (i) {
				ix0 = lengthdir_x(0.5 * inner, oa) * w * 2;
				iy0 = lengthdir_y(0.5 * inner, oa) * h * 2;

				nx0 = lengthdir_x(0.5, oa) * w * 2;
				ny0 = lengthdir_y(0.5, oa) * h * 2;

				Push(triangles, {{Vector2{ix0, iy0}, Vector2{nx0, ny0}, Vector2{nx1, ny1}}});
				Push(triangles, {{Vector2{ix0, iy0}, Vector2{nx1, ny1}, Vector2{ix1, iy1}}});
			}

			Push(sgArcI, {Vector2{ix1, iy1}});
			Push(sgArcO, {Vector2{nx1, ny1}});

			oa = na;
		}

		if (cap) {
			cx = lengthdir_x(0.5 * (inner + 1) / 2, radRan[0]) * w * 2;
			cy = lengthdir_y(0.5 * (inner + 1) / 2, radRan[0]) * h * 2;

			sgCapI = {};
			sgCapO = {};
			prec = max(ceil(prec / 2), 2);

			for (i = 0; i <= prec; i++) {
				na = radRan[0] - 180 * i / prec;
				nx = cx + lengthdir_x((1 - inner) / 2, na) * w;
				ny = cy + lengthdir_y((1 - inner) / 2, na) * h;

				if (i) Push(triangles, {{Vector2{cx, cy}, Vector2{ox, oy}, Vector2{nx, ny}}});

				Push(sgCapI, {Vector2{nx, ny}});

				oa = na;
				ox = nx;
				oy = ny;
			}

			cx = lengthdir_x(0.5 * (inner + 1) / 2, radRan[1]) * w * 2;
			cy = lengthdir_y(0.5 * (inner + 1) / 2, radRan[1]) * h * 2;

			for (i = 0; i <= prec; i++) {
				na = radRan[1] + 180 * i / prec;
				nx = cx + lengthdir_x((1 - inner) / 2, na) * w;
				ny = cy + lengthdir_y((1 - inner) / 2, na) * h;

				if (i) Push(triangles, {{Vector2{cx, cy}, Vector2{ox, oy}, Vector2{nx, ny}}});

				Push(sgCapO, {Vector2{nx, ny}});

				oa = na;
				ox = nx;
				oy = ny;
			}

			Append(segment, sgArcI);
			Append(segment, Reverse(sgCapO));

			Append(segment, Reverse(sgArcO));
			Append(segment, sgCapI);
		} else {
			Append(segment, sgArcI);
			Append(segment, Reverse(sgArcO));
			Push(segment, {sgArcI[0]});
		}

		return Geometry{{Object{true, triangles}}, segment};
	}
	inline Geometry crescent(const Data &data) {
		std::vector<Triangle> triangles;
		std::vector<Vector2> segment;
		[[maybe_unused]] double w = 0, h = 0, s = 0, ow = 0, oh = 0, iw = 0, ih = 0, an = 0, cx = 0, cy = 0,
								i = 0, a0 = 0, a1 = 0, ox0 = 0, oy0 = 0, ox1 = 0, oy1 = 0, ix0 = 0, iy0 = 0,
								ix1 = 0, iy1 = 0;

		w = data.scale[0];
		h = data.scale[1];

		s = max(3, data.side);
		ow = w;
		oh = h;
		iw = w * data.inner;
		ih = h * data.inner;
		an = 360 / s;

		cx = w - w * data.inner;
		cy = 0;

		triangles = {};
		segment = {};

		for (i = 0; i < s; i++) {
			a0 = i * an;
			a1 = a0 + an;

			ox0 = lengthdir_x(ow, a0);
			oy0 = lengthdir_y(oh, a0);
			ox1 = lengthdir_x(ow, a1);
			oy1 = lengthdir_y(oh, a1);

			ix0 = cx + lengthdir_x(iw, a0);
			iy0 = cy + lengthdir_y(ih, a0);
			ix1 = cx + lengthdir_x(iw, a1);
			iy1 = cy + lengthdir_y(ih, a1);

			if (triangle_area_points(ix0, iy0, ox0, oy0, ox1, oy1) > 0.1)
				Push(triangles, {{Vector2{ix0, iy0}, Vector2{ox0, oy0}, Vector2{ox1, oy1}}});
			if (triangle_area_points(ix0, iy0, ox1, oy1, ix1, iy1) > 0.1)
				Push(triangles, {{Vector2{ix0, iy0}, Vector2{ox1, oy1}, Vector2{ix1, iy1}}});

			if (segment.empty()) Push(segment, {Vector2{ox0, oy0}});
			Push(segment, {Vector2{ox1, oy1}});
		}

		return Geometry{{Object{false, triangles}}, segment};
	}
	inline Geometry pie(const Data &data) {
		std::vector<Triangle> triangles;
		std::vector<Vector2> segment;
		std::array<double, 2> r{};
		[[maybe_unused]] double w = 0, h = 0, p = 0, a = 0, i = 0, d0 = 0, d1 = 0, x0 = 0, y0 = 0, x1 = 0,
								y1 = 0;

		w = data.scale[0];
		h = data.scale[1];

		p = max(3, data.side);
		a = 1 / p;
		r = data.radRan;

		triangles.resize(p);
		segment.resize(p + 1);

		for (i = 0; i < p; i++) {
			d0 = lerp(r[0], r[1], i * a);
			d1 = lerp(r[0], r[1], i * a + a);

			x0 = lengthdir_x(w, d0);
			y0 = lengthdir_y(h, d0);
			x1 = lengthdir_x(w, d1);
			y1 = lengthdir_y(h, d1);

			triangles[i] = {Vector2{0, 0}, Vector2{x0, y0}, Vector2{x1, y1}};

			if (i == 0) segment[0] = Vector2{x0, y0};
			segment[i + 1] = Vector2{x1, y1};
		}

		return Geometry{{Object{false, triangles}}, segment};
	}
	inline Geometry squircle(const Data &data) {
		std::vector<Triangle> triangles;
		std::vector<Vector2> segment;
		[[maybe_unused]] double w = 0, h = 0, s = 0, a = 0, f = 0, ox = 0, oy = 0, nx = 0, ny = 0, s2 = 0,
								i = 0, d = 0, r = 0;

		w = data.scale[0];
		h = data.scale[1];

		s = max(3, data.side);
		a = 360 / s;
		f = max(.001, data.factor);
		triangles = {};
		segment = {};

		s2 = sqrt(2);

		for (i = 0; i <= s; i++) {
			d = i * a;
			r = 1 / power(power(abs(dcos(d)), f) + power(abs(dsin(d)), f), 1 / f);

			nx = lengthdir_x(r * w, d);
			ny = lengthdir_y(r * h, d);

			if (i) Push(triangles, {{Vector2{0, 0}, Vector2{ox, oy}, Vector2{nx, ny}}});
			Push(segment, {Vector2{nx, ny}});

			ox = nx;
			oy = ny;
		}

		segment[s] = segment[0];

		return Geometry{{Object{false, triangles}}, segment};
	}
	inline Geometry reg_poly(const Data &data) {
		std::vector<Triangle> triangles;
		std::vector<Vector2> segment;
		[[maybe_unused]] double w = 0, h = 0, p = 0, a = 0, i = 0, d0 = 0, d1 = 0, x0 = 0, y0 = 0, x1 = 0,
								y1 = 0;

		w = data.scale[0];
		h = data.scale[1];

		p = max(3, data.side);
		a = 360 / p;

		triangles.resize(p);
		segment.resize(p + 1);

		for (i = 0; i < p; i++) {
			d0 = i * a;
			d1 = d0 + a;

			x0 = lengthdir_x(w, d0);
			y0 = lengthdir_y(h, d0);
			x1 = lengthdir_x(w, d1);
			y1 = lengthdir_y(h, d1);

			triangles[i] = {Vector2{0, 0}, Vector2{x0, y0}, Vector2{x1, y1}};

			if (i == 0) segment[0] = Vector2{x0, y0};
			segment[i + 1] = Vector2{x1, y1};
		}

		return Geometry{{Object{false, triangles}}, segment};
	}
	inline Geometry star(const Data &data) {
		std::vector<Triangle> triangles;
		std::vector<Vector2> segment;
		Vector2 pi0{};
		[[maybe_unused]] double w = 0, h = 0, prec = 0, inner = 0, i = 0, otx = 0, oty = 0, inx = 0, iny = 0;

		w = data.scale[0];
		h = data.scale[1];

		prec = max(3, data.side);
		inner = data.inner;
		triangles = {};
		segment = {};

		for (i = 0; i < prec; i++) {
			otx = lengthdir_x(0.5, i / prec * 360) * w * 2;
			oty = lengthdir_y(0.5, i / prec * 360) * h * 2;

			inx = lengthdir_x(inner / 2, (i + 0.5) / prec * 360) * w * 2;
			iny = lengthdir_y(inner / 2, (i + 0.5) / prec * 360) * h * 2;
			Push(triangles, {{Vector2{0, 0}, Vector2{otx, oty}, Vector2{inx, iny}}});

			pi0 = Vector2{inx, iny};

			inx = lengthdir_x(inner / 2, (i - 0.5) / prec * 360) * w * 2;
			iny = lengthdir_y(inner / 2, (i - 0.5) / prec * 360) * h * 2;
			Push(triangles, {{Vector2{0, 0}, Vector2{inx, iny}, Vector2{otx, oty}}});

			Push(segment, {Vector2{inx, iny}});
			Push(segment, {Vector2{otx, oty}});
			Push(segment, {pi0});
		}

		return Geometry{{Object{false, triangles}}, segment};
	}
	inline Geometry cross(const Data &data) {
		std::vector<Triangle> triangles;
		std::vector<Vector2> segment;
		[[maybe_unused]] double w = 0, h = 0, inner = 0, side = 0;

		w = data.scale[0];
		h = data.scale[1];

		inner = data.inner;
		triangles = {};
		segment = {};
		side = min(w, h) * inner;

		Push(
			triangles,
			{{Vector2{-side, -side}, Vector2{-side, side}, Vector2{side, -side}},
			 {Vector2{side, -side}, Vector2{-side, side}, Vector2{side, side}}}
		);

		Push(
			triangles,
			{{Vector2{-side, -side}, Vector2{side, -side}, Vector2{-side, -h}},
			 {Vector2{side, -side}, Vector2{side, -h}, Vector2{-side, -h}}}
		);

		Push(
			triangles,
			{{Vector2{-side, h}, Vector2{side, h}, Vector2{-side, side}},
			 {Vector2{side, h}, Vector2{side, side}, Vector2{-side, side}}}
		);

		Push(
			triangles,
			{{Vector2{-side, -side}, Vector2{-w, -side}, Vector2{-side, side}},
			 {Vector2{-w, -side}, Vector2{-w, side}, Vector2{-side, side}}}
		);

		Push(
			triangles,
			{{Vector2{w, -side}, Vector2{side, -side}, Vector2{w, side}},
			 {Vector2{side, -side}, Vector2{side, side}, Vector2{w, side}}}
		);

		Push(segment, {Vector2{-side, -side}, Vector2{-side, -h}, Vector2{side, -h}, Vector2{side, -side}});
		Push(segment, {Vector2{w, -side}, Vector2{w, side}, Vector2{side, side}});
		Push(segment, {Vector2{side, h}, Vector2{-side, h}, Vector2{-side, side}});
		Push(segment, {Vector2{-w, side}, Vector2{-w, -side}, Vector2{-side, -side}});

		return Geometry{{Object{true, triangles}}, segment};
	}
	inline Geometry capsule(const Data &data) {
		std::vector<Object> shapes = std::vector<Object>(3);
		std::vector<Vector2> segment;
		std::vector<Triangle> triangles;
		std::vector<Vector2> _seg;
		[[maybe_unused]] double w = 0, h = 0, rad = 0, prec = 0, hh = 0, cx = 0, cy = 0, ox = 0, oy = 0,
								nx = 0, ny = 0, oa = 0, na = 0, i = 0, n = 0;

		w = data.scale[0];
		h = data.scale[1];

		rad = data.radius;
		prec = max(2, data.side);
		hh = h * rad;
		shapes = std::vector<Object>(3);
		segment = {};
		Push(segment, {Vector2{-w + h, h}, Vector2{w - hh, hh}});

		triangles = {
			{Vector2{-w + h, -h}, Vector2{-w + h, h}, Vector2{w - hh, -hh}},
			{Vector2{w - hh, -hh}, Vector2{-w + h, h}, Vector2{w - hh, hh}}
		};
		shapes[0] = Object{true, triangles};

		triangles = {};
		cx = -w + h;
		cy = 0;

		for (i = 0; i <= prec; i++) {
			na = lerp(270, 90, i / prec);
			nx = cx + lengthdir_x(h, na);
			ny = cy + lengthdir_y(h, na);

			if (i) {
				Push(triangles, {{Vector2{cx, cy}, Vector2{nx, ny}, Vector2{ox, oy}}});
				Push(segment, {Vector2{ox, oy}});
			}
			Push(segment, {Vector2{nx, ny}});

			oa = na;
			ox = nx;
			oy = ny;
		}

		Push(segment, {Vector2{-w + h, -h}, Vector2{w - hh, -hh}});
		shapes[1] = Object{false, triangles};

		triangles = {};
		cx = w - hh;
		cy = 0;
		_seg = {};

		for (i = 0; i <= prec; i++) {
			na = lerp(-90, 90, i / prec);
			nx = cx + lengthdir_x(hh, na);
			ny = cy + lengthdir_y(hh, na);

			if (i) {
				Push(triangles, {{Vector2{cx, cy}, Vector2{ox, oy}, Vector2{nx, ny}}});
				Push(_seg, {Vector2{ox, oy}});
			}
			Push(_seg, {Vector2{nx, ny}});

			oa = na;
			ox = nx;
			oy = ny;
		}

		for (i = 0, n = _seg.size(); i < n; i++)
			Push(segment, {_seg[_seg.size() - i - 1]});

		shapes[2] = Object{false, triangles};

		return Geometry{shapes, segment};
	}
	inline Geometry leaf(const Data &data) {
		std::vector<Triangle> triangles;
		std::vector<Vector2> s0;
		std::vector<Vector2> s1;
		std::vector<Vector2> segment;
		[[maybe_unused]] double w = 0, h = 0, p = 0, a = 0, _oy = 0, _ox0 = 0, _ox1 = 0, i = 0, d = 0,
								_ny = 0, _nx0 = 0, _nx1 = 0;

		w = data.scale[0];
		h = data.scale[1];

		p = max(3, data.side);
		a = 180 / (p - 1);

		triangles = {};
		s0 = {};
		s1 = {};

		for (i = 0; i < p; i++) {
			d = lerp(-60, 60, i / (p - 1));

			_ny = lengthdir_y(h, d) / dsin(60);
			_nx0 = -w + lengthdir_x(w * 2, d);
			_nx1 = w - lengthdir_x(w * 2, d);

			if (i) {
				Push(triangles, {{Vector2{_nx0, _ny}, Vector2{_nx1, _ny}, Vector2{_ox0, _oy}}});
				Push(triangles, {{Vector2{_ox0, _oy}, Vector2{_nx1, _ny}, Vector2{_ox1, _oy}}});

				if (s0.empty()) Push(s0, {Vector2{_ox0, _oy}});
				Push(s0, {Vector2{_nx0, _ny}});
				if (s1.empty()) Push(s1, {Vector2{_ox1, _oy}});
				Push(s1, {Vector2{_nx1, _ny}});
			}

			_oy = _ny;
			_ox0 = _nx0;
			_ox1 = _nx1;
		}

		std::reverse(s1.begin(), s1.end());
		segment = array_merge(s0, s1);

		return Geometry{{Object{true, triangles}}, segment};
	}
	inline Geometry gear(const Data &data) {
		std::vector<Triangle> triangles;
		std::vector<Vector2> segment;
		[[maybe_unused]] double w = 0, h = 0, teeth = 0, teethH = 0, teethT = 0, prec = 0, inner = 0,
								body = 0, teth = 0, i = 0, ix0 = 0, iy0 = 0, nx0 = 0, ny0 = 0, ix1 = 0,
								iy1 = 0, nx1 = 0, ny1 = 0, tx0 = 0, ty0 = 0, tx1 = 0, ty1 = 0;

		w = data.scale[0];
		h = data.scale[1];

		teeth = max(3, data.teeth);
		teethH = data.teethH;
		teethT = data.teethT;
		prec = teeth * 2;
		inner = data.inner;
		body = 0.5 * (1 - teethH);
		teth = 0.5 * teethH;
		triangles = {};
		segment = {};

		for (i = 0; i < prec; i++) {
			ix0 = lengthdir_x(body * inner, i / prec * 360) * w * 2;
			iy0 = lengthdir_y(body * inner, i / prec * 360) * h * 2;

			nx0 = lengthdir_x(body, i / prec * 360) * w * 2;
			ny0 = lengthdir_y(body, i / prec * 360) * h * 2;

			ix1 = lengthdir_x(body * inner, (i + 1) / prec * 360) * w * 2;
			iy1 = lengthdir_y(body * inner, (i + 1) / prec * 360) * h * 2;

			nx1 = lengthdir_x(body, (i + 1) / prec * 360) * w * 2;
			ny1 = lengthdir_y(body, (i + 1) / prec * 360) * h * 2;

			Push(triangles, {{Vector2{ix0, iy0}, Vector2{nx0, ny0}, Vector2{nx1, ny1}}});
			Push(triangles, {{Vector2{ix0, iy0}, Vector2{nx1, ny1}, Vector2{ix1, iy1}}});

			if (i == 0) Push(segment, {Vector2{nx0, ny0}});

			if (std::fmod(i, 2)) {
				tx0 = nx0 + lengthdir_x(teth, (i + 0.5 - teethT) / prec * 360) * w * 2;
				ty0 = ny0 + lengthdir_y(teth, (i + 0.5 - teethT) / prec * 360) * h * 2;

				tx1 = nx1 + lengthdir_x(teth, (i + 0.5 + teethT) / prec * 360) * w * 2;
				ty1 = ny1 + lengthdir_y(teth, (i + 0.5 + teethT) / prec * 360) * h * 2;

				Push(triangles, {{Vector2{tx0, ty0}, Vector2{nx1, ny1}, Vector2{nx0, ny0}}});
				Push(triangles, {{Vector2{tx0, ty0}, Vector2{tx1, ty1}, Vector2{nx1, ny1}}});

				Push(segment, {Vector2{tx0, ty0}});
				Push(segment, {Vector2{tx1, ty1}});
			}

			Push(segment, {Vector2{nx1, ny1}});
		}

		return Geometry{{Object{true, triangles}}, segment};
	}
}
