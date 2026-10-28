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

# 3D compute-shader ray tracer
add_executable(BlackHole3D black_hole.cpp)
target_link_libraries(BlackHole3D PRIVATE bh_common ${DEPS})

# Shader files: copy next to the executable so it runs from any working directory
file(GLOB SHADERS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.vert"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.frag"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.comp"
)
target_sources(BlackHole3D PRIVATE ${SHADERS})          # show them in the IDE
set_source_files_properties(${SHADERS} PROPERTIES HEADER_FILE_ONLY ON)
source_group("Shaders" FILES ${SHADERS})
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
