/*
 * 2011+ Copyright (c) Evgeniy Polyakov <zbr@ioremap.net>
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "config.h"

#include <sys/stat.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>

#include "elliptics.h"
#include "elliptics/interface.h"

static void dnet_schedule_io(struct dnet_node *n, struct dnet_io_req *r)
{
	struct dnet_io *io = n->io;

	dnet_log(r->st->n, DNET_LOG_DSA, "%s: %s: queueing IO event: %p: hsize: %zu, dsize: %zu\n",
			dnet_state_dump_addr(r->st), dnet_dump_id(r->header), r, r->hsize, r->dsize);

	pthread_mutex_lock(&io->recv_lock);
	list_add_tail(&r->req_entry, &io->recv_list);

	pthread_cond_broadcast(&io->recv_wait);
	pthread_mutex_unlock(&io->recv_lock);
}

void dnet_schedule_command(struct dnet_net_state *st)
{
	st->rcv_flags = DNET_IO_CMD;

	if (st->rcv_data) {
#if 0
		struct dnet_cmd *c = &st->rcv_cmd;
		unsigned long long tid = c->trans & ~DNET_TRANS_REPLY;
		dnet_log(st->n, DNET_LOG_DSA, "freed: size: %llu, trans: %llu, reply: %d, ptr: %p.\n",
						(unsigned long long)c->size, tid, tid != c->trans, st->rcv_data);
#endif
		free(st->rcv_data);
		st->rcv_data = NULL;
	}

	st->rcv_end = sizeof(struct dnet_cmd);
	st->rcv_offset = 0;
}

static int dnet_process_recv_single(struct dnet_net_state *st)
{
	struct dnet_node *n = st->n;
	struct dnet_io_req *r;
	void *data;
	uint64_t size;
	int err;

again:
	/*
	 * Reading command first.
	 */
	if (st->rcv_flags & DNET_IO_CMD)
		data = &st->rcv_cmd;
	else
		data = st->rcv_data;
	data += st->rcv_offset;
	size = st->rcv_end - st->rcv_offset;

	if (size) {
		err = recv(st->read_s, data, size, 0);
		if (err < 0) {
			err = -EAGAIN;
			if (errno != EAGAIN && errno != EINTR) {
				err = -errno;
				dnet_log_err(n, "failed to receive data, socket: %d", st->read_s);
				goto out;
			}

			goto out;
		}

		if (err == 0) {
			dnet_log(n, DNET_LOG_ERROR, "Peer %s has disconnected.\n",
				dnet_server_convert_dnet_addr(&st->addr));
			err = -ECONNRESET;
			goto out;
		}

		st->rcv_offset += err;
	}

	dnet_log(n, DNET_LOG_DSA, "%s: rcv_offset: %llu, rcv_end: %llu, rcv_flags: %x\n",
			dnet_state_dump_addr(st),
			(unsigned long long)st->rcv_offset,
			(unsigned long long)st->rcv_end,
			st->rcv_flags);

	if (st->rcv_offset != st->rcv_end)
		goto again;

	if (st->rcv_flags & DNET_IO_CMD) {
		unsigned long long tid;
		struct dnet_cmd *c = &st->rcv_cmd;

		dnet_convert_cmd(c);

		tid = c->trans & ~DNET_TRANS_REPLY;

		dnet_log(n, DNET_LOG_DSA, "%s: received trans: %llu / %llx, "
				"reply: %d, size: %llu, flags: %x, status: %d.\n",
				dnet_dump_id(&c->id), tid, (unsigned long long)c->trans,
				!!(c->trans & DNET_TRANS_REPLY),
				(unsigned long long)c->size, c->flags, c->status);

		r = malloc(c->size + sizeof(struct dnet_cmd) + sizeof(struct dnet_io_req));
		if (!r) {
			err = -ENOMEM;
			goto out;
		}
		memset(r, 0, sizeof(struct dnet_io_req));

		r->header = r + 1;
		r->hsize = sizeof(struct dnet_cmd);
		memcpy(r->header, &st->rcv_cmd, sizeof(struct dnet_cmd));

		st->rcv_data = r;
		st->rcv_offset = sizeof(struct dnet_io_req) + sizeof(struct dnet_cmd);
		st->rcv_end = st->rcv_offset + c->size;
		st->rcv_flags &= ~DNET_IO_CMD;

		if (c->size) {
			r->data = r->header + sizeof(struct dnet_cmd);
			r->dsize = c->size;

			/*
			 * We read the command header, now get the data.
			 */
			goto again;
		}
	}

	r = st->rcv_data;
	st->rcv_data = NULL;

	dnet_schedule_command(st);

	r->st = dnet_state_get(st);
	dnet_schedule_io(n, r);
	return 0;

out:
	if (err != -EAGAIN && err != -EINTR)
		dnet_schedule_command(st);

	return err;
}

int dnet_state_accept_process(struct dnet_net_state *orig, struct epoll_event *ev __unused)
{
	struct dnet_node *n = orig->n;
	int err, cs;
	struct dnet_addr addr;
	struct dnet_net_state *st;

	memset(&addr, 0, sizeof(addr));

	addr.addr_len = sizeof(addr.addr);
	cs = accept(orig->read_s, (struct sockaddr *)&addr.addr, &addr.addr_len);
	if (cs <= 0) {
		err = -errno;
		if (err != -EAGAIN)
			dnet_log_err(n, "failed to accept new client at %s", dnet_state_dump_addr(orig));
		goto err_out_exit;
	}

	dnet_set_sockopt(cs);

	st = dnet_state_create(n, 0, NULL, 0, &addr, cs, &err, dnet_state_net_process);
	if (!st)
		goto err_out_close;

	dnet_log(n, DNET_LOG_INFO, "Accepted client %s, socket: %d.\n",
			dnet_server_convert_dnet_addr(&addr), cs);

	return 0;

err_out_close:
	dnet_sock_close(cs);
err_out_exit:
	return err;
}

void dnet_unschedule_send(struct dnet_net_state *st)
{
	struct epoll_event ev;

	ev.events = EPOLLOUT;
	ev.data.ptr = st;

	epoll_ctl(st->n->io->epoll_fd, EPOLL_CTL_DEL, st->write_s, &ev);
}

void dnet_unschedule_recv(struct dnet_net_state *st)
{
	struct epoll_event ev;

	ev.events = EPOLLIN;
	ev.data.ptr = st;

	epoll_ctl(st->n->io->epoll_fd, EPOLL_CTL_DEL, st->read_s, &ev);
}

static int dnet_process_send_single(struct dnet_net_state *st)
{
	struct dnet_io_req *r = NULL;
	int err;

	while (1) {
		r = NULL;

		pthread_mutex_lock(&st->send_lock);
		if (!list_empty(&st->send_list)) {
			r = list_first_entry(&st->send_list, struct dnet_io_req, req_entry);
		} else {
			dnet_unschedule_send(st);
		}
		pthread_mutex_unlock(&st->send_lock);

		if (!r) {
			err = -EAGAIN;
			goto err_out_exit;
		}

		err = dnet_send_request(st, r);
		if (err)
			goto err_out_exit;
	}

err_out_exit:
	return err;
}

static int dnet_schedule_network_io(struct dnet_net_state *st, int send)
{
	struct epoll_event ev;
	int err, fd;

	if (send) {
		ev.events = EPOLLOUT;
		fd = st->write_s;
	} else {
		ev.events = EPOLLIN;
		fd = st->read_s;
	}
	ev.data.ptr = st;

	err = epoll_ctl(st->n->io->epoll_fd, EPOLL_CTL_ADD, fd, &ev);
	if (err < 0) {
		err = -errno;

		if (err == -EEXIST) {
			err = 0;
		} else {
			dnet_log_err(st->n, "%s: failed to add %s event", dnet_state_dump_addr(st), send ? "SEND" : "RECV");
		}
	}

	dnet_log(st->n, DNET_LOG_DSA, "%s: scheduled %s event\n", dnet_state_dump_addr(st), send ? "SEND" : "RECV");

	return err;
}

int dnet_schedule_send(struct dnet_net_state *st)
{
	return dnet_schedule_network_io(st, 1);
}

int dnet_schedule_recv(struct dnet_net_state *st)
{
	return dnet_schedule_network_io(st, 0);
}

int dnet_state_net_process(struct dnet_net_state *st, struct epoll_event *ev)
{
	int err = -ECONNRESET;

	dnet_log(st->n, DNET_LOG_DSA, "%s: net process, event: %x\n", dnet_state_dump_addr(st), ev->events);

	if (ev->events & EPOLLIN) {
		err = dnet_process_recv_single(st);
	} else if (ev->events & EPOLLOUT) {
		err = dnet_process_send_single(st);
	}

	if (ev->events & (EPOLLHUP | EPOLLERR)) {
		dnet_log(st->n, DNET_LOG_ERROR, "%s: received error event mask %x\n", dnet_state_dump_addr(st), ev->events);
		err = -ECONNRESET;
	}

	return err;
}

static void *dnet_io_process(void *data_)
{
	struct dnet_node *n = data_;
	struct dnet_io *io = n->io;
	struct dnet_net_state *st;
	struct epoll_event ev;
	int err;

	while (!n->need_exit) {
		err = epoll_wait(io->epoll_fd, &ev, 1, 1000);
		if (err == 0)
			continue;

		if (err < 0) {
			err = -errno;

			if (err == -EAGAIN || err == -EINTR)
				continue;

			dnet_log_err(n, "Failed to wait for IO fds");
			n->need_exit = err;
			break;
		}

		st = ev.data.ptr;

		while (1) {
			err = st->process(st, &ev);
			if (err == 0)
				continue;

			if (err == -EAGAIN)
				break;

			if (err < 0) {
				dnet_state_reset(st);
				break;
			}
		}
	}

	return &n->need_exit;
}

static void dnet_io_cleanup_states(struct dnet_node *n)
{
	struct dnet_net_state *st, *tmp;

	list_for_each_entry_safe(st, tmp, &n->storage_state_list, storage_state_entry) {
		dnet_state_reset(st);
	}
}

static void *dnet_io_process_pool(void *data_)
{
	struct dnet_node *n = data_;
	struct dnet_net_state *st;
	struct dnet_io *io = n->io;
	struct timespec ts;
	struct timeval tv;
	struct dnet_io_req *r;
	int err = 0;

	dnet_log(n, DNET_LOG_INFO, "Starting IO processing thread.\n");

	while (!n->need_exit) {
		r = NULL;
		err = 0;

		gettimeofday(&tv, NULL);
		ts.tv_sec = tv.tv_sec + 1;
		ts.tv_nsec = tv.tv_usec * 1000;

		pthread_mutex_lock(&io->recv_lock);
		if (!list_empty(&io->recv_list)) {
			r = list_first_entry(&io->recv_list, struct dnet_io_req, req_entry);
		} else {
			err = pthread_cond_timedwait(&io->recv_wait, &io->recv_lock, &ts);
			if (!list_empty(&io->recv_list)) {
				r = list_first_entry(&io->recv_list, struct dnet_io_req, req_entry);
				err = 0;
			}
		}

		if (r)
			list_del_init(&r->req_entry);
		pthread_mutex_unlock(&io->recv_lock);

		if (!r)
			continue;

		st = r->st;

		dnet_log(n, DNET_LOG_DSA, "%s: %s: got IO event: %p: hsize: %zu, dsize: %zu\n",
				dnet_state_dump_addr(st), dnet_dump_id(r->header), r, r->hsize, r->dsize);

		err = dnet_process_recv(st, r);

		pthread_mutex_lock(&io->recv_lock);
		list_del(&r->req_entry);
		pthread_mutex_unlock(&io->recv_lock);
		dnet_io_req_free(r);

		dnet_state_put(st);
	}

	dnet_log(n, DNET_LOG_INFO, "Exiting IO processing thread: need_exit: %d, err: %d.\n", n->need_exit, err);
	return NULL;
}

int dnet_io_init(struct dnet_node *n, struct dnet_config *cfg)
{
	int err, i;
	struct dnet_io *io;

	io = malloc(sizeof(struct dnet_io) + sizeof(pthread_t) * cfg->io_thread_num);
	if (!io) {
		err = -ENOMEM;
		goto err_out_exit;
	}

	memset(io, 0, sizeof(struct dnet_io));

	io->thread_num = cfg->io_thread_num;
	INIT_LIST_HEAD(&io->recv_list);
	n->io = io;

	err = pthread_cond_init(&io->recv_wait, NULL);
	if (err) {
		err = -err;
		dnet_log(n, DNET_LOG_ERROR, "Failed to initialize send cond: %d\n", err);
		goto err_out_free;
	}

	err = pthread_mutex_init(&io->recv_lock, NULL);
	if (err) {
		err = -err;
		dnet_log(n, DNET_LOG_ERROR, "Failed to initialize send lock: %d\n", err);
		goto err_out_recv_cond;
	}

	io->epoll_fd = epoll_create(100000);
	if (io->epoll_fd < 0) {
		err = -errno;
		dnet_log_err(n, "Failed to create epoll fd");
		goto err_out_recv_lock;
	}

	err = pthread_create(&io->tid, NULL, dnet_io_process, n);
	if (err) {
		err = -err;
		dnet_log(n, DNET_LOG_ERROR, "Failed to create network processing thread: %d\n", err);
		goto err_out_epoll_fd;
	}

	for (i=0; i<io->thread_num; ++i) {
		err = pthread_create(&io->threads[i], NULL, dnet_io_process_pool, n);
		if (err) {
			err = -err;
			io->thread_num = i;
			dnet_log(n, DNET_LOG_ERROR, "Failed to create IO thread: %d\n", err);
			goto err_out_io_threads;
		}
	}

	return 0;

err_out_io_threads:
	for (i=0; i<io->thread_num; ++i)
		pthread_join(io->threads[i], NULL);

	pthread_join(io->tid, NULL);
err_out_epoll_fd:
	close(io->epoll_fd);
err_out_recv_lock:
	pthread_mutex_destroy(&io->recv_lock);
err_out_recv_cond:
	pthread_cond_destroy(&io->recv_wait);
err_out_free:
	free(io);
err_out_exit:
	return err;
}

void dnet_io_exit(struct dnet_node *n)
{
	struct dnet_io *io = n->io;
	struct dnet_io_req *r, *tmp;
	int i;

	for (i=0; i<io->thread_num; ++i)
		pthread_join(io->threads[i], NULL);

	pthread_join(io->tid, NULL);
	dnet_io_cleanup_states(n);

	close(io->epoll_fd);

	list_for_each_entry_safe(r, tmp, &io->recv_list, req_entry) {
		list_del(&r->req_entry);
		dnet_io_req_free(r);
	}

	pthread_mutex_destroy(&io->recv_lock);
	pthread_cond_destroy(&io->recv_wait);

	free(io);
}