// thread

#include "lua_thread.h"

//

LUAMOD_API int luaopen_thread( lua_State *L ) {
    lua_newmt(L, LUA_MT_THREAD, __thread_index, lua_thread_gc);

    luaL_newlib(L, __index);
    return 1;
}

// arg#1 - string - file to execute
// arg#2 - table - meta args
// arg#3 - table - meta keys to copy
static int lua_thread_start( lua_State *L ) {
    luaL_checkstring(L, 1);

    lua_ud_thread *thread = (lua_ud_thread *)lua_newuserdata(L, sizeof(lua_ud_thread));

    if ( !thread ) {
        lua_fail(L, "lua_ud_thread alloc failed", 0);
    }

    thread->L = luaL_newstate();

    if ( !thread->L ) {
        lua_fail(L, "luaL_newstate alloc failed", 0);
    }

    thread->detached = 0;
    thread->id = inc_id();

    // meta id
    lua_pushnumber(thread->L, thread->id);
    lua_setfield(thread->L, LUA_REGISTRYINDEX, LUA_THREAD_ID_METAFIELD);
    //

    lua_atpanic(thread->L, lua_thread_atpanic);

    luaL_openlibs(thread->L);

    // meta args
    lua_newtable(thread->L);

    if ( lua_istable(L, 2) ) {
        lua_pushnil(L);

        while ( lua_next(L, 2) != 0 ) {
            lua_thread_xcopy(L, thread->L);
            lua_pop(L, 1);
        }
    }

    lua_setfield(thread->L, LUA_REGISTRYINDEX, LUA_THREAD_ARGS_METAFIELD);
    //

    // meta keys to copy
    int type;
    if ( lua_istable(L, 3) ) {
        lua_pushnil(L);

        while ( lua_next(L, 3) != 0 ) {
            type = lua_getfield(L, LUA_REGISTRYINDEX, lua_tostring(L, -1));

            if ( type==LUA_TLIGHTUSERDATA ) {
                lua_pushlightuserdata(thread->L, lua_touserdata(L, -1));
                lua_setfield(thread->L, LUA_REGISTRYINDEX, lua_tostring(L, -2));
            }

            lua_pop(L, 2); // +1 for lua_next, +1 for lua_getfield(registry)
        }
    }
    //

    // push file path at the top of the stack
    lua_pushstring(thread->L, lua_tostring(L, 1));
    //

    luaL_setmetatable(L, LUA_MT_THREAD);

    //

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

    int r = pthread_create(&thread->thread, &attr, lua_thread_create_worker, thread);

    pthread_attr_destroy(&attr);

    if ( r != 0 ) {
        thread->detached = 1;
        lua_fail(L, "thread creation failed", r);
    }

    lua_pushnumber(L, thread->id);

    return 2;
}

static int lua_thread_stop( lua_State *L ) {
    lua_ud_thread *thread = luaL_checkudata(L, 1, LUA_MT_THREAD);

    lua_thread_detach(thread);
    pthread_cancel(thread->thread);

    return 0;
}

static int lua_thread_join( lua_State *L ) {
    lua_ud_thread *thread = luaL_checkudata(L, 1, LUA_MT_THREAD);

    int r = pthread_join(thread->thread, NULL);

    if ( r != 0 ) {
        lua_fail(L, "thread join failed", r);
    }

    lua_pushboolean(L, 1);
    return 1;
}

static int lua_thread_get_id( lua_State *L ) {
    lua_getfield(L, LUA_REGISTRYINDEX, LUA_THREAD_ID_METAFIELD);
    return 1;
}

static int lua_thread_get_args( lua_State *L ) {
    lua_getfield(L, LUA_REGISTRYINDEX, LUA_THREAD_ARGS_METAFIELD);
    return 1;
}

static int lua_thread_set_cancel_point( lua_State *L ) {
    pthread_testcancel();
    return 0;
}

static int lua_thread_gc( lua_State *L ) {
    lua_ud_thread *thread = luaL_checkudata(L, 1, LUA_MT_THREAD);

    if ( thread->L ) {
        lua_close(thread->L);
        thread->L = NULL;
    }

    lua_thread_detach(thread);

    return 0;
}

//

static uint64_t inc_id( void ) {
    static volatile uint64_t id = 0;
    return __sync_add_and_fetch(&id, 1);
}

static void *lua_thread_create_worker( void *arg ) {
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, NULL);

    lua_ud_thread *thread = (lua_ud_thread *)arg;
    lua_State *L = thread->L;

    lua_thread_set_cancel_point(L);

    int r;

    r = luaL_loadfilex(L, lua_tostring(L, 1), "bt");

    if ( r != LUA_OK ) {
        lua_thread_atpanic(L);
    } else {
        r = lua_custom_pcall(L, 0, 0);

        if ( r != LUA_OK ) {
            lua_thread_atpanic(L);
        }
    }

    lua_thread_detach(thread);

    pthread_exit(NULL);
}

static int lua_thread_atpanic( lua_State *L ) {
    lua_getfield(L, LUA_REGISTRYINDEX, LUA_THREAD_ID_METAFIELD);
    fprintf(stderr, "lua thread #%zu: PANIC: %s\n", (size_t)lua_tonumber(L, -1), lua_tostring(L, -2));
    return 0;
}

// fromL: arg#-1 - value, arg#-2 - key
// toL: arg#-1 - table to copy to
static void lua_thread_xcopy( lua_State *fromL, lua_State *toL ) {
    int type; // LUA_TNONE, LUA_TNIL, LUA_TNUMBER, LUA_TBOOLEAN, LUA_TSTRING, LUA_TTABLE, LUA_TFUNCTION, LUA_TUSERDATA, LUA_TTHREAD, LUA_TLIGHTUSERDATA

    size_t len;
    const char *str;

    // copy key

    type = lua_type(fromL, -2);

    if ( type==LUA_TNUMBER ) {
        lua_pushnumber(toL, lua_tonumber(fromL, -2));
    } else if ( type==LUA_TBOOLEAN ) {
        lua_pushboolean(toL, -2);
    } else if ( type==LUA_TSTRING ) {
        str = lua_tolstring(fromL, -2, &len);
        lua_pushlstring(toL, str, len);
    } else if ( type==LUA_TTABLE ) {
        lua_newtable(toL);
    } else {
        return;
    }

    // copy value

    type = lua_type(fromL, -1);

    if ( type==LUA_TNUMBER ) {
        lua_pushnumber(toL, lua_tonumber(fromL, -1));
    } else if ( type==LUA_TBOOLEAN ) {
        lua_pushboolean(toL, -1);
    } else if ( type==LUA_TSTRING ) {
        str = lua_tolstring(fromL, -1, &len);
        lua_pushlstring(toL, str, len);
    } else if ( type==LUA_TTABLE ) {
        lua_newtable(toL);
    } else {
        lua_pop(toL, 1);
        return;
    }

    lua_settable(toL, -3);
}

static int lua_custom_traceback( lua_State *L ) {
    const char *msg = lua_tostring(L, 1);
    if ( msg ) {
        luaL_traceback(L, L, msg, 1);
    }
    return 1;
}

static int lua_custom_pcall( lua_State *L, int narg, int nres ) {
    int base = lua_gettop(L) - narg; // traceback index
    lua_pushcfunction(L, lua_custom_traceback);
    lua_insert(L, base);
    int r = lua_pcall(L, narg, nres, base);
    lua_remove(L, base);
    return r;
}
