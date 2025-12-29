#pragma once

#include <cstdint>
#include <list>
#include <unordered_map>

struct Order;

class PriceLevel {
private:
    int64_t price;
    int64_t total_size;
    std::list<Order*> orders;
    std::unordered_map<uint64_t, std::list<Order*>::iterator> order_map;
public:
    PriceLevel(int64_t price, int64_t total_size);
    ~PriceLevel();
    void add_order(Order* order);
    void remove_order(uint64_t order_id);
    void modify_order(uint64_t order_id);
    Order* get_first_order();
    void set_price(int64_t new_price);
    void set_total_size(int64_t new_total_size);
    int64_t get_price();
    int64_t get_total_size();
};