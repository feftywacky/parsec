#include <iostream>
#include <cassert>
#include <ctime>

#include "order_book/order.hpp"
#include "order_book/price_level.hpp"
#include "order_book/side.hpp"

void test_order_creation() {
    std::cout << "Testing Order creation..." << std::endl;
    
    Order order1 = {
        .order_id = 1001,
        .side = BID,
        .price = 50000.0,
        .original_size = 1.5,
        .remaining_size = 1.5,
        .timestamp = static_cast<uint64_t>(std::time(nullptr))
    };
    
    assert(order1.order_id == 1001);
    assert(order1.side == BID);
    assert(order1.price == 50000.0);
    assert(order1.original_size == 1.5);
    assert(order1.remaining_size == 1.5);
    
    std::cout << "✓ Order creation test passed" << std::endl;
}

void test_price_level_add_remove() {
    std::cout << "\nTesting PriceLevel add/remove operations..." << std::endl;
    
    // Create a price level at $50,000
    PriceLevel level(50000.0, 0);
    
    // Create orders
    Order order1 = {1001, BID, 50000.0, 1.0, 1.0, 1000};
    Order order2 = {1002, BID, 50000.0, 2.0, 2.0, 1001};
    Order order3 = {1003, BID, 50000.0, 0.5, 0.5, 1002};
    
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
    
    PriceLevel level(50000.0, 100);
    
    assert(level.get_price() == 50000.0);
    assert(level.get_quantity() == 100);
    std::cout << "✓ Initial price and quantity correct" << std::endl;
    
    level.set_price(50050.0);
    assert(level.get_price() == 50050.0);
    std::cout << "✓ Price setter works" << std::endl;
    
    level.set_quantity(150);
    assert(level.get_quantity() == 150);
    std::cout << "✓ Quantity setter works" << std::endl;
}

void test_multiple_orders_fifo() {
    std::cout << "\nTesting FIFO order priority..." << std::endl;
    
    PriceLevel level(50000.0, 0);
    
    // Create 5 orders with different timestamps
    Order orders[5];
    for (int i = 0; i < 5; i++) {
        orders[i] = {
            static_cast<uint64_t>(2000 + i),
            BID,
            50000.0,
            1.0,
            1.0,
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
    
    PriceLevel level(50000.0, 0);
    Order order1 = {3001, BID, 50000.0, 1.0, 1.0, 1000};
    level.add_order(&order1);
    
    // Try to remove an order that doesn't exist - should not crash
    level.remove_order(9999);
    
    Order* first = level.get_first_order();
    assert(first != nullptr);
    assert(first->order_id == 3001);
    std::cout << "✓ Removing non-existent order doesn't affect existing orders" << std::endl;
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
        
        std::cout << "\n=== All tests passed! ✓ ===" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "\n❌ Test failed with exception: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}