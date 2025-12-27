cmake_minimum_required(VERSION 3.15)
project(parsec)

# Set C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Add compiler warnings
add_compile_options(-Wall -Wextra -pedantic)

# Include directories
include_directories(${CMAKE_SOURCE_DIR})

# Source files
set(ORDER_BOOK_SOURCES
    order_book/price_level.cpp
    order_book/book_side.cpp
    order_book/order_book.cpp
)

# Create executable
add_executable(main 
    main.cpp
    ${ORDER_BOOK_SOURCES}
)
