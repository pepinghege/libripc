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
#define RESOURCES_H_

#include <config.h>
#include <common.h>
#include <ripc.h>
#ifdef NETARCH_INFINIBAND
#include <infiniband/resources.h>
#endif
#ifdef NETARCH_BGP
#include <bgp/resources.h>
#endif 
#ifdef NETARCH_LOCAL
#include <local/resources.h>
#endif 
#ifdef NETARCH_IWARP
#include <iwarp/resources.h>
#endif 

struct rdma_connect_msg {
       enum msg_type type;
       uint16_t dest_service_id;
       uint16_t src_service_id;
       uint16_t lid;
       struct netarch_rdma_connect_msg na;
};
extern pthread_mutex_t rdma_connect_mutex;
extern struct service_id rdma_service_id;

void alloc_queue_state(struct service_id *service_id);

#endif /* RESOURCES_H_ */
