/*
 * Copyright © 2018-2018 jouyouyun <jouyouwen717@gmail.com>
 *
 * Permission to use, copy, modify, distribute, and sell this software
 * and its documentation for any purpose is hereby granted without
 * fee, provided that the above copyright notice appear in all copies
 * and that both that copyright notice and this permission notice
 * appear in supporting documentation, and that the name of Red Hat
 * not be used in advertising or publicity pertaining to distribution
 * of the software without specific, written prior permission.  Red
 * Hat makes no representations about the suitability of this software
 * for any purpose.  It is provided "as is" without express or implied
 * warranty.
 *
 * THE AUTHORS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN
 * NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS
 * OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
 * NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <math.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "touch_wrapper.h"

#define TOUCH_MOTION_DIST_MAX 5

typedef struct _touch_post_list {
	unsigned int length;
	int fingers;
	int sent;
	double x;
	double y;
	double dx;
	double dy;
	timer_t id;
	DeviceIntPtr dev;
	ValuatorMask *m;
	struct touch_post_event** events;
} TouchPostList;

static void touch_event_post_touch(struct touch_post_event* post);
static void touch_post_list_free(void);
static void touch_post_list_post_touch(void);
static void touch_post_list_post_button(int is_press);

static int start_touch_timer(unsigned int duration);
static void stop_touch_timer(void);
static void thread_handler(union sigval v);

static TouchPostList touch_list = {
	.length = 0,
	.fingers = 0,
	.sent = 0,
	.events = NULL,
	.id = NULL,
};

static InputInfoPtr _pInfo = NULL;

int touch_post_list_append(struct touch_post_event* post)
{
	xf86IDrvMsg(post->pInfo, X_INFO, "[List Append] start, fingers: %d, sent: %d\n",
		    touch_list.fingers, touch_list.sent);
	if (touch_list.sent == 1) {
		// had sent
		return 0;
	}

	if (touch_list.fingers == 0) {
		// start, launch alarm
		touch_list.sent = 0;
		touch_list.x = post->x;
		touch_list.y = post->y;
		touch_list.dx = post->dx;
		touch_list.dy = post->dy;
		touch_list.dev = post->dev;
		touch_list.m = post->m;
		xf86IDrvMsg(post->pInfo, X_INFO, "[List Append] start timer:\n");
		_pInfo = post->pInfo;
		start_touch_timer(600);
	} else if (touch_list.fingers != 0 && touch_list.events == NULL) {
		// had canceled
		xf86IDrvMsg(post->pInfo, X_INFO, "[List Append] had cancel:\n");
		return -1;
	}

	if (post->type == XI_TouchBegin) {
		xf86IDrvMsg(post->pInfo, X_INFO, "[List Append] fingers added:\n");
		touch_list.fingers++;
		if (touch_list.fingers > 1) {
			// cancel alarm
			xf86IDrvMsg(post->pInfo, X_INFO, "[List Append] cancel timer and events:\n");
			stop_touch_timer();
			touch_post_list_post_touch();
			touch_post_list_free();
			return -1;
		}
	} else if (post->type == XI_TouchUpdate) {
		if (fabs(touch_list.x - post->x) > TOUCH_MOTION_DIST_MAX || fabs(touch_list.y - post->y) > TOUCH_MOTION_DIST_MAX) {
			// cancel alarm
			xf86IDrvMsg(post->pInfo, X_INFO, "[List Append] motion cancel timer and events:\n");
			stop_touch_timer();
			touch_post_list_post_touch();
			touch_post_list_free();
			return -1;
		}
	}
	xf86IDrvMsg(post->pInfo, X_INFO, "[List Append] start to append: %u\n", touch_list.length);
	struct touch_post_event** tmp = NULL;
	tmp = (struct touch_post_event**)realloc(touch_list.events,
						 (touch_list.length + 1) * sizeof(struct touch_post_event*));
	touch_list.events = tmp;
	touch_list.events[touch_list.length] = post;
	touch_list.length++;
	return 0;
}

int touch_post_list_end(void)
{
	int ret = -1;

	stop_touch_timer();
	if (touch_list.sent == 1) {
		ret = 0;
		touch_list.sent = 0;
		if (_pInfo) {
			xf86IDrvMsg(_pInfo, X_INFO, "Up post released button\n");
		}
		touch_post_list_post_button(0);
	} else {
		touch_post_list_post_touch();
	}

	_pInfo = NULL;
	touch_post_list_free();
	if (touch_list.fingers > 0) {
		touch_list.fingers--;
	}
	touch_list.dev = NULL;
	touch_list.m = NULL;
	return ret;
}

static void
touch_post_list_post_touch(void)
{
	unsigned int i = 0;
	for (; i < touch_list.length; i++) {
		touch_event_post_touch(touch_list.events[i]);
	}
	touch_post_list_free();
}

static void
touch_post_list_post_button(int is_press)
{
	// right button
	unsigned int button = 3;
	if (_pInfo) {
		xf86IDrvMsg(_pInfo, X_INFO, "[Button] pressed: %d, dx: %f, dy: %f\n",
			    is_press, touch_list.dx, touch_list.dy);
	}
	if (touch_list.dev == NULL || touch_list.m == NULL) {
		return;
	}
	/* xf86PostButtonEvent(touch_list.dev, Relative, button, */
	/* 		    is_press, 0, 2, (int)(touch_list.dx), (int)(touch_list.dy)); */
	valuator_mask_zero(touch_list.m);
	valuator_mask_set_double(touch_list.m, 0, touch_list.dx);
	valuator_mask_set_double(touch_list.m, 1, touch_list.dy);
	xf86PostButtonEventM(touch_list.dev, Relative, button,
			    is_press, touch_list.m);
	touch_post_list_free();
}

static void
touch_event_post_touch(struct touch_post_event* post)
{
	valuator_mask_zero(post->m);
	if (post->type != XI_TouchEnd) {
		valuator_mask_set_double(post->m, 0, post->dx);
		valuator_mask_set_double(post->m, 1, post->dy);
	}
	xf86PostTouchEvent(post->dev, post->slot, post->type, 0, post->m);
}

static void
touch_post_list_free(void)
{
	if (touch_list.events == NULL) {
		return;
	}

	unsigned int i = 0;
	for (; i < touch_list.length; i++) {
		free(touch_list.events[i]);
	}

	free(touch_list.events);
	touch_list.events = NULL;
	touch_list.length = 0;
}

static int
start_touch_timer(unsigned int duration)
{
	int ret;
	struct sigevent sev;
	struct itimerspec its;

	memset(&sev, 0, sizeof(struct sigevent));
	sev.sigev_notify = SIGEV_THREAD;
	sev.sigev_notify_function = thread_handler;
	ret = timer_create(CLOCK_REALTIME, &sev, &touch_list.id);
	if (_pInfo) {
		xf86IDrvMsg(_pInfo, X_INFO, "[Timer] create timer result: %d\n", ret);
	}
	if (ret == -1) {
		return ret;
	}

	memset(&its, 0, sizeof(struct itimerspec));
	if (duration >= 1000) {
		its.it_value.tv_sec = duration / 1000;
		its.it_value.tv_nsec = (duration%1000) * 100000;
	} else {
		its.it_value.tv_nsec = duration * 100000;
	}
	ret = timer_settime(touch_list.id, 0, &its, NULL);
	if (_pInfo) {
		xf86IDrvMsg(_pInfo, X_INFO, "[Timer] set timer result: %d\n", ret);
	}
	if (ret == -1) {
		stop_touch_timer();
		return ret;
	}
	if (_pInfo) {
		xf86IDrvMsg(_pInfo, X_INFO, "Timer id: %p\n", touch_list.id);
	}
	return 0;
}

static void
stop_touch_timer(void)
{
	if (touch_list.id == NULL) {
		return;
	}
	timer_delete(touch_list.id);
	touch_list.id = NULL;
}

static void
thread_handler(union sigval v)
{
	if (_pInfo) {
		xf86IDrvMsg(_pInfo, X_INFO, "Timer recieved\n");
	}
	touch_post_list_post_button(1);
	touch_list.sent = 1;
}
