#pragma once

#include "Engine/Core/Types.h"

#include <array>

// Provisional content (docs/DESIGN.md). Kept as plain tables behind one header so it can move to data files
// (the modding boundary) without touching the systems that use it.
namespace gx::content {

// Calendar year at SimTime::epoch() for display.
inline constexpr i64 kEpochYear = 2400;

enum Faction : u32 { kFactionPlayer = 0, kFactionIndependent = 1, kFactionCount };

inline constexpr std::array<const char*, kFactionCount> kFactionNames = {"Jugador",
                                                                         "Transportistas independientes"};

enum ShipClass : u32 { kShipClassCourier = 0, kShipClassHauler = 1, kShipClassCount };

struct ShipClassDef {
    const char* name;
    f64 maxAcceleration;      // m/s^2, sublight
    f64 cruiseSpeed;          // m/s, sublight autopilot limit
    f64 hyperspaceSpeed;      // m/s
    f64 hyperspaceChargeTime; // s
};

// Real-time scale (x1 = real time, ADR-021): a trip between planets takes minutes at x1, seconds at x10.
inline constexpr std::array<ShipClassDef, kShipClassCount> kShipClasses = {{
    {"Correo", 50'000.0, 15'000'000.0, 1.5e9, 5.0},   // 1 AU in hyperspace: ~100 s
    {"Carguero", 15'000.0, 6'000'000.0, 6.0e8, 12.0}, // 1 AU in hyperspace: ~250 s
}};

// Sensors and signatures (ADR-023). Emission units are arbitrary; ranges follow from the SNR formulas:
// an idle Correo is seen passively at ~22,000 km by a Carguero, at ~700,000 km while thrusting hard; any ship
// in hyperspace is visible across most of the system; the Correo's radar identifies a Carguero at ~3 million
// km.
struct SensorDef {
    f64 baseEmission;
    f64 driveEmission;
    f64 crossSection; // m^2
    f64 passiveSensitivity;
    f64 activeStrength; // 0: no radar
};

inline constexpr std::array<SensorDef, kShipClassCount> kShipSensors = {{
    {1e3, 1e6, 1e3, 1e12, 6.25e34}, // Correo: small, quiet, good sensors and a radar
    {5e3, 2e6, 1e4, 5e11, 0.0},     // Carguero: bigger and louder, basic passive sensors only
}};

inline constexpr const char* kPlayerShipName = "Errante";

inline constexpr std::array<const char*, 16> kHaulerNames = {
    "Brisa", "Faro",     "Halcón", "Lince",  "Marea", "Nómada", "Ceniza",   "Alba",
    "Sirga", "Torrente", "Vigía",  "Quilla", "Ámbar", "Estela", "Albatros", "Cierzo"};

} // namespace gx::content
