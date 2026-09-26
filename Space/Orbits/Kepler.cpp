#include "Space/Orbits/Kepler.h"

#include "Engine/Core/Assert.h"

#include <algorithm>
#include <cmath>

namespace gx {

f64 orbitalPeriod(const OrbitalElements& orbit, f64 parentGm) {
    GX_ASSERT(orbit.semiMajorAxis > 0.0 && parentGm > 0.0, "invalid orbit");
    const f64 a = orbit.semiMajorAxis;
    return kTwoPi * std::sqrt(a * a * a / parentGm);
}

f64 solveKepler(f64 meanAnomaly, f64 eccentricity) {
    GX_ASSERT(eccentricity >= 0.0 && eccentricity < 1.0, "only elliptic orbits are supported");
    // Safeguarded Newton: the root is bracketed by [M - e, M + e] because E - M = e sin E. Newton steps that
    // leave the bracket fall back to bisection, so it converges for every e < 1 (plain Newton can diverge
    // for high eccentricities).
    f64 low = meanAnomaly - eccentricity;
    f64 high = meanAnomaly + eccentricity;
    const f64 sign = std::sin(meanAnomaly) >= 0.0 ? 1.0 : -1.0;
    f64 e = std::clamp(meanAnomaly + 0.85 * eccentricity * sign, low, high); // Danby's starting value
    for (int iteration = 0; iteration < 64; ++iteration) {
        const f64 residual = e - eccentricity * std::sin(e) - meanAnomaly;
        if (residual > 0.0) {
            high = e;
        } else {
            low = e;
        }
        f64 next = e - residual / (1.0 - eccentricity * std::cos(e));
        if (!(next > low && next < high)) {
            next = 0.5 * (low + high);
        }
        if (std::abs(next - e) < 1e-15) {
            return next;
        }
        e = next;
    }
    return e;
}

OrbitState orbitStateAtEccentricAnomaly(const OrbitalElements& orbit, f64 parentGm, f64 eccentricAnomaly) {
    const f64 a = orbit.semiMajorAxis;
    const f64 ecc = orbit.eccentricity;
    const f64 b = a * std::sqrt(1.0 - ecc * ecc);
    const f64 meanMotion = std::sqrt(parentGm / (a * a * a));

    const f64 cosE = std::cos(eccentricAnomaly);
    const f64 sinE = std::sin(eccentricAnomaly);
    const f64 eccentricRate = meanMotion / (1.0 - ecc * cosE); // dE/dt

    // Perifocal frame: x towards periapsis, y along the direction of motion at periapsis.
    const f64 x = a * (cosE - ecc);
    const f64 y = b * sinE;
    const f64 vx = -a * sinE * eccentricRate;
    const f64 vy = b * cosE * eccentricRate;

    const f64 cosNode = std::cos(orbit.longitudeOfAscendingNode);
    const f64 sinNode = std::sin(orbit.longitudeOfAscendingNode);
    const f64 cosArg = std::cos(orbit.argumentOfPeriapsis);
    const f64 sinArg = std::sin(orbit.argumentOfPeriapsis);
    const f64 cosInc = std::cos(orbit.inclination);
    const f64 sinInc = std::sin(orbit.inclination);

    const Vec3d p{cosNode * cosArg - sinNode * sinArg * cosInc, sinNode * cosArg + cosNode * sinArg * cosInc,
                  sinArg * sinInc};
    const Vec3d q{-cosNode * sinArg - sinNode * cosArg * cosInc,
                  -sinNode * sinArg + cosNode * cosArg * cosInc, cosArg * sinInc};
    return {p * x + q * y, p * vx + q * vy};
}

OrbitState orbitStateAt(const OrbitalElements& orbit, f64 parentGm, SimTime time) {
    const f64 period = orbitalPeriod(orbit, parentGm);
    // Reduce time modulo the period first: keeps the mean anomaly small and precise over centuries.
    const f64 seconds = static_cast<f64>(time.microsecondsSinceEpoch()) / 1'000'000.0;
    const f64 phase = std::fmod(seconds, period) / period; // (-1, 1)
    f64 meanAnomaly = std::fmod(orbit.meanAnomalyAtEpoch + kTwoPi * phase, kTwoPi);
    if (meanAnomaly > kPi) {
        meanAnomaly -= kTwoPi;
    } else if (meanAnomaly < -kPi) {
        meanAnomaly += kTwoPi;
    }
    return orbitStateAtEccentricAnomaly(orbit, parentGm, solveKepler(meanAnomaly, orbit.eccentricity));
}

} // namespace gx
