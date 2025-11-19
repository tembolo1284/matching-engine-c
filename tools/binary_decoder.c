#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

#define BINARY_MAGIC 0x4D
#define BINARY_SYMBOL_LEN 8

typedef struct __attribute__((packed)) {
    uint8_t magic;
    uint8_t msg_type;
    char symbol[BINARY_SYMBOL_LEN];
    uint32_t user_id;
    uint32_t user_order_id;
} binary_ack_t;

typedef struct __attribute__((packed)) {
    uint8_t magic;
    uint8_t msg_type;
    char symbol[BINARY_SYMBOL_LEN];
    uint32_t user_id;
    uint32_t user_order_id;
} binary_cancel_ack_t;

typedef struct __attribute__((packed)) {
    uint8_t magic;
    uint8_t msg_type;
    char symbol[BINARY_SYMBOL_LEN];
    uint32_t user_id_buy;
    uint32_t user_order_id_buy;
    uint32_t user_id_sell;
    uint32_t user_order_id_sell;
    uint32_t price;
    uint32_t quantity;
} binary_trade_t;

typedef struct __attribute__((packed)) {
    uint8_t magic;
    uint8_t msg_type;
    char symbol[BINARY_SYMBOL_LEN];
    uint8_t side;
    uint32_t price;
    uint32_t quantity;
} binary_top_of_book_t;

/* Helper to safely extract symbol */
static void extract_symbol(char* dest, const char* src) {
    size_t i;
    for (i = 0; i < BINARY_SYMBOL_LEN && src[i] != '\0'; i++) {
        dest[i] = src[i];
    }
    dest[i] = '\0';
}

void decode_message(const unsigned char* data, size_t len) {
    if (len < 2 || data[0] != BINARY_MAGIC) {
        printf("Not a binary message\n");
        return;
    }
    
    uint8_t msg_type = data[1];
    
    switch (msg_type) {
        case 'A': {
            if (len < sizeof(binary_ack_t)) {
                printf("Incomplete ACK message\n");
                return;
            }
            binary_ack_t* ack = (binary_ack_t*)data;
            char symbol[BINARY_SYMBOL_LEN + 1];
            extract_symbol(symbol, ack->symbol);
            printf("A, %s, %u, %u\n", symbol, 
                   ntohl(ack->user_id), ntohl(ack->user_order_id));
            break;
        }
        
        case 'X': {
            if (len < sizeof(binary_cancel_ack_t)) {
                printf("Incomplete CANCEL_ACK message\n");
                return;
            }
            binary_cancel_ack_t* ack = (binary_cancel_ack_t*)data;
            char symbol[BINARY_SYMBOL_LEN + 1];
            extract_symbol(symbol, ack->symbol);
            printf("C, %s, %u, %u\n", symbol,
                   ntohl(ack->user_id), ntohl(ack->user_order_id));
            break;
        }
        
        case 'T': {
            if (len < sizeof(binary_trade_t)) {
                printf("Incomplete TRADE message\n");
                return;
            }
            binary_trade_t* trade = (binary_trade_t*)data;
            char symbol[BINARY_SYMBOL_LEN + 1];
            extract_symbol(symbol, trade->symbol);
            printf("T, %s, %u, %u, %u, %u, %u, %u\n", symbol,
                   ntohl(trade->user_id_buy), ntohl(trade->user_order_id_buy),
                   ntohl(trade->user_id_sell), ntohl(trade->user_order_id_sell),
                   ntohl(trade->price), ntohl(trade->quantity));
            break;
        }
        
        case 'B': {
            if (len < sizeof(binary_top_of_book_t)) {
                printf("Incomplete TOB message\n");
                return;
            }
            binary_top_of_book_t* tob = (binary_top_of_book_t*)data;
            char symbol[BINARY_SYMBOL_LEN + 1];
            extract_symbol(symbol, tob->symbol);
            uint32_t price = ntohl(tob->price);
            uint32_t qty = ntohl(tob->quantity);
            
            if (price == 0) {
                printf("B, %s, %c, -, -\n", symbol, tob->side);
            } else {
                printf("B, %s, %c, %u, %u\n", symbol, tob->side, price, qty);
            }
            break;
        }
        
        default:
            printf("Unknown message type: 0x%02X\n", msg_type);
    }
}

int main(void) {
    printf("Binary Message Decoder\n");
    printf("Reading from stdin...\n\n");
    
    unsigned char buffer[1024];
    
    while (1) {
        ssize_t bytes = read(STDIN_FILENO, buffer, sizeof(buffer));
        
        if (bytes <= 0) break;
        
        size_t offset = 0;
        while (offset < (size_t)bytes) {
            if (buffer[offset] == BINARY_MAGIC) {
                /* Determine message size based on type */
                size_t msg_size = 0;
                if (offset + 1 < (size_t)bytes) {
                    uint8_t msg_type = buffer[offset + 1];
                    switch (msg_type) {
                        case 'A': msg_size = sizeof(binary_ack_t); break;
                        case 'X': msg_size = sizeof(binary_cancel_ack_t); break;
                        case 'T': msg_size = sizeof(binary_trade_t); break;
                        case 'B': msg_size = sizeof(binary_top_of_book_t); break;
                        default: msg_size = 2; break;
                    }
                }
                
                if (offset + msg_size <= (size_t)bytes) {
                    decode_message(&buffer[offset], msg_size);
                    offset += msg_size;
                } else {
                    break;
                }
            } else {
                offset++;
            }
        }
    }
    
    return 0;
}
