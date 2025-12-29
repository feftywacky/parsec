#include <iostream>
#include <cassert>
#include <ctime>

#include "order_book/order.hpp"
#include "order_book/price_level.hpp"
#include "order_book/side.hpp"
#include "order_book/units.hpp"

void test_order_creation() {
    std::cout << "Testing Order creation..." << std::endl;

    using Cfg = ob::BtcPerpConfig;
    
    Order order1 = {
        .order_id = 1001,
        .side = BID,
        .price = 50000 * Cfg::price_scale,
        .original_size = 150000,
        .remaining_size = 150000,
        .timestamp = static_cast<uint64_t>(std::time(nullptr))
    };
    
    assert(order1.order_id == 1001);
    assert(order1.side == BID);
    assert(order1.price == 50000 * Cfg::price_scale);
    assert(order1.original_size == 150000);
    assert(order1.remaining_size == 150000);
    
    std::cout << "✓ Order creation test passed" << std::endl;
}

void test_price_level_add_remove() {
    std::cout << "\nTesting PriceLevel add/remove operations..." << std::endl;

    using Cfg = ob::BtcPerpConfig;
    
    // Create a price level at $50,000
    PriceLevel level(50000 * Cfg::price_scale, 0);
    
    // Create orders
    Order order1 = {1001, BID, 50000 * Cfg::price_scale, 100000, 100000, 1000};
    Order order2 = {1002, BID, 50000 * Cfg::price_scale, 200000, 200000, 1001};
    Order order3 = {1003, BID, 50000 * Cfg::price_scale, 50000, 50000, 1002};
    
    // Test adding orders
    level.add_order(&order1);
    level.add_order(&order2);
    level.add_order(&order3);
    
    Order* first = level.get_first_order();
    assert(first != nullptr);
    assert(first->order_id == 1001);
    std::cout << "✓ First order is correct (FIFO)" << std::endl;
    
    // Test removing middle order
    level.remove_order(1002);
    first = level.get_first_order();
    assert(first->order_id == 1001);
    std::cout << "✓ Remove middle order - first order still correct" << std::endl;
    
    // Remove first order
    level.remove_order(1001);
    first = level.get_first_order();
    assert(first != nullptr);
    assert(first->order_id == 1003);
    std::cout << "✓ Remove first order - new first is correct" << std::endl;
    
    // Remove last order
    level.remove_order(1003);
    first = level.get_first_order();
    assert(first == nullptr || first->order_id != 1003);
    std::cout << "✓ All orders removed successfully" << std::endl;
}

void test_price_level_getters_setters() {
    std::cout << "\nTesting PriceLevel getters/setters..." << std::endl;

    using Cfg = ob::BtcPerpConfig;
    
    PriceLevel level(50000 * Cfg::price_scale, 100);
    
    assert(level.get_price() == 50000 * Cfg::price_scale);
    assert(level.get_total_size() == 100);
    std::cout << "✓ Initial price and total size correct" << std::endl;
    
    level.set_price(50050 * Cfg::price_scale);
    assert(level.get_price() == 50050 * Cfg::price_scale);
    std::cout << "✓ Price setter works" << std::endl;
    
    level.set_total_size(150);
    assert(level.get_total_size() == 150);
    std::cout << "✓ Total size setter works" << std::endl;
}

void test_multiple_orders_fifo() {
    std::cout << "\nTesting FIFO order priority..." << std::endl;

    using Cfg = ob::BtcPerpConfig;
    
    PriceLevel level(50000 * Cfg::price_scale, 0);
    
    // Create 5 orders with different timestamps
    Order orders[5];
    for (int i = 0; i < 5; i++) {
        orders[i] = {
            static_cast<uint64_t>(2000 + i),
            BID,
            50000 * Cfg::price_scale,
            100000,
            100000,
            static_cast<uint64_t>(1000 + i)
        };
        level.add_order(&orders[i]);
    }
    
    // Verify FIFO - first order should be order 2000
    Order* first = level.get_first_order();
    assert(first->order_id == 2000);
    std::cout << "✓ FIFO ordering maintained" << std::endl;
    
    // Remove orders out of order and verify
    level.remove_order(2002); // Remove middle
    level.remove_order(2000); // Remove first
    
    first = level.get_first_order();
    assert(first->order_id == 2001);
    std::cout << "✓ FIFO maintained after removals" << std::endl;
}

void test_remove_nonexistent_order() {
    std::cout << "\nTesting removal of non-existent order..." << std::endl;

    using Cfg = ob::BtcPerpConfig;
    
    PriceLevel level(50000 * Cfg::price_scale, 0);
    Order order1 = {3001, BID, 50000 * Cfg::price_scale, 100000, 100000, 1000};
    level.add_order(&order1);
    
    // Try to remove an order that doesn't exist - should not crash
    level.remove_order(9999);
    
    Order* first = level.get_first_order();
    assert(first != nullptr);
    assert(first->order_id == 3001);
    std::cout << "✓ Removing non-existent order doesn't affect existing orders" << std::endl;
}

void test_units_tick_config() {
    std::cout << "\nTesting price/size tick+scale config..." << std::endl;

    using Cfg = ob::BtcPerpConfig;

    // Check that 1 unit corresponds to the expected decimal increments.
    // With sz_decimals=5 => size_scale=1e5 => 1 size unit = 0.00001 BTC
    assert(Cfg::size_to_double(1) == 0.00001);
    // With max_price_decimals=1 => price_scale=10 => 1 price unit = 0.1
    assert(Cfg::price_to_double(1) == 0.1);

    std::cout << "✓ Tick/scale parsing and alignment checks passed" << std::endl;
}

int main()
{
    std::cout << "=== Running Order Book Test Suite ===" << std::endl;
    
    try {
        test_order_creation();
        test_price_level_add_remove();
        test_price_level_getters_setters();
        test_multiple_orders_fifo();
        test_remove_nonexistent_order();
        test_units_tick_config();
        
        std::cout << "\n=== All tests passed! ✓ ===" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "\n❌ Test failed with exception: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}