/*
 * Copyright 2009 Lennart Poettering
 * Copyright 2010 Maarten Lankhorst
 * RealtimeKit is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * RealtimeKit is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 * You should have received a copy of the GNU General Public License
 * along with RealtimeKit. If not, see <http://www.gnu.org/licenses/>.
 */
#define _DEFAULT_SOURCE
#define _GNU_SOURCE
#include <sys/epoll.h>
#include <sys/queue.h>
#include <sys/resource.h>
#include <sys/signalfd.h>
#include <sys/time.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sched.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <syscall.h>
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
	int64_t rtt_usec_max;
	int32_t max_realtime_priority;
	int32_t min_nice_level;
};

static struct properties props = {
	.rtt_usec_max = 200000ULL,
	.max_realtime_priority = 20,
	.min_nice_level = -15,
};

struct process {
	pid_t pid;
	int pidfd;
	TAILQ_ENTRY(process) entries;
};

struct user {
	uid_t uid;
	TAILQ_ENTRY(user) entries;
};

static TAILQ_HEAD(, user) users = TAILQ_HEAD_INITIALIZER(users);
static TAILQ_HEAD(, process) processes = TAILQ_HEAD_INITIALIZER(processes);

static int sched_policy = SCHED_RR;
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

static int
open_pidfd(pid_t pid)
{
	int fd = -1;
	if ((fd = _pidfd_open(pid, 0)) >= 0) {
		return fd;
	}
	if (errno != ENOSYS)
		return -1;
	char path[PATH_MAX];
	snprintf(path, sizeof path, "/proc/%d", pid);
	return open(path, O_DIRECTORY|O_RDONLY|O_CLOEXEC);
}

static int
get_message_sender_uid(const char *sender, uid_t *uid)
{
	sd_bus_message *reply = NULL;
	int rv;

	rv = sd_bus_call_method(
			bus,
			"org.freedesktop.DBus",
			"/org/freedesktop/DBus",
			"org.freedesktop.DBus",
			"GetConnectionUnixUser",
			NULL,
			&reply,
			"s",
			sender);
	if (rv < 0) 
		return rv;

	if ((rv = sd_bus_message_read(reply, "u", uid)) < 0)
		return rv;

	return 0;
}

static int
get_message_sender_pid(const char *sender, pid_t *pid)
{
	sd_bus_message *reply = NULL;
	int rv;

	rv = sd_bus_call_method(
			bus,
			"org.freedesktop.DBus",
			"/org/freedesktop/DBus",
			"org.freedesktop.DBus",
			"GetConnectionUnixProcessID",
			NULL,
			&reply,
			"s",
			sender);
	if (rv < 0) 
		return rv;

	if ((rv = sd_bus_message_read(reply, "u", pid)) < 0)
		return rv;

	return 0;
}

static struct user *
find_user(uid_t uid)
{
	struct user *user;
	TAILQ_FOREACH(user, &users, entries) {
		if (user->uid == uid)
			return user;
	}
	if ((user = calloc(1, sizeof *user)) == NULL)
		return NULL;
	user->uid = uid;
	TAILQ_INSERT_TAIL(&users, user, entries);
	return user;
}

static struct process *
find_process(pid_t pid)
{
	struct process *proc;

	TAILQ_FOREACH(proc, &processes, entries) {
		if (proc->pid == pid)
			return proc;
	}

	int pidfd = open_pidfd(pid);
	if (pidfd == -1)
		return NULL;

	if ((proc = calloc(1, sizeof *proc)) == NULL) {
		close(pidfd);
		return NULL;
	}
	proc->pid = pid;
	proc->pidfd = pidfd;

	struct epoll_event event = {
		.events = EPOLLIN,
		.data.ptr = proc,
	};
	if (epoll_ctl(epollfd, EPOLL_CTL_ADD, pidfd, &event) == -1) {
		fprintf(stderr, "Failed to add epoll event: %s\n", strerror(errno));
		close(pidfd);
		free(proc);
		return NULL;
	}

	TAILQ_INSERT_TAIL(&processes, proc, entries);
	return proc;
}

static int
lookup(struct user **user, struct process **process,
		sd_bus_message *m, pid_t pid, pid_t tid)
{
	uid_t uid = -1;
	int rv;
	const char *sender;

	if ((sender = sd_bus_message_get_sender(m)) == NULL)
		return -ENODATA;

	if (pid == -1 && (rv = get_message_sender_pid(sender, &pid)) < 0)
		return rv;

	if ((rv = get_message_sender_uid(sender, &uid)) < 0)
		return rv;

	if ((*user = find_user(uid)) == NULL)
		return -errno;

	if ((*process = find_process(pid)) == NULL)
		return -errno;

	return 0;
}


#if 0
static int
get_message_creds_pid(sd_bus_message *m, pid_t *pid)
{
	sd_bus_creds *c;
	int rv;
	if ((c = sd_bus_message_get_creds(m)) == NULL)
		return -ENODATA;
	if ((rv = sd_bus_creds_get_pid(c, pid)) < 0)
		return rv;
	return 0;
}
#endif

static int
set_realtime(struct process *proc, int32_t priority)
{
	struct sched_param param = {0};

	param.sched_priority = (int)priority;
	if (_sched_setscheduler(proc->pid, sched_policy|SCHED_RESET_ON_FORK, &param) == -1) {
		return -errno;
	}

	fprintf(stderr, "Successfully made process %llu RT at level %u.\n",
		(unsigned long long)proc->pid, priority);

	return 0;
}

static int
check_user(struct process *proc, struct user *user)
{
	return 0;
}

static int
check_rttime(struct process *proc)
{
	struct rlimit limit;
	if (prlimit(proc->pid, RLIMIT_RTTIME, NULL, &limit) == -1)
		return -errno;
	if (limit.rlim_max > props.rtt_usec_max)
		return -EPERM;
	return 0;
}

static int
make_thread_realtime(sd_bus_message *m, void *userdata, sd_bus_error *error)
{
	int rv;
	uint64_t tid;
	uint32_t priority;
	struct user *user = NULL;
	struct process *process = NULL;

	if ((rv = sd_bus_message_read(m, "tu", &tid, &priority)) < 0) {
		fprintf(stderr, "Failed to parse parameters: %s\n", strerror(-rv));
		return rv;
	}
	if ((rv = lookup(&user, &process, m, -1, tid)) < 0) {
		return rv;
	}
	if ((rv = check_user(process, user)) < 0 ||
	    (rv = check_rttime(process)) < 0) {
		return rv;
	}
	if ((rv = set_realtime(process, priority)) < 0) {
		fprintf(stderr, "set_realtime: %s\n", strerror(-rv));
		return rv;
	}
	return sd_bus_reply_method_return(m, "");
}

static int
make_thread_realtime_with_pid(sd_bus_message *m, void *userdata, sd_bus_error *error)
{
	pid_t pid;
	uint64_t tid;
	uint32_t priority;
	int rv;
	struct user *user = NULL;
	struct process *process = NULL;

	if ((rv = sd_bus_message_read(m, "ttu", &pid, &tid, &priority)) < 0) {
		fprintf(stderr, "Failed to parse parameters: %s\n", strerror(-rv));
		return rv;
	}
	if ((rv = lookup(&user, &process, m, pid, tid)) < 0) {
		return rv;
	}
	if ((rv = check_user(process, user)) < 0 ||
	    (rv = check_rttime(process)) < 0) {
		return rv;
	}
	if ((rv = set_realtime(process, priority)) < 0) {
		return rv;
	}
	return sd_bus_reply_method_return(m, "");
}

static int
set_high_priority(struct process *proc, int32_t priority)
{
	struct sched_param param = {0};

	if (priority < PRIO_MIN || priority >= PRIO_MAX)
		return -EINVAL;
	if (priority < props.min_nice_level)
		return -EPERM;

	if (_sched_setscheduler(proc->pid, SCHED_OTHER|SCHED_RESET_ON_FORK, &param) == -1)
		return -errno;
	if (setpriority(PRIO_PROCESS, proc->pid, priority) == -1)
		return -errno;

	fprintf(stderr, "Successfully made process %llu high priority at nice level %i.\n",
		(unsigned long long)proc->pid, priority);
	return 0;
}

static int
make_thread_high_priority(sd_bus_message *m, void *userdata, sd_bus_error *error)
{
	int rv;
	pid_t tid;
	int32_t priority;
	struct user *user = NULL;
	struct process *process = NULL;

	if ((rv = sd_bus_message_read(m, "ti", &tid, &priority)) < 0) {
		fprintf(stderr, "Failed to parse parameters: %s\n", strerror(-rv));
		return rv;
	}
	if ((rv = lookup(&user, &process, m, -1, tid)) < 0) {
		return rv;
	}
	if ((rv = check_user(process, user)) < 0) {
		return rv;
	}
	if ((rv = set_high_priority(process, priority)) < 0) {
		fprintf(stderr, "set_high_priority: %s\n", strerror(-rv));
		return rv;
	}
	return sd_bus_reply_method_return(m, "");
}

static int
make_thread_high_priority_with_pid(sd_bus_message *m, void *userdata, sd_bus_error *error)
{
	int rv;
	pid_t pid;
	pid_t tid;
	int32_t priority;
	struct user *user = NULL;
	struct process *process = NULL;

	if ((rv = sd_bus_message_read(m, "tti", &pid, &tid, &priority)) < 0) {
		fprintf(stderr, "Failed to parse parameters: %s\n", strerror(-rv));
		return rv;
	}
	if ((rv = lookup(&user, &process, m, pid, tid)) < 0) {
		return rv;
	}
	if ((rv = check_user(process, user)) < 0) {
		return rv;
	}
	if ((rv = set_high_priority(process, priority)) < 0) {
		return rv;
	}
	return sd_bus_reply_method_return(m, "");
}

static int
process_reset(struct process *proc)
{
	struct sched_param param = {0};
	int r = 0;

	if (_sched_setscheduler(proc->pid, SCHED_OTHER, &param) == -1) {
		fprintf(stderr, "Failed to demote process %d: %s\n", proc->pid, strerror(errno));
		r = -1;
	}
	if (setpriority(PRIO_PROCESS, proc->pid, 0) == -1) {
		fprintf(stderr, "Failed to demote process %d: %s\n", proc->pid, strerror(errno));
		r = -1;
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
	struct process *proc;
	fprintf(stderr, "Demoting known real-time threads.\n");
	size_t n = 0;
	TAILQ_FOREACH(proc, &processes, entries) {
		if (process_reset(proc) == 0)
			n++;
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
			offsetof(struct properties, rtt_usec_max),
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
	int rv;

	if ((epollfd = epoll_create1(EPOLL_CLOEXEC)) == -1) {
		fprintf(stderr, "Failed to create epoll instance: %s\n", strerror(errno));
		return 1;
	}

	if ((rv = sd_bus_default_system(&bus)) < 0) {
		fprintf(stderr, "Failed to connect to system bus: %s\n", strerror(-rv));
		return 1;
	}
	rv = sd_bus_add_object_vtable(bus, &slot, "/org/freedesktop/RealtimeKit1", "org.freedesktop.RealtimeKit1", rtkit_vtable, &props);
	if (rv < 0) {
		fprintf(stderr, "Failed to issue method call: %s\n", strerror(-rv));
		return 1;
	}
	if ((rv = sd_bus_request_name(bus, "org.freedesktop.RealtimeKit1", 0)) < 0) {
		fprintf(stderr, "Failed to aquire service name: %s\n", strerror(-rv));
		return 1;
	}
	int busfd = sd_bus_get_fd(bus);
	if (busfd < 0) {
		fprintf(stderr, "Failed to get bus file descriptor: %s\n", strerror(-busfd));
		return 1;
	}
	struct epoll_event event = {
		.events = EPOLLIN,
		.data.fd = busfd,
	};
	if (epoll_ctl(epollfd, EPOLL_CTL_ADD, busfd, &event) == -1) {
		fprintf(stderr, "Failed to add epoll event: %s\n", strerror(errno));
		return 1;
	}
	sigset_t mask;
	sigemptyset(&mask);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGTERM);
	if (sigprocmask(SIG_BLOCK, &mask, NULL) == -1) {
		fprintf(stderr, "Failed to block signals: %s\n", strerror(errno));
		return 1;
	}
	int sigfd = signalfd(-1, &mask, SFD_CLOEXEC);
	event.events = EPOLLIN;
	event.data.fd = sigfd;
	if (epoll_ctl(epollfd, EPOLL_CTL_ADD, sigfd, &event) == -1) {
		fprintf(stderr, "Failed to add epoll event: %s\n", strerror(errno));
		return 1;
	}

#ifdef HAVE_LIBSYSTEMD
	sd_notify(0, "STATUS=Running.");
#endif

	for (;;) {
		int n = epoll_wait(epollfd, &event, 1, -1);
		if (n == -1) {
			fprintf(stderr, "Failed to get epoll events: %s\n", strerror(errno));
			return 1;
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
			if ((rv = sd_bus_process(bus, NULL)) < 0) {
				fprintf(stderr, "Failed to process bus: %s\n", strerror(-rv));
				break;
			}
		} else {
			struct process *proc = event.data.ptr;
			TAILQ_REMOVE(&processes, proc, entries);
			if (epoll_ctl(epollfd, EPOLL_CTL_DEL, proc->pidfd, NULL) == -1) {
				fprintf(stderr, "Failed to delete epoll event: %s\n", strerror(errno));
			}
			close(proc->pidfd);
			free(proc);
		}
	}

	reset_known();

	sd_bus_slot_unref(slot);
	sd_bus_unref(bus);
	return 0;
}
