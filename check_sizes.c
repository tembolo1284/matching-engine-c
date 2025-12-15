#include <stdio.h>
#include "include/core/matching_engine.h"
#include "include/core/order_book.h"

int main() {
    printf("=== Top Level ===\n");
    printf("sizeof(matching_engine_t) = %zu\n", sizeof(matching_engine_t));
    printf("sizeof(memory_pools_t) = %zu\n", sizeof(memory_pools_t));
    printf("sizeof(output_buffer_t) = %zu\n", sizeof(output_buffer_t));
    
    printf("\n=== Order Book Components ===\n");
    printf("sizeof(order_book_t) = %zu\n", sizeof(order_book_t));
    printf("sizeof(price_level_t) = %zu\n", sizeof(price_level_t));
    printf("sizeof(order_t) = %zu\n", sizeof(order_t));
    
    printf("\n=== Constants ===\n");
    printf("MAX_SYMBOLS = %d\n", MAX_SYMBOLS);
    printf("MAX_PRICE_LEVELS = %d\n", MAX_PRICE_LEVELS);
    printf("MAX_ORDERS_IN_POOL = %d\n", MAX_ORDERS_IN_POOL);
    
    printf("\n=== Calculated ===\n");
    printf("order_book_t * MAX_SYMBOLS = %zu\n", sizeof(order_book_t) * MAX_SYMBOLS);
    printf("price_level_t * MAX_PRICE_LEVELS * 2 = %zu\n", sizeof(price_level_t) * MAX_PRICE_LEVELS * 2);
    printf("order_t * MAX_ORDERS_IN_POOL = %zu\n", sizeof(order_t) * MAX_ORDERS_IN_POOL);
    
    return 0;
}
