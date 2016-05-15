static void
ngx_stream_init_session(ngx_connection_t *c)
{
    ngx_stream_session_t        *s;
    ngx_stream_core_srv_conf_t  *cscf;

    s = c->data;
    c->log->action = "handling client connection";

    cscf = ngx_stream_get_module_srv_conf(s, ngx_stream_core_module);

    s->ctx = ngx_pcalloc(c->pool, sizeof(void *) * ngx_stream_max_module);
    if (s->ctx == NULL) {
        ngx_stream_close_connection(c);
        return;
    }

    cscf->handler(s);
}


// handler
void
ngx_stream_lua_content_handler(ngx_stream_session_t *s)
{
    ngx_stream_lua_srv_conf_t       *lscf;
    ngx_stream_lua_ctx_t            *ctx;

    ngx_log_debug1(NGX_LOG_DEBUG_STREAM, s->connection->log, 0,
                   "stream lua content handler fd:%d",
                   (int) s->connection->fd);

    lscf = ngx_stream_get_module_srv_conf(s, ngx_stream_lua_module);

    if (lscf->content_handler == NULL) {
        dd("no content handler found");
        ngx_stream_lua_finalize_session(s, NGX_DECLINED);
        return;
    }

    ctx = ngx_stream_get_module_ctx(s, ngx_stream_lua_module);

    dd("ctx = %p", ctx);

    if (ctx == NULL) {
        ctx = ngx_stream_lua_create_ctx(s);
        if (ctx == NULL) {
            ngx_stream_lua_finalize_session(s, NGX_ERROR);
            return;
        }
    }

    dd("entered? %d", (int) ctx->entered_content_phase);

    if (ctx->entered_content_phase) {
        dd("calling wev handler");
        ngx_stream_lua_finalize_session(s, ctx->resume_handler(s, ctx));
        return;
    }

    dd("setting entered");

    ctx->entered_content_phase = 1;

    dd("calling content handler");
    ngx_stream_lua_finalize_session(s, lscf->content_handler(s, ctx));
}
 
// content_handler
ngx_int_t
ngx_stream_lua_content_handler_file(ngx_stream_session_t *s)
{
    lua_State                       *L;
    ngx_int_t                        rc;
    u_char                          *script_path;
    ngx_stream_lua_srv_conf_t       *lscf;

    lscf = ngx_stream_get_module_srv_conf(s, ngx_stream_lua_module);

    script_path = ngx_stream_lua_rebase_path(s->connection->pool,
                                             lscf->content_src.data,
                                             lscf->content_src.len);

    if (script_path == NULL) {
        return NGX_ERROR;
    }

    L = ngx_stream_lua_get_lua_vm(s, NULL);

    /*  load Lua script file (w/ cache)        sp = 1 */
    rc = ngx_stream_lua_cache_loadfile(s->connection->log, L, script_path,
                                       lscf->content_src_key);
    if (rc != NGX_OK) {
        return NGX_ERROR;
    }

    /*  make sure we have a valid code chunk */
    ngx_stream_lua_assert(lua_isfunction(L, -1));

    return ngx_stream_lua_content_by_chunk(L, s);
}


