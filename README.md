# ZeroTick: Ultra-Low-Latency Layer-2 Market Reflex Engine & Bytecode Compiler

> **Sub-microsecond quantitative rule execution (**$\sim 109\text{ ns}$**), zero-copy Layer-2 raw socket packet interception, and lock-free atomic strategy hot-swapping in pure C.**

## 1. The Story: Why ZeroTick Exists

Let's start with a simple question: **What actually happens inside a computer during the milliseconds between a market price changing and an algorithmic trading bot firing an order?**

If you build a trading system the way most standard software is built today, a single market tick has to survive a slow, obstacle-filled journey before your strategy logic even sees it:

1. **The OS Network Tax:** A packet arrives at the network interface card (NIC) and is forced to climb the entire Linux kernel networking hierarchy,passing through firewall hooks (`iptables`/`nftables`), IPv4/IPv6 header validation and routing tables, and the heavyweight TCP state machine (sequence numbers, ACKs, congestion windows, and socket buffer copies).

2. **The Serialization Tax:** Finally, the data reaches userspace,usually formatted as a human-readable JSON or FIX string like `{"symbol": "BTC", "bid": 64840, "ask": 64850}`. Now your CPU wastes thousands of clock cycles scanning text strings, hunting for quotation marks and colons, and running `strtod()` or `atoi()` to turn ASCII characters into numbers.

3. **The Runtime Tax:** If your trading logic is written in a high-level language like Python, Java, or Node.js, you pay for dynamic type-checking, heap allocations, pointer chasing, and unpredictable Garbage Collection (GC) pauses that can freeze execution for milliseconds at the worst possible moment.

4. **The Deployment Tax:** Suppose market conditions shift and you want to deploy a new trading rule. In a conventional setup, you have to stop your trading process, recompile or reload the script, and restart—going completely blind and dropping thousands of live market packets during the transition.

By the time a traditional software stack finishes all four steps, **hundreds of microseconds to several milliseconds** have vanished. In quantitative systems engineering, that is an eternity.

**ZeroTick was built from scratch in pure C (with zero external library dependencies) to answer a radical systems question:**
*What happens if we rip out every single layer of standard abstractions and build a vertically integrated stack spanning **Computer Networks**, **Operating Systems**, and **Compilers & Language Systems**?*

## 2. How ZeroTick Works: A Conversational Tour of the Architecture

Instead of treating networking, IPC, and language execution as separate worlds, ZeroTick fuses them into a single, continuous pipeline where data never gets copied, parsed, or locked.

```
+-----------------------------------------------------------------------------------+
|                        POSIX SHARED MEMORY (/dev/shm/zerotick_whiteboard)         |
|                                                                                   |
|   +-------------------------------+       +-------------------------------+       |
|   |   SLOT 0 (Active Strategy)    |       |  SLOT 1 (Standby / Hot-Swap)  |       |
|   |  zt_instruction_t code[32]    |       |  zt_instruction_t code[32]    |       |
|   +-------------------------------+       +-------------------------------+       |
|                   ^                                       ^                       |
|                   | 1. Atomic Read (acquire)              | 2. Write Inactive &   |
|                   |    active_slot (0 or 1)               |    Atomic Flip!       |
+-------------------|---------------------------------------|-----------------------+
                    |                                       |
                    |                               +-------+-------+
                    |                               |    ztc.c      |
                    |                               |  5-Stage DSL  |
                    |                               |   Compiler    |
                    |                               +---------------+
                    |                                       ^
                    |                                       | Compiles .zs script
+-------------------+-------------------+           +-------+-------+
|             zt_engine.c               |           |  strategy.zs  |
|   Layer-2 Packet Reflex Engine & VM   |           +---------------+
|  - Zero-copy cast at Byte Offset 14   |
|  - 8-Register VM (R0..R7), ~109 ns    |
+--------+---------------------+--------+
         ^                     |
         | 32B ZT-ITCH         | 24B ZT-OUCH
         | Market Tick         | Order / Kill-Switch
         | (46B Raw Frame)     | (38B Raw Frame)
         |                     v
  [ veth-zt ] <===========> [ veth-exch ]   (Linux Virtual Ethernet Pair, EtherType 0x88B5)
         ^                     |
         |                     v
+--------+---------------------+--------+
|          exchange_blaster.c           |
|     Mock Exchange & Latency Probe     |
|  - Generates live order-book ticks    |
|  - Injects Dips, Spikes & Crashes     |
|  - Measures hardware-clock RTT (ns)   |
+---------------------------------------+

```

Let's walk through the three core pillars that make this work.

### Pillar I: Computer Networks — Intercepting Packets at Stage 1 of the Linux Kernel

When you look at `zt_engine.c`, you'll see standard C system calls like `socket()`, `bind()`, `recv()`, and `send()`. At first glance, a reader might wonder: *"Wait—if you're calling `recv()` and `send()`, aren't you just doing normal socket communication between two programs? How is `zt_engine` sitting at the front door of the network stack?"*

The magic lies in a **single line of C code** where we open the socket:

```
int sock_fd = socket(AF_PACKET, SOCK_RAW, htons(ZT_ETHERTYPE));

```

Changing those three arguments from a standard web socket (`AF_INET, SOCK_STREAM, 0`) to `AF_PACKET, SOCK_RAW, htons(0x88B5)` completely changes **where** our program plugs into the Linux kernel.

When a packet arrives on a Linux network interface, it enters the kernel's front door: a function inside `net/core/dev.c` called `__netif_receive_skb_core()`. Look at what happens right inside that entrance function:

```
   [ Physical / Virtual Wire: veth-zt ]
                    |
                    v
+-------------------------------------------------------+
| STAGE 1: NETWORK DRIVER ENTRANCE                      |
| (__netif_receive_skb_core in Linux net/core/dev.c)    |
| Kernel inspects Bytes 12..13 (EtherType = 0x88B5)     |
+-------------------+-----------------------------------+
                    |
                    +===> [ AF_PACKET + SOCK_RAW TAP (zt_engine.c) ]
                    |     Kernel sees EtherType 0x88B5 registered in ptype_base!
                    |     Delivers raw frame straight into zt_engine's rx_buf!
                    |     (THE PACKET'S KERNEL JOURNEY ENDS RIGHT HERE!)
                    |
   =================|====================================  <-- ZeroTick packets NEVER cross this line!
                    v
+-------------------------------------------------------+
| STAGE 2: LINUX FIREWALL / NETFILTER (iptables / nft)  |  (100% Bypassed!)
+-------------------------------------------------------+
                    |
                    v
+-------------------------------------------------------+
| STAGE 3: LAYER-3 IPv4 / IPv6 ROUTING (ip_rcv)         |  (100% Bypassed!)
+-------------------------------------------------------+
                    |
                    v
+-------------------------------------------------------+
| STAGE 4: LAYER-4 TCP / UDP STATE MACHINE (tcp_v4_rcv) |  (100% Bypassed!)
+-------------------------------------------------------+
                    |
                    v
         [ Normal AF_INET Socket ]
     (Where Python / Node.js / Standard C apps wait)

```

Why is our packet guaranteed never to cross that dotted line into Stages 2, 3, or 4?

1. **`AF_PACKET`** registers our socket directly inside the kernel's Layer-2 protocol jump table (`ptype_base`) at **Stage 1**.

2. **`SOCK_RAW`** instructs the kernel not to strip or parse any headers; it hands us the raw Ethernet frame starting at Byte 0 (the MAC addresses).

3. **Custom EtherType `0x88B5` (`ZT_ETHERTYPE`):** Standard Linux firewalls, IP routers, and TCP/UDP state machines only know how to process EtherType `0x0800` (IPv4) or `0x86DD` (IPv6). Because our packets carry custom EtherType `0x88B5`, the upper layers of the OS networking stack don't even have a handler for them! The kernel intercepts the frame at Stage 1, hands it straight to `zt_engine`, and drops out.

#### Zero-Copy Pointer Casting at Byte Offset 14

Once those raw bytes land in `uint8_t rx_buf[128]`, we don't parse strings or copy fields. Every Ethernet header is exactly **14 bytes** (`6B Dst MAC + 6B Src MAC + 2B EtherType`). So `zt_engine.c` simply advances a pointer 14 bytes forward and casts the raw wire memory directly into our packed 32-byte market tick struct:

```
const zt_itch_tick_t *tick = (const zt_itch_tick_t *)(rx_buf + ZT_ETH_HEADER_LEN);

```

In **0 nanoseconds of parsing time**, `tick->bid_price`, `tick->ask_price`, and `tick->timestamp_ns` are ready in CPU registers.

### Pillar II: Compilers & Language Systems — The `ZeroScript` DSL & Register VM

Hardcoding trading strategies in C would mean recompiling the engine every time a quantitative researcher wants to tweak a threshold. On the other hand, embedding a Python or Lua interpreter inside a raw packet loop would destroy our nanosecond latency budget.

To get the best of both worlds, we designed **ZeroScript (`.zs`)**—a domain-specific language for tick-level trading rules,and built **`ztc` (`ztc.c`)**, a standalone 5-stage compiler and static safety verifier:

```
  [ Source Text (.zs) ]
   "LET spread = ask_price - bid_price"
   "IF spread > 500 * 2 THEN KILL_SWITCH"
            |
            v
  STAGE 1: LEXER            ---> Tokenizes source into [LET] [spread] [=] [ask_price] ...
            |
            v
  STAGE 2: AST PARSER       ---> Builds an Abstract Syntax Tree of math & conditions
            |
            v
  STAGE 3: OPTIMIZER PASS   ---> Constant Folding! Spots (500 * 2) in the AST and folds
                                 it into (1000) at compile time—saving runtime ALU cycles!
            |
            v
  STAGE 4: STATIC VERIFIER  ---> Proves: Registers <= 8, Instructions <= 32, Loops == 0
            |
            v
  STAGE 5: CODEGEN & SWAP   ---> Emits 8-byte zt_instruction_t[] opcodes into inactive
                                 Whiteboard slot & atomically flips active_slot!

```

* **Why Static Verification Matters:** In high-speed packet processing, an infinite loop or a memory leak inside a strategy script will lock up the network engine. `ztc` refuses to compile any script that uses more than **8 hardware virtual registers (`R0..R7`)**, exceeds **32 instructions**, or contains backward jumps/loops. Because the instruction graph is strictly acyclic, every ZeroScript program is mathematically guaranteed to terminate in a few dozen nanoseconds.

* **Cache-Line-Sized Bytecode (`zt_instruction_t`):** Every compiled instruction is exactly **8 bytes** (`opcode`, `dst`, `src1`, `src2`, `imm_val`). A complete 6-instruction trading strategy occupies just **48 bytes of RAM**—fitting entirely inside a **single 64-byte CPU L1 cache line**!

### Pillar III: Operating Systems — Lock-Free POSIX Shared Memory & Atomic Double-Buffering

Now comes the classic Operating Systems challenge: **How does the compiler (`ztc`) replace the bytecode inside `zt_engine` while `zt_engine` is actively intercepting packets at wire speed?**

* If we used a `pthread_mutex_lock()` around the bytecode array, `zt_engine` would have to acquire and release a lock on every single packet—and worse, when `ztc` updated the strategy, `zt_engine` would stall waiting for the mutex.

* Instead, ZeroTick uses **Lock-Free Atomic Double-Buffering** inside a POSIX Shared Memory segment (`/dev/shm/zerotick_whiteboard`) mapped into both processes via `shm_open()` and `mmap()`.

Here is how the zero-downtime hot-swap works:

1. The Shared Whiteboard contains two complete strategy slots: **`slots[0]`** and **`slots[1]`**, plus a single atomic integer: **`atomic_uint active_slot`**.

2. Suppose `zt_engine` is currently executing **`slots[0]`** (`active_slot == 0`).

3. When you run `sudo ./ztc spike_seller.zs`, the compiler checks `active_slot`, sees that `0` is busy, and writes all the new opcodes quietly into **`slots[1]`** (the standby slot).

4. Once `slots[1]` is 100% ready in RAM, `ztc` executes a single C11 atomic release store:

   ```
   atomic_store_explicit(&wb->active_slot, target_slot, memory_order_release);
   
   ```

5. On the very next packet, `zt_engine` performs an atomic acquire load (`atomic_load_explicit(&wb->active_slot, memory_order_acquire)`), sees `1`, and immediately executes the new strategy from **`slots[1]`**.

6. **Result:** Zero mutexes. Zero OS context switches. Zero dropped packets. $0\text{ ns}$ **downtime.**

## 3. Wait—Is This Realistic? (Real-World Wall Street & NSE HFT Parallels)

When sharp engineers read through this codebase, two big questions naturally pop up. Let me guess and answer both of them head-on, because they reveal how closely ZeroTick mirrors real-world institutional High-Frequency Trading (HFT) infrastructure.

### Question 1: *"In ZeroTick, you control `exchange_blaster.c`, so you put your struct right at Byte 14 after the Ethernet header (`0x88B5`). In real life, an exchange broadcasts to hundreds of firms using standard protocols, so isn't skipping straight to Byte 14 a cheat?"*

**Nope that’s the trick!!, and it is almost byte-for-byte how real kernel-bypass HFT engines work!**

In the real world, exchanges like NASDAQ or the NSE broadcast market data to hundreds of trading firms simultaneously inside a co-location data center using **Layer-3 IP Multicast + UDP** (for example, NASDAQ's **TotalView-ITCH 5.0** over **MoldUDP64**, which is where we got the names `ZT-ITCH` and `ZT-OUCH`!).

Because real exchanges use UDP Multicast so hardware switches can replicate packets to 500 firms at once, a real HFT packet has a 20-byte IPv4 header and an 8-byte UDP header sitting between the 14-byte Ethernet header and the binary payload.

Look at a real NASDAQ ITCH packet next to a ZeroTick packet:

```
ZEROTICK FRAME (Direct Point-to-Point Layer-2 Wire):
+------------------------+----------------------------------------------------+
| 14B Ethernet Header    | 32B ZT-ITCH Packed Binary Struct                   |
+------------------------+----------------------------------------------------+
^                        ^
rx_buf + 0               rx_buf + 14  <-- Where zt_engine.c casts the pointer!


REAL-WORLD NASDAQ / NSE HFT FRAME (Layer-3 UDP Multicast):
+---------------+---------------+---------------+-----------------------------+
| 14B Ethernet  | 20B IPv4 Hdr  |  8B UDP Hdr   | Real NASDAQ ITCH 5.0 Struct |
+---------------+---------------+---------------+-----------------------------+
^                                               ^
rx_buf + 0                                      rx_buf + 42  <-- Where a real HFT casts the pointer!

```

Real HFT firms **do not** let the Linux kernel parse those IP and UDP headers! Using kernel-bypass frameworks (like Solarflare `ef_vi`, DPDK, or eBPF/XDP), their C/C++ engine grabs the raw Ethernet frame off the network card, jumps a fixed **42 bytes** (`14B Eth + 20B IP + 8B UDP`), and casts `rx_buf + 42` directly into a `__attribute__((packed))` C struct pointer—the exact same zero-copy technique `zt_engine.c` performs at `rx_buf + 14`!

Even our outgoing order buffer (`tx_buf`) in `zt_engine.c` uses a classic Wall Street technique called **Packet Templating**:

* Before trading starts, `zt_engine.c` pre-fills the Ethernet header in `tx_buf` once. When an order triggers inside the hot loop, we only overwrite `order_price` and `order_qty` at Byte 14 and immediately fire the frame.

* Real HFT engines do the exact same thing: they pre-populate the 42-byte Ethernet/IP/TCP headers in L1 cache before the market opens, and mutate only the price/quantity bytes in-place when firing an order.

### Question 2: *"Wait, then if HFT engines skip the header and blindly cast the payload into a struct without encryption, what stops an adversary from sending a fake packet of the exact same size to trick a firm into buying or selling millions?"*

If high-frequency trading happened over the public internet, skipping straight to the payload without TLS encryption would bankrupt a firm in seconds. Yet real exchanges **do not encrypt** high-speed market feeds because decryption costs microseconds.

Instead, real exchanges and ZeroTick rely on **four layers of physical and hardware isolation**:

1. **Physical Air-Gapping (Co-Location Cross-Connects):** Real HFT market feeds never touch the public internet. HFT firms rent server racks inside the exchange's own data center building (Co-Location) and run a dedicated, physically locked fiber-optic cable,a **Direct Cross-Connect**,straight from the exchange's switch into their network card. In ZeroTick, our virtual Ethernet cable (`veth-exch <===> veth-zt`) models this exact point-to-point Cross-Connect tunnel.

2. **Switch Silicon Port Isolation (Unidirectional Multicast ACLs):**
   What if a rival HFT firm plugged into the same exchange switch tries to inject a fake market tick back up their cable? On the exchange's hardware switch ASIC, only the Exchange Server's physical port has transmit permissions for the multicast group; every trading firm's port is hardware-locked to **Receive-Only** via Ingress Access Control Lists (ACLs). A spoofed packet is dropped by the switch silicon in $0\text{ ns}$.

3. **NIC Hardware Filtering (Before C Code Runs):**
   Why doesn't the C code check the Ethernet/IP/UDP headers? Because the **Network Card's silicon chip** checks them first! Only frames matching the exact interface, MAC, and protocol filter (`bind()` to `IFACE_ZT` and `htons(ZT_ETHERTYPE)` in our engine) are placed into `rx_buf`.

4. **Protocol Magic & Monotonic Sequence Numbers (`magic` & `seq_num`):**
   Every `ZT-ITCH` tick carries a 32-bit protocol signature (`0x5A544954` / `"ZTIT"`) and a strictly increasing sequence number (`seq_num = 1, 2, 3...`), mirroring NASDAQ MoldUDP64 sequence validation.

## 4. Wire Protocol & VM Bytecode Specifications (`zt_common.h`)

### Incoming Market Tick: `ZT-ITCH` (32B Payload / 46B Frame)

| Byte Offset | Field | C Type | Description | 
| ----- | ----- | ----- | ----- | 
| `0 .. 13` | `eth_hdr` | `uint8_t[14]` | `Dst MAC (6B)` + `Src MAC (6B)` + `EtherType 0x88B5 (2B)` | 
| `14 .. 17` | `magic` | `uint32_t` | `0x5A544954` (`"ZTIT"` wire verification magic) | 
| `18 .. 21` | `seq_num` | `uint32_t` | Monotonically increasing tick sequence counter | 
| `22 .. 29` | `timestamp_ns` | `uint64_t` | Hardware `CLOCK_MONOTONIC_RAW` nanosecond timestamp | 
| `30 .. 33` | `symbol_id` | `uint32_t` | Instrument ID (`1 = BTC`) | 
| `34 .. 37` | `bid_price` | `uint32_t` | Best Bid Price (fixed-point integer, zero FPU overhead) | 
| `38 .. 41` | `ask_price` | `uint32_t` | Best Ask Price (fixed-point integer, zero FPU overhead) | 
| `42 .. 43` | `bid_vol` | `uint16_t` | Available Bid Liquidity Volume | 
| `44 .. 45` | `ask_vol` | `uint16_t` | Available Ask Liquidity Volume | 

### Outgoing Reflex Order: `ZT-OUCH` (24B Payload / 38B Frame)

| Byte Offset | Field | C Type | Description | 
| ----- | ----- | ----- | ----- | 
| `0 .. 13` | `eth_hdr` | `uint8_t[14]` | `Dst MAC (6B)` + `Src MAC (6B)` + `EtherType 0x88B5 (2B)` | 
| `14 .. 17` | `magic` | `uint32_t` | `0x5A544F55` (`"ZTOU"` wire verification magic) | 
| `18 .. 21` | `trigger_seq_num` | `uint32_t` | Echoed `seq_num` of the tick that triggered this action | 
| `22 .. 29` | `origin_ts_ns` | `uint64_t` | Echoed nanosecond timestamp for exact wire-to-wire RTT math | 
| `30 .. 33` | `order_price` | `uint32_t` | Execution price (`0` when `KILL_SWITCH` is tripped) | 
| `34 .. 35` | `order_qty` | `uint16_t` | Order quantity to buy or sell | 
| `36` | `action` | `uint8_t` | `1 = ACTION_BUY`, `2 = ACTION_SELL`, `3 = ACTION_KILL_SWITCH` | 
| `37` | `pad` | `uint8_t` | Explicit struct alignment byte | 

### The ZeroTick Virtual Machine ISA (`zt_instruction_t`)

| Opcode | Mnemonic | VM Execution Semantics (`int64_t R[8]`) | 
| ----- | ----- | ----- | 
| `0x00` | `OP_HALT` | Stop bytecode execution for the current packet | 
| `0x01` | `OP_LOAD_FIELD` | `R[dst] = tick.<src1>` (`0=bid_price, 1=ask_price, 2=bid_vol, 3=ask_vol`) | 
| `0x02` | `OP_LOAD_IMM` | `R[dst] = imm_val` (Load 32-bit signed constant) | 
| `0x03` | `OP_ADD` | `R[dst] = R[src1] + R[src2]` | 
| `0x04` | `OP_SUB` | `R[dst] = R[src1] - R[src2]` | 
| `0x05` | `OP_CMP_GT` | `R[dst] = (R[src1] > R[src2]) ? 1 : 0` | 
| `0x06` | `OP_CMP_LT` | `R[dst] = (R[src1] < R[src2]) ? 1 : 0` | 
| `0x07` | `OP_JMP_IF_ZERO` | If `R[src1] == 0`, exit VM early (rule condition not met) | 
| `0x08` | `OP_FIRE_BUY` | Stamp `ZT-OUCH` `BUY` order (`price = R[src1]`, `qty = imm_val`) & transmit | 
| `0x09` | `OP_FIRE_SELL` | Stamp `ZT-OUCH` `SELL` order (`price = R[src1]`, `qty = imm_val`) & transmit | 
| `0x0A` | `OP_KILL_SWITCH` | Stamp `ZT-OUCH` `KILL_SWITCH` emergency signal & transmit | 

## 5. Engineering Case Study: How We Crushed RTT by $650\times$ ($10.2\text{ ms} \rightarrow 14.2\ \mu\text{s}$)

One of the most rewarding parts of building ZeroTick was diagnosing a massive latency anomaly during our early test runs,and fixing it using pure systems principles.

### The Mystery of the $10,164,991\text{ ns}$ Round-Trip Time

In our first working prototype, the 6-instruction VM was fast, yet `exchange_blaster` reported a wire-to-wire Round-Trip Time (RTT) of **`10,164,991 ns` (**$\sim 10.16\text{ ms}$**)** on every triggered order, and `zt_engine` reported a VM execution time of **`19,375 ns`**. Why?

We tracked down **three distinct OS & timing culprits**:

1. **Culprit #1: The `usleep(10000)` Blind Spot in `exchange_blaster.c`**
   In Normal Cruise Mode (100 ticks/sec, or 1 tick every $10\text{ ms}$), `exchange_blaster` originally called `send()` at $t = 0\text{ ns}$, immediately called `recv(..., MSG_DONTWAIT)` once at $t = 50\text{ ns}$, saw no reply on the wire yet (since the packet was still crossing the kernel `veth` driver), hit `break;`, and put itself to sleep via **`usleep(10000)` (**$10,000,000\text{ ns}$**)**!
   When `zt_engine`'s reply arrived at $t \approx 15,000\text{ ns}$, `exchange_blaster` was fast asleep inside `usleep(10000)`. It didn't wake up to read the socket buffer until the *next* loop iteration ($10\text{ ms}$ later)—accidentally baking its own $10,000,000\text{ ns}$ sleep right into the RTT stopwatch!

2. **Culprit #2: Blocking `recv(..., 0)` OS Scheduler Eviction in `zt_engine.c`**
   Calling blocking `recv()` with flags set to `0` caused the Linux OS scheduler to mark `zt_engine` as sleeping between ticks, evicting it from the CPU core and cooling down the L1/L2 caches. Waking the process back up on an interrupt added tens of microseconds.

3. **Culprit #3: Measuring the Kernel `send()` Syscall Inside the VM Timer**
   Our initial stopwatch wrapped both the register VM loop *and* the `send()` system call, mixing user-space bytecode execution with kernel `sk_buff` allocation.

### The Fix: Active Clock Polling & Non-Blocking Spin Loops

* We eliminated `usleep(10000)` completely and replaced it with an **Active Listen Window** driven by `CLOCK_MONOTONIC_RAW`:

  ```
  uint64_t next_tick_ns = tick->timestamp_ns + (turbo_mode ? 20000ULL : 10000000ULL);
  while (keep_running && zt_now_ns() < next_tick_ns) {
      ssize_t n = recv(sock_fd, rx_buff, sizeof(rx_buff), MSG_DONTWAIT);
      if (n < (ssize_t)(ZT_ETH_HEADER_LEN + sizeof(zt_ouch_order_t))) {
          continue; // Stay wide awake listening to the wire!
      }
      // Timestamp & process return order the exact nanosecond it arrives!
  }
  
  ```

* We switched `zt_engine.c` to non-blocking `recv(..., MSG_DONTWAIT)` so the engine never yields its CPU core, and separated the pure VM execution timer from the kernel `send()` call.

### Final Measured Benchmarks (Ubuntu Linux x86_64, GCC `-O3`)

| Benchmark Metric | Before Optimization | After Optimization | Speedup | 
| ----- | ----- | ----- | ----- | 
| **Pure VM Bytecode Execution (`zt_engine`)** | $19,375\text{ ns}$ *(included syscall)* | **`109 ns – 365 ns`** | $53\times - 177\times$ | 
| **Wire-to-Wire Round-Trip Time (`RTT`)** | $10,164,991\text{ ns}$ ($\sim 10.16\text{ ms}$) | **`14,251 ns – 17,062 ns`** ($\sim 14.2\ \mu\text{s}$) | $> 650\times$ | 
| **Live Strategy Hot-Swap Downtime (`ztc`)** | Process restart required | **`0 ns` (Single Atomic Flip)** | **Zero packet drops** | 

## 6. How to Build & Run ZeroTick on Your Machine

ZeroTick runs on any Linux environment (Ubuntu, Debian, Fedora, Arch, or WSL2). It requires **zero external libraries**—only `gcc`, `iproute2`, and standard POSIX headers.

### Step 1: Clone the Repo & Set Up the Virtual Ethernet (`veth`) Wire

Run `setup_veth.sh` once per boot. It creates the virtual cross-connect cable (`veth-exch <===> veth-zt`), assigns deterministic MAC addresses (`02:00:00:00:00:01` and `02:00:00:00:00:02`), and disables kernel checksum offloading:

```
git clone https://github.com/Abhi00728/ZeroTick
cd ZeroTick
chmod +x setup_veth.sh
sudo ./setup_veth.sh

```

### Step 2: Compile the Binaries (Zero Warnings with `-Wall -Wextra`)

```
gcc -O3 -Wall -Wextra exchange_blaster.c -o exch_blaster -lrt
gcc -O3 -Wall -Wextra zt_engine.c -o zt_engine -lrt
gcc -O3 -Wall -Wextra ztc.c -o ztc -lrt

```

## 7. Interactive 3-Terminal Demo Walkthrough

To experience ZeroTick properly, open **3 terminal windows side-by-side** in your project directory:

### Terminal 1: Boot the Packet Reflex Engine (`zt_engine`)

*(Tip: If you want to wipe any previously cached strategy from RAM first, run `sudo rm -f /dev/shm/zerotick_whiteboard`)*

```
sudo ./zt_engine

```

When started on a clean whiteboard, `zt_engine` loads a default 6-opcode dip-buying strategy into **Slot 0**:
`IF ask_price < 64900 THEN BUY 10 @ ask_price`

### Terminal 2: Start the Mock Exchange Blaster (`exch_blaster`)

```
sudo ./exch_blaster

```

`exch_blaster` begins streaming 46-byte raw `ZT-ITCH` Ethernet frames (`100 ticks/sec` in normal mode, or pass `--turbo` for unthrottled benchmarking). As it streams normal market ticks around `65,000`, it automatically injects three market events:

* **Every 100th Tick:** *Flash Dip!* (`Ask = 64850`)

* **Every 250th Tick:** *Price Spike!* (`Bid = 65250`)

* **Every 500th Tick:** *Liquidity Crash / Spread Blowout!* (`Bid = 64000, Ask = 65500` $\rightarrow$ `Spread = 1500`)

**Watch Terminals 1 & 2:** Every 100th tick (`#100`, `#200`, `#300`...), `zt_engine` intercepts the Flash Dip, runs the VM in **`~109–350 ns`**, and blasts back a `FIRE_BUY` order caught by `exch_blaster` in **`~15,000 ns` (`15 µs`) RTT**!

### Terminal 3: Live-Compile & Hot-Swap Strategies on the Fly (`ztc`)

**Do not stop Terminal 1 or Terminal 2!** While packets are actively flying across the wire, use Terminal 3 to compile and inject brand-new `.zs` strategies into the running engine:

#### Demo A: Hot-Swap from Dip-Buyer to Spike-Seller (`spike_seller.zs`)

Create `spike_seller.zs` (or use the one in the repo):

```
cat << 'EOF' > spike_seller.zs
# Sell 25 units whenever Bid jumps above 65200
# Notice 65000 + 200 -> ztc constant-folds this into 65200 at compile time!
IF bid_price > 65000 + 200 THEN SELL 25 AT bid_price
EOF

```

Compile and hot-swap it live:

```
sudo ./ztc spike_seller.zs

```

**What happens immediately:**

1. `ztc` tokenizes the script, builds the AST, prints `[OPT PASS] Constant Folding: (65000 + 200) ---> 65200`, verifies `Registers used: 3/8`, prints the full bytecode disassembly, writes the opcodes into **Slot 1**, and atomically flips `active_slot` from `0` to `1`.

2. Look at Terminals 1 & 2: **without dropping a single tick**, `zt_engine` immediately ignores the Tick #100 dips and instead fires **`Action: SELL | Price: 65250 | Qty: 25`** exclusively on every **250th tick** (`#250`, `#750`, `#1250`...)!

#### Demo B: Hot-Swap a Pre-Trade Risk Kill-Switch (`spread_guard.zs`)

```
cat << 'EOF' > spread_guard.zs
# Trip the emergency KILL_SWITCH if the Bid-Ask spread blows out past 1000
LET spread = ask_price - bid_price
IF spread > 500 * 2 THEN KILL_SWITCH
EOF

sudo ./ztc spread_guard.zs

```

**What happens immediately:**

* `ztc` folds `500 * 2` into `1000`, binds the `spread` variable to hardware register `R2`, writes the bytecode into **Slot 0**, and atomically flips `active_slot` back to `0`.

* On every **500th tick** (`Spread = 1500`), `zt_engine` fires `KILL_SWITCH` across the wire!

## 8. Repository File Map

```
ZeroTick/
├── setup_veth.sh        # Creates the veth-exch <-> veth-zt raw Layer-2 Ethernet tunnel
├── zt_common.h          # Packed wire structs (ZT-ITCH/ZT-OUCH), VM ISA & POSIX SHM layout
├── exchange_blaster.c   # Mock Exchange: blasts raw ticks & measures nanosecond wire RTT
├── zt_engine.c          # Reflex Engine: Stage-1 raw socket interceptor & 8-register VM
├── ztc.c                # 5-stage ZeroScript DSL Compiler, AST Optimizer & Hot-Swapper
├── spike_seller.zs      # Sample strategy: sells 25 units into price spikes > 65200
├── spread_guard.zs      # Sample strategy: trips KILL_SWITCH when bid-ask spread > 1000
└── README.md            # Complete system architecture & engineering documentation

```