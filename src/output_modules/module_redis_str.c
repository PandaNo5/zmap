/*
 * ZMap Copyright 2013 Regents of the University of Michigan
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not
 * use this file except in compliance with the License. You may obtain a copy
 * of the License at http://www.apache.org/licenses/LICENSE-2.0
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "../../lib/logger.h"
#include "../../lib/xalloc.h"
#include "../../lib/redis.h"

#include "output_modules.h"

#define UNUSED __attribute__((unused))

#define BUFFER_SIZE 500
#define DEFAULT_STR_LEN 1024

typedef struct varchar {
    size_t len;
    char *str;
} varchar_t;

static varchar_t *buffer;
static char **buffer_export;
static int buffer_fill = 0;
static char *queue_name = NULL;

int redisstrmodule_init(struct state_conf *conf, UNUSED char **fields, UNUSED int fieldlens)
{
	buffer = xcalloc(BUFFER_SIZE, sizeof(varchar_t));
	buffer_export = xcalloc(BUFFER_SIZE, sizeof(char*));
	buffer_fill = 0;
    // initialize queue
    for (int i=0; i < BUFFER_SIZE; i++) {
        buffer[i].len = DEFAULT_STR_LEN;
        buffer_export[i] = buffer[i].str = xmalloc(DEFAULT_STR_LEN);
    }
	if (conf->output_args) {
		redisconf_t *rconf = redis_parse_connstr(conf->output_args);
		if (rconf->type == T_TCP) {
			log_info("redis-module", "{type: TCP, server: %s, "
					"port: %u, list: %s}", rconf->server,
					rconf->port, rconf->list_name);
		} else {
			log_info("redis-module", "{type: LOCAL, path: %s, "
					"list: %s}", rconf->path, rconf->list_name);
		}
		queue_name = rconf->list_name;
	} else {
		queue_name = strdup("zmap");
	}
	return redis_init(conf->output_args);
}

static int redisstrmodule_flush(void)
{
	if (redis_lpush_strings((char*) queue_name, buffer_export, buffer_fill)) {
		return EXIT_FAILURE;
	}
	buffer_fill = 0;
	return EXIT_SUCCESS;
}

#define INT_STR_LEN 20 // len(9223372036854775807) == 19

static size_t guess_csv_string_length(fieldset_t *fs)
{
    size_t len = 0;
    for (int i=0; i < fs->len; i++) {
        field_t *f = &(fs->fields[i]);
        if (f->type == FS_STRING) {
            len += strlen(f->value.ptr);
            len += 2; // potential quotes
        } else if (f->type == FS_UINT64) {
            len += INT_STR_LEN;
        } else if (f->type == FS_BINARY) {
            len += 2*f->len;
        } else if (f->type == FS_NULL) {
            // do nothing
        } else {
            log_fatal("csv", "received unknown output type "
                    "(not str, binary, null, or uint64_t)");
        }
    }
    // estimated length + number of commas
    return len + (size_t) len + 256;
}

static void hex_encode_str(char *f, unsigned char* readbuf, size_t len)
{
    char *temp = f;
    for(size_t i=0; i < len; i++) {
        sprintf(temp, "%02x", readbuf[i]);
        temp += (size_t) 2*sizeof(char);
    }
}

void make_csv_string(fieldset_t *fs, char *out, size_t len)
{
    memset(out, 0, len);
    for (int i=0; i < fs->len; i++) {
        char *temp = out + (size_t) strlen(out);
        field_t *f = &(fs->fields[i]);
        if (i) {
            sprintf(temp, ",");
        }
        if (f->type == FS_STRING) {
            if (strlen(temp) + strlen((char*) f->value.ptr) >= len) {
                log_fatal("redis-str", "out of memory---will overflow");
            }
            if (strchr((char*) f->value.ptr, ',')) {
                sprintf(temp, "\"%s\"", (char*) f->value.ptr);
            } else {
                sprintf(temp, "%s", (char*) f->value.ptr);
            }
        } else if (f->type == FS_UINT64) {
            if (strlen(temp) + INT_STR_LEN >= len) {
                log_fatal("redis-str", "out of memory---will overflow");
            }
            sprintf(temp, "%" PRIu64, (uint64_t) f->value.num);
        } else if (f->type == FS_BINARY) {
            if (strlen(temp) + 2*f->len >= len) {
                log_fatal("redis-str", "out of memory---will overflow");
            }
            hex_encode_str(out, (unsigned char*) f->value.ptr, f->len);
        } else if (f->type == FS_NULL) {
            // do nothing
        } else {
            log_fatal("csv", "received unknown output type");
        }
    }
}

int redisstrmodule_process(fieldset_t *fs)
{
    size_t reqd_space = guess_csv_string_length(fs);
    size_t curr_length = buffer[buffer_fill].len;
    // do we have enough space in buffer? if not allocate more.
    if (reqd_space > curr_length) {
        free(buffer[buffer_fill].str);
        buffer[buffer_fill].str = xmalloc(reqd_space);
        buffer_export[buffer_fill] = buffer[buffer_fill].str;
    }
    make_csv_string(fs, buffer[buffer_fill].str, buffer[buffer_fill].len);    
    // if full, flush all to redis
	if (++buffer_fill == BUFFER_SIZE) {
		if (redisstrmodule_flush()) {
			return EXIT_FAILURE;
		}
	}
	return EXIT_SUCCESS;
}

int redisstrmodule_close(UNUSED struct state_conf* c,
		UNUSED struct state_send *s,
		UNUSED struct state_recv *r)
{
	if (redisstrmodule_flush()) {
		return EXIT_FAILURE;
	}
	if (redis_close()) {
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}

output_module_t module_redis_str = {
	.name = "redis-string",
	.init = &redisstrmodule_init,
	.start = NULL,
	.update = NULL,
	.update_interval = 0,
	.close = &redisstrmodule_close,
	.process_ip = &redisstrmodule_process,
	.helptext = NULL
};

