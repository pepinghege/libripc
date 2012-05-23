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
#include <../common.h>
#include <../infiniband/common.h>
#include <../resources.h>
#include <memory.h>

struct service_id rdma_service_id;
pthread_mutex_t rdma_connect_mutex;

void alloc_queue_state(struct service_id *service_id) {
	uint32_t i;

	if (!service_id->na.no_cchannel) { //completion channel is optional
		service_id->na.cchannel = ibv_create_comp_channel(context.na.device_context);
		if (!service_id->na.cchannel) {
			ERROR("Failed to allocate completion event channel!");
			goto error;
			return;
		} else {
			DEBUG("Allocated completion event channel.");
		}
	}

	service_id->na.recv_cq = siw_create_cq(	context.na.device_context,
						100,
						service_id->na.nocchannel ? NULL : service_id->na.cchannel,
						0);
	if (!service_id->na.recv_cq) {
		ERROR("Failed to allocate receive completion queue!");
		goto error;
		return;
	} else {
		DEBUG("Allocated receive completion queue: %u", (service->id->na.recv_cq)->handle);
	}

	siw_notify_cq(service_id->na.recv_cq, 0);

	struct ibv_qp_init_attr init_attr = {
		.send_cq	= service_id->na.send_cq,
		.recv_cq	= service_id->na.recv_cq,
		.cap		= {
					.max_send_wr	= NUM_RECV_BUFFERS * 200,
					.max_recv_wr	= NUM_RECV_BUFFERS * 200,
					.max_send_sge	= 10,
					.max_recv_sge	= 10
				},
		.qp_type	= IBV_QPT_UD,
		.sq_sig_all	= 0
	};

	service_id->na.qp = siw_create_qp(context.na.pd, &init_attr);
	if (!service_id->na.qp) {
		ERROR("Failed to allocate queue pair!");
		goto error;
		return;
	} else {
		DEBUG("Allocated queue pair: %u", (service_id->na.qp)->qp_num);
	}

	struct ibv_qp_attr attr;
	attr.qp_state	= IBV_QPS_INIT;
	attr.pkey_index	= 0;
	attr.port_num	= context.na.port_num;
	attr.qkey	= service_id->number;

	if (siw_modify_qp(	service_id->na.qp,
				&attr,
				IBV_QP_STATE
					| IBV_QP_PKEY_INDEX
					| IBV_QP_PORT
					| IBV_QP_QKEY)) {
		ERROR("Failed to modify QP state to INIT");
		goto error;
		return;
	} else {
		DEBUG("Modified state of QP %u to INIT.", (service_id->na.qp)->qp_num);
	}

	attr.qp_state = IBV_QPS_RTR;
	if (siw_modify_qp(service_id->na.qp, &attr, IBV_QP_STATE)) {
		ERROR("Failed to modify QP state to RTR");
		goto error;
		return;
	} else {
		DEBUG("Modified state of QP %u to RTR", (service_id->na.qp)->qp_num);
	}

	attr.qp_state	= IBV_QPS_RTS;
	attr.sq_psn	= 0;
	if (siw_modify_qp(service_id->na.qp, &attr, (IBV_QP_STATE | IBV_QP_SQ_PSN))) {
		ERROR("Failed to modify QP state to RTS");
		goto error;
		return;
	} else {
		DEBUG("Modified state of QP %u to RTS", (service_id->na.qp)->qp_num);
	}

#ifdef HAVE_DEBUG
	siw_query_qp(service_id->na.qp, &attr, ~0, &init_attr);
	DEBUG("qkey of QP %u is %#x", (service_id->na.qp)->qp_num, attr.qkey);
#endif

	for (i = 0; i < NUM_RECV_BUFFERS; ++i) {
		post_new_recv_buf(service_id->na.qp);
	}

	return;

error:
        if (service_id->na.qp) {
                siw_destroy_qp(service_id->na.qp);
                service_id->na.qp = NULL;
        }
        if (service_id->na.recv_cq) {
                siw_destroy_cq(service_id->na.recv_cq);
                service_id->na.recv_cq = NULL;
        }
        if (service_id->na.send_cq) {
                siw_destroy_cq(service_id->na.send_cq);
                service_id->na.send_cq = NULL;
        }
        if (service_id->na.cchannel) {
                siw_destroy_comp_channel(service_id->na.cchannel);
                service_id->na.cchannel = NULL;
        }

}
