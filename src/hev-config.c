/*
 ============================================================================
 Name        : hev-config.c
 Author      : Heiher <r@hev.cc>
 Copyright   : Copyright (c) 2017 - 2019 Heiher.
 Description : Config
 ============================================================================
 */

#include <stdio.h>
#include <arpa/inet.h>
#include <yaml.h>

#include "hev-config.h"
#include "hev-config-const.h"

static unsigned int workers;

static struct sockaddr_in6 socks5_address;
static struct sockaddr_in6 tcp_listen_address;
static struct sockaddr_in6 dns_listen_address;

static char log_file[1024];
static char pid_file[1024];
static int limit_nofile = -2;

static int
address_to_sockaddr (const char *address, unsigned short port,
                     struct sockaddr_in6 *addr)
{
    __builtin_bzero (addr, sizeof (*addr));

    addr->sin6_family = AF_INET6;
    addr->sin6_port = htons (port);
    if (inet_pton (AF_INET, address, &addr->sin6_addr.s6_addr[12]) == 1) {
        ((uint16_t *)&addr->sin6_addr)[5] = 0xffff;
    } else {
        if (inet_pton (AF_INET6, address, &addr->sin6_addr) != 1)
            return -1;
    }

    return 0;
}

static int
hev_config_parse_main (yaml_document_t *doc, yaml_node_t *base)
{
    yaml_node_pair_t *pair;

    if (!base || YAML_MAPPING_NODE != base->type)
        return -1;

    for (pair = base->data.mapping.pairs.start;
         pair <= base->data.mapping.pairs.top; pair++) {
        yaml_node_t *node;
        const char *key, *value;

        if (!pair->key || !pair->value)
            break;

        node = yaml_document_get_node (doc, pair->key);
        if (!node || YAML_SCALAR_NODE != node->type)
            break;
        key = (const char *)node->data.scalar.value;

        node = yaml_document_get_node (doc, pair->value);
        if (!node || YAML_SCALAR_NODE != node->type)
            break;
        value = (const char *)node->data.scalar.value;

        if (0 == strcmp (key, "workers"))
            workers = strtoul (value, NULL, 10);
    }

    if (!workers) {
        fprintf (stderr, "Can't found main.workers!\n");
        return -1;
    }

    return 0;
}

static int
hev_config_parse_socks5 (yaml_document_t *doc, yaml_node_t *base)
{
    yaml_node_pair_t *pair;
    int port = 0;
    const char *addr = NULL;

    if (!base || YAML_MAPPING_NODE != base->type)
        return -1;

    for (pair = base->data.mapping.pairs.start;
         pair <= base->data.mapping.pairs.top; pair++) {
        yaml_node_t *node;
        const char *key, *value;

        if (!pair->key || !pair->value)
            break;

        node = yaml_document_get_node (doc, pair->key);
        if (!node || YAML_SCALAR_NODE != node->type)
            break;
        key = (const char *)node->data.scalar.value;

        node = yaml_document_get_node (doc, pair->value);
        if (!node || YAML_SCALAR_NODE != node->type)
            break;
        value = (const char *)node->data.scalar.value;

        if (0 == strcmp (key, "port"))
            port = strtoul (value, NULL, 10);
        else if (0 == strcmp (key, "address"))
            addr = value;
    }

    if (!port) {
        fprintf (stderr, "Can't found socks5.port!\n");
        return -1;
    }

    if (!addr) {
        fprintf (stderr, "Can't found socks5.address!\n");
        return -1;
    }

    return address_to_sockaddr (addr, port, &socks5_address);
}

static int
hev_config_parse_tcp (yaml_document_t *doc, yaml_node_t *base)
{
    yaml_node_pair_t *pair;
    int port = 0;
    const char *addr = NULL;

    if (!base || YAML_MAPPING_NODE != base->type)
        return -1;

    for (pair = base->data.mapping.pairs.start;
         pair <= base->data.mapping.pairs.top; pair++) {
        yaml_node_t *node;
        const char *key, *value;

        if (!pair->key || !pair->value)
            break;

        node = yaml_document_get_node (doc, pair->key);
        if (!node || YAML_SCALAR_NODE != node->type)
            break;
        key = (const char *)node->data.scalar.value;

        node = yaml_document_get_node (doc, pair->value);
        if (!node || YAML_SCALAR_NODE != node->type)
            break;
        value = (const char *)node->data.scalar.value;

        if (0 == strcmp (key, "port"))
            port = strtoul (value, NULL, 10);
        else if (0 == strcmp (key, "listen-address"))
            addr = value;
    }

    if (!port) {
        fprintf (stderr, "Can't found tcp.port!\n");
        return -1;
    }

    if (!addr) {
        fprintf (stderr, "Can't found tcp.address!\n");
        return -1;
    }

    return address_to_sockaddr (addr, port, &tcp_listen_address);
}

static int
hev_config_parse_dns (yaml_document_t *doc, yaml_node_t *base)
{
    yaml_node_pair_t *pair;
    int port = 0;
    const char *addr = NULL;

    if (!base || YAML_MAPPING_NODE != base->type)
        return -1;

    for (pair = base->data.mapping.pairs.start;
         pair <= base->data.mapping.pairs.top; pair++) {
        yaml_node_t *node;
        const char *key, *value;

        if (!pair->key || !pair->value)
            break;

        node = yaml_document_get_node (doc, pair->key);
        if (!node || YAML_SCALAR_NODE != node->type)
            break;
        key = (const char *)node->data.scalar.value;

        node = yaml_document_get_node (doc, pair->value);
        if (!node || YAML_SCALAR_NODE != node->type)
            break;
        value = (const char *)node->data.scalar.value;

        if (0 == strcmp (key, "port"))
            port = strtoul (value, NULL, 10);
        else if (0 == strcmp (key, "listen-address"))
            addr = value;
    }

    if (!port) {
        fprintf (stderr, "Can't found dns.port!\n");
        return -1;
    }

    if (!addr) {
        fprintf (stderr, "Can't found dns.address!\n");
        return -1;
    }

    return address_to_sockaddr (addr, port, &dns_listen_address);
}

static int
hev_config_parse_misc (yaml_document_t *doc, yaml_node_t *base)
{
    yaml_node_pair_t *pair;

    if (!base || YAML_MAPPING_NODE != base->type)
        return -1;

    for (pair = base->data.mapping.pairs.start;
         pair <= base->data.mapping.pairs.top; pair++) {
        yaml_node_t *node;
        const char *key, *value;

        if (!pair->key || !pair->value)
            break;

        node = yaml_document_get_node (doc, pair->key);
        if (!node || YAML_SCALAR_NODE != node->type)
            break;
        key = (const char *)node->data.scalar.value;

        node = yaml_document_get_node (doc, pair->value);
        if (!node || YAML_SCALAR_NODE != node->type)
            break;
        value = (const char *)node->data.scalar.value;

        if (0 == strcmp (key, "pid-file"))
            strncpy (pid_file, value, 1024 - 1);
        else if (0 == strcmp (key, "log-file"))
            strncpy (log_file, value, 1024 - 1);
        else if (0 == strcmp (key, "limit-nofile"))
            limit_nofile = strtol (value, NULL, 10);
    }

    return 0;
}

static int
hev_config_parse_doc (yaml_document_t *doc)
{
    yaml_node_t *root;
    yaml_node_pair_t *pair;

    root = yaml_document_get_root_node (doc);
    if (!root || YAML_MAPPING_NODE != root->type)
        return -1;

    for (pair = root->data.mapping.pairs.start;
         pair <= root->data.mapping.pairs.top; pair++) {
        yaml_node_t *node;
        const char *key;
        int res = 0;

        if (!pair->key || !pair->value)
            break;

        node = yaml_document_get_node (doc, pair->key);
        if (!node || YAML_SCALAR_NODE != node->type)
            break;

        key = (const char *)node->data.scalar.value;
        node = yaml_document_get_node (doc, pair->value);

        if (0 == strcmp (key, "main"))
            res = hev_config_parse_main (doc, node);
        else if (0 == strcmp (key, "socks5"))
            res = hev_config_parse_socks5 (doc, node);
        else if (0 == strcmp (key, "tcp"))
            res = hev_config_parse_tcp (doc, node);
        else if (0 == strcmp (key, "dns"))
            res = hev_config_parse_dns (doc, node);
        else if (0 == strcmp (key, "misc"))
            res = hev_config_parse_misc (doc, node);

        if (res < 0)
            return -1;
    }

    return 0;
}

int
hev_config_init (const char *config_path)
{
    yaml_parser_t parser;
    yaml_document_t doc;
    FILE *fp;
    int res = -1;

    if (!yaml_parser_initialize (&parser))
        goto exit;

    fp = fopen (config_path, "r");
    if (!fp) {
        fprintf (stderr, "Open %s failed!\n", config_path);
        goto exit_free_parser;
    }

    yaml_parser_set_input_file (&parser, fp);
    if (!yaml_parser_load (&parser, &doc)) {
        fprintf (stderr, "Parse %s failed!\n", config_path);
        goto exit_close_fp;
    }

    res = hev_config_parse_doc (&doc);
    yaml_document_delete (&doc);

exit_close_fp:
    fclose (fp);
exit_free_parser:
    yaml_parser_delete (&parser);
exit:
    return res;
}

void
hev_config_fini (void)
{
}

unsigned int
hev_config_get_workers (void)
{
    return workers;
}

struct sockaddr *
hev_config_get_socks5_address (socklen_t *addr_len)
{
    *addr_len = sizeof (socks5_address);
    return (struct sockaddr *)&socks5_address;
}

struct sockaddr *
hev_config_get_tcp_listen_address (socklen_t *addr_len)
{
    if (!tcp_listen_address.sin6_port)
        return NULL;

    *addr_len = sizeof (tcp_listen_address);
    return (struct sockaddr *)&tcp_listen_address;
}

struct sockaddr *
hev_config_get_dns_listen_address (socklen_t *addr_len)
{
    if (!dns_listen_address.sin6_port)
        return NULL;

    *addr_len = sizeof (dns_listen_address);
    return (struct sockaddr *)&dns_listen_address;
}

const char *
hev_config_get_misc_pid_file (void)
{
    if ('\0' == pid_file[0])
        return NULL;

    return pid_file;
}

int
hev_config_get_misc_limit_nofile (void)
{
    return limit_nofile;
}

const char *
hev_config_get_misc_log_file (void)
{
    if ('\0' == log_file[0])
        return NULL;
    if (0 == strcmp (log_file, "null"))
        return NULL;

    return log_file;
}
