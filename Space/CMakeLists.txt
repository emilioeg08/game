# Space domain: orbits, celestial bodies, ships, flight and star system generation.
add_library(gx_space STATIC
    Bodies/CelestialBody.cpp
    Bodies/CelestialBody.h
    Generation/StarSystemGenerator.cpp
    Generation/StarSystemGenerator.h
    Orbits/Kepler.cpp
    Orbits/Kepler.h
    Ships/Flight.cpp
    Ships/Flight.h
    Ships/Ship.h
)

target_link_libraries(gx_space PUBLIC gx_simulation)
gx_configure_target(gx_space)
