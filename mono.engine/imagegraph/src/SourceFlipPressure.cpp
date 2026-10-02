#include "SourceFlipDomain.hpp"

namespace engine::imagegraph::detail::source_flip {
	void setBoundary(SourceDomain domain, DoubleBuffer a) {
		int x = domain.fNumX;
		int y = domain.fNumY;
		int c = domain.collideWall;

		if ((c & 0b0001) == 0)
			for (int j = 0; j < y; j++) {
				a[getIndex(j, 1, y)] = a[getIndex(j, 2, y)];
				a[getIndex(j, 0, y)] = a[getIndex(j, 1, y)];
			}
		if ((c & 0b0010) == 0)
			for (int j = 0; j < y; j++) {
				a[getIndex(j, y - 1, y)] = a[getIndex(j, y - 2, y)];
			}
		if ((c & 0b0100) == 0)
			for (int i = 0; i < x; i++) {
				a[getIndex(1, i, y)] = a[getIndex(2, i, y)];
				a[getIndex(0, i, y)] = a[getIndex(1, i, y)];
			}
		if ((c & 0b1000) == 0)
			for (int i = 0; i < x; i++) {
				a[getIndex(x - 1, i, y)] = a[getIndex(x - 2, i, y)];
			}

		a[0] = 0.5 * (a[1] + a[y]);
		a[y - 1] = 0.5 * (a[y - 2] + a[2 * y - 1]);
		a[(x - 1) * y] = 0.5 * (a[(x - 2) * y] + a[(x - 1) * y + 1]);
		a[x * y - 1] = 0.5 * (a[(x - 1) * y - 1] + a[x * y - 2]);
	}
	void solveIncompressibility(SourceDomain domain) {
		DoubleBuffer u = domain.u;
		DoubleBuffer v = domain.v;
		DoubleBuffer p = domain.p;
		DoubleBuffer prevU = domain.prevU;
		DoubleBuffer prevV = domain.prevV;
		DoubleBuffer cellType = domain.cellType;
		DoubleBuffer particleDensity = domain.particleDensity;

		double fNumX = domain.fNumX;
		double fNumY = domain.fNumY;
		double fNumCells = domain.fNumCells;
		double overRelaxation = domain.overRelaxation;
		double particleRestDensity = domain.particleRestDensity;

		for (int i = 0; i < fNumCells; i++) {
			prevU[i] = u[i];
			prevV[i] = v[i];
			p[i] = 0;
		}

		int n = domain.fNumY;
		double cp = domain.density * domain.h / domain.dt;

		for (int iter = 0; iter < domain.numPressureIterations; iter++) {

			for (int i = 1; i < fNumX - 1; i++)
				for (int j = 1; j < fNumY - 1; j++) {
					if (cellType[i * n + j] != fluid) continue;

					int left = (i - 1) * n + j;
					int right = (i + 1) * n + j;
					int center = i * n + j;
					int bottom = i * n + j - 1;
					int top = i * n + j + 1;

					double sx0 = getCellOpen(domain, left);
					double sx1 = getCellOpen(domain, right);
					double sy0 = getCellOpen(domain, bottom);
					double sy1 = getCellOpen(domain, top);
					double _s = sx0 + sx1 + sy0 + sy1;
					if (_s == 0) continue;

					double ux1 = u[right];
					double vy1 = v[top];
					double _div = ux1 - u[center] + vy1 - v[center];

					if (particleRestDensity > 0) {
						double compression = particleDensity[i * n + j] - particleRestDensity;
						if (compression > 0) _div = _div - compression;
					}

					double _p = -_div / _s;
					_p *= overRelaxation;
					p[center] += cp * _p;

					u[center] -= sx0 * _p;
					v[center] -= sy0 * _p;

					u[right] += sx1 * _p;
					v[top] += sy1 * _p;
				}
		}

		setBoundary(domain, p);
		setBoundary(domain, u);
		setBoundary(domain, v);
	}
}
