#include <ngx_lua_ipc.h>
#include <ipc.h>
#include <shmem.h>
#include <lauxlib.h>
#include "ngx_http_lua_api.h"

#include "ngx_lua_ipc_scripts.h"
#include "ngx_lua_ipc.h"

#include <assert.h>

#define DEBUG_LEVEL NGX_LOG_DEBUG
//#define DEBUG_LEVEL NGX_LOG_WARN

#define LOAD_SCRIPTS_AS_NAMED_CHUNKS

#define DBG(fmt, args...) ngx_log_error(DEBUG_LEVEL, ngx_cycle->log, 0, "IPC:" fmt, ##args)
#define ERR(fmt, args...) ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "IPC:" fmt, ##args)

#ifndef container_of
#define container_of(ptr, type, member) ({                      \
        const typeof( ((type *)0)->member ) *__mptr = (ptr);    \
        (type *)( (char *)__mptr - offsetof(type,member) );})
#endif


#ifdef LOAD_SCRIPTS_AS_NAMED_CHUNKS
#define ngx_lua_ipc_loadscript(lua_state, name)                 \
          luaL_loadbuffer(lua_state, ngx_ipc_lua_scripts.name, strlen(ngx_ipc_lua_scripts.name), #name); \
          lua_call(lua_state, 0, LUA_MULTRET)
#else
#define ngx_lua_ipc_loadscript(lua_state, name)                 \
          luaL_dostring(lua_state, ngx_ipc_lua_scripts.name)
#endif
static shmem_t *shm = NULL;
static shm_data_t *shdata = NULL;

static ipc_t ipc_data;
static ipc_t *ipc = NULL;

// lua for the ipc alert handlers will be run from this timer's context
static ngx_event_t *hacktimer = NULL;

static ngx_int_t max_workers;

//received but yet-unprocessed alerts are quered here
static struct {
    ipc_alert_waiting_t *head;
    ipc_alert_waiting_t *tail;
} received_alerts = {NULL, NULL};

typedef struct {
    ngx_event_handler_pt original_handler;
    void *original_privdata;
} hacktimer_data_t;

static hacktimer_data_t hacktimer_data;
static int running_hacked_timer_handler = 0;

//wrapper for the hacked timer that 
static void hacktimer_handler(ngx_event_t *ev) {
    assert(ev->data == &hacktimer_data);
    hacktimer = NULL; //it's about to be deleted (and a new one added), might as well clear it right now.

    running_hacked_timer_handler = 1;
    //run original lua timer handler
    ev->handler = hacktimer_data.original_handler;
    ev->data = hacktimer_data.original_privdata;
    ev->handler(ev);
    running_hacked_timer_handler = 0;
}

static void ngx_lua_ipc_alert_handler(ngx_int_t sender, ngx_str_t *name, ngx_str_t *data);

static ngx_int_t initialize_shm(ngx_shm_zone_t *zone, void *data) {
    size_t size = sizeof(worker_slot_t) * 128;
    int i;
    if (data) { //zone being passed after restart
        zone->data = data;
        shm_reinit(shm);
    } else {
        shm_init(shm);

        if ((shdata = shm_calloc(shm, sizeof(*shdata), "root shared data")) == NULL) {
            return NGX_ERROR;
        }

        if ((shdata->worker_slots = shm_calloc(shm, size, "worker slots")) == NULL) {
            return NGX_ERROR;
        }

        zone->data = shdata;
    }

    ngx_memzero(shdata->worker_slots, size);

    return NGX_OK;
}

ngx_int_t luaL_checklstring_as_ngx_str(lua_State *L, int n, ngx_str_t *str) {
    size_t data_sz;
    const char *data = luaL_checklstring(L, n, &data_sz);

    str->data = (u_char *) data;
    str->len = data_sz;
    return NGX_OK;
}

static ngx_int_t ngx_lua_ipc_get_alert_args(lua_State *L, int stack_offset, ngx_str_t *name, ngx_str_t *data) {
    luaL_checklstring_as_ngx_str(L, 1 + stack_offset, name);
    if (lua_gettop(L) >= 2 + stack_offset) {
        luaL_checklstring_as_ngx_str(L, 2 + stack_offset, data);
    } else {
        data->len = 0;
        data->data = NULL;
    }

    return NGX_OK;
}

static int ngx_lua_ipc_hacktimer_alert_iterator(lua_State *L) {
    ipc_alert_waiting_t *cur = received_alerts.head;
    if (cur) {
        received_alerts.head = cur->next;
        if (received_alerts.tail == cur) {
            assert(cur->next == NULL);
            received_alerts.tail = NULL;
        }

        lua_pushinteger(L, cur->sender_slot);
        lua_pushinteger(L, cur->sender_pid);
        lua_pushlstring(L, (const char *) cur->name.data, cur->name.len);
        lua_pushlstring(L, (const char *) cur->data.data, cur->data.len);

        ngx_free(cur);
    } else {
        lua_pushnil(L);
        lua_pushnil(L);
        lua_pushnil(L);
        lua_pushnil(L);
    }
    return 4;
}

ngx_event_t *ngx_lua_ipc_get_hacktimer(ngx_msec_t magic_key) {

    ngx_event_t *ev;
    ngx_rbtree_node_t **p, *sentinel = ngx_event_timer_rbtree.sentinel, *temp = ngx_event_timer_rbtree.root;
    if (temp == sentinel) {
        return NULL;
    }
    while (1) {
        p = ((ngx_rbtree_key_int_t)(magic_key - temp->key) < 0) ? &temp->left : &temp->right;
        if (*p == sentinel) {
            break;
        }
        temp = *p;
    }

    if (temp != sentinel && temp->key == magic_key) {
        ev = container_of(temp, ngx_event_t, timer);
        return ev;
    } else {
        return NULL;
    }
}

static int
ngx_lua_ipc_hacktimer_unique_timeout(lua_State *L, uint32_t *unique_timeout, ngx_msec_t *unique_timeout_key) {
    uint32_t i;
    ngx_msec_t timeout_msec, timeout_key;
    for (i = 4596313; i != 0; i++) {
        lua_pushinteger(L, i);
        timeout_msec = (ngx_msec_t)(luaL_checknumber(L, -1) * 1000);
        //generate the timeout just like the lua call would
        lua_pop(L, 1);
        timeout_key = timeout_msec + ngx_current_msec;
        if (ngx_lua_ipc_get_hacktimer(timeout_key) == NULL) {
            //no timer exists with this timeout
            *unique_timeout = i;
            *unique_timeout_key = timeout_key;
            return 1;
        }

        DBG("timer with timeout %i exists, try again", timeout_msec);
    }
    return 0;
}

static int ngx_lua_ipc_hacktimer_add_and_hack(lua_State *L) {
    uint32_t unique_timeout;
    ngx_msec_t unique_timeout_key;
    ngx_event_t *ev;

    if (!ngx_event_timer_rbtree.sentinel) { //tree not ready yet
        luaL_error(L,
                   "Can't register receive handlers in init_by_lua, too early in Nginx start. Try init_worker_by_lua.");
    }

    DBG("ngx_lua_ipc_hacktimer_add_and_hack");
//    if (hacktimer) hacktimer = NULL;
//    assert(hacktimer == NULL);

    luaL_checktype(L, 1, LUA_TFUNCTION);

    lua_getglobal(L, "ngx");
    lua_getfield(L, -1, "timer");
    lua_getfield(L, -1, "at");

    if (!ngx_lua_ipc_hacktimer_unique_timeout(L, &unique_timeout, &unique_timeout_key)) {
        ERR("couldn't find unique timeout. hack failed.");
        return 0;
    }

    lua_pushinteger(L, unique_timeout);
    lua_pushvalue(L, 1); //callback function
    lua_call(L, 2, 0);

    //now find that crazy timer
    hacktimer = ngx_lua_ipc_get_hacktimer(unique_timeout_key);
//    assert(ev);
//
//    hacktimer_data.original_handler = ev->handler;
//    hacktimer_data.original_privdata = ev->data;
//
//    ev->data = &hacktimer_data;
//    ev->handler = hacktimer_handler;
//
//    ngx_del_timer(ev);
//    ngx_add_timer(ev, unique_timeout);
//    DBG("set hacked timer %p (prev %p)", ev, hacktimer);
//    hacktimer = ev;

    return 0;
}

static int ngx_lua_ipc_send_alert(lua_State *L) {
    int target_worker = luaL_checknumber(L, 1);

    ngx_str_t name, data;
    ngx_lua_ipc_get_alert_args(L, 1, &name, &data);

    int i;

    for (i = 0; i < max_workers; i++) {
        if (shdata->worker_slots[i].pid == target_worker) {
            ipc_alert(ipc, shdata->worker_slots[i].slot, &name, &data);
            break;
        }
    }
    return 0;
}

static int ngx_lua_ipc_broadcast_alert(lua_State *L) {
    ngx_str_t name, data;

    ngx_lua_ipc_get_alert_args(L, 0, &name, &data);
    int i;

    for (i = 0; i < max_workers; i++) {
        if (shdata->worker_slots[i].pid != ngx_pid) {
            ipc_alert(ipc, shdata->worker_slots[i].slot, &name, &data);
        }
    }

    return 0;
}

static void ngx_lua_ipc_alert_handler(ngx_int_t sender_slot, ngx_str_t *name, ngx_str_t *data) {

    ipc_alert_waiting_t *alert;
    int i;
    ngx_pid_t sender_pid = NGX_INVALID_PID;

    if (!hacktimer && !running_hacked_timer_handler) {
        //no alert handlers here
        return;
    }

    alert = ngx_alloc(sizeof(*alert) + name->len + data->len, ngx_cycle->log);
    assert(alert);

    //find sender process id
    for (i = 0; i < max_workers; i++) {
        if (shdata->worker_slots[i].slot == sender_slot) {
            sender_pid = shdata->worker_slots[i].pid;
            break;
        }
    }

    alert->sender_slot = sender_slot;
    alert->sender_pid = sender_pid;

    alert->name.data = (u_char * ) & alert[1];
    alert->name.len = name->len;
    ngx_memcpy(alert->name.data, name->data, name->len);

    alert->data.data = alert->name.data + alert->name.len;
    alert->data.len = data->len;
    ngx_memcpy(alert->data.data, data->data, data->len);

    alert->next = NULL;

    if (received_alerts.tail) {
        received_alerts.tail->next = alert;
    }
    received_alerts.tail = alert;
    if (!received_alerts.head) {
        received_alerts.head = alert;
    }

    //listener timer now!!
    if (hacktimer && hacktimer->timer.key > ngx_current_msec) {
        DBG("run hacked timer next cycle: %p", hacktimer);
        ngx_del_timer(hacktimer);
        ngx_add_timer(hacktimer, 0);
    } else if (!hacktimer) {
        DBG("timer handler running right now");
        assert(running_hacked_timer_handler == 1);
    } else {
        DBG("hacked timer %p already set to run on next cycle", hacktimer);
    }
    return;

}

static int ngx_lua_ipc_init_lua_code(lua_State *L) {
    if (ngx_process != NGX_PROCESS_WORKER && ngx_process != NGX_PROCESS_SINGLE) {
        return NGX_OK;
    }

    int t = lua_gettop(L) + 1;
    lua_createtable(L, 0, 5);
    lua_pushcfunction(L, ngx_lua_ipc_send_alert);
    lua_setfield(L, t, "send");

    lua_pushcfunction(L, ngx_lua_ipc_broadcast_alert);
    lua_setfield(L, t, "broadcast");

    ngx_lua_ipc_loadscript(L, reply);
    lua_pushvalue(L, t);
    lua_call(L, 1, 1);
    lua_setfield(L, t, "reply");

    ngx_lua_ipc_loadscript(L, register_receive_handler);
    lua_pushvalue(L, t); //ipc table
    lua_pushcfunction(L, ngx_lua_ipc_hacktimer_add_and_hack);
    lua_pushcfunction(L, ngx_lua_ipc_hacktimer_alert_iterator);
    lua_call(L, 3, 1);

    lua_setfield(L, t, "receive");

    lua_newtable(L); //handlers table
    lua_setfield(L, t, "handlers");
    return 1;
}

static ngx_int_t ngx_lua_ipc_init_postconfig(ngx_conf_t *cf) {
    ngx_str_t name = ngx_string("ngx_lua_ipc");

    shm = shm_create(&name, &ngx_lua_ipc_module, cf, 1024 * 1024, initialize_shm, &ngx_lua_ipc_module);

    if (ngx_http_lua_add_package_preload(cf, "ngx.ipc", ngx_lua_ipc_init_lua_code) != NGX_OK) {
        return NGX_ERROR;
    }

    return NGX_OK;
}

static ngx_int_t ngx_lua_ipc_init_module(ngx_cycle_t *cycle) {
    ngx_core_conf_t *ccf = (ngx_core_conf_t *) ngx_get_conf(cycle->conf_ctx, ngx_core_module);

    max_workers = ccf->worker_processes;

    //initialize our little IPC
    if (ipc == NULL) {
        ipc = &ipc_data;
        ipc_init(ipc);
        ipc_set_handler(ipc, ngx_lua_ipc_alert_handler);
    }
    ipc_open(ipc, cycle, ccf->worker_processes, NULL);
    return NGX_OK;
}

static ngx_int_t ngx_lua_ipc_init_worker(ngx_cycle_t *cycle) {
    int i, found = 0;
    if (ngx_process != NGX_PROCESS_WORKER && ngx_process != NGX_PROCESS_SINGLE) {
        return NGX_OK;
    }
    shmtx_lock(shm);
    for (i = 0; i < max_workers; i++) {
        DBG("test i:%d, pid:%d, slot:%d, id:%d", i, ngx_pid, ngx_process_slot, shdata->worker_slots[i].pid);
        if (shdata->worker_slots[i].pid == 0) {
            shdata->worker_slots[i].pid = ngx_pid;
            shdata->worker_slots[i].slot = ngx_process_slot;
            found = 1;
            DBG("pick i:%d, pid:%d, slot:%d, id:%d", i, ngx_pid, ngx_process_slot, shdata->worker_slots[i].pid);
            break;
        }
    }
    shmtx_unlock(shm);

    if (!found) {
        return NGX_ERROR;
    }
    ipc_register_worker(ipc, cycle);

    return NGX_OK;
}

static void ngx_lua_ipc_exit_worker(ngx_cycle_t *cycle) {
    if (ngx_process != NGX_PROCESS_WORKER && ngx_process != NGX_PROCESS_SINGLE) {
        return;
    }
    int i;
    shmtx_lock(shm);
    for (i = 0; i < max_workers; i++) {
        DBG("test i:%d, pid:%d, slot:%d, id:%d", i, ngx_pid, ngx_process_slot, shdata->worker_slots[i].pid);
        if (shdata->worker_slots[i].pid == ngx_pid) {
            shdata->worker_slots[i].pid = 0;
            shdata->worker_slots[i].slot = 0;
            DBG("drop i:%d, pid:%d, slot:%d, id:%d", i, ngx_pid, ngx_process_slot, shdata->worker_slots[i].pid);
            break;
        }
    }
    shmtx_unlock(shm);

    ipc_close(ipc, cycle);
}

static void ngx_lua_ipc_exit_master(ngx_cycle_t *cycle) {
    ipc_close(ipc, cycle);
//    if (shdata)shm_free(shm, shdata);
    shm_destroy(shm);
}

static ngx_command_t ngx_lua_ipc_commands[] = {
        ngx_null_command
};

static ngx_http_module_t ngx_lua_ipc_ctx = {
        NULL,                          /* preconfiguration */
        ngx_lua_ipc_init_postconfig,   /* postconfiguration */
        NULL,                          /* create main configuration */
        NULL,                          /* init main configuration */
        NULL,                          /* create server configuration */
        NULL,                          /* merge server configuration */
        NULL,                          /* create location configuration */
        NULL,                          /* merge location configuration */
};

ngx_module_t ngx_lua_ipc_module = {
        NGX_MODULE_V1,
        &ngx_lua_ipc_ctx,              /* module context */
        ngx_lua_ipc_commands,          /* module directives */
        NGX_HTTP_MODULE,               /* module type */
        NULL,                          /* init master */
        ngx_lua_ipc_init_module,       /* init module */
        ngx_lua_ipc_init_worker,       /* init process */
        NULL,                          /* init thread */
        NULL,                          /* exit thread */
        ngx_lua_ipc_exit_worker,       /* exit process */
        ngx_lua_ipc_exit_master,       /* exit master */
        NGX_MODULE_V1_PADDING
};
