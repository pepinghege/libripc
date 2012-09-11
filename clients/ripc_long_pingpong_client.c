/*  Copyright 2011, 2012 Jens Kehne
 *  Copyright 2012 Jan Stoess, Karlsruhe Institute of Technology
 *
 *  LibRIPC is free software: you can redistribute it and/or modify it under
 *  the terms of the GNU Lesser General Public License as published by the
 *  Free Software Foundation, either version 2.1 of the License, or (at your
 *  option) any later version.
 *
 *  LibRIPC is distributed in the hope that it will be useful, but WITHOUT
 *  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 *  FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 *  License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public License
 *  along with libRIPC.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/ripc.h"
#include "config.h"
#include "common.h"

int main(int argc, char *argv[]) {
#ifndef CLIENT_SERVICE_ID
	uint16_t my_service_id = ripc_register_random_service_id();
#else
	uint16_t my_service_id = CLIENT_SERVICE_ID;
	ripc_register_service_id(my_service_id);
#endif
	sleep(1);
	int i, j, len, recvd = 0;

	//for benchmarking
	struct timeval before, after;
	uint64_t before_usec, after_usec, diff;

	//for sending
	void *msg_array[WORDS_PER_PACKET];
	void *return_buf_array[CLIENT_RETURN_BUFFERS];
	uint32_t length_array[WORDS_PER_PACKET];
	uint32_t return_buf_length_array[CLIENT_RETURN_BUFFERS];
	uint32_t packet_size = argc > 1 ? atoi(argv[1]) : PACKET_SIZE;

	//for receiving
	void **short_items = NULL, **long_items = NULL;
	uint16_t from, num_short, num_long;
	uint32_t *short_sizes, *long_sizes;

	gettimeofday(&before, NULL);

	msg_array[0] = ripc_buf_alloc(packet_size * WORDS_PER_PACKET);
	memset(msg_array[0], 0, packet_size * WORDS_PER_PACKET);
	length_array[0] = packet_size;
	for (i = 1; i < WORDS_PER_PACKET; ++i) {
		length_array[i] = packet_size;
		msg_array[i] = msg_array[0] + i * packet_size;
	}

	strcpy(msg_array[0], "Hello long world!");

	return_buf_array[0] = ripc_buf_alloc(packet_size * CLIENT_RETURN_BUFFERS);
	if (return_buf_array[0]) {
		memset(return_buf_array[0], 0, packet_size * CLIENT_RETURN_BUFFERS);
		return_buf_length_array[0] = packet_size;
		for (i = 1; i < CLIENT_RETURN_BUFFERS; ++i) {
			return_buf_length_array[i] = packet_size;
			return_buf_array[i] = return_buf_array[0] + i * packet_size;
		}
	}

	printf("Starting loop\n");

//	for (i = 0; i < NUM_ROUNDS; ++i) {
	for (i = 0; i < 5; ++i) {
		if (ripc_send_long(
				my_service_id,
				SERVER_SERVICE_ID,
				msg_array,
				length_array,
				WORDS_PER_PACKET,
				return_buf_array,
				return_buf_length_array,
				CLIENT_RETURN_BUFFERS))
			continue;

		for (j = 0; j < WORDS_PER_PACKET; ++j)
			ripc_reg_recv_window(msg_array[j], PACKET_SIZE);

		ripc_receive(
				my_service_id,
				&from,
				&short_items,
				&short_sizes,
				&num_short,
				&long_items,
				&long_sizes,
				&num_long);

		//printf("Received item\n");
		DEBUG("Message reads: %s\n", (char *)long_items[0]);
		recvd++;
		free(long_items);
		//sleep(1);
	}

	gettimeofday(&after, NULL);
	before_usec = before.tv_sec * 1000000 + before.tv_usec;
	after_usec = after.tv_sec * 1000000 + after.tv_usec;
	diff = after_usec - before_usec;

	sleep(5);

	printf("Exchanged %d items in %lu usec (rtt %f usec)\n", recvd, diff, (double)diff / NUM_ROUNDS);
	//printf("%f\n",(double)diff / NUM_ROUNDS);
	return EXIT_SUCCESS;
}
