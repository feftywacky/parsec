#include <cstdint>
#include <list>
#include <unordered_map>
#include <algorithm>

#include "price_level.hpp"
#include "order.hpp"

PriceLevel::PriceLevel(int64_t price, int64_t total_size) : price(price), total_size(total_size){}

PriceLevel::~PriceLevel(){}

void PriceLevel::add_order(Order* order)
{
    orders.push_back(order);
    order_map[order->order_id]= --orders.end(); 
}

void PriceLevel::remove_order(uint64_t order_id)
{
    auto map_it = order_map.find(order_id);
    if(map_it!=order_map.end())
    {
        orders.erase(map_it->second);
        order_map.erase(map_it);
    }
}

void PriceLevel::set_price(int64_t new_price)
{
    price = new_price;
}

void PriceLevel::set_total_size(int64_t new_total_size)
{
    total_size = new_total_size;
}

int64_t PriceLevel::get_price()
{
    return price;
}

int64_t PriceLevel::get_total_size()
{
    return total_size;
}

Order* PriceLevel::get_first_order()
{
    if (orders.empty()) {
        return nullptr;
    }
    return orders.front();
}

void PriceLevel::modify_order(uint64_t order_id)
{
    // TODO
}