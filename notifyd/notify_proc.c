/*
 * Copyright (c) 2003-2010 Apple Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */

#include "notifyd.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <asl.h>
#include <bsm/libbsm.h>
#include <mach/mach.h>
#include <mach/mach_error.h>
#include <mach/mach_traps.h>
#include <sys/sysctl.h>
#include <pthread.h>
#include <sys/fcntl.h>
#include <dispatch/private.h>
#include "notify.h"
#include "service.h"
#include "notify_ipc.h"
#include "notify_ipcServer.h"

static void
cancel_subscription(client_t *c)
{
	name_info_t *n;
	int token;

	if (global.notify_state == NULL) return;
	if (c == NULL) return;

	call_statistics.cleanup++;

	token = (int)c->client_id;

	if (c->private != NULL)
	{
		service_close(c->private);
		c->private = NULL;
	}

	n = c->name_info;
	if ((n != NULL) && (n->refcount == 1) && (n->private != NULL))
	{
		service_close(n->private);
		n->private = NULL;
	}

	_notify_lib_port_proc_release(global.notify_state, MACH_PORT_NULL, c->pid);

	if (c->notify_type == NOTIFY_TYPE_MEMORY)
	{
		global.shared_memory_refcount[n->slot]--;
	}
	else if (c->notify_type == NOTIFY_TYPE_PORT)
	{
		_notify_lib_port_proc_release(global.notify_state, c->port, 0);
	}

	_notify_lib_cancel(global.notify_state, c->pid, token);
}

static void
cancel_proc(void *px)
{
	void *tt;
	long lpid = (long)px;
	pid_t pid;
	client_t *c;
	list_t *l, *x;

	if (global.notify_state == NULL) return;

	pid = (pid_t)lpid;
	x = NULL;

	tt = _nc_table_traverse_start(global.notify_state->client_table);
	while (tt != NULL)
	{
		c = _nc_table_traverse(global.notify_state->client_table, tt);
		if (c == NULL) break;

		if (c->pid == pid) x = _nc_list_prepend(x, _nc_list_new(c));
	}
	_nc_table_traverse_end(global.notify_state->client_table, tt);

	for (l = x; l != NULL; l = _nc_list_next(l))
	{
		c = _nc_list_data(l);
		cancel_subscription(c);
	}

	_nc_list_free_list(x);
}

static void
cancel_port(mach_port_t port, dispatch_source_t src)
{
	void *tt;
	client_t *c;
	list_t *l, *x;

	if (global.notify_state == NULL) return;

	x = NULL;

	tt = _nc_table_traverse_start(global.notify_state->client_table);
	while (tt != NULL)
	{
		c = _nc_table_traverse(global.notify_state->client_table, tt);
		if (c == NULL) break;

		if (c->port == port) x = _nc_list_prepend(x, _nc_list_new(c));
	}
	_nc_table_traverse_end(global.notify_state->client_table, tt);

	for (l = x; l != NULL; l = _nc_list_next(l))
	{
		c = _nc_list_data(l);
		cancel_subscription(c);
	}

	_nc_list_free_list(x);
}

static void
port_event(void *px)
{
	long lport = (long)px;
	mach_port_t port;
	unsigned long data;
	portproc_data_t *pp;

	if (global.notify_state == NULL) return;

	port = (mach_port_t)lport;
	if (port == MACH_PORT_NULL) return;

	pp = _notify_lib_port_proc_find(global.notify_state, port, 0);
	if (pp == NULL)
	{
		log_message(ASL_LEVEL_DEBUG, "can't find port source for %u\n", port);
		return;
	}

	data = dispatch_source_get_data(pp->src);

	if (data & DISPATCH_MACH_SEND_DEAD)
	{
		cancel_port(port, pp->src);
	}
	else if (data & DISPATCH_MACH_SEND_POSSIBLE)
	{
		_notify_lib_resume_port(global.notify_state, port);
	}
	else
	{
		log_message(ASL_LEVEL_DEBUG, "unknown data 0x%lx for %u\n", data, port);
	}

	_notify_lib_port_proc_release(global.notify_state, port, 0);
}

static void
port_dealloc(void *px)
{
	long lport = (long)px;
	mach_port_t port = (mach_port_t)lport;

	if (port == MACH_PORT_NULL) return;
	mach_port_deallocate(mach_task_self(), port);
}

static void
port_registration_complete(void *px)
{
	long lport = (long)px;
	mach_port_t port;

	if (global.notify_state == NULL) return;

	port = (mach_port_t)lport;
	_notify_lib_resume_port(global.notify_state, port);
}

static void
register_pid(pid_t pid)
{
	dispatch_source_t src;
	long lpid;
	portproc_data_t *pp;

	if (global.notify_state == NULL) return;
	if (pid <= 0) return;

	/*
	 * Check if this pid has already registered.
	 * N.B. This call retains the portproc_data_t.  We want that.
	 */
	pp = _notify_lib_port_proc_find(global.notify_state, MACH_PORT_NULL, pid);
	if (pp != NULL) return;

	src = dispatch_source_create(DISPATCH_SOURCE_TYPE_PROC, pid, DISPATCH_PROC_EXIT, global.workloop);
	dispatch_source_set_event_handler_f(src, (dispatch_function_t)cancel_proc);

	lpid = pid;
	dispatch_set_context(src, (void *)lpid);

	_notify_lib_port_proc_new(global.notify_state, MACH_PORT_NULL, pid, 0, src);

	dispatch_activate(src);
}

static void
register_port(client_t *c)
{
	dispatch_source_t src;
	long lport;
	portproc_data_t *pp;

	if (c == NULL) return;

	/* ignore MACH_PORT_DEAD */
	if (c->port == MACH_PORT_DEAD) return;

	/*
	 * Check if this port has already registered.
	 * N.B. This call retains the portproc_data_t.  We want that.
	 */
	pp = _notify_lib_port_proc_find(global.notify_state, c->port, 0);
	if (pp != NULL) return;

	src = dispatch_source_create(DISPATCH_SOURCE_TYPE_MACH_SEND, c->port, DISPATCH_MACH_SEND_DEAD | DISPATCH_MACH_SEND_POSSIBLE, global.workloop);

	dispatch_source_set_event_handler_f(src, (dispatch_function_t)port_event);

	/* retain send right for port - port_dealloc() will release when the source goes away */
	mach_port_mod_refs(mach_task_self(), c->port, MACH_PORT_RIGHT_SEND, +1);
	dispatch_source_set_cancel_handler_f(src, (dispatch_function_t)port_dealloc);

	lport = c->port;
	dispatch_set_context(src, (void *)lport);

	dispatch_source_set_registration_handler_f(src, (dispatch_function_t)port_registration_complete);

	_notify_lib_port_proc_new(global.notify_state, c->port, 0, NOTIFY_PORT_PROC_STATE_SUSPENDED, src);

	dispatch_activate(src);
}

static uint32_t
string_validate(caddr_t path, mach_msg_type_number_t pathCnt)
{
	if (path == NULL && pathCnt != 0) {
		return NOTIFY_STATUS_INVALID_REQUEST;
	}
	if (path && (pathCnt == 0 || path[pathCnt - 1] != '\0')) {
		return NOTIFY_STATUS_INVALID_REQUEST;
	}
	return NOTIFY_STATUS_OK;
}

static uint32_t
server_preflight(audit_token_t audit, int token, uid_t *uid, gid_t *gid, pid_t *pid, uint64_t *cid)
{
	pid_t xpid;

	if (global.notify_state == NULL)
	{
		return NOTIFY_STATUS_FAILED;
	}

	audit_token_to_au32(audit, NULL, uid, gid, NULL, NULL, &xpid, NULL, NULL);
	if (pid != NULL) *pid = xpid;

	if (token > 0)
	{
		client_t *c;
		uint64_t xcid = make_client_id(xpid, token);
		if (cid != NULL) *cid = xcid;

		c = _nc_table_find_64(global.notify_state->client_table, xcid);
		if (c != NULL)
		{
			/* duplicate tokens can occur if a process exec()s */
			log_message(ASL_LEVEL_DEBUG, "duplicate token %d sent from PID %d\n", token, xpid);
			cancel_subscription(c);
		}
	}

	return NOTIFY_STATUS_OK;
}

kern_return_t __notify_server_post_3
(
	mach_port_t server,
	uint64_t name_id,
	boolean_t claim_root_access,
	audit_token_t audit
)
{
	uid_t uid = (uid_t)-1;
	gid_t gid = (gid_t)-1;
	pid_t pid = (pid_t)-1;
	int status;
	name_info_t *n;

	if (global.notify_state == NULL) return KERN_SUCCESS;

	n = (name_info_t *)_nc_table_find_64(global.notify_state->name_id_table, name_id);
	if (n == NULL) return KERN_SUCCESS;

	n->postcount++;

	status = server_preflight(audit, -1, &uid, &gid, &pid, NULL);
	if (status != NOTIFY_STATUS_OK) return KERN_SUCCESS;
	
	if ((uid != 0) && claim_root_access && has_root_entitlement(pid)) uid = 0;

	status = _notify_lib_check_controlled_access(global.notify_state, n->name, uid, gid, NOTIFY_ACCESS_WRITE);
	if (status != NOTIFY_STATUS_OK) return KERN_SUCCESS;

	call_statistics.post++;
	call_statistics.post_by_id++;

	log_message(ASL_LEVEL_DEBUG, "__notify_server_post name_id %llu [%s]\n", name_id, n->name);

	daemon_post_nid(name_id, uid, gid);
	return KERN_SUCCESS;
}

kern_return_t __notify_server_post_2
(
	mach_port_t server,
	caddr_t name,
	uint64_t *name_id,
	int *status,
	boolean_t claim_root_access,
	audit_token_t audit
)
{
	uid_t uid = (uid_t)-1;
	gid_t gid = (gid_t)-1;
	pid_t pid = (pid_t)-1;
	name_info_t *n;

	*name_id = 0;

	*status = server_preflight(audit, -1, &uid, &gid, &pid, NULL);
	if (*status != NOTIFY_STATUS_OK) return KERN_SUCCESS;

	if ((uid != 0) && claim_root_access && has_root_entitlement(pid)) uid = 0;

	*status = _notify_lib_check_controlled_access(global.notify_state, name, uid, gid, NOTIFY_ACCESS_WRITE);
	if (*status != NOTIFY_STATUS_OK)
	{
		return KERN_SUCCESS;
	}

	call_statistics.post++;
	call_statistics.post_by_name_and_fetch_id++;
	
	n = NULL;
	*status = daemon_post(name, uid, gid);
	if (*status == NOTIFY_STATUS_OK)
	{
		n = (name_info_t *)_nc_table_find(global.notify_state->name_table, name);
	}

	if (n == NULL)
	{
		*status = NOTIFY_STATUS_INVALID_NAME;
		*name_id = UINT64_MAX;
		call_statistics.post_no_op++;
	}
	else
	{
		n->postcount++;
		*name_id = n->name_id;
	}

	if (*name_id == UINT64_MAX) log_message(ASL_LEVEL_DEBUG, "__notify_server_post %s\n", name);
	else log_message(ASL_LEVEL_DEBUG, "__notify_server_post %s [%llu]\n", name, *name_id);

	return KERN_SUCCESS;
}

kern_return_t __notify_server_post_4
(
	mach_port_t server,
	caddr_t name,
	boolean_t claim_root_access,
	audit_token_t audit
)
{
	uint64_t ignored_name_id;
	int ignored_status;
	kern_return_t kstatus;

	kstatus = __notify_server_post_2(server, name, &ignored_name_id, &ignored_status, claim_root_access, audit);

	call_statistics.post_by_name_and_fetch_id--;
	call_statistics.post_by_name++;

	return kstatus;
}

kern_return_t __notify_server_register_plain_2
(
	mach_port_t server,
	caddr_t name,
	int token,
	audit_token_t audit
)
{
	client_t *c;
	uint64_t nid, cid;
	uint32_t status;
	uid_t uid = (uid_t)-1;
	gid_t gid = (gid_t)-1;
	pid_t pid = (pid_t)-1;

	call_statistics.reg++;
	call_statistics.reg_plain++;

	status = server_preflight(audit, token, &uid, &gid, &pid, &cid);
	if (status != NOTIFY_STATUS_OK) return KERN_SUCCESS;

	log_message(ASL_LEVEL_DEBUG, "__notify_server_register_plain %s %d %d\n", name, pid, token);

	status = _notify_lib_register_plain(global.notify_state, name, pid, token, -1, uid, gid, &nid);
	if (status != NOTIFY_STATUS_OK)
	{
		return KERN_SUCCESS;
	}

	c = _nc_table_find_64(global.notify_state->client_table, cid);

	if (!strncmp(name, SERVICE_PREFIX, SERVICE_PREFIX_LEN)) service_open(name, c, uid, gid);

	register_pid(pid);

	return KERN_SUCCESS;
}

kern_return_t __notify_server_register_check_2
(
	mach_port_t server,
	caddr_t name,
	int token,
	int *size,
	int *slot,
	uint64_t *name_id,
	int *status,
	audit_token_t audit
)
{
	name_info_t *n;
	uint32_t i, j, x, new_slot;
	uint64_t cid;
	client_t *c;
	uid_t uid = (uid_t)-1;
	gid_t gid = (gid_t)-1;
	pid_t pid = (pid_t)-1;

	*size = 0;
	*slot = 0;
	*name_id = 0;
	*status = NOTIFY_STATUS_OK;

	*status = server_preflight(audit, token, &uid, &gid, &pid, &cid);
	if (*status != NOTIFY_STATUS_OK) return KERN_SUCCESS;

	call_statistics.reg++;
	call_statistics.reg_check++;

	if (global.nslots == 0)
	{
		*size = -1;
		*slot = -1;
		return __notify_server_register_plain_2(server, name, token, audit);
	}

	x = (uint32_t)-1;

	n = (name_info_t *)_nc_table_find(global.notify_state->name_table, name);
	if (n != NULL) x = n->slot;

	new_slot = 0;
	if (x == (uint32_t)-1) 
	{
		/* find a slot */
		new_slot = 1;

		/*
		 * Check slots beginning at the current slot_id + 1, since it's likely that the
		 * next slot will be available.  Keep looking until we have examined all the
		 * slots (skipping slot 0, which is reserved for notifyd). Stop if we find
		 * an unused (refcount == 0) slot.
		 */
		for (i = 1, j = global.slot_id + 1; i < global.nslots; i++, j++)
		{
			if (j >= global.nslots) j = 1;
			if (global.shared_memory_refcount[j] == 0)
			{
				x = j;
				break;
			}
		}

		if (x == (uint32_t)-1)
		{
			/*
			 * We did not find an unused slot.  At this point, the shared
			 * memory table is full, so we start re-using slots, beginning at 
			 * global.slot_id + 1.
			 */
			global.slot_id++;

			/* wrap around to slot 1 (slot 0 is reserved for notifyd) */
			if (global.slot_id >= global.nslots) global.slot_id = 1;
			log_message(ASL_LEVEL_DEBUG, "reused shared memory slot %u\n", global.slot_id);
			x = global.slot_id;
		}
		else
		{
			/* found a free slot */
			global.slot_id = x;
		}
	}

	if (new_slot == 1) global.shared_memory_base[x] = 1;
	global.shared_memory_refcount[x]++;

	log_message(ASL_LEVEL_DEBUG, "__notify_server_register_check %s %d %d\n", name, pid, token);

	*size = global.nslots * sizeof(uint32_t);
	*slot = x;
	*status = _notify_lib_register_plain(global.notify_state, name, pid, token, x, uid, gid, name_id);
	if (*status != NOTIFY_STATUS_OK)
	{
		return KERN_SUCCESS;
	}

	c = _nc_table_find_64(global.notify_state->client_table, cid);

	if (!strncmp(name, SERVICE_PREFIX, SERVICE_PREFIX_LEN)) service_open(name, c, uid, gid);

	register_pid(pid);

	return KERN_SUCCESS;
}

kern_return_t __notify_server_register_signal_2
(
	mach_port_t server,
	caddr_t name,
	int token,
	int sig,
	audit_token_t audit
)
{
	client_t *c;
	uint64_t name_id, cid;
	uint32_t status;
	uid_t uid = (uid_t)-1;
	gid_t gid = (gid_t)-1;
	pid_t pid = (pid_t)-1;

	status = server_preflight(audit, token, &uid, &gid, &pid, &cid);
	if (status != NOTIFY_STATUS_OK) return KERN_SUCCESS;

	call_statistics.reg++;
	call_statistics.reg_signal++;

	log_message(ASL_LEVEL_DEBUG, "__notify_server_register_signal %s %d %d %d\n", name, pid, token, sig);

	status = _notify_lib_register_signal(global.notify_state, name, pid, token, sig, uid, gid, &name_id);
	if (status != NOTIFY_STATUS_OK)
	{
		return KERN_SUCCESS;
	}

	c = _nc_table_find_64(global.notify_state->client_table, cid);

	if (!strncmp(name, SERVICE_PREFIX, SERVICE_PREFIX_LEN)) service_open(name, c, uid, gid);

	register_pid(pid);

	return KERN_SUCCESS;
}

kern_return_t __notify_server_register_file_descriptor_2
(
	mach_port_t server,
	caddr_t name,
	int token,
	fileport_t fileport,
	audit_token_t audit
)
{
	client_t *c;
	int fd, flags;
	uint32_t status;
	uint64_t name_id, cid;
	uid_t uid = (uid_t)-1;
	gid_t gid = (gid_t)-1;
	pid_t pid = (pid_t)-1;

	status = server_preflight(audit, token, &uid, &gid, &pid, &cid);
	if (status != NOTIFY_STATUS_OK) return KERN_SUCCESS;

	call_statistics.reg++;
	call_statistics.reg_file++;

	log_message(ASL_LEVEL_DEBUG, "__notify_server_register_file_descriptor %s %d %d\n", name, pid, token);

	fd = fileport_makefd(fileport);
	mach_port_deallocate(mach_task_self(), fileport);
	if (fd < 0)
	{
		return KERN_SUCCESS;
	}

	flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0)
	{
		return KERN_SUCCESS;
	}

	flags |= O_NONBLOCK;
	if (fcntl(fd, F_SETFL, flags) < 0)
	{
		return KERN_SUCCESS;
	}

	status = _notify_lib_register_file_descriptor(global.notify_state, name, pid, token, fd, uid, gid, &name_id);
	if (status != NOTIFY_STATUS_OK)
	{
		return KERN_SUCCESS;
	}

	c = _nc_table_find_64(global.notify_state->client_table, cid);

	if (!strncmp(name, SERVICE_PREFIX, SERVICE_PREFIX_LEN)) service_open(name, c, uid, gid);

	register_pid(pid);

	return KERN_SUCCESS;
}

kern_return_t __notify_server_register_mach_port_2
(
	mach_port_t server,
	caddr_t name,
	int token,
	mach_port_t port,
	audit_token_t audit
)
{
	client_t *c;
	uint64_t name_id, cid;
	uint32_t status;
	uid_t uid = (uid_t)-1;
	gid_t gid = (gid_t)-1;
	pid_t pid = (pid_t)-1;

	if (port == MACH_PORT_DEAD) return KERN_SUCCESS;

	status = server_preflight(audit, token, &uid, &gid, &pid, &cid);
	if (status != NOTIFY_STATUS_OK)
	{
		mach_port_deallocate(mach_task_self(), port);
		return KERN_SUCCESS;
	}

	call_statistics.reg++;
	call_statistics.reg_port++;

	log_message(ASL_LEVEL_DEBUG, "__notify_server_register_mach_port %s %d %d\n", name, pid, token);

	status = _notify_lib_register_mach_port(global.notify_state, name, pid, token, port, uid, gid, &name_id);
	if (status != NOTIFY_STATUS_OK)
	{
		mach_port_deallocate(mach_task_self(), port);
		return KERN_SUCCESS;
	}

	c = _nc_table_find_64(global.notify_state->client_table,cid);

	if (!strncmp(name, SERVICE_PREFIX, SERVICE_PREFIX_LEN)) service_open(name, c, uid, gid);

	register_pid(pid);
	register_port(c);

	return KERN_SUCCESS;
}

kern_return_t __notify_server_cancel_2
(
	mach_port_t server,
	int token,
	audit_token_t audit
)
{
	client_t *c;
	uid_t uid = (uid_t)-1;
	pid_t pid = (pid_t)-1;
	int status;

	status = server_preflight(audit, -1, &uid, NULL, &pid, NULL);
	if (status != NOTIFY_STATUS_OK) return KERN_SUCCESS;

	call_statistics.cancel++;

	log_message(ASL_LEVEL_DEBUG, "__notify_server_cancel %d %d\n", pid, token);

	c = _nc_table_find_64(global.notify_state->client_table, make_client_id(pid, token));
	if (c != NULL) cancel_subscription(c);

	return KERN_SUCCESS;
}

kern_return_t __notify_server_suspend
(
	mach_port_t server,
	int token,
	int *status,
	audit_token_t audit
)
{
	pid_t pid = (pid_t)-1;

	*status = server_preflight(audit, -1, NULL, NULL, &pid, NULL);
	if (*status != NOTIFY_STATUS_OK) return KERN_SUCCESS;

	call_statistics.suspend++;

	log_message(ASL_LEVEL_DEBUG, "__notify_server_suspend %d %d\n", pid, token);

	_notify_lib_suspend(global.notify_state, pid, token);
	*status = NOTIFY_STATUS_OK;

	return KERN_SUCCESS;
}

kern_return_t __notify_server_resume
(
	mach_port_t server,
	int token,
	int *status,
	audit_token_t audit
)
{
	pid_t pid = (pid_t)-1;

	*status = server_preflight(audit, -1, NULL, NULL, &pid, NULL);
	if (*status != NOTIFY_STATUS_OK) return KERN_SUCCESS;

	call_statistics.resume++;

	log_message(ASL_LEVEL_DEBUG, "__notify_server_resume %d %d\n", pid, token);

	_notify_lib_resume(global.notify_state, pid, token);
	*status = NOTIFY_STATUS_OK;

	return KERN_SUCCESS;
}

static uid_t
uid_for_pid(pid_t pid)
{
	int mib[4];
	struct kinfo_proc info;
	size_t size = sizeof(struct kinfo_proc);

	mib[0] = CTL_KERN;
	mib[1] = KERN_PROC;
	mib[2] = KERN_PROC_PID;
	mib[3] = pid;

	sysctl(mib, 4, &info, &size, 0, 0);

	return (uid_t)info.kp_eproc.e_ucred.cr_uid;
}

kern_return_t __notify_server_suspend_pid
(
	mach_port_t server,
	int pid,
	int *status,
	audit_token_t audit
)
{
	uid_t uid, target_uid;

	uid = (uid_t)-1;

	*status = server_preflight(audit, -1, &uid, NULL, NULL, NULL);
	if (*status != NOTIFY_STATUS_OK) return KERN_SUCCESS;

	call_statistics.suspend_pid++;

	log_message(ASL_LEVEL_DEBUG, "__notify_server_suspend_pid %d\n", pid);

	target_uid = uid_for_pid(pid);

	if ((uid != 0) && (target_uid != uid))
	{
		*status = NOTIFY_STATUS_NOT_AUTHORIZED;
		return KERN_SUCCESS;
	}

	*status = NOTIFY_STATUS_OK;

	_notify_lib_suspend_proc(global.notify_state, pid);

	return KERN_SUCCESS;
}

kern_return_t __notify_server_resume_pid
(
	mach_port_t server,
	int pid,
	int *status,
	audit_token_t audit
)
{
	uid_t uid, target_uid;

	uid = (uid_t)-1;

	*status = server_preflight(audit, -1, &uid, NULL, NULL, NULL);
	if (*status != NOTIFY_STATUS_OK) return KERN_SUCCESS;

	call_statistics.resume_pid++;

	log_message(ASL_LEVEL_DEBUG, "__notify_server_resume_pid %d\n", pid);

	target_uid = uid_for_pid(pid);

	if ((uid != 0) && (target_uid != uid))
	{
		*status = NOTIFY_STATUS_NOT_AUTHORIZED;
		return KERN_SUCCESS;
	}

	*status = NOTIFY_STATUS_OK;

	_notify_lib_resume_proc(global.notify_state, pid);

	return KERN_SUCCESS;
}

kern_return_t __notify_server_check
(
	mach_port_t server,
	int token,
	int *check,
	int *status,
	audit_token_t audit
)
{
	pid_t pid = (gid_t)-1;

	*check = 0;

	call_statistics.check++;

	audit_token_to_au32(audit, NULL, NULL, NULL, NULL, NULL, &pid, NULL, NULL);

	log_message(ASL_LEVEL_DEBUG, "__notify_server_check %d %d\n", pid, token);

	*status =  _notify_lib_check(global.notify_state, pid, token, check);
	return KERN_SUCCESS;
}

kern_return_t __notify_server_get_state
(
	mach_port_t server,
	int token,
	uint64_t *state,
	int *status,
	audit_token_t audit
)
{
	uid_t uid = (uid_t)-1;
	gid_t gid = (gid_t)-1;
	pid_t pid = (pid_t)-1;
	client_t *c;

	*state = 0;

	*status = server_preflight(audit, -1, &uid, &gid, &pid, NULL);
	if (*status != NOTIFY_STATUS_OK) return KERN_SUCCESS;

	call_statistics.get_state++;
	call_statistics.get_state_by_client++;

	log_message(ASL_LEVEL_DEBUG, "__notify_server_get_state %d %d\n", pid, token);

	c = _nc_table_find_64(global.notify_state->client_table, make_client_id(pid, token));
	if ((c == NULL) || (c->name_info == NULL))
	{
		*status = NOTIFY_STATUS_FAILED;
	}
	else
	{
		c->get_state_count++;
		*status = _notify_lib_get_state(global.notify_state, c->name_info->name_id, state, uid, gid);
	}

	return KERN_SUCCESS;
}

kern_return_t __notify_server_get_state_2
(
	mach_port_t server,
	uint64_t name_id,
	uint64_t *state,
	int *status,
	audit_token_t audit
)
{
	uid_t uid = (uid_t)-1;
	gid_t gid = (gid_t)-1;

	*state = 0;

	*status = server_preflight(audit, -1, &uid, &gid, NULL, NULL);
	if (*status != NOTIFY_STATUS_OK) return KERN_SUCCESS;

	log_message(ASL_LEVEL_DEBUG, "__notify_server_get_state_2 %llu\n", name_id);

	*status = _notify_lib_get_state(global.notify_state, name_id, state, uid, gid);
	return KERN_SUCCESS;
}

kern_return_t __notify_server_get_state_3
(
	mach_port_t server,
	int token,
	uint64_t *state,
	uint64_t *name_id,
	int *status,
	audit_token_t audit
)
{
	uid_t uid = (uid_t)-1;
	gid_t gid = (gid_t)-1;
	pid_t pid = (pid_t)-1;
	client_t *c;

	*state = 0;
	*name_id = 0;

	*status = server_preflight(audit, -1, &uid, &gid, &pid, NULL);
	if (*status != NOTIFY_STATUS_OK) return KERN_SUCCESS;

	call_statistics.get_state++;
	call_statistics.get_state_by_client_and_fetch_id++;

	c = _nc_table_find_64(global.notify_state->client_table, make_client_id(pid, token));
	if ((c == NULL) || (c->name_info == NULL))
	{
		*status = NOTIFY_STATUS_FAILED;
		*name_id = UINT64_MAX;
	}
	else
	{
		c->get_state_count++;
		*status = _notify_lib_get_state(global.notify_state, c->name_info->name_id, state, uid, gid);
		*name_id = c->name_info->name_id;
	}

	if (*name_id == UINT64_MAX) log_message(ASL_LEVEL_DEBUG, "__notify_server_get_state_3 %d %d\n", pid, token);
	else log_message(ASL_LEVEL_DEBUG, "__notify_server_get_state_3 %d %d [%llu]\n", pid, token, *name_id);

	return KERN_SUCCESS;
}

kern_return_t __notify_server_set_state_3
(
	mach_port_t server,
	int token,
	uint64_t state,
	uint64_t *name_id,
	int *status,
	boolean_t claim_root_access,
	audit_token_t audit
)
{
	client_t *c;
	uid_t uid = (uid_t)-1;
	gid_t gid = (gid_t)-1;
	pid_t pid = (pid_t)-1;

	*name_id = 0;

	*status = server_preflight(audit, -1, &uid, &gid, &pid, NULL);
	if (*status != NOTIFY_STATUS_OK) return KERN_SUCCESS;

	bool root_entitlement = uid != 0 && claim_root_access && has_root_entitlement(pid);
	if (root_entitlement) uid = 0;

	call_statistics.set_state++;
	call_statistics.set_state_by_client_and_fetch_id++;

	c = _nc_table_find_64(global.notify_state->client_table, make_client_id(pid, token));
	if ((c == NULL) || (c->name_info == NULL))
	{
		*status = NOTIFY_STATUS_FAILED;
		*name_id = UINT64_MAX;
	}
	else
	{
		*status = _notify_lib_set_state(global.notify_state, c->name_info->name_id, state, uid, gid);
		*name_id = c->name_info->name_id; 
	}

	if (*name_id == UINT64_MAX) log_message(ASL_LEVEL_DEBUG, "__notify_server_set_state_3 %d %d %llu [uid %d%s gid %d]\n", pid, token, state, uid, root_entitlement ? " (entitlement)" : "", gid);
	else log_message(ASL_LEVEL_DEBUG, "__notify_server_set_state_3 %d %d %llu [%llu] [uid %d%s gid %d]\n", pid, token, state, *name_id, uid, root_entitlement ? " (entitlement)" : "", gid);

	return KERN_SUCCESS;
}

kern_return_t __notify_server_set_state_2
(
	mach_port_t server,
	uint64_t name_id,
	uint64_t state,
	boolean_t claim_root_access,
	audit_token_t audit
)
{
	uint32_t status;
	uid_t uid = (uid_t)-1;
	gid_t gid = (gid_t)-1;
	pid_t pid = (pid_t)-1;

	if (global.notify_state == NULL) return KERN_SUCCESS;

	call_statistics.set_state++;
	call_statistics.set_state_by_id++;

	audit_token_to_au32(audit, NULL, &uid, &gid, NULL, NULL, &pid, NULL, NULL);

	bool root_entitlement = (uid != 0) && claim_root_access && has_root_entitlement(pid);
	if (root_entitlement) uid = 0;

	log_message(ASL_LEVEL_DEBUG, "__notify_server_set_state_2 %d %llu %llu [uid %d%s gid %d]\n", pid, name_id, state, uid, root_entitlement ? " (entitlement)" : "", gid);

	status = _notify_lib_set_state(global.notify_state, name_id, state, uid, gid);
	return KERN_SUCCESS;
}

kern_return_t __notify_server_monitor_file_2
(
	mach_port_t server,
	int token,
	caddr_t path,
	mach_msg_type_number_t pathCnt,
	int flags,
	audit_token_t audit
)
{
	client_t *c;
	name_info_t *n;
	uid_t uid = (uid_t)-1;
	gid_t gid = (gid_t)-1;
	pid_t pid = (pid_t)-1;
	uint32_t ubits = (uint32_t)flags;
	int status;

	status = string_validate(path, pathCnt);
	if (status != NOTIFY_STATUS_OK) return KERN_SUCCESS;

	status = server_preflight(audit, -1, &uid, &gid, &pid, NULL);
	if (status != NOTIFY_STATUS_OK) return KERN_SUCCESS;

	call_statistics.monitor_file++;

	log_message(ASL_LEVEL_DEBUG, "__notify_server_monitor_file %d %d %s 0x%08x\n", pid, token, path, ubits);

	c = _nc_table_find_64(global.notify_state->client_table, make_client_id(pid, token));
	if (c == NULL)
	{
		return KERN_SUCCESS;
	}

	n = c->name_info;
	if (n == NULL)
	{
		return KERN_SUCCESS;
	}

	service_open_path_private(n->name, c, path, uid, gid, ubits);

	return KERN_SUCCESS;
}

kern_return_t __notify_server_regenerate
(
	mach_port_t server,
	caddr_t name,
	int token,
	uint32_t reg_type,
	mach_port_t port,
	int sig,
	int prev_slot,
	uint64_t prev_state,
	uint64_t prev_time,
	caddr_t path,
	mach_msg_type_number_t pathCnt,
	int path_flags,
	int *new_slot,
	uint64_t *new_nid,
	int *status,
	audit_token_t audit
)
{
	kern_return_t kstatus;
	pid_t pid = (pid_t)-1;
	int size;
	name_info_t *n;
	client_t *c;
	uint64_t cid;

	*new_slot = 0;
	*new_nid = 0;
	*status = NOTIFY_STATUS_OK;

	*status = string_validate(path, pathCnt);
	if (*status != NOTIFY_STATUS_OK) return KERN_SUCCESS;

	call_statistics.regenerate++;

	audit_token_to_au32(audit, NULL, NULL, NULL, NULL, NULL, &pid, NULL, NULL);

	log_message(ASL_LEVEL_DEBUG, "__notify_server_regenerate %s %d %d %d %u %d %d %llu %s %d\n", name, pid, token, reg_type, port, sig, prev_slot, prev_state, path, path_flags);

	cid = make_client_id(pid, token);
	c = (client_t *)_nc_table_find_64(global.notify_state->client_table, cid);
	if (c != NULL)
	{
		/* duplicate client - this should never happen */
		*status = NOTIFY_STATUS_FAILED;
		return KERN_SUCCESS;
	}

	switch (reg_type)
	{
		case NOTIFY_TYPE_MEMORY:
		{
			/* prev_slot must be between 0 and global.nslots */
			if ((prev_slot < 0) || (prev_slot >= global.nslots))
			{
				*status = NOTIFY_STATUS_INVALID_REQUEST;
				return KERN_SUCCESS;
			}

			kstatus = __notify_server_register_check_2(server, name, token, &size, new_slot, new_nid, status, audit);
			if (*status == NOTIFY_STATUS_OK)
			{
				if ((*new_slot != UINT32_MAX) && (global.last_shm_base != NULL))
				{
					global.shared_memory_base[*new_slot] = global.shared_memory_base[*new_slot] + global.last_shm_base[prev_slot] - 1;
					global.last_shm_base[prev_slot] = 0;
				}
			}
			break;
		}
		case NOTIFY_TYPE_PLAIN:
		{
			kstatus = __notify_server_register_plain_2(server, name, token, audit);
			break;
		}
		case NOTIFY_TYPE_PORT:
		{
			kstatus = __notify_server_register_mach_port_2(server, name, token, port, audit);
			break;
		}
		case NOTIFY_TYPE_SIGNAL:
		{
			kstatus = __notify_server_register_signal_2(server, name, token, sig, audit);
			break;
		}
		case NOTIFY_TYPE_FILE: /* fall through */
		default:
		{
			/* can not regenerate this type */
			*status = NOTIFY_STATUS_FAILED;
			return KERN_SUCCESS;
		}
	}

	if (path != NULL)
	{
		__notify_server_monitor_file_2(server, token, path, pathCnt, path_flags, audit);
	}

	c = (client_t *)_nc_table_find_64(global.notify_state->client_table, cid);
	if (c == NULL)
	{
		*status = NOTIFY_STATUS_FAILED;
	}
	else
	{
		*status = NOTIFY_STATUS_OK;
		n = c->name_info;
		*new_nid = n->name_id;
		if (prev_time > n->state_time) n->state = prev_state;
	}

	return KERN_SUCCESS;
}

kern_return_t __notify_server_checkin
(
	mach_port_t server,
	uint32_t *version,
	uint32_t *server_pid,
	int *status,
	audit_token_t audit
)
{
	pid_t pid = (pid_t)-1;

	call_statistics.checkin++;

	audit_token_to_au32(audit, NULL, NULL, NULL, NULL, NULL, &pid, NULL, NULL);

	log_message(ASL_LEVEL_DEBUG, "__notify_server_checkin %d\n", pid);
	*version = NOTIFY_IPC_VERSION;
	*server_pid = getpid();
	*status = NOTIFY_STATUS_OK;
	return KERN_SUCCESS;
}


kern_return_t __notify_server_dump
(
	mach_port_t server,
	fileport_t fileport,
	audit_token_t audit
)
{
	int fd;
	int flags;

	fd = fileport_makefd(fileport);
	mach_port_deallocate(mach_task_self(), fileport);
	if (fd < 0)
	{
		return KERN_SUCCESS;
	}

	flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0)
	{
		return KERN_SUCCESS;
	}

	flags |= O_NONBLOCK;
	if (fcntl(fd, F_SETFL, flags) < 0)
	{
		return KERN_SUCCESS;
	}

	dump_status(STATUS_REQUEST_SHORT, fd);

	return KERN_SUCCESS;
}
