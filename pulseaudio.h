#pragma once
#define _GNU_SOURCE
#include <math.h>
#include <string.h>

#include <lua.h>

typedef struct
{
    pa_threaded_mainloop* mainloop;
    pa_context* context;

    lua_State* L;

    char* default_sink;

    int success;
} pulseaudio_t;

typedef void (getter) (pulseaudio_t*);

typedef pa_operation* (mute_setter) (pa_context*, uint32_t, int,               pa_context_success_cb_t, void*);
typedef pa_operation* (vol_setter)  (pa_context*, uint32_t, const pa_cvolume*, pa_context_success_cb_t, void*);
