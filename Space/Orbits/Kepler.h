#pragma once

#include "Engine/Core/Types.h"
#include "Engine/Math/Vec3.h"
#include "Engine/Time/SimTime.h"

// Two-body Keplerian orbits evaluated analytically: the state at any time is a pure function of the
// elements and the time, so celestial bodies move "on rails" with no integration, no drift and no cost when
// nobody looks at them. Units: metres, seconds, radians; gm in m^3/s^2.
//
// Uses sin/cos, so positions are bit-identical for a given binary but not guaranteed across C runtimes
// (ADR-005).
namespace gx {

inline constexpr f64 kPi = 3.14159265358979323846;
inline constexpr f64 kTwoPi = 2.0 * kPi;

struct OrbitalElements {
    f64 semiMajorAxis = 0.0;            // m
    f64 eccentricity = 0.0;             // [0, 1)
    f64 inclination = 0.0;              // rad, to the system's reference (ecliptic) plane
    f64 longitudeOfAscendingNode = 0.0; // rad
    f64 argumentOfPeriapsis = 0.0;      // rad
    f64 meanAnomalyAtEpoch = 0.0;       // rad, at SimTime::epoch()

    template <typename Archive>
    void io(Archive& ar) {
        ar.io("semiMajorAxis", semiMajorAxis);
        ar.io("eccentricity", eccentricity);
        ar.io("inclination", inclination);
        ar.io("longitudeOfAscendingNode", longitudeOfAscendingNode);
        ar.io("argumentOfPeriapsis", argumentOfPeriapsis);
        ar.io("meanAnomalyAtEpoch", meanAnomalyAtEpoch);
    }
};

struct OrbitState {
    Vec3d position; // relative to the parent body
    Vec3d velocity;
};

[[nodiscard]] f64 orbitalPeriod(const OrbitalElements& orbit, f64 parentGm);

// Eccentric anomaly E with E - e sin E = M (Newton iteration, |residual| < 1e-13 for e < 0.99).
[[nodiscard]] f64 solveKepler(f64 meanAnomaly, f64 eccentricity);

// State relative to the parent at `time`.
[[nodiscard]] OrbitState orbitStateAt(const OrbitalElements& orbit, f64 parentGm, SimTime time);

// State at a given eccentric anomaly (used to draw orbit paths).
[[nodiscard]] OrbitState orbitStateAtEccentricAnomaly(const OrbitalElements& orbit, f64 parentGm,
                                                      f64 eccentricAnomaly);

} // namespace gx
