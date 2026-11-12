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
}


int main(){
	signal(SIGINT,handle_sigint);
