#pragma once
// Pinned FLIP_fluid.gml solver arrays are borrowed only for one synchronous kernel call.
#include <engine/imagegraph/FluidDomain.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>
#include <span>
#include <vector>
namespace engine::imagegraph::detail::source_flip {
	using std::abs;
	using std::cos;
	using std::floor;
	using std::isnan;
	using std::max;
	using std::min;
	using std::sin;
	using std::sqrt;
	inline constexpr double SourcePi = std::numbers::pi;
	inline constexpr double solid = 0, fluid = 1, air = 2;
	enum COLLISION_SHAPE { circle, rectangle };
	struct SourceObstacle {
		double x, y, velX, velY;
		COLLISION_SHAPE type;
		double radius, width, height;
	};
	struct BoundsViolation {};
	struct WorkViolation {};
	struct DoubleBuffer {
		double *Data = nullptr;
		size_t Size = 0;
		uint64_t *RemainingWork = nullptr;
		double &operator[](size_t index) const {
			if (index >= Size) throw BoundsViolation{};
			if (RemainingWork) {
				if (!*RemainingWork) throw WorkViolation{};
				--*RemainingWork;
			}
			return Data[index];
		}
		operator double *() const {
			return Data;
		}
	};
	struct SourceDomain {
		double width;
		double height;
		double spacing;
		double density;
		double viscosity;

		double friction = 1;

		double fNumX;
		double fNumY;
		double fNumX1;
		double fNumY1;
		double h;
		double fInvSpacing;
		double fNumCells;

		int collideWall;
		double wallElasticity = 1;

		DoubleBuffer u;
		DoubleBuffer v;
		DoubleBuffer du;
		DoubleBuffer dv;
		DoubleBuffer prevU;
		DoubleBuffer prevV;
		DoubleBuffer p;
		DoubleBuffer s;
		DoubleBuffer solidMap; // ?? kinda confuse with the s array.

		DoubleBuffer cellType;
		DoubleBuffer particlePos;
		DoubleBuffer particleVel;
		DoubleBuffer particleDensity;
		DoubleBuffer particleLife;

		double particleRestDensity;

		double particleRadius;
		double pInvSpacing;
		double pNumX;
		double pNumY;
		double pNumCells;

		double velocityDamping = 0.9;

		DoubleBuffer numCellParticles;
		DoubleBuffer firstCellParticle;
		DoubleBuffer cellParticleIds;

		double maxParticles;
		double numParticles;

		double dt = 0.5;
		double globalIteration = 4;

		double gravity = 50;
		double gravityDirection = 270;
		double flipRatio = 0.5;
		double numPressureIterations = 4;
		double numParticleIterations = 8;
		double overRelaxation = 1.8;

		std::span<const SourceObstacle> obstacles;
	};
	inline double clamp(double x, double min, double max) {
		return x < min ? min : x > max ? max : x;
	}
	static inline int getIndex(int x, int y, int n) {
		return x * n + y;
	}
	static inline double getCellOpen(const SourceDomain &domain, int idx) {
		return (domain.s[idx] != 0 && domain.solidMap[idx] <= 0.5) ? 1.0 : 0.0;
	}
	static inline bool isSolidCell(const SourceDomain &domain, int x, int y) {
		if (x < 0 || y < 0 || x >= domain.fNumX || y >= domain.fNumY) return true;
		return getCellOpen(domain, getIndex(x, y, (int)domain.fNumY)) == 0.0;
	}
	void setBoundary(SourceDomain domain, DoubleBuffer a);
	void integrateParticles(SourceDomain domain);
	void pushParticlesApart(SourceDomain domain);
	void handleParticleCollisions(SourceDomain domain);
	void updateParticleDensity(SourceDomain domain);
	void transferVelocities(SourceDomain domain, bool toGrid);
	void solveIncompressibility(SourceDomain domain);
}
