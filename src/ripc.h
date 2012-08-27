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
#ifndef RIPC_H_
#define RIPC_H_

#include <stdint.h>
#include <sys/types.h>
#include <common.h>

#define NUM_RECV_BUFFERS 10

#ifdef __cplusplus
extern "C" {
#endif

uint8_t init(void);

uint16_t ripc_register_random_service_id(void);
uint8_t ripc_register_service_id(int);

void *ripc_buf_alloc(size_t size);
void ripc_buf_free(void *buf);
uint8_t ripc_reg_recv_window(void *base, size_t size);
uint8_t ripc_buf_register(void *buf, size_t size);

uint8_t ripc_send_short(
		uint16_t src,
		uint16_t dest,
		void **buf,
		uint32_t *length,
		uint16_t num_items,
		void **return_bufs,
		uint32_t *return_buf_lengths,
		uint16_t num_return_bufs
		);

uint8_t ripc_send_long(
		uint16_t src,
		uint16_t dest,
		void **buf,
		uint32_t *length,
		uint16_t num_items,
		void **return_bufs,
		uint32_t *return_buf_lengths,
		uint16_t num_return_bufs
		);

uint8_t ripc_receive(
		uint16_t service_id,
		uint16_t *from_service_id,
		void ***short_items,
		uint32_t **short_item_sizes,
		uint16_t *num_short_items,
		void ***long_items,
		uint32_t **long_item_sizes,
		uint16_t *num_long_items
		);

#ifdef __cplusplus
} //extern "C"
#endif

extern struct library_context context;

#endif /* RIPC_H_ */
