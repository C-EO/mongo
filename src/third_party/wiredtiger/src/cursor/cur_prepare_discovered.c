/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int __cursor_prepared_discover_list_create(
  WT_SESSION_IMPL *, WT_CURSOR_PREPARE_DISCOVERED *);
static int __cursor_prepared_discover_setup(WT_SESSION_IMPL *, WT_CURSOR_PREPARE_DISCOVERED *);

/*
 * __cursor_prepared_discover_next --
 *     WT_CURSOR->next method for the prepared transaction cursor type.
 */
static int
__cursor_prepared_discover_next(WT_CURSOR *cursor)
{
    WT_CURSOR_PREPARE_DISCOVERED *cursor_prepare;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    size_t size;

    cursor_prepare = (WT_CURSOR_PREPARE_DISCOVERED *)cursor;
    CURSOR_API_CALL(cursor, session, ret, next, NULL);

    if (cursor_prepare->list == NULL || cursor_prepare->list[cursor_prepare->next] == 0) {
        F_CLR(cursor, WT_CURSTD_KEY_SET);
        WT_ERR(WT_NOTFOUND);
    }

    WT_ERR(__wt_struct_size(
      session, &size, cursor->key_format, cursor_prepare->list[cursor_prepare->next]));
    WT_ERR(__wt_buf_initsize(session, &cursor->key, size));
    WT_ERR(__wt_struct_pack(session, cursor->key.mem, size, cursor->key_format,
      cursor_prepare->list[cursor_prepare->next]));
    ++cursor_prepare->next;

    F_SET(cursor, WT_CURSTD_KEY_INT);

err:
    API_END_RET(session, ret);
}

/*
 * __cursor_prepared_discover_reset --
 *     WT_CURSOR->reset method for the prepared transaction cursor type.
 */
static int
__cursor_prepared_discover_reset(WT_CURSOR *cursor)
{
    WT_CURSOR_PREPARE_DISCOVERED *cursor_prepare;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    cursor_prepare = (WT_CURSOR_PREPARE_DISCOVERED *)cursor;
    CURSOR_API_CALL_PREPARE_ALLOWED(cursor, session, reset, NULL);

    cursor_prepare->next = 0;
    F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);

err:
    API_END_RET(session, ret);
}

/*
 * __cursor_prepared_discover_close --
 *     WT_CURSOR->close method for the prepared transaction cursor type.
 */
static int
__cursor_prepared_discover_close(WT_CURSOR *cursor)
{
    WT_CURSOR_PREPARE_DISCOVERED *cursor_prepare;
    WT_DECL_RET;
    WT_SESSION_IMPL *next_session;
    WT_SESSION_IMPL *session;
    WT_TXN_GLOBAL *txn_global;
    size_t prepared_session_cnt;

    cursor_prepare = (WT_CURSOR_PREPARE_DISCOVERED *)cursor;
    CURSOR_API_CALL_PREPARE_ALLOWED(cursor, session, close, NULL);

    txn_global = &S2C(session)->txn_global;
    if (txn_global->pending_prepared_sessions_allocated > 0) {
        for (prepared_session_cnt = 0;
             prepared_session_cnt < txn_global->pending_prepared_sessions_count;
             prepared_session_cnt++) {
            next_session = txn_global->pending_prepared_sessions[prepared_session_cnt];
            if (next_session != NULL)
                WT_ERR(__wt_session_close_internal(next_session));
        }
        __wt_free(session, txn_global->pending_prepared_sessions);
        txn_global->pending_prepared_sessions_allocated = 0;
        txn_global->pending_prepared_sessions_count = 0;
    }

err:

    __wt_free(session, cursor_prepare->list);
    __wt_cursor_close(cursor);

    API_END_RET(session, ret);
}

/*
 * __wt_cursor_prepared_discover_open --
 *     WT_SESSION->open_cursor method for the prepared transaction cursor type.
 */
int
__wt_cursor_prepared_discover_open(WT_SESSION_IMPL *session, const char *uri, WT_CURSOR *other,
  const char *cfg[], WT_CURSOR **cursorp)
{
    WT_CURSOR_STATIC_INIT(iface, __wt_cursor_get_key, /* get-key */
      __wti_cursor_get_value_notsup,                  /* get-value */
      __wti_cursor_get_raw_key_value_notsup,          /* get-raw-key-value */
      __wti_cursor_set_key_notsup,                    /* set-key */
      __wti_cursor_set_value_notsup,                  /* set-value */
      __wti_cursor_compare_notsup,                    /* compare */
      __wti_cursor_equals_notsup,                     /* equals */
      __cursor_prepared_discover_next,                /* next */
      __wt_cursor_notsup,                             /* prev */
      __cursor_prepared_discover_reset,               /* reset */
      __wt_cursor_notsup,                             /* search */
      __wt_cursor_search_near_notsup,                 /* search-near */
      __wt_cursor_notsup,                             /* insert */
      __wt_cursor_modify_notsup,                      /* modify */
      __wt_cursor_notsup,                             /* update */
      __wt_cursor_notsup,                             /* remove */
      __wt_cursor_notsup,                             /* reserve */
      __wt_cursor_config_notsup,                      /* reconfigure */
      __wt_cursor_notsup,                             /* largest_key */
      __wt_cursor_config_notsup,                      /* bound */
      __wt_cursor_notsup,                             /* cache */
      __wt_cursor_reopen_notsup,                      /* reopen */
      __wt_cursor_checkpoint_id,                      /* checkpoint ID */
      __cursor_prepared_discover_close);              /* close */
    WT_CURSOR *cursor;
    WT_CURSOR_PREPARE_DISCOVERED *cursor_prepare;
    WT_DECL_RET;

    WT_VERIFY_OPAQUE_POINTER(WT_CURSOR_PREPARE_DISCOVERED);

    WT_UNUSED(other);
    WT_UNUSED(cfg);

    /*
     * TODO: This should acquire a prepared transaction discovery lock read/write lock in write
     * mode. Any thread wanting to commit a prepared transaction should acquire that lock in read
     * mode (or return an error). If the write lock is already held, this should exit immediately.
     */
    WT_RET(__wt_calloc_one(session, &cursor_prepare));
    cursor = (WT_CURSOR *)cursor_prepare;
    *cursor = iface;
    cursor->session = (WT_SESSION *)session;
    cursor->key_format = "Q";  /* The key is an unsigned 64 bit number. */
    cursor->value_format = ""; /* Empty for now, will probably have something eventually */

    /*
     * Start the prepared transaction cursor which will fill in the cursor's list. Acquire the
     * schema lock, we need a consistent view of the metadata when scanning for prepared artifacts.
     */
    WT_WITH_CHECKPOINT_LOCK(session,
      WT_WITH_SCHEMA_LOCK(
        session, ret = __cursor_prepared_discover_setup(session, cursor_prepare)));
    WT_ERR(ret);
    WT_ERR(__wt_cursor_init(cursor, uri, NULL, cfg, cursorp));

    if (0) {
err:
        WT_TRET(__cursor_prepared_discover_close(cursor));
        *cursorp = NULL;
    }

    return (ret);
}

/*
 * __cursor_prepared_discover_setup --
 *     Setup a prepared transaction cursor on open. This will populate the data structures for the
 *     cursor to traverse. Some data structures live in this cursor, others live in the connection
 *     handle, since they can be claimed by other sessions while the cursor is open.
 */
static int
__cursor_prepared_discover_setup(
  WT_SESSION_IMPL *session, WT_CURSOR_PREPARE_DISCOVERED *cursor_prepare)
{
    WT_RET(__wt_prepared_discover_filter_apply_handles(session));
    WT_RET(__cursor_prepared_discover_list_create(session, cursor_prepare));
    return (0);
}

/*
 * __cursor_prepared_discover_list_create --
 *     Review the pending prepared transactions in the transaction global list and create a list of
 *     transactions for this cursor to traverse through. The cursor could just use that list
 *     directly, but the level of indirection feels as though it will be helpful.
 */
static int
__cursor_prepared_discover_list_create(
  WT_SESSION_IMPL *session, WT_CURSOR_PREPARE_DISCOVERED *cursor_prepare)
{
    WT_TXN_GLOBAL *txn_global;
    uint32_t i, prepared_discovered_count;

    txn_global = &S2C(session)->txn_global;

    /* If no transactions were discovered, there is nothing more to do here */
    if (txn_global->pending_prepared_sessions == NULL)
        return (0);

    for (prepared_discovered_count = 0;
         txn_global->pending_prepared_sessions[prepared_discovered_count] != NULL;
         ++prepared_discovered_count)
        ;

    /* Leave a NULL at the end to mark the end of the list. */
    WT_RET(__wt_realloc_def(session, &cursor_prepare->list_allocated, prepared_discovered_count + 1,
      &cursor_prepare->list));
    for (i = 0; i < prepared_discovered_count; i++)
        cursor_prepare->list[i] = txn_global->pending_prepared_sessions[i]->txn->prepared_id;

    return (0);
}
