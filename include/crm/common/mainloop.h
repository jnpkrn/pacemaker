/* 
 * Copyright (C) 2009 Andrew Beekhof <andrew@beekhof.net>
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 * 
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */
#ifndef CRM_COMMON_MAINLOOP__H
#  define CRM_COMMON_MAINLOOP__H

#  include <glib.h>

typedef struct trigger_s crm_trigger_t;

extern crm_trigger_t *mainloop_add_trigger(int priority, int(*dispatch) (gpointer user_data),
                                           gpointer userdata);

extern void mainloop_set_trigger(crm_trigger_t * source);

extern void mainloop_trigger_complete(crm_trigger_t *trig);

extern gboolean mainloop_destroy_trigger(crm_trigger_t * source);

extern gboolean crm_signal(int sig, void (*dispatch) (int sig));

extern gboolean mainloop_add_signal(int sig, void (*dispatch) (int sig));

extern gboolean mainloop_destroy_signal(int sig);

#include <crm/common/ipc.h>

struct ipc_client_callbacks 
{
        int (*dispatch)(const char *buffer, ssize_t length, gpointer userdata);
        void (*destroy) (gpointer);
};

qb_ipcs_service_t *mainloop_add_ipc_server(
    const char *name, enum qb_ipc_type type, struct qb_ipcs_service_handlers *callbacks);

void mainloop_del_ipc_server(qb_ipcs_service_t *server);

typedef struct mainloop_io_s mainloop_io_t;

mainloop_io_t *mainloop_add_ipc_client(
    const char *name, size_t max_size, void *userdata, struct ipc_client_callbacks *callbacks);

void mainloop_del_ipc_client(mainloop_io_t *client);

crm_ipc_t *mainloop_get_ipc_client(mainloop_io_t *client);


struct mainloop_fd_callbacks 
{
        int (*dispatch)(gpointer userdata);
        void (*destroy)(gpointer userdata);
};

mainloop_io_t *mainloop_add_fd(
    const char *name, int fd, void *userdata, struct mainloop_fd_callbacks *callbacks);

void mainloop_del_fd(mainloop_io_t *client);

#endif
