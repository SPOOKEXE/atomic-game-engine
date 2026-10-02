#include "SourceFlipDomain.hpp"

namespace engine::imagegraph::detail::source_flip {
	void updateParticleDensity(SourceDomain domain) {
		DoubleBuffer particlePos = domain.particlePos;
		DoubleBuffer d = domain.particleDensity;
		DoubleBuffer cellType = domain.cellType;

		double n = domain.fNumY;
		double h = domain.h;
		double h1 = domain.fInvSpacing;
		double h2 = 0.5 * domain.h;
		double fNumX = domain.fNumX;
		double fNumY = domain.fNumY;
		double fNumX1 = domain.fNumX1;
		double fNumY1 = domain.fNumY1;
		double fNumCells = domain.fNumCells;

		double numParticles = domain.numParticles;
		double particleRestDensity = domain.particleRestDensity;

		int c = domain.collideWall;
		bool cnTop = (c & 0b0001) == 0;
		bool cnBot = (c & 0b0010) == 0;
		bool cnLef = (c & 0b0100) == 0;
		bool cnRig = (c & 0b1000) == 0;

		for (int i = 0; i < fNumCells; i++)
			d[i] = 0;

		for (int i = 0; i < numParticles; i++) {
			int i2 = i * 2;

			double _x = particlePos[i2];
			double _y = particlePos[i2 + 1];

			if (_x == 0 && _y == 0) continue;

			if (cnTop) {
				if (_y < h) continue;
			} else
				_y = max(_y, h);
			if (cnBot) {
				if (_y > fNumY1 * h) continue;
			} else
				_y = min(_y, fNumY1 * h);
			if (cnLef) {
				if (_x < h) continue;
			} else
				_x = max(_x, h);
			if (cnRig) {
				if (_x > fNumX1 * h) continue;
			} else
				_x = min(_x, fNumX1 * h);

			double x0 = floor((_x - h2) * h1);
			double tx = ((_x - h2) - x0 * h) * h1;
			double x1 = min(x0 + 1, fNumX1);

			double y0 = floor((_y - h2) * h1);
			double ty = ((_y - h2) - y0 * h) * h1;
			double y1 = min(y0 + 1, fNumY1);

			double sx = 1.0 - tx;
			double sy = 1.0 - ty;

			if (x0 < fNumX && y0 < fNumY) d[(int)(x0 * n + y0)] += sx * sy;
			if (x1 < fNumX && y0 < fNumY) d[(int)(x1 * n + y0)] += tx * sy;
			if (x1 < fNumX && y1 < fNumY) d[(int)(x1 * n + y1)] += tx * ty;
			if (x0 < fNumX && y1 < fNumY) d[(int)(x0 * n + y1)] += sx * ty;
		}

		if (particleRestDensity == 0) {
			double sum = 0;
			double numFluidCells = 0;

			for (int i = 0; i < fNumCells; i++) {
				if (cellType[i] == fluid) {
					sum += d[i];
					numFluidCells++;
				}
			}

			if (numFluidCells > 0) particleRestDensity = sum / numFluidCells;
		}
	}
	void transferVelocities(SourceDomain domain, bool toGrid) {
		DoubleBuffer u = domain.u;
		DoubleBuffer v = domain.v;
		DoubleBuffer du = domain.du;
		DoubleBuffer dv = domain.dv;
		DoubleBuffer prevU = domain.prevU;
		DoubleBuffer prevV = domain.prevV;
		DoubleBuffer s = domain.s;
		DoubleBuffer cellType = domain.cellType;
		DoubleBuffer particlePos = domain.particlePos;
		DoubleBuffer particleVel = domain.particleVel;

		int n = domain.fNumY;
		double h = domain.h;
		double h1 = domain.fInvSpacing;
		double h2 = 0.5 * domain.h;
		double fNumX = domain.fNumX;
		double fNumY = domain.fNumY;
		double fNumX1 = domain.fNumX1;
		double fNumY1 = domain.fNumY1;
		double fNumCells = domain.fNumCells;
		double flipRatio = domain.flipRatio;
		double friction = domain.friction;

		double numParticles = domain.numParticles;
		double velocityDamping = domain.velocityDamping;

		int c = domain.collideWall;
		bool cnTop = (c & 0b0001) == 0;
		bool cnBot = (c & 0b0010) == 0;
		bool cnLef = (c & 0b0100) == 0;
		bool cnRig = (c & 0b1000) == 0;

		if (toGrid) {
			for (int i = 0; i < fNumCells; i++) {
				prevU[i] = u[i] * velocityDamping;
				prevV[i] = v[i] * velocityDamping;

				du[i] = 0;
				dv[i] = 0;
				u[i] = 0;
				v[i] = 0;

				cellType[i] = getCellOpen(domain, i) == 0.0 ? solid : air;
			}

			for (int i = 0; i < numParticles; i++) {
				int i2 = i * 2;

				double _x = particlePos[i2];
				double _y = particlePos[i2 + 1];

				if (_x == 0 && _y == 0) continue;

				int xi = floor(_x * h1);
				int yi = floor(_y * h1);

				if (cnTop) {
					if (yi < 0) continue;
				} else
					yi = max(yi, 0);
				if (cnBot) {
					if (yi > fNumY1) continue;
				} else
					yi = min(yi, (int)fNumY1);
				if (cnLef) {
					if (xi < 0) continue;
				} else
					xi = max(xi, 0);
				if (cnRig) {
					if (xi > fNumX1) continue;
				} else
					xi = min(xi, (int)fNumX1);

				int cellNr = xi * n + yi;
				if (cellType[cellNr] == air) cellType[cellNr] = fluid;
			}
		}

		double _fNxH = fNumX1 * h;
		double _fNyH = fNumY1 * h;
		double invFlipRatio = 1.0 - flipRatio;

		for (int _com = 0; _com < 2; _com++) {
			double dx = _com == 0 ? 0 : h2;
			double dy = _com == 0 ? h2 : 0;

			DoubleBuffer f = _com == 0 ? u : v;
			DoubleBuffer prevF = _com == 0 ? prevU : prevV;
			DoubleBuffer d = _com == 0 ? du : dv;

			for (int i = 0; i < numParticles; i++) {
				int i2 = i * 2;

				double _x = particlePos[i2];
				double _y = particlePos[i2 + 1];

				if (_x == 0 && _y == 0) continue;

				if (cnTop) {
					if (_y < h) continue;
				} else
					_y = max(_y, h);
				if (cnBot) {
					if (_y > _fNyH) continue;
				} else
					_y = min(_y, _fNyH);
				if (cnLef) {
					if (_x < h) continue;
				} else
					_x = max(_x, h);
				if (cnRig) {
					if (_x > _fNxH) continue;
				} else
					_x = min(_x, _fNxH);

				double x0 = clamp(floor((_x - dx) * h1), 0, fNumX1);
				double tx = ((_x - dx) - x0 * h) * h1;
				double x1 = clamp(x0 + 1, 0, fNumX1);

				double y0 = clamp(floor((_y - dy) * h1), 0, fNumY1);
				double ty = ((_y - dy) - y0 * h) * h1;
				double y1 = clamp(y0 + 1, 0, fNumY1);

				double sx = 1.0 - tx;
				double sy = 1.0 - ty;

				double d0 = sx * sy;
				double d1 = tx * sy;
				double d2 = tx * ty;
				double d3 = sx * ty;

				int nr0 = x0 * n + y0;
				int nr1 = x1 * n + y0;
				int nr2 = x1 * n + y1;
				int nr3 = x0 * n + y1;

				if (toGrid) {
					double pv = particleVel[i2 + _com];
					f[nr0] += pv * d0;
					d[nr0] += d0;
					f[nr1] += pv * d1;
					d[nr1] += d1;
					f[nr2] += pv * d2;
					d[nr2] += d2;
					f[nr3] += pv * d3;
					d[nr3] += d3;
				} else {
					int offset = _com == 0 ? n : 1;
					double valid0 = cellType[nr0] != air || cellType[nr0 - offset] != air ? 1.0 : 0;
					double valid1 = cellType[nr1] != air || cellType[nr1 - offset] != air ? 1.0 : 0;
					double valid2 = cellType[nr2] != air || cellType[nr2 - offset] != air ? 1.0 : 0;
					double valid3 = cellType[nr3] != air || cellType[nr3 - offset] != air ? 1.0 : 0;

					double _v = particleVel[i2 + _com];
					double _d = valid0 * d0 + valid1 * d1 + valid2 * d2 + valid3 * d3;

					if (_d > 0) {
						double picV = (valid0 * d0 * f[nr0] + valid1 * d1 * f[nr1] + valid2 * d2 * f[nr2] +
									   valid3 * d3 * f[nr3]) /
									  _d;

						double corr =
							(valid0 * d0 * (f[nr0] - prevF[nr0]) + valid1 * d1 * (f[nr1] - prevF[nr1]) +
							 valid2 * d2 * (f[nr2] - prevF[nr2]) + valid3 * d3 * (f[nr3] - prevF[nr3])) /
							_d;

						double flipV = _v + corr;
						double vel = (invFlipRatio * picV + flipRatio * flipV) * friction;
						vel = clamp(vel, -1000, 1000);

						particleVel[i2 + _com] = isnan(vel) ? 0 : vel;
					}
				}
			}

			if (toGrid) {
				for (int i = 0; i < fNumCells; i++)
					if (d[i] > 0) f[i] /= d[i];

				for (int i = 0; i < fNumX; i++)
					for (int j = 0; j < fNumY; j++) {

						int index = i * n + j;
						bool _solid = cellType[index] == solid;

						if (_solid || (i > 0 && cellType[(i - 1) * n + j] == solid)) u[index] = prevU[index];

						if (_solid || (j > 0 && cellType[i * n + j - 1] == solid)) v[index] = prevV[index];
					}
			}

			setBoundary(domain, u);
			setBoundary(domain, v);
		}
	}
}
