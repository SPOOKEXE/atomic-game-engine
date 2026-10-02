#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace engine::imagegraph::detail::source_oklch {
	using Triplet = std::array<double, 3>;
	inline double Sign(double value) {
		return value < 0 ? -1 : value > 0 ? 1 : 0;
	}
	inline double Clamp(double value, double low, double high) {
		return std::clamp(value, low, high);
	}
	inline double Max(double a, double b) {
		return std::max(a, b);
	}
	// grug retain pinned okhsl_function.gml coefficients and distinct transfer paths for each clip mode.
	inline Triplet RgbToLinear(const Triplet &rgb) {
		Triplet result{};
		for (auto i = 0; i < 3; i++) {
			auto c = rgb[i];
			result[i] = std::abs(c) <= 0.04045
							? c / 12.92
							: (c < 0 ? -1 : 1) * std::pow((std::abs(c) + 0.055) / 1.055, 2.4);
		}
		return result;
	}

	inline Triplet LinearToRgb(const Triplet &rgb) {
		Triplet result{};
		for (auto i = 0; i < 3; i++) {
			auto c = rgb[i];
			result[i] = std::abs(c) > 0.0031308
							? (c < 0 ? -1 : 1) * (1.055 * std::pow(std::abs(c), 1.0 / 2.4) - 0.055)
							: 12.92 * c;
		}
		return result;
	}

	inline Triplet LinearToOklab(const Triplet &rgb) {
		auto r = rgb[0];
		auto g = rgb[1];
		auto b = rgb[2];

		auto l = 0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b;
		auto m = 0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b;
		auto s = 0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b;

		auto l_ = std::pow(l, 1.0 / 3.0);
		auto m_ = std::pow(m, 1.0 / 3.0);
		auto s_ = std::pow(s, 1.0 / 3.0);

		return {
			0.2104542683093140 * l_ + 0.7936177747023054 * m_ - 0.0040720430116193 * s_,
			1.9779985324311684 * l_ - 2.4285922420485799 * m_ + 0.4505937096174110 * s_,
			0.0259040424655478 * l_ + 0.7827717124575296 * m_ - 0.8086757549230774 * s_,
		};
	}

	inline Triplet OklabToLinear(const Triplet &lab) {
		auto L = lab[0];
		auto a = lab[1];
		auto b = lab[2];

		auto l_ = L + 0.3963377774 * a + 0.2158037573 * b;
		auto m_ = L - 0.1055613458 * a - 0.0638541728 * b;
		auto s_ = L - 0.0894841775 * a - 1.2914855480 * b;

		auto l = std::pow(l_, 3);
		auto m = std::pow(m_, 3);
		auto s = std::pow(s_, 3);

		return {
			+4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s,
			-1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s,
			-0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s,
		};
	}

	inline Triplet SourceOklabToLinear(const Triplet &lab) {
		auto l = lab[0];
		auto a = lab[1];
		auto b = lab[2];

		auto L =
			std::pow(l * 0.99999999845051981432 + 0.39633779217376785678 * a + 0.21580375806075880339 * b, 3);
		auto M =
			std::pow(l * 1.0000000088817607767 - 0.1055613423236563494 * a - 0.063854174771705903402 * b, 3);
		auto S =
			std::pow(l * 1.0000000546724109177 - 0.089484182094965759684 * a - 1.2914855378640917399 * b, 3);

		return {
			+4.076741661347994 * L - 3.307711590408193 * M + 0.230969928729428 * S,
			-1.2684380040921763 * L + 2.6097574006633715 * M - 0.3413193963102197 * S,
			-0.004196086541837188 * L - 0.7034186144594493 * M + 1.7076147009309444 * S
		};
	}

	inline Triplet SourceLinearToOklab(const Triplet &rgb) {
		auto r = rgb[0];
		auto g = rgb[1];
		auto b = rgb[2];

		auto L = std::pow(0.41222147079999993 * r + 0.5363325363 * g + 0.0514459929 * b, 1.0 / 3.0);
		auto M = std::pow(0.2119034981999999 * r + 0.6806995450999999 * g + 0.1073969566 * b, 1.0 / 3.0);
		auto S = std::pow(0.08830246189999998 * r + 0.2817188376 * g + 0.6299787005000002 * b, 1.0 / 3.0);

		return {
			0.2104542553 * L + 0.793617785 * M - 0.0040720468 * S,
			1.9779984951 * L - 2.428592205 * M + 0.4505937099 * S,
			0.0259040371 * L + 0.7827717662 * M - 0.808675766 * S
		};
	}

	inline double MaxSaturation(double a, double b) {

		double k0, k1, k2, k3, k4, wl, wm, ws;

		if (-1.88170328 * a - 0.80936493 * b > 1) {

			k0 = +1.19086277;
			k1 = +1.76576728;
			k2 = +0.59662641;
			k3 = +0.75515197;
			k4 = +0.56771245;
			wl = +4.0767416621;
			wm = -3.3077115913;
			ws = +0.2309699292;

		} else if (1.81444104 * a - 1.19445276 * b > 1) {

			k0 = +0.73956515;
			k1 = -0.45954404;
			k2 = +0.08285427;
			k3 = +0.12541070;
			k4 = +0.14503204;
			wl = -1.2684380046;
			wm = +2.6097574011;
			ws = -0.3413193965;

		} else {

			k0 = +1.35733652;
			k1 = -0.00915799;
			k2 = -1.15130210;
			k3 = -0.50559606;
			k4 = +0.00692167;
			wl = -0.0041960863;
			wm = -0.7034186147;
			ws = +1.7076147010;
		}

		auto S = k0 + k1 * a + k2 * b + k3 * a * a + k4 * a * b;

		auto k_l = +0.3963377774 * a + 0.2158037573 * b;
		auto k_m = -0.1055613458 * a - 0.0638541728 * b;
		auto k_s = -0.0894841775 * a - 1.2914855480 * b;

		auto l_ = 1. + S * k_l;
		auto m_ = 1. + S * k_m;
		auto s_ = 1. + S * k_s;

		auto l = l_ * l_ * l_;
		auto m = m_ * m_ * m_;
		auto s = s_ * s_ * s_;

		auto l_dS = 3. * k_l * l_ * l_;
		auto m_dS = 3. * k_m * m_ * m_;
		auto s_dS = 3. * k_s * s_ * s_;

		auto l_dS2 = 6. * k_l * k_l * l_;
		auto m_dS2 = 6. * k_m * k_m * m_;
		auto s_dS2 = 6. * k_s * k_s * s_;

		auto f = wl * l + wm * m + ws * s;
		auto f1 = wl * l_dS + wm * m_dS + ws * s_dS;
		auto f2 = wl * l_dS2 + wm * m_dS2 + ws * s_dS2;

		S = S - f * f1 / (f1 * f1 - 0.5 * f * f2);

		return S;
	}

	inline Triplet Cusp(double a, double b) {

		auto S_cusp = MaxSaturation(a, b);

		auto rgb_at_max = OklabToLinear({1, S_cusp * a, S_cusp * b});
		auto L_cusp = std::pow(1 / std::max({rgb_at_max[0], rgb_at_max[1], rgb_at_max[2]}), 1.0 / 3.0);
		auto C_cusp = L_cusp * S_cusp;

		return {L_cusp, C_cusp};
	}

	inline double Intersection(double a, double b, double L1, double C1, double L0) {

		auto cusp = Cusp(a, b);

		double t;
		if (((L1 - L0) * cusp[1] - (cusp[0] - L0) * C1) <= 0.) {

			t = cusp[1] * L0 / (C1 * cusp[0] + cusp[1] * (L0 - L1));

		} else {

			t = cusp[1] * (L0 - 1.) / (C1 * (cusp[0] - 1.) + cusp[1] * (L0 - L1));

			auto dL = L1 - L0;
			auto dC = C1;

			auto k_l = +0.3963377774 * a + 0.2158037573 * b;
			auto k_m = -0.1055613458 * a - 0.0638541728 * b;
			auto k_s = -0.0894841775 * a - 1.2914855480 * b;

			auto l_dt = dL + dC * k_l;
			auto m_dt = dL + dC * k_m;
			auto s_dt = dL + dC * k_s;

			auto L = L0 * (1. - t) + t * L1;
			auto C = t * C1;

			auto l_ = L + C * k_l;
			auto m_ = L + C * k_m;
			auto s_ = L + C * k_s;

			auto l = l_ * l_ * l_;
			auto m = m_ * m_ * m_;
			auto s = s_ * s_ * s_;

			auto ldt = 3 * l_dt * l_ * l_;
			auto mdt = 3 * m_dt * m_ * m_;
			auto sdt = 3 * s_dt * s_ * s_;

			auto ldt2 = 6 * l_dt * l_dt * l_;
			auto mdt2 = 6 * m_dt * m_dt * m_;
			auto sdt2 = 6 * s_dt * s_dt * s_;

			auto r = 4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s - 1;
			auto r1 = 4.0767416621 * ldt - 3.3077115913 * mdt + 0.2309699292 * sdt;
			auto r2 = 4.0767416621 * ldt2 - 3.3077115913 * mdt2 + 0.2309699292 * sdt2;

			auto u_r = r1 / (r1 * r1 - 0.5 * r * r2);
			auto t_r = -r * u_r;

			auto g = -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s - 1;
			auto g1 = -1.2684380046 * ldt + 2.6097574011 * mdt - 0.3413193965 * sdt;
			auto g2 = -1.2684380046 * ldt2 + 2.6097574011 * mdt2 - 0.3413193965 * sdt2;

			auto u_g = g1 / (g1 * g1 - 0.5 * g * g2);
			auto t_g = -g * u_g;

			auto b0 = -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s - 1;
			auto b1 = -0.0041960863 * ldt - 0.7034186147 * mdt + 1.7076147010 * sdt;
			auto b2 = -0.0041960863 * ldt2 - 0.7034186147 * mdt2 + 1.7076147010 * sdt2;

			auto u_b = b1 / (b1 * b1 - 0.5 * b0 * b2);
			auto t_b = -b0 * u_b;

			t_r = u_r >= 0. ? t_r : 99999.;
			t_g = u_g >= 0. ? t_g : 99999.;
			t_b = u_b >= 0. ? t_b : 99999.;

			t += std::min({t_r, t_g, t_b});
		}

		return t;
	}

	inline Triplet ClipChroma(const Triplet &rgb) {
		if (rgb[0] < 1 && rgb[1] < 1 && rgb[2] < 1 && rgb[0] > 0 && rgb[1] > 0 && rgb[2] > 0) return rgb;

		auto lab = SourceLinearToOklab(RgbToLinear(rgb));
		auto L = lab[0];
		auto eps = 0.00001;
		auto d = std::isnan(lab[1]) || std::isnan(lab[2]) ? 0 : std::sqrt(lab[1] * lab[1] + lab[2] * lab[2]);
		auto C = Max(eps, d);
		auto a_ = lab[1] / C;
		auto b_ = lab[2] / C;

		auto L0 = Clamp(L, 0, 1);

		auto t = Intersection(a_, b_, L, C, L0);
		auto L_clipped = L0 * (1 - t) + t * L;
		auto C_clipped = t * C;

		return LinearToRgb(SourceOklabToLinear({L_clipped, C_clipped * a_, C_clipped * b_}));
	}

	inline Triplet ClipGrey(const Triplet &rgb) {
		if (rgb[0] < 1 && rgb[1] < 1 && rgb[2] < 1 && rgb[0] > 0 && rgb[1] > 0 && rgb[2] > 0) return rgb;

		auto lab = LinearToOklab(rgb);

		auto L = lab[0];
		auto eps = 0.00001;
		auto d = std::isnan(lab[1]) || std::isnan(lab[2]) ? 0 : std::sqrt(lab[1] * lab[1] + lab[2] * lab[2]);
		auto C = Max(eps, d);
		auto a_ = lab[1] / C;
		auto b_ = lab[2] / C;

		auto L0 = 0.5;

		auto t = Intersection(a_, b_, L, C, L0);
		auto L_clipped = L0 * (1 - t) + t * L;
		auto C_clipped = t * C;

		return OklabToLinear({L_clipped, C_clipped * a_, C_clipped * b_});
	}

	inline Triplet ClipAdaptiveGrey(const Triplet &rgb, double alpha = 0.05) {
		if (rgb[0] < 1 && rgb[1] < 1 && rgb[2] < 1 && rgb[0] > 0 && rgb[1] > 0 && rgb[2] > 0) return rgb;

		auto lab = LinearToOklab(rgb);

		auto L = lab[0];
		auto eps = 0.00001;
		auto d = std::isnan(lab[1]) || std::isnan(lab[2]) ? 0 : std::sqrt(lab[1] * lab[1] + lab[2] * lab[2]);
		auto C = Max(eps, d);
		auto a_ = lab[1] / C;
		auto b_ = lab[2] / C;

		double Ld = L - 0.5, L0 = 0;

		if (!std::isnan(Ld)) {
			auto e1 = 0.5 + std::abs(Ld) + alpha * C;
			L0 = 0.5 * (1. + Sign(Ld) * (e1 - std::sqrt(e1 * e1 - 2. * std::abs(Ld))));
		}

		auto t = Intersection(a_, b_, L, C, L0);
		auto L_clipped = L0 * (1. - t) + t * L;
		auto C_clipped = t * C;

		return OklabToLinear({L_clipped, C_clipped * a_, C_clipped * b_});
	}
}
