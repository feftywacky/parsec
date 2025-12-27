#include <cstdint>
#include <list>
#include <unordered_map>
#include <algorithm>

#include "price_level.hpp"
#include "order.hpp"

PriceLevel::PriceLevel(double price, uint32_t quantity) : price(price), quantity(quantity){}

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

void PriceLevel::set_price(double new_price)
{
    price = new_price;
}

void PriceLevel::set_quantity(uint32_t new_quantity)
{
    quantity = new_quantity;
}

double PriceLevel::get_price()
{
    return price;
}

double PriceLevel::get_quantity()
{
    return quantity;
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