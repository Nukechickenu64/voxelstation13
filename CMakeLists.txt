cmake_minimum_required(VERSION 3.25)
project(VoxelStation13 VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── FetchContent dependencies ─────────────────────────────────────────────────
include(FetchContent)

# GLM — header-only math
FetchContent_Declare(glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG        1.0.1
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(glm)

# SDL3 — system installed
find_package(SDL3 REQUIRED CONFIG)

# stb — header-only image loading
FetchContent_Declare(stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(stb)

# ── Shader generation (Python SPIRV assembler) ────────────────────────────────
find_package(Python3 QUIET)
if(Python3_FOUND)
    set(SHADER_VERT_H   "${CMAKE_SOURCE_DIR}/src/render/shaders/chunk_vert_spv.h")
    set(SHADER_FRAG_H   "${CMAKE_SOURCE_DIR}/src/render/shaders/chunk_frag_spv.h")
    set(FONT_ATLAS_H    "${CMAKE_SOURCE_DIR}/src/render/shaders/roboto_atlas.h")
    set(UI_TEXT_FRAG_H  "${CMAKE_SOURCE_DIR}/src/render/shaders/ui_text_frag_spv.h")
    add_custom_command(
        OUTPUT  "${SHADER_VERT_H}" "${SHADER_FRAG_H}" "${UI_TEXT_FRAG_H}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/gen_shaders.py"
        DEPENDS "${CMAKE_SOURCE_DIR}/tools/gen_shaders.py"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Generating SPIRV shader headers"
    )
    add_custom_command(
        OUTPUT  "${FONT_ATLAS_H}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/gen_font_data.py"
        DEPENDS "${CMAKE_SOURCE_DIR}/tools/gen_font_data.py"
                "${CMAKE_SOURCE_DIR}/textures/font/roboto.png"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Generating Roboto font atlas header"
    )
    add_custom_target(gen_shaders   DEPENDS "${SHADER_VERT_H}" "${SHADER_FRAG_H}" "${UI_TEXT_FRAG_H}")
    add_custom_target(gen_font_data DEPENDS "${FONT_ATLAS_H}")
endif()


# ── Source files ──────────────────────────────────────────────────────────────
set(SRC_CORE
    src/core/game_loop.cpp
    src/core/world.cpp
    src/core/entity_manager.cpp
)

set(SRC_RENDER
    src/render/renderer.cpp
    src/render/chunk_mesher.cpp
    src/render/lighting.cpp
    src/render/ui_renderer.cpp
    src/render/ui_item_icons.cpp
    src/render/model_loader.cpp
)

set(SRC_SIM
    src/simulation/atmos.cpp
    src/simulation/enclosure.cpp
    src/simulation/power.cpp
    src/simulation/pipes.cpp
    src/simulation/physics.cpp
    src/simulation/world_items.cpp
    src/simulation/model_objects.cpp
)

set(SRC_INPUT
    src/input/input_manager.cpp
    src/input/alt_mode.cpp
)

set(SRC_INVENTORY
    src/inventory/inventory.cpp
    src/inventory/item_registry.cpp
)

set(SRC_NETWORK
    src/network/server.cpp
    src/network/client.cpp
)

set(SRC_AUDIO
    src/audio/audio_manager.cpp
)

set(SRC_DATA
    src/data/voxel_registry.cpp
    src/data/data_validator.cpp
    src/data/mob_species_registry.cpp
    src/data/map_io.cpp
)

set(SRC_UI
    src/ui/hud.cpp
    src/ui/debug_overlay.cpp
    src/ui/inventory_panel.cpp
    src/ui/context_menu.cpp
    src/ui/creative_menu.cpp
    src/ui/gas_overlay.cpp
    src/ui/player_stats_overlay.cpp
    src/ui/map_editor.cpp
    src/ui/admin_menu.cpp
)

# ── Executable ────────────────────────────────────────────────────────────────
add_executable(vs13
    src/main.cpp
    src/stb_impl.cpp
    ${SRC_CORE}
    ${SRC_RENDER}
    ${SRC_SIM}
    ${SRC_INPUT}
    ${SRC_INVENTORY}
    ${SRC_NETWORK}
    ${SRC_AUDIO}
    ${SRC_DATA}
    ${SRC_UI}
)

if(Python3_FOUND)
    add_dependencies(vs13 gen_shaders gen_font_data)
endif()

target_include_directories(vs13 PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/includes
    ${stb_SOURCE_DIR}
)

target_link_libraries(vs13 PRIVATE
    SDL3::SDL3
    glm::glm
)

# ── Compiler warnings ─────────────────────────────────────────────────────────
if(MSVC)
    target_compile_options(vs13 PRIVATE /W4 /WX-)
else()
    target_compile_options(vs13 PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ── Install ───────────────────────────────────────────────────────────────────
install(TARGETS vs13 RUNTIME DESTINATION bin)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/data    DESTINATION share/vs13)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/textures DESTINATION share/vs13)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/sounds  DESTINATION share/vs13)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/maps    DESTINATION share/vs13)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/scripts DESTINATION share/vs13)
