# Protocol Specifications

Complete documentation for CSV and Binary protocols supported by the Matching Engine.

## Table of Contents
- [Overview](#overview)
- [CSV Protocol](#csv-protocol)
- [Binary Protocol](#binary-protocol)
- [Protocol Detection](#protocol-detection)
- [TCP Framing Protocol](#tcp-framing-protocol)
- [Multicast Protocol](#multicast-protocol)
- [Dual-Processor Routing](#dual-processor-routing)
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

**Protocol format is independent of:**
- Processor routing (both protocols work identically in single/dual-processor modes)
- Transport layer (TCP, UDP, or Multicast)
- Output destination (TCP clients or multicast subscribers)

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

**Dual-Processor Routing:** Symbol's first character determines processor (A-M → P0, N-Z → P1).

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

**Dual-Processor Routing:** Cancel is sent to **both processors** (order could be on either).

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

**Dual-Processor Routing:** Flush is sent to **both processors** (affects all symbols).

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

This works across all transports (TCP, UDP, Multicast).

---

## TCP Framing Protocol

TCP streams don't have message boundaries, so we use **length-prefixed framing**.

### Wire Format
```
┌─────────────────────────────────────────────────────────────┐
│  4 bytes (big-endian)  │           N bytes                  │
│     Message Length     │        Message Payload              │
│                        │       (CSV or Binary)               │
└─────────────────────────────────────────────────────────────┘
```

### Examples

**CSV message:**
```
Message: "N, 1, IBM, 10000, 50, B, 1\n" (28 bytes)

Wire format:
00 00 00 1C   4E 2C 20 31 2C 20 49 42 4D 2C 20 31 ...
└─────────┘   └─────────────────────────────────────
length=28     CSV payload
```

**Binary message:**
```
Message: Binary new order (30 bytes)

Wire format:
00 00 00 1E   4D 4E 00 00 00 01 49 42 4D 00 00 00 ...
└─────────┘   └─────────────────────────────────────
length=30     Binary payload (starts with 0x4D magic)
```

### Client Implementation (Python)
```python
import struct
import socket

def send_framed_message(sock, message):
    """Send a length-prefixed message over TCP."""
    data = message.encode('utf-8')
    length = struct.pack('>I', len(data))  # 4-byte big-endian
    sock.sendall(length + data)

def recv_framed_message(sock):
    """Receive a length-prefixed message from TCP."""
    # Read 4-byte length header
    header = sock.recv(4)
    if len(header) < 4:
        return None
    length = struct.unpack('>I', header)[0]
    
    # Read message body
    data = b''
    while len(data) < length:
        chunk = sock.recv(length - len(data))
        if not chunk:
            return None
        data += chunk
    
    return data.decode('utf-8')

# Usage
sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
sock.connect(('localhost', 1234))

send_framed_message(sock, "N, 1, IBM, 10000, 50, B, 1\n")
response = recv_framed_message(sock)
print(response)  # "A, IBM, 1, 1"
```

### Client Implementation (C)
```c
#include <arpa/inet.h>
#include <string.h>

/* Send framed message */
bool send_framed(int sock, const char* msg, size_t len) {
    uint32_t net_len = htonl(len);
    
    // Send length header
    if (send(sock, &net_len, 4, 0) != 4) return false;
    
    // Send message body
    if (send(sock, msg, len, 0) != (ssize_t)len) return false;
    
    return true;
}

/* Receive framed message */
ssize_t recv_framed(int sock, char* buf, size_t buf_size) {
    uint32_t net_len;
    
    // Read length header
    if (recv(sock, &net_len, 4, MSG_WAITALL) != 4) return -1;
    
    uint32_t len = ntohl(net_len);
    if (len > buf_size) return -1;
    
    // Read message body
    if (recv(sock, buf, len, MSG_WAITALL) != (ssize_t)len) return -1;
    
    return len;
}
```

### Constants
```c
#define MAX_FRAMED_MESSAGE_SIZE 16384   // 16KB max message
#define FRAME_HEADER_SIZE 4             // 4-byte length prefix
```

### UDP Operation

UDP mode does **not** use length framing - UDP packets have built-in boundaries. Format detection (CSV vs Binary) still applies based on the first byte.

---

## Multicast Protocol

### Overview

The multicast publisher broadcasts market data to a UDP multicast group. This is the **industry-standard pattern** used by real exchanges (CME, NASDAQ, ICE) for market data distribution.

### Multicast Transport
```
┌─────────────────────────────────────────────────────────────┐
│                   Multicast Packet Format                    │
├─────────────────────────────────────────────────────────────┤
│                                                              │
│  ┌──────────────────────────────────────────────────────┐  │
│  │              UDP Datagram Payload                     │  │
│  │     (CSV or Binary message - NO length prefix)        │  │
│  └──────────────────────────────────────────────────────┘  │
│                                                              │
│  Notes:                                                      │
│  - Each UDP packet = one message                            │
│  - No length framing needed (UDP has packet boundaries)     │
│  - Format detection via first byte (0x4D = Binary)          │
│  - Same message format as TCP (minus length prefix)         │
│                                                              │
└─────────────────────────────────────────────────────────────┘
```

### Multicast Addressing

| Component | Description |
|-----------|-------------|
| **Group Address** | 239.255.0.1 (organization-local scope) |
| **Port** | 5000 (default) |
| **TTL** | 1 (local subnet only) |
| **Protocol** | UDP |

### Address Ranges

| Range | Scope | Use Case |
|-------|-------|----------|
| 224.0.0.0 - 224.0.0.255 | Link-local | Reserved (routing protocols) |
| **239.0.0.0 - 239.255.255.255** | **Organization-local** | **Our default (perfect for LANs)** |
| 224.0.1.0 - 238.255.255.255 | Internet-wide | Global multicast |

### Subscriber Implementation

The `multicast_subscriber` tool demonstrates how to receive multicast data:
```c
// Create UDP socket
int sock = socket(AF_INET, SOCK_DGRAM, 0);

// Allow multiple subscribers on same machine
int reuse = 1;
setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

// Bind to multicast port
struct sockaddr_in addr = {
    .sin_family = AF_INET,
    .sin_port = htons(5000),
    .sin_addr.s_addr = htonl(INADDR_ANY)
};
bind(sock, (struct sockaddr*)&addr, sizeof(addr));

// Join multicast group
struct ip_mreq mreq = {
    .imr_multiaddr.s_addr = inet_addr("239.255.0.1"),
    .imr_interface.s_addr = htonl(INADDR_ANY)
};
setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));

// Receive messages
char buffer[1024];
while (1) {
    ssize_t len = recvfrom(sock, buffer, sizeof(buffer), 0, NULL, NULL);
    if (len > 0) {
        // Auto-detect format
        if (buffer[0] == 0x4D) {
            // Binary protocol
            parse_binary_message(buffer, len);
        } else {
            // CSV protocol
            parse_csv_message(buffer, len);
        }
    }
}
```

### Python Subscriber Example
```python
import socket
import struct

# Create UDP socket
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)

# Bind to port
sock.bind(('', 5000))

# Join multicast group
mreq = struct.pack('4sl', socket.inet_aton('239.255.0.1'), socket.INADDR_ANY)
sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)

# Receive messages
while True:
    data, addr = sock.recvfrom(1024)
    if data[0] == 0x4D:
        print(f"Binary message: {len(data)} bytes")
    else:
        print(f"CSV message: {data.decode('utf-8')}")
```

### Multicast Message Types

All standard output message types are supported over multicast:

| Message Type | CSV Format | Binary Size |
|--------------|------------|-------------|
| Acknowledgement | `A, symbol, userId, orderId` | 19 bytes |
| Cancel Ack | `C, symbol, userId, orderId` | 19 bytes |
| Trade | `T, symbol, buyUser, buyOrd, sellUser, sellOrd, price, qty` | 31 bytes |
| Top of Book | `B, symbol, side, price, qty` | 20 bytes |

### Multicast vs TCP

| Aspect | TCP (Output Router) | Multicast |
|--------|---------------------|-----------|
| **Delivery** | Per-client queues | One packet to all |
| **Reliability** | Guaranteed | Best-effort |
| **Overhead** | N sends for N clients | 1 send for N clients |
| **Filtering** | Client-specific | All messages to all |
| **Use Case** | Order entry, private data | Market data distribution |

### Enabling Multicast
```bash
# Server with multicast (CSV output)
./build/matching_engine --tcp --multicast 239.255.0.1:5000

# Server with binary multicast
./build/matching_engine --tcp --binary --multicast 239.255.0.1:5000

# Subscribers (run multiple instances)
./build/multicast_subscriber 239.255.0.1 5000
```

### Multicast Scaling

| Subscribers | Server Sends | Network Cost |
|-------------|--------------|--------------|
| 1 | 1 | 1× |
| 10 | 1 | 1× |
| 100 | 1 | 1× |
| 1000 | 1 | 1× |

**Key insight:** Multicast server cost is constant regardless of subscriber count!

---

## Dual-Processor Routing

In dual-processor mode, messages are routed based on symbol:

### Routing Rules

| Message Type | Symbol Available | Routing |
|--------------|------------------|---------|
| New Order | Yes (in message) | Route by symbol: A-M → P0, N-Z → P1 |
| Cancel | No (just order ID) | Send to **BOTH** processors |
| Flush | N/A | Send to **BOTH** processors |

### Symbol Routing Logic
```c
// First character of symbol determines processor
char first = symbol[0];

// Normalize to uppercase
if (first >= 'a' && first <= 'z') {
    first = first - 'a' + 'A';
}

// Route
if (first >= 'A' && first <= 'M') {
    return PROCESSOR_0;  // A-M
} else if (first >= 'N' && first <= 'Z') {
    return PROCESSOR_1;  // N-Z
} else {
    return PROCESSOR_0;  // Default for non-alphabetic
}
```

### Example Routing

| Order | Symbol | First Char | Processor |
|-------|--------|------------|-----------|
| `N, 1, AAPL, 100, 50, B, 1` | AAPL | A | Processor 0 |
| `N, 1, IBM, 100, 50, B, 2` | IBM | I | Processor 0 |
| `N, 1, NVDA, 100, 50, B, 3` | NVDA | N | Processor 1 |
| `N, 1, TSLA, 100, 50, B, 4` | TSLA | T | Processor 1 |
| `C, 1, 3` | (unknown) | - | Both |
| `F` | (all) | - | Both |

### Transparent to Clients

**Clients don't need to know about processor routing.** The routing is handled entirely by the server. Clients send orders normally and receive responses normally - the dual-processor architecture is transparent.

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

### Multicast Subscriber

Receives and displays multicast market data.
```bash
./build/multicast_subscriber <multicast_group> <port>

# Example
./build/multicast_subscriber 239.255.0.1 5000
```

**Features:**
- Auto-detects CSV vs Binary protocol
- Real-time display of market data
- Statistics tracking (packets, messages, errors)
- Throughput calculation (messages/sec)
- Signal handling (Ctrl+C shows statistics)

**Output Example:**
```
[Multicast Subscriber] Joined group 239.255.0.1:5000
[Multicast Subscriber] Waiting for market data...

[CSV] A, IBM, 1, 1
[CSV] T, IBM, 1, 1, 2, 1, 10000, 50
[CSV] B, IBM, B, -, -

--- Statistics ---
Packets received: 3
Messages: 3
Elapsed: 1.234s
Throughput: 2.43 msg/sec
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

### Multicast

**Do:**
- Use organization-local scope (239.x.x.x) for LANs
- Set appropriate TTL (1 for local subnet)
- Handle packet loss gracefully (UDP is best-effort)
- Run multiple subscribers to test distribution
- Use `SO_REUSEADDR` for multiple subscribers on same machine

**Don't:**
- Assume reliable delivery (unlike TCP)
- Use global multicast addresses without infrastructure
- Exceed network MTU (typically 1500 bytes)
- Forget to join multicast group before receiving

### TCP vs UDP vs Multicast

**TCP (recommended for order entry):**
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

**Multicast (for market data distribution):**
- One send reaches all subscribers
- Zero per-subscriber overhead
- Best-effort delivery (like UDP)
- Industry standard for market data
- Scales to unlimited subscribers

### Mixed Protocol Operation

You can mix protocols in the same system:
```bash
# Server accepts both CSV and Binary, broadcasts via multicast
./build/matching_engine --tcp --multicast 239.255.0.1:5000

# Client 1 uses CSV over TCP
./build/tcp_client localhost 1234

# Client 2 uses Binary over TCP
./build/binary_client 1234 --tcp

# Subscriber receives via multicast (auto-detects format)
./build/multicast_subscriber 239.255.0.1 5000
```

The server auto-detects and handles all correctly!

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

### Multicast Errors

**Common errors:**
- Failed to join group (network doesn't support multicast)
- Packet loss under high load
- TTL too low (packets don't reach subnet)

**Subscriber behavior:**
- Logs join failures
- Tracks parse errors in statistics
- Continues receiving (best-effort)

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

**Multicast:** Same output formats as above, delivered via UDP multicast (no length framing).

**Dual-Processor:** Symbol routing (A-M → P0, N-Z → P1) is transparent to clients.

---

## See Also

- [Quick Start Guide](QUICK_START.md) - Get running quickly
- [Architecture](ARCHITECTURE.md) - System internals, dual-processor, and multicast design
- [Testing](TESTING.md) - Protocol and multicast testing guide
- [Build Instructions](BUILD.md) - CMake build setup
