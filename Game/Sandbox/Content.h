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
    f64 maxAcceleration; // m/s^2
    f64 cruiseSpeed;     // m/s
};

inline constexpr std::array<ShipClassDef, kShipClassCount> kShipClasses = {{
    {"Correo", 3'000.0, 4'000'000.0}, // fast personal ship: ~10 h per AU at cruise
    {"Carguero", 600.0, 1'500'000.0}, // bulk hauler: ~28 h per AU at cruise
}};

inline constexpr const char* kPlayerShipName = "Errante";

inline constexpr std::array<const char*, 16> kHaulerNames = {
    "Brisa", "Faro",     "Halcón", "Lince",  "Marea", "Nómada", "Ceniza",   "Alba",
    "Sirga", "Torrente", "Vigía",  "Quilla", "Ámbar", "Estela", "Albatros", "Cierzo"};

} // namespace gx::content
