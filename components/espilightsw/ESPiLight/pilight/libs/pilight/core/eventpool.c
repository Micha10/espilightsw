/*
	Copyright (C) 2015 - 2016 CurlyMo

  This Source Code Form is subject to the terms of the Mozilla Public
  License, v. 2.0. If a copy of the MPL was not distributed with this
  file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

#include "eventpool.h"

#include <sys/types.h>
#ifdef _WIN32
	#if _WIN32_WINNT < 0x0501
		#undef _WIN32_WINNT
		#define _WIN32_WINNT 0x0501
	#endif
	#define WIN32_LEAN_AND_MEAN
	#include <winsock2.h>
	#include <ws2tcpip.h>
	#include <mstcpip.h>
	#include <windows.h>
	struct pollfd {
		int fd;
		short events;
		short revents;
	};
	#define POLLIN	0x0001
	#define POLLPRI	0x0002
	#define POLLOUT	0x0004
#else
	#include <sys/socket.h>
	#include <arpa/inet.h>
	#include <poll.h>
	#include <unistd.h>
#endif
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/syscall.h>
#include <assert.h>

#include "log.h"
#include "../../libuv/uv.h"
#include "mem.h"
#include "network.h"

static uv_async_t *async_event_req = NULL;

typedef struct eventqueue_t {
	int reason;
	void *(*done)(void *);
	void *data;
	struct eventqueue_t *next;
} eventqueue_t;

typedef struct eventqueue_data_t {
	void *data;
	void *userdata;
} eventqueue_data_t;

typedef struct thread_list_t {
	int main;
	char *name;
	void *gc;
	uv_work_t *work_req;
	uv_work_cb work_cb;
	uv_after_work_cb after_work_cb;
	struct thread_list_t *next;
} thread_list_t;

static struct thread_list_t *thread_list = NULL;

static uv_mutex_t thread_lock;
static uv_async_t *thread_async_req = NULL;

static int nrlisteners[REASON_END+10000] = {0};
static struct eventqueue_t *eventqueue = NULL;

static int threads = EVENTPOOL_NO_THREADS;
static uv_mutex_t listeners_lock;
// static pthread_mutexattr_t listeners_attr;
static int lockinit = 0;
static int eventpoolinit = 0;

static struct eventpool_listener_t *eventpool_listeners = NULL;

static struct reasons_t {
	int number;
	char *reason;
	int priority;
} reasons[REASON_END+1] = {
	{	REASON_SEND_CODE, 						"REASON_SEND_CODE",							0 },
	{	REASON_CONTROL_DEVICE, 				"REASON_CONTROL_DEVICE",				0 },
	{	REASON_CODE_SENT, 						"REASON_CODE_SENT",							0 },
	{	REASON_SOCKET_SEND,						"REASON_CODE_SEND_FAIL",				0 },
	{	REASON_SOCKET_SEND,						"REASON_CODE_SEND_SUCCESS",			0 },
	{	REASON_CODE_RECEIVED, 				"REASON_CODE_RECEIVED",					0 },
	{	REASON_RECEIVED_PULSETRAIN, 	"REASON_RECEIVED_PULSETRAIN",		0 },
	{	REASON_RECEIVED_OOK,				 	"REASON_RECEIVED_OOK",					0 },
	{	REASON_RECEIVED_API,				 	"REASON_RECEIVED_API",					0 },
	{	REASON_BROADCAST, 						"REASON_BROADCAST",							0 },
	{	REASON_BROADCAST_CORE, 				"REASON_BROADCAST_CORE",				0 },
	{	REASON_FORWARD, 							"REASON_FORWARD",								0 },
	{	REASON_CONFIG_UPDATE, 				"REASON_CONFIG_UPDATE",					0 },
	{	REASON_CONFIG_UPDATED, 				"REASON_CONFIG_UPDATED",				0 },
	{	REASON_SOCKET_RECEIVED, 			"REASON_SOCKET_RECEIVED",				0 },
	{	REASON_SOCKET_DISCONNECTED,	 	"REASON_SOCKET_DISCONNECTED",		0 },
	{	REASON_SOCKET_CONNECTED,			"REASON_SOCKET_CONNECTED",			0 },
	{	REASON_SOCKET_SEND,						"REASON_SOCKET_SEND",						0 },
	{	REASON_SSDP_RECEIVED, 				"REASON_SSDP_RECEIVED",					0 },
	{	REASON_SSDP_RECEIVED_FREE,		"REASON_SSDP_RECEIVED_FREE",		0 },
	{	REASON_SSDP_DISCONNECTED,			"REASON_SSDP_DISCONNECTED",			0 },
	{	REASON_SSDP_CONNECTED,				"REASON_SSDP_CONNECTED",				0 },
	{	REASON_WEBSERVER_CONNECTED,		"REASON_WEBSERVER_CONNECTED",		0 },
	{	REASON_DEVICE_ADDED,					"REASON_DEVICE_ADDED",					0 },
	{	REASON_DEVICE_ADAPT,					"REASON_DEVICE_ADAPT",					0 },
	{	REASON_ADHOC_MODE,						"REASON_ADHOC_MODE",						0 },
	{	REASON_ADHOC_CONNECTED,				"REASON_ADHOC_CONNECTED",				0 },
	{	REASON_ADHOC_CONFIG_RECEIVED,	"REASON_ADHOC_CONFIG_RECEIVED",	0 },
	{	REASON_ADHOC_DATA_RECEIVED,		"REASON_ADHOC_DATA_RECEIVED",		0 },
	{	REASON_ADHOC_UPDATE_RECEIVED,	"REASON_ADHOC_UPDATE_RECEIVED",	0 },
	{	REASON_ADHOC_DISCONNECTED,		"REASON_ADHOC_DISCONNECTED",		0 },
	{	REASON_SEND_BEGIN,						"REASON_SEND_BEGIN",						0 },
	{	REASON_SEND_END,							"REASON_SEND_END",							0 },
	{	REASON_ARP_FOUND_DEVICE,			"REASON_ARP_FOUND_DEVICE",			0 },
	{	REASON_ARP_LOST_DEVICE,				"REASON_ARP_LOST_DEVICE", 			0 },
	{	REASON_ARP_CHANGED_DEVICE,		"REASON_ARP_CHANGED_DEVICE",		0	},
	{	REASON_LOG,										"REASON_LOG",										0 },
	{	REASON_END,										"REASON_END",										0 }
};

static void safe_thread_loop(uv_async_t *handle) {
	const uv_thread_t pth_cur_id = uv_thread_self();
	assert(uv_thread_equal(&pth_main_id, &pth_cur_id));

	uv_mutex_lock(&thread_lock);

	struct thread_list_t *node = NULL;
	node = thread_list;

	if(node != NULL) {
		if(node->main == 1) {
			node->work_cb(node->work_req);
			node->after_work_cb(node->work_req, 0);
		} else {
			uv_queue_work(uv_default_loop(), node->work_req, node->name, node->work_cb, node->after_work_cb);
		}

		thread_list = thread_list->next;
		FREE(node);
	}

	if(thread_list != NULL) {
		uv_async_send(thread_async_req);
	}
	uv_mutex_unlock(&thread_lock);
}

void uv_queue_work_s(uv_work_t *req, char *name, int main, uv_work_cb work_cb, uv_after_work_cb after_work_cb) {
	uv_mutex_lock(&thread_lock);

	struct thread_list_t *node = MALLOC(sizeof(struct thread_list_t));
	node->name = name;
	node->work_cb = work_cb;
	node->work_req = req;
	node->after_work_cb = after_work_cb;
	node->main = main;
	node->next = thread_list;
	thread_list = node;

	uv_async_send(thread_async_req);

	uv_mutex_unlock(&thread_lock);
}

static void fib_free(uv_work_t *req, int status) {
	FREE(req->data);
	FREE(req);
}

static void fib(uv_work_t *req) {
	struct threadpool_data_t *data = req->data;
	struct eventqueue_data_t *ndata = data->userdata;

	if(data->func != NULL) {
		data->func(data->reason, ndata->data, ndata->userdata);
	}

	int x = 0;
	if(data->ref != NULL) {
		x = uv_sem_trywait(data->ref);
	}
	if((data->ref == NULL) || (x == UV__EAGAIN)) {
		if(data->done != NULL && data->reason != REASON_END+10000) {
			data->done(ndata->data);
		}
		if(data->ref != NULL) {
			FREE(data->ref);
		}
	}
	FREE(ndata);
	// FREE(req->data);
}

void eventpool_callback_remove(struct eventpool_listener_t *node) {
	if(lockinit == 1) {
		uv_mutex_lock(&listeners_lock);
	}

	struct eventpool_listener_t *currP, *prevP;
	int reason = node->reason;

	prevP = NULL;

	for(currP = eventpool_listeners; currP != NULL; prevP = currP, currP = currP->next) {
		if(currP == node) {
			if(prevP == NULL) {
				eventpool_listeners = currP->next;
			} else {
				prevP->next = currP->next;
			}

#ifdef _WIN32
			InterlockedDecrement(&nrlisteners[reason]);
#else
			__sync_add_and_fetch(&nrlisteners[reason], -1);
#endif

			FREE(currP);
			break;
		}
	}
	if(lockinit == 1) {
		uv_mutex_unlock(&listeners_lock);
	}
}

void *eventpool_callback(int reason, void *(*func)(int, void *, void *), void *userdata) {
	if(lockinit == 1) {
		uv_mutex_lock(&listeners_lock);
	}
	struct eventpool_listener_t *node = MALLOC(sizeof(struct eventpool_listener_t));
	if(node == NULL) {
		OUT_OF_MEMORY /*LCOV_EXCL_LINE*/
	}
	node->func = func;
	node->reason = reason;
	node->next = NULL;
	node->userdata = userdata;

	node->next = eventpool_listeners;
	eventpool_listeners = node;

#ifdef _WIN32
	InterlockedIncrement(&nrlisteners[reason]);
#else
	__sync_add_and_fetch(&nrlisteners[reason], 1);
#endif

	if(lockinit == 1) {
		uv_mutex_unlock(&listeners_lock);
	}

	return node;
}

void eventpool_trigger(int reason, void *(*done)(void *), void *data) {
	if(eventpoolinit == 0) {
		return;
	}

#ifdef _WIN32
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
#else
	struct sched_param sched;
	memset(&sched, 0, sizeof(sched));
	sched.sched_priority = 80;
	pthread_setschedparam(pthread_self(), SCHED_RR, &sched);
#endif
	int eventqueue_size = 0;

	struct eventqueue_t *node = MALLOC(sizeof(struct eventqueue_t));
	if(node == NULL) {
		OUT_OF_MEMORY /*LCOV_EXCL_LINE*/
	}
	memset(node, 0, sizeof(struct eventqueue_t));
	node->reason = reason;
	node->done = done;
	node->data = data;

	uv_mutex_lock(&listeners_lock);
	struct eventqueue_t *tmp = eventqueue;
	if(tmp != NULL) {
		while(tmp->next != NULL) {
			eventqueue_size++;
			tmp = tmp->next;
		}
		tmp->next = node;
		node = tmp;
	} else {
		node->next = eventqueue;
		eventqueue = node;
	}

	// /*
	 // * If the eventqueue size is above
	 // * 50 entries then there must be a bug
	 // * at the trigger side.
	 // */
	// assert(eventqueue_size < 50);

	uv_async_send(async_event_req);

	uv_mutex_unlock(&listeners_lock);
}

static void eventpool_execute(uv_async_t *handle) {
	/*
	 * Make sure we execute in the main thread
	 */
	const uv_thread_t pth_cur_id = uv_thread_self();
	assert(uv_thread_equal(&pth_main_id, &pth_cur_id));

	struct threadpool_tasks_t **node = NULL;
	int nrlisteners1[REASON_END+10000] = {0};
	int nr1 = 0, nrnodes = 16, nrnodes1 = 0, i = 0;

	if((node = MALLOC(sizeof(struct threadpool_tasks_t *)*nrnodes)) == NULL) {
		OUT_OF_MEMORY /*LCOV_EXCL_LINE*/
	}

	uv_mutex_lock(&listeners_lock);

	struct eventqueue_t *queue = NULL;
	while(eventqueue) {
		queue = eventqueue;
		uv_sem_t *ref = NULL;

#ifdef _WIN32
		if((nr1 = InterlockedExchangeAdd(&nrlisteners[queue->reason], 0)) == 0) {
#else
		if((nr1 = __sync_add_and_fetch(&nrlisteners[queue->reason], 0)) == 0) {
#endif
			if(queue->done != NULL) {
				queue->done((void *)queue->data);
			}
		} else {
			if(threads == EVENTPOOL_THREADED) {
				if((ref = MALLOC(sizeof(uv_sem_t))) == NULL) {
					OUT_OF_MEMORY /*LCOV_EXCL_LINE*/
				}
				uv_sem_init(ref, nr1-1);
			}

			struct eventpool_listener_t *listeners = eventpool_listeners;
			if(listeners == NULL) {
				if(queue->done != NULL) {
					queue->done((void *)queue->data);
				}
			}

			while(listeners) {
				if(listeners->reason == queue->reason) {
					if(nrnodes1 == nrnodes) {
						nrnodes *= 2;
						/*LCOV_EXCL_START*/
						if((node = REALLOC(node, sizeof(struct threadpool_tasks_t *)*nrnodes)) == NULL) {
							OUT_OF_MEMORY
						}
						/*LCOV_EXCL_STOP*/
					}
					if((node[nrnodes1] = MALLOC(sizeof(struct threadpool_tasks_t))) == NULL) {
						OUT_OF_MEMORY /*LCOV_EXCL_LINE*/
					}
					node[nrnodes1]->func = listeners->func;
					if((node[nrnodes1]->userdata = MALLOC(sizeof(struct eventqueue_data_t))) == NULL) {
						OUT_OF_MEMORY
					}
					struct eventqueue_data_t *data = node[nrnodes1]->userdata;
					data->userdata = listeners->userdata;
					data->data = queue->data;
					node[nrnodes1]->done = queue->done;
					node[nrnodes1]->ref = ref;
					node[nrnodes1]->reason = listeners->reason;
					nrnodes1++;
					if(threads == EVENTPOOL_THREADED) {
						nrlisteners1[queue->reason]++;
					}
				}
				listeners = listeners->next;
			}
		}
		eventqueue = eventqueue->next;
		FREE(queue);
	}
	uv_mutex_unlock(&listeners_lock);

	if(nrnodes1 > 0) {
		for(i=0;i<nrnodes1;i++) {
			struct eventqueue_data_t *data = node[i]->userdata;
			if(threads == EVENTPOOL_NO_THREADS) {
				nrlisteners1[node[i]->reason]++;
#ifdef _WIN32
				int nr = InterlockedExchangeAdd(&nrlisteners[node[i]->reason]);
#else
				int nr = __sync_add_and_fetch(&nrlisteners[node[i]->reason], 0);
#endif
				node[i]->func(node[i]->reason, data->data, data->userdata);

				if(nrlisteners1[node[i]->reason] == nr) {
					if(node[i]->done != NULL) {
						node[i]->done((void *)data->data);
					}
					nrlisteners1[node[i]->reason] = 0;
				}
				FREE(data);
			} else {
				struct threadpool_data_t *tpdata = NULL;
				tpdata = MALLOC(sizeof(struct threadpool_data_t));
				if(tpdata == NULL) {
					OUT_OF_MEMORY /*LCOV_EXCL_LINE*/
				}

				tpdata->userdata = node[i]->userdata;
				tpdata->func = node[i]->func;
				tpdata->done = node[i]->done;
				tpdata->ref = node[i]->ref;
				tpdata->reason = node[i]->reason;

				if(node[i]->reason >= 10000) {
					node[i]->reason -= 10000;
				}

				tpdata->priority = reasons[node[i]->reason].priority;

				uv_work_t *tp_work_req = MALLOC(sizeof(uv_work_t));
				if(tp_work_req == NULL) {
					OUT_OF_MEMORY /*LCOV_EXCL_LINE*/
				}
				tp_work_req->data = tpdata;
				if(uv_queue_work(uv_default_loop(), tp_work_req, reasons[node[i]->reason].reason, fib, fib_free) < 0) {
					if(node[i]->done != NULL) {
						node[i]->done((void *)data->data);
					}
					FREE(data);
					FREE(tpdata);
					FREE(node[i]->ref);
				}
			}
			FREE(node[i]);
		}
	}
	for(i=0;i<REASON_END+10000;i++) {
		nrlisteners1[i] = 0;
	}
	FREE(node);
	uv_mutex_lock(&listeners_lock);
	if(eventqueue != NULL) {
		uv_async_send(async_event_req);
	}
	uv_mutex_unlock(&listeners_lock);
}

int eventpool_gc(void) {
	if(lockinit == 1) {
		uv_mutex_lock(&listeners_lock);
	}
	struct eventqueue_t *queue = NULL;
	while(eventqueue) {
		queue = eventqueue;
		if(eventqueue->data != NULL && eventqueue->done != NULL) {
			eventqueue->done(eventqueue->data);
		}
		eventqueue = eventqueue->next;
		FREE(queue);
	}
	struct eventpool_listener_t *listeners = NULL;
	while(eventpool_listeners) {
		listeners = eventpool_listeners;
		eventpool_listeners = eventpool_listeners->next;
		FREE(listeners);
	}
	if(eventpool_listeners != NULL) {
		FREE(eventpool_listeners);
	}
	threads = EVENTPOOL_NO_THREADS;

	int i = 0;
	for(i=0;i<REASON_END+10000;i++) {
		nrlisteners[i] = 0;
	}

	if(lockinit == 1) {
		uv_mutex_unlock(&listeners_lock);
	}

	uv_mutex_lock(&thread_lock);
	struct thread_list_t *node = NULL;
	while(thread_list) {
		node = thread_list;
		thread_list = thread_list->next;
		node->after_work_cb(node->work_req, -99);
		FREE(node);
	}
	uv_mutex_unlock(&thread_lock);

	eventpoolinit = 0;
	return 0;
}

static void eventpool_update_poll(uv_poll_t *req) {
	struct uv_custom_poll_t *custom_poll_data = NULL;
	struct iobuf_t *send_io = NULL;
	int action = 0, r = 0;

	custom_poll_data = req->data;
	if(custom_poll_data == NULL) {
		return;
	}

	if(uv_is_closing((uv_handle_t *)req)) {
		return;
	}

	assert(custom_poll_data->threadid == syscall(__NR_gettid));

	send_io = &custom_poll_data->send_iobuf;

	if(custom_poll_data->doread == 1) {
		action |= UV_READABLE;
	}

	if(custom_poll_data->dowrite == 1) {
		action |= UV_WRITABLE;
	}

	if(custom_poll_data->doclose == 1 && send_io->len == 0) {
		custom_poll_data->doclose = 2;
		if(custom_poll_data->close_cb != NULL) {
			custom_poll_data->close_cb(req);
		}
		if(!uv_is_closing((uv_handle_t *)req)) {
			uv_poll_stop(req);
		}
	} else if(custom_poll_data->action != action && action > 0) {
		uv_poll_start(req, action, uv_custom_poll_cb);
		if(r != 0) {
			/*LCOV_EXCL_START*/
			logprintf(LOG_ERR, "uv_poll_start: %s", uv_strerror(r));
			return;
			/*LCOV_EXCL_STOP*/
		}
		custom_poll_data->action = action;
	}
}

void iobuf_init(struct iobuf_t *iobuf, size_t initial_size) {
	iobuf->len = iobuf->size = 0;
	iobuf->buf = NULL;
	uv_mutex_init(&iobuf->lock);
}

void iobuf_remove(struct iobuf_t *io, size_t n) {
	uv_mutex_lock(&io->lock);
  if(n > 0 && n <= io->len) {
    memmove(io->buf, io->buf + n, io->len - n);
    io->len -= n;
		if(io->len == 0) {
			FREE(io->buf);
			io->buf = NULL;
		} else {
			io->buf = REALLOC(io->buf, io->len + 1);
			io->buf[io->len] = 0;
		}
		io->size = io->len;
  }
	uv_mutex_unlock(&io->lock);
}

size_t iobuf_append_remove(struct iobuf_t *a, struct iobuf_t *b) {
	char *p = NULL;
	size_t i = -1;

	uv_mutex_lock(&b->lock);
	uv_mutex_lock(&a->lock);
	i = a->len;

	assert(b != NULL);
	assert(b->len <= b->size);

	if(a->len <= 0) {
	} else if(b->len + a->len <= b->size) {
		memcpy(b->buf + b->len, a->buf, a->len);
		b->len += a->len;
	} else if((p = REALLOC(b->buf, b->len + a->len + 1)) != NULL) {
		b->buf = p;
		memset(&b->buf[b->len], 0, b->len + a->len);
		memcpy(b->buf + b->len, a->buf, a->len);
		b->len += a->len;
		b->size = b->len;
	}

	if(i > 0) {
		if(a->buf != NULL) {
			FREE(a->buf);
		}
		a->len = a->size = 0;
	}

	uv_mutex_unlock(&a->lock);
	uv_mutex_unlock(&b->lock);

	return i;
}

size_t iobuf_append(struct iobuf_t *io, const void *buf, int len) {
  char *p = NULL;

	uv_mutex_lock(&io->lock);
  assert(io != NULL);
  assert(io->len <= io->size);

  if(len <= 0) {
  } else if(io->len + len <= io->size) {
    memcpy(io->buf + io->len, buf, len);
    io->len += len;
  } else if((p = REALLOC(io->buf, io->len + len + 1)) != NULL) {
    io->buf = p;
		memset(&io->buf[io->len], 0, len + 1);
    memcpy(io->buf + io->len, buf, len);
    io->len += len;
    io->size = io->len;
  } else {
    len = 0;
  }
	uv_mutex_unlock(&io->lock);

  return len;
}

/*LCOV_EXCL_START*/
static void my_debug(void *ctx, int level, const char *file, int line, const char *str) {
	printf("%s:%04d: %s", file, line, str );
}
/*LCOV_EXCL_STOP*/

void uv_custom_poll_cb(uv_poll_t *req, int status, int events) {
	/*
	 * Make sure we execute in the main thread
	 */
	const uv_thread_t pth_cur_id = uv_thread_self();
	assert(uv_thread_equal(&pth_main_id, &pth_cur_id));

	struct uv_custom_poll_t *custom_poll_data = NULL;
	struct iobuf_t *send_io = NULL;
	char buffer[BUFFER_SIZE];
	uv_os_fd_t fd = 0;
	long int fromlen = 0;
	int r = 0, n = 0, err = 0;

	custom_poll_data = req->data;
	if(custom_poll_data == NULL) {
		uv_poll_stop(req);
		return;
	}

	/*
	 * Status == -9: Socket is unreachable
	 * Events == 0: Client-end got disconnected
	 */
	if(status < 0 || events == 0) {
		if(status == -9) {
			logprintf(LOG_ERR, "uv_custom_poll_cb: socket not responding");
		} else {
			logprintf(LOG_ERR, "uv_custom_poll_cb: %s", uv_strerror(status));
		}
		custom_poll_data->doclose = 1;
		custom_poll_data->doread = 0;
		// goto end;
	}

	custom_poll_data->started = 1;

	send_io = &custom_poll_data->send_iobuf;

	memset(&buffer, 0, BUFFER_SIZE);

	if(uv_is_closing((uv_handle_t *)req)) {
		return;
	}

	r = uv_fileno((uv_handle_t *)req, &fd);
	if(r != 0) {
		logprintf(LOG_ERR, "uv_fileno: %s", uv_strerror(r));
		return;
	}

	if(custom_poll_data->is_ssl == 1 && custom_poll_data->ssl.init == 0) {
		custom_poll_data->ssl.init = 1;
		struct mbedtls_ssl_config *ssl_conf = &ssl_client_conf;
		if(custom_poll_data->is_server == 1) {
			custom_poll_data->ssl.handshake = 1;
			ssl_conf = &ssl_server_conf;
		}
		if((r = mbedtls_ssl_setup(&custom_poll_data->ssl.ctx, ssl_conf)) < 0) {
			mbedtls_strerror(r, (char *)&buffer, BUFFER_SIZE);
			logprintf(LOG_ERR, "mbedtls_ssl_setup: %s", buffer);
			FREE(req);
			return;
		}

		if((r = mbedtls_ssl_session_reset(&custom_poll_data->ssl.ctx)) < 0) {
			mbedtls_strerror(r, (char *)&buffer, BUFFER_SIZE);
			logprintf(LOG_ERR, "mbedtls_ssl_session_reset: %s", buffer);
			FREE(req);
			return;
		}
		// mbedtls_debug_set_threshold(2);
		mbedtls_ssl_set_bio(&custom_poll_data->ssl.ctx, &fd, mbedtls_net_send, mbedtls_net_recv, NULL);
		mbedtls_ssl_conf_dbg(ssl_conf, my_debug, stdout);
		if(custom_poll_data->host != NULL) {
			mbedtls_ssl_set_hostname(&custom_poll_data->ssl.ctx, custom_poll_data->host);
		}
	}

	if(custom_poll_data->is_ssl == 1 && custom_poll_data->ssl.handshake == 0) {
		n = mbedtls_ssl_handshake(&custom_poll_data->ssl.ctx);
		if(n == MBEDTLS_ERR_SSL_WANT_READ) {
			custom_poll_data->doread = 1;
			custom_poll_data->dowrite = 0;
			goto end;
		} else if(n == MBEDTLS_ERR_SSL_WANT_WRITE) {
			/*LCOV_EXCL_START*/
			custom_poll_data->dowrite = 1;
			custom_poll_data->doread = 0;
			goto end;
			/*LCOV_EXCL_STOP*/
		}
		if(n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) {
		} else if(n < 0) {
			/*LCOV_EXCL_START*/
			mbedtls_strerror(n, (char *)&buffer, BUFFER_SIZE);
			logprintf(LOG_NOTICE, "mbedtls_ssl_handshake: %s", buffer);
			uv_poll_stop(req);
			return;
			/*LCOV_EXCL_STOP*/
		} else {
			custom_poll_data->ssl.handshake = 1;
		}
		custom_poll_data->dowrite = 1;
		goto end;
	}

	if(events & UV_WRITABLE) {
		if(send_io->len > 0) {
			if(custom_poll_data->is_ssl == 1) {
				n = mbedtls_ssl_write(&custom_poll_data->ssl.ctx, (unsigned char *)send_io->buf, send_io->len);
				if(n == MBEDTLS_ERR_SSL_WANT_READ) {
					/*LCOV_EXCL_START*/
					custom_poll_data->doread = 1;
					custom_poll_data->dowrite = 0;
					goto end;
					/*LCOV_EXCL_STOP*/
				} else if(n == MBEDTLS_ERR_SSL_WANT_WRITE) {
					/*LCOV_EXCL_START*/
					custom_poll_data->dowrite = 1;
					custom_poll_data->doread = 0;
					goto end;
					/*LCOV_EXCL_STOP*/
				}
				if(n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) {
				} else if(n < 0) {
					/*LCOV_EXCL_START*/
					mbedtls_strerror(n, (char *)&buffer, BUFFER_SIZE);
					logprintf(LOG_NOTICE, "mbedtls_ssl_handshake: %s", buffer);
					uv_poll_stop(req);
					return;
					/*LCOV_EXCL_STOP*/
				}
			} else {
				n = (int)send((unsigned int)fd, send_io->buf, send_io->len, 0);
				err = errno;
			}
			if(n > 0) {
				iobuf_remove(send_io, n);
				if(send_io->len > 0) {
					custom_poll_data->dowrite = 1;
				} else {
					custom_poll_data->dowrite = 0;
					if(custom_poll_data->doclose == 1 && send_io->len == 0) {
						custom_poll_data->doread = 0;
						goto end;
					} else {
						custom_poll_data->dowrite = 0;
						if(custom_poll_data->write_cb != NULL) {
							custom_poll_data->write_cb(req);
						}
					}
				}
			} else if(n == 0) {
			} else if(custom_poll_data->is_ssl == 0 && n < 0 && err != EAGAIN && err != EINTR) {
				if(err == ECONNRESET) {
					uv_poll_stop(req);
					return;
				} else {
					uv_poll_stop(req);
					return;
				}
			}
		} else {
			custom_poll_data->dowrite = 0;
			if(custom_poll_data->doclose == 1 && send_io->len == 0) {
				custom_poll_data->doread = 0;
				goto end;
			} else {
				custom_poll_data->dowrite = 0;
				if(custom_poll_data->write_cb != NULL) {
					custom_poll_data->write_cb(req);
				}
			}
		}
	}

	if(send_io->len > 0) {
		custom_poll_data->dowrite = 1;
	}

	if(events & UV_READABLE) {
		if(custom_poll_data->is_ssl == 1) {
			n = mbedtls_ssl_read(&custom_poll_data->ssl.ctx, (unsigned char *)buffer, BUFFER_SIZE);
			if(n == MBEDTLS_ERR_SSL_WANT_READ) {
				custom_poll_data->doread = 1;
				custom_poll_data->dowrite = 0;
				goto end;
			} else if(n == MBEDTLS_ERR_SSL_WANT_WRITE) {
				/*LCOV_EXCL_START*/
				custom_poll_data->dowrite = 1;
				custom_poll_data->doread = 0;
				goto end;
				/*LCOV_EXCL_STOP*/
			} else if(n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
				custom_poll_data->doread = 0;
				if(custom_poll_data->read_cb != NULL) {
					ssize_t len = 0;
					len = custom_poll_data->read_cb(req, custom_poll_data->recv_iobuf.len, (const char *)custom_poll_data->recv_iobuf.buf);
					if(!uv_is_closing((uv_handle_t *)req)) {
						iobuf_remove(&custom_poll_data->recv_iobuf, len);
					}
				}
			}
			if(n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) {
			} else if(n < 0) {
				if(n == MBEDTLS_ERR_NET_RECV_FAILED) {
					/*
					 * FIXME: New client not yet accepted
					 */
					if(custom_poll_data->read_cb != NULL) {
						custom_poll_data->read_cb(req, 1, NULL);
					}
				} else if(n != MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
					mbedtls_strerror(n, (char *)&buffer, BUFFER_SIZE);
					logprintf(LOG_NOTICE, "mbedtls_ssl_handshake: %s", buffer);
					uv_poll_stop(req);
				}
				return;
			}
		} else {
			if(custom_poll_data->custom_recv == 0) {
				if(custom_poll_data->is_udp == 1) {
					n = (int)recv((unsigned int)fd, buffer, BUFFER_SIZE, 0);
					err = errno;
				} else {
#ifdef _WIN32
					n = recvfrom((SOCKET)fd, buffer, BUFFER_SIZE, 0, NULL, (socklen_t *)&fromlen);
#else
					n = recvfrom(fd, buffer, BUFFER_SIZE, 0, NULL, (socklen_t *)&fromlen);
#endif
					err = errno;
				}
			}
		}

		if(custom_poll_data->custom_recv == 0) {
			if(n > 0) {
				iobuf_append(&custom_poll_data->recv_iobuf, buffer, n);
				custom_poll_data->doread = 0;
				if(custom_poll_data->read_cb != NULL) {
					ssize_t len = 0;
					len = custom_poll_data->read_cb(req, custom_poll_data->recv_iobuf.len, custom_poll_data->recv_iobuf.buf);
					if(!uv_is_closing((uv_handle_t *)req)) {
						iobuf_remove(&custom_poll_data->recv_iobuf, len);
					}
				}
			} else if(n < 0 && err != EINTR) {
#ifdef _WIN32
				switch(WSAGetLastError()) {
					case WSAENOTCONN:
						if(custom_poll_data->read_cb != NULL) {
							one = 1;
							custom_poll_data->read_cb(req, 1, NULL);
						}
					break;
					case WSAEWOULDBLOCK:
#else
				switch(err) {
					case ENOTCONN:
						if(custom_poll_data->read_cb != NULL) {
							custom_poll_data->read_cb(req, 1, NULL);
						}
					break;
#if defined EAGAIN
					case EAGAIN:
#endif
#if defined EWOULDBLOCK && EWOULDBLOCK != EAGAIN
					case EWOULDBLOCK:
#endif
#endif
						custom_poll_data->doread = 1;
					break;
					default:
					break;
				}
			/*
			 * Client was disconnected
			 */
			} else if(n == 0 || (n < 0 && err == EPIPE)) {
				custom_poll_data->doclose = 1;
				custom_poll_data->doread = 0;
				goto end;
			}
		} else {
			custom_poll_data->doread = 0;
			if(custom_poll_data->read_cb != NULL) {
				custom_poll_data->read_cb(req, 0, NULL);
			}
		}
	}

end:
	eventpool_update_poll(req);
}

void iobuf_free(struct iobuf_t *iobuf) {
  if(iobuf != NULL) {
		uv_mutex_lock(&iobuf->lock);
    if(iobuf->buf != NULL) {
			FREE(iobuf->buf);
		}
		iobuf->len = iobuf->size = 0;
  }
	uv_mutex_unlock(&iobuf->lock);
}

void uv_custom_poll_free(struct uv_custom_poll_t *data) {
	assert(data->threadid == syscall(__NR_gettid));

	if(data->is_ssl == 1) {
		mbedtls_ssl_free(&data->ssl.ctx);
	}
	if(data->host != NULL) {
		FREE(data->host);
	}
	if(data->send_iobuf.size > 0) {
		iobuf_free(&data->send_iobuf);
	}
	if(data->recv_iobuf.size > 0) {
		iobuf_free(&data->recv_iobuf);
	}

	FREE(data);
}

void uv_custom_poll_init(struct uv_custom_poll_t **custom_poll, uv_poll_t *poll, void *data) {
	if((*custom_poll = MALLOC(sizeof(struct uv_custom_poll_t))) == NULL) {
		OUT_OF_MEMORY /*LCOV_EXCL_LINE*/
	}
	memset(*custom_poll, '\0', sizeof(struct uv_custom_poll_t));
	iobuf_init(&(*custom_poll)->send_iobuf, 0);
	iobuf_init(&(*custom_poll)->recv_iobuf, 0);

	(*custom_poll)->threadid = syscall(__NR_gettid);

	poll->data = *custom_poll;
	(*custom_poll)->data = data;
}

int uv_custom_close(uv_poll_t *req) {
	struct uv_custom_poll_t *custom_poll_data = req->data;
	struct iobuf_t *send_io = NULL;

	assert(custom_poll_data->threadid == syscall(__NR_gettid));

	if(uv_is_closing((uv_handle_t *)req)) {
		return -1;
	}

	if(custom_poll_data != NULL) {
		custom_poll_data->doclose = 1;
	}

	send_io = &custom_poll_data->send_iobuf;

	if(custom_poll_data->doclose == 1 && send_io->len == 0) {
		custom_poll_data->doclose = 2;
		if(custom_poll_data->close_cb != NULL) {
			custom_poll_data->close_cb(req);
		}
		if(!uv_is_closing((uv_handle_t *)req)) {
			uv_poll_stop(req);
		}
	} else if(send_io->len > 0) {
		uv_custom_write(req);
	}

	return 0;
}

int uv_custom_read(uv_poll_t *req) {
	struct uv_custom_poll_t *custom_poll_data = req->data;

	assert(custom_poll_data->threadid == syscall(__NR_gettid));

	if(uv_is_closing((uv_handle_t *)req)) {
		return -1;
	}

	if(custom_poll_data != NULL) {
		custom_poll_data->doread = 1;
	}

	// if(custom_poll_data->started == 0) {
		eventpool_update_poll(req);
	// }
	return 0;
}

int uv_custom_write(uv_poll_t *req) {
	struct uv_custom_poll_t *custom_poll_data = req->data;

	assert(custom_poll_data->threadid == syscall(__NR_gettid));

	if(uv_is_closing((uv_handle_t *)req)) {
		return -1;
	}

	if(custom_poll_data != NULL) {
		custom_poll_data->dowrite = 1;
	}

	// if(custom_poll_data->started == 0) {
		eventpool_update_poll(req);
	// }
	return 0;
}

void eventpool_init(enum eventpool_threads_t t) {
	/*
	 * Make sure we execute in the main thread
	 */
	const uv_thread_t pth_cur_id = uv_thread_self();
	assert(uv_thread_equal(&pth_main_id, &pth_cur_id));

	if(eventpoolinit == 1) {
		return;
	}
	eventpoolinit = 1;
	threads = t;

	uv_mutex_init(&thread_lock);
	if((thread_async_req = MALLOC(sizeof(uv_async_t))) == NULL) {
		OUT_OF_MEMORY /*LCOV_EXCL_LINE*/
	}
	uv_async_init(uv_default_loop(), thread_async_req, safe_thread_loop);

	if((async_event_req = MALLOC(sizeof(uv_async_t))) == NULL) {
		OUT_OF_MEMORY /*LCOV_EXCL_LINE*/
	}
	uv_async_init(uv_default_loop(), async_event_req, eventpool_execute);

	if(lockinit == 0) {
		lockinit = 1;
		// pthread_mutexattr_init(&listeners_attr);
		// pthread_mutexattr_settype(&listeners_attr, PTHREAD_MUTEX_RECURSIVE);
		// pthread_mutex_init(&listeners_lock, &listeners_attr);
		uv_mutex_init(&listeners_lock);
	}
}

enum eventpool_threads_t eventpool_threaded(void) {
	return threads;
}
