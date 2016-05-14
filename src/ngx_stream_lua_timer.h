
/*
 * Copyright (C) Xiaozhe Wang (chaoslawful)
 * Copyright (C) Yichun Zhang (agentzh)
 */


#ifndef _NGX_STREAM_LUA_TIMER_H_INCLUDED_
#define _NGX_STREAM_LUA_TIMER_H_INCLUDED_


#include "ngx_stream_lua_common.h"

char * ngx_stream_lua_mgr_timer_by_lua(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);

void ngx_stream_lua_inject_timer_api(lua_State *L);

// char * ngx_stream_lua_mgr_timer(void* data);

#endif /* _NGX_STREAM_LUA_TIMER_H_INCLUDED_ */
