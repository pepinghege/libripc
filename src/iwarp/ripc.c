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

	context.na.echannel	= rdma_create_event_channel();
	if (!context.na.echannel)
		panic("Could not create event channel");

	if (rdma_create_id(context.na.echannel, &context.na.listen_cm_id, NULL, RDMA_PS_TCP) < 0) 
		panic("Could not create cm-id");

	context_list		= rdma_get_devices(&num_contexts);

	for (i = 0; i < num_contexts; i++) {
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

	uint32_t i;
	uint32_t total_data_length	= 0;
	uint32_t total_msg_length	= 0;
        mem_buf_t mem_buf;
        
	pthread_mutex_lock(&remotes_mutex);
	if (!context.remotes[dest]
		|| (context.remotes[dest]->state != RIPC_RDMA_ESTABLISHED)) {
		pthread_mutex_unlock(&remotes_mutex);
		create_rdma_connection(src, dest);
		pthread_mutex_lock(&remotes_mutex);
	}
	assert(context.remotes[dest] 
		&& (context.remotes[dest]->state == RIPC_RDMA_ESTABLISHED));
	pthread_mutex_unlock(&remotes_mutex);

	for (i = 0; i < num_items; ++i)
		total_data_length += length[i];

	total_msg_length		= total_data_length
					+ sizeof(struct msg_header)
					+ sizeof(struct short_header) * num_items
					+ sizeof(struct long_desc) * num_return_bufs;

	DEBUG("Total data length: %u", total_data_length);
	DEBUG("Total message length: %u", total_msg_length);
#if 0
	if (total_msg_length > RECV_BUF_SIZE) {
		ERROR("Packet too long! Size: %u, maximum: %u",
				total_data_length,
				RECV_BUF_SIZE
				- sizeof(struct msg_header)
				- sizeof(struct short_header) * num_items
				- sizeof(struct long_desc) * num_return_bufs
				);
		return -1; //probably won't fit at receiving end either
	}
#endif

	//build packet header
	struct ibv_mr *header_mr =
			ripc_alloc_recv_buf(total_msg_length - total_data_length).na;
	struct msg_header *hdr = (struct msg_header *)header_mr->addr;
	struct short_header *msg = (struct short_header *)(
			(uint64_t)hdr
			+ sizeof(struct msg_header));
	struct long_desc *return_bufs_msg = (struct long_desc *)(
			(uint64_t)msg
			+ sizeof(struct short_header) * num_items);


	hdr->type = RIPC_MSG_SEND;
	hdr->from = src;
	hdr->to = dest;
	hdr->short_words = num_items;
	hdr->long_words = 0;
	hdr->new_return_bufs = num_return_bufs;

	struct ibv_sge sge[num_items + 1]; //+1 for header
	sge[0].addr = (uint64_t)header_mr->addr;
	sge[0].length = sizeof(struct msg_header)
							+ sizeof(struct short_header) * num_items
							+ sizeof(struct long_desc) * num_return_bufs;
	sge[0].lkey = header_mr->lkey;

	uint32_t offset =
			40 //skip GRH
			+ sizeof(struct msg_header)
			+ sizeof(struct short_header) * num_items
			+ sizeof(struct long_desc) * num_return_bufs;

	for (i = 0; i < num_items; ++i) {

		DEBUG("First message: offset %#x, length %u", offset, length[i]);
		msg[i].offset = offset;
		msg[i].size = length[i];

		offset += length[i]; //offset of next message item

		//make sure send buffers are registered with the hardware
                mem_buf_t mem_buf = used_buf_list_get(buf[i]);
		void *tmp_buf;

		if (mem_buf.size == -1) { //not registered yet
			DEBUG("mr not found in cache, creating new one");
			mem_buf = ripc_alloc_recv_buf(length[i]);
			tmp_buf = mem_buf.na->addr;
			memcpy(tmp_buf,buf[i],length[i]);
		} else {
			DEBUG("Found mr in cache!");
                        used_buf_list_add(mem_buf);
			tmp_buf = buf[i];
		}

		assert(mem_buf.na);
		//assert(mr->length >= length[i]); //the hardware won't allow it anyway

		sge[i + 1].addr = (uint64_t)tmp_buf;
		sge[i + 1].length = length[i];
		sge[i + 1].lkey = mem_buf.na->lkey;
	}

	//process new return buffers
	for (i = 0; i < num_return_bufs; ++i) {
		if (return_buf_lengths[i] == 0)
			continue;
		DEBUG("Found return buffer: address %p, size %zu",
				return_bufs[i],
				return_buf_lengths[i]);

		if (return_bufs[i] == NULL) { //user wants us to allocate a buffer
			DEBUG("User requested return buffer allocation");
			mem_buf = ripc_alloc_recv_buf(return_buf_lengths[i]);

		} else {
			mem_buf = used_buf_list_get(return_bufs[i]);                       

			if (mem_buf.size == -1) { //not registered, try to register now
				DEBUG("Return buffer not registered, attempting registration");
				ripc_buf_register(return_bufs[i], return_buf_lengths[i]);
				mem_buf = used_buf_list_get(return_bufs[i]);
				if (!mem_buf.size == -1) {
                                        DEBUG("Registration failed, drop buffer");
					continue;
                                } else {
                                        DEBUG("Registration successful! rkey is %#x", mem_buf.na->rkey);
                                        used_buf_list_add(mem_buf);
                                }

			} else { //mr was registered
				DEBUG("Found mr at address %lx, size %zu, rkey %#x",
                                      mem_buf.addr, mem_buf.size, mem_buf.na->rkey);
				//need to re-add the buffer even if it's too small
				used_buf_list_add(mem_buf);

				//check if the registered buffer is big enough to hold the return buffer
				if ((uint64_t)return_bufs[i] + return_buf_lengths[i] > 
                                    mem_buf.addr + mem_buf.size) {
					DEBUG("Buffer is too small, skipping");
					continue; //if it's too small, discard it
				}
			}
		}

		/*
		 * At this point, we should have an mr, and we should know the buffer
		 * represented by the mr is big enough for our return buffer.
		 */
		assert(mem_buf.na);
		assert ((uint64_t)return_bufs[i] + return_buf_lengths[i] <= mem_buf.addr + mem_buf.size);

		return_bufs_msg[i].addr =
                        return_bufs[i] ? (uint64_t)return_bufs[i] : mem_buf.addr;
		return_bufs_msg[i].length = return_buf_lengths[i];
		return_bufs_msg[i].rkey = mem_buf.na->rkey;
	}

	struct ibv_send_wr wr;
	wr.next = NULL;
	wr.opcode = IBV_WR_SEND;
	wr.num_sge = num_items + 1;
	wr.sg_list = sge;
	wr.wr_id = 0xdeadbeef; //TODO: Make this a counter?
	wr.wr.ud.remote_qkey = (uint32_t)dest;
	wr.send_flags = IBV_SEND_SIGNALED;

	DEBUG("Sending message containing %u items to service %u", wr.num_sge, dest);
#ifdef HAVE_DEBUG
	for (i = 0; i < wr.num_sge; ++i) {
		ERROR("Item %u: address: %lx, length %u",
			i,
			wr.sg_list[i].addr,
			wr.sg_list[i].length);
	}
#endif

	struct ibv_send_wr *bad_wr = NULL;

	pthread_mutex_lock(&remotes_mutex);
	struct ibv_qp *dest_qp = context.remotes[dest]->na.rdma_qp;
	struct ibv_cq *dest_cq = context.remotes[dest]->na.rdma_send_cq;
	pthread_mutex_unlock(&remotes_mutex);

	int ret = ibv_post_send(dest_qp, &wr, &bad_wr);

	if (bad_wr) {
		ERROR("Failed to post send: ", strerror(ret));
		return ret;
	} else {
		DEBUG("Successfully posted send!");
	}


//#ifdef HAVE_DEBUG
	struct ibv_wc wc;
	while (!(ibv_poll_cq(dest_cq, 1, &wc))); //polling is probably faster here
	//ibv_ack_cq_events(context.services[src]->recv_cq, 1);
	DEBUG("received completion message!");
	if (wc.status) {
		ERROR("Send result: %s", ibv_wc_status_str(wc.status));
		ERROR("QP state: %d", dest_qp->state);
	}
	DEBUG("Result: %s", ibv_wc_status_str(wc.status));
//#endif

	ripc_buf_free(hdr);

	//return wc.status;
	return ret;
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

	return 1;
}
