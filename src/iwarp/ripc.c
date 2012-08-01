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
#include <inttypes.h>
#include <rdma/rdma_cma.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "ripc.h"
#include "common.h"
#include "memory.h"
#include "resolver.h"
#include "resources.h"

#ifndef SUBNETARCH_PREFIX
#define SUBNETARCH_PREFIX	""
#define	SUBNETARCH_PREFIX_LEN	0
#endif

uint8_t strip_subnetarch_prefix(const char *src, char *dest) {
	size_t i = SUBNETARCH_PREFIX_LEN;
	size_t j = 0;

#ifdef 	HAVE_DEBUG
	if (strncmp(src, SUBNETARCH_PREFIX, SUBNETARCH_PREFIX_LEN))
		return false;
#endif

	do {
		dest[j++] = src[i];
	} while (src[i++] != '\0');

	return true;
}

struct library_context context;

pthread_mutex_t services_mutex, remotes_mutex;

void netarch_init(void) {
        DEBUG("netarch_init");

	#define	SIOCGIFADDR	0x8915
	#define SIOCGIFBRDADDR	0x8919

	struct ibv_device_attr device_attr;
	struct ibv_port_attr port_attr;
	struct ibv_context **context_list;
	int num_contexts	= 0;
	size_t i;
	uint8_t j;
	bool bound;

	context.na.echannel	= rdma_create_event_channel();
	if (!context.na.echannel)
		panic("Could not create event channel");

	if (rdma_create_id(context.na.echannel, &context.na.listen_cm_id, NULL, RDMA_PS_TCP) < 0) 
		panic("Could not create cm-id");

	context_list		= rdma_get_devices(&num_contexts);

	bound			= false;
	for (i = 0; !bound && i < num_contexts; i++) {
		struct ibv_device *dev		= (*(context_list + i))->device;
		char *name			= (char*) malloc(sizeof(char) * 16);
		int sock;
		struct sockaddr_in *dev_addr;
		struct interface dev_if;

		strip_subnetarch_prefix(ibv_get_device_name(dev), name);
		DEBUG("Found device: %s (prefix-stripped: %s)", ibv_get_device_name(dev), name);
		
		//Retrieve IP-address of the device
		sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		memset(&dev_if, 0, sizeof(dev_if));
		strcpy(dev_if.name, name);
		ioctl(sock, SIOCGIFADDR, &dev_if);
		dev_addr			= (struct sockaddr_in*) &dev_if.addr;
		//Make sure that relevant fields are set correctly
		dev_addr->sin_family		= AF_INET;
		dev_addr->sin_port		= 0;
		/*
		 * We currently listen on one ip-address only, although in general, listening on all available
		 * ip-addresses is possible. Two different schemes could be realized:
		 * 1)   We handle multiple ip-addresses, i.e. connections with different remotes would be established
		 *      over different ip-addresses.
		 * 2)   We start listen on all ip-addresses, but after the first establisehd connection, only the
		 *      respective ip-address can be used by all other services.
		 */
		if (rdma_bind_addr(context.na.listen_cm_id, (struct sockaddr*) dev_addr) < 0) {
			DEBUG("Could not bind on device %s", ibv_get_device_name(dev));
			free(name);
			continue;
		} else {
			bound			= true;
		}

		context.na.conn_listen_port	= ntohs(rdma_get_src_port(context.na.listen_cm_id));
		context.na.ip_addr		= ((struct sockaddr_in*) rdma_get_local_addr(context.na.listen_cm_id))->sin_addr.s_addr;

		//Retrieve broadcast-ip-address of the device
		memset(&dev_if, 0, sizeof(dev_if));
		strcpy(dev_if.name, name);
		ioctl(sock, SIOCGIFBRDADDR, &dev_if);
		dev_addr			= (struct sockaddr_in*) &dev_if.addr;
		context.na.bcast_ip_addr	= dev_addr->sin_addr.s_addr;

		free(name);

		break;
	}

	if (!num_contexts)
		panic("Did not find any devices.");
	else if (i == num_contexts)
		panic("Could not bind to any of the found devices.");

	context.na.pd		= ibv_alloc_pd(context.na.listen_cm_id->verbs);
	if (!context.na.pd)
		panic("Could not allocate protection domain!");
	else
		DEBUG("Allocated protection domain: %u", context.na.pd->handle);

	ibv_query_device(context.na.listen_cm_id->verbs, &device_attr);

	for (j = 0; j < device_attr.phys_port_cnt; j++) {
		ibv_query_port(context.na.listen_cm_id->verbs, j, &port_attr);

		if (port_attr.state == IBV_PORT_ACTIVE) {
			DEBUG("Port %d is active, we shall use it", j);
			context.na.lid		= port_attr.lid;
			context.na.port_num	= j;
			break;
		}
	}

	context.initialized	= true;
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

	DEBUG("Starting long send: %u -> %u (%u items)", src, dest, num_items);

	return 0;
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


	return 0;
}
