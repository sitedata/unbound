/*
 * daemon/worker.h - worker that handles a pending list of requests.
 *
 * Copyright (c) 2007, NLnet Labs. All rights reserved.
 *
 * This software is open source.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 
 * Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 * 
 * Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 * 
 * Neither the name of the NLNET LABS nor the names of its contributors may
 * be used to endorse or promote products derived from this software without
 * specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * \file
 *
 * This file describes the worker structure that holds a list of 
 * pending requests and handles them.
 */

#ifndef DAEMON_WORKER_H
#define DAEMON_WORKER_H

#include "config.h"
#include "util/netevent.h"
#include "util/locks.h"
#include "util/alloc.h"
#include "util/data/msgreply.h"
#include "util/data/msgparse.h"
#include "daemon/stats.h"
#include "util/module.h"
struct listen_dnsport;
struct outside_network;
struct config_file;
struct daemon;
struct listen_port;
struct ub_randstate;
struct region;

/** size of table used for random numbers. large to be more secure. */
#define RND_STATE_SIZE 256

/** worker commands */
enum worker_commands {
	/** make the worker quit */
	worker_cmd_quit
};

/**
 * Structure holding working information for unbound.
 * Holds globally visible information.
 */
struct worker {
	/** the thread number (in daemon array). First in struct for debug. */
	int thread_num;
	/** global shared daemon structure */
	struct daemon* daemon;
	/** thread id */
	ub_thread_t thr_id;
	/** fd 0 of socketpair, write commands for worker to this one */
	int cmd_send_fd;
	/** fd 1 of socketpair, worker listens on this one */
	int cmd_recv_fd;
	/** the event base this worker works with */
	struct comm_base* base;
	/** the frontside listening interface where request events come in */
	struct listen_dnsport* front;
	/** the backside outside network interface to the auth servers */
	struct outside_network* back;
	/** the signal handler */
	struct comm_signal* comsig;
	/** commpoint to listen to commands. */
	struct comm_point* cmd_com;

	/** number of requests that can be handled by this worker */
	size_t request_size;

	/** random() table for this worker. */
	struct ub_randstate* rndstate;
	/** do we need to restart (instead of exit) ? */
	int need_to_restart;
	/** allocation cache for this thread */
	struct alloc_cache alloc;
	/** per thread statistics */
	struct server_stats stats;
	/** thread scratch region */
	struct region* scratchpad;

	/** module environment passed to modules, changed for this thread */
	struct module_env env;
};

/**
 * Create the worker structure. Bare bones version, zeroed struct,
 * with backpointers only. Use worker_init on it later.
 * @param daemon: the daemon that this worker thread is part of.
 * @param id: the thread number from 0.. numthreads-1.
 * @return: the new worker or NULL on alloc failure.
 */
struct worker* worker_create(struct daemon* daemon, int id);

/**
 * Initialize worker.
 * Allocates event base, listens to ports
 * @param worker: worker to initialize, created with worker_create.
 * @param cfg: configuration settings.
 * @param ports: list of shared query ports.
 * @param do_sigs: if true, worker installs signal handlers.
 * @return: false on error.
 */
int worker_init(struct worker* worker, struct config_file *cfg, 
	struct listen_port* ports, int do_sigs);

/**
 * Make worker work.
 */
void worker_work(struct worker* worker);

/**
 * Delete worker.
 */
void worker_delete(struct worker* worker);

/**
 * Send a command to a worker. Uses blocking writes.
 * @param worker: worker to send command to.
 * @param buffer: an empty buffer to use.
 * @param cmd: command to send.
 */
void worker_send_cmd(struct worker* worker, ldns_buffer* buffer,
        enum worker_commands cmd);

/**
 * Worker signal handler function. User argument is the worker itself.
 * @param sig: signal number.
 * @param arg: the worker (main worker) that handles signals.
 */
void worker_sighandler(int sig, void* arg);

/**
 * Worker service routine to send udp messages for modules.
 * @param pkt: packet to send.
 * @param addr: where to.
 * @param addrlen: length of addr.
 * @param timeout: seconds to wait until timeout.
 * @param q: wich query state to reactivate upon return.
 * @param use_tcp: true to use TCP, false for UDP.
 * @return: false on failure (memory or socket related). no query was
 *      sent.
 */
int worker_send_packet(ldns_buffer* pkt, struct sockaddr_storage* addr,
	socklen_t addrlen, int timeout, struct module_qstate* q, int use_tcp);

/**
 * Worker service routine to send serviced queries to authoritative servers.
 * @param qname: query name. (host order)
 * @param qnamelen: length in bytes of qname, including trailing 0.
 * @param qtype: query type. (host order)
 * @param qclass: query class. (host order)
 * @param flags: host order flags word, with opcode and CD bit.
 * @param dnssec: if set, EDNS record will have DO bit set.
 * @param addr: where to.
 * @param addrlen: length of addr.
 * @param q: wich query state to reactivate upon return.
 * @return: false on failure (memory or socket related). no query was
 *      sent.
 */
struct outbound_entry* worker_send_query(uint8_t* qname, size_t qnamelen, 
	uint16_t qtype, uint16_t qclass, uint16_t flags, int dnssec, 
	struct sockaddr_storage* addr, socklen_t addrlen,
	struct module_qstate* q);

/** 
 * process control messages from the main thread. 
 * @param c: comm point to read from.
 * @param arg: worker.
 * @param error: error status of comm point.
 * @param reply_info: not used.
 */
int worker_handle_control_cmd(struct comm_point* c, void* arg, int error, 
	struct comm_reply* reply_info);

/** handles callbacks from listening event interface */
int worker_handle_request(struct comm_point* c, void* arg, int error,
	struct comm_reply* repinfo);

#endif /* DAEMON_WORKER_H */
