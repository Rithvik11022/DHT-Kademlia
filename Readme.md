# Kademlia DHT Implementation

A C++17 implementation of the Kademlia Distributed Hash Table (DHT) protocol, based on the original paper by Maymounkov and Mazières.

## Overview

Kademlia is a peer-to-peer distributed hash table that provides efficient key-value storage and lookup in decentralized networks. This implementation includes:

- **160-bit Node IDs** using XOR distance metric
- **K-bucket routing tables** with LRU eviction (K=20)
- **Iterative routing** with α=3 parallel lookups
- **UDP-based RPC protocol** for peer communication
- **Key-value storage** with automatic republishing

## Architecture

### Core Components

1. **Node (`node.h/cpp`)**: Main DHT node implementation
   - RPC handlers (PING, FIND_NODE, FIND_VALUE, STORE)
   - Iterative lookup algorithms
   - Bootstrap and network joining

2. **Routing Table (`routing_table.h/cpp`)**: K-bucket based routing
   - 160 buckets (one per bit in NodeID space)
   - LRU eviction policy per bucket
   - XOR distance-based closest node selection

3. **Storage (`storage.h/cpp`)**: Thread-safe key-value store
   - Concurrent read/write with shared_mutex
   - Hex-encoded key storage

4. **Network (`network.h/cpp`)**: UDP communication layer
   - Asynchronous message handling
   - Synchronous request-response with timeouts
   - Pipe-delimited text protocol

### Kademlia Protocol Details

#### XOR Distance Metric
```
distance(A, B) = A ⊕ B (bitwise XOR)
```

Properties:
- `d(x,x) = 0`
- `d(x,y) > 0` if `x ≠ y`
- `d(x,y) = d(y,x)` (symmetric)
- Triangle inequality: `d(x,y) + d(y,z) ≥ d(x,z)`

#### K-Bucket Structure
- **160 buckets** indexed by prefix length (distance from self)
- Bucket `i` stores nodes with distance `2^i ≤ d < 2^(i+1)`
- Each bucket holds up to **K=20** nodes
- **LRU eviction**: new contacts move to head, old contacts dropped when full

#### Iterative Lookup Algorithm

**FIND_NODE(target)**:
1. Start with α=3 closest nodes from routing table
2. Query α nodes in parallel for closer nodes
3. Add new nodes to candidate list
4. Sort by XOR distance to target
5. Repeat until no closer nodes found or K closest discovered

**FIND_VALUE(key)**:
- Same as FIND_NODE, but stops when value is found
- Returns value or K closest nodes

**STORE(key, value)**:
1. Perform FIND_NODE(key) to find K closest nodes
2. Send STORE RPC to all K closest nodes
3. Ensures replication across K nodes

### RPC Protocol

All messages are pipe-delimited (`|`) and newline-terminated:

```
PING|<sender_id>|<sender_addr>
→ PONG|1|<responder_id>

FIND_NODE|<sender_id>|<sender_addr>|<target_id>
→ FIND_NODE_REPLY|<addr1>,<id1>;<addr2>,<id2>;...

FIND_VALUE|<sender_id>|<sender_addr>|<key_hex>
→ FIND_VALUE_REPLY|VALUE|<value>
→ FIND_VALUE_REPLY|NODES|<addr1>,<id1>;<addr2>,<id2>;...

STORE|<sender_id>|<sender_addr>|<key_hex>|<value>
→ STORE_REPLY|OK
```

## Building

```bash
make
```

Requires:
- g++ with C++17 support
- pthread library

## Usage

### Starting a Node

```bash
# Start bootstrap node
./bin/kademlia --port 3000

# Start additional nodes
./bin/kademlia --port 3001 --bootstrap <bootstrap_ip>:3000
./bin/kademlia --port 3002 --bootstrap <bootstrap_ip>:3000
```

### Commands

**STORE key value** - Store a key-value pair
```
> STORE abc123... myvalue
```
- Finds K closest nodes to key
- Replicates value across those nodes

**FINDVAL_AT key** - Find value by key
```
> FINDVAL_AT abc123...
VALUE: myvalue
```
- Returns value if found
- Returns closest nodes if not found

**FINDVAL_TRACE key** - Find value with trace output
```
> FINDVAL_TRACE abc123...
+-- 192.168.1.1:3001 || 192.168.1.2:3002 || 192.168.1.3:3003--+
|
VALUE: myvalue
```
- Shows routing path taken during lookup

**FINDNODE hexid** - Find nodes closest to ID
```
> FINDNODE def456...
Found nodes:
  192.168.1.1:3001 id=...
  192.168.1.2:3002 id=...
```

**HELP** - Show help

**QUIT** - Exit node

## Key Features

### Automatic Republishing
- Every 3600 seconds (1 hour), stored values are republished
- Ensures data persistence in face of node churn
- Implementation in `main.cpp` event loop

### Network Address Detection
- Automatically detects non-loopback IPv4 address
- Ensures nodes can communicate across network
- Falls back to 127.0.0.1 if no network interface found

### Thread-Safe Operations
- Routing table uses mutex for concurrent access
- Storage uses shared_mutex for reader-writer pattern
- Network layer handles concurrent RPC requests

### Bootstrap Process
1. PING bootstrap node to verify connectivity
2. FIND_NODE for self ID to populate routing table
3. Nodes from response are added to routing table
4. Node is now part of network and can route requests

## Protocol Parameters

| Parameter | Value | Description |
|-----------|-------|-------------|
| ID_BYTES | 20 | 160-bit node IDs |
| K_BUCKET_SIZE | 20 | Max nodes per k-bucket |
| ALPHA | 3 | Parallelism for lookups |
| RPC Timeout | 500-800ms | Request timeout |
| Republish Interval | 3600s | Value republishing period |

## Implementation Notes

### Differences from Paper

1. **No bucket splitting**: Fixed 160 buckets instead of dynamic splitting
2. **Simple LRU**: No PING-based verification before eviction
3. **No caching**: Nodes don't cache values along lookup path
4. **Synchronous lookups**: Iterative lookups block, not fully async
5. **No key republishing by original publisher**: Only storing nodes republish

### Limitations

- UDP packet size limited to 4096 bytes
- No encryption or authentication
- No NAT traversal support
- Single-threaded RPC handling per node
- No persistent storage (data lost on restart)

## Testing

See `test_kademlia.sh` for automated testing suite that verifies:
- Network formation and bootstrap
- STORE/FIND_VALUE operations
- Iterative lookup convergence
- K-replication of stored values
- Routing table convergence
- Node departure handling

## References

Maymounkov, P., & Mazières, D. (2002). *Kademlia: A peer-to-peer information system based on the XOR metric*. In International Workshop on Peer-to-Peer Systems (pp. 53-65). Springer, Berlin, Heidelberg.

Paper: https://pdos.csail.mit.edu/~petar/papers/maymounkov-kademlia-lncs.pdf

## License

Public Domain / MIT - See individual file headers