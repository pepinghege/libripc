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
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <rdma/rdma_cma.h>
#include <netinet/in.h>
#include <../common.h>
#include <../resources.h>
#include <memory.h>

struct service_id rdma_service_id;
pthread_t conn_mgmt_thread;
//TODO: Use this mutex!
pthread_mutex_t rdma_connect_mutex;

void conn_mgmt_init() {
	struct sockaddr_in listen_addr;
	socklen_t addr_len		= sizeof(listen_addr);
	
	pthread_mutex_lock(&rdma_connect_mutex); //Unlocked in start_conn_manager

	context.na.echannel	= rdma_create_event_channel();
	if (!context.na.echannel) {
		panic("Could not create event channel.");
	}
	if (rdma_create_id(context.na.echannel, &context.na.listen_cm_id, NULL, RDMA_PS_TCP) < 0) {
		int err = errno;
		panic("Could not create cm-id for conn-mgmt-thread.");
	}

	memset(&listen_addr, 0, addr_len);
	listen_addr.sin_family		= AF_INET;
	listen_addr.sin_addr.s_addr	= context.na.ip_addr;
	/*
	 * We currently listen on one ip-address only, although in general, listening on all available
	 * ip-addresses is possible. Two different schemes could be realized:
	 * 1)	We handle multiple ip-addresses, i.e. connections with different remotes would be established
	 *	over different ip-addresses.
	 * 2)	We start listen on all ip-addresses, but after the first establisehd connection, only the
	 *	respective ip-address can be used by all other services.
	 */
	if (rdma_bind_addr(context.na.listen_cm_id, (struct sockaddr*) &listen_addr) < 0) {
		int err = errno;
		panic("Could not bind to given ip:port.");
	}
	context.na.conn_listen_port	= rdma_get_src_port(context.na.listen_cm_id);

	pthread_create(&conn_mgmt_thread, NULL, &start_conn_manager, NULL);
}

void *start_conn_manager(void *arg) {
	DEBUG("Starting connection management thread.");

	struct rdma_cm_event *event	= NULL;

	if (rdma_listen(context.na.listen_cm_id, 1) < 0) {
		int err = errno;
		panic("Could not listen on ip:port.");
	}

	pthread_mutex_unlock(&rdma_connect_mutex);

	/*
	 * TODO:	We don't handle cases in which the remote side crashes after sending a conn-request
	 *		but before receiving our acceptation. In this case the connection holds the state
	 *		RIPC_RDMA_CONNECTING. That would make it impossible for us to create a connection
	 *		ever again in this running instance of libRIPC.
         */
	while (1) {
		if (rdma_get_cm_event(context.na.echannel, &event) < 0) {
			int err = errno;
			DEBUG("Waiting for cm-event failed. Code: %d (%s).", err, strerror(err));
			continue;
		}

		switch (event->event) {
		case (RDMA_CM_EVENT_CONNECT_REQUEST): {
			struct rdma_connect_msg *msg, resp;
			struct rdma_cm_id *conn_id;
			struct remote_context *remote;
			struct ibv_qp_init_attr qp_init_attr;
			struct rdma_conn_param conn_param;

			if (event->param.conn.private_data_len != sizeof(*msg)) {
				DEBUG("Did not receive rdma connect message with connection request.");
				break;
			}
			msg = (struct rdma_connect_msg*) event->param.conn.private_data;

			if (msg->type != RIPC_RDMA_CONN_REQ) {
				DEBUG("Received invalid rdma connect message with connection request.");
				break;
			}

			conn_id = event->id;

			pthread_mutex_lock(&services_mutex);
			if (!context.services[msg->dest_service_id]) {
				pthread_mutex_unlock(&services_mutex);
				DEBUG("Received rdma connect request for service-id we don't host.");
				rdma_ack_cm_event(event);
				if (rdma_reject(conn_id, NULL, 0) < 0) {
					DEBUG(	"Error rejecting an invalid connection request. Code: %d (%s)", 
						err, strerror(err));
					continue;
				}
			}
			pthread_mutex_unlock(&services_mutex);
			pthread_mutex_lock(&remotes_mutex);
			if (context.remotes[msg->src_service_id]
				&& (context.remotes[msg->src_service_id]->state == RIPC_RDMA_CONNECTING)) {
				pthread_mutex_unlock(&remotes_mutex);
				DEBUG("Received rdma connect request while another connection establishment is already in progress");
				if (rdma_reject(conn_id, NULL, 0) < 0) {
					int err = errno;
					DEBUG(	"Error rejecting an invalid connection request. Code: %d (%s)", 
						err, strerror(err));
				}
				break;
			}
			if (context.remotes[msg->src_service_id]
				&& (context.remotes[msg->src_service_id]->state == RIPC_RDMA_ESTABLISHED)) {
				/*
				 * This should not happen and may indicate that someone does nasty stuff here.
				 *
				 * A valid situation in which a remote we already are connected with creates a
				 * new connection is that he was moved to another machine. 
				 * But in this case, the rdma-connection would not be valid anymore (there lies
				 * a TCP-connection below us, which will probably not accept the migration
				 * (TODO: or will it?). That would lead in a new resolving, and when that happens
				 * any established connection would have been stripped!
				 */
				pthread_mutex_unlock(&remotes_mutex);
				DEBUG("Received rdma connect request from a remote we are already connected with!");
				if (rdma_reject(conn_id, NULL, 0) < 0) {
					int err = errno;
					DEBUG(	"Error rejecting an invalid connection request. Code: %d (%s)", 
						err, strerror(err));
				}
				break;
			}
			if (context.remotes[msg->src_service_id]
				&& (context.remotes[msg->src_service_id]->state != RIPC_RDMA_DISCONNECTED)) {
				/*
				 * We should not be here! Either we have the case of a remote context with an invalid
				 * state, or the context of the given conn_id pointed to some random location, which we
				 * were allowed to touch. 
				 */
				pthread_mutex_unlock(&remotes_mutex);
				DEBUG("Received a connection error with invalid context-state.");
				break;
			}

			//If we are here, we have a valid connection request
			if (!context.remotes[msg->src_service_id]) {
				/*
				 * We know nothing yet of the remote service
				 * TODO:This case is pretty unlikely, if not impossible: Every remote instance which
				 *	sends a connection request has to have resolved us. During this resolving
				 *	process we would have created the remote context. So maybe just delete this?
				 */
				context.remotes[msg->src_service_id] = alloc_remote_context();
			}
			remote = context.remotes[msg->src_service_id];
			remote->state			= RIPC_RDMA_CONNECTING;
			remote->na.rdma_cchannel	= ibv_create_comp_channel(conn_id->verbs);
			if (!remote->na.rdma_cchannel) {
				DEBUG("Could not create completion channel for remote-id %hu.", msg->src_service_id);
				rdma_ack_cm_event(event);
				strip_remote_context(remote);
				pthread_mutex_unlock(&remotes_mutex);
				continue;
			}

			remote->na.rdma_send_cq		= ibv_create_cq(conn_id->verbs, 50, NULL, NULL, 0);
			if (!remote->na.rdma_send_cq) {
				DEBUG("Could not create sending completion queue for remote-id %hu.", msg->src_service_id);
				rdma_ack_cm_event(event);
				strip_remote_context(remote);
				pthread_mutex_unlock(&remotes_mutex);
				continue;
			}

			remote->na.rdma_recv_cq		= ibv_create_cq(conn_id->verbs, 50, NULL, remote->na.rdma_cchannel, 0);
			if (!remote->na.rdma_recv_cq) {
				DEBUG("Could not create receiving completion queue for remote-id %hu.", msg->src_service_id);
				rdma_ack_cm_event(event);
				strip_remote_context(remote);
				pthread_mutex_unlock(&remotes_mutex);
				continue;
			}

			prepare_qp_init_attr(&qp_init_attr, remote);
			if (rdma_create_qp(conn_id, context.na.pd, &qp_init_attr) < 0) {
				DEBUG("Could not create queue pair for remote-id %hu.", msg->src_service_id);
				rdma_ack_cm_event(event);
				strip_remote_context(remote);
				pthread_mutex_unlock(&remotes_mutex);
				continue;
			}
			remote->na.rdma_qp		= conn_id->qp;

			pthread_mutex_lock(&(remote->na.cm_id_mutex));
			remote->na.rdma_cm_id		= conn_id;
			conn_id->context		= remote; //This way we generate a cm_id-to-remote_context-mapping.
			pthread_mutex_unlock(&remotes_mutex);

			//TODO:	I may want to send more private data (maybe buffers, etc)
			memcpy(&resp, msg, sizeof(resp));
			resp.type		= RIPC_RDMA_CONN_REPLY;
			prepare_conn_param(&conn_param, &resp, sizeof(resp));
			if (rdma_accept(conn_id, &conn_param) < 0) {
				int err = errno;
				DEBUG("Could not send rdma connection accept. Code: %d (%s).", err, strerror(err));
				rdma_ack_cm_event(event);
				pthread_mutex_lock(&remotes_mutex);
				pthread_mutex_unlock(&(remote->na.cm_id_mutex));
				strip_remote_context(remote);
				pthread_mutex_unlock(&remotes_mutex);
				continue;
			}
			pthread_mutex_unlock(&(remote->na.cm_id_mutex));

			break;
		}
		case (RDMA_CM_EVENT_CONNECT_ERROR): {
			/*
			 * This event is received when an error occurs during connection establishment. Hence it should only
			 * occur wenn the state of the remote context is RIPC_RDMA_CONNECTING. If this is the case, we just
			 * abort the connecting-process. Otherwise we ignore the event but post a debug-message.
			 */
			struct rdma_cm_id *conn_id	= event->id;
			struct remote_context *remote	= conn_id->context;

			if (!remote) {
				DEBUG("Received a connecting error without a linked remote context.");
				break;
			}

			pthread_mutex_lock(&remotes_mutex);
			switch (remote->state) {
			case RIPC_RDMA_DISCONNECTED:
				DEBUG("Received a connecting error on a remote context which is disconnected.");
				break;
			case RIPC_RDMA_ESTABLISHED:
				DEBUG("Received a connecting error on a remote context which is already connected.");
				break;
			case RIPC_RDMA_CONNECTING:
				strip_remote_context(remote);
				break;
			default:
				/*
				 * We should not be here! Either we have the case of a remote context with an invalid
				 * state, or the context of the given conn_id pointed to some random location, which we
				 * were allowed to touch. 
				 */
				DEBUG("Received a connection error with invalid context-state.");
			}
			pthread_mutex_unlock(&remotes_mutex);

			break;
		}
		case (RDMA_CM_EVENT_DISCONNECTED):
			/*
			 * This event is received when a) we or b) our communication partner called rdma_disconnect
			 * on a connection. 
			 * As disconnection aren't handled in libRIPC we do not handle this event yet.
			 */
			break;
		case (RDMA_CM_EVENT_ESTABLISHED): {
			/*
			 * This event indicates that the connection establishment is complete.
			 */
			struct rdma_cm_id *conn_id	= event->id;
			struct remote_context *remote	= conn_id->context;

			if (!remote) {
				DEBUG("Received a connecting error without a linked remote context.");
				break;
			}

			pthread_mutex_lock(&remotes_mutex);
			switch (remote->state) {
			case RIPC_RDMA_DISCONNECTED:
				DEBUG("Received connection establishment on a remote we were not establishing a connection with.");
				break;
			case RIPC_RDMA_ESTABLISHED:
				DEBUG("Received connection establishment on a remote we are already connected with.");
				break;
			case RIPC_RDMA_CONNECTING:
				remote->state = RIPC_RDMA_ESTABLISHED;
				break;
			default:
				/*
				 * We should not be here! Either we have the case of a remote context with an invalid
				 * state, or the context of the given conn_id pointed to some random location, which we
				 * were allowed to touch. 
				 */
				DEBUG("Received a connection establishment with invalid context-state.");
			}
			pthread_mutex_unlock(&remotes_mutex);

			break;
		}
		default:
			DEBUG("Received unexpected connection event (%s).", rdma_event_str(event->event));
			break;
		}

		rdma_ack_cm_event(event);
	}

}

void strip_remote_context(struct remote_context *remote) {
	if (remote->na.rdma_qp) {
		pthread_mutex_lock(&(remote->na.cm_id_mutex));
		rdma_destroy_qp(remote->na.rdma_cm_id);
		remote->na.rdma_qp		= NULL;
		pthread_mutex_unlock(&(remote->na.cm_id_mutex));
	}
	if (remote->na.rdma_recv_cq) {
		ibv_destroy_cq(remote->na.rdma_recv_cq);
		remote->na.rdma_recv_cq		= NULL;
	}
	if (remote->na.rdma_send_cq) {
		ibv_destroy_cq(remote->na.rdma_send_cq);
		remote->na.rdma_send_cq		= NULL;
	}
	if (remote->na.rdma_cchannel) {
		ibv_destroy_comp_channel(remote->na.rdma_cchannel);
		remote->na.rdma_cchannel	= NULL;
	}
	if (remote->return_bufs) {
		free(remote->return_bufs);
		remote->return_bufs		= NULL;
	}
	if (remote->na.rdma_cm_id) {
		pthread_mutex_lock(&(remote->na.cm_id_mutex));
		rdma_destroy_id(remote->na.rdma_cm_id);
		remote->na.rdma_cm_id		= NULL;
		pthread_mutex_unlock(&(remote->na.cm_id_mutex));
	}
	remote->state			= RIPC_RDMA_DISCONNECTED;
}

void prepare_qp_init_attr(struct ibv_qp_init_attr *init_attr, struct remote_context *remote) {
	memset(init_attr, 0, sizeof(*init_attr));
	init_attr->send_cq		= remote->na.rdma_send_cq;
	init_attr->recv_cq		= remote->na.rdma_recv_cq;
	//TODO:	Think those numbers through
	init_attr->cap.max_send_wr	= 1000;
	init_attr->cap.max_recv_wr	= 1000;
	init_attr->cap.max_send_sge	= 2;
	init_attr->cap.max_recv_sge	= 2;
	init_attr->cap.max_inline_data	= 64;
	init_attr->qp_type		= IBV_QPT_RC;
	init_attr->sq_sig_all		= 0;
}

void prepare_conn_param(struct rdma_conn_param *conn_param, void *payload, size_t len) {
	memset(conn_param, 0, sizeof(*conn_param));
	conn_param->private_data	= payload;
	conn_param->private_data_len	= len;
	conn_param->responder_resources	= 5;
	conn_param->initiator_depth	= 5;
	conn_param->flow_control	= 0;
	conn_param->rnr_retry_count	= 5;
}

void create_rdma_connection(uint16_t src, uint16_t dest) {
	pthread_mutex_lock(&services_mutex);
	assert(context.services[src]);
	pthread_mutex_unlock(&services_mutex);

	struct remote_context *remote;
	pthread_mutex_t *conn_id_mutex;
	struct sockaddr_in local_addr, remote_addr;
	socklen_t addr_len		= sizeof(local_addr);
	struct rdma_cm_event *event	= NULL;
	struct rdma_cm_id *conn_id;
	struct rdma_event_channel *echannel;
	struct ibv_qp_init_attr qp_init_attr;
	struct rdma_conn_param conn_param;
	struct rdma_connect_msg msg, *resp;
	size_t retries;
	size_t max_retries		= 2;

	/*
	 *NOTE:	We hold this mutex _very_ long, especially as we hold it while waiting for network events.
	 *	Unfortunately, this is necessary as the rdmacm-library is working on the remote.na.rdma_cm_id
	 *	and we do not want to let it be changed while the rdmacm-library is performing its magic on it.
	 *TODO: Maybe establish a mutex for every remote.na.rdma_cm_id, so that other remotes or some resolver-
	 *	action is not affected by this.
	 *NOTE:	We have to consider, that this mutex might even block our responder-thread, what in turn may lead
	 *	to timeouts on his part, and therefore really damages the whole system instead of the performance only.
	 */
	pthread_mutex_lock(&remotes_mutex);
	if (!context.remotes[dest]) {
		pthread_mutex_unlock(&remotes_mutex);
		resolve(src, dest);
		pthread_mutex_lock(&remotes_mutex);
	}
	remote	= context.remotes[dest];

	if (remote->state != RIPC_RDMA_DISCONNECTED) {
		pthread_mutex_unlock(&remotes_mutex);
		return;
	}

	//If we are here, the dest-service-id is resolved, but no connection established, yet
	remote->state == RIPC_RDMA_CONNECTING;
	conn_id_mutex = &remote->na.cm_id_mutex;
	pthread_mutex_unlock(&remotes_mutex);

	echannel = rdma_create_event_channel();
	if (!echannel) {
		DEBUG("Could not create event channel for connection establishment.");
		pthread_mutex_lock(&remotes_mutex);
		remote->state == RIPC_RDMA_DISCONNECTED;
		pthread_mutex_unlock(&remotes_mutex);
		return;
	}

	pthread_mutex_lock(conn_id_mutex);
	if (rdma_create_id(echannel, &(remote->na.rdma_cm_id), context.na.pd, RDMA_PS_TCP) < 0) {
		DEBUG("Could not create cm-id.");
		pthread_mutex_unlock(conn_id_mutex);
		pthread_mutex_lock(&remotes_mutex);
		remote->state == RIPC_RDMA_DISCONNECTED;
		pthread_mutex_unlock(&remotes_mutex);
		rdma_destroy_event_channel(echannel);
		return;
	}
	remote->na.rdma_cm_id->context	= remote;
	conn_id				= remote->na.rdma_cm_id;

	memset(&local_addr, 0, addr_len);
	local_addr.sin_family		= AF_INET;
	local_addr.sin_addr.s_addr	= context.na.ip_addr;
	memset(&remote_addr, 0, addr_len);
	remote_addr.sin_family		= AF_INET;
	remote_addr.sin_addr.s_addr	= remote->na.ip_addr;
	remote_addr.sin_port		= htons(remote->na.rdma_listen_port);

	retries = 0;
resolve_addr:
	if (rdma_resolve_addr(	remote->na.rdma_cm_id, (struct sockaddr*) &local_addr,
				(struct sockaddr*) &remote_addr, 3000) < 0) {
		int err = errno;
		DEBUG("Error, while resolving remote address in %hu. try. Code: %d (%s).", ++retries, err, strerror(err));
		if (retries <= max_retries)
			goto resolve_addr;

		pthread_mutex_unlock(conn_id_mutex);
		pthread_mutex_lock(&remotes_mutex);
		strip_remote_context(remote);
		phtread_mutex_unlock(&remotes_mutex);
		rdma_destroy_event_channel(echannel);
		return;
	}

	/*
	 * TODO:We need some kind of timeout mechanism.
	 * 	That mechanism has to skip the blocking of rdma_get_cm_event, but must also catch cases in which
	 *	unexpected events are received again and again, without the blocking-timeout to be reached.
	 *
	 *	Rumors are, that some file-descriptor manipulations allow for blocking-timeouts..
	 */
	if (rdma_get_cm_event(context.na.echannel, &event) < 0) {
		int err = errno;
		DEBUG("Error, while waiting for RDMA_CM_EVENT_ADDR_RESOLVED. Code: %d (%s).", err, strerror(err));
	}

	while (!event || event->event != RDMA_CM_EVENT_ADDR_RESOLVED) {
		if (!event) {
			DEBUG("Could not resolve address in %hu. try.", ++retries);
			if (retries <= max_retries)
				goto resolve_addr;

			pthread_mutex_unlock(conn_id_mutex);
			pthread_mutex_lock(&remotes_mutex);
			strip_remote_context(remote);
			pthread_mutex_unlock(&remotes_mutex);
			rdma_destroy_event_channel(echannel);
			return;
		} else if (event->event == RDMA_CM_EVENT_ADDR_ERROR) {
			DEBUG("Could not resolve address in %hu. try.", ++retries);
			rdma_ack_cm_event(event);
			if (retries <= max_retries)
				goto resolve_addr;

			pthread_mutex_unlock(conn_id_mutex);
			pthread_mutex_lock(&remotes_mutex);
			strip_remote_context(remote);
			pthread_mutex_unlock(&remotes_mutex);
			rdma_destroy_event_channel(echannel);
			return;
		} else
			DEBUG(	"Received unexpected event (%s instead of RDMA_CM_EVENT_ADDR_RESOLVED).",
				rdma_event_str(event->event));
		rdma_ack_cm_event(event);
		event = NULL;

		if (rdma_get_cm_event(context.na.echannel, &event) < 0) {
			int err = errno;
			DEBUG("Error, while waiting for RDMA_CM_EVENT_ADDR_RESOLVED. Code: %d (%s).", err, strerror(err));
		}
	}
	rdma_ack_cm_event(event);
	event				= NULL;

	//TODO:	A fine programmer would source the following part (including the QP-creation) out..
	pthread_mutex_lock(&remotes_mutex);
	remote->na.rdma_cchannel	= ibv_create_comp_channel(conn_id->verbs);
	if (!remote->na.rdma_cchannel) {
		DEBUG("Could not create completion channel for remote-id %hu.", msg->src_service_id);
		rdma_ack_cm_event(event);
		strip_remote_context(remote);
		pthread_mutex_unlock(&remotes_mutex);
		rdma_destroy_event_channel(echannel);
		return;
	}

	remote->na.rdma_send_cq		= ibv_create_cq(conn_id->verbs, 50, NULL, NULL, 0);
	if (!remote->na.rdma_send_cq) {
		DEBUG("Could not create sending completion queue for remote-id %hu.", dest);
		rdma_ack_cm_event(event);
		strip_remote_context(remote);
		pthread_mutex_unlock(&remotes_mutex);
		rdma_destroy_event_channel(echannel);
		return;
	}

	remote->na.rdma_recv_cq		= ibv_create_cq(conn_id->verbs, 50, NULL, remote->na.rdma_cchannel, 0);
	if (!remote->na.rdma_recv_cq) {
		DEBUG("Could not create receiving completion queue for remote-id %hu.", dest);
		rdma_ack_cm_event(event);
		strip_remote_context(remote);
		pthread_mutex_unlock(&remotes_mutex);
		rdma_destroy_event_channel(echannel);
		return;
	}

	prepare_qp_init_attr(&qp_init_attr, remote);
	if (rdma_create_qp(conn_id, context.na.pd, &qp_init_attr) < 0) {
		DEBUG("Could not create queue pair for remote-id %hu.", msg->src_service_id);
		rdma_ack_cm_event(event);
		strip_remote_context(remote);
		pthread_mutex_unlock(&remotes_mutex);
		rdma_destroy_event_channel(echannel);
		return;
	}
	remote->na.rdma_qp		= conn_id->qp;
	pthread_mutex_unlock(&remotes_mutex);

	retries = 0;
resolve_route:
	if (rdma_resolve_route(remote->na.rdma_cm_id, 3000) < 0) {
		int err = errno;
		DEBUG("Error, while resolving route in %hu. try. Code: %d (%s).", ++retries, err, strerror(err));
		if (retries <= max_retries) 
			goto resolve_route;

		pthread_mutex_unlock(conn_id_mutex);
		pthread_mutex_lock(&remotes_mutex);
		strip_remote_context(remote);
		pthread_mutex_unlock(&remotes_mutex);
		rdma_destroy_event_channel(echannel);
		return;
	}
	
	if (rdma_get_cm_event(context.na.echannel, &event) < 0) {
		int err = errno;
		DEBUG("Error, while waiting for RDMA_CM_EVENT_ROUTE_RESOLVED. Code: %d (%s).", err, strerror(err));
	}

	while (!event || event->event != RDMA_CM_EVENT_ROUTE_RESOLVED) {
		if (!event) {
			DEBUG("Could not resolve address in %hu. try.", ++retries;);
			if (retries <= max_retries) 
				goto resolve_route;

			pthread_mutex_unlock(conn_id_mutex);
			pthread_mutex_lock(&remotes_mutex);
			strip_remote_context(remote);
			pthread_mutex_unlock(&remotes_mutex);
			rdma_destroy_event_channel(echannel);
			return;
		} else if (event->event == RDMA_CM_EVENT_ROUTE_ERROR) {
			DEBUG("Could not resolve address in %hu. try.", ++retries);
			rdma_ack_cm_event(event);
			if (retries <= max_retries) 
				goto resolve_route;

			pthread_mutex_unlock(&remotes_mutex);
			pthread_mutex_lock(&remotes_mutex);
			strip_remote_context(remote);
			pthread_mutex_unlock(&remotes_mutex);
			rdma_destroy_event_channel(echannel);
			return;
		} else
			DEBUG(	"Received unexpected event (%s instead of RDMA_CM_EVENT_ROUTE_RESOLVED).",
				rdma_event_str(event->event));
		rdma_ack_cm_event(event);
		event = NULL;

		if (rdma_get_cm_event(context.na.echannel, &event) < 0) {
			int err = errno;
			DEBUG("Error, while waiting for RDMA_CM_EVENT_ROUTE_RESOLVED. Code: %d (%s).", err, strerror(err));
		}
	}
	rdma_ack_cm_event(event);
	event				= NULL;
	
	memset(&msg, 0, sizeof(msg));
	msg.type			= RIPC_RDMA_CONN_REQ;
	msg.dest_service_id		= dest;
	msg.src_service_id		= src;
	prepare_conn_param(&conn_param, (void*) &msg, sizeof(msg));

	retries = 0;
	//NOTE:	Again, we are holding this mutex _very_ long. See above for details.
	pthread_mutex_lock(&remotes_mutex);
connect:
	if (++retries > max_retries) 
		goto out_cmid;

	if (rdma_connect(remote->na.rdma_cm_id, &conn_param) < 0) {
		int err = errno;
		DEBUG("Error, while connecting in %hu. try. Code: %d (%s).", ++retries, err, strerror(err));
		goto connect;
	}
	
	if (rdma_get_cm_event(context.na.echannel, &event) < 0) {
		int err = errno;
		DEBUG("Error, while waiting for RDMA_CM_EVENT_ESTABLISHED. Code: %d (%s).", err, strerror(err));
	}

	while (!event || event->event != RDMA_CM_EVENT_ESTABLISHED) {
		if (!event) {
			DEBUG("Could not connect in %hu. try.", (retries + 1));
			rdma_ack_cm_event(event);
			goto connect;
		} else if (event->event == RDMA_CM_EVENT_CONNECT_ERROR) {
			DEBUG("Could not connect in %hu. try.", (retries + 1));
			rdma_ack_cm_event(event);
			goto connect;
		} else if (event->event == RDMA_CM_EVENT_REJECTED) {
			DEBUG("Remote side rejected connection in %hu. try." (retries + 1));
			rdma_ack_cm_event(event);
			goto out_cmid;
		} else if (event->event == RDMA_CM_EVENT_UNREACHABLE) {
			DEBUG("Remote side is unreachable in &hu. try." (retries + 1));
			rdma_ack_cm_event(event);
			goto connect;
		} else {
			DEBUG(	"Received unexpected event (%s instead of RDMA_CM_EVENT_ROUTE_RESOLVED).",
				rdma_event_str(event->event));
		}
		rdma_ack_cm_event(event);
		event = NULL;

		if (rdma_get_cm_event(context.na.echannel, &event) < 0) {
			int err = errno;
			DEBUG("Error, while waiting for RDMA_CM_EVENT_ESTABLISHED. Code: %d (%s).", err, strerror(err));
		}
	}

	pthread_mutex_unlock(conn_id_mutex);

	if (event->param.conn.private_data_len != sizeof(*resp)) {
		DEBUG("The private data of the connection response has invalid length.");
		rdma_ack_cm_event(event);
		//TODO:	Here we would have to disconnect
		goto out;
	}
	resp = (struct rdma_connect_msg*) event->param.conn.private_data;
	if (resp->type != RIPC_RDMA_CONN_REPLY) {
		DEBUG("The received connection message has an unexpected type.");
		rdma_ack_cm_event(event);
		//TODO:	Here we would have to disconnect
		goto out;
	}
	if (resp->dest_service_id != dest || resp->src_service_id != src) {
		DEBUG("We received a connection message which was not meant for us.");
		rdma_ack_cm_event(event);
		//TODO:	Here we would have to disconnect
		goto out;
	}

	pthread_mutex_lock(&remotes_mutex);
	remote->state = RIPC_RDMA_ESTABLISHED;
	pthread_mutex_unlock(&remotes_mutex);

	pthread_mutex_lock(conn_id_mutex);
	pthread_mutex_lock(&rdma_connect_mutex);
	rdma_migrate_id(conn_id, context.na.echannel);
	pthread_mutex_unlock(&rdma_connect_mutex);
	pthread_mutex_unlock(conn_id_mutex);

	rdma_destroy_event_channel(echannel);
	return;

out_cmid:
	pthread_mutex_unlock(conn_id_mutex);
out:
	pthread_mutex_lock(&remotes_mutex);
	strip_remote_context(remote);
	pthread_mutex_unlock(&remotes_mutex);
	rdma_destroy_event_channel(echannel);
	return;
}

struct remote_context *alloc_remote_context(void) {
	size_t len			= sizeof(struct remote_context);
	struct remote_context *temp	= (struct remote_context*) malloc(len);
	memset(temp, 0, len);
	temp->state			= RIPC_RDMA_DISCONNECTED;
	pthread_mutex_init(&(temp->na.cm_id_mutex), NULL);

	return temp;
}

void alloc_queue_state(struct service_id *service_id) {
	DEBUG("alloc_queue_state");
}
