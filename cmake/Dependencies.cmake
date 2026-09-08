# Third-party dependencies, fetched and built in-tree via FetchContent.
#
# FetchContent over vcpkg/Conan: for a handful of pinned deps, `git clone && cmake -B build`
# should work on a fresh machine with no extra toolchain or lockfile daemon (docs/05-ui.md §6).
#
# Every GIT_TAG below is pinned to a real tag (never a branch), with the sole exception of
# imgui's `docking` branch, which is itself the release channel we need (docking is not yet
# in imgui master) — see docs/05-ui.md §1 and §7.3.

include(FetchContent)
include(GNUInstallDirs)

# ---------------------------------------------------------------------------------------------
# GLFW
# ---------------------------------------------------------------------------------------------
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.5.1
    GIT_SHALLOW    TRUE)

# ---------------------------------------------------------------------------------------------
# Dear ImGui (docking branch — required for ImGuiConfigFlags_DockingEnable, docs/05-ui.md §1)
# ---------------------------------------------------------------------------------------------
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.92.9b-docking
    GIT_SHALLOW    TRUE)

# ---------------------------------------------------------------------------------------------
# nlohmann/json — config and CLI only, never on the hot path (docs/05-ui.md §5.3)
# ---------------------------------------------------------------------------------------------
FetchContent_Declare(json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.12.0
    GIT_SHALLOW    TRUE)

# ---------------------------------------------------------------------------------------------
# doctest
# ---------------------------------------------------------------------------------------------
FetchContent_Declare(doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG        v2.4.12
    GIT_SHALLOW    TRUE)

FetchContent_MakeAvailable(glfw imgui json doctest)
find_package(OpenGL REQUIRED)

# imgui ships no CMakeLists.txt of its own — build it ourselves as a static lib. We use the
# GLFW + OpenGL3 backend deliberately (docs/05-ui.md §7.1): adding the Metal backend would
# require enabling OBJC/OBJCXX as project languages, which breaks GLFW's own .m files.
add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui PUBLIC glfw OpenGL::GL)
if(APPLE)
    target_compile_definitions(imgui PUBLIC GL_SILENCE_DEPRECATION)
endif()

# ---------------------------------------------------------------------------------------------
# Corrosion — imports the Rust staticlib as a CMake target (docs/04-rust-layer.md §5.4)
#
# v0.5.2 verified to exist via `git ls-remote --tags`, so we pin it rather than the unverified
# tag docs/05-ui.md §7 flagged as a risk.
# ---------------------------------------------------------------------------------------------
FetchContent_Declare(Corrosion
    GIT_REPOSITORY https://github.com/corrosion-rs/corrosion.git
    GIT_TAG        v0.5.2
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(Corrosion)
