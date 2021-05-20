/* 
 * Copyright 2009 Lennart Poettering
 * Copyright 2010 David Henningsson <diwic@ubuntu.com>
 * 
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge,
 * publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */ 
#define _GNU_SOURCE
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>

#include "rtkit.h"

int
rtkit_get_max_realtime_priority(sd_bus *bus)
{
	int r, retval;

	r = sd_bus_get_property_trivial(
			bus,
			RTKIT_SERVICE_NAME,
			RTKIT_OBJECT_PATH,
			"org.freedesktop.RealtimeKit1",
			"MaxRealtimePriority",
			NULL,
			'i',
			&retval);
	if (r < 0)
		return r;

	return retval;
}

int
rtkit_get_min_nice_level(sd_bus *bus, int *min_nice_level)
{
	int r;

	r = sd_bus_get_property_trivial(
			bus,
			RTKIT_SERVICE_NAME,
			RTKIT_OBJECT_PATH,
			"org.freedesktop.RealtimeKit1",
			"MinNiceLevel",
			NULL,
			'i',
			min_nice_level);
	if (r < 0)
		return r;
	return 0;
}

long long
rtkit_get_rttime_usec_max(sd_bus *bus)
{
	long long retval;
	int r;

	r = sd_bus_get_property_trivial(
			bus,
			RTKIT_SERVICE_NAME,
			RTKIT_OBJECT_PATH,
			"org.freedesktop.RealtimeKit1",
			"RTTimeUSecMax",
			NULL,
			'x',
			&retval);
	if (r < 0)
		return r;

	return retval;
}

int
rtkit_make_realtime(sd_bus *bus, pid_t thread, int priority)
{
	int r;

	if (thread == 0)
		thread = gettid();

	r = sd_bus_call_method(
			bus,
			RTKIT_SERVICE_NAME,
			RTKIT_OBJECT_PATH,
			"org.freedesktop.RealtimeKit1",
			"MakeThreadRealtime",
			NULL,
			NULL,
			"tu",
			thread,
			priority);
	if (r < 0)
		return r;

	return 0;
}

int
rtkit_make_high_priority(sd_bus *bus, pid_t thread, int nice_level)
{
	int r;

	if (thread == 0)
		thread = gettid();

	r = sd_bus_call_method(
			bus,
			RTKIT_SERVICE_NAME,
			RTKIT_OBJECT_PATH,
			"org.freedesktop.RealtimeKit1",
			"MakeThreadHighPriority",
			NULL,
			NULL,
			"ti",
			thread,
			nice_level);
	if (r < 0)
		return r;

	return 0;
}
