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
#include "resolver.h"
#include "ripc.h"
#include "resources.h"
#include "memory.h"
#include <string.h>
#include <pthread.h>

struct service_id mcast_service_id, unicast_service_id;

struct ibv_ah *mcast_ah;
pthread_t responder_thread;
pthread_mutex_t resolver_mutex;

/*
 * TODO:
 * - Make this a separate thread
 * - Check if the multicast group already exists, and if so, attach to it
 * - Of course, wait for requests and create responses :)
 */

void handle_rdma_connect(struct rdma_connect_msg *msg) {
	DEBUG("Received rdma connect message: from %u, for %u, remote lid: %u, remote psn: %u, remote qpn: %u",
			msg->src_service_id,
			msg->dest_service_id,
			msg->lid,
			msg->psn,
			msg->qpn);

	uint32_t psn = 0;
	struct ibv_qp_attr attr;
	int ret;

    if (! context.remotes[msg->src_service_id])
            resolve(msg->dest_service_id, msg->src_service_id);
    assert(context.remotes[msg->src_service_id]);
    struct remote_context *remote = context.remotes[msg->src_service_id];

    pthread_mutex_lock(&remotes_mutex);
    if (remote->state == RIPC_RDMA_ESTABLISHED) {
		/*
		 * Either another thread beat us here, or a previous response
		 * got lost. In either case, if remote is still the same service
		 * that sent the first request, we can keep our state and just
		 * re-send the reply. However, if remote is a new instance with
		 * the same service ID, this will result in an invalid connection
		 * state.
		 *
		 * todo: proper handling of crashed remotes.
		 */
    	DEBUG("Connection for remote %u already established", msg->src_service_id);
		goto reply;
    }
    if (remote->state == RIPC_RDMA_CONNECTING) {
		//another thread is in the process of connecting, but not finished yet
		pthread_mutex_unlock(&remotes_mutex);
		//fixme: sleep() is nasty, find something better
		sleep(1); //give the other guy some time to finish
		return;
    }

    /*
     * Now setup connection state.
     * Note that there is no completion channel here, as we only ever do
     * rdma on this qp. We do need the cqs though to wait for completion of
     * certain events.
     * TODO: Waiting spins on the cqs at the moment. Is that wise?
     */

	remote->state = RIPC_RDMA_CONNECTING;

	remote->na.rdma_cchannel = ibv_create_comp_channel(context.na.device_context);
    if (remote->na.rdma_cchannel == NULL) {
            ERROR("Failed to allocate rdma completion channel!");
            goto error;
            return;
    } else {
            DEBUG("Allocated rdma completion channel: %u", remote->na.rdma_cchannel->fd);
    }

    remote->na.rdma_recv_cq = ibv_create_cq(
                    context.na.device_context,
                    100,
                    NULL,
                    NULL,
                    0);
    if (remote->na.rdma_recv_cq == NULL) {
            ERROR("Failed to allocate receive completion queue!");
            goto error;
            return;
    } else {
            DEBUG("Allocated receive completion queue: %u", remote->na.rdma_recv_cq->handle);
    }

    remote->na.rdma_send_cq = ibv_create_cq(
                    context.na.device_context,
                    100,
                    NULL,
                    remote->na.rdma_cchannel,
                    0);
    if (remote->na.rdma_send_cq == NULL) {
            ERROR("Failed to allocate send completion queue!");
            goto error;
            return;
    } else {
            DEBUG("Allocated send completion queue: %u", remote->na.rdma_send_cq->handle);
    }

    ibv_req_notify_cq(remote->na.rdma_send_cq, 0);

    //now for the qp. Remember that we need an RC qp here!
    struct ibv_qp_init_attr init_attr = {
            .send_cq = remote->na.rdma_send_cq,
            .recv_cq = remote->na.rdma_recv_cq,
            .cap     = {
                    .max_send_wr  = 2000,
                    .max_recv_wr  = 1, //0 doesn't work here as it seems...
                    .max_send_sge = 1, //we don't do scatter-gather for long sends
                    .max_recv_sge = 1,
            },
            .qp_type = IBV_QPT_RC
    };

    remote->na.rdma_qp = ibv_create_qp(context.na.pd, &init_attr);
    if (!remote->na.rdma_qp) {
            ERROR("Failed to allocate rdma QP");
            goto error;
    } else {
            DEBUG("Allocated rdma QP %u", remote->na.rdma_qp->qp_num);
    }

    attr.qp_state = IBV_QPS_INIT;
    attr.port_num = context.na.port_num;
    attr.pkey_index = 0;
    attr.qp_access_flags = IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;

    if (ibv_modify_qp(remote->na.rdma_qp, &attr,
                    IBV_QP_STATE |
                    IBV_QP_PKEY_INDEX |
                    IBV_QP_PORT |
                    IBV_QP_ACCESS_FLAGS
                    )) {
            ERROR("Failed to modify rdma QP %u state to INIT", remote->na.rdma_qp->qp_num);
            goto error;
    }

    /*
     * now we have the same state as the other side, but we have the information
     * the other side is missing. We complete the process, then we send the reply.
     * We could send the reply first, which would make the remaining establishement
     * process concurrent, but that might raise synchronization issues.
     */

	attr.qp_state = IBV_QPS_RTR;
	attr.path_mtu = IBV_MTU_2048;
	attr.dest_qp_num = msg->qpn;
	attr.rq_psn = msg->psn;
	attr.max_dest_rd_atomic = 1;
	attr.min_rnr_timer = 12;
	attr.ah_attr.is_global = 0;
	attr.ah_attr.dlid = msg->lid;
	attr.ah_attr.sl = 0;
	attr.ah_attr.src_path_bits = 0;
	attr.ah_attr.port_num = context.na.port_num;

	if (ibv_modify_qp(remote->na.rdma_qp, &attr,
			  IBV_QP_STATE              |
			  IBV_QP_AV                 |
			  IBV_QP_PATH_MTU           |
			  IBV_QP_DEST_QPN           |
			  IBV_QP_RQ_PSN             |
			  IBV_QP_MAX_DEST_RD_ATOMIC |
			  IBV_QP_MIN_RNR_TIMER)) {
		ERROR("Failed to rdma modify QP %u to RTR", remote->na.rdma_qp->qp_num);
		goto error;
	}

	psn = rand() & 0xffffff;
	DEBUG("My psn is %u", psn);

	attr.qp_state 	    = IBV_QPS_RTS;
	attr.timeout 	    = 14;
	attr.retry_cnt 	    = 7;
	attr.rnr_retry 	    = 7;
	attr.sq_psn 	    = psn;
	attr.max_rd_atomic  = 1;
	if (ibv_modify_qp(remote->na.rdma_qp, &attr,
			  IBV_QP_STATE              |
			  IBV_QP_TIMEOUT            |
			  IBV_QP_RETRY_CNT          |
			  IBV_QP_RNR_RETRY          |
			  IBV_QP_SQ_PSN             |
			  IBV_QP_MAX_QP_RD_ATOMIC)) {
		ERROR("Failed to modify rdma QP %u to RTS", remote->na.rdma_qp->qp_num);
		goto error;
	}

	post_new_recv_buf(remote->na.rdma_qp);

	//all done? Then we're connected now :)
	remote->state = RIPC_RDMA_ESTABLISHED;

#ifdef HAVE_DEBUG
	dump_qp_state(remote->na.rdma_qp);
#endif

	//now send a reply to let the other side know our details
reply:
	/*
	 * If psn is 0, then we're re-sending a reply for an already
	 * established connection. In that case, we need to figure
	 * out the current psn and send it to the other side, or the
	 * connection state will be invalid.
	 */
	if (psn == 0) {
		struct ibv_qp_init_attr tmp_init_attr;
		ret = ibv_query_qp(remote->na.rdma_qp, &attr, IBV_QP_SQ_PSN, &tmp_init_attr);
		if (ret) {
			ERROR("Failed to query existing connection state for remote %u: %s",
					msg->src_service_id,
					strerror(ret));
		}
		psn = attr.sq_psn;
	}

	msg->lid = context.na.lid;
	msg->qpn = remote->na.rdma_qp->qp_num;
	msg->psn = psn;
	//msg->na.response_qpn = rdma_qp->qp_num;
	msg->type = RIPC_RDMA_CONN_REPLY;

	struct ibv_mr *msg_mr = used_buf_list_get(msg).na;
	assert(msg_mr);

	struct ibv_sge sge;
	sge.addr = (uint64_t)msg;
	sge.length = sizeof(struct rdma_connect_msg);
	sge.lkey = msg_mr->lkey;

	struct ibv_send_wr wr;
	wr.next = NULL;
	wr.num_sge = 1;
	wr.opcode = IBV_WR_SEND;
	wr.send_flags = IBV_SEND_SIGNALED;
	wr.sg_list = &sge;
	wr.wr_id = 0xdeadbeef;
	wr.wr.ud.ah = remote->na.ah;
	wr.wr.ud.remote_qkey = 0xffff;
	wr.wr.ud.remote_qpn = msg->response_qpn;

	struct ibv_send_wr *bad_wr;

    //holding a lock while waiting on the network is BAD(tm)
    pthread_mutex_unlock(&remotes_mutex);

        ret = ibv_post_send(unicast_service_id.na.qp, &wr, &bad_wr);
    	if (ret) {
    		ERROR("Failed to send connect response to remote %u (qp %u): %s",
				msg->src_service_id,
				msg->response_qpn,
				strerror(ret));
		goto error;
	} else {
		DEBUG("Sent rdma connect response to remote %u (qp %u), containing lid %u, qpn %u and psn %u",
				msg->src_service_id,
				msg->na.response_qpn,
				msg->lid,
				msg->qpn,
				msg->psn);
	}

	struct ibv_wc wc;

	while ( ! ibv_poll_cq(unicast_service_id.na.send_cq, 1, &wc)) { /* wait */ }

	assert(wc.status == IBV_WC_SUCCESS);

	//msg is freed in caller!
	return;

    error:
    if (remote->na.rdma_qp)
    	ibv_destroy_qp(remote->na.rdma_qp);
    if (remote->na.rdma_recv_cq)
    	ibv_destroy_cq(remote->na.rdma_recv_cq);
    if (remote->na.rdma_send_cq)
    	ibv_destroy_cq(remote->na.rdma_recv_cq);
    if (remote->na.rdma_cchannel)
    	ibv_destroy_comp_channel(remote->na.rdma_cchannel);
    remote->state = RIPC_RDMA_DISCONNECTED;
    pthread_mutex_unlock(&remotes_mutex);
}

void *start_responder(void *arg) {
	DEBUG("Allocating responder state");

	uint32_t i;
        mcast_service_id.number = 0xffff;
        unicast_service_id.na.no_cchannel = false;
	//multicast state, to wait for requests
	alloc_queue_state(&mcast_service_id);

        unicast_service_id.number = 0xffff;
        unicast_service_id.na.no_cchannel = true;
	//unicast state, to send requests
	alloc_queue_state(&unicast_service_id);

	//got all my local state, now register for multicast
	struct mcast_parameters mcg_params;
	struct ibv_port_attr port_attr;

	//mcg_params.user_mgid = NULL; //NULL means "default"
						//			0xde  ad  be  ef  ca  fe
	mcg_params.user_mgid = "255:1:0:0:222:173:190:239:202:254:0:0:0:0:0:1";
	set_multicast_gid(&mcg_params, mcast_service_id.na.qp->qp_num, 1);

	if (ibv_query_gid(context.na.device_context, 1, 0, &mcg_params.port_gid)) {
			return NULL;
	}

	if (ibv_query_pkey(context.na.device_context, 1, DEF_PKEY_IDX, &mcg_params.pkey)) {
		return NULL;
	}

	if (ibv_query_port(context.na.device_context, 1, &port_attr)) {
		return NULL;
	}
	mcg_params.ib_devname = NULL;
	mcg_params.sm_lid  = port_attr.sm_lid;
	mcg_params.sm_sl   = port_attr.sm_sl;
	mcg_params.ib_port = 1;

	DEBUG("Prepared multicast descriptor item");

	/*
	 * To do multicast, we first need to tell the fabric to create a multicast
	 * group, and then attach ourselves to it. join_multicast_group uses libumad
	 * to send the necessary management packets to create a multicast group
	 * with a random LID.
	 */
	if (join_multicast_group(SUBN_ADM_METHOD_SET,&mcg_params)) {
		ERROR(" Failed to Join Mcast request\n");
		return NULL;
	}

	DEBUG("Successfully created multicast group with LID %#x and GID %lx:%lx",
			mcg_params.mlid,
			mcg_params.mgid.global.subnet_prefix,
			mcg_params.mgid.global.interface_id);

	/*
	 * Now that our multicast group exists, we need to attach a QP (or more)
	 * to it. Every QP attached to the group will receive all packets sent to
	 * its LID.
	 */
	int ret;
	if (ret =/*=*/ ibv_attach_mcast(mcast_service_id.na.qp,&mcg_params.mgid,mcg_params.mlid)) {
		ERROR("Couldn't attach QP to MultiCast group (%s)", strerror(ret));
		return NULL;
	}

	DEBUG("Successfully attached to multicast group");

	//cache address handle used for sending requests
	struct ibv_ah_attr ah_attr;
	ah_attr.dlid = mcg_params.mlid;
	ah_attr.is_global = 1;
	ah_attr.sl = 0;
	ah_attr.port_num = context.na.port_num;
	ah_attr.grh.dgid = mcg_params.mgid;
	ah_attr.grh.sgid_index = 0;
	ah_attr.grh.hop_limit = 1;
	ah_attr.src_path_bits = 0;

	mcast_ah = ibv_create_ah(context.na.pd, &ah_attr);

	if (!mcast_ah) {
		ERROR("Failed to create resolver address handle");
		goto error;
	}

	DEBUG("Successfully created resolver address handle");

	//all done, now enter event loop
	struct ibv_cq *recvd_on;
	void *cq_context;
	struct ibv_wc wc;
	struct ibv_recv_wr *wr;
	struct ibv_send_wr resp_wr, *bad_send_wr;
	struct ibv_sge resp_sge;
	struct resolver_msg *msg, *response;
	struct ibv_mr *resp_mr;
	memset(&ah_attr, 0, sizeof(struct ibv_ah_attr)); //prepare for re-use
	bool for_us;

	//prepare a response as far as possible
	resp_mr = ripc_alloc_recv_buf(sizeof(struct resolver_msg)).na;
	response = (struct resolver_msg *)resp_mr->addr;
	response->lid = context.na.lid;
	response->type = RIPC_MSG_RESOLVE_REPLY;
	response->na.resolver_qpn = mcast_service_id.na.qp->qp_num;
	response->na.response_qpn = unicast_service_id.na.qp->qp_num;

	resp_sge.addr = (uint64_t)response;
	resp_sge.length = sizeof(struct resolver_msg);
	resp_sge.lkey = resp_mr->lkey;

	resp_wr.next = NULL;
	resp_wr.num_sge = 1;
	resp_wr.opcode = IBV_WR_SEND;
	resp_wr.send_flags = IBV_SEND_SIGNALED;
	resp_wr.sg_list = &resp_sge;
	resp_wr.wr_id = 0xdeadbeef;
	resp_wr.wr.ud.remote_qkey = 0xffff;

	pthread_mutex_unlock(&resolver_mutex); //was locked in dispatch_responder

	while(true) {
		 while ( ibv_poll_cq(mcast_service_id.na.recv_cq, 1, &wc) < 1) {
			ibv_get_cq_event(mcast_service_id.na.cchannel, &recvd_on, &cq_context);

			assert(recvd_on == mcast_service_id.na.recv_cq);

			ibv_ack_cq_events(recvd_on, 1);
			ibv_req_notify_cq(recvd_on, 0);
		}

		 assert(wc.status == IBV_WC_SUCCESS);

		wr = (struct ibv_recv_wr *) wc.wr_id;
		msg = (struct resolver_msg *)(wr->sg_list->addr + 40);

		DEBUG("Received message: from service: %u, for service: %u, from qpn: %u, from lid: %u, response to: %u",
				msg->src_service_id,
				msg->dest_service_id,
				msg->na.service_qpn,
				msg->lid,
				msg->na.response_qpn
				);

		post_new_recv_buf(mcast_service_id.na.qp);

		if (msg->type == RIPC_RDMA_CONN_REQ) {
			handle_rdma_connect((struct rdma_connect_msg *)msg);
			ripc_buf_free(msg);
			free(wr->sg_list);
			free(wr);
			continue;
		}

		//assert(msg->type == RIPC_MSG_RESOLVE_REQ);
		if (msg->type != RIPC_MSG_RESOLVE_REQ) {
			ERROR("Spurious resolver message, discarding");
			ERROR("Type: %#x, expected %#x", msg->type, RIPC_MSG_RESOLVE_REQ);
			ripc_buf_free(msg);
			free(wr->sg_list);
			free(wr);
			continue;
		}

		/*
		 * First, check if one of our own services has been requested,
		 * and if so, queue a reply for sending. We can update our own
		 * cache while the send takes place.
		 */
		for_us = false;

		pthread_mutex_lock(&services_mutex);

		if (context.services[msg->dest_service_id]) {
			//yay, it's for us!
			DEBUG("Message is for me :)");

			response->na.service_qpn =
					context.services[msg->dest_service_id]->na.qp->qp_num;

			pthread_mutex_unlock(&services_mutex);

			for_us = true;
			response->dest_service_id = msg->dest_service_id;
			response->src_service_id = msg->src_service_id;

			ah_attr.dlid = msg->lid;
			ah_attr.port_num = context.na.port_num;
			resp_wr.wr.ud.ah = ibv_create_ah(context.na.pd, &ah_attr);
			resp_wr.wr.ud.remote_qpn = msg->na.response_qpn;

			ret = ibv_post_send(unicast_service_id.na.qp, &resp_wr, &bad_send_wr);
			if (ret) {
				ERROR("Failed to post resolver reply: %s", strerror(ret));
			}
			DEBUG("Sent reply");
		} else
			pthread_mutex_unlock(&services_mutex);

		/*
		 * Now, cache the requestor's contact info, just in case we
		 * want to send him a message in the future.
		 */
		pthread_mutex_lock(&remotes_mutex);

		if (!context.remotes[msg->src_service_id]) {
			context.remotes[msg->src_service_id] =
					malloc(sizeof(struct remote_context));
			memset(context.remotes[msg->src_service_id], 0, sizeof(struct remote_context));
			context.remotes[msg->src_service_id]->state = RIPC_RDMA_DISCONNECTED;
		}

		assert(context.remotes[msg->src_service_id]);

		if ((context.remotes[msg->src_service_id]->qp_num != msg->na.service_qpn) ||
				(context.remotes[msg->src_service_id]->resolver_qp != msg->na.resolver_qpn)) {

			if (context.remotes[msg->src_service_id]->na.ah) {
				ibv_destroy_ah(context.remotes[msg->src_service_id]->na.ah);
				context.remotes[msg->src_service_id]->na.ah = NULL;
			}

			ah_attr.dlid = msg->lid;
			ah_attr.port_num = context.na.port_num;

			context.remotes[msg->src_service_id]->na.ah =
					ibv_create_ah(context.na.pd, &ah_attr);
			context.remotes[msg->src_service_id]->qp_num = msg->na.service_qpn;
			context.remotes[msg->src_service_id]->resolver_qp = msg->na.resolver_qpn;
			context.remotes[msg->src_service_id]->state = RIPC_RDMA_DISCONNECTED;

			if (context.remotes[msg->src_service_id]->na.rdma_qp) {
				ibv_destroy_qp(context.remotes[msg->src_service_id]->na.rdma_qp);
				context.remotes[msg->src_service_id]->na.rdma_qp = NULL;
			}

			if (context.remotes[msg->src_service_id]->na.rdma_send_cq) {
				ibv_destroy_cq(context.remotes[msg->src_service_id]->na.rdma_send_cq);
				context.remotes[msg->src_service_id]->na.rdma_send_cq = NULL;
			}

			if (context.remotes[msg->src_service_id]->na.rdma_recv_cq) {
				ibv_destroy_cq(context.remotes[msg->src_service_id]->na.rdma_recv_cq);
				context.remotes[msg->src_service_id]->na.rdma_recv_cq = NULL;
			}
		}

		pthread_mutex_unlock(&remotes_mutex);
		DEBUG("Cached remote contact info");

		/*
		 * Finally, poll for the send completion if necessary
		 */
		if (for_us) {
			while (!ibv_poll_cq(unicast_service_id.na.send_cq, 1, &wc)) { /* wait */ }
			DEBUG("Got send completion, result: %s", ibv_wc_status_str(wc.status));
			assert(wc.status == IBV_WC_SUCCESS);

			//we won't be needing the response ah for a while
			ibv_destroy_ah(resp_wr.wr.ud.ah);
		}

		ripc_buf_free(msg);
		free(wr->sg_list);
		free(wr);
	}

	return NULL;

	error:
	if (mcast_service_id.na.qp)
		ibv_destroy_qp(mcast_service_id.na.qp);
	if (mcast_service_id.na.recv_cq)
		ibv_destroy_cq(mcast_service_id.na.recv_cq);
	if (mcast_service_id.na.send_cq)
		ibv_destroy_cq(mcast_service_id.na.send_cq);
	if (mcast_service_id.na.cchannel)
		ibv_destroy_comp_channel(mcast_service_id.na.cchannel);
	if (unicast_service_id.na.qp)
		ibv_destroy_qp(unicast_service_id.na.qp);
	if (unicast_service_id.na.recv_cq)
		ibv_destroy_cq(unicast_service_id.na.recv_cq);
	if (unicast_service_id.na.send_cq)
		ibv_destroy_cq(unicast_service_id.na.send_cq);
	if (mcast_ah)
		ibv_destroy_ah(mcast_ah);

	return NULL;
}

void resolver_init(void) {
	//just trampoline to the real init function in new thread
	pthread_mutex_lock(&resolver_mutex); //unlocked in start_responder
	pthread_create(&responder_thread, NULL, &start_responder, NULL);
}

void resolve(uint16_t src, uint16_t dest) {
	struct ibv_send_wr wr, *bad_wr;
	struct ibv_recv_wr *resp_wr;
	struct ibv_mr *buf_mr = NULL;
	struct ibv_wc wc;
	struct ibv_sge sge;
	struct ibv_ah_attr ah_attr;
	struct ibv_ah *tmp_ah;
	int i, ret;

	buf_mr = ripc_alloc_recv_buf(sizeof(struct resolver_msg)).na;

	if (!buf_mr) {
		ERROR("Failed to allocate multicast send mr");
		return;
	}

	DEBUG("Allocated multicast mr");
	DEBUG("Address: %p", buf_mr->addr);

	struct resolver_msg *req_msg = (struct resolver_msg *)buf_mr->addr;
	struct resolver_msg *msg;

	req_msg->type = RIPC_MSG_RESOLVE_REQ;
	req_msg->dest_service_id = dest;
	req_msg->src_service_id = src;
	req_msg->lid = context.na.lid;
	pthread_mutex_lock(&services_mutex);
	req_msg->na.service_qpn = context.services[src]->na.qp->qp_num;
	pthread_mutex_unlock(&services_mutex);
	req_msg->na.response_qpn = unicast_service_id.na.qp->qp_num;
	req_msg->na.resolver_qpn = mcast_service_id.na.qp->qp_num;

	sge.addr = (uint64_t)req_msg;
	sge.length = sizeof(struct resolver_msg);
	sge.lkey = buf_mr->lkey;

	wr.next = NULL;
	wr.num_sge = 1;
	wr.opcode = IBV_WR_SEND;
	wr.send_flags = IBV_SEND_SIGNALED;
	wr.sg_list = &sge;
	wr.wr_id = 0xdeadbeef;
	wr.wr.ud.ah = mcast_ah;
	wr.wr.ud.remote_qkey = 0xffff;
	wr.wr.ud.remote_qpn = 0xffffff;

	DEBUG("Posting multicast send request");

	/*
	 * Currently, the resolver assumes that the reply it receives belongs
	 * to the last request it sent. If multiple requests are in flight
	 * simultansously and their responses are not received in the same order
	 * as their requests, bad things will happen. We therefore allow only
	 * one request at a time in flight for now.
	 * TODO: Come up with a better solution!
	 */
	pthread_mutex_lock(&resolver_mutex);
retry:
	DEBUG("About to actually post multicast send request");
	ret = ibv_post_send(mcast_service_id.na.qp, &wr, &bad_wr);
	if (ret != 0) {
		ERROR("Failed to post multicast send request: %s",strerror(ret));
		return;
	}

	DEBUG("Posted multicast send request");

	while ( ! ibv_poll_cq(mcast_service_id.na.send_cq, 1, &wc)) { /* wait */ }

	assert(wc.status == IBV_WC_SUCCESS);

	DEBUG("Got multicast send completion");

	DEBUG("Multicast message contents: from service: %u, for service: %u, from qpn: %u, from lid: %u, response to: %u",
			req_msg->src_service_id,
			req_msg->dest_service_id,
			req_msg->na.service_qpn,
			req_msg->lid,
			req_msg->na.response_qpn
			);

	DEBUG("Successfully sent multicast request, now waiting for reply on qp %u",
			unicast_service_id.na.qp->qp_num);

	/*
	 * Processing of the request shouldn't take long, so we just spin.
	 * TODO: Implement some sort of timeout!
	 */
	i = 0;
keep_waiting:
	while ( ! ibv_poll_cq(unicast_service_id.na.recv_cq, 1, &wc)) {
		if (i++ > 100000000)
			goto retry;
	}

	assert(wc.status == IBV_WC_SUCCESS);

	post_new_recv_buf(unicast_service_id.na.qp);

	resp_wr = (struct ibv_recv_wr *)wc.wr_id;
	msg = (struct resolver_msg *)(resp_wr->sg_list->addr + 40);

	assert(msg->type == RIPC_MSG_RESOLVE_REPLY);

	DEBUG("Received message: from service: %u, for service: %u, from qpn: %u, from lid: %u, response to: %u",
			msg->src_service_id,
			msg->dest_service_id,
			msg->na.service_qpn,
			msg->lid,
			msg->na.response_qpn
			);

	//assert(msg->dest_service_id == dest);
	if (msg->dest_service_id != dest) {
		DEBUG("Stale resolver response!");
		ripc_buf_free(msg);
		goto keep_waiting;
	}

	pthread_mutex_unlock(&resolver_mutex);

	ripc_buf_free(req_msg);

	 //got the info we wanted, now feed it to the cache
	ah_attr.dlid = msg->lid;
	ah_attr.port_num = context.na.port_num;
	tmp_ah = ibv_create_ah(context.na.pd, &ah_attr);

	pthread_mutex_lock(&remotes_mutex);

	if (!context.remotes[dest]) {
		context.remotes[dest] = malloc(sizeof(struct remote_context));
		memset(context.remotes[dest], 0, sizeof(struct remote_context));
		context.remotes[dest]->state = RIPC_RDMA_DISCONNECTED;
	}
	context.remotes[dest]->na.ah = tmp_ah;
	context.remotes[dest]->qp_num = msg->na.service_qpn;
	context.remotes[dest]->resolver_qp = msg->na.resolver_qpn;

	pthread_mutex_unlock(&remotes_mutex);

	ripc_buf_free(msg);
}
