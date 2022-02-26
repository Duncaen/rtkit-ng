/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Copyright (C) 2009 Lennart Poettering
 * Copyright (C) 2022 Duncan Overbruck <mail@duncano.de>
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

#include <sys/resource.h>

#include <errno.h>
#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "rtkit.h"

#ifndef SCHED_RESET_ON_FORK
#define SCHED_RESET_ON_FORK 0x40000000
#endif

#ifndef RLIMIT_RTTIME
#define RLIMIT_RTTIME 15
#endif

static int max_realtime_priority;
#define MAX_PRIME 1000000
/* #define MAX_PRIME 1000 */

static void *
work(void *userdata)
{
	sd_bus *bus;
	int r;

	fprintf(stderr, "started thread\n");

	if ((r = sd_bus_default_system(&bus)) < 0) {
		fprintf(stderr, "Failed to connect to system bus: %s\n", strerror(-r));
		return NULL;
	}
	fprintf(stderr, "got system bus\n");

	if ((r = rtkit_make_realtime(bus, 0, max_realtime_priority)) < 0) {
		fprintf(stderr, "Failed to become realtime: %s\n", strerror(-r));
		return NULL;
	}
	fprintf(stderr, "successfuly made realtime\n");

	unsigned long i, num, primes = 0;
	for (num = 1; num <= MAX_PRIME; ++num) {
		for (i = 2; (i <= num) && (num % i != 0); ++i);
		if (i == num)
			++primes;
		usleep(1);
	}
	fprintf(stderr, "Calculated %lu primes.\n", primes);
	return NULL;
}

int
main(void)
{
	struct rlimit rlim = {0};
	sd_bus *bus;
	int r;
	fprintf(stderr, "pid=%u\n", getpid());

	if ((r = sd_bus_default_system(&bus)) < 0) {
		fprintf(stderr, "Failed to connect to system bus: %s\n", strerror(-r));
		return 1;
	}

	if ((max_realtime_priority = rtkit_get_max_realtime_priority(bus)) < 0) {
		fprintf(stderr, "Failed to retrieve max realtime priority: %s\n", strerror(-max_realtime_priority));
		return 1;
	}
	printf("Max realtime priority is: %d\n", max_realtime_priority);

	rlim.rlim_cur = rlim.rlim_max = 200000ULL; /* 200ms */
	if ((setrlimit(RLIMIT_RTTIME, &rlim) < 0)) {
		fprintf(stderr, "Failed to set RLIMIT_RTTIME: %s\n", strerror(errno));
		return 1;
	}


	pthread_t threads[32] = {0};
	size_t nthreads = sizeof(threads)/sizeof(threads[0]);
	for (int i = 0; i < nthreads; i++) {
		fprintf(stderr, "starting thread: %d\n", i);
		if ((r = -pthread_create(&threads[i], NULL, work, NULL)) < 0) {
			fprintf(stderr, "pthread_create: %s\n", strerror(-r));
			return 1;
		}
	}
	/* sleep(1000); */
	for (int i = 0; i < nthreads; i++) {
		pthread_join(threads[i], NULL);
	}

	sd_bus_unref(bus);

	return 0;
}
