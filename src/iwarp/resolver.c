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
#include <errno.h>
#include <sys/socket.h>

pthread_t resolver_thread;
pthread_mutex_t resolver_mutex;

void resolver_init(void) {
	pthread_create(&resolver_thread, NULL, &start_responder, NULL);
}

void *start_responder(void *arg) {
	DEBUG("Start responder.");

	assert(context.na.ip_addr > 0);
	assert(context.na.conn_listen_port > 0);

	int sock;
	struct sockaddr_in listen_addr, client_addr;
	struct resolver_msg req, resp;
	struct remote_context *remote	= NULL;
	socklen_t addr_len		= sizeof(listen_addr);
	socklen_t client_len		= sizeof(listen_addr);
	size_t msg_len			= sizeof(req);
	
	//Prepare response message as far as we can
	resp.type			= RIPC_MSG_RESOLVE_REPLY;
	resp.na.ip_addr			= context.na.ip_addr;
	resp.na.conn_port		= context.na.conn_listen_port;
	resp.na.answer_port		= 0;

	//Prepare socket
	sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	memset(&listen_addr, 0, addr_len);
	listen_addr.sin_family		= AF_INET;
	listen_addr.sin_addr.s_addr	= INADDR_ANY;
	listen_addr.sin_port		= htons(RESOLVER_BCAST_PORT);
	if (bind(sock, (struct sockaddr*) &listen_addr, addr_len) < 0) {
		DEBUG("Could not bind resolver listening socket");
		return 0;
		//panic("Could not bind resolver listening socket");
	} else
		DEBUG("Bound to listen to any address.");

	while (true) {
		in_addr_t client_ip;
		uint16_t client_resolver_port;
		uint16_t client_rdma_port;
		uint16_t client_msg_port;
		memset(&client_addr, 0, addr_len);
		DEBUG("Waiting for resolver request");
		if (recvfrom(sock, &req, msg_len, 0, (struct sockaddr*) &client_addr, &client_len) < 0) {
			int err = errno;
			DEBUG("Error while receiving broadcast message. Code %d (%s).", err, strerror(err));
			continue;
		} else {
			DEBUG(	"Received resolver message for service %hu from service %hu (%x:%u)", 
				req.dest_service_id, req.src_service_id,
				ntohl(client_addr.sin_addr.s_addr), ntohs(client_addr.sin_port));
		}

		if (req.type != RIPC_MSG_RESOLVE_REQ) {
			DEBUG("Received broadcast message with unfitting type. Ignoring.");
			continue;
		}

		if (client_len) {
			client_ip		= req.na.ip_addr	? req.na.ip_addr	: client_addr.sin_addr.s_addr;
			client_resolver_port	= req.na.answer_port	? req.na.answer_port	: ntohs(client_addr.sin_port);
		} else {
			client_ip		= req.na.ip_addr;
			client_resolver_port	= req.na.answer_port;
		}
		client_rdma_port		= req.na.conn_port;
		client_msg_port			= req.na.msg_port;

		if (!context.services[req.dest_service_id])
			goto cache_resolve_data;

		//If we are here, then the received message was meant for us
		//NOTICE: We are using src and dest from the resolve-initiators point of view!
		resp.dest_service_id		= req.dest_service_id;
		resp.src_service_id		= req.src_service_id;
		resp.na.msg_port		= context.na.msg_listen_port;
		
		client_addr.sin_addr.s_addr	= client_ip;
		client_addr.sin_port		= htons(client_resolver_port);

		DEBUG("Sending resolver reply");
		if (sendto(sock, &resp, msg_len, 0, (struct sockaddr*) &client_addr, addr_len) < 0) {
			int err = errno;
			DEBUG("Could not send resolver response. Code %d (%s).", err, strerror(err));
		}

cache_resolve_data:
		pthread_mutex_lock(&remotes_mutex);
		remote = context.remotes[req.src_service_id];
		if (!remote) {
			DEBUG("Allocating remote context");
			remote = alloc_remote_context();
		} else if (remote->state != RIPC_RDMA_DISCONNECTED
				&& remote->na.ip_addr != 0
				&& remote->na.ip_addr != client_ip) {
			/*
			 * In this case we have a rdma-connection (or we are currently establishing one), which is
			 * no longer valid, as the remote has a new ip-address, which has turned down the TCP
			 * connection. So we strip the remote context, what in turn will lead to a new connection
			 * establishment on the new ip.
			 */
			DEBUG("Remote service now has a different IP - Stripping remote context.");
			strip_remote_context(remote);
		}
		remote->na.ip_addr			= client_ip;
		remote->na.rdma_listen_port		= client_rdma_port;
		remote->na.msg_port			= client_msg_port;

		context.remotes[req.src_service_id]	= remote;
		pthread_mutex_unlock(&remotes_mutex);
	}	

	return NULL;
}

void resolve(uint16_t src, uint16_t dest) {
	DEBUG("Resolving: src: %hu, dest: %hu.", src, dest);

	assert(context.na.bcast_ip_addr > 0);
	assert(context.na.ip_addr > 0);
	assert(context.services[src]);

	int sock;
	size_t timeouts, retries;
	size_t max_retries		= 5;
	size_t max_timeouts		= 3;
	struct timeval timeout		= {
		.tv_sec		= 5,
		.tv_usec	= 0
	};
	int sock_option;
	struct remote_context *remote;
	struct resolver_msg req, resp;
	struct sockaddr_in bcast_addr, resp_addr, my_addr;
	size_t msg_len			= sizeof(req);
	socklen_t addr_len		= sizeof(bcast_addr);

	memset(&bcast_addr, 0, addr_len);
	bcast_addr.sin_family		= AF_INET;
	bcast_addr.sin_addr.s_addr	= context.na.bcast_ip_addr;
	bcast_addr.sin_port		= htons(RESOLVER_BCAST_PORT);

	memset(&my_addr, 0, addr_len);

	memset(&resp_addr, 0, addr_len);
	my_addr.sin_family		= AF_INET;
	my_addr.sin_addr.s_addr		= INADDR_ANY;

	sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if ((bind(sock, (struct sockaddr*) &my_addr, addr_len) < 0)) {
		int err = errno;
		panic("Could not bind to broadcast address. Code %d (%s).", err, strerror(err));
	}

	if ((getsockname(sock, (struct sockaddr*) &my_addr, &addr_len) < 0)) {
		int err = errno;
		panic("Could not get the port on which we bound. Code %d (%s)", err, strerror(err));
	} else {
		DEBUG("Bound to port %hu", ntohs(my_addr.sin_port));
	}

	if (setsockopt(sock, SOL_SOCKET, SO_BROADCAST, (void *) &sock_option, sizeof(sock_option)) < 0) {
		int err = errno;
		pthread_mutex_unlock(&resolver_mutex);
		panic("Could not set socket to send broadcast messages. Code %d (%s).", err, strerror(err));
	}

	if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (void*) &timeout, sizeof(timeout)) < 0) {
		int err = errno;
		panic("Could not set receiving timeout of socket. Code %d (%s).", err, strerror(err));
	}

	req.type			= RIPC_MSG_RESOLVE_REQ;
	req.dest_service_id		= dest;
	req.src_service_id		= src;
	req.na.ip_addr			= context.na.ip_addr;
	req.na.conn_port		= context.na.conn_listen_port;
	req.na.answer_port		= ntohs(my_addr.sin_port);
	req.na.msg_port			= context.na.msg_listen_port;

	memset(&resp, 0, msg_len);

	sock_option = 1;
	retries = -1;
	pthread_mutex_lock(&resolver_mutex);
retry:
	retries++;

	DEBUG("Sending resolver message to broadcast-address %x:%u", ntohl(bcast_addr.sin_addr.s_addr), ntohs(bcast_addr.sin_port));
	if (sendto(sock, &req, msg_len, 0, (struct sockaddr*) &bcast_addr, addr_len) < 0) {
		int err = errno;
		pthread_mutex_unlock(&resolver_mutex);
		DEBUG("Could not send message to broadcast address. Code %d (%s).", err, strerror(err));
		return;
	}

keep_waiting:
	for (timeouts = 0; timeouts < max_timeouts; /* NOP */) {
		DEBUG("Waiting for resolver answer");
		if (recvfrom(sock, &resp, msg_len, 0, (struct sockaddr*) &resp_addr, &addr_len) < 0) 
			DEBUG("Hit %zu. timeout while waiting for resolver reply.", ++timeouts);
		else
			break;
	}

	if ((timeouts >= max_timeouts) && (retries < max_retries)) {
		goto retry;
	} else if (retries >= max_retries) {
		pthread_mutex_unlock(&resolver_mutex);
		DEBUG("No resolver-reply after %zu retries.", retries);
		return;
	}

	//If we are here, we received an answer
	//NOTICE: service_ids are always named from the resolve-initiatiors (our) point of view
	if (resp.dest_service_id != dest) {
		DEBUG("Fetched a wrong message.");
		goto keep_waiting;
	}
	DEBUG(	"Recevied message from service-id %u. Connection data (IP:Port): %x:%u",
		resp.dest_service_id, ntohl(resp.na.ip_addr), resp.na.conn_port);
	pthread_mutex_unlock(&resolver_mutex);
		
	pthread_mutex_lock(&remotes_mutex);
	remote = context.remotes[dest];
	if (!remote) {
		remote = alloc_remote_context();
	} else if (remote->state != RIPC_RDMA_DISCONNECTED
			&& remote->na.ip_addr != 0
			&& remote->na.ip_addr != resp.na.ip_addr) {
		strip_remote_context(remote);
	}
	remote->na.ip_addr		= resp.na.ip_addr;
	remote->na.rdma_listen_port	= resp.na.conn_port;
	remote->na.msg_port		= resp.na.msg_port;

	context.remotes[dest] = remote;
	DEBUG("Set remote context for service %u", dest);
	pthread_mutex_unlock(&remotes_mutex);
}
