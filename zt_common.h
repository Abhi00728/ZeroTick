#ifndef ZT_COMMON_H
#define ZT_COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <stdatomic.h>

// 1. VIRTUAL WIRE CONSTANTS

#define ZT_SHM_NAME       "/zerotick_whiteboard"
#define IFACE_EXCH        "veth-exch"
#define IFACE_ZT          "veth-zt"

// Raw Ethernet Header is 14 bytes. Our custom payload sits right at byte 14!
// Using a custom EtherType (0x88B5 = Local Experimental Ethernet Protocol)
// means we don't even need bloated IP/UDP headers on our raw wire!

#define ZT_ETH_HEADER_LEN 14
#define ZT_ETHERTYPE      0x88B5

static const uint8_t MAC_EXCH[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
static const uint8_t MAC_ZT[6]   = {0x02, 0x00, 0x00, 0x00, 0x00, 0x02};

// 2. CUSTOM WIRE PROTOCOL STRUCTS (Layer 1)

#define ZT_ITCH_MAGIC 0x5A544954 // "ZTIT"
#define ZT_OUCH_MAGIC 0x5A544F55 // "ZTOU"

// Order Side Constants
#define ACTION_NONE        0
#define ACTION_BUY         1
#define ACTION_SELL        2
#define ACTION_KILL_SWITCH 3

// Incoming Market Tick Packet (32 Bytes) : Exchange ---> ZeroTick
typedef struct __attribute__((packed)) {
    uint32_t magic;        // Always ZT_ITCH_MAGIC
    uint32_t seq_num;      // Packet # (1, 2, 3...)
    uint64_t timestamp_ns; // Stopwatch start time (nanoseconds)
    uint32_t symbol_id;    // 1 = BTC, 2 = ETH
    uint32_t bid_price;    // e.g., 64900
    uint32_t ask_price;    // e.g., 65000
    uint16_t bid_vol;      // e.g., 50
    uint16_t ask_vol;      // e.g., 20
} zt_itch_tick_t;

// Outgoing Order Execution Packet (24 Bytes) : ZeroTick ---> Exchange
typedef struct __attribute__((packed)) {
    uint32_t magic;           // Always ZT_OUCH_MAGIC
    uint32_t trigger_seq_num; // Which Tick # triggered this order
    uint64_t origin_ts_ns;    // Echoed stopwatch time from the tick
    uint32_t order_price;     // Price to buy/sell at
    uint16_t order_qty;       // Quantity to buy/sell
    uint8_t  action;          // ACTION_BUY (1), ACTION_SELL (2), ACTION_KILL_SWITCH (3)
    uint8_t  pad;             // Padding to align to 24 bytes
} zt_ouch_order_t;

// 3. SIMPLE OPCODES & INSTRUCTION STRUCT (Layer 2 & 3)

// Packet Field IDs for OP_LOAD_FIELD
#define FIELD_BID_PRICE 0
#define FIELD_ASK_PRICE 1
#define FIELD_BID_VOL   2
#define FIELD_ASK_VOL   3

// Our Dumb & Simple Opcode Numbers
typedef enum {
    OP_HALT        = 0, // Stop running for this packet
    OP_LOAD_FIELD  = 1, // R[dst] = Packet Field (bid_price, ask_price, etc.)
    OP_LOAD_IMM    = 2, // R[dst] = imm_val (Load a constant number like 65000)
    OP_ADD         = 3, // R[dst] = R[src1] + R[src2]
    OP_SUB         = 4, // R[dst] = R[src1] - R[src2]
    OP_CMP_GT      = 5, // R[dst] = (R[src1] > R[src2]) ? 1 : 0
    OP_CMP_LT      = 6, // R[dst] = (R[src1] < R[src2]) ? 1 : 0
    OP_JMP_IF_ZERO = 7, // If R[src1] == 0, skip to the end (HALT)
    OP_FIRE_BUY    = 8, // Fire BUY order:  price = R[src1], qty = imm_val
    OP_FIRE_SELL   = 9, // Fire SELL order: price = R[src1], qty = imm_val
    OP_KILL_SWITCH = 10 // Fire Risk KILL_SWITCH packet immediately!
} zt_opcode_t;

// A single compiled instruction (Clean 8-byte struct, zero bit-shifting needed!)
typedef struct {
    uint8_t opcode;   // One of zt_opcode_t above
    uint8_t dst;      // Target register (R0..R7)
    uint8_t src1;     // First input register (or FIELD_* ID)
    uint8_t src2;     // Second input register
    int32_t imm_val;  // Direct number (e.g. 65000 for price, or 10 for quantity)
} zt_instruction_t;

// 4. THE SHARED WHITEBOARD IN RAM (Shared Memory Layout)

#define MAX_INSTRUCTIONS 32
#define LOG_RING_SIZE    64

// A single program slot on the whiteboard
typedef struct {
    char             strategy_name[32];
    uint32_t         num_instructions;
    zt_instruction_t code[MAX_INSTRUCTIONS];
} zt_strategy_slot_t;

// A single entry for the live scoreboard (zt_top)
typedef struct {
    uint32_t seq_num;
    uint32_t bid_price;
    uint32_t ask_price;
    uint8_t  action_fired; // 0=None, 1=BUY, 2=SELL, 3=KILL
    uint32_t order_price;
    uint16_t order_qty;
    uint64_t latency_ns;   // Measured processing time
} zt_log_entry_t;

// The Master Shared Whiteboard Struct
typedef struct {
    uint32_t           initialized_magic; // 0xCAFEBABE when ready
    
    // Double-buffered strategy slots (Slot 0 and Slot 1) for instant hot-swap
    _Atomic uint32_t   active_slot;       // 0 or 1
    zt_strategy_slot_t slots[2];

    // Live counters for the dashboard
    _Atomic uint64_t   ticks_processed;
    _Atomic uint64_t   orders_fired;
    _Atomic uint64_t   kill_switches_tripped;
    _Atomic uint64_t   last_rtt_ns;
    _Atomic uint64_t   min_rtt_ns;
    _Atomic uint64_t   max_rtt_ns;
    _Atomic uint64_t   sum_rtt_ns;

    // Recent packet log for zt_top scoreboard
    _Atomic uint32_t   log_head;
    zt_log_entry_t     logs[LOG_RING_SIZE];
} zt_whiteboard_t;

// 5. HELPER UTILITIES (Stopwatch + Shared Whiteboard Loader)

// Returns current hardware clock in nanoseconds
static inline uint64_t zt_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

// Opens (or creates) the Shared Whiteboard in RAM so any program can use it
static inline zt_whiteboard_t* zt_open_whiteboard(bool create_if_missing) {
    int flags = create_if_missing ? (O_CREAT | O_RDWR) : O_RDWR;
    int fd = shm_open(ZT_SHM_NAME, flags, 0666);
    if (fd < 0) {
        return NULL;
    }
    if (create_if_missing) {
        if (ftruncate(fd, sizeof(zt_whiteboard_t)) != 0) {
            close(fd);
            return NULL;
        }
    }
    zt_whiteboard_t *wb = (zt_whiteboard_t*)mmap(
        NULL, sizeof(zt_whiteboard_t),
        PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0
    );
    close(fd);
    if (wb == MAP_FAILED) return NULL;

    // If this is the very first time creating it, initialize defaults
    if (create_if_missing && wb->initialized_magic != 0xCAFEBABE) {
        memset(wb, 0, sizeof(zt_whiteboard_t));
        wb->initialized_magic = 0xCAFEBABE;
        wb->min_rtt_ns = 999999999ULL;
    }
    return wb;
}

#endif // ZT_COMMON_H
