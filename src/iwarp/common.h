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
#ifndef COMMON_H_
#include "../common.h"
#endif

#ifndef __IWARP_COMMON_H__
#define __IWARP_COMMON_H__

#include <infiniband/verbs.h>
#include <rdma/rdma_cma.h>
#include <netinet/in.h>

#define	RECV_BUF_SIZE	1024

struct netarch_service_id {
	bool no_cchannel;
	struct ibv_cq *send_cq;
	struct ibv_cq *recv_cq;
	struct ibv_comp_channel *cchannel;
};

struct netarch_remote_context {
	struct rdma_cm_id *rdma_cm_id;
	pthread_mutex_t cm_id_mutex;
	struct ibv_qp *rdma_qp;
	struct ibv_cq *rdma_send_cq;
	struct ibv_cq *rdma_recv_cq;
	struct ibv_comp_channel *rdma_cchannel;
	in_addr_t ip_addr;
	uint16_t rdma_listen_port;
};

struct netarch_library_context {
	in_addr_t ip_addr;
	in_addr_t bcast_ip_addr;
	uint16_t conn_listen_port;
	struct rdma_event_channel *echannel;
	uint16_t lid;
	uint8_t port_num;
	struct rdma_cm_id *listen_cm_id;
	struct ibv_pd *pd;
};

struct interface {
	char name[16];
	struct sockaddr addr;
};

void netarch_init(void);

#endif /* !__IWARP_COMMON_H__ */
