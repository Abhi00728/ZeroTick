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

void load_default_strategy(zt_whiteboard_t *wb){
	uint32_t active = atomic_load(&wb->active_slot);
	zt_strategy_slot_t *slot = &wb->slots[active];

	if(slot->num_instructions > 0){
		printf("EXISTING STRATEGY FOUND\n");
		return;
	}
	printf("NO STRATEGY FOUND!!LOADING DEFAULT STRATEGY\n");
	printf("PLAN : if ask_price < 64900, then buy 10 @ ask_price\n");
	strncpy(slot->strategy_name,"default_strategy",sizeof(slot->strategy_name) -1 );

	slot->code[0] = (zt_instruction_t){OP_LOAD_FIELD,0,FIELD_ASK_PRICE,0,0};
	slot->code[1] = (zt_instruction_t){OP_LOAD_IMM,1,0,0,64900};
	slot->code[2] = (zt_instruction_t){OP_CMP_LT,2,0,1,0};
	slot->code[3] = (zt_instruction_t){OP_JMP_IF_ZERO,0,2,0,0};
	slot->code[4] = (zt_instruction_t){OP_FIRE_BUY,0,0,0,10};
	slot->code[5] = (zt_instruction_t){OP_HALT,0,0,0,0};

	slot->num_instructions = 6;
}

static inline void execute_vm_on_tick(const zt_itch_tick_t* tick,const zt_strategy_slot_t* slot,int sock_fd,uint8_t *tx_buf,
		zt_ouch_order_t* out_order, zt_whiteboard_t *wb, uint8_t *fired_action){
	int64_t R[8] = {0};
	*fired_action = ACTION_NONE;

	for(uint32_t pc = 0 ; pc < slot->num_instructions ; pc++){
		zt_instruction_t inst = slot->code[pc];
		switch(inst.opcode){
			case OP_LOAD_FIELD:
				switch(inst.src1){
					case FIELD_BID_PRICE: 
						R[inst.dst] = tick->bid_price;
						break;
					case FIELD_ASK_PRICE:
                                                R[inst.dst] = tick->ask_price;
                                                break;
					case FIELD_BID_VOL:
                                                R[inst.dst] = tick->bid_vol;;
                                                break;
					case FIELD_ASK_VOL:
                                                R[inst.dst] = tick->ask_vol;
                                                break;
				}
				break;
			
			case OP_LOAD_IMM:
				R[inst.dst] = inst.imm_val;
				break;

			case OP_ADD:
				R[inst.dst] = R[inst.src1] + R[inst.src2];
				break;
			case OP_SUB:
				R[inst.dst] = R[inst.src1] - R[inst.src2];
				break;
			case OP_CMP_GT:
				R[inst.dst] = (R[inst.src1] > R[inst.src2]) ? 1 : 0;
				break;
			case OP_CMP_LT:
				R[inst.dst] = (R[inst.src1] < R[inst.src2]) ? 1 : 0;
				break;
			case OP_JMP_IF_ZERO:
				if(R[inst.src1] == 0){
					return;
				}
				break;

                               
                        case OP_FIRE_BUY:
            		case OP_FIRE_SELL: {
                		uint8_t act = (inst.opcode == OP_FIRE_BUY) ? ACTION_BUY : ACTION_SELL;
                		out_order->trigger_seq_num = tick->seq_num;
                		out_order->origin_ts_ns    = tick->timestamp_ns;
                		out_order->order_price     = (uint32_t)R[inst.src1];
                		out_order->order_qty       = (uint16_t)inst.imm_val;
                		out_order->action          = act;

                		// Blast 38-byte raw Ethernet + ZT-OUCH frame straight back out veth-zt
                		send(sock_fd, tx_buf, ZT_ETH_HEADER_LEN + sizeof(zt_ouch_order_t), 0);

                		atomic_fetch_add(&wb->orders_fired, 1);
                		*fired_action = act;
                		return;
            		}

            		case OP_KILL_SWITCH:
                		out_order->trigger_seq_num = tick->seq_num;
                		out_order->origin_ts_ns    = tick->timestamp_ns;
                		out_order->order_price     = 0;
                		out_order->order_qty       = 0;
                		out_order->action          = ACTION_KILL_SWITCH;

                		send(sock_fd, tx_buf, ZT_ETH_HEADER_LEN + sizeof(zt_ouch_order_t), 0);

                		atomic_fetch_add(&wb->kill_switches_tripped, 1);
                		*fired_action = ACTION_KILL_SWITCH;
                		return;
			
			case OP_HALT:
				return;
		}
	}
}

int main(){
    	struct sigaction sa = {0};
    	sa.sa_handler = handle_sigint;
    	sigemptyset(&sa.sa_mask);
    	sigaction(SIGINT, &sa, NULL); // No SA_RESTART -> recv() wakes up immediately on Ctrl+C!
	zt_whiteboard_t *wb = zt_open_whiteboard(true);
	if(!wb){
		perror("OPENING WHITEBOARD FAILED!");
		return 0;
	}

	
	load_default_strategy(wb);
	
	//Create socket first!
	
	int sock_fd = socket(AF_PACKET,SOCK_RAW,htons(ETH_P_ALL));
	if(sock_fd < 0){
		perror("Socket creation, run with sudo!");
		return 0;
	}
	int ifidx = if_nametoindex(IFACE_ZT);
	if(ifidx == 0){
		perror("veth_zt not found!, run the setup_veth.sh first!");
		return 0;
	}

	//now bind that socket to veth_zt
	struct sockaddr_ll bind_addr = {0};
	bind_addr.sll_family = AF_PACKET;
	bind_addr.sll_protocol = htons(ETH_P_ALL);
	bind_addr.sll_ifindex = ifidx;
	if(bind(sock_fd,(struct sockaddr*)&bind_addr,sizeof(bind_addr)) < 0){
		perror("bind to veth_zt failed!");
		close(sock_fd);
		return 0;
	}

	//pre_build the outgoing order(HEADER+OUCH_PACKET)
	uint8_t tx_buf[ZT_ETH_HEADER_LEN + sizeof(zt_ouch_order_t)] = {0};
	memcpy(tx_buf + 0,MAC_EXCH,6);
	memcpy(tx_buf + 6,MAC_ZT,6);
	uint16_t zt_ether = htons(ZT_ETHERTYPE);
	memcpy(tx_buf + 12,&zt_ether,2);

	zt_ouch_order_t *order = (zt_ouch_order_t*)(tx_buf + ZT_ETH_HEADER_LEN);
	order->magic = ZT_OUCH_MAGIC;
	printf("================================================================\n");
    	printf("  ZEROTICK PACKET REFLEX ENGINE LIVE (Interface: %s)\n", IFACE_ZT);
    	printf("  Listening for raw ZT-ITCH frames at Byte Offset 14...\n");
    	printf("================================================================\n");	
	uint8_t rx_buf[128];

	while(keep_running){
		ssize_t n = recv(sock_fd,rx_buf,sizeof(rx_buf),0);
		if(n < (ssize_t)(ZT_ETH_HEADER_LEN + sizeof(zt_itch_tick_t))){
			continue;
		}
		//wrote this because i may have a chance of receiving my own packet right?(i'm still not sure though)!
		if (memcmp(rx_buf + 6, MAC_ZT, 6) == 0) {
            		continue;
        	}
		const zt_itch_tick_t *tick = (const zt_itch_tick_t*)(rx_buf + ZT_ETH_HEADER_LEN);
		if(tick->magic != ZT_ITCH_MAGIC){
			continue;
		}
		uint64_t time = zt_now_ns();
		uint32_t active_idx = atomic_load_explicit(&wb->active_slot,memory_order_acquire);
		const zt_strategy_slot_t *active_strategy = &wb->slots[active_idx];

		uint8_t fired_action = ACTION_NONE;
		execute_vm_on_tick(tick,active_strategy,sock_fd,tx_buf,order,wb,&fired_action);
		
		uint64_t exec_ns = zt_now_ns() - time;
		uint64_t total_ticks = atomic_fetch_add(&wb->ticks_processed,1) + 1;

		uint32_t log_idx = atomic_fetch_add(&wb->log_head, 1) % LOG_RING_SIZE;

		wb->logs[log_idx] = (zt_log_entry_t){
			.seq_num = tick->seq_num,
			.bid_price = tick->bid_price,
			.ask_price = tick->ask_price,
			.action_fired = fired_action,
			.order_price = order->order_price,
			.order_qty = order->order_qty,
			.latency_ns = exec_ns
		};
		if (fired_action != ACTION_NONE) {
            		const char *act_name = (fired_action == ACTION_BUY)  ? "FIRE_BUY" :
                                   (fired_action == ACTION_SELL) ? "FIRE_SELL" : "KILL_SWITCH";
            		printf("⚡ [ZT_ENGINE] Tick #%u | Ask: %u | Triggered %-11s | VM Exec Time: %lu ns\n",
                   	tick->seq_num, tick->ask_price, act_name, (unsigned long)exec_ns);
        	} 
		else if (total_ticks % 50 == 0) {
            		printf("[Intercepted] %lu ticks | Active Strategy: '%s' (Slot %u)\r",
                   		(unsigned long)total_ticks, active_strategy->strategy_name, active_idx);
            		fflush(stdout);
        	}
	}

	printf("SHUTTING DOWN ZERO TICK!.\n");
	close(sock_fd);
	return 0;
}

