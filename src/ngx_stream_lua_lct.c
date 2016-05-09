
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

static time_t ngx_stream_lcache_timer_manager(void *data);

static void *ngx_stream_lua_create_srv_conf(ngx_conf_t *cf);
static char *ngx_stream_lua_merge_srv_conf(ngx_conf_t *cf, void *parent,
    void *child);

static time_t
ngx_stream_lcache_timer_manager(void *data)
{
    ngx_http_file_cache_t  *cache = data;
    ngx_stream_lua_lct_main_conf_t *lmlctcf = data->lmlctcf;
    ngx_stream_lua_main_conf_t lmcf = lmlctcf->lmcf;

    lua_State* L = lmcf->lua;

    int                      nargs, co_ref;
    u_char                  *p;
    lua_State               *vm;  /* the main thread */
    lua_State               *co;
    ngx_msec_t               delay;
    ngx_event_t             *ev = NULL;
    ngx_stream_session_t    *s;
    ngx_connection_t        *saved_c = NULL;
    ngx_stream_lua_ctx_t    *ctx;
#if 0
    ngx_stream_connection_t   *hc;
#endif

    ngx_stream_lua_timer_ctx_t      *tctx = NULL;
    ngx_stream_lua_main_conf_t      *lmcf;
#if 0
    ngx_stream_core_main_conf_t     *cmcf;
#endif

    nargs = lua_gettop(L);
    if (nargs < 2) {
        return luaL_error(L, "expecting at least 2 arguments but got %d",
                          nargs);
    }

    delay = (ngx_msec_t) (luaL_checknumber(L, 1) * 1000);

    luaL_argcheck(L, lua_isfunction(L, 2) && !lua_iscfunction(L, 2), 2,
                  "Lua function expected");

    s = ngx_stream_lua_get_session(L);
    if (s == NULL) {
        return luaL_error(L, "no session");
    }

    ngx_log_debug1(NGX_LOG_DEBUG_STREAM, s->connection->log, 0,
                   "stream lua creating new timer with delay %M", delay);

    ctx = ngx_stream_get_module_ctx(s, ngx_stream_lua_module);

    if (ngx_exiting && delay > 0) {
        lua_pushnil(L);
        lua_pushliteral(L, "process exiting");
        return 2;
    }

    lmcf = ngx_stream_get_module_main_conf(s, ngx_stream_lua_module);

    if (lmcf->pending_timers >= lmcf->max_pending_timers) {
        lua_pushnil(L);
        lua_pushliteral(L, "too many pending timers");
        return 2;
    }

    if (lmcf->watcher == NULL) {
        /* create the watcher fake connection */

        ngx_log_debug0(NGX_LOG_DEBUG_STREAM, ngx_cycle->log, 0,
                       "stream lua creating fake watcher connection");

        if (ngx_cycle->files) {
            saved_c = ngx_cycle->files[0];
        }

        lmcf->watcher = ngx_get_connection(0, ngx_cycle->log);

        if (ngx_cycle->files) {
            ngx_cycle->files[0] = saved_c;
        }

        if (lmcf->watcher == NULL) {
            return luaL_error(L, "no memory");
        }

        /* to work around the -1 check in ngx_worker_process_cycle: */
        lmcf->watcher->fd = (ngx_socket_t) -2;

        lmcf->watcher->idle = 1;
        lmcf->watcher->read->handler = ngx_stream_lua_abort_pending_timers;
        lmcf->watcher->data = lmcf;
    }

    vm = ngx_stream_lua_get_lua_vm(s, ctx);

    co = lua_newthread(vm);

    /* L stack: time func [args] thread */

#if 0
    /* TODO */
    ngx_stream_lua_probe_user_coroutine_create(s, L, co);
#endif

    lua_createtable(co, 0, 0);  /* the new globals table */

    /* co stack: global_tb */

    lua_createtable(co, 0, 1);  /* the metatable */
    ngx_stream_lua_get_globals_table(co);
    lua_setfield(co, -2, "__index");
    lua_setmetatable(co, -2);

    /* co stack: global_tb */

    ngx_stream_lua_set_globals_table(co);

    /* co stack: <empty> */

    dd("stack top: %d", lua_gettop(L));

    lua_xmove(vm, L, 1);    /* move coroutine from main thread to L */

    /* L stack: time func [args] thread */
    /* vm stack: empty */

    lua_pushvalue(L, 2);    /* copy entry function to top of L*/

    /* L stack: time func [args] thread func */

    lua_xmove(L, co, 1);    /* move entry function from L to co */

    /* L stack: time func [args] thread */
    /* co stack: func */

    ngx_stream_lua_get_globals_table(co);
    lua_setfenv(co, -2);

    /* co stack: func */

    lua_pushlightuserdata(L, &ngx_stream_lua_coroutines_key);
    lua_rawget(L, LUA_REGISTRYINDEX);

    /* L stack: time func [args] thread corountines */

    lua_pushvalue(L, -2);

    /* L stack: time func [args] thread coroutines thread */

    co_ref = luaL_ref(L, -2);
    lua_pop(L, 1);

    /* L stack: time func [args] thread */

    if (nargs > 2) {
        lua_pop(L, 1);  /* L stack: time func [args] */
        lua_xmove(L, co, nargs - 2);  /* L stack: time func */

        /* co stack: func [args] */
    }

    p = ngx_alloc(sizeof(ngx_event_t) + sizeof(ngx_stream_lua_timer_ctx_t),
                  s->connection->log);
    if (p == NULL) {
        goto nomem;
    }

    ev = (ngx_event_t *) p;

    ngx_memzero(ev, sizeof(ngx_event_t));

    p += sizeof(ngx_event_t);

    tctx = (ngx_stream_lua_timer_ctx_t *) p;

    tctx->premature = 0;
    tctx->co_ref = co_ref;
    tctx->co = co;
    tctx->main_conf = s->main_conf;
    tctx->srv_conf = s->srv_conf;
    tctx->lmcf = lmcf;

    tctx->pool = ngx_create_pool(128, ngx_cycle->log);
    if (tctx->pool == NULL) {
        goto nomem;
    }

    if (s->connection) {
        tctx->listening = s->connection->listening;

    } else {
        tctx->listening = NULL;
    }

    if (s->connection->addr_text.len) {
        tctx->client_addr_text.data = ngx_palloc(tctx->pool,
                                                 s->connection->addr_text.len);
        if (tctx->client_addr_text.data == NULL) {
            goto nomem;
        }

        ngx_memcpy(tctx->client_addr_text.data, s->connection->addr_text.data,
                   s->connection->addr_text.len);
        tctx->client_addr_text.len = s->connection->addr_text.len;

    } else {
        tctx->client_addr_text.len = 0;
        tctx->client_addr_text.data = NULL;
    }

    if (ctx && ctx->vm_state) {
        tctx->vm_state = ctx->vm_state;
        tctx->vm_state->count++;

    } else {
        tctx->vm_state = NULL;
    }

    ev->handler = ngx_stream_lua_timer_handler;
    ev->data = tctx;
    ev->log = ngx_cycle->log;

    lmcf->pending_timers++;

    ngx_add_timer(ev, delay);

    lua_pushinteger(L, 1);
    return 1;

nomem:

    if (tctx && tctx->pool) {
        ngx_destroy_pool(tctx->pool);
    }

    if (ev) {
        ngx_free(ev);
    }

    lua_pushlightuserdata(L, &ngx_stream_lua_coroutines_key);
    lua_rawget(L, LUA_REGISTRYINDEX);
    luaL_unref(L, -1, co_ref);

    return luaL_error(L, "no memory");
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
