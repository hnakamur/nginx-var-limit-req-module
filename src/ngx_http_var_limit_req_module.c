
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 * Copyright (C) Hiroaki Nakamura
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>


#define NGX_HTTP_VAR_LIMIT_REQ_PASSED            1
#define NGX_HTTP_VAR_LIMIT_REQ_DELAYED           2
#define NGX_HTTP_VAR_LIMIT_REQ_REJECTED          3
#define NGX_HTTP_VAR_LIMIT_REQ_DELAYED_DRY_RUN   4
#define NGX_HTTP_VAR_LIMIT_REQ_REJECTED_DRY_RUN  5


#define lit_len(lit) (sizeof(lit) - 1)
#define ngx_array_item(a, i) ((void *)((char *)(a)->elts + (a)->size * (i)))


typedef struct {
    u_char                       color;
    u_char                       dummy;
    u_short                      len;
    ngx_queue_t                  queue;
    ngx_msec_t                   last;
    /* integer value, 1 corresponds to 0.001 r/s */
    ngx_uint_t                   excess;
    ngx_uint_t                   count;
    ngx_time_t                   last_time;
    /* integer value, 1 corresponds to 0.001 r/s */
    ngx_uint_t                   rate;
    /* integer value, 1 corresponds to 0.001 r/s */
    ngx_uint_t                   burst;
    u_char                       data[1];
} ngx_http_var_limit_req_node_t;


typedef struct {
    ngx_rbtree_t                  rbtree;
    ngx_rbtree_node_t             sentinel;
    ngx_queue_t                   queue;
} ngx_http_var_limit_req_shctx_t;


typedef struct {
    ngx_http_var_limit_req_shctx_t  *sh;
    ngx_slab_pool_t                 *shpool;
    /* integer value, 1 corresponds to 0.001 r/s */
    ngx_uint_t                       rate;
    ngx_http_complex_value_t         rate_var;
    ngx_http_complex_value_t         burst_var;
    ngx_http_complex_value_t         dry_run_var;
    ngx_http_complex_value_t         status_var;
    ngx_http_complex_value_t         key;
    ngx_http_var_limit_req_node_t   *node;
} ngx_http_var_limit_req_ctx_t;


typedef struct {
    ngx_shm_zone_t              *shm_zone;
    /* integer value, 1 corresponds to 0.001 r/s */
    ngx_uint_t                   burst;
    ngx_uint_t                   delay;
} ngx_http_var_limit_req_limit_t;


typedef struct {
    ngx_array_t                  limits;
    ngx_uint_t                   limit_log_level;
    ngx_uint_t                   delay_log_level;
    ngx_uint_t                   status_code;
    ngx_flag_t                   dry_run;

    ngx_shm_zone_t              *top_shm_zone;
    ngx_uint_t                   default_n;
} ngx_http_var_limit_req_conf_t;


typedef struct {
    ngx_str_t                    key;
    ngx_msec_t                   last;
    ngx_time_t                   last_time;
    /* integer value, 1 corresponds to 0.001 r/s */
    ngx_uint_t                   adjusted_excess;
    /* integer value, 1 corresponds to 0.001 r/s */
    ngx_uint_t                   raw_excess;
    /* integer value, 1 corresponds to 0.001 r/s */
    ngx_uint_t                   rate;
    /* integer value, 1 corresponds to 0.001 r/s */
    ngx_uint_t                   burst;
} ngx_http_var_limit_req_top_item_t;


static ngx_uint_t ngx_http_var_limit_req_get_status(ngx_http_request_t *r,
    ngx_http_var_limit_req_ctx_t *ctx);
static void ngx_http_var_limit_req_delay(ngx_http_request_t *r);
static ngx_int_t ngx_http_var_limit_req_lookup(ngx_http_request_t *r,
    ngx_http_var_limit_req_limit_t *limit,
    ngx_uint_t hash, ngx_str_t *key, ngx_uint_t *ep, ngx_uint_t account,
    ngx_uint_t rate, ngx_uint_t burst);
static ngx_uint_t ngx_http_var_limit_req_adjust_excess(ngx_uint_t raw_excess,
    ngx_msec_int_t ms, ngx_uint_t rate, ngx_flag_t just_monitoring);
static ngx_msec_int_t ngx_http_var_limit_req_duration_after_last_access(
    ngx_msec_t now, ngx_msec_t last);
static ngx_msec_t ngx_http_var_limit_req_account(
    ngx_http_var_limit_req_limit_t *limits, ngx_uint_t n, ngx_uint_t *ep,
    ngx_http_var_limit_req_limit_t **limit, ngx_uint_t rate);
static void ngx_http_var_limit_req_unlock(
    ngx_http_var_limit_req_limit_t *limits, ngx_uint_t n);
static void ngx_http_var_limit_req_expire(ngx_http_var_limit_req_ctx_t *ctx,
    ngx_uint_t n, ngx_uint_t rate);

static ngx_int_t ngx_http_var_limit_req_status_variable(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data);
static void *ngx_http_var_limit_req_create_conf(ngx_conf_t *cf);
static char *ngx_http_var_limit_req_merge_conf(ngx_conf_t *cf, void *parent,
    void *child);
static char *ngx_http_var_limit_req_zone(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_http_var_limit_req(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);

static ngx_int_t ngx_http_var_limit_req_top_handler(ngx_http_request_t *r);
static ngx_int_t ngx_http_var_limit_req_top_build_items(ngx_http_request_t *r,
    ngx_rbtree_t *rbtree, ngx_array_t *items);
static ngx_uint_t ngx_http_var_limit_req_binary_search(ngx_array_t *items,
    const ngx_http_var_limit_req_top_item_t *item);
static ngx_int_t ngx_http_var_limit_req_top_build_response(
    ngx_http_request_t *r, ngx_array_t *items, ngx_chain_t *out);
static ngx_int_t ngx_http_var_limit_req_top_item_cmp(const void *a,
    const void *b);
static void ngx_http_var_limit_req_format_http_time(time_t sec,
    u_char *out_buf);
static char *ngx_http_var_limit_req_top(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);

static ngx_int_t ngx_http_var_limit_req_add_variables(ngx_conf_t *cf);
static ngx_int_t ngx_http_var_limit_req_init(ngx_conf_t *cf);


static ngx_conf_enum_t  ngx_http_var_limit_req_log_levels[] = {
    { ngx_string("info"), NGX_LOG_INFO },
    { ngx_string("notice"), NGX_LOG_NOTICE },
    { ngx_string("warn"), NGX_LOG_WARN },
    { ngx_string("error"), NGX_LOG_ERR },
    { ngx_null_string, 0 }
};


static ngx_conf_num_bounds_t  ngx_http_var_limit_req_status_bounds = {
    ngx_conf_check_num_bounds, 400, 599
};


static ngx_command_t  ngx_http_var_limit_req_commands[] = {

    { ngx_string("var_limit_req_zone"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE3|NGX_CONF_TAKE4|NGX_CONF_TAKE5
          |NGX_CONF_TAKE6|NGX_CONF_TAKE7,
      ngx_http_var_limit_req_zone,
      0,
      0,
      NULL },

    { ngx_string("var_limit_req"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE123,
      ngx_http_var_limit_req,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("var_limit_req_log_level"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_enum_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_var_limit_req_conf_t, limit_log_level),
      &ngx_http_var_limit_req_log_levels },

    { ngx_string("var_limit_req_status"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_num_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_var_limit_req_conf_t, status_code),
      &ngx_http_var_limit_req_status_bounds },

    { ngx_string("var_limit_req_dry_run"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_FLAG,
      ngx_conf_set_flag_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_var_limit_req_conf_t, dry_run),
      NULL },

    { ngx_string("var_limit_req_top"),
      NGX_HTTP_LOC_CONF|NGX_CONF_TAKE2,
      ngx_http_var_limit_req_top,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },

      ngx_null_command
};


static ngx_http_module_t  ngx_http_var_limit_req_module_ctx = {
    ngx_http_var_limit_req_add_variables,  /* preconfiguration */
    ngx_http_var_limit_req_init,           /* postconfiguration */

    NULL,                                  /* create main configuration */
    NULL,                                  /* init main configuration */

    NULL,                                  /* create server configuration */
    NULL,                                  /* merge server configuration */

    ngx_http_var_limit_req_create_conf,    /* create location configuration */
    ngx_http_var_limit_req_merge_conf      /* merge location configuration */
};


ngx_module_t  ngx_http_var_limit_req_module = {
    NGX_MODULE_V1,
    &ngx_http_var_limit_req_module_ctx,    /* module context */
    ngx_http_var_limit_req_commands,       /* module directives */
    NGX_HTTP_MODULE,                       /* module type */
    NULL,                                  /* init master */
    NULL,                                  /* init module */
    NULL,                                  /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    NULL,                                  /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};


static ngx_http_variable_t  ngx_http_var_limit_req_vars[] = {

    { ngx_string("var_limit_req_status"), NULL,
      ngx_http_var_limit_req_status_variable, 0, NGX_HTTP_VAR_NOCACHEABLE, 0 },

      ngx_http_null_variable
};


static ngx_str_t  ngx_http_var_limit_req_status[] = {
    ngx_string("PASSED"),
    ngx_string("DELAYED"),
    ngx_string("REJECTED"),
    ngx_string("DELAYED_DRY_RUN"),
    ngx_string("REJECTED_DRY_RUN")
};


static char  *week[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static char  *months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                           "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };


static ngx_int_t
ngx_http_var_limit_req_handler(ngx_http_request_t *r)
{
    uint32_t                         hash;
    ngx_str_t                        key, rate_var, burst_var, dry_run_var;
    ngx_int_t                        rc;
    ngx_uint_t                       n, excess, rate, scale, burst;
    ngx_msec_t                       delay;
    ngx_http_var_limit_req_ctx_t    *ctx;
    ngx_http_var_limit_req_conf_t   *lrcf;
    ngx_http_var_limit_req_limit_t  *limit, *limits;
    u_char                          *p;
    size_t                           len;
    ngx_flag_t                       dry_run;

    if (r->main->limit_req_status) {
        return NGX_DECLINED;
    }

    lrcf = ngx_http_get_module_loc_conf(r, ngx_http_var_limit_req_module);
    limits = lrcf->limits.elts;

    excess = 0;
    rate = 0;

    rc = NGX_DECLINED;

#if (NGX_SUPPRESS_WARN)
    limit = NULL;
#endif

    dry_run = lrcf->dry_run;

    for (n = 0; n < lrcf->limits.nelts; n++) {

        limit = &limits[n];

        ctx = limit->shm_zone->data;

        if (ngx_http_complex_value(r, &ctx->key, &key) != NGX_OK) {
            ngx_http_var_limit_req_unlock(limits, n);
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }

        if (key.len == 0) {
            continue;
        }

        if (key.len > 65535) {
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                          "the value of the \"%V\" key "
                          "is more than 65535 bytes: \"%V\"",
                          &ctx->key.value, &key);
            continue;
        }

        hash = ngx_crc32_short(key.data, key.len);

        rate = ctx->rate;
        if (ngx_http_complex_value(r, &ctx->rate_var, &rate_var) != NGX_OK) {
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                          "could not get rate_var value");
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }
        if (rate_var.len != 0) {
            scale = 1;
            len = rate_var.len;
            if (len >= 3) {
                p = rate_var.data + len - 3;

                if (ngx_strncmp(p, "r/s", 3) == 0) {
                    len -= 3;

                } else if (ngx_strncmp(p, "r/m", 3) == 0) {
                    scale = 60;
                    len -= 3;
                }
            }

            rate = ngx_atoi(rate_var.data, len);
            if (rate <= 0) {
                ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                            "invalid rate_var value \"%V\"", &rate_var);
                return NGX_HTTP_INTERNAL_SERVER_ERROR;
            }

            rate *= 1000 / scale;
        }

        burst = limit->burst;
        if (ngx_http_complex_value(r, &ctx->burst_var, &burst_var) != NGX_OK) {
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                          "could not get burst_var value");
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }
        if (burst_var.len != 0) {
            burst = ngx_atoi(burst_var.data, burst_var.len);
            if (burst <= 0) {
                ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                              "invalid burst_var value \"%V\"", &burst_var);
                return NGX_HTTP_INTERNAL_SERVER_ERROR;
            }

            burst *= 1000;
        }

        if (ngx_http_complex_value(r, &ctx->dry_run_var, &dry_run_var) != NGX_OK) {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }
        if (dry_run_var.len != 0) {
            if (ngx_strcasecmp(dry_run_var.data, (u_char *) "on") == 0) {
                dry_run = 1;

            } else if (ngx_strcasecmp(dry_run_var.data, (u_char *) "off") == 0) {
                dry_run = 0;

            } else {
                ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                              "the value of the \"%V\" key "
                              "must be \"on\" or \"off\": \"%V\"",
                              &ctx->dry_run_var.value, &dry_run_var);
                continue;
            }
        }

        ngx_shmtx_lock(&ctx->shpool->mutex);

        rc = ngx_http_var_limit_req_lookup(r, limit, hash, &key, &excess,
                                           (n == lrcf->limits.nelts - 1),
                                           rate, burst);

        ngx_shmtx_unlock(&ctx->shpool->mutex);

        ngx_log_debug4(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                       "var_limit_req[%ui]: %i %ui.%03ui",
                       n, rc, excess / 1000, excess % 1000);

        if (rc != NGX_AGAIN) {
            break;
        }
    }

    if (rc == NGX_DECLINED) {
        return NGX_DECLINED;
    }

    if (rc == NGX_BUSY || rc == NGX_ERROR) {

        if (rc == NGX_BUSY) {
            ngx_log_error(lrcf->limit_log_level, r->connection->log, 0,
                        "limiting requests%s, excess: %ui.%03ui by zone \"%V\"",
                        dry_run ? ", dry run" : "",
                        excess / 1000, excess % 1000,
                        &limit->shm_zone->shm.name);
        }

        ngx_http_var_limit_req_unlock(limits, n);

        if (dry_run) {
            r->main->limit_req_status = NGX_HTTP_VAR_LIMIT_REQ_REJECTED_DRY_RUN;
            return NGX_DECLINED;
        }

        r->main->limit_req_status = NGX_HTTP_VAR_LIMIT_REQ_REJECTED;

        return ngx_http_var_limit_req_get_status(r, ctx);
    }

    /* rc == NGX_AGAIN || rc == NGX_OK */

    if (rc == NGX_AGAIN) {
        excess = 0;
    }

    delay = ngx_http_var_limit_req_account(limits, n, &excess, &limit, rate);

    if (!delay) {
        r->main->limit_req_status = NGX_HTTP_VAR_LIMIT_REQ_PASSED;
        return NGX_DECLINED;
    }

    ngx_log_error(lrcf->delay_log_level, r->connection->log, 0,
                  "delaying request%s, excess: %ui.%03ui, by zone \"%V\"",
                  dry_run ? ", dry run" : "",
                  excess / 1000, excess % 1000, &limit->shm_zone->shm.name);

    if (dry_run) {
        r->main->limit_req_status = NGX_HTTP_VAR_LIMIT_REQ_DELAYED_DRY_RUN;
        return NGX_DECLINED;
    }

    r->main->limit_req_status = NGX_HTTP_VAR_LIMIT_REQ_DELAYED;

    if (r->connection->read->ready) {
        ngx_post_event(r->connection->read, &ngx_posted_events);

    } else {
        if (ngx_handle_read_event(r->connection->read, 0) != NGX_OK) {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }
    }

    r->read_event_handler = ngx_http_test_reading;
    r->write_event_handler = ngx_http_var_limit_req_delay;

    r->connection->write->delayed = 1;
    ngx_add_timer(r->connection->write, delay);

    return NGX_AGAIN;
}


static ngx_uint_t
ngx_http_var_limit_req_get_status(ngx_http_request_t *r,
    ngx_http_var_limit_req_ctx_t *ctx)
{
    ngx_http_var_limit_req_conf_t      *lrcf;
    ngx_str_t                           status_var;
    ngx_int_t                           status;

    if (ngx_http_complex_value(r, &ctx->status_var, &status_var) != NGX_OK) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }
    if (status_var.len != 0) {
        status = ngx_atoi(status_var.data, status_var.len);
        if (status >= 400 && status <= 599) {
            return (ngx_uint_t) status;
        }
    }
    lrcf = ngx_http_get_module_loc_conf(r, ngx_http_var_limit_req_module);
    return lrcf->status_code;
}


static void
ngx_http_var_limit_req_delay(ngx_http_request_t *r)
{
    ngx_event_t  *wev;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "var_limit_req delay");

    wev = r->connection->write;

    if (wev->delayed) {

        if (ngx_handle_write_event(wev, 0) != NGX_OK) {
            ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
        }

        return;
    }

    if (ngx_handle_read_event(r->connection->read, 0) != NGX_OK) {
        ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
        return;
    }

    r->read_event_handler = ngx_http_block_reading;
    r->write_event_handler = ngx_http_core_run_phases;

    ngx_http_core_run_phases(r);
}


static void
ngx_http_var_limit_req_rbtree_insert_value(ngx_rbtree_node_t *temp,
    ngx_rbtree_node_t *node, ngx_rbtree_node_t *sentinel)
{
    ngx_rbtree_node_t              **p;
    ngx_http_var_limit_req_node_t   *lrn, *lrnt;

    for ( ;; ) {

        if (node->key < temp->key) {

            p = &temp->left;

        } else if (node->key > temp->key) {

            p = &temp->right;

        } else { /* node->key == temp->key */

            lrn = (ngx_http_var_limit_req_node_t *) &node->color;
            lrnt = (ngx_http_var_limit_req_node_t *) &temp->color;

            p = (ngx_memn2cmp(lrn->data, lrnt->data, lrn->len, lrnt->len) < 0)
                ? &temp->left : &temp->right;
        }

        if (*p == sentinel) {
            break;
        }

        temp = *p;
    }

    *p = node;
    node->parent = temp;
    node->left = sentinel;
    node->right = sentinel;
    ngx_rbt_red(node);
}


static ngx_int_t
ngx_http_var_limit_req_lookup(ngx_http_request_t *r,
    ngx_http_var_limit_req_limit_t *limit, ngx_uint_t hash,
    ngx_str_t *key, ngx_uint_t *ep, ngx_uint_t account,
    ngx_uint_t rate, ngx_uint_t burst)
{
    size_t                          size;
    ngx_int_t                       rc;
    ngx_msec_t                      now;
    ngx_time_t                      now_time;
    ngx_msec_int_t                  ms;
    ngx_rbtree_node_t              *node, *sentinel;
    ngx_http_var_limit_req_ctx_t   *ctx;
    ngx_http_var_limit_req_node_t  *lr;

    now = ngx_current_msec;
    now_time = *ngx_cached_time;

    ctx = limit->shm_zone->data;

    node = ctx->sh->rbtree.root;
    sentinel = ctx->sh->rbtree.sentinel;

    while (node != sentinel) {

        if (hash < node->key) {
            node = node->left;
            continue;
        }

        if (hash > node->key) {
            node = node->right;
            continue;
        }

        /* hash == node->key */

        lr = (ngx_http_var_limit_req_node_t *) &node->color;

        rc = ngx_memn2cmp(key->data, lr->data, key->len, (size_t) lr->len);

        if (rc == 0) {
            ngx_queue_remove(&lr->queue);
            ngx_queue_insert_head(&ctx->sh->queue, &lr->queue);

            ms = ngx_http_var_limit_req_duration_after_last_access(now,
                                                                   lr->last);
            *ep = ngx_http_var_limit_req_adjust_excess(lr->excess, ms, rate, 0);
            lr->rate = rate;
            lr->burst = burst;

            if ((ngx_uint_t) *ep > burst) {
                return NGX_BUSY;
            }

            if (account) {
                lr->excess = *ep;

                if (ms) {
                    lr->last = now;
                    lr->last_time = now_time;
                }

                return NGX_OK;
            }

            lr->count++;

            ctx->node = lr;

            return NGX_AGAIN;
        }

        node = (rc < 0) ? node->left : node->right;
    }

    *ep = 0;

    size = offsetof(ngx_rbtree_node_t, color)
           + offsetof(ngx_http_var_limit_req_node_t, data)
           + key->len;

    ngx_http_var_limit_req_expire(ctx, 1, rate);

    node = ngx_slab_alloc_locked(ctx->shpool, size);

    if (node == NULL) {
        ngx_http_var_limit_req_expire(ctx, 0, rate);

        node = ngx_slab_alloc_locked(ctx->shpool, size);
        if (node == NULL) {
            ngx_log_error(NGX_LOG_ALERT, ngx_cycle->log, 0,
                          "could not allocate node%s", ctx->shpool->log_ctx);
            return NGX_ERROR;
        }
    }

    node->key = hash;

    lr = (ngx_http_var_limit_req_node_t *) &node->color;

    lr->len = (u_short) key->len;
    lr->excess = 0;

    ngx_memcpy(lr->data, key->data, key->len);

    ngx_rbtree_insert(&ctx->sh->rbtree, node);

    ngx_queue_insert_head(&ctx->sh->queue, &lr->queue);

    lr->rate = rate;
    lr->burst = burst;

    if (account) {
        lr->last = now;
        lr->last_time = now_time;
        lr->count = 0;
        return NGX_OK;
    }

    lr->last = 0;
    ngx_memzero(&lr->last_time, sizeof(ngx_time_t));
    lr->count = 1;

    ctx->node = lr;

    return NGX_AGAIN;
}


static ngx_uint_t
ngx_http_var_limit_req_adjust_excess(ngx_uint_t raw_excess, ngx_msec_int_t ms,
    ngx_uint_t rate, ngx_flag_t just_monitoring)
{
    ngx_int_t                       excess;

    excess = raw_excess - rate * ms / 1000 + (just_monitoring ? 0 : 1000);

    if (excess < 0) {
        excess = 0;
    }

    return (ngx_uint_t)excess;
}


static ngx_msec_int_t
ngx_http_var_limit_req_duration_after_last_access(ngx_msec_t now,
    ngx_msec_t last)
{
    ngx_msec_int_t                  ms;

    ms = (ngx_msec_int_t) (now - last);

    if (ms < -60000) {
        ms = 1;

    } else if (ms < 0) {
        ms = 0;
    }

    return ms;
}


static ngx_msec_t
ngx_http_var_limit_req_account(ngx_http_var_limit_req_limit_t *limits, ngx_uint_t n,
    ngx_uint_t *ep, ngx_http_var_limit_req_limit_t **limit, ngx_uint_t rate)
{
    ngx_uint_t                      excess;
    ngx_msec_t                      now, delay, max_delay;
    ngx_time_t                      now_time;
    ngx_msec_int_t                  ms;
    ngx_http_var_limit_req_ctx_t   *ctx;
    ngx_http_var_limit_req_node_t  *lr;

    excess = *ep;

    if ((ngx_uint_t) excess <= (*limit)->delay) {
        max_delay = 0;

    } else {
        ctx = (*limit)->shm_zone->data;
        max_delay = (excess - (*limit)->delay) * 1000 / rate;
    }

    while (n--) {
        ctx = limits[n].shm_zone->data;
        lr = ctx->node;

        if (lr == NULL) {
            continue;
        }

        ngx_shmtx_lock(&ctx->shpool->mutex);

        now = ngx_current_msec;
        now_time = *ngx_cached_time;

        ms = ngx_http_var_limit_req_duration_after_last_access(now, lr->last);
        excess = ngx_http_var_limit_req_adjust_excess(lr->excess, ms, rate, 0);

        if (ms) {
            lr->last = now;
            lr->last_time = now_time;
        }

        lr->excess = excess;
        lr->count--;

        ngx_shmtx_unlock(&ctx->shpool->mutex);

        ctx->node = NULL;

        if ((ngx_uint_t) excess <= limits[n].delay) {
            continue;
        }

        delay = (excess - limits[n].delay) * 1000 / rate;

        if (delay > max_delay) {
            max_delay = delay;
            *ep = excess;
            *limit = &limits[n];
        }
    }

    return max_delay;
}


static void
ngx_http_var_limit_req_unlock(ngx_http_var_limit_req_limit_t *limits, ngx_uint_t n)
{
    ngx_http_var_limit_req_ctx_t  *ctx;

    while (n--) {
        ctx = limits[n].shm_zone->data;

        if (ctx->node == NULL) {
            continue;
        }

        ngx_shmtx_lock(&ctx->shpool->mutex);

        ctx->node->count--;

        ngx_shmtx_unlock(&ctx->shpool->mutex);

        ctx->node = NULL;
    }
}


static void
ngx_http_var_limit_req_expire(ngx_http_var_limit_req_ctx_t *ctx, ngx_uint_t n,
    ngx_uint_t rate)
{
    ngx_uint_t                      excess;
    ngx_msec_t                      now;
    ngx_queue_t                    *q;
    ngx_msec_int_t                  ms;
    ngx_rbtree_node_t              *node;
    ngx_http_var_limit_req_node_t  *lr;

    now = ngx_current_msec;

    /*
     * n == 1 deletes one or two zero rate entries
     * n == 0 deletes oldest entry by force
     *        and one or two zero rate entries
     */

    while (n < 3) {

        if (ngx_queue_empty(&ctx->sh->queue)) {
            return;
        }

        q = ngx_queue_last(&ctx->sh->queue);

        lr = ngx_queue_data(q, ngx_http_var_limit_req_node_t, queue);

        if (lr->count) {

            /*
             * There is not much sense in looking further,
             * because we bump nodes on the lookup stage.
             */

            return;
        }

        if (n++ != 0) {

            ms = (ngx_msec_int_t) (now - lr->last);
            ms = ngx_abs(ms);

            if (ms < 60000) {
                return;
            }

            excess = ngx_http_var_limit_req_adjust_excess(lr->excess, ms, rate,
                                                          1);

            if (excess > 0) {
                return;
            }
        }

        ngx_queue_remove(q);

        node = (ngx_rbtree_node_t *)
                   ((u_char *) lr - offsetof(ngx_rbtree_node_t, color));

        ngx_rbtree_delete(&ctx->sh->rbtree, node);

        ngx_slab_free_locked(ctx->shpool, node);
    }
}


static ngx_int_t
ngx_http_var_limit_req_init_zone(ngx_shm_zone_t *shm_zone, void *data)
{
    ngx_http_var_limit_req_ctx_t  *octx = data;

    size_t                         len;
    ngx_http_var_limit_req_ctx_t  *ctx;

    ctx = shm_zone->data;

    if (octx) {
        if (ctx->key.value.len != octx->key.value.len
            || ngx_strncmp(ctx->key.value.data, octx->key.value.data,
                           ctx->key.value.len)
               != 0)
        {
            ngx_log_error(NGX_LOG_EMERG, shm_zone->shm.log, 0,
                          "var_limit_req \"%V\" uses the \"%V\" key "
                          "while previously it used the \"%V\" key",
                          &shm_zone->shm.name, &ctx->key.value,
                          &octx->key.value);
            return NGX_ERROR;
        }

        ctx->sh = octx->sh;
        ctx->shpool = octx->shpool;

        return NGX_OK;
    }

    ctx->shpool = (ngx_slab_pool_t *) shm_zone->shm.addr;

    if (shm_zone->shm.exists) {
        ctx->sh = ctx->shpool->data;

        return NGX_OK;
    }

    ctx->sh = ngx_slab_alloc(ctx->shpool, sizeof(ngx_http_var_limit_req_shctx_t));
    if (ctx->sh == NULL) {
        return NGX_ERROR;
    }

    ctx->shpool->data = ctx->sh;

    ngx_rbtree_init(&ctx->sh->rbtree, &ctx->sh->sentinel,
                    ngx_http_var_limit_req_rbtree_insert_value);

    ngx_queue_init(&ctx->sh->queue);

    len = sizeof(" in limit_req zone \"\"") + shm_zone->shm.name.len;

    ctx->shpool->log_ctx = ngx_slab_alloc(ctx->shpool, len);
    if (ctx->shpool->log_ctx == NULL) {
        return NGX_ERROR;
    }

    ngx_sprintf(ctx->shpool->log_ctx, " in limit_req zone \"%V\"%Z",
                &shm_zone->shm.name);

    ctx->shpool->log_nomem = 0;

    return NGX_OK;
}


static ngx_int_t
ngx_http_var_limit_req_status_variable(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data)
{
    if (r->main->limit_req_status == 0) {
        v->not_found = 1;
        return NGX_OK;
    }

    v->valid = 1;
    v->no_cacheable = 0;
    v->not_found = 0;
    v->len = ngx_http_var_limit_req_status[r->main->limit_req_status - 1].len;
    v->data = ngx_http_var_limit_req_status[r->main->limit_req_status - 1].data;

    return NGX_OK;
}


static void *
ngx_http_var_limit_req_create_conf(ngx_conf_t *cf)
{
    ngx_http_var_limit_req_conf_t  *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_var_limit_req_conf_t));
    if (conf == NULL) {
        return NULL;
    }

    /*
     * set by ngx_pcalloc():
     *
     *     conf->limits.elts = NULL;
     */

    conf->limit_log_level = NGX_CONF_UNSET_UINT;
    conf->status_code = NGX_CONF_UNSET_UINT;
    conf->dry_run = NGX_CONF_UNSET;

    return conf;
}


static char *
ngx_http_var_limit_req_merge_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_var_limit_req_conf_t *prev = parent;
    ngx_http_var_limit_req_conf_t *conf = child;

    if (conf->limits.elts == NULL) {
        conf->limits = prev->limits;
    }

    ngx_conf_merge_uint_value(conf->limit_log_level, prev->limit_log_level,
                              NGX_LOG_ERR);

    conf->delay_log_level = (conf->limit_log_level == NGX_LOG_INFO) ?
                                NGX_LOG_INFO : conf->limit_log_level + 1;

    ngx_conf_merge_uint_value(conf->status_code, prev->status_code,
                              NGX_HTTP_SERVICE_UNAVAILABLE);

    ngx_conf_merge_value(conf->dry_run, prev->dry_run, 0);

    return NGX_CONF_OK;
}


static char *
ngx_http_var_limit_req_zone(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    u_char                            *p;
    size_t                             len;
    ssize_t                            size;
    ngx_str_t                         *value, name, s;
    ngx_int_t                          rate, scale;
    ngx_uint_t                         i;
    ngx_shm_zone_t                    *shm_zone;
    ngx_http_var_limit_req_ctx_t      *ctx;
    ngx_http_compile_complex_value_t   ccv;

    value = cf->args->elts;

    ctx = ngx_pcalloc(cf->pool, sizeof(ngx_http_var_limit_req_ctx_t));
    if (ctx == NULL) {
        return NGX_CONF_ERROR;
    }

    ngx_memzero(&ccv, sizeof(ngx_http_compile_complex_value_t));

    ccv.cf = cf;
    ccv.value = &value[1];
    ccv.complex_value = &ctx->key;

    if (ngx_http_compile_complex_value(&ccv) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    size = 0;
    rate = 1;
    scale = 1;
    name.len = 0;

    for (i = 2; i < cf->args->nelts; i++) {

        if (ngx_strncmp(value[i].data, "zone=", 5) == 0) {

            name.data = value[i].data + 5;

            p = (u_char *) ngx_strchr(name.data, ':');

            if (p == NULL) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "invalid zone size \"%V\"", &value[i]);
                return NGX_CONF_ERROR;
            }

            name.len = p - name.data;

            s.data = p + 1;
            s.len = value[i].data + value[i].len - s.data;

            size = ngx_parse_size(&s);

            if (size == NGX_ERROR) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "invalid zone size \"%V\"", &value[i]);
                return NGX_CONF_ERROR;
            }

            if (size < (ssize_t) (8 * ngx_pagesize)) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "zone \"%V\" is too small", &value[i]);
                return NGX_CONF_ERROR;
            }

            continue;
        }

        if (ngx_strncmp(value[i].data, "rate=", 5) == 0) {

            len = value[i].len;
            p = value[i].data + len - 3;

            if (ngx_strncmp(p, "r/s", 3) == 0) {
                scale = 1;
                len -= 3;

            } else if (ngx_strncmp(p, "r/m", 3) == 0) {
                scale = 60;
                len -= 3;
            }

            rate = ngx_atoi(value[i].data + 5, len - 5);
            if (rate <= 0) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "invalid rate \"%V\"", &value[i]);
                return NGX_CONF_ERROR;
            }

            continue;
        }

        if (ngx_strncmp(value[i].data, "rate_var=", 9) == 0) {

            s.data = value[i].data + 9;
            s.len = value[i].len - 9;

            ngx_memzero(&ccv, sizeof(ngx_http_compile_complex_value_t));

            ccv.cf = cf;
            ccv.value = &s;
            ccv.complex_value = &ctx->rate_var;

            if (ngx_http_compile_complex_value(&ccv) != NGX_OK) {
                return NGX_CONF_ERROR;
            }

            continue;
        }

        if (ngx_strncmp(value[i].data, "burst_var=", 10) == 0) {

            s.data = value[i].data + 10;
            s.len = value[i].len - 10;

            ngx_memzero(&ccv, sizeof(ngx_http_compile_complex_value_t));

            ccv.cf = cf;
            ccv.value = &s;
            ccv.complex_value = &ctx->burst_var;

            if (ngx_http_compile_complex_value(&ccv) != NGX_OK) {
                return NGX_CONF_ERROR;
            }

            continue;
        }

        if (ngx_strncmp(value[i].data, "dry_run_var=", 12) == 0) {

            s.data = value[i].data + 12;
            s.len = value[i].len - 12;

            ngx_memzero(&ccv, sizeof(ngx_http_compile_complex_value_t));

            ccv.cf = cf;
            ccv.value = &s;
            ccv.complex_value = &ctx->dry_run_var;

            if (ngx_http_compile_complex_value(&ccv) != NGX_OK) {
                return NGX_CONF_ERROR;
            }

            continue;
        }

        if (ngx_strncmp(value[i].data, "status_var=", 11) == 0) {

            s.data = value[i].data + 11;
            s.len = value[i].len - 11;

            ngx_memzero(&ccv, sizeof(ngx_http_compile_complex_value_t));

            ccv.cf = cf;
            ccv.value = &s;
            ccv.complex_value = &ctx->status_var;

            if (ngx_http_compile_complex_value(&ccv) != NGX_OK) {
                return NGX_CONF_ERROR;
            }

            continue;
        }

        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid parameter \"%V\"", &value[i]);
        return NGX_CONF_ERROR;
    }

    if (name.len == 0) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "\"%V\" must have \"zone\" parameter",
                           &cmd->name);
        return NGX_CONF_ERROR;
    }

    ctx->rate = rate * 1000 / scale;

    shm_zone = ngx_shared_memory_add(cf, &name, size,
                                     &ngx_http_var_limit_req_module);
    if (shm_zone == NULL) {
        return NGX_CONF_ERROR;
    }

    if (shm_zone->data) {
        ctx = shm_zone->data;

        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "%V \"%V\" is already bound to key \"%V\"",
                           &cmd->name, &name, &ctx->key.value);
        return NGX_CONF_ERROR;
    }

    shm_zone->init = ngx_http_var_limit_req_init_zone;
    shm_zone->data = ctx;

    return NGX_CONF_OK;
}


static char *
ngx_http_var_limit_req(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_var_limit_req_conf_t  *lrcf = conf;

    ngx_int_t                        burst, delay;
    ngx_str_t                       *value, s;
    ngx_uint_t                       i;
    ngx_shm_zone_t                  *shm_zone;
    ngx_http_var_limit_req_limit_t  *limit, *limits;

    value = cf->args->elts;

    shm_zone = NULL;
    burst = 0;
    delay = 0;

    for (i = 1; i < cf->args->nelts; i++) {

        if (ngx_strncmp(value[i].data, "zone=", 5) == 0) {

            s.len = value[i].len - 5;
            s.data = value[i].data + 5;

            shm_zone = ngx_shared_memory_add(cf, &s, 0,
                                             &ngx_http_var_limit_req_module);
            if (shm_zone == NULL) {
                return NGX_CONF_ERROR;
            }

            continue;
        }

        if (ngx_strncmp(value[i].data, "burst=", 6) == 0) {

            burst = ngx_atoi(value[i].data + 6, value[i].len - 6);
            if (burst <= 0) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "invalid burst value \"%V\"", &value[i]);
                return NGX_CONF_ERROR;
            }

            continue;
        }

        if (ngx_strncmp(value[i].data, "delay=", 6) == 0) {

            delay = ngx_atoi(value[i].data + 6, value[i].len - 6);
            if (delay <= 0) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "invalid delay value \"%V\"", &value[i]);
                return NGX_CONF_ERROR;
            }

            continue;
        }

        if (ngx_strcmp(value[i].data, "nodelay") == 0) {
            delay = NGX_MAX_INT_T_VALUE / 1000;
            continue;
        }

        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid parameter \"%V\"", &value[i]);
        return NGX_CONF_ERROR;
    }

    if (shm_zone == NULL) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "\"%V\" must have \"zone\" parameter",
                           &cmd->name);
        return NGX_CONF_ERROR;
    }

    limits = lrcf->limits.elts;

    if (limits == NULL) {
        if (ngx_array_init(&lrcf->limits, cf->pool, 1,
                           sizeof(ngx_http_var_limit_req_limit_t))
            != NGX_OK)
        {
            return NGX_CONF_ERROR;
        }
    }

    for (i = 0; i < lrcf->limits.nelts; i++) {
        if (shm_zone == limits[i].shm_zone) {
            return "is duplicate";
        }
    }

    limit = ngx_array_push(&lrcf->limits);
    if (limit == NULL) {
        return NGX_CONF_ERROR;
    }

    limit->shm_zone = shm_zone;
    limit->burst = burst * 1000;
    limit->delay = delay * 1000;

    return NGX_CONF_OK;
}


static ngx_int_t
ngx_http_var_limit_req_top_handler(ngx_http_request_t *r)
{
    ngx_http_var_limit_req_conf_t  *lrcf;
    ngx_shm_zone_t                 *shm_zone;
    ngx_http_var_limit_req_ctx_t   *ctx;
    ngx_int_t                       rc;
    ngx_chain_t                     out;
    ngx_array_t                     items;

    lrcf = ngx_http_get_module_loc_conf(r, ngx_http_var_limit_req_module);
    if (lrcf == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    shm_zone = lrcf->top_shm_zone;
    if (shm_zone == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    rc = ngx_http_discard_request_body(r);
    if (rc != NGX_OK) {
        return rc;
    }

    r->headers_out.content_type_len = sizeof("text/plain") - 1;
    ngx_str_set(&r->headers_out.content_type, "text/plain");
    r->headers_out.content_type_lowcase = NULL;

    ctx = shm_zone->data;
    ngx_shmtx_lock(&ctx->shpool->mutex);

    rc = ngx_http_var_limit_req_top_build_items(r, &ctx->sh->rbtree, &items);

    ngx_shmtx_unlock(&ctx->shpool->mutex);

    if (rc != NGX_OK) {
        return rc;
    }

    rc = ngx_http_var_limit_req_top_build_response(r, &items, &out);

    if (rc != NGX_OK) {
        return rc;
    }

    rc = ngx_http_send_header(r);

    if (rc == NGX_ERROR || rc > NGX_OK || r->header_only) {
        return rc;
    }

    return ngx_http_output_filter(r, &out);
}


static ngx_int_t
ngx_http_var_limit_req_top_build_items(ngx_http_request_t *r,
    ngx_rbtree_t *rbtree, ngx_array_t *items)
{
    ngx_http_var_limit_req_conf_t      *lrcf;
    ngx_rbtree_node_t                  *node, *root, *sentinel;
    ngx_http_var_limit_req_node_t      *lcn;
    ngx_int_t                           rc;
    ngx_uint_t                          i, top_n, old_nelts;
    ngx_http_var_limit_req_top_item_t  *item, tmp_item;
    ngx_msec_t                          now;
    ngx_msec_int_t                      ms;

    lrcf = ngx_http_get_module_loc_conf(r, ngx_http_var_limit_req_module);
    top_n = lrcf->default_n;

    sentinel = rbtree->sentinel;
    root = rbtree->root;

    rc = ngx_array_init(items, r->pool, top_n,
                        sizeof(ngx_http_var_limit_req_top_item_t));
    if (rc != NGX_OK) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    if (root == sentinel) {
        return NGX_OK;
    }

    now = ngx_current_msec;

    /* keep only top n items whlie looping using binary search */
    for (node = ngx_rbtree_min(root, sentinel);
         node;
         node = ngx_rbtree_next(rbtree, node))
    {
        lcn = (ngx_http_var_limit_req_node_t *) &node->color;

        ms = ngx_http_var_limit_req_duration_after_last_access(now, lcn->last);
        tmp_item.adjusted_excess = ngx_http_var_limit_req_adjust_excess(
            lcn->excess, ms, lcn->rate, 1);
        tmp_item.key.len = lcn->len;
        tmp_item.key.data = lcn->data;
        tmp_item.last = lcn->last;
        tmp_item.last_time = lcn->last_time;
        tmp_item.raw_excess = lcn->excess;
        tmp_item.rate = lcn->rate;
        tmp_item.burst = lcn->burst;

        i = ngx_http_var_limit_req_binary_search(items, &tmp_item);
        if (i > top_n) {
            continue;
        }

        old_nelts = items->nelts;
        if (old_nelts < top_n) {
            /* no error or realloc happens since we initialized items with
             * top_n capacity.
             */
            item = ngx_array_push(items);
            /*
             * ex. top_n = 5, old_nelts = 4, i = 1
             *
             * | a | b | c | d |
             *       ^i
             */
            if (i < old_nelts) {
                ngx_memmove(ngx_array_item(items, i + 1),
                            ngx_array_item(items, i),
                            items->size * (old_nelts - i));
            }

        } else {
            item = ngx_array_item(items, i);

            /*
             * ex2. top_n = 5, old_nelts = 5, i = 1
             *
             * | a | b | c | d | e |
             *       ^i
             */
            if (i < old_nelts) {
                ngx_memmove(ngx_array_item(items, i + 1),
                            ngx_array_item(items, i),
                            items->size * (old_nelts - i - 1));
            }
        }

        *item = tmp_item;
    }

    /* Copy top n entries keys */
    for (i = 0; i < items->nelts; ++i) {
        item = ngx_array_item(items, i);
        item->key.data = ngx_pstrdup(r->pool, &item->key);
        if (item->key.data == NULL) {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }
    }

    return NGX_OK;
}


static ngx_uint_t
ngx_http_var_limit_req_binary_search(ngx_array_t *items,
    const ngx_http_var_limit_req_top_item_t *item)
{
    ngx_uint_t                                min_index, one_past_max_index,
                                              index;
    ngx_int_t                                 res;
    const ngx_http_var_limit_req_top_item_t  *item2;

    min_index = 0;
    one_past_max_index = items->nelts;
    while (one_past_max_index != min_index) {
        index = (min_index + one_past_max_index) / 2;
        item2 = ngx_array_item(items, index);
        res = ngx_http_var_limit_req_top_item_cmp(item, item2);
        if (res == 0) {
            return index;
        }
        if (res < 0) {
            one_past_max_index = index;
        } else {
            min_index = index + 1;
        }
    }
    return min_index;
}


static ngx_int_t
ngx_http_var_limit_req_top_build_response(ngx_http_request_t *r,
    ngx_array_t *items, ngx_chain_t *out)
{
    ngx_uint_t                           i;
    ngx_http_var_limit_req_top_item_t  *item, *items_ptr;
    size_t                               buf_size = 0;
    ngx_buf_t                           *b;
    u_char               http_time_buf[sizeof("Mon, 28 Sep 1970 06:00:00 GMT")];

    if (items->nelts == 0) {
        r->header_only = 1;
        r->headers_out.status = NGX_HTTP_NO_CONTENT;
        return NGX_OK;
    }

    items_ptr = items->elts;
    for (i = 0; i < items->nelts; i++) {
        item = &items_ptr[i];
        buf_size += lit_len("key:") + item->key.len
                    + lit_len("\tadjusted_excess:") + NGX_SIZE_T_LEN
                    + lit_len("\traw_excess:") + NGX_SIZE_T_LEN
                    + lit_len("\trate:") + NGX_SIZE_T_LEN
                    + lit_len("\tburst:") + NGX_SIZE_T_LEN
                    + lit_len("\tlast:") + NGX_SIZE_T_LEN
                    + lit_len("\tlast_time:Mon, 28 Sep 1970 06:00:00 GMT\n");
    }

    b = ngx_create_temp_buf(r->pool, buf_size);
    if (b == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    // TODO: remove sort here
    ngx_sort(items->elts, items->nelts,
        sizeof(ngx_http_var_limit_req_top_item_t),
        ngx_http_var_limit_req_top_item_cmp);

    for (i = 0; i < items->nelts; i++) {
        item = &items_ptr[i];
        ngx_http_var_limit_req_format_http_time(item->last_time.sec,
                                                http_time_buf);
        b->last = ngx_sprintf(b->last,
                              "key:%V\tadjusted_excess:%ud"
                              "\traw_excess:%ud\trate:%ud\tburst:%ud"
                              "\tlast:%ud"
                              "\tlast_time:%*s\n",
                              &item->key, item->adjusted_excess,
                              item->raw_excess, item->rate, item->burst,
                              item->last,
                              (int)sizeof(http_time_buf) - 1, http_time_buf);
    }

    b->last_buf = 1;
    b->last_in_chain = 1;

    out->buf = b;
    out->next = NULL;
    b->last_buf = 1;
    r->headers_out.content_length_n = b->last - b->pos;
    r->headers_out.status = NGX_HTTP_OK;
    return NGX_OK;
}


static ngx_int_t
ngx_http_var_limit_req_top_item_cmp(const void *a, const void *b)
{
    const ngx_http_var_limit_req_top_item_t  *item_a, *item_b;
    size_t                                     n;
    ngx_int_t                                  rc;

    item_a = a;
    item_b = b;

    /* order by adjusted_excess desc, raw_excess desc, last desc, key asc */

    if (item_a->adjusted_excess > item_b->adjusted_excess) {
        return -1;
    }
    if (item_a->adjusted_excess < item_b->adjusted_excess) {
        return 1;
    }

    if (item_a->raw_excess > item_b->raw_excess) {
        return -1;
    }
    if (item_a->raw_excess < item_b->raw_excess) {
        return 1;
    }

    if (item_a->last > item_b->last) {
        return -1;
    }
    if (item_a->last < item_b->last) {
        return 1;
    }

    n = ngx_min(item_a->key.len, item_b->key.len);
    rc = ngx_strncmp(item_a->key.data, item_b->key.data, n);
    if (rc) {
        return rc;
    }
    if (item_a->key.len > item_b->key.len) {
        return 1;
    }
    if (item_a->key.len < item_b->key.len) {
        return -1;
    }
    return 0;
}


static void
ngx_http_var_limit_req_format_http_time(time_t sec, u_char *out_buf)
{
    ngx_tm_t         gmt;

    ngx_gmtime(sec, &gmt);

    (void) ngx_sprintf(out_buf, "%s, %02d %s %4d %02d:%02d:%02d GMT",
                       week[gmt.ngx_tm_wday], gmt.ngx_tm_mday,
                       months[gmt.ngx_tm_mon - 1], gmt.ngx_tm_year,
                       gmt.ngx_tm_hour, gmt.ngx_tm_min, gmt.ngx_tm_sec);
}


static char *
ngx_http_var_limit_req_top(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_shm_zone_t                   *shm_zone;
    ngx_http_var_limit_req_conf_t   *lrcf = conf;
    ngx_http_core_loc_conf_t         *clcf;
    ngx_uint_t                        i;
    ngx_str_t                         default_n_str;
    ngx_int_t                         default_n = 0;

    ngx_str_t  *value;

    value = cf->args->elts;

    shm_zone = ngx_shared_memory_add(cf, &value[1], 0,
                                     &ngx_http_var_limit_req_module);
    if (shm_zone == NULL) {
        return NGX_CONF_ERROR;
    }

    lrcf->top_shm_zone = shm_zone;

    for (i = 2; i < cf->args->nelts; i++) {

        if (ngx_strncmp(value[i].data, "default_n=", 10) == 0) {

            default_n_str.data = value[i].data + 10;
            default_n_str.len = value[i].len - 10;

            default_n = ngx_atoi(default_n_str.data, default_n_str.len);
            if (default_n == NGX_ERROR || default_n <= 0) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "invalid default_n \"%V\"", &default_n_str);
                return NGX_CONF_ERROR;
            }

            continue;
        }

        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid parameter \"%V\"", &value[i]);
        return NGX_CONF_ERROR;
    }

    if (default_n == 0) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "\"%V\" must have \"default_n\" parameter",
                           &cmd->name);
        return NGX_CONF_ERROR;
    }

    lrcf->default_n = (ngx_uint_t) default_n;

    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
    clcf->handler = ngx_http_var_limit_req_top_handler;

    return NGX_CONF_OK;
}


static ngx_int_t
ngx_http_var_limit_req_add_variables(ngx_conf_t *cf)
{
    ngx_http_variable_t  *var, *v;

    for (v = ngx_http_var_limit_req_vars; v->name.len; v++) {
        var = ngx_http_add_variable(cf, &v->name, v->flags);
        if (var == NULL) {
            return NGX_ERROR;
        }

        var->get_handler = v->get_handler;
        var->data = v->data;
    }

    return NGX_OK;
}


static ngx_int_t
ngx_http_var_limit_req_init(ngx_conf_t *cf)
{
    ngx_http_handler_pt        *h;
    ngx_http_core_main_conf_t  *cmcf;

    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);

    h = ngx_array_push(&cmcf->phases[NGX_HTTP_PREACCESS_PHASE].handlers);
    if (h == NULL) {
        return NGX_ERROR;
    }

    *h = ngx_http_var_limit_req_handler;

    return NGX_OK;
}
