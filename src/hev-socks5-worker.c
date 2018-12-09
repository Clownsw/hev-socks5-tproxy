/*
 ============================================================================
 Name        : hev-socks5-worker.c
 Author      : Heiher <r@hev.cc>
 Copyright   : Copyright (c) 2017 everyone.
 Description : Socks5 worker
 ============================================================================
 */

#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/eventfd.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "hev-socks5-worker.h"
#include "hev-socks5-session.h"
#include "hev-memory-allocator.h"
#include "hev-config.h"
#include "hev-task.h"
#include "hev-task-io.h"
#include "hev-task-io-socket.h"

#define TIMEOUT (30 * 1000)

struct _HevSocks5Worker
{
    int fd_tcp;
    int fd_dns;
    int event_fd;
    int quit;

    HevTask *task_worker_tcp;
    HevTask *task_worker_dns;
    HevTask *task_event;
    HevTask *task_session_manager;
    HevSocks5SessionBase *session_list;
};

static void hev_socks5_worker_tcp_task_entry (void *data);
static void hev_socks5_worker_dns_task_entry (void *data);
static void hev_socks5_event_task_entry (void *data);
static void hev_socks5_session_manager_task_entry (void *data);

static void session_manager_insert_session (HevSocks5Worker *self,
                                            HevSocks5Session *session);
static void session_manager_remove_session (HevSocks5Worker *self,
                                            HevSocks5Session *session);
static void session_close_handler (HevSocks5Session *session, void *data);

HevSocks5Worker *
hev_socks5_worker_new (int fd_tcp, int fd_dns)
{
    HevSocks5Worker *self;

    self = hev_malloc0 (sizeof (HevSocks5Worker));
    if (!self) {
        fprintf (stderr, "Allocate worker failed!\n");
        return NULL;
    }

    self->fd_tcp = fd_tcp;
    self->fd_dns = fd_dns;
    self->event_fd = -1;

    if (fd_tcp >= 0) {
        self->task_worker_tcp = hev_task_new (8192);
        if (!self->task_worker_tcp) {
            fprintf (stderr, "Create worker tcp's task failed!\n");
            hev_free (self);
            return NULL;
        }
    }

    if (fd_dns >= 0) {
        self->task_worker_dns = hev_task_new (8192);
        if (!self->task_worker_dns) {
            fprintf (stderr, "Create worker dns's task failed!\n");
            if (self->task_worker_tcp)
                hev_task_unref (self->task_worker_tcp);
            hev_free (self);
            return NULL;
        }
    }

    self->task_event = hev_task_new (8192);
    if (!self->task_event) {
        fprintf (stderr, "Create event's task failed!\n");
        if (self->task_worker_tcp)
            hev_task_unref (self->task_worker_tcp);
        if (self->task_worker_dns)
            hev_task_unref (self->task_worker_dns);
        hev_free (self);
        return NULL;
    }

    self->task_session_manager = hev_task_new (8192);
    if (!self->task_session_manager) {
        fprintf (stderr, "Create session manager's task failed!\n");
        hev_task_unref (self->task_event);
        if (self->task_worker_tcp)
            hev_task_unref (self->task_worker_tcp);
        if (self->task_worker_dns)
            hev_task_unref (self->task_worker_dns);
        hev_free (self);
        return NULL;
    }

    return self;
}

void
hev_socks5_worker_destroy (HevSocks5Worker *self)
{
    hev_free (self);
}

void
hev_socks5_worker_start (HevSocks5Worker *self)
{
    if (self->task_worker_tcp)
        hev_task_run (self->task_worker_tcp, hev_socks5_worker_tcp_task_entry,
                      self);
    if (self->task_worker_dns)
        hev_task_run (self->task_worker_dns, hev_socks5_worker_dns_task_entry,
                      self);
    hev_task_run (self->task_event, hev_socks5_event_task_entry, self);
    hev_task_run (self->task_session_manager,
                  hev_socks5_session_manager_task_entry, self);
}

void
hev_socks5_worker_stop (HevSocks5Worker *self)
{
    if (self->event_fd == -1)
        return;

    if (eventfd_write (self->event_fd, 1) == -1)
        fprintf (stderr, "Write stop event failed!\n");
}

static int
worker_task_io_yielder (HevTaskYieldType type, void *data)
{
    HevSocks5Worker *self = data;

    hev_task_yield (type);

    return (self->quit) ? -1 : 0;
}

static void
hev_socks5_worker_tcp_task_entry (void *data)
{
    HevSocks5Worker *self = data;
    HevTask *task = hev_task_self ();

    hev_task_add_fd (task, self->fd_tcp, POLLIN);

    for (;;) {
        int client_fd;
        struct sockaddr_in addr;
        struct sockaddr *in_addr = (struct sockaddr *)&addr;
        socklen_t addr_len = sizeof (addr);
        HevSocks5Session *session;

        client_fd = hev_task_io_socket_accept (self->fd_tcp, in_addr, &addr_len,
                                               worker_task_io_yielder, self);
        if (-1 == client_fd) {
            fprintf (stderr, "Accept failed!\n");
            continue;
        } else if (-2 == client_fd) {
            break;
        }

#ifdef _DEBUG
        printf ("Worker %p: New client %d enter from %s:%u\n", self, client_fd,
                inet_ntoa (addr.sin_addr), ntohs (addr.sin_port));
#endif

        session =
            hev_socks5_session_new_tcp (client_fd, session_close_handler, self);
        if (!session) {
            close (client_fd);
            continue;
        }

        session_manager_insert_session (self, session);
        hev_socks5_session_run (session);
    }
}

static void
hev_socks5_worker_dns_task_entry (void *data)
{
    HevSocks5Worker *self = data;
    HevTask *task = hev_task_self ();

    hev_task_add_fd (task, self->fd_dns, POLLIN);

    for (;;) {
        unsigned char buf[2048];
        ssize_t len;
        struct sockaddr_in addr;
        struct sockaddr *in_addr = (struct sockaddr *)&addr;
        socklen_t addr_len = sizeof (addr);
        HevSocks5Session *session;

        len = recvfrom (self->fd_dns, buf, 2048, MSG_PEEK, in_addr, &addr_len);
        if (len == -1) {
            if (errno == EAGAIN) {
                hev_task_yield (HEV_TASK_WAITIO);
                if (self->quit)
                    break;
                continue;
            }

            fprintf (stderr, "Receive failed!\n");
            break;
        }

#ifdef _DEBUG
        printf ("Worker %p: New DNS request from %s:%u\n", self,
                inet_ntoa (addr.sin_addr), ntohs (addr.sin_port));
#endif

        session = hev_socks5_session_new_dns (self->fd_dns,
                                              session_close_handler, self);
        if (!session)
            continue;

        session_manager_insert_session (self, session);
        hev_socks5_session_run (session);
    }
}

static void
hev_socks5_event_task_entry (void *data)
{
    HevSocks5Worker *self = data;
    HevTask *task = hev_task_self ();
    ssize_t size;
    HevSocks5SessionBase *session;

    self->event_fd = eventfd (0, EFD_NONBLOCK);
    if (-1 == self->event_fd) {
        fprintf (stderr, "Create eventfd failed!\n");
        return;
    }

    hev_task_add_fd (task, self->event_fd, POLLIN);

    for (;;) {
        eventfd_t val;
        size = eventfd_read (self->event_fd, &val);
        if (-1 == size && errno == EAGAIN) {
            hev_task_yield (HEV_TASK_WAITIO);
            continue;
        }
        break;
    }

    /* set quit flag */
    self->quit = 1;
    /* wakeup worker tcp's task */
    if (self->task_worker_tcp)
        hev_task_wakeup (self->task_worker_tcp);
    /* wakeup worker dns's task */
    if (self->task_worker_dns)
        hev_task_wakeup (self->task_worker_dns);
    /* wakeup session manager's task */
    hev_task_wakeup (self->task_session_manager);

    /* wakeup sessions's task */
#ifdef _DEBUG
    printf ("Worker %p: Enumerating session list ...\n", self);
#endif
    for (session = self->session_list; session; session = session->next) {
#ifdef _DEBUG
        printf ("Worker %p: Set session %p's hp = 0\n", self, session);
#endif
        session->hp = 0;

        /* wakeup session's task to do destroy */
        hev_task_wakeup (session->task);
#ifdef _DEBUG
        printf ("Worker %p: Wakeup session %p's task %p\n", self, session,
                session->task);
#endif
    }

    close (self->event_fd);
}

static void
hev_socks5_session_manager_task_entry (void *data)
{
    HevSocks5Worker *self = data;

    for (;;) {
        HevSocks5SessionBase *session;

        hev_task_sleep (TIMEOUT);
        if (self->quit)
            break;

#ifdef _DEBUG
        printf ("Worker %p: Enumerating session list ...\n", self);
#endif
        for (session = self->session_list; session; session = session->next) {
#ifdef _DEBUG
            printf ("Worker %p: Session %p's hp %d\n", self, session,
                    session->hp);
#endif
            session->hp--;
            if (session->hp > 0)
                continue;

            /* wakeup session's task to do destroy */
            hev_task_wakeup (session->task);
#ifdef _DEBUG
            printf ("Worker %p: Wakeup session %p's task %p\n", self, session,
                    session->task);
#endif
        }
    }
}

static void
session_manager_insert_session (HevSocks5Worker *self,
                                HevSocks5Session *session)
{
    HevSocks5SessionBase *session_base = (HevSocks5SessionBase *)session;

#ifdef _DEBUG
    printf ("Worker %p: Insert session: %p\n", self, session);
#endif
    /* insert session to session_list */
    session_base->prev = NULL;
    session_base->next = self->session_list;
    if (self->session_list)
        self->session_list->prev = session_base;
    self->session_list = session_base;
}

static void
session_manager_remove_session (HevSocks5Worker *self,
                                HevSocks5Session *session)
{
    HevSocks5SessionBase *session_base = (HevSocks5SessionBase *)session;

#ifdef _DEBUG
    printf ("Worker %p: Remove session: %p\n", self, session);
#endif
    /* remove session from session_list */
    if (session_base->prev) {
        session_base->prev->next = session_base->next;
    } else {
        self->session_list = session_base->next;
    }
    if (session_base->next) {
        session_base->next->prev = session_base->prev;
    }
}

static void
session_close_handler (HevSocks5Session *session, void *data)
{
    HevSocks5Worker *self = data;

    session_manager_remove_session (self, session);
}
