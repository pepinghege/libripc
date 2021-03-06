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
#ifndef RESOURCES_H_
#include "../resources.h"
#endif

#ifndef __IWARP__RESOURCES_H__
#define __IWARP__RESOURCES_H__

struct netarch_rdma_connect_msg {
};

typedef struct cq_list_element {
	struct cq_list_element *next;
	struct ibv_cq *cq;
} cq_list_t;

void add_cq_to_list(struct ibv_cq *cq);

void del_cq_from_list(struct ibv_cq *cq);

void dump_cq_list(void);

void conn_mgmt_init(void);

void *start_conn_manager(void *arg);

void create_rdma_connection(uint16_t src, uint16_t dest);

/*
 * Mutex remotes_mutex must be hold when calling these functions!
 */
void prepare_qp_init_attr(struct ibv_qp_init_attr *init_attr, struct remote_context *remote);
/*
 * The remote-specific cm_id_mutex must not be hold when calling these functions!
 */
void dump_remote_context(struct remote_context*);
struct remote_context *alloc_remote_context(void);
void strip_remote_context(struct remote_context *remote);



void prepare_conn_param(struct rdma_conn_param *conn_param, void *payload, size_t len);

/*
 * List to hold all Completion Queues on which we expect either short messages
 * or the administration part of long messages.
 */
extern cq_list_t *cq_list;
extern pthread_mutex_t cq_list_mutex;

#endif /* !__IWARP__RESOURCES_H__ */
