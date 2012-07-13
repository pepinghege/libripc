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
#include <errno.h>
#include <rdma/rdma_cma.h>
#include <../common.h>
#include <../resources.h>
#include <memory.h>

struct service_id rdma_service_id;
pthread_t conn_mgmt_thread;
pthread_mutex_t rdma_connect_mutex;

void conn_mgmt_init() {
	pthread_mutex_lock(&rdma_connect_mutex); //Unlocked in start_conn_manager

	context.na.echannel	= rdma_create_event_channel();
	if (rdma_create_id(context.na.echannel, &context.na.listen_cm_id, NULL, RDMA_PS_TCP) < 0) {
		int err = errno;
		panic("Could not create cm-id for conn-mgmt-thread. Code: %d (%s).", err, strerror(err));
	}

	pthread_create(&conn_mgmt_thread, NULL, &start_conn_manager, NULL);
}

void *start_conn_manager(void *arg) {
	DEBUG("Starting connection management thread.");

	struct rdma_cm_event *event	= NULL;
	struct sockaddr_in listen_addr;
	socklen_t addr_len		= sizeof(listen_addr);
	
	memset(&listen_addr, 0, addr_len);
	listen_addr.sin_family		= AF_INET;
	listen_addr.sin_addr.s_addr	= context.na.ip_addr;
	listen_addr.sin_port		= htons(context.na.conn_listen_port);
	if (rdma_bind_addr(context.na.listen_cm_id, (struct sockaddr*) &listen_addr) < 0) {
		int err = errno;
		panic("Could not bind to given ip:port. Code %d (%s).", err, strerror(err));
	}

	if (rdma_listen(context.na.listen_cm_id, 1) < 0) {
		int err = errno;
		panic("Could not listen. Code %d (%s).", err, strerror(err));
	}

	pthread_mutex_unlock(&rdma_connect_mutex);

	while (1) {
		if (rdma_get_cm_event(context.na.echannel, &event) < 0) {
			int err = errno;
			DEBUG("Waiting for cm-event failed. Code: %d (%s).", err, strerror(err));
			continue;
		}

		switch (event->event) {
		case (RDMA_CM_EVENT_CONNECT_REQUEST):
			struct rdma_connect_msg *msg;
			struct rdma_cm_id *conn_id;
			struct remote_context *context;
			struct ibv_qp_init_attr qp_init_attr;

			if (event->param.conn.private_data_len != sizeof(*msg)) {
				DEBUG("Did not receive rdma connect message with connection request.");
				rdma_ack_cm_event(event);
				continue;
			}
			struct rdma_connect_msg *msg = (struct rdma_connect_msg*) event->param.conn.private_data;

			if (msg->type != RIPC_RDMA_CONN_REQ) {
				DEBUG("Received invalid rdma connect message with connection request.");
				rdma_ack_cm_event(event);
				continue;
			}

			pthread_mutex_lock(&services_mutex);
			if (!context.services[msg->dest_service_id]) {
				pthread_mutex_unlock(&services_mutex);
				DEBUG("Received rdma connect message for service-id we don't host.");
				conn_id = event->id;
				rdma_ack_cm_event(event);
				if (rdma_reject(conn_id, NULL, 0) < 0) {
					DEBUG(	"Error rejecting an invalid connection request. Code: %d (%s)", 
						err, strerror(err);
					continue;
				}
			}
			pthread_mutex_unlock(&services_mutex);

			//If we are here, we have a valid connection request
			pthread_mutex_lock(&remotes_mutex);
			if (!context.remotes[msg->src_service_id]) {
				//We know nothing yet of the remote service
				context.remotes[msg->src_service_id]
					= (struct remote_context*) malloc(sizeof(struct remote_context));
				memset(context.remotes[msg->src_service_id], 0, sizeof(struct remote_context));
			}
			remote = context.remotes[msg->src_service_id];
			remote->rdma_cchannel = ibv_create_comp_channel(conn_id->verbs);
			if (!remote->rdma_cchannel) {
				DEBUG("Could not create completion channel for remote-id %hu.", msg->src_service_id);
				goto conn_req_out;
			}

			remote->rdma_send_cq = ibv_create_cq(conn_id->verbs, 50, NULL, 0);
			if (!remote->rdma_send_cq) {
				DEBUG("Could not create sending completion queue for remote-id %hu.", msg->src_service_id);
				goto conn_req_out;
			}

			remote->rdma_recv_cq = ibv_create_cq(conn_id->verbs, 50, cc, 0);
			if (!remote->rdma_recv_cq) {
				DEBUG("Could not create receiving completion queue for remote-id %hu.", msg->src_service_id);
				goto conn_req_out;
			}

			prepare_qp_init_attr(&qp_init_attr, remote);
			remote->rdma_qp = ibv_create_qp(conn_id, context.na.pd, &qp_init_attr);
			if (!remote->rdma_qp) {
				DEBUG("Could not create queue pair for remote-id %hu.", msg->src_service_id);
				goto conn_req_out;
			}

			//TODO:	Perform rdma_connect, and decide what private data I send..

			pthread_mutex_unlock(&remotes_mutex);
			rdma_ack_cm_event(event);
			break;
conn_req_out:
			rdma_ack_cm_event(event);
			if (remote->rdma_recv_cq)
				ibv_destroy_cq(remote->rdma_recv_cq);
			if (remote->rdma_send_cq)
				ibv_destroy_cq(remote->rdma_send_cq);
			if (remote->rdma_cchannel)
				ibv_destroy_comp_channel(remote->rdma_cchannel);
			free(remote);
			context.remotes[msg->src_service_id] = NULL;
			pthread_mutex_unlock(&remotes_mutex);
			continue;

		case (RDMA_CM_EVENT_CONNECT_ERROR):
			break;
		case (RDMA_CM_EVENT_DISCONNECTED):
			break;
		case (RDMA_CM_EVENT_ESTABLISHED):
			break;
		default:
			break;
		}
	}

}

void alloc_queue_state(struct service_id *service_id) {
	DEBUG("alloc_queue_state");
}
