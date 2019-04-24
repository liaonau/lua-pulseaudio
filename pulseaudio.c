#define _GNU_SOURCE
#define UNUSED __attribute__((unused))

#include <stdbool.h>

#include <lua.h>
#include <lauxlib.h>
#include <pulse/pulseaudio.h>

#include "pulseaudio.h"

bool pulse_init(pulseaudio_t* pulse);
void pulse_deinit(pulseaudio_t* pulse);
void get_pa_sink_inputs(pulseaudio_t* pulse);
void get_pa_sinks(pulseaudio_t* pulse);
bool set_pa_volume(pulseaudio_t* pulse, mute_setter* Fmute, vol_setter* Fvol);
bool set_pa_volume(pulseaudio_t* pulse, mute_setter* Fmute, vol_setter* Fvol);
static int luaP_move_sink_input(lua_State* L);
static int luaP_set_default_sink(lua_State* L);

static int luaP_get_volume(lua_State* L, getter* Fget)
{
    pulseaudio_t pulse;
    pulse.L = L;

    if (!pulse_init(&pulse))
    {
        pulse_deinit(&pulse);
        return luaL_error(L, "%s\n", "couldn't initialize pulseaudio");
    }

    lua_newtable(L);
    Fget(&pulse);

    pulse_deinit(&pulse);
    return 1;
}

static int luaP_get_sink_inputs(lua_State* L)
{
    return luaP_get_volume(L, get_pa_sink_inputs);
}

static int luaP_get_sinks(lua_State* L)
{
    return luaP_get_volume(L, get_pa_sinks);
}

static int luaP_set_volume(lua_State* L, mute_setter* Fmute, vol_setter* Fvol)
{
    pulseaudio_t pulse;
    pulse.L = L;

    if (!lua_isnumber(L, -2))
        return luaL_error(L, "%s\n", "first argument should be number");
    if (!lua_istable(L, -1))
        return luaL_error(L, "%s\n", "second argument should be table");

    if (!pulse_init(&pulse))
    {
        pulse_deinit(&pulse);
        return luaL_error(L, "%s\n", "couldn't initialize pulseaudio");
    }

    bool success = set_pa_volume(&pulse, Fmute, Fvol);

    pulse_deinit(&pulse);
    lua_pushboolean(L, success);
    return 1;
}

static int luaP_set_sink_volume(lua_State* L)
{
    return luaP_set_volume(L, &pa_context_set_sink_mute_by_index, &pa_context_set_sink_volume_by_index);
}

static int luaP_set_sink_input_volume(lua_State* L)
{
    return luaP_set_volume(L, &pa_context_set_sink_input_mute, &pa_context_set_sink_input_volume);
}

static const struct luaL_Reg pulseaudio_reg[] =
{
    {"set_sink_volume",       luaP_set_sink_volume      },
    {"set_sink_input_volume", luaP_set_sink_input_volume},
    {"get_sinks",             luaP_get_sinks            },
    {"get_sink_inputs",       luaP_get_sink_inputs      },
    {"move_sink_input",       luaP_move_sink_input      },
    {"set_default_sink",      luaP_set_default_sink     },
    {NULL,                    NULL                      }
};

int luaopen_pulseaudio(lua_State* L)
{
    luaL_newlib(L, pulseaudio_reg);
    return 1;
}



inline static void async_wait(pulseaudio_t* pulse, pa_operation* op)
{
    while (pa_operation_get_state(op) == PA_OPERATION_RUNNING)
        pa_threaded_mainloop_wait(pulse->mainloop);
    pa_operation_unref(op);
    pa_threaded_mainloop_unlock(pulse->mainloop);
}

static void state_cb(pa_context *context, void *userdata)
{
    pulseaudio_t* pulse = (pulseaudio_t*) userdata;
    switch (pa_context_get_state(context))
    {
    case PA_CONTEXT_READY:
    case PA_CONTEXT_FAILED:
    case PA_CONTEXT_TERMINATED:
        pa_threaded_mainloop_signal(pulse->mainloop, 0);
        break;

    case PA_CONTEXT_UNCONNECTED:
    case PA_CONTEXT_CONNECTING:
    case PA_CONTEXT_AUTHORIZING:
    case PA_CONTEXT_SETTING_NAME:
        break;
    }
}

static void server_info_cb(UNUSED pa_context *c, const pa_server_info *i, void *userdata)
{
    pulseaudio_t* pulse = (pulseaudio_t*) userdata;
    pulse->default_sink = strdup(i->default_sink_name);

    pa_threaded_mainloop_signal(pulse->mainloop, 0);
}

static void success_cb(pa_context UNUSED *c, int success, void *userdata)
{
    pulseaudio_t* pulse = (pulseaudio_t*) userdata;
    pulse->success = success;
    pa_threaded_mainloop_signal(pulse->mainloop, 0);
}

static void get_pa_sink_inputs_cb(UNUSED pa_context* c, const pa_sink_input_info* i, int eol, void* userdata)
{
    pulseaudio_t* pulse = (pulseaudio_t*) userdata;
    if (!eol)
    {
        double dB = pa_sw_volume_to_dB(pa_cvolume_avg(&i->volume));
        int v = (int) round(100*exp10(dB/60));
        const char* name = i->name;
        if (pa_proplist_contains(i->proplist, "application.name"))
            name = pa_proplist_gets(i->proplist, "application.name");

        lua_State* L = pulse->L;

        lua_pushnumber(L, i->index);

        lua_newtable(L);
        lua_pushliteral(L, "index");
        lua_pushnumber(L, i->index);
        lua_rawset(L, -3);
        lua_pushliteral(L, "volume");
        lua_pushnumber(L, v);
        lua_rawset(L, -3);
        lua_pushliteral(L, "sink");
        lua_pushnumber(L, i->sink);
        lua_rawset(L, -3);
        lua_pushliteral(L, "mute");
        lua_pushboolean(L, i->mute);
        lua_rawset(L, -3);
        lua_pushliteral(L, "name");
        lua_pushstring(L, name);
        lua_rawset(L, -3);
        if (pa_proplist_contains(i->proplist, "application.process.id"))
        {
            const char *pid;
            pid = pa_proplist_gets(i->proplist, "application.process.id");
            lua_pushliteral(L, "pid");
            lua_pushnumber(L, atoi(pid));
            lua_rawset(L, -3);
        }
        if (pa_proplist_contains(i->proplist, "application.process.binary"))
        {
            const char *binary;
            binary = pa_proplist_gets(i->proplist, "application.process.binary");
            lua_pushliteral(L, "binary");
            lua_pushstring(L, binary);
            lua_rawset(L, -3);
        }

        lua_rawset(L, -3);
    }
    pa_threaded_mainloop_signal(pulse->mainloop, 0);
}

static void get_pa_sink_cb(UNUSED pa_context* c, const pa_sink_info* i, int eol, void* userdata)
{
    pulseaudio_t* pulse = (pulseaudio_t*) userdata;
    if (!eol)
    {
        double dB = pa_sw_volume_to_dB(pa_cvolume_avg(&i->volume));
        int v = (int) round(100*exp10(dB/60));
        const char* name = i->name;

        lua_State* L = pulse->L;

        lua_pushnumber(L, i->index);

        lua_newtable(L);
        lua_pushliteral(L, "index");
        lua_pushnumber(L, i->index);
        lua_rawset(L, -3);
        lua_pushliteral(L, "volume");
        lua_pushnumber(L, v);
        lua_rawset(L, -3);
        lua_pushliteral(L, "mute");
        lua_pushboolean(L, i->mute);
        lua_rawset(L, -3);
        lua_pushliteral(L, "name");
        lua_pushstring(L, name);
        lua_rawset(L, -3);
        lua_pushliteral(L, "default");
        lua_pushboolean(L, strcmp(name, pulse->default_sink) == 0);
        lua_rawset(L, -3);

        lua_rawset(L, -3);

        lua_pushstring(L, name);

        lua_newtable(L);
        lua_pushliteral(L, "index");
        lua_pushnumber(L, i->index);
        lua_rawset(L, -3);
        lua_pushliteral(L, "volume");
        lua_pushnumber(L, v);
        lua_rawset(L, -3);
        lua_pushliteral(L, "mute");
        lua_pushboolean(L, i->mute);
        lua_rawset(L, -3);
        lua_pushliteral(L, "name");
        lua_pushstring(L, name);
        lua_rawset(L, -3);
        lua_pushliteral(L, "default");
        lua_pushboolean(L, strcmp(name, pulse->default_sink) == 0);
        lua_rawset(L, -3);

        lua_rawset(L, -3);
    }
    pa_threaded_mainloop_signal(pulse->mainloop, 0);
}


bool pulse_init(pulseaudio_t* pulse)
{
    pulse->mainloop = pa_threaded_mainloop_new();
    pulse->context = pa_context_new(pa_threaded_mainloop_get_api(pulse->mainloop), "lua_pulseaudio");
    pulse->default_sink = NULL;

    pa_context_set_state_callback(pulse->context, state_cb, pulse);

    pa_context_connect(pulse->context, NULL, PA_CONTEXT_NOFLAGS, NULL);

    pa_threaded_mainloop_lock(pulse->mainloop);
    pa_threaded_mainloop_start(pulse->mainloop);

    pa_threaded_mainloop_wait(pulse->mainloop);

    if (pa_context_get_state(pulse->context) != PA_CONTEXT_READY)
    {
        return false;
    }
    pa_threaded_mainloop_unlock(pulse->mainloop);
    return true;
}

void pulse_deinit(pulseaudio_t* pulse)
{
    pa_context_unref(pulse->context);
    pulse->context = NULL;

    pa_threaded_mainloop_stop(pulse->mainloop);
    pa_threaded_mainloop_free(pulse->mainloop);
    pulse->mainloop = NULL;
}



bool set_pa_volume(pulseaudio_t* pulse, mute_setter* Fmute, vol_setter* Fvol)
{
    lua_State* L = pulse->L;
    uint32_t index = lua_tonumber(L, -2);
    int success1 = true;
    int success2 = true;

    lua_pushliteral(L, "mute");
    lua_rawget(L, -2);
    if (lua_isboolean(L, -1))
    {
        pa_threaded_mainloop_lock(pulse->mainloop);
        pa_operation *op = Fmute(pulse->context, index, lua_toboolean(L, -1), success_cb, pulse);
        async_wait(pulse, op);
        success1 = pulse->success;
    }
    lua_pop(L, 1);

    lua_pushliteral(L, "volume");
    lua_rawget(L, -2);
    if (lua_isnumber(L, -1))
    {
        pa_cvolume cv;
        double v = lua_tonumber(L, -1);
        if (v >= 100) v = 100;
        if (v < 0)    v = 0;
        double dB = 60 * log10(v / 100);
        pa_volume_t pvt = pa_sw_volume_from_dB(dB);
        pa_cvolume_set(&cv, 1, pvt);
        pa_threaded_mainloop_lock(pulse->mainloop);
        pa_operation *op = Fvol(pulse->context, index, &cv, success_cb, pulse);
        async_wait(pulse, op);
        success2 = pulse->success;
    }
    lua_pop(L, 1);

    return success1 && success2;
}

void get_pa_sinks(pulseaudio_t* pulse)
{
    pa_threaded_mainloop_lock(pulse->mainloop);
    pa_operation *op = pa_context_get_server_info(pulse->context, server_info_cb, pulse);
    async_wait(pulse, op);

    pa_threaded_mainloop_lock(pulse->mainloop);
    op = pa_context_get_sink_info_list(pulse->context, get_pa_sink_cb, (void*) pulse);
    async_wait(pulse, op);
}

void get_pa_sink_inputs(pulseaudio_t* pulse)
{
    pa_threaded_mainloop_lock(pulse->mainloop);
    pa_operation* op = pa_context_get_sink_input_info_list(pulse->context, get_pa_sink_inputs_cb, (void*) pulse);
    async_wait(pulse, op);
}

static int luaP_move_sink_input(lua_State* L)
{
    int success = true;
    pulseaudio_t pulse;

    if (!lua_isnumber(L, -2))
        return luaL_error(L, "%s\n", "first argument should be number");
    if (!lua_isnumber(L, -1))
        return luaL_error(L, "%s\n", "second argument should be number");

    int sink_index  = lua_tonumber(L, -1);
    int input_index = lua_tonumber(L, -2);
    lua_pop(L, 2);

    if (!pulse_init(&pulse))
    {
        pulse_deinit(&pulse);
        return luaL_error(L, "%s\n", "couldn't initialize pulseaudio");
    }

    pa_threaded_mainloop_lock(pulse.mainloop);
    pa_operation* op = pa_context_move_sink_input_by_index(pulse.context, input_index, sink_index, success_cb, (void*) &pulse);
    async_wait(&pulse, op);
    success = pulse.success;

    pulse_deinit(&pulse);
    lua_pushboolean(L, success);
    return 1;
}

static int luaP_set_default_sink(lua_State* L)
{
    int success = true;
    pulseaudio_t pulse;

    if (!lua_isstring(L, -1))
        return luaL_error(L, "%s\n", "argument should be a string");

    const char* name = lua_tostring(L, -1);

    if (!pulse_init(&pulse))
    {
        pulse_deinit(&pulse);
        return luaL_error(L, "%s\n", "couldn't initialize pulseaudio");
    }

    pa_threaded_mainloop_lock(pulse.mainloop);
    pa_operation* op = pa_context_set_default_sink(pulse.context, name, success_cb, (void*) &pulse);
    async_wait(&pulse, op);
    success = pulse.success;

    pulse_deinit(&pulse);
    lua_pop(L, 1);
    lua_pushboolean(L, success);
    return 1;
}
