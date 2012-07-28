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

	uint32_t i;
	uint32_t data_length = 0;
	uint32_t total_length;
        mem_buf_t mem_buf;
	struct sockaddr_in addr;
        
	for (i = 0; i < num_items; ++i)
		data_length += length[i];

	total_length = data_length
			+ sizeof(struct msg_header)
			+ sizeof(struct short_header) * num_items
			+ sizeof(struct long_desc) * num_return_bufs;

	DEBUG("Total length: %u", data_length);
	if (data_length > (
			RECV_BUF_SIZE
			- sizeof(struct msg_header)
			- sizeof(struct short_header) * num_items
			- sizeof(struct long_desc) * num_return_bufs
			)) {
		ERROR("Packet too long! Size: %"PRIx32", maximum: %"PRIx64,
				data_length,
				RECV_BUF_SIZE
				- sizeof(struct msg_header)
				- sizeof(struct short_header) * num_items
				- sizeof(struct long_desc) * num_return_bufs
				);
		return -1;
	}

	//build packet header
/*
	struct ibv_mr *header_mr =
			ripc_alloc_recv_buf(
                                sizeof(struct msg_header)
                                + sizeof(struct short_header) * num_items
                                + sizeof(struct long_desc) * num_return_bufs
                                ).na;
	struct msg_header *hdr = (struct msg_header *)header_mr->addr;
	struct short_header *msg = (struct short_header *)(
			(uint64_t)hdr
			+ sizeof(struct msg_header));
	struct long_desc *return_bufs_msg = (struct long_desc *)(
			(uint64_t)hdr
			+ sizeof(struct msg_header)
			+ sizeof(struct short_header) * num_items);
*/
	void *msg_buffer 		= malloc(	data_length
							+ sizeof(struct msg_header)
							+ sizeof(struct short_header) * num_items
							+ sizeof(struct long_desc) * num_return_bufs);
	struct msg_header *hdr		= (struct msg_header*) msg_buffer;
	struct short_header *msg	= (struct short_header*) (hdr + sizeof(struct msg_header));
	struct long_desc *buf_msg	= (struct long_desc*) (msg + (sizeof(struct msg_header) * num_items));

	hdr->type = RIPC_MSG_SEND;
	hdr->from = src;
	hdr->to = dest;
	hdr->short_words = num_items;
	hdr->long_words = 0;
	hdr->new_return_bufs = num_return_bufs;

/*
	struct ibv_sge sge[num_items + 1]; //+1 for header
	sge[0].addr = (uint64_t)header_mr->addr;
	sge[0].length = sizeof(struct msg_header)
							+ sizeof(struct short_header) * num_items
							+ sizeof(struct long_desc) * num_return_bufs;
	sge[0].lkey = header_mr->lkey;

	uint32_t offset =
			+ sizeof(struct msg_header)
			+ sizeof(struct short_header) * num_items
			+ sizeof(struct long_desc) * num_return_bufs;
*/
	uint64_t offset = buf_msg + (sizeof(struct long_desc) * num_return_bufs);

	for (i = 0; i < num_items; ++i) {

		DEBUG("First message: offset %#x, length %u", offset, length[i]);
		msg[i].offset = offset;
		msg[i].size = length[i];

		memcpy(msg_buffer + offset, buf[i], length[i]);

		offset += length[i]; //offset of next message item
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

	if (!context.remotes[dest])
		resolve(src, dest);
	assert(context.remotes[dest]);


	memset(&addr, 0, sizeof(addr));
	addr.sin_family		= AF_INET;
	pthread_mutex_lock(&remotes_mutex);
	addr.sin_addr.s_addr	= context.remotes[dest]->ip_addr;
	addr.sin_port		= htons(context.remotes[dest]->msg_port);
	pthread_mutex_unlock(&remotes_mutex);

	pthread_mutex_lock(&(context.na.socket_mutex));
	if (sendto(context.na.msg_socket, msg_buffer, total_length, 0, (struct sockaddr*) &addr, sizeof(addr)) < 0) {
		int err = errno;
		DEBUG("Could not send short message. Error code %d (%s).", err, strerror(err));
	}
	pthread_mutex_unlock(&(context.na.socket_mutex));


//#ifdef HAVE_DEBUG
//#endif

	free(msg_buffer);

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

	DEBUG("Starting long send: %u -> %u (%u items)", src, dest, num_items);

	if (( ! context.remotes[dest]) ||
			(context.remotes[dest]->state != RIPC_RDMA_ESTABLISHED)) {
		create_rdma_connection(src, dest);
	}

	uint32_t i;
	int ret;
	mem_buf_t mem_buf, return_mem_buf;

	//build packet header
	mem_buf_t header_mem_buf = 
			ripc_alloc_recv_buf(
					sizeof(struct msg_header)
					+ sizeof(struct long_desc) * num_items
					+ sizeof(struct long_desc) * num_return_bufs
					);
	struct msg_header *hdr = (struct msg_header *)header_mem_buf.na->addr;
	struct long_desc *msg =
			(struct long_desc *)((uint64_t)hdr + sizeof(struct msg_header));
	struct long_desc *return_bufs_msg = (struct long_desc *)(
			(uint64_t)hdr
			+ sizeof(struct msg_header)
			+ sizeof(struct long_desc) * num_items);

	hdr->type = RIPC_MSG_SEND;
	hdr->from = src;
	hdr->to = dest;
	hdr->short_words = 0;
	hdr->long_words = num_items;
	hdr->new_return_bufs = num_return_bufs;

	struct ibv_sge sge; //+1 for header
	sge.addr = header_mem_buf.addr;
	sge.length = sizeof(struct msg_header)
							+ sizeof(struct long_desc) * num_items
							+ sizeof(struct long_desc) * num_return_bufs;
	sge.lkey = header_mem_buf.na->lkey;

	for (i = 0; i < num_items; ++i) {

		//make sure send buffers are registered with the hardware
		mem_buf = used_buf_list_get(buf[i]);
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

		msg[i].addr = mem_buf.addr;
		msg[i].length = length[i];
		msg[i].rkey = mem_buf.na->rkey;

		DEBUG("Long word %u: addr %lx, length %zu, rkey %#x",
				i,
				mem_buf.addr,
				length[i],
				mem_buf.na->rkey);

		DEBUG("Message reads: %s", (char *)mem_buf.addr);
		/*
		 * Now, check if we have a return buffer available. If so, push the
		 * contents of the long word to the other side; if not, just send the
		 * descriptor item and wait for the other side to pull the data.
		 */
		retry:
		return_mem_buf = return_buf_list_get(dest, length[i]);
		if (return_mem_buf.size == -1) {//no return buffer available
			DEBUG("Did not find a return buffer for item %u (checked: dest %u, length %zu)",
					i,
					dest,
					length[i]);
			continue;
		}
		DEBUG("Found suitable return buffer: Remote address %lx, size %zu, rkey %#x",
                      return_mem_buf.addr, return_mem_buf.size, return_mem_buf.na->rkey);

		struct ibv_sge rdma_sge;
		rdma_sge.addr = msg[i].addr;
		rdma_sge.length = length[i];
		rdma_sge.lkey = mem_buf.na->lkey;

		struct ibv_send_wr rdma_wr;
		rdma_wr.next = NULL;
		rdma_wr.num_sge = 1;
		rdma_wr.opcode = IBV_WR_RDMA_WRITE;
		rdma_wr.send_flags = IBV_SEND_SIGNALED;
		rdma_wr.sg_list = &rdma_sge;
		rdma_wr.wr_id = 0xdeadbeef;
		rdma_wr.wr.rdma.remote_addr = return_mem_buf.addr;
		rdma_wr.wr.rdma.rkey = return_mem_buf.na->rkey;
		struct ibv_send_wr *rdma_bad_wr;

		struct ibv_qp *rdma_qp;
		struct ibv_cq *rdma_cq, *tmp_cq;
		struct ibv_comp_channel *rdma_cchannel;
		pthread_mutex_lock(&remotes_mutex);
		rdma_qp = context.remotes[dest]->na.rdma_qp;
		rdma_cq = context.remotes[dest]->na.rdma_send_cq;
		rdma_cchannel = context.remotes[dest]->na.rdma_cchannel;
		pthread_mutex_unlock(&remotes_mutex);

		ret = ibv_post_send(
				rdma_qp,
				&rdma_wr,
				&rdma_bad_wr);
		if (ret) {
			ERROR("Failed to post write to return buffer for message item %u: %s", i, strerror(ret));
		} else {
			DEBUG("Posted write to return buffer for message item %u", i);
		}

		struct ibv_wc rdma_wc;
		void *ctx; //unused

		do {
			ibv_get_cq_event(rdma_cchannel,
			&tmp_cq,
			&ctx);

			assert(tmp_cq == rdma_cq);

			ibv_ack_cq_events(rdma_cq, 1);
			ibv_req_notify_cq(rdma_cq, 0);

		} while (!(ibv_poll_cq(rdma_cq, 1, &rdma_wc)));

		DEBUG("received completion message!");
		if (rdma_wc.status) {
			ERROR("Send result: %s", ibv_wc_status_str(rdma_wc.status));
			ERROR("QP state: %d", context.remotes[dest]->na.rdma_qp->state);
			free(return_mem_buf.na);
			goto retry; //return buffer was invalid, but maybe the next one will do
		} else {
			DEBUG("Result: %s", ibv_wc_status_str(rdma_wc.status));
			msg[i].transferred = 1;
			msg[i].addr = return_mem_buf.addr;
		}

		free(return_mem_buf.na);
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
				if (mem_buf.size == -1) //registration failed, drop buffer
					continue;
				//else
				DEBUG("Registration successful! rkey is %#x", mem_buf.na->rkey);
				used_buf_list_add(mem_buf);

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

		return_bufs_msg[i].addr = return_bufs[i] ? (uint64_t)return_bufs[i] : mem_buf.addr;
		return_bufs_msg[i].length = return_buf_lengths[i];
		return_bufs_msg[i].rkey = mem_buf.na->rkey;
	}

	//message item done, now send it
	struct ibv_send_wr wr;
	wr.next = NULL;
	wr.num_sge = 1;
	wr.opcode = IBV_WR_SEND;
	wr.send_flags = IBV_SEND_SIGNALED;
	wr.sg_list = &sge;
	wr.wr_id = 0xdeadbeef;

	struct ibv_send_wr *bad_wr = NULL;

	pthread_mutex_lock(&remotees_mutex);
	struct ibv_qp *dest_qp = context.remotes[src]->na.qp;
	struct ibv_cq *dest_cq = context.remotes[src]->na.send_cq;
	pthread_mutex_unlock(&remotes_mutex);

	ret = ibv_post_send(dest_qp, &wr, &bad_wr);

	if (bad_wr) {
		ERROR("Failed to post send: ", strerror(ret));
		return ret;
	} else {
		DEBUG("Successfully posted send!");
	}


	struct ibv_wc wc;
	while (!(ibv_poll_cq(dest_cq, 1, &wc))); //polling is probably faster here
	DEBUG("received completion message!");
	if (wc.status) {
		ERROR("Send result: %s", ibv_wc_status_str(wc.status));
		ERROR("QP state: %d", dest_qp->state);
	}
	DEBUG("Result: %s", ibv_wc_status_str(wc.status));

	ripc_buf_free(hdr);

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

	struct ibv_wc wc;
	void *ctx;
	struct ibv_cq *cq, *recv_cq;
	struct ibv_comp_channel *cchannel;
	struct ibv_qp *qp;
	uint32_t i;
	int ret = 0;

	pthread_mutex_lock(&services_mutex);
        
	cchannel = context.services[service_id]->na.cchannel;
	recv_cq = context.services[service_id]->na.recv_cq;
//The information from whom I receive is not given! I have to figure out a way receive it anyway!!
//This may best be doen via the completion channel or the completion queue. 
	qp = context.services[service_id]->na.qp;

	pthread_mutex_unlock(&services_mutex);
	DEBUG("receive cchannel %p service_id %d", cchannel, service_id);

	restart:

	post_new_recv_buf(qp);

	while (ibv_poll_cq(recv_cq, 1, &wc) < 1) {
		if (ibv_get_cq_event(cchannel,
		&cq,
		&ctx) < 0) {
			if (errno == EINTR) //interrupted, most likely by a user timeout mechanism
				return 1;
			else
				continue;
		}

		assert(cq == recv_cq);

		ibv_ack_cq_events(recv_cq, 1);
		ibv_req_notify_cq(recv_cq, 0);

	}

	//alarm(0);

	assert(wc.status == IBV_WC_SUCCESS);

	DEBUG("received!");

	struct ibv_recv_wr *wr = (struct ibv_recv_wr *)(wc.wr_id);
	struct msg_header *hdr = (struct msg_header *)(wr->sg_list->addr);

	if ((hdr->type != RIPC_MSG_SEND) || (hdr->to != service_id)) {
		ripc_buf_free(hdr);
		free(wr->sg_list);
		free(wr);
		ERROR("Spurious message, restarting");
		goto restart;
	}

	DEBUG("Message type is %#x", hdr->type);

	//the next block can cause segmentation faults. Disable it for now until
	//the error is found.
#if 0
	//cache remote address handle if we don't have it already
	pthread_mutex_lock(&remotes_mutex);

	if (( ! context.remotes[hdr->from]) ||
			( ! context.remotes[hdr->from]->na.ah)) {
		DEBUG("Caching remote address handle for remote %u", hdr->from);

		if ( ! context.remotes[hdr->from]) {
			context.remotes[hdr->from] = malloc(sizeof(struct remote_context));
			memset(context.remotes[hdr->from], 0, sizeof(struct remote_context));
		}

		assert(context.remotes[hdr->from]);

		DEBUG("context: %p", &context);
		DEBUG("context.remotes[hdr->from]: %p", context.remotes[hdr->from]);
		DEBUG("context.na.pd: %p", context.na.pd);
		DEBUG("wc: %p", &wc);

		context.remotes[hdr->from]->na.ah =
				ibv_create_ah_from_wc(context.na.pd, &wc, NULL, 1);

		//not conditional as we assume when the ah needs updating, so does the qp number
		context.remotes[hdr->from]->qp_num = wc.src_qp;
	}

	pthread_mutex_unlock(&remotes_mutex);
#endif
	struct short_header *msg =
			(struct short_header *)(wr->sg_list->addr
					+ sizeof(struct msg_header));

	struct long_desc *long_msg =
			(struct long_desc *)(wr->sg_list->addr
					+ sizeof(struct msg_header)
					+ sizeof(struct short_header) * hdr->short_words);

	struct long_desc *return_bufs =
			(struct long_desc *)(wr->sg_list->addr
					+ sizeof(struct msg_header)
					+ sizeof(struct short_header) * hdr->short_words
					+ sizeof(struct long_desc) * hdr->long_words);

	if (hdr->short_words) {
		*short_items = malloc(sizeof(void *) * hdr->short_words);
		assert(*short_items);
		*short_item_sizes = malloc(sizeof(uint32_t *) * hdr->short_words);
		assert(*short_item_sizes);
	} else {
		*short_items = NULL;
		*short_item_sizes = NULL;
	}

	for (i = 0; i < hdr->short_words; ++i) {
		(*short_items)[i] = (void *)(wr->sg_list->addr + msg[i].offset);
		(*short_item_sizes)[i] = msg[i].size;
		DEBUG("Short word %u reads:\n%s", i, (char *) (*short_items)[i]);
	}

	if (hdr->long_words) {
		*long_items = malloc(sizeof(void *) * hdr->long_words);
		assert(*long_items);
		*long_item_sizes = malloc(sizeof(uint32_t *) * hdr->long_words);
		assert(*long_item_sizes);
	} else {
		*long_items = NULL;
		*long_item_sizes = NULL;
	}

	for (i = 0; i < hdr->long_words; ++i) {
		DEBUG("Received long item: addr %#lx, length %zu, rkey %#x",
				long_msg[i].addr,
				long_msg[i].length,
				//long_msg[i].qp_num,
				long_msg[i].rkey);

		if (long_msg[i].transferred) {
			//message has been pushed to a return buffer
			DEBUG("Sender used return buffer at address %lx",
					long_msg[i].addr);
			(*long_items)[i] = (void *)long_msg[i].addr;
			(*long_item_sizes)[i] = long_msg[i].length;
			continue;
		}

		void *rdma_addr = recv_window_list_get(long_msg[i].length);
		if (!rdma_addr) {
			DEBUG("Not enough receive windows available! Discarding rest of message");
			ret = 1;
			break;
		}
		DEBUG("Found receive window at address %p", rdma_addr);

		mem_buf_t rdma_mem_buf = used_buf_list_get(rdma_addr);
		used_buf_list_add(rdma_mem_buf);

		DEBUG("Found rdma mr: addr %lx, length %zu",
				rdma_mem_buf.addr, rdma_mem_buf.size);

		struct ibv_sge rdma_sge;
		rdma_sge.addr = (uint64_t)rdma_addr;
		rdma_sge.length = long_msg[i].length;
		rdma_sge.lkey = rdma_mem_buf.na->lkey;

		struct ibv_send_wr rdma_wr;
		rdma_wr.next = NULL;
		rdma_wr.num_sge = 1;
		rdma_wr.opcode = IBV_WR_RDMA_READ;
		rdma_wr.send_flags = IBV_SEND_SIGNALED;
		rdma_wr.sg_list = &rdma_sge;
		rdma_wr.wr_id = 0xdeadbeef;
		rdma_wr.wr.rdma.remote_addr = long_msg[i].addr;
		rdma_wr.wr.rdma.rkey = long_msg[i].rkey;
		struct ibv_send_wr *rdma_bad_wr;

		struct ibv_qp *rdma_qp;
		struct ibv_cq *rdma_cq, *tmp_cq;
		struct ibv_comp_channel *rdma_cchannel;
		pthread_mutex_lock(&remotes_mutex);
		rdma_qp = context.remotes[hdr->from]->na.rdma_qp;
		rdma_cq = context.remotes[hdr->from]->na.rdma_send_cq;
		rdma_cchannel = context.remotes[hdr->from]->na.rdma_cchannel;
		pthread_mutex_unlock(&remotes_mutex);

		ret = ibv_post_send(
				rdma_qp,
				&rdma_wr,
				&rdma_bad_wr);
		if (ret) {
			ERROR("Failed to post rdma read for message item %u: %s", i, strerror(ret));
		} else {
			DEBUG("Posted rdma read for message item %u", i);
		}

		struct ibv_wc rdma_wc;

		do {
			ibv_get_cq_event(rdma_cchannel,
			&tmp_cq,
			&ctx);
			/*
			 * We don't want to interrupt a running transfer!
			 * Note that this RDMA operation always generates a completion
			 * eventually. If it fails, it generates a completion denoting
			 * the failure.
			 */

			assert(tmp_cq == rdma_cq);

			ibv_ack_cq_events(rdma_cq, 1);
			ibv_req_notify_cq(rdma_cq, 0);

		} while (!(ibv_poll_cq(rdma_cq, 1, &rdma_wc)));

		DEBUG("received completion message!");
		if (rdma_wc.status) {
			ERROR("Send result: %s", ibv_wc_status_str(rdma_wc.status));
			ERROR("QP state: %d", context.remotes[hdr->from]->na.rdma_qp->state);
		} else {
			DEBUG("Result: %s", ibv_wc_status_str(rdma_wc.status));
		}

		DEBUG("Message reads: %s", (char *)rdma_addr);

		(*long_items)[i] = rdma_addr;
		(*long_item_sizes)[i] = long_msg[i].length;
	}

	mem_buf_t mem_buf;
	for (i = 0; i < hdr->new_return_bufs; ++i) {
		DEBUG("Found new return buffer: Address %lx, length %zu, rkey %#x",
				return_bufs[i].addr,
				return_bufs[i].length,
				return_bufs[i].rkey);

		//if addr==0, the buffer was faulty at the other end
		if (return_bufs[i].addr) {
			DEBUG("Return buffer is valid");

			/*
			 * Our buffer lists store MRs by default, so take a little detour
			 * here to make them happy.
			 */
                        mem_buf.na = malloc(sizeof(struct ibv_mr));
			memset(mem_buf.na, 0, sizeof(struct ibv_mr));

			mem_buf.na->addr = (void *)return_bufs[i].addr;
			mem_buf.na->length = return_bufs[i].length;
                        mem_buf.na->rkey = return_bufs[i].rkey;
			mem_buf.addr = (uint64_t) mem_buf.na->addr;
			mem_buf.size = mem_buf.na->length;

			return_buf_list_add(hdr->from, mem_buf);

			DEBUG("Saved return buffer for destination %u", hdr->from);
		}
	}

	*from_service_id = hdr->from;
	*num_short_items = hdr->short_words;
	*num_long_items = hdr->long_words;

	if (! hdr->short_words)
		ripc_buf_free(hdr);
	free(wr->sg_list);
	free(wr);

	return ret;
}
