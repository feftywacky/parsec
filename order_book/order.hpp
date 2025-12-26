#pragma once

#include <cstdint>

#include "side.hpp"

struct Order {
    uint64_t order_id;
    Side side;
    double price;
    double original_size;
    double remaining_size;
    uint64_t timestamp;
};