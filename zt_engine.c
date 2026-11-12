#include "zt_common.h"
#include <sys/socket.h>
#include <linux/if_packet.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <signal.h>

static volatile keep_running = 1;

void handle_sigint(int sig){
	(void)sig;
	keep_running = 0;
}

void load_default_strategy(zt_whiteboard_t *wb){
	uint32_t active = atomic_load(wb->active_slot);
	zt_strategy_slot_t *slot = &wb->slots[active];

	if(slot->num_instructions > 0){
		printf("EXISTING STRATEGY FOUND\n");
		return;
	}
	printf("NO STRATEGY FOUND!!LOADING DEFAULT STRATEGY\n");
	printf("PLAN : if ask_price < 64900, then buy 10 @ ask_price\n");
	strncpy(slot->strategy_name,"default_strategy",sizeof(slot->strategy_name) -1 );

	slot->code[0] = (zt_instruction_t){OP_LOAD_FIELD,0,FIELD_ASK_PRICE,0,0}
	slot->code[1] = (zt_instruction_t){OP_LOAD_IMM,1,0,0,64900};
	slot->code[2] = (zt_instruction_t){OP_CMP_LT,2,0,1,0};
	slot->code[3] = (zt_instruction_t){OP_JMP_IF_ZERO,0,2,0,0}
	slot->code[4] = (zt_instruction_t){OP_FIRE_BUY,0,0,0,10};
	slot->code[5] = (zt_instruction_t){OP_HALT,0,0,0,0};

	slot->num_instructions = 6;
}

static inline void execute_vm_on_tick(const zt_itch_tick_t* tick,const zt_strategy_slot_t* slot,int sock_fd,uint8_t *tx_buf,
		zt_ouch_order_t* out_order, zt_whiteboard_t *wb, uint8_t *fired_action){
	int64_t R[8] = {0};
	*fired_action = ACTION_NONE;

	for(uint32_t pc = 0 ; pc < slot->num_instructions ; pc++){
		zt_instruction_t inst = slot[pc];
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
				R[inst.dst] = (R[src1] > R[src2]) ? 1 : 0;
				break;
			case OP_CMP_LT:
				R[inst.dst] = (R[src1] < R[src2]) ? 1 : 0;
				break;
			case OP_JMP_IF_ZERO:
				if(R[src1] == 0){
					return;
				}
				break;

			case OP_FIRE_BUY:
				uint8_t act = "ACTION_BUY";
				out_order->magic = ZT_OUCH_MAGIC;
				out_order->trigger_seq_num = tick->seq_num;
				out_order->origin_ts_ns = tick->timestamp_ns;
				out_order->order_price = (uint32_t)R[inst.src1];
				out_order->order_qty = (uint16_t)inst.imm_val;
				out_order->action = act;
//Here if i just send the out_order thing, then the header would be missing out!!, so instead send tx_buff, which i'll later modify 
				send(sock_fd,tx_buff,ZT_ETH_HEADER_LEN + sizeof(zt_ouch_order_t),0);
				atomic_fetch_add(&wb->orders_fired,1);
				fired_action = act;


}


int main(){
	signal(SIGINT,handle_sigint);
