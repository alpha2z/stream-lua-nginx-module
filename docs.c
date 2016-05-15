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


