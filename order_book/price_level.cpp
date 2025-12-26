#include <cstdint>
#include <list>
#include <unordered_map>

#include "price_level.hpp"
#include "order.hpp"

PriceLevel::PriceLevel(double price, uint32_t quantity) : price(price), quantity(quantity){}

PriceLevel::~PriceLevel(){}

bool PriceLevel::add_order(Order* order)
{

}

void PriceLevel::set_price(double new_price)
{
    price = new_price;
}

void PriceLevel::set_quantity(uint32_t new_quantity)
{
    quantity = new_quantity;
}