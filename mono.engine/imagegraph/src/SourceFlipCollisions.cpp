#include "SourceFlipDomain.hpp"

namespace engine::imagegraph::detail::source_flip {
	static inline void resolveSolidGridCollision(
		const SourceDomain &domain, double &px, double &py, double &vx, double &vy, double r
	) {
		double h = 1.0 / domain.fInvSpacing;
		double invH = domain.fInvSpacing;
		double rr = r * r;

		// A few iterations are enough to resolve corner/edge stacking against voxelized solids.
		for (int iter = 0; iter < 3; ++iter) {
			int minI = (int)floor((px - r) * invH);
			int maxI = (int)floor((px + r) * invH);
			int minJ = (int)floor((py - r) * invH);
			int maxJ = (int)floor((py + r) * invH);

			minI = (int)clamp(minI, 0, domain.fNumX1);
			maxI = (int)clamp(maxI, 0, domain.fNumX1);
			minJ = (int)clamp(minJ, 0, domain.fNumY1);
			maxJ = (int)clamp(maxJ, 0, domain.fNumY1);

			bool moved = false;

			for (int i = minI; i <= maxI; ++i)
				for (int j = minJ; j <= maxJ; ++j) {
					if (!isSolidCell(domain, i, j)) continue;

					double left = i * h;
					double right = (i + 1) * h;
					double bottom = j * h;
					double top = (j + 1) * h;

					double cx = clamp(px, left, right);
					double cy = clamp(py, bottom, top);
					double dx = px - cx;
					double dy = py - cy;
					double d2 = dx * dx + dy * dy;

					if (d2 >= rr) continue;

					double nx = 0.0;
					double ny = 0.0;
					double pen = 0.0;

					if (d2 > 1e-12) {
						double d = sqrt(d2);
						nx = dx / d;
						ny = dy / d;
						pen = r - d;
					} else {
						// Center exactly inside/on boundary: eject along the smallest axis penetration.
						double dl = abs(px - left);
						double dr = abs(right - px);
						double db = abs(py - bottom);
						double dt = abs(top - py);

						double m = dl;
						nx = -1.0;
						ny = 0.0;
						if (dr < m) {
							m = dr;
							nx = 1.0;
							ny = 0.0;
						}
						if (db < m) {
							m = db;
							nx = 0.0;
							ny = -1.0;
						}
						if (dt < m) {
							m = dt;
							nx = 0.0;
							ny = 1.0;
						}
						pen = r + m;
					}

					px += nx * pen;
					py += ny * pen;

					double vn = vx * nx + vy * ny;
					if (vn < 0) {
						double bounce = 1.0 + domain.wallElasticity;
						vx -= bounce * vn * nx;
						vy -= bounce * vn * ny;
					}

					moved = true;
				}

			if (!moved) break;
		}
	}
	void handleParticleCollisions(SourceDomain domain) {
		double h = 1.0 / domain.fInvSpacing;
		double r = domain.particleRadius;

		double dt = domain.dt;

		double minX = h + r;
		double maxX = (domain.fNumX - 1) * h - r;
		double minY = h + r;
		double maxY = (domain.fNumY - 1) * h - r;

		DoubleBuffer particlePos = domain.particlePos;
		DoubleBuffer particleVel = domain.particleVel;

		double numParticles = domain.numParticles;

		for (int o = 0; o < domain.obstacles.size(); o++) {
			const SourceObstacle &obstacle = domain.obstacles[o];

			double _or = obstacle.radius;
			double minDist = obstacle.radius + r;
			double minDist2 = minDist * minDist;

			double _ow = obstacle.width;
			double _oh = obstacle.height;

			switch (obstacle.type) {
			case circle:

				for (int i = 0; i < numParticles; i++) {
					int i2 = i * 2;

					double _x = particlePos[i2];
					double _y = particlePos[i2 + 1];

					if (_x == 0 && _y == 0) continue;

					double dx = _x - obstacle.x;
					double dy = _y - obstacle.y;
					double d2 = dx * dx + dy * dy;

					// obstacle collision
					if (d2 < minDist2) {
						particlePos[i2] = obstacle.x + dx * minDist / sqrt(d2);
						particlePos[i2 + 1] = obstacle.y + dy * minDist / sqrt(d2);

						particleVel[i2] = obstacle.velX;
						particleVel[i2 + 1] = obstacle.velY;

						if (dy < 0) {
							if (dx < 0) {
								particleVel[i2] += dy * 0.25;
								particleVel[i2 + 1] += -dx * 0.25;
							} else {
								particleVel[i2] += -dy * 0.25;
								particleVel[i2 + 1] += -dx * 0.25;
							}
						}
					}
				}
				break;

			case rectangle:
				for (int i = 0; i < numParticles; i++) {
					int i2 = i * 2;

					double _x = particlePos[i2];
					double _y = particlePos[i2 + 1];

					if (_x == 0 && _y == 0) continue;

					double dx = _x - obstacle.x;
					double dy = _y - obstacle.y;

					// obstacle collision
					if (abs(dx) < _ow && abs(dy) < _oh) {
						double _ex = _ow - abs(dx);
						double _ey = _oh - abs(dy);

						if (_ex < _ey) {
							if (dx < 0) {
								particlePos[i2] = obstacle.x - _ow - r;
								particleVel[i2] = obstacle.velX;
							} else {
								particlePos[i2] = obstacle.x + _ow + r;
								particleVel[i2] = obstacle.velX;
							}
						} else {
							if (dy < 0) {
								particlePos[i2 + 1] = obstacle.y - _oh - r;
								particleVel[i2 + 1] = obstacle.velY;
							} else {
								particlePos[i2 + 1] = obstacle.y + _oh + r;
								particleVel[i2 + 1] = obstacle.velY;
							}
						}
					}
				}
				break;
			}
		}

		for (int i = 0; i < numParticles; i++) {
			int i2 = i * 2;

			double _x = particlePos[i2];
			double _y = particlePos[i2 + 1];

			if (_x == 0 && _y == 0) continue;

			resolveSolidGridCollision(domain, _x, _y, particleVel[i2], particleVel[i2 + 1], r);

			particlePos[i2] = _x;
			particlePos[i2 + 1] = _y;
		}

		// wall collisions
		double wallElasticity = domain.wallElasticity;
		int c = domain.collideWall;
		bool cTop = c & 0b0001;
		bool cBot = c & 0b0010;
		bool cLef = c & 0b0100;
		bool cRig = c & 0b1000;

		for (int i = 0; i < numParticles; i++) {
			int i2 = i * 2;

			double _x = particlePos[i2];
			double _y = particlePos[i2 + 1];

			if (_x == 0 && _y == 0) continue;

			if (cTop && _y < minY) {
				_y = minY;
				particleVel[i2 + 1] *= -wallElasticity;
			}
			if (cBot && _y > maxY) {
				_y = maxY;
				particleVel[i2 + 1] *= -wallElasticity;
			}
			if (cLef && _x < minX) {
				_x = minX;
				particleVel[i2] *= -wallElasticity;
			}
			if (cRig && _x > maxX) {
				_x = maxX;
				particleVel[i2] *= -wallElasticity;
			}

			particlePos[i2] = _x;
			particlePos[i2 + 1] = _y;
		}
	}
}
