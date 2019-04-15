/*
 ============================================================================
 Name        : hev-socks5-tproxy.c
 Author      : Heiher <r@hev.cc>
 Copyright   : Copyright (c) 2017 - 2019 everyone.
 Description : Socks5 tproxy
 ============================================================================
 */

#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <sys/ioctl.h>

#include "hev-socks5-tproxy.h"
#include "hev-socks5-worker.h"
#include "hev-config.h"
#include "hev-task.h"
#include "hev-task-io.h"
#include "hev-task-io-socket.h"
#include "hev-task-system.h"
#include "hev-memory-allocator.h"

typedef struct _HevSocks5WorkerData HevSocks5WorkerData;

struct _HevSocks5WorkerData
{
    int fd_tcp;
    int fd_dns;
    HevSocks5Worker *worker;
};

static unsigned int workers;
static HevSocks5WorkerData *worker_list;

static void sigint_handler (int signum);
static void *work_thread_handler (void *data);
static int hev_socks5_tproxy_tcp_socket (int reuseport);
static int hev_socks5_tproxy_dns_socket (int reuseport);

int
hev_socks5_tproxy_init (void)
{
    int i;

    if (hev_task_system_init () < 0) {
        fprintf (stderr, "Init task system failed!\n");
        return -1;
    }

    workers = hev_config_get_workers ();
    worker_list = hev_malloc0 (sizeof (HevSocks5WorkerData *) * workers);
    if (!worker_list) {
        fprintf (stderr, "Allocate worker list failed!\n");
        return -2;
    }

    worker_list[0].fd_tcp = hev_socks5_tproxy_tcp_socket (1);
    if (worker_list[0].fd_tcp == -4)
        worker_list[0].fd_tcp = hev_socks5_tproxy_tcp_socket (0);
    worker_list[0].fd_dns = hev_socks5_tproxy_dns_socket (1);
    if (worker_list[0].fd_dns == -4)
        worker_list[0].fd_dns = hev_socks5_tproxy_dns_socket (0);
    if (worker_list[0].fd_tcp < 0 || worker_list[0].fd_dns < 0) {
        fprintf (stderr, "TCP and DNS listen failed!\n");
        return -3;
    }

    for (i = 1; i < workers; i++) {
        worker_list[i].fd_tcp = hev_socks5_tproxy_tcp_socket (1);
        if (worker_list[i].fd_tcp < 0)
            worker_list[i].fd_tcp = worker_list[0].fd_tcp;
        worker_list[i].fd_dns = hev_socks5_tproxy_dns_socket (1);
        if (worker_list[i].fd_dns < 0)
            worker_list[i].fd_dns = worker_list[0].fd_dns;
    }

    if (signal (SIGPIPE, SIG_IGN) == SIG_ERR) {
        fprintf (stderr, "Set signal pipe's handler failed!\n");
        return -6;
    }

    if (signal (SIGINT, sigint_handler) == SIG_ERR) {
        fprintf (stderr, "Set signal int's handler failed!\n");
        return -7;
    }

    return 0;
}

void
hev_socks5_tproxy_fini (void)
{
    hev_free (worker_list);
    hev_task_system_fini ();
}

int
hev_socks5_tproxy_run (void)
{
    int i;
    pthread_t work_threads[workers];

    worker_list[0].worker =
        hev_socks5_worker_new (worker_list[0].fd_tcp, worker_list[0].fd_dns);
    if (!worker_list[0].worker) {
        fprintf (stderr, "Create socks5 worker 0 failed!\n");
        return -1;
    }

    hev_socks5_worker_start (worker_list[0].worker);

    for (i = 1; i < workers; i++) {
        pthread_create (&work_threads[i], NULL, work_thread_handler,
                        (void *)(intptr_t)i);
    }

    hev_task_system_run ();

    for (i = 1; i < workers; i++) {
        pthread_join (work_threads[i], NULL);
    }

    hev_socks5_worker_destroy (worker_list[0].worker);
    close (worker_list[0].fd_tcp);
    close (worker_list[0].fd_dns);

    return 0;
}

void
hev_socks5_tproxy_stop (void)
{
    int i;

    for (i = 0; i < workers; i++) {
        if (!worker_list[i].worker)
            continue;

        hev_socks5_worker_stop (worker_list[i].worker);
    }
}

static void
sigint_handler (int signum)
{
    hev_socks5_tproxy_stop ();
}

static void *
work_thread_handler (void *data)
{
    int i = (intptr_t)data;

    if (hev_task_system_init () < 0) {
        fprintf (stderr, "Init task system failed!\n");
        return NULL;
    }

    worker_list[i].worker =
        hev_socks5_worker_new (worker_list[i].fd_tcp, worker_list[i].fd_dns);
    if (!worker_list[i].worker) {
        fprintf (stderr, "Create socks5 worker %d failed!\n", i);
        hev_task_system_fini ();
        return NULL;
    }

    hev_socks5_worker_start (worker_list[i].worker);

    hev_task_system_run ();

    hev_socks5_worker_destroy (worker_list[i].worker);
    close (worker_list[i].fd_tcp);
    close (worker_list[i].fd_dns);

    hev_task_system_fini ();

    return NULL;
}

static int
hev_socks5_tproxy_tcp_socket (int reuseport)
{
    int ret, fd, reuse = 1;
    struct sockaddr *addr;
    socklen_t addr_len;

    addr = hev_config_get_tcp_listen_address (&addr_len);
    if (!addr)
        return -1;

    fd = hev_task_io_socket_socket (AF_INET6, SOCK_STREAM, 0);
    if (fd == -1) {
        fprintf (stderr, "Create socket failed!\n");
        return -2;
    }

    ret = setsockopt (fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof (reuse));
    if (ret == -1) {
        fprintf (stderr, "Set reuse address failed!\n");
        close (fd);
        return -3;
    }

    if (reuseport) {
        ret = setsockopt (fd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof (reuse));
        if (ret == -1) {
            fprintf (stderr, "Set reuse port failed!\n");
            close (fd);
            return -4;
        }
    }

    ret = bind (fd, addr, addr_len);
    if (ret == -1) {
        fprintf (stderr, "Bind address failed!\n");
        close (fd);
        return -5;
    }
    ret = listen (fd, 100);
    if (ret == -1) {
        fprintf (stderr, "Listen failed!\n");
        close (fd);
        return -6;
    }

    return fd;
}

static int
hev_socks5_tproxy_dns_socket (int reuseport)
{
    int ret, fd, reuse = 1;
    struct sockaddr *addr;
    socklen_t addr_len;

    addr = hev_config_get_dns_listen_address (&addr_len);
    if (!addr)
        return -1;

    fd = hev_task_io_socket_socket (AF_INET6, SOCK_DGRAM, 0);
    if (fd == -1) {
        fprintf (stderr, "Create socket failed!\n");
        return -2;
    }

    ret = setsockopt (fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof (reuse));
    if (ret == -1) {
        fprintf (stderr, "Set reuse address failed!\n");
        close (fd);
        return -3;
    }

    if (reuseport) {
        ret = setsockopt (fd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof (reuse));
        if (ret == -1) {
            fprintf (stderr, "Set reuse port failed!\n");
            close (fd);
            return -4;
        }
    }

    ret = bind (fd, addr, addr_len);
    if (ret == -1) {
        fprintf (stderr, "Bind address failed!\n");
        close (fd);
        return -5;
    }

    return fd;
}
