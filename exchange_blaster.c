#include "zt_common.h"
#include <sys/socket.h>
#include <linux/if_packet.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <signal.h>

static volatile int keep_running = 1;

void handle_sigint(int sig){
        (void)sig;
        keep_running = 0;
}

int main(int argc,char** argv){
        struct sigaction sa = {0};
        sa.sa_handler = handle_sigint;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGINT, &sa, NULL);

        bool turbo_mode = (argc > 1 && strcmp(argv[1],"--turbo") == 0);

        zt_whiteboard_t* wb = zt_open_whiteboard(true);
        if(!wb){
                perror("FAILED TO OPEN SHARED WHITEBOARD");
                return 0;
        }

        int sock_fd = socket(AF_PACKET,SOCK_RAW,htons(ZT_ETHERTYPE));
        if(sock_fd < 0){
                perror("SOCKET(AF_PACKET) FAILED!(did u run sudo?)");
                return 0;
        }

        int if_idx = if_nametoindex(IFACE_EXCH);
        if(if_idx == 0){
                perror("couldn't find veth-exch interface, try running the setup_veth bash script first!");
                close(sock_fd);
                return 0;
        }
        struct sockaddr_ll bind_addr = {0};
        bind_addr.sll_family = AF_PACKET;
        bind_addr.sll_protocol = htons(ZT_ETHERTYPE);
        bind_addr.sll_ifindex = if_idx;

        if(bind(sock_fd,(struct sockaddr *)&bind_addr,sizeof(bind_addr)) < 0){
                perror("binding to the veth-exch failed!");
                close(sock_fd);
                return 0;
        }

        uint8_t tx_buf[ZT_ETH_HEADER_LEN + sizeof(zt_itch_tick_t)] = {0};
        memcpy(tx_buf+0,MAC_ZT,6);
        memcpy(tx_buf+6,MAC_EXCH,6);
        uint16_t eth_type = htons(ZT_ETHERTYPE);
        memcpy(tx_buf+12, &eth_type,2);
        zt_itch_tick_t *tick = (zt_itch_tick_t*)(tx_buf+ZT_ETH_HEADER_LEN);
        tick->magic = ZT_ITCH_MAGIC;
        tick->symbol_id = 1;

        printf("================================================================\n");
        printf("  ZEROTICK MOCK EXCHANGE BLASTER (Interface: %s)\n", IFACE_EXCH);
        printf("  Mode: %s | Protocol: ZT-ITCH (32B) / ZT-OUCH (24B)\n",
                        turbo_mode ? "TURBO BENCHMARK (Unthrottled)" : "NORMAL CRUISE (100 ticks/sec)");
        printf("================================================================\n");

        uint32_t seq = 0;
        uint8_t rx_buff[128];

        while(keep_running){
                seq++;
                tick->seq_num = seq;
                uint32_t drift = (seq % 7 ) * 3;
                tick->bid_price = 64990 + drift;
                tick->ask_price = 65000 + drift;
                tick->bid_vol = 25;
                tick->ask_vol = 20;

                if (seq % 500 == 0) {
                        tick->bid_price = 64000;
                        tick->ask_price = 65500;
                        if (!turbo_mode) {
                                printf("\n[EXCH -> WIRE] Tick #%u | SPREAD BLOWOUT! Spread = %u\n",
                                seq, tick->ask_price - tick->bid_price);
                        }
                }
                else if (seq % 250 == 0) {
                        tick->bid_price = 65250;
                        tick->ask_price = 65260;
                        tick->bid_vol   = 15;
                        tick->ask_vol   = 140;
                        if (!turbo_mode) {
                                printf("\n[EXCH -> WIRE] Tick #%u | PRICE SPIKE INJECTED! Bid = %u\n",
                                seq, tick->bid_price);
                        }
                }
                else if(seq % 100 == 0){
                        tick->bid_price = 64840;
                        tick->ask_price = 64850;
                        tick->bid_vol = 120;
                        tick->ask_vol = 15;

                        if(!turbo_mode){
                                printf("\n[EXCH -> WIRE] Tick #%u | FLASH DIP INJECTED! Ask = %u\n",seq,tick->ask_price);
                        }
                }

                if (!turbo_mode && (seq % 50 == 0)) {
                        printf("[Heartbeat] Sent %u ticks | Bid: %u | Ask: %u\r",
                        seq, tick->bid_price, tick->ask_price);
                        fflush(stdout);
                }

                tick->timestamp_ns = zt_now_ns();

                if(send(sock_fd,tx_buf,sizeof(tx_buf),0) < 0){
                        perror("FAILURE IN TRANSMITTING DATA");
                        return 0;
                }

                //well in the previous version my usleep is the culprit for the bad rtt, so used that 10ms efficiently now!
		uint64_t next_tick_ns = tick->timestamp_ns + (turbo_mode ? 20000ULL : 10000000ULL);
                while (keep_running && zt_now_ns() < next_tick_ns) {
                        ssize_t n = recv(sock_fd,rx_buff,sizeof(rx_buff),MSG_DONTWAIT);
                        if(n < (ssize_t)(ZT_ETH_HEADER_LEN + sizeof(zt_ouch_order_t))){
				__builtin_ia32_pause();
                                continue; // Keep polling! Do NOT break and go to sleep!
                        }
                        if (memcmp(rx_buff + 6, MAC_EXCH, 6) == 0) {
                                continue; // Ignore our own outgoing packet
                        }
                        zt_ouch_order_t *order = (zt_ouch_order_t*)(rx_buff + ZT_ETH_HEADER_LEN);
                        if(order->magic == ZT_OUCH_MAGIC){
                                uint64_t now_ns = zt_now_ns();
                                uint64_t rtt_ns = now_ns - order->origin_ts_ns;

                                wb->last_rtt_ns = rtt_ns;
                                wb->sum_rtt_ns += rtt_ns;

                                if(rtt_ns < wb->min_rtt_ns || wb->min_rtt_ns == 0){
                                        wb->min_rtt_ns = rtt_ns;
                                }
                                if(rtt_ns > wb->max_rtt_ns){
                                        wb->max_rtt_ns = rtt_ns;
                                }
                                const char *act = (order->action == ACTION_BUY) ? "BUY" :
                                                        (order->action == ACTION_SELL) ? "SELL":
                                                        (order->action == ACTION_KILL_SWITCH) ? "KILL_SWITCH" : "UNKNOWN";
                                if(!turbo_mode){
                                        printf("  ⚡ [WIRE -> EXCH] Order Caught! Action: %-11s | Price: %u | Qty: %u | Tick #%u | RTT: %lu ns\n",
                                               act, order->order_price, order->order_qty, order->trigger_seq_num, (unsigned long)rtt_ns);
                                }
                                if (turbo_mode) break; // In turbo mode, fire next tick immediately after catching reply
                        }
                }
        }

        printf("\nCLOSING THE ENGINE, TOTAL_TICKS SENT = %u\n",seq);
        close(sock_fd);

        return 0;
}
