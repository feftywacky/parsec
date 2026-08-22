cmake_minimum_required(VERSION 3.22)
project(ffidemo CXX)
set(CMAKE_CXX_STANDARD 20)

include(FetchContent)
FetchContent_Declare(
    Corrosion
    GIT_REPOSITORY https://github.com/corrosion-rs/corrosion.git
    GIT_TAG v0.5.2
)
FetchContent_MakeAvailable(Corrosion)

corrosion_import_crate(
    MANIFEST_PATH ${CMAKE_CURRENT_SOURCE_DIR}/rust/Cargo.toml
    CRATE_TYPES staticlib
    PROFILE release
)

add_executable(app cpp/main.cpp)
target_link_libraries(app PRIVATE hl_bridge)

if(APPLE)
  target_link_libraries(app PRIVATE
    "-framework Security" "-framework SystemConfiguration" "-framework CoreFoundation" iconv)
endif()
