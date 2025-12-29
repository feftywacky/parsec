#pragma once

#include <cstdint>

#include "side.hpp"

struct Order {
    uint64_t order_id;
    Side side;
    int64_t price;
    int64_t original_size;
    int64_t remaining_size;
    uint64_t timestamp;
};