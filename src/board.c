/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "ty/common.h"
#include "compat.h"
#include "ty/board.h"
#include "board_priv.h"
#include "ty/firmware.h"
#include "htable.h"
#include "list.h"
#include "ty/system.h"
#include "ty/timer.h"

struct ty_board_manager {
    ty_device_monitor *monitor;
    ty_timer *timer;

    bool enumerated;

    ty_list_head callbacks;
    int callback_id;

    ty_mutex refresh_mutex;
    ty_cond refresh_cond;

    ty_list_head boards;
    ty_list_head missing_boards;

    ty_htable interfaces;

    void *udata;
};

struct ty_board_model {
    TY_BOARD_MODEL
};

struct callback {
    ty_list_head list;
    int id;

    ty_board_manager_callback_func *f;
    void *udata;
};

struct firmware_signature {
    const ty_board_model *model;
    uint8_t magic[8];
};

extern const ty_board_model _ty_teensy_pp10_model;
extern const ty_board_model _ty_teensy_20_model;
extern const ty_board_model _ty_teensy_pp20_model;
extern const ty_board_model _ty_teensy_30_model;
extern const ty_board_model _ty_teensy_31_model;

const ty_board_model *ty_board_models[] = {
    &_ty_teensy_pp10_model,
    &_ty_teensy_20_model,
    &_ty_teensy_pp20_model,
    &_ty_teensy_30_model,
    &_ty_teensy_31_model,
    NULL
};

extern const struct _ty_board_vendor _ty_teensy_vendor;

static const struct _ty_board_vendor *vendors[] = {
    &_ty_teensy_vendor,
    NULL
};

static const char *capability_names[] = {
    "upload",
    "reset",
    "reboot",
    "serial"
};

static const struct firmware_signature signatures[] = {
    {&_ty_teensy_pp10_model, {0x0C, 0x94, 0x00, 0x7E, 0xFF, 0xCF, 0xF8, 0x94}},
    {&_ty_teensy_20_model,   {0x0C, 0x94, 0x00, 0x3F, 0xFF, 0xCF, 0xF8, 0x94}},
    {&_ty_teensy_pp20_model, {0x0C, 0x94, 0x00, 0xFE, 0xFF, 0xCF, 0xF8, 0x94}},
    {&_ty_teensy_30_model,   {0x38, 0x80, 0x04, 0x40, 0x82, 0x3F, 0x04, 0x00}},
    {&_ty_teensy_31_model,   {0x30, 0x80, 0x04, 0x40, 0x82, 0x3F, 0x04, 0x00}},
    {0}
};

static const int drop_board_delay = 5000;

static void drop_callback(struct callback *callback)
{
    ty_list_remove(&callback->list);
    free(callback);
}

static int trigger_callbacks(ty_board *board, ty_board_event event)
{
    ty_list_foreach(cur, &board->manager->callbacks) {
        struct callback *callback = ty_container_of(cur, struct callback, list);
        int r;

        r = (*callback->f)(board, event, callback->udata);
        if (r < 0)
            return r;
        if (r)
            drop_callback(callback);
    }

    return 0;
}

static int add_board(ty_board_manager *manager, ty_board_interface *iface, ty_board **rboard)
{
    ty_board *board;
    int r;

    board = calloc(1, sizeof(*board));
    if (!board) {
        r = ty_error(TY_ERROR_MEMORY, NULL);
        goto error;
    }
    board->refcount = 1;

    r = ty_mutex_init(&board->mutex, TY_MUTEX_RECURSIVE);
    if (r < 0)
        goto error;

    board->location = strdup(ty_device_get_location(iface->dev));
    if (!board->location) {
        r = ty_error(TY_ERROR_MEMORY, NULL);
        goto error;
    }

    ty_list_init(&board->interfaces);

    board->model = iface->model;
    board->serial = iface->serial;

    board->vid = ty_device_get_vid(iface->dev);
    board->pid = ty_device_get_pid(iface->dev);

    r = asprintf(&board->identity, "%s#%"PRIu64, board->location, board->serial);
    if (r < 0) {
        r = ty_error(TY_ERROR_MEMORY, NULL);
        goto error;
    }

    board->manager = manager;
    ty_list_add_tail(&manager->boards, &board->list);

    *rboard = board;
    return 0;

error:
    ty_board_unref(board);
    return r;
}

static void close_board(ty_board *board)
{
    board->state = TY_BOARD_STATE_MISSING;

    ty_list_foreach(cur, &board->interfaces) {
        ty_board_interface *iface = ty_container_of(cur, ty_board_interface, list);

        if (iface->hnode.next)
            ty_htable_remove(&iface->hnode);

        ty_board_interface_unref(iface);
    }
    ty_list_init(&board->interfaces);

    memset(board->cap2iface, 0, sizeof(board->cap2iface));
    board->capabilities = 0;

    trigger_callbacks(board, TY_BOARD_EVENT_DISAPPEARED);
}

static int add_missing_board(ty_board *board)
{
    board->missing_since = ty_millis();
    if (board->missing.prev)
        ty_list_remove(&board->missing);
    ty_list_add_tail(&board->manager->missing_boards, &board->missing);

    // There may be other boards waiting to be dropped, set timeout for the next in line
    board = ty_list_get_first(&board->manager->missing_boards, ty_board, missing);

    return ty_timer_set(board->manager->timer, ty_adjust_timeout(drop_board_delay, board->missing_since), TY_TIMER_ONESHOT);
}

static void drop_board(ty_board *board)
{
    board->state = TY_BOARD_STATE_DROPPED;

    if (board->missing.prev)
        ty_list_remove(&board->missing);

    trigger_callbacks(board, TY_BOARD_EVENT_DROPPED);

    ty_list_remove(&board->list);
    board->manager = NULL;
}

static ty_board *find_board(ty_board_manager *manager, const char *location)
{
    ty_list_foreach(cur, &manager->boards) {
        ty_board *board = ty_container_of(cur, ty_board, list);

        if (strcmp(board->location, location) == 0)
            return board;
    }

    return NULL;
}

static int open_interface(ty_device *dev, ty_board_interface **riface)
{
    ty_board_interface *iface;
    const char *serial;
    int r;

    iface = calloc(1, sizeof(*iface));
    if (!iface) {
        r = ty_error(TY_ERROR_MEMORY, NULL);
        goto error;
    }
    iface->refcount = 1;

    iface->dev = ty_device_ref(dev);

    serial = ty_device_get_serial_number(dev);
    if (serial)
        iface->serial = strtoull(serial, NULL, 10);

    r = 0;
    for (const struct _ty_board_vendor **cur = vendors; *cur; cur++) {
        const struct _ty_board_vendor *vendor = *cur;

        ty_error_mask(TY_ERROR_NOT_FOUND);
        r = (*vendor->open_interface)(iface);
        ty_error_unmask();
        if (r < 0) {
            if (r != TY_ERROR_NOT_FOUND)
                goto error;
        }
        if (r)
            break;
    }
    if (!r)
        goto error;

    *riface = iface;
    return 1;

error:
    ty_board_interface_unref(iface);
    return r;
}

static ty_board_interface *find_interface(ty_board_manager *manager, ty_device *dev)
{
    ty_htable_foreach_hash(cur, &manager->interfaces, ty_htable_hash_ptr(dev)) {
        ty_board_interface *iface = ty_container_of(cur, ty_board_interface, hnode);

        if (iface->dev == dev)
            return iface;
    }

    return NULL;
}

static inline bool model_is_valid(const ty_board_model *model)
{
    return model && model->code_size;
}

static int add_interface(ty_board_manager *manager, ty_device *dev)
{
    ty_board_interface *iface = NULL;
    ty_board *board = NULL;
    ty_board_event event;
    int r;

    r = open_interface(dev, &iface);
    if (r <= 0)
        goto cleanup;

    board = find_board(manager, ty_device_get_location(dev));

    /* Maybe the device notifications came in the wrong order, or somehow the device removal
       notifications were dropped somewhere and we never got it, so use heuristics to improve
       board change detection. */
    if (board) {
        ty_board_lock(board);

        if ((model_is_valid(iface->model) && model_is_valid(board->model) && iface->model != board->model)
                || iface->serial != board->serial) {
            drop_board(board);

            ty_board_unlock(board);
            ty_board_unref(board);

            board = NULL;
        } else if (board->vid != ty_device_get_vid(dev) || board->pid != ty_device_get_pid(dev)) {
            close_board(board);

            board->vid = ty_device_get_vid(dev);
            board->pid = ty_device_get_pid(dev);
        }
    }

    if (board) {
        if (model_is_valid(iface->model))
            board->model = iface->model;
        if (iface->serial)
            board->serial = iface->serial;

        event = TY_BOARD_EVENT_CHANGED;
    } else {
        r = add_board(manager, iface, &board);
        if (r < 0)
            goto cleanup;
        ty_board_lock(board);

        event = TY_BOARD_EVENT_ADDED;
    }

    iface->board = board;

    ty_list_add_tail(&board->interfaces, &iface->list);
    ty_htable_add(&manager->interfaces, ty_htable_hash_ptr(iface->dev), &iface->hnode);

    for (int i = 0; i < (int)TY_COUNTOF(board->cap2iface); i++) {
        if (iface->capabilities & (1 << i))
            board->cap2iface[i] = iface;
    }
    board->capabilities |= iface->capabilities;

    if (board->missing.prev)
        ty_list_remove(&board->missing);

    board->state = TY_BOARD_STATE_ONLINE;
    iface = NULL;

    r = trigger_callbacks(board, event);

cleanup:
    if (board)
        ty_board_unlock(board);
    ty_board_interface_unref(iface);
    return r;
}

static int remove_interface(ty_board_manager *manager, ty_device *dev)
{
    ty_board_interface *iface;
    ty_board *board;
    int r;

    iface = find_interface(manager, dev);
    if (!iface)
        return 0;

    board = iface->board;

    ty_board_lock(board);

    ty_htable_remove(&iface->hnode);
    ty_list_remove(&iface->list);

    ty_board_interface_unref(iface);

    memset(board->cap2iface, 0, sizeof(board->cap2iface));
    board->capabilities = 0;

    ty_list_foreach(cur, &board->interfaces) {
        iface = ty_container_of(cur, ty_board_interface, list);

        for (unsigned int i = 0; i < TY_COUNTOF(board->cap2iface); i++) {
            if (iface->capabilities & (1 << i))
                board->cap2iface[i] = iface;
        }
        board->capabilities |= iface->capabilities;
    }

    if (ty_list_is_empty(&board->interfaces)) {
        close_board(board);

        r = add_missing_board(board);
        if (r < 0)
            goto cleanup;
    } else {
        r = trigger_callbacks(board, TY_BOARD_EVENT_CHANGED);
        if (r < 0)
            goto cleanup;
    }

    r = 0;
cleanup:
    ty_board_unlock(board);
    return r;
}

static int device_callback(ty_device *dev, ty_device_event event, void *udata)
{
    ty_board_manager *manager = udata;

    switch (event) {
    case TY_DEVICE_EVENT_ADDED:
        return add_interface(manager, dev);

    case TY_DEVICE_EVENT_REMOVED:
        return remove_interface(manager, dev);
    }

    assert(false);
    __builtin_unreachable();
}

int ty_board_manager_new(ty_board_manager **rmanager)
{
    assert(rmanager);

    ty_board_manager *manager;
    int r;

    manager = calloc(1, sizeof(*manager));
    if (!manager) {
        r = ty_error(TY_ERROR_MEMORY, NULL);
        goto error;
    }

    r = ty_device_monitor_new(&manager->monitor);
    if (r < 0)
        goto error;

    r = ty_device_monitor_register_callback(manager->monitor, device_callback, manager);
    if (r < 0)
        goto error;

    r = ty_timer_new(&manager->timer);
    if (r < 0)
        goto error;

    r = ty_mutex_init(&manager->refresh_mutex, TY_MUTEX_FAST);
    if (r < 0)
        goto error;

    r = ty_cond_init(&manager->refresh_cond);
    if (r < 0)
        goto error;

    ty_list_init(&manager->boards);
    ty_list_init(&manager->missing_boards);

    r = ty_htable_init(&manager->interfaces, 64);
    if (r < 0)
        goto error;

    ty_list_init(&manager->callbacks);

    *rmanager = manager;
    return 0;

error:
    ty_board_manager_free(manager);
    return r;
}

void ty_board_manager_free(ty_board_manager *manager)
{
    if (manager) {
        ty_cond_release(&manager->refresh_cond);
        ty_mutex_release(&manager->refresh_mutex);

        ty_device_monitor_free(manager->monitor);
        ty_timer_free(manager->timer);

        ty_list_foreach(cur, &manager->callbacks) {
            struct callback *callback = ty_container_of(cur, struct callback, list);
            free(callback);
        }

        ty_list_foreach(cur, &manager->boards) {
            ty_board *board = ty_container_of(cur, ty_board, list);

            board->manager = NULL;
            ty_board_unref(board);
        }

        ty_htable_release(&manager->interfaces);
    }

    free(manager);
}

void ty_board_manager_set_udata(ty_board_manager *manager, void *udata)
{
    assert(manager);
    manager->udata = udata;
}

void *ty_board_manager_get_udata(const ty_board_manager *manager)
{
    assert(manager);
    return manager->udata;
}

void ty_board_manager_get_descriptors(const ty_board_manager *manager, ty_descriptor_set *set, int id)
{
    assert(manager);
    assert(set);

    ty_device_monitor_get_descriptors(manager->monitor, set, id);
    ty_timer_get_descriptors(manager->timer, set, id);
}

int ty_board_manager_register_callback(ty_board_manager *manager, ty_board_manager_callback_func *f, void *udata)
{
    assert(manager);
    assert(f);

    struct callback *callback = calloc(1, sizeof(*callback));
    if (!callback)
        return ty_error(TY_ERROR_MEMORY, NULL);

    callback->id = manager->callback_id++;
    callback->f = f;
    callback->udata = udata;

    ty_list_add_tail(&manager->callbacks, &callback->list);

    return callback->id;
}

void ty_board_manager_deregister_callback(ty_board_manager *manager, int id)
{
    assert(manager);
    assert(id >= 0);

    ty_list_foreach(cur, &manager->callbacks) {
        struct callback *callback = ty_container_of(cur, struct callback, list);
        if (callback->id == id) {
            drop_callback(callback);
            break;
        }
    }
}

int ty_board_manager_refresh(ty_board_manager *manager)
{
    assert(manager);

    int r;

    if (ty_timer_rearm(manager->timer)) {
        ty_list_foreach(cur, &manager->missing_boards) {
            ty_board *board = ty_container_of(cur, ty_board, missing);
            int timeout;

            timeout = ty_adjust_timeout(drop_board_delay, board->missing_since);
            if (timeout) {
                r = ty_timer_set(manager->timer, timeout, TY_TIMER_ONESHOT);
                if (r < 0)
                    return r;
                break;
            }

            drop_board(board);

            ty_board_unref(board);
        }
    }

    if (!manager->enumerated) {
        manager->enumerated = true;

        // FIXME: never listed devices if error on enumeration (unlink the real refresh)
        r = ty_device_monitor_list(manager->monitor, device_callback, manager);
        if (r < 0)
            return r;

        return 0;
    }

    r = ty_device_monitor_refresh(manager->monitor);
    if (r < 0)
        return r;

    ty_mutex_lock(&manager->refresh_mutex);
    ty_cond_broadcast(&manager->refresh_cond);
    ty_mutex_unlock(&manager->refresh_mutex);

    return 0;
}

int ty_board_manager_wait(ty_board_manager *manager, ty_board_manager_wait_func *f, void *udata, int timeout)
{
    assert(manager);

    ty_descriptor_set set = {0};
    uint64_t start;
    int r;

    ty_board_manager_get_descriptors(manager, &set, 1);

    start = ty_millis();
    do {
        r = ty_board_manager_refresh(manager);
        if (r < 0)
            return (int)r;

        if (f) {
            r = (*f)(manager, udata);
            if (r)
                return (int)r;
        }

        r = ty_poll(&set, ty_adjust_timeout(timeout, start));
    } while (r > 0);

    return r;
}

int ty_board_manager_list(ty_board_manager *manager, ty_board_manager_callback_func *f, void *udata)
{
    assert(manager);
    assert(f);

    ty_list_foreach(cur, &manager->boards) {
        ty_board *board = ty_container_of(cur, ty_board, list);
        int r;

        if (board->state == TY_BOARD_STATE_ONLINE) {
            r = (*f)(board, TY_BOARD_EVENT_ADDED, udata);
            if (r)
                return r;
        }
    }

    return 0;
}

const ty_board_model *ty_board_find_model(const char *name)
{
    assert(name);

    for (const ty_board_model **cur = ty_board_models; *cur; cur++) {
        const ty_board_model *model = *cur;
        if (strcmp(model->name, name) == 0 || strcmp(model->mcu, name) == 0)
            return model;
    }

    return NULL;
}

const char *ty_board_model_get_name(const ty_board_model *model)
{
    assert(model);
    return model->name;
}

const char *ty_board_model_get_mcu(const ty_board_model *model)
{
    assert(model);
    return model->mcu;
}

const char *ty_board_model_get_desc(const ty_board_model *model)
{
    assert(model);
    return model->desc;
}

size_t ty_board_model_get_code_size(const ty_board_model *model)
{
    assert(model);
    return model->code_size;
}

const char *ty_board_get_capability_name(ty_board_capability cap)
{
    assert((int)cap >= 0 && (int)cap < TY_BOARD_CAPABILITY_COUNT);
    return capability_names[cap];
}

ty_board *ty_board_ref(ty_board *board)
{
    assert(board);

    __atomic_add_fetch(&board->refcount, 1, __ATOMIC_RELAXED);
    return board;
}

void ty_board_unref(ty_board *board)
{
    if (board) {
        if (__atomic_fetch_sub(&board->refcount, 1, __ATOMIC_RELEASE) > 1)
            return;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);

        ty_mutex_release(&board->mutex);

        free(board->identity);
        free(board->location);

        ty_list_foreach(cur, &board->interfaces) {
            ty_board_interface *iface = ty_container_of(cur, ty_board_interface, list);

            if (iface->hnode.next)
                ty_htable_remove(&iface->hnode);

            ty_board_interface_unref(iface);
        }
    }

    free(board);
}

void ty_board_lock(const ty_board *board)
{
    assert(board);

    ty_mutex_lock(&((ty_board *)board)->mutex);
}

void ty_board_unlock(const ty_board *board)
{
    assert(board);

    ty_mutex_unlock(&((ty_board *)board)->mutex);
}

static int parse_identity(const char *id, char **rlocation, uint64_t *rserial)
{
    char *location = NULL;
    uint64_t serial = 0;
    char *ptr;
    int r;

    ptr = strchr(id, '#');

    if (ptr != id) {
        if (ptr) {
            location = strndup(id, (size_t)(ptr - id));
        } else {
            location = strdup(id);
        }
        if (!location) {
            r = ty_error(TY_ERROR_MEMORY, NULL);
            goto error;
        }
    }

    if (ptr) {
        char *end;
        serial = strtoull(++ptr, &end, 10);
        if (end == ptr || *end) {
            r = ty_error(TY_ERROR_PARAM, "#<serial> must be a number");
            goto error;
        }
    }

    *rlocation = location;
    *rserial = serial;
    return 0;

error:
    free(location);
    return r;
}

int ty_board_matches_identity(ty_board *board, const char *id)
{
    assert(board);

    if (!id || !*id)
        return 1;

    char *location = NULL;
    uint64_t serial = 0;
    int r;

    r = parse_identity(id, &location, &serial);
    if (r < 0)
        return r;

    r = 0;
    if (location && strcmp(location, board->location) != 0)
        goto cleanup;
    if (serial && serial != board->serial)
        goto cleanup;

    r = 1;
cleanup:
    free(location);
    return r;
}

void ty_board_set_udata(ty_board *board, void *udata)
{
    assert(board);
    board->udata = udata;
}

void *ty_board_get_udata(const ty_board *board)
{
    assert(board);
    return board->udata;
}

ty_board_manager *ty_board_get_manager(const ty_board *board)
{
    assert(board);
    return board->manager;
}

ty_board_state ty_board_get_state(const ty_board *board)
{
    assert(board);
    return board->state;
}

const char *ty_board_get_identity(const ty_board *board)
{
    assert(board);
    return board->identity;
}

const char *ty_board_get_location(const ty_board *board)
{
    assert(board);
    return board->location;
}

const ty_board_model *ty_board_get_model(const ty_board *board)
{
    assert(board);
    return board->model;
}

const char *ty_board_get_model_name(const ty_board *board)
{
    assert(board);

    const ty_board_model *model = board->model;
    if (!model)
        return NULL;

    return model->name;
}

const char *ty_board_get_model_desc(const ty_board *board)
{
    assert(board);

    const ty_board_model *model = board->model;
    if (!model)
        return NULL;

    return model->desc;
}

ty_board_interface *ty_board_get_interface(const ty_board *board, ty_board_capability cap)
{
    assert(board);
    assert((int)cap < (int)TY_COUNTOF(board->cap2iface));

    ty_board_interface *iface;

    ty_board_lock(board);

    iface = board->cap2iface[cap];
    if (iface)
        ty_board_interface_ref(iface);

    ty_board_unlock(board);

    return iface;
}

int ty_board_get_capabilities(const ty_board *board)
{
    assert(board);
    return board->capabilities;
}

uint64_t ty_board_get_serial_number(const ty_board *board)
{
    assert(board);
    return board->serial;
}

ty_device *ty_board_get_device(const ty_board *board, ty_board_capability cap)
{
    assert(board);

    ty_board_interface *iface;
    ty_device *dev;

    iface = ty_board_get_interface(board, cap);
    if (!iface)
        return NULL;

    dev = iface->dev;

    ty_board_interface_unref(iface);
    return dev;
}

ty_handle *ty_board_get_handle(const ty_board *board, ty_board_capability cap)
{
    assert(board);

    ty_board_interface *iface;
    ty_handle *h;

    iface = ty_board_get_interface(board, cap);
    if (!iface)
        return NULL;

    h = iface->h;

    ty_board_interface_unref(iface);
    return h;
}

void ty_board_get_descriptors(const ty_board *board, ty_board_capability cap, struct ty_descriptor_set *set, int id)
{
    assert(board);

    ty_board_interface *iface = ty_board_get_interface(board, cap);
    if (!iface)
        return;

    ty_device_get_descriptors(iface->h, set, id);
    ty_board_interface_unref(iface);
}

int ty_board_list_interfaces(ty_board *board, ty_board_list_interfaces_func *f, void *udata)
{
    assert(board);
    assert(f);

    int r;

    ty_board_lock(board);

    ty_list_foreach(cur, &board->interfaces) {
        ty_board_interface *iface = ty_container_of(cur, ty_board_interface, list);

        r = (*f)(iface, udata);
        if (r)
            goto cleanup;
    }

    r = 0;
cleanup:
    ty_board_unlock(board);
    return r;
}

struct wait_for_context {
    ty_board *board;
    ty_board_capability capability;
};

static int wait_for_callback(ty_board_manager *manager, void *udata)
{
    TY_UNUSED(manager);

    struct wait_for_context *ctx = udata;

    if (ctx->board->state == TY_BOARD_STATE_DROPPED)
        return ty_error(TY_ERROR_NOT_FOUND, "Board has disappeared");

    return ty_board_has_capability(ctx->board, ctx->capability);
}

int ty_board_wait_for(ty_board *board, ty_board_capability capability, bool parallel, int timeout)
{
    assert(board);

    ty_board_manager *manager = board->manager;

    struct wait_for_context ctx;
    int r;

    if (!manager)
        return ty_error(TY_ERROR_NOT_FOUND, "Board has disappeared");

    ctx.board = board;
    ctx.capability = capability;

    if (parallel) {
        uint64_t start;

        ty_mutex_lock(&manager->refresh_mutex);

        start = ty_millis();
        while (!(r = wait_for_callback(manager, &ctx))) {
            r = ty_cond_wait(&manager->refresh_cond, &manager->refresh_mutex, ty_adjust_timeout(timeout, start));
            if (!r)
                break;
        }

        ty_mutex_unlock(&manager->refresh_mutex);

        return r;
    } else {
        return ty_board_manager_wait(manager, wait_for_callback, &ctx, timeout);
    }
}

int ty_board_serial_set_attributes(ty_board *board, uint32_t rate, int flags)
{
    assert(board);

    ty_board_interface *iface;
    int r;

    iface = ty_board_get_interface(board, TY_BOARD_CAPABILITY_SERIAL);
    if (!iface)
        return ty_error(TY_ERROR_MODE, "Serial transfer is not available in this mode");

    r = (*iface->vtable->serial_set_attributes)(iface, rate, flags);

    ty_board_interface_unref(iface);
    return r;
}

ssize_t ty_board_serial_read(ty_board *board, char *buf, size_t size, int timeout)
{
    assert(board);
    assert(buf);
    assert(size);

    ty_board_interface *iface;
    ssize_t r;

    iface = ty_board_get_interface(board, TY_BOARD_CAPABILITY_SERIAL);
    if (!iface)
        return ty_error(TY_ERROR_MODE, "Serial transfer is not available in this mode");

    r = (*iface->vtable->serial_read)(iface, buf, size, timeout);

    ty_board_interface_unref(iface);
    return r;
}

ssize_t ty_board_serial_write(ty_board *board, const char *buf, size_t size)
{
    assert(board);
    assert(buf);

    ty_board_interface *iface;
    ssize_t r;

    iface = ty_board_get_interface(board, TY_BOARD_CAPABILITY_SERIAL);
    if (!iface)
        return ty_error(TY_ERROR_MODE, "Serial transfer is not available in this mode");

    if (!size)
        size = strlen(buf);

    r = (*iface->vtable->serial_write)(iface, buf, size);

    ty_board_interface_unref(iface);
    return r;
}

int ty_board_upload(ty_board *board, ty_firmware *f, int flags, ty_board_upload_progress_func *pf, void *udata)
{
    assert(board);
    assert(f);

    ty_board_interface *iface;
    int r;

    iface = ty_board_get_interface(board, TY_BOARD_CAPABILITY_UPLOAD);
    if (!iface) {
        r = ty_error(TY_ERROR_MODE, "Firmware upload is not available in this mode");
        goto cleanup;
    }

    if (!model_is_valid(board->model)) {
        r = ty_error(TY_ERROR_MODE, "Cannot upload to unknown board model");
        goto cleanup;
    }

    // FIXME: detail error message (max allowed, ratio)
    if (f->size > board->model->code_size) {
        r = ty_error(TY_ERROR_RANGE, "Firmware is too big for %s", board->model->desc);
        goto cleanup;
    }

    if (!(flags & TY_BOARD_UPLOAD_NOCHECK)) {
        const ty_board_model *guess;

        guess = ty_board_test_firmware(f);
        if (!guess) {
            r = ty_error(TY_ERROR_FIRMWARE, "This firmware was not compiled for a known device");
            goto cleanup;
        }

        if (guess != board->model) {
            r = ty_error(TY_ERROR_FIRMWARE, "This firmware was compiled for %s", guess->desc);
            goto cleanup;
        }
    }

    r = (*iface->vtable->upload)(iface, f, flags, pf, udata);

cleanup:
    ty_board_interface_unref(iface);
    return r;
}

int ty_board_reset(ty_board *board)
{
    assert(board);

    ty_board_interface *iface;
    int r;

    iface = ty_board_get_interface(board, TY_BOARD_CAPABILITY_RESET);
    if (!iface)
        return ty_error(TY_ERROR_MODE, "Cannot reset in this mode");

    r = (*iface->vtable->reset)(iface);

    ty_board_interface_unref(iface);
    return r;
}

int ty_board_reboot(ty_board *board)
{
    assert(board);

    ty_board_interface *iface;
    int r;

    iface = ty_board_get_interface(board, TY_BOARD_CAPABILITY_REBOOT);
    if (!iface)
        return ty_error(TY_ERROR_MODE, "Cannot reboot in this mode");

    r = (*iface->vtable->reboot)(iface);

    ty_board_interface_unref(iface);
    return r;
}

const ty_board_model *ty_board_test_firmware(const ty_firmware *f)
{
    assert(f);

    size_t magic_size = sizeof(signatures[0].magic);

    if (f->size < magic_size)
        return NULL;

    /* Naive search with each board's signature, not pretty but unless
       thousands of models appear this is good enough. */
    for (size_t i = 0; i < f->size - magic_size; i++) {
        for (const struct firmware_signature *sig = signatures; sig->model; sig++) {
            if (memcmp(f->image + i, sig->magic, magic_size) == 0)
                return sig->model;
        }
    }

    return NULL;
}

ty_board_interface *ty_board_interface_ref(ty_board_interface *iface)
{
    assert(iface);

    __atomic_add_fetch(&iface->refcount, 1, __ATOMIC_RELAXED);
    return iface;
}

void ty_board_interface_unref(ty_board_interface *iface)
{
    if (iface) {
        if (__atomic_fetch_sub(&iface->refcount, 1, __ATOMIC_RELEASE) > 1)
            return;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);

        ty_device_close(iface->h);
        ty_device_unref(iface->dev);
    }

    free(iface);
}

const char *ty_board_interface_get_desc(const ty_board_interface *iface)
{
    assert(iface);
    return iface->desc;
}

int ty_board_interface_get_capabilities(const ty_board_interface *iface)
{
    assert(iface);
    return iface->capabilities;
}

const char *ty_board_interface_get_path(const ty_board_interface *iface)
{
    assert(iface);
    return ty_device_get_path(iface->dev);
}

uint8_t ty_board_interface_get_interface_number(const ty_board_interface *iface)
{
    assert(iface);
    return ty_device_get_interface_number(iface->dev);
}

ty_device *ty_board_interface_get_device(const ty_board_interface *iface)
{
    assert(iface);
    return iface->dev;
}

ty_handle *ty_board_interface_get_handle(const ty_board_interface *iface)
{
    assert(iface);
    return iface->h;
}

void ty_board_interface_get_descriptors(const ty_board_interface *iface, struct ty_descriptor_set *set, int id)
{
    assert(iface);
    assert(set);

    ty_device_get_descriptors(iface->h, set, id);
}
