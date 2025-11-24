# Protocol Specifications

Complete documentation for CSV and Binary protocols supported by the Matching Engine.

## Table of Contents
- [Overview](#overview)
- [CSV Protocol](#csv-protocol)
- [Binary Protocol](#binary-protocol)
- [Protocol Detection](#protocol-detection)
- [Binary Tools](#binary-tools)
- [Creating Custom Messages](#creating-custom-messages)
- [Protocol Comparison](#protocol-comparison)
- [Best Practices](#best-practices)

---

## Overview

The Matching Engine supports two protocols:
1. **CSV Protocol** - Human-readable, text-based
2. **Binary Protocol** - High-performance, fixed-size structs

Both protocols can be used simultaneously through automatic format detection. This allows for mixed-mode operation where some clients use CSV (for debugging) while others use Binary (for performance).

---

## CSV Protocol

### Input Messages

#### New Order
```csv
N, userId, symbol, price, qty, side, userOrderId
```

**Fields:**
- `userId` (uint32_t) - User identifier (must match client_id in TCP mode)
- `symbol` (string) - Trading symbol (max 8 chars, e.g., "IBM", "AAPL")
- `price` (uint32_t) - Price in cents (e.g., 10000 = $100.00)
- `qty` (uint32_t) - Quantity in shares
- `side` (char) - 'B' for buy, 'S' for sell
- `userOrderId` (uint32_t) - Client's order identifier (must be unique per user)

**Example:**
```csv
N, 1, IBM, 10000, 50, B, 1
# User 1 places buy order: 50 shares of IBM at $100.00, order ID 1
```

#### Cancel Order
```csv
C, userId, userOrderId
```

**Fields:**
- `userId` (uint32_t) - User identifier
- `userOrderId` (uint32_t) - Order to cancel

**Example:**
```csv
C, 1, 1
# User 1 cancels order ID 1
```

#### Flush All Orders
```csv
F
```

Cancels all orders in all order books. Generates cancel acknowledgements for each order.

**Example:**
```csv
F
# Cancel all orders system-wide
```

### Output Messages

#### Acknowledgement
```csv
A, symbol, userId, userOrderId
```

Confirms order was accepted and added to the book.

**Example:**
```csv
A, IBM, 1, 1
# Order accepted: User 1, Order 1, Symbol IBM
```

#### Cancel Acknowledgement
```csv
C, symbol, userId, userOrderId
```

Confirms order was cancelled or flushed.

**Example:**
```csv
C, IBM, 1, 1
# Order cancelled: User 1, Order 1, Symbol IBM
```

#### Trade Execution
```csv
T, symbol, buyUserId, buyOrderId, sellUserId, sellOrderId, price, qty
```

**Fields:**
- `symbol` - Trading symbol
- `buyUserId` - Buyer's user ID
- `buyOrderId` - Buy order ID
- `sellUserId` - Seller's user ID
- `sellOrderId` - Sell order ID
- `price` - Trade execution price
- `qty` - Trade quantity

**Example:**
```csv
T, IBM, 1, 1, 2, 1, 10000, 50
# Trade: User 1 order 1 bought 50 shares from User 2 order 1 at $100.00
```

#### Top of Book Update
```csv
B, symbol, side, price, qty
```

**Fields:**
- `symbol` - Trading symbol
- `side` - 'B' for bid (best buy), 'S' for ask (best sell)
- `price` - Best price on this side
- `qty` - Total quantity at best price

**Example:**
```csv
B, IBM, B, 10000, 50
# Best bid: 50 shares at $100.00

B, IBM, S, 10050, 100
# Best ask: 100 shares at $100.50
```

#### Top of Book Eliminated
```csv
B, symbol, side, -, -
```

Indicates no orders remain on this side of the book.

**Example:**
```csv
B, IBM, B, -, -
# No more buy orders for IBM
```

---

## Binary Protocol

### Overview

All binary messages use:
- **Network byte order** (big endian) for integers
- **Magic byte** `0x4D` ('M') as first byte
- **Fixed-size structs** with `__attribute__((packed))`
- **Null-padded strings** (8 bytes for symbols)

### Message Type Bytes

- `'N'` (0x4E) - New Order
- `'C'` (0x43) - Cancel Order
- `'F'` (0x46) - Flush
- `'A'` (0x41) - Acknowledgement
- `'X'` (0x58) - Cancel Acknowledgement
- `'T'` (0x54) - Trade
- `'B'` (0x42) - Top of Book

### Input Messages

#### New Order (30 bytes)
```c
typedef struct __attribute__((packed)) {
    uint8_t  magic;         // 0x4D
    uint8_t  msg_type;      // 'N' (0x4E)
    uint32_t user_id;       // Network byte order (htonl)
    char     symbol[8];     // Null-padded, e.g., "IBM\0\0\0\0\0"
    uint32_t price;         // Network byte order
    uint32_t quantity;      // Network byte order
    uint8_t  side;          // 'B' or 'S'
    uint32_t user_order_id; // Network byte order
} binary_new_order_t;
```

**Size breakdown:**
- 1 byte magic
- 1 byte type
- 4 bytes user_id
- 8 bytes symbol
- 4 bytes price
- 4 bytes quantity
- 1 byte side
- 4 bytes user_order_id
- **Total: 27 bytes** (padded to 30)

**Example (hex dump):**
```
4D 4E 00 00 00 01 49 42 4D 00 00 00 00 00 00 00 27 10 00 00 00 32 42 00 00 00 01
│  │  └────────┘ └──────────────────────┘ └────────┘ └────────┘ │  └────────┘
│  │  user_id=1  symbol="IBM"             price=10000 qty=50     │  order_id=1
│  │                                                              side='B'
│  msg_type='N'
magic=0x4D
```

#### Cancel Order (11 bytes)
```c
typedef struct __attribute__((packed)) {
    uint8_t  magic;         // 0x4D
    uint8_t  msg_type;      // 'C' (0x43)
    uint32_t user_id;       // Network byte order
    uint32_t user_order_id; // Network byte order
} binary_cancel_order_t;
```

**Size:** 11 bytes (1 + 1 + 4 + 4 + 1 padding)

#### Flush (2 bytes)
```c
typedef struct __attribute__((packed)) {
    uint8_t magic;      // 0x4D
    uint8_t msg_type;   // 'F' (0x46)
} binary_flush_t;
```

**Size:** 2 bytes

### Output Messages

#### Acknowledgement (19 bytes)
```c
typedef struct __attribute__((packed)) {
    uint8_t  magic;         // 0x4D
    uint8_t  msg_type;      // 'A' (0x41)
    char     symbol[8];     // Null-padded
    uint32_t user_id;       // Network byte order
    uint32_t user_order_id; // Network byte order
} binary_ack_t;
```

#### Cancel Acknowledgement (19 bytes)
```c
typedef struct __attribute__((packed)) {
    uint8_t  magic;         // 0x4D
    uint8_t  msg_type;      // 'X' (0x58)
    char     symbol[8];     // Null-padded
    uint32_t user_id;       // Network byte order
    uint32_t user_order_id; // Network byte order
} binary_cancel_ack_t;
```

#### Trade (31 bytes)
```c
typedef struct __attribute__((packed)) {
    uint8_t  magic;          // 0x4D
    uint8_t  msg_type;       // 'T' (0x54)
    char     symbol[8];      // Null-padded
    uint32_t buy_user_id;    // Network byte order
    uint32_t buy_order_id;   // Network byte order
    uint32_t sell_user_id;   // Network byte order
    uint32_t sell_order_id;  // Network byte order
    uint32_t price;          // Network byte order
    uint32_t quantity;       // Network byte order
} binary_trade_t;
```

#### Top of Book (20 bytes)
```c
typedef struct __attribute__((packed)) {
    uint8_t  magic;      // 0x4D
    uint8_t  msg_type;   // 'B' (0x42)
    char     symbol[8];  // Null-padded
    uint8_t  side;       // 'B' or 'S'
    uint32_t price;      // Network byte order (0 if no orders)
    uint32_t quantity;   // Network byte order (0 if no orders)
} binary_top_of_book_t;
```

---

## Protocol Detection

### Automatic Format Detection

The receiver thread automatically detects message format by examining the first byte:

```c
if (data[0] == 0x4D) {
    // Binary protocol (magic byte detected)
    parse_binary_message(data, length, &msg);
} else {
    // CSV protocol (assume text)
    parse_csv_message(data, length, &msg);
}
```

### TCP Framing

TCP mode uses length-prefixed framing to handle message boundaries:

```
┌────────────────┬──────────────────────┐
│  Length (4B)   │  Message Payload     │
│  (big endian)  │  (CSV or Binary)     │
└────────────────┴──────────────────────┘
```

**Example:**
```
00 00 00 1E  4D 4E 00 00 00 01 49 42 4D ...
└─────────┘  └────────────────────────────
length=30    binary new order message
```

### UDP Operation

UDP mode does not use length framing (UDP packets have built-in boundaries). Format detection still applies.

---

## Binary Tools

### Binary Client

Sends binary protocol messages via UDP or TCP.

```bash
# UDP mode (fire-and-forget)
./build/binary_client <port> [scenario]

# TCP mode (persistent connection)
./build/binary_client <port> [scenario] --tcp

# Force CSV protocol
./build/binary_client <port> [scenario] --csv
```

**Scenarios:**
- `1` - Simple orders (buy + sell + flush)
- `2` - Trade execution (matching orders)
- `3` - Order cancellation

**Examples:**
```bash
# UDP with binary protocol
./build/binary_client 1234 2

# TCP with binary protocol
./build/binary_client 1234 2 --tcp

# TCP with CSV protocol
./build/binary_client 1234 2 --tcp --csv
```

### Binary Decoder

Decodes binary output to human-readable CSV.

```bash
# Pipe server output through decoder
./build/matching_engine --binary 2>/dev/null | ./build/binary_decoder

# Or decode a binary file
cat binary_output.bin | ./build/binary_decoder
```

**Example output:**
```
Binary message (30 bytes): New Order
  User ID: 1
  Symbol: IBM
  Price: 10000
  Quantity: 50
  Side: B
  Order ID: 1

CSV equivalent: N, 1, IBM, 10000, 50, B, 1
```

---

## Creating Custom Messages

### C Example

```c
#include <arpa/inet.h>
#include <sys/socket.h>
#include <string.h>

typedef struct __attribute__((packed)) {
    uint8_t  magic;
    uint8_t  msg_type;
    uint32_t user_id;
    char     symbol[8];
    uint32_t price;
    uint32_t quantity;
    uint8_t  side;
    uint32_t user_order_id;
} binary_new_order_t;

void send_binary_order(int sock, struct sockaddr_in* addr) {
    binary_new_order_t msg = {
        .magic = 0x4D,
        .msg_type = 'N',
        .user_id = htonl(1),
        .price = htonl(10000),
        .quantity = htonl(50),
        .side = 'B',
        .user_order_id = htonl(1)
    };
    
    // Copy symbol with null padding
    strncpy(msg.symbol, "IBM", 8);
    
    // Send via UDP
    sendto(sock, &msg, sizeof(msg), 0,
           (struct sockaddr*)addr, sizeof(*addr));
}
```

### Python Example

```python
import struct
import socket

def create_new_order(user_id, symbol, price, qty, side, order_id):
    """Create a binary new order message."""
    magic = 0x4D
    msg_type = ord('N')
    
    # Pack symbol (8 bytes, null-padded)
    symbol_bytes = symbol.encode('ascii').ljust(8, b'\0')
    
    # Pack message (network byte order)
    message = struct.pack(
        '!BB I 8s III B I',
        magic,           # uint8
        msg_type,        # uint8
        user_id,         # uint32
        symbol_bytes,    # 8 bytes
        price,           # uint32
        qty,             # uint32
        ord(side),       # uint8
        order_id         # uint32
    )
    return message

# Usage
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
msg = create_new_order(1, "IBM", 10000, 50, "B", 1)
sock.sendto(msg, ('localhost', 1234))
```

### Testing Your Messages

```bash
# Start server in binary mode with decoder
./build/matching_engine --udp --binary 2>/dev/null | ./build/binary_decoder

# Send your custom message
python send_order.py

# Verify decoded output matches expectation
```

---

## Protocol Comparison

### Message Size Comparison

| Message Type | CSV Size | Binary Size | Savings |
|--------------|----------|-------------|---------|
| New Order | 40-50 bytes | 30 bytes | 40-60% |
| Cancel | 20-30 bytes | 11 bytes | 60-70% |
| Flush | 2 bytes | 2 bytes | 0% |
| Acknowledgement | 20-30 bytes | 19 bytes | 30-50% |
| Trade | 50-70 bytes | 31 bytes | 50-60% |
| Top of Book | 30-40 bytes | 20 bytes | 40-50% |

### Performance Comparison

| Metric | CSV | Binary | Improvement |
|--------|-----|--------|-------------|
| Parse time | 500-2000ns | 50-200ns | **5-10x faster** |
| Network bandwidth | Baseline | 50-70% of CSV | **2x more efficient** |
| CPU usage | Higher (string parsing) | Lower (memcpy) | **Significant** |
| Validation | Runtime errors possible | Type-safe at compile time | **Better** |

### Feature Comparison

| Feature | CSV | Binary |
|---------|-----|--------|
| Human readable | ✓ | ✗ |
| Easy debugging | ✓ | Needs decoder |
| High performance | - | ✓ |
| Deterministic parsing | - | ✓ |
| Network efficient | - | ✓ |
| No external tools needed | ✓ | ✗ |

### When to Use Each Protocol

**Use CSV when:**
- Debugging or development
- Interoperability with text-based tools
- Human monitoring required
- Bandwidth not critical
- Latency > 100μs acceptable

**Use Binary when:**
- Production high-frequency trading
- Bandwidth constrained
- Latency < 50μs required
- Maximum throughput needed
- Deterministic performance critical

---

## Best Practices

### CSV Protocol

**Do:**
- Use consistent spacing: `N, 1, IBM, 100, 50, B, 1`
- Validate input before sending
- Handle commas in data properly
- Test with netcat: `echo "N, 1, IBM, 100, 50, B, 1" | nc -u localhost 1234`

**Don't:**
- Mix tabs and spaces
- Include trailing commas
- Use non-ASCII characters in symbols
- Exceed field limits (symbol: 8 chars)

### Binary Protocol

**Do:**
- Always use `htonl()` for integers (network byte order)
- Null-pad strings to full width
- Use `__attribute__((packed))` on structs
- Verify struct sizes match specification
- Send magic byte first

**Don't:**
- Use host byte order (native endianness)
- Forget to pad symbols to 8 bytes
- Skip the magic byte
- Assume struct padding matches target

### TCP vs UDP

**TCP (recommended for most use cases):**
- Reliable delivery
- Message ordering guaranteed
- Built-in flow control
- Supports multiple clients
- Use length-prefix framing

**UDP (for extreme performance):**
- Lower latency (~10-20μs less)
- No connection overhead
- Fire-and-forget
- No guaranteed delivery
- Single client per port

### Mixed Protocol Operation

You can mix protocols in the same system:

```bash
# Server accepts both CSV and Binary
./build/matching_engine --tcp

# Client 1 uses CSV
./build/tcp_client localhost 1234

# Client 2 uses Binary
./build/binary_client 1234 --tcp
```

The server auto-detects and handles both correctly!

---

## Error Handling

### CSV Parsing Errors

**Common errors:**
- Malformed CSV: `N, 1 IBM, 100, 50, B, 1` (missing comma)
- Invalid number: `N, ABC, IBM, 100, 50, B, 1`
- Wrong field count: `N, 1, IBM, 100, 50`

**Server behavior:**
- Logs error message
- Drops invalid message
- Continues processing

### Binary Parsing Errors

**Common errors:**
- Wrong magic byte (not 0x4D)
- Invalid message type
- Truncated message
- Wrong endianness

**Server behavior:**
- Logs error with hex dump
- Drops invalid message
- Continues processing

---

## Protocol Specifications Reference

### Quick Reference Card

**CSV Input:**
```
N, <user>, <sym>, <price>, <qty>, <side>, <orderId>
C, <user>, <orderId>
F
```

**CSV Output:**
```
A, <sym>, <user>, <orderId>
C, <sym>, <user>, <orderId>
T, <sym>, <buyUser>, <buyOrd>, <sellUser>, <sellOrd>, <price>, <qty>
B, <sym>, <side>, <price>, <qty>
```

**Binary:** All structs in `binary_protocol.h`, all integers in network byte order.

---

## See Also

- [Quick Start Guide](documentation/QUICK_START.md) - Get running quickly
- [Architecture](documentation/ARCHITECTURE.md) - System internals
- [Testing](documentation/TESTING.md) - Protocol testing guide
- [Build Instructions](documentation/BUILD.md) - Compiler setup
