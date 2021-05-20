/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Copyright (C) 2009 Lennart Poettering
 * Copyright (C) 2010 Maarten Lankhorst
 * Copyright (C) 2021 Duncan Overbruck <mail@duncano.de>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#define _DEFAULT_SOURCE
#define _GNU_SOURCE
#include <sys/epoll.h>
#include <sys/queue.h>
#include <sys/resource.h>
#include <sys/signalfd.h>
#include <sys/time.h>
#include <sys/stat.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <syscall.h>
#include <time.h>
#include <unistd.h>

#ifdef HAVE_LIBSYSTEMD
#include <systemd/sd-bus.h>
#include <systemd/sd-bus-vtable.h>
#elif HAVE_LIBELOGIND
#include <elogind/sd-bus.h>
#include <elogind/sd-bus-vtable.h>
#elif HAVE_BASU
#include <basu/sd-bus.h>
#include <basu/sd-bus-vtable.h>
#else
#error "missing sd-bus implementation"
#endif

#ifdef HAVE_LIBSYSTEMD
#include <systemd/sd-daemon.h>
#endif

#ifndef SCHED_RESET_ON_FORK
/* "Your libc lacks the definition of SCHED_RESET_ON_FORK. We'll now define it ourselves, however make sure your kernel is new enough! */
#define SCHED_RESET_ON_FORK 0x40000000
#endif

struct properties {
	uint64_t rttime_usec_max;
	int32_t max_realtime_priority;
	int32_t min_nice_level;
};

static struct properties props = {
	.rttime_usec_max = 200000ULL,
	.max_realtime_priority = 20,
	.min_nice_level = -15,
};

struct thread {
	struct user *user;
	pid_t tid;
	int fd;
	TAILQ_ENTRY(thread) entries;
};

struct user {
	uid_t uid;
	time_t timestamp;
	size_t num_actions;
	TAILQ_HEAD(, thread) processes;
	TAILQ_ENTRY(user) entries;
};

static TAILQ_HEAD(, user) users = TAILQ_HEAD_INITIALIZER(users);

/* If we actually execute a request we temporarily upgrade our realtime priority to this level */
static unsigned our_realtime_priority = 21;

/* Normally we run at this nice level */
static int our_nice_level = 1;

/* The minimum nice level to hand out */
static int min_nice_level = -15;

/* The maximum realtime priority to hand out */
static unsigned max_realtime_priority = 20;

/* Enforce that clients have RLIMIT_RTTIME set to a value <= this */
static unsigned long long rttime_usec_max = 200000ULL; /* 200 ms */

/* Scheduling policy to use */
static int sched_policy = SCHED_RR;

/* Refuse further requests if one user issues more than ACTIONS_PER_BURST_MAX in this time */
static unsigned actions_burst_sec = 20;

/* Refuse further requests if one user issues more than this many in ACTIONS_BURST_SEC time */
static unsigned actions_per_burst_max = 25;

static sd_bus *bus;
static int epollfd;

static int
_sched_setscheduler(pid_t pid, int sched, const struct sched_param *param)
{
	return syscall(SYS_sched_setscheduler, pid, sched, param);
}

static int
_pidfd_open(pid_t pid, int flags)
{
	return syscall(SYS_pidfd_open, pid, flags);
}

static struct user *
user_find(uid_t uid)
{
	struct user *user;
	TAILQ_FOREACH(user, &users, entries) {
		if (user->uid == uid)
			return user;
	}
	if ((user = calloc(1, sizeof *user)) == NULL)
		return NULL;
	user->uid = uid;
	user->timestamp = time(NULL);
	TAILQ_INIT(&user->processes);
	TAILQ_INSERT_TAIL(&users, user, entries);
	return user;
}

static bool
user_in_burst(struct user *user)
{
	time_t now = time(NULL);
	return now < user->timestamp + actions_burst_sec;
}

static int
user_check_burst(struct user *user)
{
	if (!user_in_burst(user)) {
		/* Restart burst phase */
		user->timestamp = time(NULL);
		user->num_actions = 0;
		return 0;
	}
	user->num_actions++;
	if (user->num_actions >= actions_per_burst_max) {
		fprintf(stderr, "Warning: Reached burst limit for user %d, denying request.\n",
				user->uid);
		return -EBUSY;
	}
	return 0;
}

static void
thread_free(struct thread *thread)
{
	if (thread == NULL)
		return;
	TAILQ_REMOVE(&thread->user->processes, thread, entries);
	if (epoll_ctl(epollfd, EPOLL_CTL_DEL, thread->fd, NULL) == -1) {
		fprintf(stderr, "Failed to delete epoll event: %s\n", strerror(errno));
	}
	close(thread->fd);
	free(thread);
}

static struct thread *
user_thread_find(struct user *user, pid_t pid, pid_t tid)
{
	struct thread *thread;
	int fd = -1;
	int r;

	TAILQ_FOREACH(thread, &user->processes, entries) {
		if (thread->tid == tid)
			return thread;
	}

	if ((fd = _pidfd_open(tid, 0)) == -1) {
		r = -errno;
		goto err;
	}

	if ((thread = calloc(1, sizeof *thread)) == NULL) {
		r = -errno;
		goto err;
	}
	thread->user = user;
	thread->tid = tid;
	thread->fd = fd;
	TAILQ_INSERT_TAIL(&user->processes, thread, entries);

	struct epoll_event event = {
		.events = EPOLLIN,
		.data.ptr = thread,
	};
	if (epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event) == -1) {
		r = -errno;
		goto err;
	}

	return thread;

err:
	close(fd);
	thread_free(thread);
	errno = -r;
	return NULL;
}

static int
tid_check_uid(pid_t pid, pid_t tid, uid_t uid)
{
	struct stat st;
	char path[PATH_MAX];

	snprintf(path, sizeof path, "/proc/%d/task/%d", pid, tid);

	if (stat(path, &st) == -1)
		return -errno;
	if (st.st_uid != uid)
		return -EPERM;

	return 0;
}

static int
tid_check_rttime(pid_t tid)
{
	struct rlimit limit;
	if (prlimit(tid, RLIMIT_RTTIME, NULL, &limit) == -1)
		return -errno;
	if (limit.rlim_max > rttime_usec_max)
		return -EPERM;
	return 0;
}

static struct thread *
thread_get(pid_t pid, pid_t tid, uid_t uid)
{
	struct thread *thread;
	struct user *user;
	int r;

	if ((user = user_find(uid)) == NULL)
		return NULL;

	if ((r = user_check_burst(user)) < 0 ||
	    (r = tid_check_uid(pid, tid, user->uid)) < 0 ||
	    (r = tid_check_rttime(tid)) < 0) {
		errno = -r;
		return NULL;
	}

	if ((thread = user_thread_find(user, pid, tid)) == NULL)
		return NULL;

	return thread;
}

static int
thread_set_realtime(struct thread *thread, unsigned priority)
{
	struct sched_param param = {0};

	if ((int)priority < sched_get_priority_min(sched_policy) ||
	    (int)priority > sched_get_priority_max(sched_policy))
		return -EINVAL;

	/* We always want to be able to get a higher RT priority than the client */
	if (priority >= our_realtime_priority ||
	    priority > max_realtime_priority)
		return -EPERM;

	param.sched_priority = priority;
	if (_sched_setscheduler(thread->tid, sched_policy|SCHED_RESET_ON_FORK, &param) == -1) {
		return -errno;
	}

	fprintf(stderr, "Successfully made thread %llu RT at level %u.\n",
		(unsigned long long)thread->tid, priority);

	return 0;
}

static int
message_sender_get_pid(const char *sender, pid_t *pid)
{
	sd_bus_message *reply = NULL;
	int r;

	r = sd_bus_call_method(
			bus,
			"org.freedesktop.DBus",
			"/org/freedesktop/DBus",
			"org.freedesktop.DBus",
			"GetConnectionUnixProcessID",
			NULL,
			&reply,
			"s",
			sender);
	if (r < 0)
		return r;

	if ((r = sd_bus_message_read(reply, "u", pid)) < 0)
		return r;

	return 0;
}


static int
message_sender_get_uid(const char *sender, uid_t *uid)
{
	sd_bus_message *reply = NULL;
	int r;

	r = sd_bus_call_method(
			bus,
			"org.freedesktop.DBus",
			"/org/freedesktop/DBus",
			"org.freedesktop.DBus",
			"GetConnectionUnixUser",
			NULL,
			&reply,
			"s",
			sender);
	if (r < 0)
		return r;

	if ((r = sd_bus_message_read(reply, "u", uid)) < 0)
		return r;

	return 0;
}

static int
message_sender_get_pid_uid(sd_bus_message *m, pid_t *pid, uid_t *uid)
{
	const char *sender;
	int r;

	if ((sender = sd_bus_message_get_sender(m)) == NULL)
		return -ENODATA;
	if (pid && (r = message_sender_get_pid(sender, pid)) < 0)
		return r;
	if (uid && (r = message_sender_get_uid(sender, uid)) < 0)
		return r;

	return 0;
}

static int
make_thread_realtime(sd_bus_message *m, void *userdata, sd_bus_error *error)
{
	struct thread *thread;
	uint64_t tid;
	uint32_t priority;
	pid_t pid;
	uid_t uid;
	int r;

	if ((r = sd_bus_message_read(m, "tu", &tid, &priority)) < 0)
		return r;
	if ((r = message_sender_get_pid_uid(m, &pid, &uid)) < 0)
		return r;
	if ((thread = thread_get(pid, tid, uid)) == NULL)
		return -errno;
	if ((r = thread_set_realtime(thread, priority)) < 0)
		return r;
	return sd_bus_reply_method_return(m, "");
}

static int
make_thread_realtime_with_pid(sd_bus_message *m, void *userdata, sd_bus_error *error)
{
	struct thread *thread;
	uint64_t pid, tid;
	uint32_t priority;
	uid_t uid;
	int r;

	if ((r = sd_bus_message_read(m, "ttu", &pid, &tid, &priority)) < 0)
		return r;
	if ((r = message_sender_get_pid_uid(m, NULL, &uid)) < 0)
		return r;
	if ((thread = thread_get(pid, tid, uid)) == NULL)
		return -errno;
	if ((r = thread_set_realtime(thread, priority)) < 0)
		return r;
	return sd_bus_reply_method_return(m, "");
}

static int
thread_set_high_priority(struct thread *thread, int priority)
{
	struct sched_param param = {0};

	if (priority < PRIO_MIN || priority >= PRIO_MAX)
		return -EINVAL;
	if (priority < min_nice_level)
		return -EPERM;

	if (_sched_setscheduler(thread->tid, SCHED_OTHER|SCHED_RESET_ON_FORK, &param) == -1)
		return -errno;
	if (setpriority(PRIO_PROCESS, thread->tid, priority) == -1)
		return -errno;

	fprintf(stderr, "Successfully made thread %llu high priority at nice level %i.\n",
		(unsigned long long)thread->tid, priority);
	return 0;
}

static int
make_thread_high_priority(sd_bus_message *m, void *userdata, sd_bus_error *error)
{
	struct thread *thread;
	uint64_t tid;
	int32_t priority;
	pid_t pid;
	uid_t uid;
	int r;

	if ((r = sd_bus_message_read(m, "ti", &tid, &priority)) < 0)
		return r;
	if ((r = message_sender_get_pid_uid(m, &pid, &uid)) < 0)
		return r;
	if ((thread = thread_get(pid, tid, uid)) == NULL)
		return -errno;
	if ((r = thread_set_high_priority(thread, priority)) < 0)
		return r;
	return sd_bus_reply_method_return(m, "");
}

static int
make_thread_high_priority_with_pid(sd_bus_message *m, void *userdata, sd_bus_error *error)
{
	struct thread *thread;
	uint64_t pid, tid;
	int32_t priority;
	uid_t uid;
	int r;

	if ((r = sd_bus_message_read(m, "tti", &pid, &tid, &priority)) < 0)
		return r;
	if ((r = message_sender_get_pid_uid(m, NULL, &uid)) < 0)
		return r;
	if ((thread = thread_get(pid, tid, uid)) == NULL)
		return -errno;
	if ((r = thread_set_high_priority(thread, priority)) < 0)
		return r;
	return sd_bus_reply_method_return(m, "");
}

static int
thread_reset(struct thread *thread)
{
	struct sched_param param = {0};
	int r = 0;

	if (_sched_setscheduler(thread->tid, SCHED_OTHER, &param) == -1) {
		r = -errno;
		fprintf(stderr, "Failed to demote thread %d: %s\n", thread->tid, strerror(errno));
	}
	if (setpriority(PRIO_PROCESS, thread->tid, 0) == -1) {
		r = -errno;
		fprintf(stderr, "Failed to demote thread %d: %s\n", thread->tid, strerror(errno));
	}
	return r;
}

static void
reset_all(void)
{
}

static int
method_reset_all(sd_bus_message *m, void *userdata, sd_bus_error *error)
{
	reset_all();
	return sd_bus_reply_method_return(m, "");
}

static void
reset_known(void)
{
	struct user *user;
	struct thread *thread;
	fprintf(stderr, "Demoting known real-time threads.\n");
	size_t n = 0;
	TAILQ_FOREACH(user, &users, entries) {
		TAILQ_FOREACH(thread, &user->processes, entries) {
			if (thread_reset(thread) == 0)
				n++;
		}
	}
	fprintf(stderr, "Demoted %zu threads.\n", n);
}

static int
method_reset_known(sd_bus_message *m, void *userdata, sd_bus_error *error)
{
	reset_known();
	return sd_bus_reply_method_return(m, "");
}

static int
method_exit(sd_bus_message *m, void *userdata, sd_bus_error *error)
{
	return sd_bus_reply_method_return(m, "");
}

static const sd_bus_vtable rtkit_vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD(
			"MakeThreadRealtime",
			"tu",
			NULL,
			make_thread_realtime,
			SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD(
			"MakeThreadRealtimeWithPID",
			"ttu",
			NULL,
			make_thread_realtime_with_pid,
			SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD(
			"MakeThreadHighPriority",
			"ti",
			NULL,
			make_thread_high_priority,
			SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD(
			"MakeThreadHighPriorityWithPID",
			"tti",
			NULL,
			make_thread_high_priority_with_pid,
			SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD(
			"ResetAll",
			NULL,
			NULL,
			method_reset_all,
			0),
	SD_BUS_METHOD(
			"ResetKnown",
			NULL,
			NULL,
			method_reset_known,
			0),
	SD_BUS_METHOD(
			"Exit",
			NULL,
			NULL,
			method_exit,
			0),
	SD_BUS_PROPERTY(
			"RTTimeUSecMax",
			"x",
			NULL,
			offsetof(struct properties, rttime_usec_max),
			SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_PROPERTY(
			"MaxRealtimePriority",
			"i",
			NULL,
			offsetof(struct properties, max_realtime_priority),
			SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_PROPERTY(
			"MinNiceLevel",
			"i",
			NULL,
			offsetof(struct properties, min_nice_level),
			SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_VTABLE_END,
};

int
main(int argc, char *argv[])
{
	sd_bus_slot *slot = NULL;
	int r;

	if ((epollfd = epoll_create1(EPOLL_CLOEXEC)) == -1) {
		r = -errno;
		fprintf(stderr, "Failed to create epoll instance: %s\n", strerror(errno));
		goto err;
	}

	if ((r = sd_bus_default_system(&bus)) < 0) {
		fprintf(stderr, "Failed to connect to system bus: %s\n", strerror(-r));
		goto err;
	}
	r = sd_bus_add_object_vtable(bus, &slot, "/org/freedesktop/RealtimeKit1", "org.freedesktop.RealtimeKit1", rtkit_vtable, &props);
	if (r < 0) {
		fprintf(stderr, "Failed to issue method call: %s\n", strerror(-r));
		goto err;
	}
	if ((r = sd_bus_request_name(bus, "org.freedesktop.RealtimeKit1", 0)) < 0) {
		fprintf(stderr, "Failed to aquire service name: %s\n", strerror(-r));
		goto err;
	}
	int busfd = sd_bus_get_fd(bus);
	if (busfd < 0) {
		fprintf(stderr, "Failed to get bus file descriptor: %s\n", strerror(-busfd));
		r = -1;
		goto err;
	}
	struct epoll_event event = {
		.events = EPOLLIN,
		.data.fd = busfd,
	};
	if (epoll_ctl(epollfd, EPOLL_CTL_ADD, busfd, &event) == -1) {
		r = -errno;
		fprintf(stderr, "Failed to add epoll event: %s\n", strerror(errno));
		goto err;
	}
	sigset_t mask;
	sigemptyset(&mask);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGTERM);
	if (sigprocmask(SIG_BLOCK, &mask, NULL) == -1) {
		r = -errno;
		fprintf(stderr, "Failed to block signals: %s\n", strerror(errno));
		goto err;
	}
	int sigfd = signalfd(-1, &mask, SFD_CLOEXEC);
	event.events = EPOLLIN;
	event.data.fd = sigfd;
	if (epoll_ctl(epollfd, EPOLL_CTL_ADD, sigfd, &event) == -1) {
		r = -errno;
		fprintf(stderr, "Failed to add epoll event: %s\n", strerror(errno));
		goto err;
	}

#ifdef HAVE_LIBSYSTEMD
	sd_notify(0, "STATUS=Running.");
#endif

	for (;;) {
		int n = epoll_wait(epollfd, &event, 1, -1);
		if (n == -1) {
			r = -errno;
			fprintf(stderr, "Failed to get epoll events: %s\n", strerror(errno));
			goto err;
		}
		if (event.data.fd == sigfd) {
			struct signalfd_siginfo si;
			ssize_t rd = read(sigfd, &si, sizeof si);
			if (rd != sizeof si) {
				fprintf(stderr, "Failed to read signal info: %s\n", strerror(errno));
				continue;
			}
			fprintf(stderr, "Got signal, exiting...\n");
			break;
		} else if (event.data.fd == busfd) {
			if ((r = sd_bus_process(bus, NULL)) < 0) {
				fprintf(stderr, "Failed to process bus: %s\n", strerror(-r));
				break;
			}
		} else {
			thread_free(event.data.ptr);
		}
	}

	reset_known();

err:
	sd_bus_slot_unref(slot);
	sd_bus_unref(bus);
	return r < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
