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
#include <time.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <ripc.h>
#include <common.h>
#include <memory.h>
#include <resolver.h>
#include <resources.h>

struct library_context context;

pthread_mutex_t services_mutex, remotes_mutex;

uint8_t init() {
    
	DEBUG("init %d", context.initialized);
        if (context.initialized)
            return true;

	srand(time(NULL));

        memset(context.services, 0, UINT16_MAX * sizeof(struct service_id *));
	memset(context.remotes, 0, UINT16_MAX * sizeof(struct remote_context *));

        netarch_init();
                
        pthread_mutex_init(&services_mutex, NULL);
	pthread_mutex_init(&remotes_mutex, NULL);
	pthread_mutex_init(&used_list_mutex, NULL);
	pthread_mutex_init(&free_list_mutex, NULL);
	pthread_mutex_init(&recv_window_mutex, NULL);
	pthread_mutex_init(&rdma_connect_mutex, NULL);
	pthread_mutex_init(&resolver_mutex, NULL);

	conn_mgmt_init();
	/*
	 * Those wierd-looking locking-behaviour makes sure that our connection-request-handler
	 * is established before we answer resolving requests. If we don't do that, it may happen
	 * that we answer a resolving request before we know the port on which we listen for
	 * connection requests.
	 */
	pthread_mutex_lock(&rdma_connect_mutex);
	pthread_mutex_unlock(&rdma_connect_mutex);

        resolver_init();

       
        return 0;

}

uint16_t ripc_register_random_service_id(void) {

	init();

	uint16_t service_id;

	do { //try to find a free service id
		service_id = rand() % UINT16_MAX;
	} while (ripc_register_service_id(service_id) == false);

	return service_id;
}

uint8_t ripc_register_service_id(int service_id) {

	init();
	DEBUG("Allocating service ID %u", service_id);

	struct service_id *service_context;
	uint32_t i;

	pthread_mutex_lock(&services_mutex);

	if (context.services[service_id] != NULL) {
		pthread_mutex_unlock(&services_mutex);
		return false; //already allocated
	}

	context.services[service_id] =
		(struct service_id *)malloc(sizeof(struct service_id));
	memset(context.services[service_id],0,sizeof(struct service_id));
	service_context = context.services[service_id];

	service_context->number = service_id;
        
	alloc_queue_state(service_context);

	pthread_mutex_unlock(&services_mutex);
	return true;
}
