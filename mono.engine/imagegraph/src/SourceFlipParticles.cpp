#include "SourceFlipDomain.hpp"

namespace engine::imagegraph::detail::source_flip {
	void integrateParticles(SourceDomain domain) {
		DoubleBuffer particleVel = domain.particleVel;
		DoubleBuffer particlePos = domain.particlePos;
		DoubleBuffer particleLife = domain.particleLife;

		double dt = domain.dt;

		double gx = dt * domain.gravity * cos(domain.gravityDirection * SourcePi / 180);
		double gy = dt * domain.gravity * sin(domain.gravityDirection * SourcePi / 180);

		double _itr = 1 / domain.globalIteration;

		for (int i = 0; i < domain.numParticles; i++) {
			if (isnan(particleVel[2 * i + 1])) particleVel[2 * i + 1] = 0;
			particleVel[2 * i + 1] = clamp(particleVel[2 * i + 1], -1000, 1000);

			if (particlePos[2 * i] == 0 && particlePos[2 * i + 1] == 0) continue;

			particleVel[2 * i] += gx;
			particleVel[2 * i + 1] -= gy;

			particlePos[2 * i] += particleVel[2 * i] * dt;
			particlePos[2 * i + 1] += particleVel[2 * i + 1] * dt;

			particleLife[i] += _itr;
		}
	}
	void pushParticlesApart(SourceDomain domain) {
		int pNumCells = (int)domain.pNumCells;

		DoubleBuffer numCellParticles = domain.numCellParticles;
		DoubleBuffer firstCellParticle = domain.firstCellParticle;
		DoubleBuffer particlePos = domain.particlePos;
		DoubleBuffer particleVel = domain.particleVel;
		DoubleBuffer cellParticleIds = domain.cellParticleIds;

		double numParticles = domain.numParticles;
		double pInvSpacing = domain.pInvSpacing;
		double pNumX = domain.pNumX;
		double pNumY = domain.pNumY;

		double pNumX1 = pNumX - 1;
		double pNumY1 = pNumY - 1;

		int c = domain.collideWall;
		bool cnTop = (c & 0b0001) == 0;
		bool cnBot = (c & 0b0010) == 0;
		bool cnLef = (c & 0b0100) == 0;
		bool cnRig = (c & 0b1000) == 0;

		// count particles per cell
		for (int i = 0; i < pNumCells; i++)
			numCellParticles[i] = 0;

		for (int i = 0; i < numParticles; i++) {
			double _x = particlePos[2 * i];
			double _y = particlePos[2 * i + 1];

			if (_x == 0 && _y == 0) continue;

			double xi = floor(_x * pInvSpacing);
			double yi = floor(_y * pInvSpacing);

			if (cnTop) {
				if (yi < 0) continue;
			} else
				yi = max(yi, 0.);
			if (cnBot) {
				if (yi >= pNumY1) continue;
			} else
				yi = min(yi, pNumY1);
			if (cnLef) {
				if (xi < 0) continue;
			} else
				xi = max(xi, 0.);
			if (cnRig) {
				if (xi >= pNumX1) continue;
			} else
				xi = min(xi, pNumX1);

			int cellNr = xi * pNumY + yi;
			numCellParticles[cellNr]++;
		}

		// partial sums

		double first = 0;

		for (int i = 0; i < pNumCells; i++) {
			first += numCellParticles[i];
			firstCellParticle[i] = first;
		}
		firstCellParticle[pNumCells] = first; // guard

		// fill particles into cells

		for (int i = 0; i < numParticles; i++) {
			double _x = particlePos[2 * i];
			double _y = particlePos[2 * i + 1];

			if (_x == 0 && _y == 0) continue;

			double xi = floor(_x * pInvSpacing);
			double yi = floor(_y * pInvSpacing);

			if (cnTop) {
				if (yi < 0) continue;
			} else
				yi = max(yi, 0.);
			if (cnBot) {
				if (yi >= pNumY1) continue;
			} else
				yi = min(yi, pNumY1);
			if (cnLef) {
				if (xi < 0) continue;
			} else
				xi = max(xi, 0.);
			if (cnRig) {
				if (xi >= pNumX1) continue;
			} else
				xi = min(xi, pNumX1);

			int cellNr = xi * pNumY + yi;
			firstCellParticle[cellNr]--;
			cellParticleIds[(int)firstCellParticle[cellNr]] = i;
		}

		// push particles apart

		double minDist = 2.0 * domain.particleRadius;
		double minDist2 = minDist * minDist;
		double minDistHalf = minDist * 0.5;
		double viscosity = domain.viscosity;
		double invvisc = clamp(viscosity + 1., 0., 1.);

		for (int _m = 0; _m < domain.numParticleIterations; _m++) {
			for (int i = 0; i < numParticles; i++) {
				double px = particlePos[2 * i];
				double py = particlePos[2 * i + 1];

				if (px == 0 && py == 0) continue;

				double pxi = floor(px * pInvSpacing);
				double pyi = floor(py * pInvSpacing);

				if (cnTop && pyi < 0) continue;
				if (cnBot && pyi >= pNumY1) continue;
				if (cnLef && pxi < 0) continue;
				if (cnRig && pxi >= pNumX1) continue;

				double x0 = max(pxi - 1, 0.);
				double y0 = max(pyi - 1, 0.);
				double x1 = min(pxi + 1, pNumX1);
				double y1 = min(pyi + 1, pNumY1);

				int _i2 = i * 2;

				for (double xi = x0; xi <= x1; xi++)
					for (double yi = y0; yi <= y1; yi++) {
						int cellNr = xi * pNumY + yi;
						double first = firstCellParticle[cellNr];
						double last = firstCellParticle[cellNr + 1];

						for (int j = first; j < last; j++) {
							int _id = cellParticleIds[j];
							if (_id == i) continue;

							int _id2 = _id * 2;

							double qx = particlePos[_id2];
							double qy = particlePos[_id2 + 1];

							if (qx == 0 && qy == 0) continue;

							double dx = qx - px;
							double dy = qy - py;
							double d2 = dx * dx + dy * dy;

							if (d2 == 0) continue;
							if (d2 > minDist2 * 2) continue;

							if (d2 >
								minDist2) { // viscosity calculation: attract nearby particle from r/2 to r
								if (viscosity == 0) continue;

								double d = sqrt(d2) - minDistHalf;
								double s = max(0., minDistHalf / d - 0.5);
								dx *= s * viscosity;
								dy *= s * viscosity;

								particlePos[_i2] += dx;
								particlePos[_i2 + 1] += dy;
								particlePos[_id2] -= dx;
								particlePos[_id2 + 1] -= dy;
								continue;
							}

							double d = sqrt(d2);
							double s = minDistHalf / d - 0.5;
							dx *= s * invvisc;
							dy *= s * invvisc;

							particlePos[_i2] -= dx;
							particlePos[_i2 + 1] -= dy;
							particlePos[_id2] += dx;
							particlePos[_id2 + 1] += dy;
						}
					}
			}
		}
	}
}
