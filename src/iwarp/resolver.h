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
#ifndef RESOLVER_H_
#include "../resolver.h"
#endif

#ifndef __IWARP__RESOLVER_H__
#define __IWARP__RESOLVER_H__

//TODO:	What do we do if two processes start libRIPC on the same machine (i.e. with the same IP)
#define	RESOLVER_BCAST_PORT	15510

struct netarch_resolver_msg {
	in_addr_t ip_addr;	//IP of the sender
	uint16_t conn_port;	//Port on which the connection-thread is listening
	uint16_t msg_port;	//Port on which the receiver is listening
	//The following member is optional except for resolve requests.
	uint16_t answer_port;	//Port for resolve-answer
};

void *start_responder(void *arg);

#endif /* !__IWARP__RESOLVER_H__ */
