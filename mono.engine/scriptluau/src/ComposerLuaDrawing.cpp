#include "ComposerLuaState.hpp"

#include <algorithm>
#include <numbers>
namespace engine::script::composer_lua {
	namespace {
		SurfacePixel ColourPixel(uint32_t colour, double alpha) {
			return {
				double(colour & 255) / 255,
				double((colour >> 8) & 255) / 255,
				double((colour >> 16) & 255) / 255,
				std::clamp(alpha, 0., 1.)
			};
		}
		void Pixel(lua_State *state, int64_t x, int64_t y, SurfacePixel source) {
			auto &session = Of(state);
			if (!session.Target) luaL_errorL(state, "drawing requires Lua Surface target");
			if (!ChargePixel(session)) luaL_errorL(state, "Lua drawing exceeded pixel budget");
			if (x < 0 || y < 0 || x >= session.Target->Width || y >= session.Target->Height) return;
			SurfacePixel previous{};
			LoadSurfacePixel(*session.Target, uint32_t(x), uint32_t(y), previous);
			const double alpha = source[3];
			SurfacePixel output{};
			for (size_t channel = 0; channel < 3; ++channel) {
				const double scaled = source[channel] * alpha;
				output[channel] = session.Blend == 1   ? previous[channel] + scaled
								  : session.Blend == 2 ? previous[channel] * source[channel]
								  : session.Blend == 3 ? previous[channel] - scaled
													   : scaled + previous[channel] * (1 - alpha);
			}
			output[3] = alpha + previous[3] * (1 - alpha);
			StoreSurfacePixel(*session.Target, uint32_t(x), uint32_t(y), output);
		}
		int Clear(lua_State *state) {
			auto &session = Of(state);
			if (!session.Target) luaL_errorL(state, "clear requires Lua Surface target");
			const auto pixel = ColourPixel(Packed(Number(state, 1)), Number(state, 2));
			for (uint32_t y = 0; y < session.Target->Height; ++y)
				for (uint32_t x = 0; x < session.Target->Width; ++x) {
					if (!ChargePixel(session)) luaL_errorL(state, "Lua drawing exceeded pixel budget");
					StoreSurfacePixel(*session.Target, x, y, pixel);
				}
			return 0;
		}
		int SetColour(lua_State *state) {
			Of(state).Colour = Packed(Number(state, 1, 0xffffff));
			return 0;
		}
		int SetAlpha(lua_State *state) {
			Of(state).Alpha = std::clamp(Number(state, 1, 1), 0., 1.);
			return 0;
		}
		int SetColourAlpha(lua_State *state) {
			SetColour(state);
			Of(state).Alpha = std::clamp(Number(state, 2, 1), 0., 1.);
			return 0;
		}
		int GetColour(lua_State *state) {
			lua_pushnumber(state, Of(state).Colour);
			return 1;
		}
		int GetAlpha(lua_State *state) {
			lua_pushnumber(state, Of(state).Alpha);
			return 1;
		}
		int SetBlend(lua_State *state) {
			const auto blend = int64_t(Number(state, 1));
			if (blend < 0 || blend > 3) luaL_errorL(state, "blend mode must be Normal/Add/Multiply/Subtract");
			Of(state).Blend = int(blend);
			return 0;
		}
		int ResetBlend(lua_State *state) {
			Of(state).Blend = 0;
			return 0;
		}
		int DrawPixel(lua_State *state) {
			auto &session = Of(state);
			Pixel(
				state,
				int64_t(Number(state, 1)),
				int64_t(Number(state, 2)),
				ColourPixel(session.Colour, session.Alpha)
			);
			return 0;
		}
		int Shape(lua_State *state) {
			auto &session = Of(state);
			if (!session.Target) luaL_errorL(state, "drawing requires Lua Surface target");
			const int kind = int(lua_tointeger(state, lua_upvalueindex(1)));
			const double a = Number(state, 1), b = Number(state, 2), c = Number(state, 3),
						 d = kind == 2 || kind == 3 ? c : Number(state, 4),
						 thickness = std::max(0., Number(state, kind == 2 || kind == 3 ? 4 : 5, 1));
			double left = a, top = b, right = c, bottom = d;
			if (kind == 2 || kind == 3) {
				left = a - c;
				right = a + c;
				top = b - c;
				bottom = b + c;
			} else {
				if (left > right) std::swap(left, right);
				if (top > bottom) std::swap(top, bottom);
			}
			if (std::abs(left) > 0x1p40 || std::abs(top) > 0x1p40 || std::abs(right) > 0x1p40 ||
				std::abs(bottom) > 0x1p40)
				luaL_errorL(state, "drawing coordinates exceed bounded range");
			const int64_t x0 = int64_t(std::max(0., std::floor(left - thickness))),
						  x1 = int64_t(
							  std::min(double(session.Target->Width - 1), std::ceil(right + thickness))
						  ),
						  y0 = int64_t(std::max(0., std::floor(top - thickness))),
						  y1 = int64_t(
							  std::min(double(session.Target->Height - 1), std::ceil(bottom + thickness))
						  );
			const auto colour = ColourPixel(session.Colour, session.Alpha);
			for (int64_t y = y0; y <= y1; ++y)
				for (int64_t x = x0; x <= x1; ++x) {
					if (!ChargePixel(session)) luaL_errorL(state, "Lua drawing exceeded pixel budget");
					const double px = double(x) + 0.5, py = double(y) + 0.5;
					bool covered = false;
					if (kind == 0 || kind == 1) {
						covered = px >= left && px <= right && py >= top && py <= bottom;
						if (kind == 1)
							covered = covered && (px < left + thickness || px > right - thickness ||
												  py < top + thickness || py > bottom - thickness);
					} else if (kind >= 2 && kind <= 5) {
						const double cx = (left + right) / 2, cy = (top + bottom) / 2,
									 rx = (right - left) / 2, ry = (bottom - top) / 2;
						if (rx > 0 && ry > 0) {
							const double distance = std::pow((px - cx) / rx, 2) + std::pow((py - cy) / ry, 2);
							covered = distance <= 1;
							if (kind == 3 || kind == 5) {
								const double innerX = std::max(0., rx - thickness),
											 innerY = std::max(0., ry - thickness);
								covered =
									covered &&
									(innerX == 0 || innerY == 0 ||
									 std::pow((px - cx) / innerX, 2) + std::pow((py - cy) / innerY, 2) >= 1);
							}
						}
					} else {
						const double vx = c - a, vy = d - b, length = vx * vx + vy * vy;
						double along = length ? ((px - a) * vx + (py - b) * vy) / length : 0;
						const bool inSegment = along >= 0 && along <= 1;
						along = std::clamp(along, 0., 1.);
						const double dx = px - (a + along * vx), dy = py - (b + along * vy);
						covered = dx * dx + dy * dy <= thickness * thickness / 4 && (kind == 7 || inSegment);
					}
					if (covered) Pixel(state, x, y, colour);
				}
			return 0;
		}
		int Sample(lua_State *state) {
			const bool other = lua_toboolean(state, lua_upvalueindex(1));
			auto &session = Of(state);
			Image *image = other ? Surface(state, 1) : session.Target;
			const int position = other ? 2 : 1;
			const double x = Number(state, position), y = Number(state, position + 1);
			SurfacePixel pixel{};
			if (image && x >= 0 && y >= 0 && x < image->Width && y < image->Height) {
				if (!ChargePixel(session)) luaL_errorL(state, "Lua sampling exceeded pixel budget");
				LoadSurfacePixel(*image, uint32_t(x), uint32_t(y), pixel);
			}
			const auto byte = [](double v) { return uint32_t(std::nearbyint(std::clamp(v, 0., 1.) * 255)); };
			lua_pushnumber(
				state,
				byte(pixel[0]) | (byte(pixel[1]) << 8) | (byte(pixel[2]) << 16) | (byte(pixel[3]) << 24)
			);
			return 1;
		}
		int SurfaceSize(lua_State *state) {
			const Image *image = Surface(state, 1);
			const int property = int(lua_tointeger(state, lua_upvalueindex(1)));
			lua_pushnumber(
				state,
				!image			? 0
				: property == 0 ? image->Width
				: property == 1 ? image->Height
								: uint8_t(image->Format)
			);
			return 1;
		}
		int Draw(lua_State *state) {
			const Image *image = Surface(state, 1);
			if (!image) return 0;
			auto &session = Of(state);
			if (!session.Target) luaL_errorL(state, "draw requires Lua Surface target");
			const int mode = int(lua_tointeger(state, lua_upvalueindex(1)));
			const double x = Number(state, 2), y = Number(state, 3);
			double sx = 1, sy = 1, rotation = 0, alpha = 1;
			uint32_t colour = 0xffffff;
			if (mode == 1) {
				colour = Packed(Number(state, 4, 0xffffff));
				alpha = Number(state, 5, 1);
			} else if (mode == 2 || mode == 3) {
				sx = Number(state, 4, 1);
				sy = Number(state, 5, mode == 2 ? sx : 1);
				rotation = Number(state, 6);
				if (mode == 3) {
					colour = Packed(Number(state, 7, 0xffffff));
					alpha = Number(state, 8, 1);
				}
			}
			if (sx == 0 || sy == 0) return 0;
			const auto tint = ColourPixel(colour, alpha);
			const double angle = rotation * std::numbers::pi / 180, cosine = std::cos(angle),
						 sine = std::sin(angle);
			for (uint32_t py = 0; py < session.Target->Height; ++py)
				for (uint32_t px = 0; px < session.Target->Width; ++px) {
					if (!ChargePixel(session)) luaL_errorL(state, "Lua drawing exceeded pixel budget");
					const double dx = double(px) + 0.5 - x, dy = double(py) + 0.5 - y,
								 u = (cosine * dx - sine * dy) / sx, v = (sine * dx + cosine * dy) / sy;
					if (u < 0 || v < 0 || u >= image->Width || v >= image->Height) continue;
					SurfacePixel pixel{};
					LoadSurfacePixel(*image, uint32_t(u), uint32_t(v), pixel);
					for (size_t channel = 0; channel < 4; ++channel)
						pixel[channel] *= tint[channel];
					Pixel(state, px, py, pixel);
				}
			return 0;
		}
		int Seed(lua_State *state) {
			Of(state).RandomState = uint64_t(int64_t(Number(state, 1, 1))) | 1;
			return 0;
		}
		int Random(lua_State *state) {
			auto &session = Of(state);
			auto &seed = session.RandomState;
			seed ^= seed << 13;
			seed ^= seed >> 7;
			seed ^= seed << 17;
			const double unit = double(seed >> 11) / double(uint64_t{1} << 53);
			const bool integer = lua_toboolean(state, lua_upvalueindex(1));
			const double high = Number(state, 1, 1);
			lua_pushnumber(
				state, integer ? std::floor(unit * (std::floor(std::max(0., high)) + 1)) : unit * high
			);
			return 1;
		}
		int Print(lua_State *state) {
			auto &session = Of(state);
			size_t size = 0;
			const char *text = luaL_checklstring(state, 1, &size);
			const uint64_t bytes = size + session.ActiveNode.size() + sizeof(ComposerLuaMessage);
			if (session.Messages.size() >= 256 || bytes > 65536 - session.MessageBytes ||
				bytes >
					session.Memory->MaximumBytes - session.Memory->VmBytes - session.Memory->ExternalBytes)
				luaL_errorL(state, "Lua print messages exceed their budget");
			session.Messages.push_back(
				{session.ActiveNode, std::string(text, size), session.Memory->MessageOrder++}
			);
			session.MessageBytes += bytes;
			session.Memory->ExternalBytes += bytes;
			return 0;
		}
	}
	void InstallDrawing(lua_State *state) {
		for (const auto &[name, fn] :
			 {std::pair<const char *, lua_CFunction>{"clear", Clear},
			  {"setColor", SetColour},
			  {"setAlpha", SetAlpha},
			  {"setColorAlpha", SetColourAlpha},
			  {"getCurrentColor", GetColour},
			  {"getCurrentAlpha", GetAlpha},
			  {"setBlend", SetBlend},
			  {"resetBlend", ResetBlend},
			  {"drawPixel", DrawPixel},
			  {"setSeed", Seed},
			  {"randomize", Seed},
			  {"print", Print}}) {
			lua_pushcfunction(state, fn, name);
			lua_setglobal(state, name);
		}
		const char *shapes[] = {
			"drawRect",
			"drawRectOutline",
			"drawCircle",
			"drawCircleOutline",
			"drawEllipse",
			"drawEllipseOutline",
			"drawLine",
			"drawLineRound"
		};
		for (int i = 0; i < 8; ++i) {
			lua_pushinteger(state, i);
			lua_pushcclosure(state, Shape, shapes[i], 1);
			lua_setglobal(state, shapes[i]);
		}
		const char *draws[] = {"draw", "drawBlend", "drawTransform", "drawGeneral"};
		for (int i = 0; i < 4; ++i) {
			lua_pushinteger(state, i);
			lua_pushcclosure(state, Draw, draws[i], 1);
			lua_setglobal(state, draws[i]);
		}
		for (int i = 0; i < 2; ++i) {
			const char *name = i ? "getColorSurface" : "getColor";
			lua_pushboolean(state, i);
			lua_pushcclosure(state, Sample, name, 1);
			lua_setglobal(state, name);
		}
		const char *sizes[] = {"surfaceGetWidth", "surfaceGetHeight", "surfaceGetFormat"};
		for (int i = 0; i < 3; ++i) {
			lua_pushinteger(state, i);
			lua_pushcclosure(state, SurfaceSize, sizes[i], 1);
			lua_setglobal(state, sizes[i]);
		}
		for (int i = 0; i < 2; ++i) {
			const char *name = i ? "irandom" : "random";
			lua_pushboolean(state, i);
			lua_pushcclosure(state, Random, name, 1);
			lua_setglobal(state, name);
		}
	}
}
