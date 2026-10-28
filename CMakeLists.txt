cmake_minimum_required(VERSION 3.21)
project(BlackHoleSim LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Dependencies (vcpkg manifest: glfw3, glm, glew)
find_package(GLEW REQUIRED)
find_package(glfw3 CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(OpenGL REQUIRED)

set(DEPS glfw GLEW::GLEW glm::glm OpenGL::GL)

# Shared compile settings
add_library(bh_common INTERFACE)
target_compile_definitions(bh_common INTERFACE
    BH_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    $<$<PLATFORM_ID:Windows>:NOMINMAX>
    $<$<PLATFORM_ID:Windows>:_CRT_SECURE_NO_WARNINGS>
)
if(MSVC)
    target_compile_options(bh_common INTERFACE /utf-8 /W3 /MP)
else()
    target_compile_options(bh_common INTERFACE -Wall -Wextra -Wno-unused-parameter)
endif()
target_include_directories(bh_common INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

# 2D lensing demo (legacy fixed-function OpenGL)
add_executable(BlackHole2D 2D_lensing.cpp)
target_link_libraries(BlackHole2D PRIVATE bh_common ${DEPS})

# Shader files
file(GLOB SHADERS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.vert"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.frag"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.comp"
)
set_source_files_properties(${SHADERS} PROPERTIES HEADER_FILE_ONLY ON)
source_group("Shaders" FILES ${SHADERS})

# Embed the shaders into the executables (the installed screensaver has no files beside it)
set(GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
set(EMBEDDED_SHADERS_H "${GENERATED_DIR}/shaders_embedded.h")
string(REPLACE ";" "|" SHADERS_ARG "${SHADERS}")
add_custom_command(
    OUTPUT "${EMBEDDED_SHADERS_H}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${GENERATED_DIR}"
    COMMAND ${CMAKE_COMMAND} "-DOUT=${EMBEDDED_SHADERS_H}" "-DSHADERS=${SHADERS_ARG}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_shaders.cmake"
    DEPENDS ${SHADERS} "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_shaders.cmake"
    COMMENT "Embedding shaders into shaders_embedded.h"
    VERBATIM
)

set(BH3D_SOURCES black_hole.cpp icon_harvest.cpp icon_harvest.h "${EMBEDDED_SHADERS_H}" ${SHADERS})

# 3D compute-shader ray tracer (interactive)
add_executable(BlackHole3D ${BH3D_SOURCES})
target_link_libraries(BlackHole3D PRIVATE bh_common ${DEPS})
target_include_directories(BlackHole3D PRIVATE "${GENERATED_DIR}")

# Windows screensaver: same program, built as BlackHoleSaver.scr without a console
if(WIN32)
    add_executable(BlackHoleSaver WIN32 ${BH3D_SOURCES})
    target_link_libraries(BlackHoleSaver PRIVATE bh_common ${DEPS} shell32 ole32)
    target_include_directories(BlackHoleSaver PRIVATE "${GENERATED_DIR}")
    target_compile_definitions(BlackHoleSaver PRIVATE BH_SCREENSAVER)
    set_target_properties(BlackHoleSaver PROPERTIES SUFFIX ".scr")
    target_link_libraries(BlackHole3D PRIVATE shell32 ole32)
endif()
# Runs on every build (not only when the exe relinks) so edited shaders are never stale.
add_custom_target(BlackHole3D_shaders ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:BlackHole3D>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SHADERS} $<TARGET_FILE_DIR:BlackHole3D>
    COMMENT "Copying shaders next to BlackHole3D"
    VERBATIM
)
add_dependencies(BlackHole3D BlackHole3D_shaders)

# Visual Studio niceties: F5 launches the 3D sim from its output directory
set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT BlackHole3D)
set_target_properties(BlackHole3D BlackHole2D PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:BlackHole3D>"
)
if(WIN32)
    set_target_properties(BlackHoleSaver PROPERTIES
        VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:BlackHole3D>"
        VS_DEBUGGER_COMMAND_ARGUMENTS "/s"
    )
endif()
