/* $OpenLDAP$ */
/* This work is part of OpenLDAP Software <http://www.openldap.org/>.
 *
 * Copyright 1998-2020 The OpenLDAP Foundation.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted only as authorized by the OpenLDAP
 * Public License.
 *
 * A copy of this license is available in the file LICENSE in the
 * top-level directory of the distribution or, alternatively, at
 * <http://www.OpenLDAP.org/license.html>.
 */

#include "portable.h"

#include <ac/socket.h>
#include <ac/errno.h>
#include <ac/string.h>
#include <ac/time.h>
#include <ac/unistd.h>

#include "lload.h"

#include "lutil.h"
#include "lutil_ldap.h"

#ifdef HAVE_CYRUS_SASL
static const sasl_callback_t client_callbacks[] = {
#ifdef SASL_CB_GETREALM
        { SASL_CB_GETREALM, NULL, NULL },
#endif
        { SASL_CB_USER, NULL, NULL },
        { SASL_CB_AUTHNAME, NULL, NULL },
        { SASL_CB_PASS, NULL, NULL },
        { SASL_CB_LIST_END, NULL, NULL }
};
#endif /* HAVE_CYRUS_SASL */

int
forward_response( LloadConnection *client, LloadOperation *op, BerElement *ber )
{
    BerElement *output;
    BerValue response, controls = BER_BVNULL;
    ber_int_t msgid;
    ber_tag_t tag, response_tag;
    ber_len_t len;

    CONNECTION_LOCK(client);
    if ( op->o_client_msgid ) {
        msgid = op->o_client_msgid;
    } else {
        assert( op->o_pin_id );
        msgid = op->o_saved_msgid;
        op->o_saved_msgid = 0;
    }
    CONNECTION_UNLOCK(client);

    response_tag = ber_skip_element( ber, &response );

    tag = ber_peek_tag( ber, &len );
    if ( tag == LDAP_TAG_CONTROLS ) {
        ber_skip_element( ber, &controls );
    }

    Debug( LDAP_DEBUG_TRACE, "forward_response: "
            "%s to client connid=%lu request msgid=%d\n",
            lload_msgtype2str( response_tag ), op->o_client_connid, msgid );

    ldap_pvt_thread_mutex_lock( &client->c_io_mutex );
    output = client->c_pendingber;
    if ( output == NULL && (output = ber_alloc()) == NULL ) {
        ber_free( ber, 1 );
        ldap_pvt_thread_mutex_unlock( &client->c_io_mutex );
        return -1;
    }
    client->c_pendingber = output;

    ber_printf( output, "t{titOtO}", LDAP_TAG_MESSAGE,
            LDAP_TAG_MSGID, msgid,
            response_tag, &response,
            LDAP_TAG_CONTROLS, BER_BV_OPTIONAL( &controls ) );

    ldap_pvt_thread_mutex_unlock( &client->c_io_mutex );

    ber_free( ber, 1 );
    connection_write_cb( -1, 0, client );
    return 0;
}

int
forward_final_response(
        LloadConnection *client,
        LloadOperation *op,
        BerElement *ber )
{
    int rc;

    Debug( LDAP_DEBUG_STATS, "forward_final_response: "
            "connid=%lu msgid=%d finishing up with a request for "
            "client connid=%lu\n",
            op->o_upstream_connid, op->o_upstream_msgid, op->o_client_connid );
    rc = forward_response( client, op, ber );
    CONNECTION_LOCK(op->o_upstream);
    op->o_res = LLOAD_OP_COMPLETED;
    if ( !op->o_pin_id || !op->o_upstream_refcnt-- ) {
        operation_destroy_from_upstream( op );
    }
    CONNECTION_UNLOCK(op->o_upstream);

    return rc;
}

static int
handle_unsolicited( LloadConnection *c, BerElement *ber )
{
    if ( c->c_state != LLOAD_C_PREPARING ) {
        c->c_state = LLOAD_C_CLOSING;
    }

    Debug( LDAP_DEBUG_STATS, "handle_unsolicited: "
            "teardown for upstream connection connid=%lu\n",
            c->c_connid );

    CONNECTION_DESTROY(c);
    ber_free( ber, 1 );

    return -1;
}

/*
 * Pull c->c_currentber from the connection and try to look up the operation on
 * the upstream.
 *
 * If it's a notice of disconnection, we won't find it and need to tear down
 * the connection and tell the clients, if we can't find the operation, ignore
 * the message (either client already disconnected/abandoned it or the upstream
 * is pulling our leg).
 *
 * Some responses need special handling:
 * - Bind response
 * - VC response where the client requested a Bind (both need to update the
 *   client's bind status)
 * - search entries/referrals and intermediate responses (will not trigger
 *   operation to be removed)
 *
 * If the worker pool is overloaded, we might be called directly from
 * the read callback, at that point, the connection hasn't been muted.
 *
 * TODO: when the client already has data pending on write, we should mute the
 * upstream.
 * - should record the BerElement on the Op and the Op on the client
 *
 * The following hold on entering any of the handlers:
 * - op->o_upstream_refcnt > 0
 * - op->o_upstream->c_refcnt > 0
 * - op->o_client->c_refcnt > 0
 */
static int
handle_one_response( LloadConnection *c )
{
    BerElement *ber;
    LloadOperation *op = NULL, needle = { .o_upstream_connid = c->c_connid };
    LloadOperationHandler handler = NULL;
    ber_tag_t tag;
    ber_len_t len;
    int rc = LDAP_SUCCESS;

    ber = c->c_currentber;
    c->c_currentber = NULL;

    tag = ber_get_int( ber, &needle.o_upstream_msgid );
    if ( tag != LDAP_TAG_MSGID ) {
        rc = -1;
        ber_free( ber, 1 );
        goto fail;
    }

    if ( needle.o_upstream_msgid == 0 ) {
        return handle_unsolicited( c, ber );
    } else if ( !( op = tavl_find(
                           c->c_ops, &needle, operation_upstream_cmp ) ) ) {
        /* Already abandoned, do nothing */
        ber_free( ber, 1 );
        return rc;
        /*
    } else if ( op->o_response_pending ) {
        c->c_pendingop = op;
        event_del( c->c_read_event );
    */
    } else {
        /*
        op->o_response_pending = ber;
        */

        tag = ber_peek_tag( ber, &len );
        switch ( tag ) {
            case LDAP_RES_SEARCH_ENTRY:
            case LDAP_RES_SEARCH_REFERENCE:
            case LDAP_RES_INTERMEDIATE:
                handler = forward_response;
                break;
            case LDAP_RES_BIND:
                handler = handle_bind_response;
                break;
            case LDAP_RES_EXTENDED:
                if ( op->o_tag == LDAP_REQ_BIND ) {
#ifdef LDAP_API_FEATURE_VERIFY_CREDENTIALS
                    if ( lload_features & LLOAD_FEATURE_VC ) {
                        handler = handle_vc_bind_response;
                    } else
#endif /* LDAP_API_FEATURE_VERIFY_CREDENTIALS */
                    {
                        handler = handle_whoami_response;
                    }
                }
                break;
        }
        if ( !handler ) {
            handler = forward_final_response;
        }
    }
    if ( op ) {
        op->o_last_response = slap_get_time();
        Debug( LDAP_DEBUG_STATS2, "handle_one_response: "
                "upstream connid=%lu, processing response for "
                "client connid=%lu, msgid=%d\n",
                c->c_connid, op->o_client_connid, op->o_client_msgid );
    } else {
        tag = ber_peek_tag( ber, &len );
        Debug( LDAP_DEBUG_STATS2, "handle_one_response: "
                "upstream connid=%lu, %s, msgid=%d not for a pending "
                "operation\n",
                c->c_connid, lload_msgtype2str( tag ),
                needle.o_upstream_msgid );
    }

    if ( handler ) {
        LloadConnection *client;

        op->o_upstream_refcnt++;
        CONNECTION_UNLOCK_INCREF(c);

        ldap_pvt_thread_mutex_lock( &op->o_link_mutex );
        client = op->o_client;
        if ( client ) {
            CONNECTION_LOCK(client);
            if ( client->c_live ) {
                op->o_client_refcnt++;
                CONNECTION_UNLOCK_INCREF(client);
            } else {
                CONNECTION_UNLOCK(client);
                client = NULL;
            }
        }
        ldap_pvt_thread_mutex_unlock( &op->o_link_mutex );

        if ( client ) {
            rc = handler( client, op, ber );
            CONNECTION_LOCK_DECREF(client);
            op->o_client_refcnt--;
            if ( !op->o_client_refcnt ) {
                operation_destroy_from_client( op );
            }
            CONNECTION_UNLOCK_OR_DESTROY(client);
        } else {
            ber_free( ber, 1 );
        }

        CONNECTION_LOCK_DECREF(c);
        op->o_upstream_refcnt--;
        if ( !client || !op->o_upstream_refcnt ) {
            operation_destroy_from_upstream( op );
        }
    } else {
        assert(0);
        ber_free( ber, 1 );
    }

fail:
    if ( rc ) {
        Debug( LDAP_DEBUG_STATS, "handle_one_response: "
                "error on processing a response (%s) on upstream connection "
                "connid=%ld, tag=%lx\n",
                lload_msgtype2str( tag ), c->c_connid, tag );
        CONNECTION_DESTROY(c);
    }
    /* We leave the connection locked */
    return rc;
}

#ifdef HAVE_CYRUS_SASL
static int
sasl_bind_step( LloadConnection *c, BerValue *scred, BerValue *ccred )
{
    LloadBackend *b = c->c_private;
    sasl_conn_t *ctx = c->c_sasl_authctx;
    sasl_interact_t *prompts = NULL;
    unsigned credlen;
    int rc = -1;

    if ( !ctx ) {
        const char *mech = NULL;
        void *ssl;

        if ( sasl_client_new( "ldap", b->b_host, NULL, NULL, client_callbacks,
                     0, &ctx ) != SASL_OK ) {
            goto done;
        }
        c->c_sasl_authctx = ctx;

        assert( c->c_sasl_defaults == NULL );
        c->c_sasl_defaults =
                lutil_sasl_defaults( NULL, bindconf.sb_saslmech.bv_val,
                        bindconf.sb_realm.bv_val, bindconf.sb_authcId.bv_val,
                        bindconf.sb_cred.bv_val, bindconf.sb_authzId.bv_val );

#ifdef HAVE_TLS
        /* Check for TLS */
        ssl = ldap_pvt_tls_sb_ctx( c->c_sb );
        if ( ssl ) {
            struct berval authid = BER_BVNULL;
            ber_len_t ssf;

            ssf = ldap_pvt_tls_get_strength( ssl );
            (void)ldap_pvt_tls_get_my_dn( ssl, &authid, NULL, 0 );

            sasl_setprop( ctx, SASL_SSF_EXTERNAL, &ssf );
            sasl_setprop( ctx, SASL_AUTH_EXTERNAL, authid.bv_val );
            ch_free( authid.bv_val );
#ifdef SASL_CHANNEL_BINDING /* 2.1.25+ */
            {
                char cbinding[64];
                struct berval cbv = { sizeof(cbinding), cbinding };
                if ( ldap_pvt_tls_get_unique( ssl, &cbv, 0 ) ) {
                    sasl_channel_binding_t *cb =
                            ch_malloc( sizeof(*cb) + cbv.bv_len );
                    void *cb_data;
                    cb->name = "ldap";
                    cb->critical = 0;
                    cb->len = cbv.bv_len;
                    cb->data = cb_data = cb + 1;
                    memcpy( cb_data, cbv.bv_val, cbv.bv_len );
                    sasl_setprop( ctx, SASL_CHANNEL_BINDING, cb );
                }
            }
#endif
        }
#endif

#if !defined(_WIN32)
        /* Check for local */
        if ( b->b_proto == LDAP_PROTO_IPC ) {
            char authid[sizeof( "gidNumber=4294967295+uidNumber=4294967295,"
                                "cn=peercred,cn=external,cn=auth" )];
            int ssf = LDAP_PVT_SASL_LOCAL_SSF;

            sprintf( authid,
                    "gidNumber=%u+uidNumber=%u,"
                    "cn=peercred,cn=external,cn=auth",
                    getegid(), geteuid() );
            sasl_setprop( ctx, SASL_SSF_EXTERNAL, &ssf );
            sasl_setprop( ctx, SASL_AUTH_EXTERNAL, authid );
        }
#endif

        do {
            rc = sasl_client_start( ctx, bindconf.sb_saslmech.bv_val,
                    &prompts,
                    (const char **)&ccred->bv_val, &credlen,
                    &mech );

            if ( rc == SASL_INTERACT ) {
                if ( lutil_sasl_interact( NULL, LDAP_SASL_QUIET,
                             c->c_sasl_defaults, prompts ) ) {
                    break;
                }
            }
        } while ( rc == SASL_INTERACT );

        ber_str2bv( mech, 0, 0, &c->c_sasl_bind_mech );
    } else {
        assert( c->c_sasl_defaults );

        do {
            rc = sasl_client_step( ctx,
                    (scred == NULL) ? NULL : scred->bv_val,
                    (scred == NULL) ? 0 : scred->bv_len,
                    &prompts,
                    (const char **)&ccred->bv_val, &credlen);

            if ( rc == SASL_INTERACT ) {
                if ( lutil_sasl_interact( NULL, LDAP_SASL_QUIET,
                             c->c_sasl_defaults, prompts ) ) {
                    break;
                }
            }
        } while ( rc == SASL_INTERACT );
    }

    if ( rc == SASL_OK ) {
        sasl_ssf_t *ssf;
        rc = sasl_getprop( ctx, SASL_SSF, (const void **)(char *)&ssf );
        if ( rc == SASL_OK && ssf && *ssf ) {
            Debug( LDAP_DEBUG_CONNS, "sasl_bind_step: "
                    "connid=%lu mech=%s setting up a new SASL security layer\n",
                    c->c_connid, c->c_sasl_bind_mech.bv_val );
            ldap_pvt_sasl_install( c->c_sb, ctx );
        }
    }
    ccred->bv_len = credlen;

done:
    Debug( LDAP_DEBUG_TRACE, "sasl_bind_step: "
            "connid=%lu next step for SASL bind mech=%s rc=%d\n",
            c->c_connid, c->c_sasl_bind_mech.bv_val, rc );
    return rc;
}
#endif /* HAVE_CYRUS_SASL */

int
upstream_bind_cb( LloadConnection *c )
{
    BerElement *ber = c->c_currentber;
    LloadBackend *b = c->c_private;
    BerValue matcheddn, message;
    ber_tag_t tag;
    ber_int_t msgid, result;

    c->c_currentber = NULL;

    if ( ber_scanf( ber, "it", &msgid, &tag ) == LBER_ERROR ) {
        Debug( LDAP_DEBUG_ANY, "upstream_bind_cb: "
                "protocol violation from server\n" );
        goto fail;
    }

    if ( msgid != ( c->c_next_msgid - 1 ) || tag != LDAP_RES_BIND ) {
        Debug( LDAP_DEBUG_ANY, "upstream_bind_cb: "
                "unexpected %s from server, msgid=%d\n",
                lload_msgtype2str( tag ), msgid );
        goto fail;
    }

    if ( ber_scanf( ber, "{emm" /* "}" */, &result, &matcheddn, &message ) ==
                 LBER_ERROR ) {
        Debug( LDAP_DEBUG_ANY, "upstream_bind_cb: "
                "response does not conform with a bind response\n" );
        goto fail;
    }

    switch ( result ) {
        case LDAP_SUCCESS:
#ifdef HAVE_CYRUS_SASL
        case LDAP_SASL_BIND_IN_PROGRESS:
            if ( !BER_BVISNULL( &c->c_sasl_bind_mech ) ) {
                BerValue scred = BER_BVNULL, ccred;
                ber_len_t len;
                int rc;

                CONNECTION_UNLOCK_INCREF(c);
                if ( ber_peek_tag( ber, &len ) == LDAP_TAG_SASL_RES_CREDS &&
                        ber_scanf( ber, "m", &scred ) == LBER_ERROR ) {
                    Debug( LDAP_DEBUG_ANY, "upstream_bind_cb: "
                            "sasl bind response malformed\n" );
                    CONNECTION_LOCK_DECREF(c);
                    goto fail;
                }

                rc = sasl_bind_step( c, &scred, &ccred );
                if ( rc != SASL_OK &&
                        ( rc != SASL_CONTINUE || result == LDAP_SUCCESS ) ) {
                    CONNECTION_LOCK_DECREF(c);
                    goto fail;
                }

                if ( result == LDAP_SASL_BIND_IN_PROGRESS ) {
                    BerElement *outber;

                    ldap_pvt_thread_mutex_lock( &c->c_io_mutex );
                    outber = c->c_pendingber;
                    if ( outber == NULL && (outber = ber_alloc()) == NULL ) {
                        ldap_pvt_thread_mutex_unlock( &c->c_io_mutex );
                        CONNECTION_LOCK_DECREF(c);
                        goto fail;
                    }
                    c->c_pendingber = outber;

                    msgid = c->c_next_msgid++;
                    ber_printf( outber, "{it{iOt{OON}N}}",
                            msgid, LDAP_REQ_BIND, LDAP_VERSION3,
                            &bindconf.sb_binddn, LDAP_AUTH_SASL,
                            &c->c_sasl_bind_mech, BER_BV_OPTIONAL( &ccred ) );
                    ldap_pvt_thread_mutex_unlock( &c->c_io_mutex );

                    connection_write_cb( -1, 0, c );

                    CONNECTION_LOCK_DECREF(c);
                    if ( rc == SASL_OK ) {
                        BER_BVZERO( &c->c_sasl_bind_mech );
                    }
                    break;
                }
                CONNECTION_LOCK_DECREF(c);
            }
            if ( result == LDAP_SASL_BIND_IN_PROGRESS ) {
                goto fail;
            }
#endif /* HAVE_CYRUS_SASL */
            c->c_pdu_cb = handle_one_response;
            c->c_state = LLOAD_C_READY;
            c->c_type = LLOAD_C_OPEN;
            c->c_read_timeout = NULL;
            event_add( c->c_read_event, c->c_read_timeout );
            Debug( LDAP_DEBUG_CONNS, "upstream_bind_cb: "
                    "connid=%lu finished binding, now active\n",
                    c->c_connid );
            CONNECTION_UNLOCK_INCREF(c);
            ldap_pvt_thread_mutex_lock( &b->b_mutex );
            LDAP_CIRCLEQ_REMOVE( &b->b_preparing, c, c_next );
            b->b_active++;
            b->b_opening--;
            b->b_failed = 0;
            if ( b->b_last_conn ) {
                LDAP_CIRCLEQ_INSERT_AFTER(
                        &b->b_conns, b->b_last_conn, c, c_next );
            } else {
                LDAP_CIRCLEQ_INSERT_HEAD( &b->b_conns, c, c_next );
            }
            b->b_last_conn = c;
            backend_retry( b );
            ldap_pvt_thread_mutex_unlock( &b->b_mutex );
            CONNECTION_LOCK_DECREF(c);
            break;
        default:
            Debug( LDAP_DEBUG_ANY, "upstream_bind_cb: "
                    "upstream bind failed, rc=%d, message='%s'\n",
                    result, message.bv_val );
            goto fail;
    }

    ber_free( ber, 1 );
    return LDAP_SUCCESS;

fail:
    ber_free( ber, 1 );
    CONNECTION_DESTROY(c);
    return -1;
}

void *
upstream_bind( void *ctx, void *arg )
{
    LloadConnection *c = arg;
    BerElement *ber;
    ber_int_t msgid;

    CONNECTION_LOCK_DECREF(c);
    c->c_pdu_cb = upstream_bind_cb;
    CONNECTION_UNLOCK_INCREF(c);

    ldap_pvt_thread_mutex_lock( &c->c_io_mutex );
    ber = c->c_pendingber;
    if ( ber == NULL && (ber = ber_alloc()) == NULL ) {
        goto fail;
    }
    c->c_pendingber = ber;
    msgid = c->c_next_msgid++;

    if ( bindconf.sb_method == LDAP_AUTH_SIMPLE ) {
        /* simple bind */
        ber_printf( ber, "{it{iOtON}}",
                msgid, LDAP_REQ_BIND, LDAP_VERSION3,
                &bindconf.sb_binddn, LDAP_AUTH_SIMPLE,
                &bindconf.sb_cred );

#ifdef HAVE_CYRUS_SASL
    } else {
        BerValue cred;
        int rc;

        rc = sasl_bind_step( c, NULL, &cred );
        if ( rc != SASL_OK && rc != SASL_CONTINUE ) {
            goto fail;
        }

        ber_printf( ber, "{it{iOt{OON}N}}",
                msgid, LDAP_REQ_BIND, LDAP_VERSION3,
                &bindconf.sb_binddn, LDAP_AUTH_SASL,
                &c->c_sasl_bind_mech, BER_BV_OPTIONAL( &cred ) );

        if ( rc == SASL_OK ) {
            BER_BVZERO( &c->c_sasl_bind_mech );
        }
#endif /* HAVE_CYRUS_SASL */
    }
    ldap_pvt_thread_mutex_unlock( &c->c_io_mutex );

    connection_write_cb( -1, 0, c );

    CONNECTION_LOCK_DECREF(c);
    c->c_read_timeout = lload_timeout_net;
    event_add( c->c_read_event, c->c_read_timeout );
    CONNECTION_UNLOCK_OR_DESTROY(c);

    return NULL;

fail:
    ldap_pvt_thread_mutex_unlock( &c->c_io_mutex );
    CONNECTION_LOCK_DESTROY(c);
    return NULL;
}

/*
 * The backend is already locked when entering the function.
 */
static int
upstream_finish( LloadConnection *c )
{
    LloadBackend *b = c->c_private;
    int is_bindconn = 0;

    c->c_pdu_cb = handle_one_response;

    /* Unless we are configured to use the VC exop, consider allocating the
     * connection into the bind conn pool. Start off by allocating one for
     * general use, then one for binds, then we start filling up the general
     * connection pool, finally the bind pool */
    if (
#ifdef LDAP_API_FEATURE_VERIFY_CREDENTIALS
            !(lload_features & LLOAD_FEATURE_VC) &&
#endif /* LDAP_API_FEATURE_VERIFY_CREDENTIALS */
            b->b_active && b->b_numbindconns ) {
        if ( !b->b_bindavail ) {
            is_bindconn = 1;
        } else if ( b->b_active >= b->b_numconns &&
                b->b_bindavail < b->b_numbindconns ) {
            is_bindconn = 1;
        }
    }

    if ( is_bindconn ) {
        LDAP_CIRCLEQ_REMOVE( &b->b_preparing, c, c_next );
        c->c_state = LLOAD_C_READY;
        c->c_type = LLOAD_C_BIND;
        b->b_bindavail++;
        b->b_opening--;
        b->b_failed = 0;
        if ( b->b_last_bindconn ) {
            LDAP_CIRCLEQ_INSERT_AFTER(
                    &b->b_bindconns, b->b_last_bindconn, c, c_next );
        } else {
            LDAP_CIRCLEQ_INSERT_HEAD( &b->b_bindconns, c, c_next );
        }
        b->b_last_bindconn = c;
    } else if ( bindconf.sb_method == LDAP_AUTH_NONE ) {
        LDAP_CIRCLEQ_REMOVE( &b->b_preparing, c, c_next );
        c->c_state = LLOAD_C_READY;
        c->c_type = LLOAD_C_OPEN;
        b->b_active++;
        b->b_opening--;
        b->b_failed = 0;
        if ( b->b_last_conn ) {
            LDAP_CIRCLEQ_INSERT_AFTER( &b->b_conns, b->b_last_conn, c, c_next );
        } else {
            LDAP_CIRCLEQ_INSERT_HEAD( &b->b_conns, c, c_next );
        }
        b->b_last_conn = c;
    } else {
        if ( ldap_pvt_thread_pool_submit(
                     &connection_pool, upstream_bind, c ) ) {
            Debug( LDAP_DEBUG_ANY, "upstream_finish: "
                    "failed to set up a bind callback for connid=%lu\n",
                    c->c_connid );
            return -1;
        }
        c->c_refcnt++;

        Debug( LDAP_DEBUG_CONNS, "upstream_finish: "
                "scheduled a bind callback for connid=%lu\n",
                c->c_connid );
        return LDAP_SUCCESS;
    }

    Debug( LDAP_DEBUG_CONNS, "upstream_finish: "
            "%sconnection connid=%lu for backend server '%s' is ready for "
            "use\n",
            is_bindconn ? "bind " : "", c->c_connid, b->b_name.bv_val );

    backend_retry( b );
    return LDAP_SUCCESS;
}

static void
upstream_tls_handshake_cb( evutil_socket_t s, short what, void *arg )
{
    LloadConnection *c = arg;
    LloadBackend *b;
    int rc = LDAP_SUCCESS;

    CONNECTION_LOCK(c);
    if ( what & EV_TIMEOUT ) {
        Debug( LDAP_DEBUG_CONNS, "upstream_tls_handshake_cb: "
                "connid=%lu, timeout reached, destroying\n",
                c->c_connid );
        goto fail;
    }
    b = c->c_private;

    rc = ldap_pvt_tls_connect( lload_tls_backend_ld, c->c_sb, b->b_host );
    if ( rc < 0 ) {
        goto fail;
    }

    if ( rc == 0 ) {
        struct event_base *base = event_get_base( c->c_read_event );

        /*
         * We're finished, replace the callbacks
         *
         * This is deadlock-safe, since both share the same base - the one
         * that's just running us.
         */
        event_del( c->c_read_event );
        event_del( c->c_write_event );

        c->c_read_timeout = NULL;
        event_assign( c->c_read_event, base, c->c_fd, EV_READ|EV_PERSIST,
                connection_read_cb, c );
        event_add( c->c_read_event, c->c_read_timeout );

        event_assign( c->c_write_event, base, c->c_fd, EV_WRITE,
                connection_write_cb, c );
        Debug( LDAP_DEBUG_CONNS, "upstream_tls_handshake_cb: "
                "connid=%lu finished\n",
                c->c_connid );
        c->c_is_tls = LLOAD_TLS_ESTABLISHED;

        CONNECTION_UNLOCK_INCREF(c);
        ldap_pvt_thread_mutex_lock( &b->b_mutex );
        CONNECTION_LOCK_DECREF(c);

        rc = upstream_finish( c );
        ldap_pvt_thread_mutex_unlock( &b->b_mutex );

        if ( rc ) {
            goto fail;
        }
    } else if ( ber_sockbuf_ctrl( c->c_sb, LBER_SB_OPT_NEEDS_WRITE, NULL ) ) {
        event_add( c->c_write_event, lload_write_timeout );
        Debug( LDAP_DEBUG_CONNS, "upstream_tls_handshake_cb: "
                "connid=%lu need write rc=%d\n",
                c->c_connid, rc );
    }
    CONNECTION_UNLOCK_OR_DESTROY(c);
    return;

fail:
    Debug( LDAP_DEBUG_CONNS, "upstream_tls_handshake_cb: "
            "connid=%lu failed rc=%d\n",
            c->c_connid, rc );
    CONNECTION_DESTROY(c);
}

static int
upstream_starttls( LloadConnection *c )
{
    BerValue matcheddn, message, responseOid,
             startTLSOid = BER_BVC(LDAP_EXOP_START_TLS);
    BerElement *ber = c->c_currentber;
    struct event_base *base;
    ber_int_t msgid, result;
    ber_tag_t tag;

    c->c_currentber = NULL;

    if ( ber_scanf( ber, "it", &msgid, &tag ) == LBER_ERROR ) {
        Debug( LDAP_DEBUG_ANY, "upstream_starttls: "
                "protocol violation from server\n" );
        goto fail;
    }

    if ( msgid != ( c->c_next_msgid - 1 ) || tag != LDAP_RES_EXTENDED ) {
        Debug( LDAP_DEBUG_ANY, "upstream_starttls: "
                "unexpected %s from server, msgid=%d\n",
                lload_msgtype2str( tag ), msgid );
        goto fail;
    }

    if ( ber_scanf( ber, "{emm}", &result, &matcheddn, &message ) ==
                 LBER_ERROR ) {
        Debug( LDAP_DEBUG_ANY, "upstream_starttls: "
                "protocol violation on StartTLS response\n" );
        goto fail;
    }

    if ( (tag = ber_get_tag( ber )) != LBER_DEFAULT ) {
        if ( tag != LDAP_TAG_EXOP_RES_OID ||
                ber_scanf( ber, "{m}", &responseOid ) == LBER_DEFAULT ) {
            Debug( LDAP_DEBUG_ANY, "upstream_starttls: "
                    "protocol violation on StartTLS response\n" );
            goto fail;
        }

        if ( ber_bvcmp( &responseOid, &startTLSOid ) ) {
            Debug( LDAP_DEBUG_ANY, "upstream_starttls: "
                    "oid=%s not a StartTLS response\n",
                    responseOid.bv_val );
            goto fail;
        }
    }

    if ( result != LDAP_SUCCESS ) {
        LloadBackend *b = c->c_private;
        int rc;

        Debug( LDAP_DEBUG_STATS, "upstream_starttls: "
                "server doesn't support StartTLS rc=%d message='%s'%s\n",
                result, message.bv_val,
                (c->c_is_tls == LLOAD_STARTTLS_OPTIONAL) ? ", ignored" : "" );
        if ( c->c_is_tls != LLOAD_STARTTLS_OPTIONAL ) {
            goto fail;
        }
        c->c_is_tls = LLOAD_CLEARTEXT;

        CONNECTION_UNLOCK_INCREF(c);
        ldap_pvt_thread_mutex_lock( &b->b_mutex );
        CONNECTION_LOCK_DECREF(c);

        rc = upstream_finish( c );
        ldap_pvt_thread_mutex_unlock( &b->b_mutex );

        if ( rc ) {
            goto fail;
        }

        ber_free( ber, 1 );
        CONNECTION_UNLOCK_OR_DESTROY(c);

        return rc;
    }

    base = event_get_base( c->c_read_event );

    event_del( c->c_read_event );
    event_del( c->c_write_event );

    c->c_read_timeout = lload_timeout_net;
    event_assign( c->c_read_event, base, c->c_fd, EV_READ|EV_PERSIST,
            upstream_tls_handshake_cb, c );
    event_assign( c->c_write_event, base, c->c_fd, EV_WRITE,
            upstream_tls_handshake_cb, c );

    event_add( c->c_read_event, c->c_read_timeout );
    event_add( c->c_write_event, lload_write_timeout );

    CONNECTION_UNLOCK(c);

    ber_free( ber, 1 );
    return -1;

fail:
    ber_free( ber, 1 );
    CONNECTION_DESTROY(c);
    return -1;
}

/*
 * We must already hold b->b_mutex when called.
 */
LloadConnection *
upstream_init( ber_socket_t s, LloadBackend *b )
{
    LloadConnection *c;
    struct event_base *base = lload_get_base( s );
    struct event *event;
    int flags;

    assert( b != NULL );

    flags = (b->b_proto == LDAP_PROTO_IPC) ? CONN_IS_IPC : 0;
    if ( (c = lload_connection_init( s, b->b_host, flags )) == NULL ) {
        return NULL;
    }

    c->c_private = b;
    c->c_is_tls = b->b_tls;
    c->c_pdu_cb = handle_one_response;

    LDAP_CIRCLEQ_INSERT_HEAD( &b->b_preparing, c, c_next );
    c->c_type = LLOAD_C_PREPARING;

    {
        ber_len_t max = sockbuf_max_incoming_upstream;
        ber_sockbuf_ctrl( c->c_sb, LBER_SB_OPT_SET_MAX_INCOMING, &max );
    }

    event = event_new( base, s, EV_READ|EV_PERSIST, connection_read_cb, c );
    if ( !event ) {
        Debug( LDAP_DEBUG_ANY, "upstream_init: "
                "Read event could not be allocated\n" );
        goto fail;
    }
    c->c_read_event = event;

    event = event_new( base, s, EV_WRITE, connection_write_cb, c );
    if ( !event ) {
        Debug( LDAP_DEBUG_ANY, "upstream_init: "
                "Write event could not be allocated\n" );
        goto fail;
    }
    /* We only add the write event when we have data pending */
    c->c_write_event = event;

    if ( c->c_is_tls == LLOAD_CLEARTEXT ) {
        if ( upstream_finish( c ) ) {
            goto fail;
        }
    } else if ( c->c_is_tls == LLOAD_LDAPS ) {
        event_assign( c->c_read_event, base, s, EV_READ|EV_PERSIST,
                upstream_tls_handshake_cb, c );
        event_assign( c->c_write_event, base, s, EV_WRITE,
                upstream_tls_handshake_cb, c );
        event_add( c->c_write_event, lload_write_timeout );
    } else if ( c->c_is_tls == LLOAD_STARTTLS ||
            c->c_is_tls == LLOAD_STARTTLS_OPTIONAL ) {
        BerElement *output;

        ldap_pvt_thread_mutex_lock( &c->c_io_mutex );
        if ( (output = c->c_pendingber = ber_alloc()) == NULL ) {
            ldap_pvt_thread_mutex_unlock( &c->c_io_mutex );
            goto fail;
        }
        ber_printf( output, "t{tit{ts}}", LDAP_TAG_MESSAGE,
                LDAP_TAG_MSGID, c->c_next_msgid++,
                LDAP_REQ_EXTENDED,
                LDAP_TAG_EXOP_REQ_OID, LDAP_EXOP_START_TLS );
        ldap_pvt_thread_mutex_unlock( &c->c_io_mutex );

        c->c_pdu_cb = upstream_starttls;
        CONNECTION_UNLOCK_INCREF(c);
        connection_write_cb( s, 0, c );
        CONNECTION_LOCK_DECREF(c);
    }
    event_add( c->c_read_event, c->c_read_timeout );

    c->c_destroy = upstream_destroy;
    CONNECTION_UNLOCK_OR_DESTROY(c);

    return c;

fail:
    if ( c->c_write_event ) {
        event_del( c->c_write_event );
        event_free( c->c_write_event );
    }
    if ( c->c_read_event ) {
        event_del( c->c_read_event );
        event_free( c->c_read_event );
    }

    c->c_state = LLOAD_C_INVALID;
    CONNECTION_DESTROY(c);
    assert( c == NULL );

    return NULL;
}

void
upstream_destroy( LloadConnection *c )
{
    LloadBackend *b = c->c_private;
    struct event *read_event, *write_event;
    TAvlnode *root, *node;
    long freed, executing;
    enum sc_state state;

    Debug( LDAP_DEBUG_CONNS, "upstream_destroy: "
            "freeing connection connid=%lu\n",
            c->c_connid );

    assert( c->c_state != LLOAD_C_INVALID );
    state = c->c_state;
    c->c_state = LLOAD_C_INVALID;

    root = c->c_ops;
    c->c_ops = NULL;
    executing = c->c_n_ops_executing;
    c->c_n_ops_executing = 0;

    read_event = c->c_read_event;
    write_event = c->c_write_event;

    for ( node = tavl_end( root, TAVL_DIR_LEFT ); node;
            node = tavl_next( node, TAVL_DIR_RIGHT ) ) {
        LloadOperation *op = node->avl_data;

        op->o_res = LLOAD_OP_FAILED;
        op->o_upstream_refcnt++;
    }

    CONNECTION_UNLOCK_INCREF(c);

    freed = tavl_free( root, (AVL_FREE)operation_lost_upstream );
    assert( freed == executing );

    /*
     * Avoid a deadlock:
     * event_del will block if the event is currently executing its callback,
     * that callback might be waiting to lock c->c_mutex
     */
    if ( read_event ) {
        event_del( read_event );
    }

    if ( write_event ) {
        event_del( write_event );
    }

    /* Remove from the backend on first pass */
    if ( state != LLOAD_C_DYING ) {
        ldap_pvt_thread_mutex_lock( &b->b_mutex );
        if ( c->c_type == LLOAD_C_PREPARING ) {
            LDAP_CIRCLEQ_REMOVE( &b->b_preparing, c, c_next );
            b->b_opening--;
            b->b_failed++;
        } else if ( c->c_type == LLOAD_C_BIND ) {
            if ( c == b->b_last_bindconn ) {
                LloadConnection *prev =
                        LDAP_CIRCLEQ_LOOP_PREV( &b->b_bindconns, c, c_next );
                if ( prev == c ) {
                    b->b_last_bindconn = NULL;
                } else {
                    b->b_last_bindconn = prev;
                }
            }
            LDAP_CIRCLEQ_REMOVE( &b->b_bindconns, c, c_next );
            b->b_bindavail--;
        } else {
            if ( c == b->b_last_conn ) {
                LloadConnection *prev =
                        LDAP_CIRCLEQ_LOOP_PREV( &b->b_conns, c, c_next );
                if ( prev == c ) {
                    b->b_last_conn = NULL;
                } else {
                    b->b_last_conn = prev;
                }
            }
            LDAP_CIRCLEQ_REMOVE( &b->b_conns, c, c_next );
            b->b_active--;
        }
        b->b_n_ops_executing -= executing;
        backend_retry( b );
        ldap_pvt_thread_mutex_unlock( &b->b_mutex );
    }

    CONNECTION_LOCK_DECREF(c);

    if ( c->c_read_event ) {
        event_free( c->c_read_event );
        c->c_read_event = NULL;
    }

    if ( c->c_write_event ) {
        event_free( c->c_write_event );
        c->c_write_event = NULL;
    }

    /*
     * If we attempted to destroy any operations, we might have lent a new
     * refcnt token for a thread that raced us to that, let them call us again
     * later
     */
    assert( c->c_refcnt >= 0 );
    if ( c->c_refcnt ) {
        c->c_state = LLOAD_C_DYING;
        Debug( LDAP_DEBUG_CONNS, "upstream_destroy: "
                "connid=%lu aborting with refcnt=%d\n",
                c->c_connid, c->c_refcnt );
        CONNECTION_UNLOCK(c);
        return;
    }

    BER_BVZERO( &c->c_sasl_bind_mech );
    connection_destroy( c );
}
