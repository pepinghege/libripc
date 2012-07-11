/*  Copyright 2011, 2012 Jens Kehne
 *  Copyright 2012 Jan Stoess, Karlsruhe Institute of Technology
 *  Copyright 2012 Felix Pepinghege
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

#ifndef __INFINIBAND__RESOLVER_H__
#define __INFINIBAND__RESOLVER_H__

struct netarch_resolver_msg {
	uint16_t lid;
	uint32_t service_qpn;
	uint32_t response_qpn;
	uint32_t resolver_qpn;
};

#endif /* !__INFINIBAND__RESOLVER_H__ */
