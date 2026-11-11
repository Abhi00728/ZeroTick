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
	signal(SIGINT,handle_sigint);

	bool turbo_mode = (argc > 1 && strcmp(argv[1],"--turbo") == 0);

	//open the shared whiteboard!!//
	zt_whiteboard_t* wb = zt_open_whiteboard(true);
	if(!wb){
		perror("FAILED TO OPEN SHARED WHITEBOARD");
		return 0;
	}

	//OPEN A RAW SOCKET AND BIND IT TO OUR VETH-EXCH!!
	int sock_fd = open(AF_PACKET,SOCK_RAW,htons(ETH_P_ALL));
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
	bind_addr.sll_protocol = htons(ETH_P_ALL);
	bind_addr.sll_ifindex = if_idx;

	if(bind(sock_fd,(struct sockaddr *)&bind_addr,sizeof(bind_addr)) < 0){
		perror("binding to the veth-exch failed!");
		close(sock_fd);
		return 0;
	}

	//NOW'S THE MAIN PART, PREPARING THE 46-BYTES PACKET THAT GOES THROUGH THIS SOCKET!!
	
	uint8_t tx_buf[ZT_ETH_HEADER_LEN + sizeof(zt_itch_tick_t)] = {0};
	memcpy(tx_buf+0,MAC_ZT,6);
	memcpy(tx_buf+6,MAX_EXCH,6);
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
	uint8_t rx_buff = 128;

	//NOW AN INFINITE LOOP TO CATCH THE ORDERS,CONTINUOUS FLOW OF DATA
	while(keep_running){
		seq++;
		tick->seq_num = seq;
		uint32_t drift = (seq % 7 ) * 3;
		tick->bid_price = 64990 + drift;
		tick->ask_price = 65000 + drift;
		tick->bid_vol = 25;
		tick->ask_vol = 20;

		if(
























