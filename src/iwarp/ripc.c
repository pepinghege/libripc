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
#include <time.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include "ripc.h"
#include "common.h"
#include "memory.h"
#include "resolver.h"
#include "resources.h"

struct library_context context;

pthread_mutex_t services_mutex, remotes_mutex;

uint8_t netarch_init(void) {
        DEBUG("netarch_init");
	context.na.device_context = NULL;
	struct ibv_context *device_context = NULL;
	struct ibv_context **list = NULL;
	int num_devices = 0;

	list = rdma_get_devices(&num_devices);
	if (!list) {
		panic("Failed to get device list: %s", strerror(errno));
	}

	device = *list;
	if (!device_context) {
		panic("No devices available!");
	}

	DEBUG("Taking 1st of %d devices.", num_devices);
	context.na.device_context = device_context;
	context.na.pd = siw_alloc_od(device_context);
	if (!context.na.pd) {
		panic("Failed to allocate protection domain!");
	} else {
		DEBUG("Allocated protection domain: %u", context.na.pd->handle);
	}

	struct ibv_device_attr device_attr;
	siw_query_device(device_context, &device_attr);
	DEBUG("Chosen device has %d physical ports", device_attr.phys_port_cnt);
        DEBUG("Maximum mr size: %lu", device_attr.max_mr_size);
        DEBUG("Maximum mr count: %u", device_attr.max_mr);
        DEBUG("Maximum number of outstanding wrs: %u", device_attr.max_qp_wr);
        DEBUG("Maximum number of outstanding cqes: %u", device_attr.max_cqe);
        DEBUG("Maximum number of sges per wr: %u", device_attr.max_sge);
        DEBUG("Local CA ACK delay: %u", device_attr.local_ca_ack_delay);
        DEBUG("Page size caps: %lx", device_attr.page_size_cap);

	uint32_t i;
	int j;
	struct ibv_port_attr port_attr;
	union ibv_gid gid;
	bool found_active_port = 0;

	for (i = 1; i <= device_attr.phys_port_cnt; ++i) {
		siw_query_port(device_context, i, &port_attr);
                DEBUG("Port %d: Found LID %u", i, port_attr.lid);
                DEBUG("Port %d has %d GIDs", i, port_attr.gid_tbl_len);
                DEBUG("Port %d's maximum message size is %u", i, port_attr.max_msg_sz);
                DEBUG("Port %d's status is %s", i, ibv_port_state_str(port_attr.state));

		if (!found_active_port && port_attr.state == IBV_PORT_ACTIVE) {
			DEBUG("Port %d is active, we shall use it", i);
			context.na.lid		= port_attr.lid;
			context.na.port_num	= i;
			found_active_port = 1;
		}
	}

	rdma_service_id.na.no_cchannel	= true;
	rdma_service_id.number		= 0xffff;
	//queues for sending connection requests
	alloc_queue_state(&rdma_service_id);

	context.initialized = true;

	rdma_free_devices(list);

        return true;
}


uint8_t
ripc_send_short(
		uint16_t src,
		uint16_t dest,
		void **buf,
		size_t *length,
		uint32_t num_items,
		void **return_bufs,
		size_t *return_buf_lengths,
		uint32_t num_return_bufs) {

	DEBUG("Starting short send: %u -> %u (%u items)", src, dest, num_items);
	return 0;
}

uint8_t
ripc_send_long(
		uint16_t src,
		uint16_t dest,
		void **buf,
		size_t *length,
		uint32_t num_items,
		void **return_bufs,
		size_t *return_buf_lengths,
		uint32_t num_return_bufs) {

	DEBUG("Long send: %u -> %u (%u items)", src, dest, num_items);
	return 1;
}

uint8_t
ripc_receive(
		uint16_t service_id,
		uint16_t *from_service_id,
		void ***short_items,
		uint32_t **short_item_sizes,
		uint16_t *num_short_items,
		void ***long_items,
		uint32_t **long_item_sizes,
		uint16_t *num_long_items) {

	return 1;
}
