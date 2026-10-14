cmake_minimum_required(VERSION 3.24)

project(HeavyGunnerWindowsPort LANGUAGES CXX)

add_executable(heavy_gunner_port WIN32
    src/main.cpp
)

add_executable(pact_format_probe
    src/tools/pact_format_probe.cpp
    src/formats/a3d.cpp
    src/formats/pcam.cpp
    src/formats/peft.cpp
    src/formats/pliv.cpp
    src/formats/pmod.cpp
)

add_executable(heavy_gunner_gl WIN32
    src/opengl_main.cpp
    src/formats/a3d.cpp
    src/formats/pcam.cpp
    src/formats/peft.cpp
    src/formats/pliv.cpp
    src/formats/pmod.cpp
)

target_compile_features(heavy_gunner_port PRIVATE cxx_std_20)
target_compile_definitions(heavy_gunner_port PRIVATE UNICODE _UNICODE NOMINMAX)
if(MSVC)
    target_compile_options(heavy_gunner_port PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(heavy_gunner_port PRIVATE -Wall -Wextra -Wpedantic)
endif()
if(MINGW)
    target_link_options(heavy_gunner_port PRIVATE -static -static-libgcc -static-libstdc++)
endif()
target_link_libraries(heavy_gunner_port PRIVATE
    d2d1
    dwrite
    windowscodecs
    ole32
    winmm
)

set_target_properties(heavy_gunner_port PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
)

target_compile_features(pact_format_probe PRIVATE cxx_std_20)
target_include_directories(pact_format_probe PRIVATE "${CMAKE_SOURCE_DIR}/src")
if(MSVC)
    target_compile_options(pact_format_probe PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(pact_format_probe PRIVATE -Wall -Wextra -Wpedantic)
    target_link_options(pact_format_probe PRIVATE -static -static-libgcc -static-libstdc++)
endif()
set_target_properties(pact_format_probe PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
)

target_compile_features(heavy_gunner_gl PRIVATE cxx_std_20)
target_include_directories(heavy_gunner_gl PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_compile_definitions(heavy_gunner_gl PRIVATE UNICODE _UNICODE NOMINMAX)
if(MSVC)
    target_compile_options(heavy_gunner_gl PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(heavy_gunner_gl PRIVATE -Wall -Wextra -Wpedantic)
    target_link_options(heavy_gunner_gl PRIVATE -static -static-libgcc -static-libstdc++)
endif()
target_link_libraries(heavy_gunner_gl PRIVATE
    opengl32
    gdi32
    windowscodecs
    ole32
    winmm
)
set_target_properties(heavy_gunner_gl PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
)

add_custom_command(TARGET heavy_gunner_port POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:heavy_gunner_port>/assets"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/assets/recovered"
        "$<TARGET_FILE_DIR:heavy_gunner_port>/assets"
    COMMENT "Copying recovered Heavy Gunner assets"
)

add_custom_command(TARGET heavy_gunner_gl POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:heavy_gunner_gl>/assets"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/assets/recovered"
        "$<TARGET_FILE_DIR:heavy_gunner_gl>/assets"
    COMMENT "Copying recovered Heavy Gunner assets for OpenGL"
)
