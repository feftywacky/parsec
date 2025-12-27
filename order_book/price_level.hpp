#pragma once

#include <cstdint>
#include <list>
#include <unordered_map>

struct Order;

class PriceLevel {
private:
    double price;
    uint32_t quantity;
    std::list<Order*> orders;
    std::unordered_map<uint64_t, std::list<Order*>::iterator> order_map;
public:
    PriceLevel(double price, uint32_t quantity);
    ~PriceLevel();
    void add_order(Order* order);
    void remove_order(uint64_t order_id);
    void modify_order(uint64_t order_id);
    Order* get_first_order();
    void set_price(double new_price);
    void set_quantity(uint32_t new_quantity);
    double get_price();
    double get_quantity();
};