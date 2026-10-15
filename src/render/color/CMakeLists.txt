# wabcut_color - the colour grade's pure C++ core (I.5, docs/color.md): .cube LUTs, the grade's parameters, curve
# baking and CPU reference, built-in looks and log input LUTs, ASC CDL, scope statistics, Auto and Color Match, and
# the colour commands (registered into editingCommands()). No Qt, no GPU: it builds in the core preset. The shaders
# and the GPU scopes that mirror it live in wabcut_render. Owner: Graphics.
find_package(nlohmann_json CONFIG REQUIRED)
find_package(pugixml CONFIG REQUIRED)

add_library(wabcut_color STATIC
    src/Cdl.cpp
    src/ColorCommands.cpp
    src/ColorMath.cpp
    src/ColorTools.cpp
    src/Grade.cpp
    src/Hdr.cpp
    src/Looks.cpp
    src/Lut.cpp
    src/Scopes.cpp
)
target_include_directories(wabcut_color PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(wabcut_color PUBLIC cxx_std_23)
target_link_libraries(wabcut_color PUBLIC wabcut_model wabcut_command PRIVATE nlohmann_json::nlohmann_json pugixml::pugixml)
wabcut_target_defaults(wabcut_color)
