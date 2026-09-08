cmake_minimum_required(VERSION 3.24)
project(parsec LANGUAGES C CXX)

# LANGUAGES C CXX only — no OBJC/OBJCXX. We use the GLFW + OpenGL3 backend deliberately, and
# enabling Objective-C++ here would make CMake mis-compile GLFW's own .m files as ObjC++.
# See docs/05-ui.md §7.1.

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(PARSEC_BUILD_TESTS "Build Parsec tests" ON)

# Several FetchContent dependencies still declare `cmake_minimum_required(VERSION <3.5)`,
# which CMake 4.x rejects outright. Scope the escape hatch narrowly around the dependency
# fetch rather than setting it globally (docs/05-ui.md §7.2).
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
include(cmake/Dependencies.cmake)
unset(CMAKE_POLICY_VERSION_MINIMUM)

# ---------------------------------------------------------------------------------------------
# Rust staticlib (docs/04-rust-layer.md §5.4)
# ---------------------------------------------------------------------------------------------
corrosion_import_crate(
    MANIFEST_PATH ${CMAKE_CURRENT_SOURCE_DIR}/rust/Cargo.toml
    CRATE_TYPES   staticlib
    PROFILE       release
    LOCKED)
# corrosion_import_crate names the imported target after the crate: parsec_rs.

# ---------------------------------------------------------------------------------------------
# parsec_core — trading core: market data, execution, risk, portfolio, engine glue.
#
# Globbed deliberately (CONFIGURE_DEPENDS) rather than listed by hand: several agents own
# different subtrees under here and add files independently, so a hand-maintained file list
# would be a constant merge-conflict magnet on this one shared CMakeLists.txt.
# ---------------------------------------------------------------------------------------------
file(GLOB_RECURSE PARSEC_CORE_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/md/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/exec/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/risk/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/portfolio/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/*.cpp)

add_library(parsec_core STATIC ${PARSEC_CORE_SOURCES})
target_include_directories(parsec_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(parsec_core PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(parsec_core PUBLIC parsec_rs)

if(APPLE)
    target_link_libraries(parsec_core PUBLIC
        "-framework Security" "-framework SystemConfiguration" "-framework CoreFoundation" iconv)
else()
    target_link_libraries(parsec_core PUBLIC pthread dl m)
endif()

# ---------------------------------------------------------------------------------------------
# parsec_ui — ImGui frontend. Reads engine snapshots only; never touches pc_* directly.
# ---------------------------------------------------------------------------------------------
file(GLOB PARSEC_UI_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/widgets/*.cpp)

add_library(parsec_ui STATIC ${PARSEC_UI_SOURCES})
target_include_directories(parsec_ui PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(parsec_ui PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(parsec_ui PUBLIC parsec_core imgui glfw OpenGL::GL)
if(APPLE)
    target_compile_definitions(parsec_ui PUBLIC GL_SILENCE_DEPRECATION)
endif()

# ---------------------------------------------------------------------------------------------
# parsec — the executable
# ---------------------------------------------------------------------------------------------
add_executable(parsec src/main.cpp)
target_link_libraries(parsec PRIVATE parsec_ui parsec_core parsec_rs)


# ---------------------------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------------------------
if(PARSEC_BUILD_TESTS)
    include(CTest)

    file(GLOB PARSEC_TEST_SOURCES CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/cpp/*.cpp)

    add_executable(parsec_tests ${PARSEC_TEST_SOURCES})
    target_include_directories(parsec_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    # Links the Rust staticlib as well as parsec_core: tests/cpp/test_abi_layout.cpp calls
    # pc_abi_version() so that the C++ and Rust views of the FFI contract are checked against
    # each other in one binary, not just asserted independently (docs/02 §5.5).
    target_link_libraries(parsec_tests PRIVATE parsec_core parsec_rs doctest::doctest)
    if(APPLE)
        target_link_libraries(parsec_tests PRIVATE
            "-framework Security" "-framework SystemConfiguration" "-framework CoreFoundation"
            iconv)
    else()
        target_link_libraries(parsec_tests PRIVATE pthread dl m)
    endif()
    add_test(NAME parsec_tests COMMAND parsec_tests)
endif()
