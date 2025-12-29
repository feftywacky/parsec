cmake_minimum_required(VERSION 3.15)
project(parsec)

# Set C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Add compiler warnings
add_compile_options(-Wall -Wextra -pedantic)

# Include directories
include_directories(${CMAKE_SOURCE_DIR}/src)

# Source files
set(ORDER_BOOK_SOURCES
    src/order_book/price_level.cpp
    src/order_book/book_side.cpp
    src/order_book/order_book.cpp
)

# Create executable
add_executable(parsec 
    src/main.cpp
    ${ORDER_BOOK_SOURCES}
)
