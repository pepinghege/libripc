#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/ripc.h"
#include "config.h"
#include "common.h"

int main(void) {
	ripc_register_service_id(4);
	void **short_items = NULL, **long_items = NULL;
	int length = sizeof(int);
	int num_items;
	while(true) {
		DEBUG("Waiting for message");
		num_items = ripc_receive(4, &short_items,&long_items);
		DEBUG("Received message: %d\n", *(int *)short_items[0]);
		ripc_send_short(4, 1, short_items, &length, 1);
		ripc_buf_free(short_items[0]); //returns receive buffer to pool
		free(short_items); //frees the array itself
	}
	return EXIT_SUCCESS;
}
