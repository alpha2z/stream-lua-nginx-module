
/*
 * Copyright (C) chandler alpha (alpha)
 */


#ifndef DDEBUG
#define DDEBUG 0
#endif
#include "ddebug.h"

#include "ngx_stream_lua_common.h"
#include "ngx_stream_lua_directive.h"
#include "ngx_stream_lua_contentby.h"
#include "ngx_stream_lua_semaphore.h"
#include "ngx_stream_lua_initby.h"
#include "ngx_stream_lua_initworkerby.h"
#include "ngx_stream_lua_util.h"
#include "ngx_stream_lua_lcachetimer.h"

typedef struct {
    ngx_rbtree_t                     rbtree;
    ngx_rbtree_node_t                sentinel;
    ngx_queue_t                      queue;
    ngx_atomic_t                     cold;
    ngx_atomic_t                     loading;
    off_t                            size;
} ngx_http_file_cache_sh_t;


struct ngx_stream_lcache_timer_t {
    ngx_http_file_cache_sh_t        *sh;
    ngx_slab_pool_t                 *shpool;

    ngx_path_t                      *path;
    ngx_path_t                      *temp_path;

    off_t                            max_size;
    size_t                           bsize;

    time_t                           inactive;

    ngx_uint_t                       files;
    ngx_uint_t                       loader_files;
    ngx_msec_t                       last;
    ngx_msec_t                       loader_sleep;
    ngx_msec_t                       loader_threshold;

    ngx_str_t                       src_name;
    ngx_array_t                     caches;

    ngx_shm_zone_t                  *shm_zone;
};

time_t
ngx_stream_lcache_timer_manager(void *data)
{
    ngx_stream_lua_lct_cache_t  *cache = data;
    ngx_stream_lua_main_conf_t lmcf = cache->lmcf;


    ngx_stream_lua_srv_conf_t       *lscf;
    ngx_stream_lua_ctx_t            *ctx;


    lua_State* L = lmcf->lua;

    int                      n;
    lua_State               *L;
    ngx_int_t                rc;
    ngx_connection_t        *c = NULL;
    ngx_stream_session_t    *s = NULL;
    ngx_stream_lua_ctx_t    *ctx;

    ngx_pool_cleanup_t            *pcln;
    ngx_stream_lua_cleanup_t      *cln;

    ngx_stream_lua_timer_ctx_t         tctx;
    ngx_stream_lua_main_conf_t        *lmcf;
    ngx_stream_core_srv_conf_t        *cscf;

    // ngx_memcpy(&tctx, ev->data, sizeof(ngx_stream_lua_timer_ctx_t));
    // ngx_free(ev);
    // ev = NULL;

    // lmcf = tctx.lmcf;

    ngx_log_debug2(NGX_LOG_DEBUG_STREAM, ngx_cycle->log, 0,
                   "stream lua ngx.timer expired (running: %i, max: %i)",
                   lmcf->running_timers, lmcf->max_running_timers);

    lmcf->pending_timers--;

    if (lmcf->running_timers >= lmcf->max_running_timers) {
        ngx_log_error(NGX_LOG_ALERT, ngx_cycle->log, 0,
                      "stream lua: %i lua_max_running_timers are not enough",
                      lmcf->max_running_timers);
        goto failed;
    }

    c = ngx_stream_lua_create_fake_connection(tctx.pool);
    if (c == NULL) {
        goto failed;
    }

    c->log->handler = ngx_stream_lua_log_timer_error;
    c->log->data = c;

    c->listening = tctx.listening;
    c->addr_text = tctx.client_addr_text;

    s = ngx_stream_lua_create_fake_session(c);
    if (s == NULL) {
        goto failed;
    }

    s->main_conf = tctx.main_conf;
    s->srv_conf = tctx.srv_conf;

    cscf = ngx_stream_get_module_srv_conf(s, ngx_stream_core_module);

#if defined(nginx_version) && nginx_version >= 1003014

#   if nginx_version >= 1009000

    ngx_set_connection_log(s->connection, cscf->error_log);

#   else

    ngx_stream_set_connection_log(s->connection, cscf->error_log);

#   endif

#else

    c->log->file = cscf->error_log->file;

    if (!(c->log->log_level & NGX_LOG_DEBUG_CONNECTION)) {
        c->log->log_level = cscf->error_log->log_level;
    }

#endif

    dd("lmcf: %p", lmcf);

    ctx = ngx_stream_lua_create_ctx(s);
    if (ctx == NULL) {
        goto failed;
    }

    if (tctx.vm_state) {
        ctx->vm_state = tctx.vm_state;

        pcln = ngx_pool_cleanup_add(s->connection->pool, 0);
        if (pcln == NULL) {
            goto failed;
        }

        pcln->handler = ngx_stream_lua_cleanup_vm;
        pcln->data = tctx.vm_state;
    }

    ctx->cur_co_ctx = &ctx->entry_co_ctx;

    L = ngx_stream_lua_get_lua_vm(s, ctx);

    cln = ngx_stream_lua_cleanup_add(s, 0);
    if (cln == NULL) {
        goto failed;
    }

    cln->handler = ngx_stream_lua_session_cleanup_handler;
    cln->data = ctx;

    ctx->entered_content_phase = 1;
    ctx->context = NGX_STREAM_LUA_CONTEXT_TIMER;

    ctx->read_event_handler = ngx_stream_lua_block_reading;

    ctx->cur_co_ctx->co_ref = tctx.co_ref;
    ctx->cur_co_ctx->co = tctx.co;
    ctx->cur_co_ctx->co_status = NGX_STREAM_LUA_CO_RUNNING;

    dd("s connection: %p, log %p", s->connection, s->connection->log);

    /*  save the session in coroutine globals table */
    ngx_stream_lua_set_session(tctx.co, s);

    dd("running_timers++");
    lmcf->running_timers++;

    lua_pushboolean(tctx.co, tctx.premature);

    n = lua_gettop(tctx.co);
    if (n > 2) {
        lua_insert(tctx.co, 2);
    }

#ifdef NGX_LUA_USE_ASSERT
    ctx->cur_co_ctx->co_top = 1;
#endif

    rc = ngx_stream_lua_run_thread(L, s, ctx, n - 1);

    dd("timer lua run thread: %d", (int) rc);

    if (rc == NGX_ERROR || rc >= NGX_OK) {
        /* do nothing */

    } else if (rc == NGX_AGAIN || rc == NGX_DONE) {
        rc = ngx_stream_lua_run_posted_threads(s->connection, L, s, ctx);

    } else {
        rc = NGX_OK;
    }

    ngx_stream_lua_finalize_session(s, rc);
    return next_secs;

failed:

    if (tctx.co_ref && tctx.co) {
        lua_pushlightuserdata(tctx.co, &ngx_stream_lua_coroutines_key);
        lua_rawget(tctx.co, LUA_REGISTRYINDEX);
        luaL_unref(tctx.co, -1, tctx.co_ref);
        lua_settop(tctx.co, 0);
    }

    if (tctx.vm_state) {
        ngx_stream_lua_cleanup_vm(tctx.vm_state);
    }

    if (c) {
        ngx_stream_lua_close_fake_connection(c);

    } else if (tctx.pool) {
        ngx_destroy_pool(tctx.pool);
    }

    return next_secs;
}



char *
ngx_stream_lua_lcache_timer_by_lua(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf)
{
    u_char                      *name;
    ngx_str_t                   *value;
    ngx_stream_lua_main_conf_t  *lmcf = conf;
    ngx_str_t                   s;
    ngx_stream_lua_lct_cache_t *cache;
    cache = ngx_pcalloc(cf->pool, sizeof(ngx_stream_lua_lct_cache_t));
    if (cache == NULL) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "malloc ngx_stream_lua_lct_cache_t failed");
        return NGX_CONF_ERROR;
    }

    dd("enter");

    cache->path->manager = cmd->post;
    cache->path->data = cache;
    cache->path->conf_file = cf->conf_file->file.name.data;
    cache->path->line = cf->conf_file->line;
    cache->lmcf = lmcf;

    value = cf->args->elts;

    if (value[1].len == 0) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid lua file name value \"%V\"", &value[1]);
        return NGX_CONF_ERROR;
    }

    cache->src_name.data = value[1].data;
    cache->src_name.len = value[1].len;

    if (value[2].len == 0) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid timer_secs value \"%V\"", &value[2]);
        return NGX_CONF_ERROR;
    }

    s.len = value[2].len;
    s.data = value[2].data;

    cache->timer_secs = ngx_parse_time(&s, 0);
    if (cache->timer_secs == (ngx_msec_t) NGX_ERROR) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                   "invalid timer_secs value \"%V\"", &value[i]);
        return NGX_CONF_ERROR;
    }

    if (ngx_add_path(cf, &cache->path) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    cache->shm_zone = ngx_shared_memory_add(cf, &name, size, cmd->post);
    if (cache->shm_zone == NULL) {
        return NGX_CONF_ERROR;
    }

    if (cache->shm_zone->data) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "duplicate zone \"%V\"", &name);
        return NGX_CONF_ERROR;
    }


    cache->shm_zone->init = ngx_http_file_cache_init;
    cache->shm_zone->data = cache;

    caches = (ngx_array_t *) (lmcf + cmd->offset);

    ce = ngx_array_push(caches);
    if (ce == NULL) {
        return NGX_CONF_ERROR;
    }

    *ce = cache;

    return NGX_CONF_OK;
}
