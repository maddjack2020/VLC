/**
 * @file rtp.c
 * @brief Real-Time Protocol (RTP) demux module for VLC media player
 */
/*****************************************************************************
 * Copyright (C) 2001-2005 VLC authors and VideoLAN
 * Copyright © 2007-2009 Rémi Denis-Courmont
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2.1
 * of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 ****************************************************************************/

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif
#include <stdarg.h>
#include <assert.h>

#include <vlc_common.h>
#include <vlc_demux.h>
#include <vlc_network.h>
#include <vlc_plugin.h>
#include "vlc_dtls.h"
#include <vlc_modules.h> /* module_exists() */

#include "rtp.h"
#ifdef HAVE_SRTP
# include "srtp.h"
# include <gcrypt.h>
# include <vlc_gcrypt.h>
#endif
#include "sdp.h"
#include "input.h"

/*
 * TODO: so much stuff
 * - send RTCP-RR and RTCP-BYE
 * - multiple medias (need SDP parser, and RTCP-SR parser for lip-sync)
 * - support for stream_filter in case of chained demux (MPEG-TS)
 */

#ifndef IPPROTO_DCCP
# define IPPROTO_DCCP 33 /* IANA */
#endif

#ifndef IPPROTO_UDPLITE
# define IPPROTO_UDPLITE 136 /* from IANA */
#endif

struct vlc_rtp_es_id {
    struct vlc_rtp_es es;
    es_out_t *out;
    es_out_id_t *id;
};

static void vlc_rtp_es_id_destroy(struct vlc_rtp_es *es)
{
    struct vlc_rtp_es_id *ei = container_of(es, struct vlc_rtp_es_id, es);

    es_out_Del(ei->out, ei->id);
    free(ei);
}

static void vlc_rtp_es_id_send(struct vlc_rtp_es *es, block_t *block)
{
    struct vlc_rtp_es_id *ei = container_of(es, struct vlc_rtp_es_id, es);

    /* TODO: Don't set PCR here. Breaks multiple sources (in a session)
     * and more importantly eventually multiple sessions. */
    if (block->i_pts != VLC_TICK_INVALID)
        es_out_SetPCR(ei->out, block->i_pts);
    es_out_Send(ei->out, ei->id, block);
}

static const struct vlc_rtp_es_operations vlc_rtp_es_id_ops = {
    vlc_rtp_es_id_destroy, vlc_rtp_es_id_send,
};

static struct vlc_rtp_es *vlc_rtp_es_request(struct vlc_rtp_pt *pt,
                                             const es_format_t *restrict fmt)
{
    demux_t *demux = pt->owner.data;

    struct vlc_rtp_es_id *ei = malloc(sizeof (*ei));
    if (unlikely(ei == NULL))
        return vlc_rtp_es_dummy;

    ei->es.ops = &vlc_rtp_es_id_ops;
    ei->out = demux->out;
    ei->id = es_out_Add(demux->out, fmt);
    if (ei->id == NULL) {
        free(ei);
        return vlc_rtp_es_dummy;
    }
    return &ei->es;
}

struct vlc_rtp_es_mux {
    struct vlc_rtp_es es;
    vlc_demux_chained_t *chained_demux;
};

static void vlc_rtp_es_mux_destroy(struct vlc_rtp_es *es)
{
    struct vlc_rtp_es_mux *em = container_of(es, struct vlc_rtp_es_mux, es);

    vlc_demux_chained_Delete(em->chained_demux);
    free(em);
}

static void vlc_rtp_es_mux_send(struct vlc_rtp_es *es, block_t *block)
{
    struct vlc_rtp_es_mux *em = container_of(es, struct vlc_rtp_es_mux, es);

    vlc_demux_chained_Send(em->chained_demux, block);
}

static const struct vlc_rtp_es_operations vlc_rtp_es_mux_ops = {
    vlc_rtp_es_mux_destroy, vlc_rtp_es_mux_send,
};

static struct vlc_rtp_es *vlc_rtp_mux_request(struct vlc_rtp_pt *pt,
                                              const char *name)
{
    demux_t *demux = pt->owner.data;

    struct vlc_rtp_es_mux *em = malloc(sizeof (*em));
    if (unlikely(em == NULL))
        return vlc_rtp_es_dummy;

    em->es.ops = &vlc_rtp_es_mux_ops;
    em->chained_demux = vlc_demux_chained_New(VLC_OBJECT(demux), name,
                                              demux->out);
    if (em->chained_demux == NULL) {
        free(em);
        return NULL;
    }
    return &em->es;
}

static const struct vlc_rtp_pt_owner_operations vlc_rtp_pt_owner_ops = {
    vlc_rtp_es_request, vlc_rtp_mux_request,
};

int vlc_rtp_pt_instantiate(vlc_object_t *obj, struct vlc_rtp_pt *restrict pt,
                           const struct vlc_sdp_pt *restrict desc)
{
    char modname[32];
    int ret = VLC_ENOTSUP;

    if (strchr(desc->name, ',') != NULL)
        /* Comma has special meaning in vlc_module_match(), forbid it */
        return VLC_EINVAL;
    if ((size_t)snprintf(modname, sizeof (modname), "%s/%s",
                         desc->media->type, desc->name) >= sizeof (modname))
        return VLC_ENOTSUP; /* Outlandish media type with long name */

    module_t **mods;
    ssize_t n = vlc_module_match("rtp parser", modname, true, &mods, NULL);

    for (ssize_t i = 0; i < n; i++) {
        vlc_rtp_parser_cb cb = vlc_module_map(vlc_object_logger(obj), mods[i]);
        if (cb == NULL)
            continue;

        ret = cb(obj, pt, desc);
        if (ret == VLC_SUCCESS) {
            msg_Dbg(obj, "- module \"%s\"", module_get_name(mods[i], true));
            assert(pt->ops != NULL);
            ret = 0;
            break;
        }
    }

    free(mods);
    return ret;
}

/**
 * Extracts port number from "[host]:port" or "host:port" strings,
 * and remove brackets from the host name.
 * @param phost pointer to the string upon entry,
 * pointer to the hostname upon return.
 * @return port number, 0 if missing.
 */
static int extract_port (char **phost)
{
    char *host = *phost, *port;

    if (host[0] == '[')
    {
        host = ++*phost; /* skip '[' */
        port = strchr (host, ']');
        if (port)
            *port++ = '\0'; /* skip ']' */
    }
    else
        port = strchr (host, ':');

    if (port == NULL)
        return 0;
    *port++ = '\0'; /* skip ':' */
    return atoi (port);
}

/**
 * Control callback
 */
static int Control (demux_t *demux, int query, va_list args)
{
    switch (query)
    {
        case DEMUX_GET_PTS_DELAY:
        {
            *va_arg (args, vlc_tick_t *) =
                VLC_TICK_FROM_MS( var_InheritInteger (demux, "network-caching") );
            return VLC_SUCCESS;
        }

        case DEMUX_CAN_PAUSE:
        case DEMUX_CAN_SEEK:
        case DEMUX_CAN_CONTROL_PACE:
        {
            bool *v = va_arg( args, bool * );
            *v = false;
            return VLC_SUCCESS;
        }
    }

    switch (query)
    {
        case DEMUX_GET_POSITION:
        {
            float *v = va_arg (args, float *);
            *v = 0.;
            return VLC_SUCCESS;
        }

        case DEMUX_GET_LENGTH:
        case DEMUX_GET_TIME:
        {
            *va_arg (args, vlc_tick_t *) = 0;
            return VLC_SUCCESS;
        }
    }

    return VLC_EGENERIC;
}

/**
 * Releases resources
 */
static void Close (vlc_object_t *obj)
{
    demux_t *demux = (demux_t *)obj;
    rtp_sys_t *p_sys = demux->p_sys;

    vlc_cancel(p_sys->thread);
    vlc_join(p_sys->thread, NULL);
#ifdef HAVE_SRTP
    if (p_sys->input_sys.srtp)
        srtp_destroy (p_sys->input_sys.srtp);
#endif
    rtp_session_destroy (obj->logger, p_sys->session);
    if (p_sys->input_sys.rtcp_sock != NULL)
        vlc_dtls_Close(p_sys->input_sys.rtcp_sock);
    vlc_dtls_Close(p_sys->input_sys.rtp_sock);
}

static int OpenSDP(vlc_object_t *obj)
{
    demux_t *demux = (demux_t *)obj;
    uint64_t size;
    const unsigned char *peek;

    assert(demux->out != NULL);

    if (vlc_stream_Peek(demux->s, &peek, 3) < 3 || memcmp(peek, "v=0", 3))
        return VLC_EGENERIC; /* not an SDP */

    if (vlc_stream_GetSize(demux->s, &size))
        size = 65536;
    else if (size > 65536) {
        msg_Err(obj, "SDP description too large: %" PRIu64 " bytes", size);
        return VLC_EGENERIC;
    }

    /* We must peek so that fallback to another plugin works. */
    ssize_t sdplen = vlc_stream_Peek(demux->s, &peek, size);
    if (sdplen < 0)
        return sdplen;

    rtp_sys_t *sys = vlc_obj_malloc(obj, sizeof (*sys));
    if (unlikely(sys == NULL))
        return VLC_ENOMEM;

    sys->input_sys.rtp_sock = NULL;
    sys->input_sys.rtcp_sock = NULL;
    sys->session = NULL;
#ifdef HAVE_SRTP
    sys->input_sys.srtp = NULL;
#endif

    struct vlc_sdp *sdp = vlc_sdp_parse((const char *)peek, sdplen);
    if (sdp == NULL) {
        msg_Err(obj, "SDP description parse error");
        return VLC_EGENERIC;
    }

    struct vlc_sdp_media *media = sdp->media;
    if (media == NULL || media->next != NULL) {
        msg_Dbg(obj, "only one SDP m= line supported");
        goto error;
    }

    if (vlc_sdp_media_attr_value(media, "control") != NULL
     || vlc_sdp_attr_value(sdp, "control") != NULL) {
        msg_Dbg(obj, "RTSP not supported");
        goto error;
    }

    struct vlc_sdp_conn *conn = media->conns;
    if (conn != NULL && conn->next != NULL) {
        msg_Dbg(obj, "only one SDP c= line supported");
        goto error;
    }

    if (conn == NULL)
        conn = sdp->conn;
    if (conn == NULL) {
        msg_Err(obj, "missing SDP c= line");
        goto error;
    }

    /* Determine destination port numbers */
    unsigned int rtp_port, rtcp_port;

    if (!vlc_sdp_media_attr_present(media, "rtcp-mux")) {
        const char *rtcp = vlc_sdp_media_attr_value(media, "rtcp");

        if (rtcp != NULL) {
            /* Explicit RTCP port */
            char *end;
            unsigned long x = strtoul(rtcp, &end, 10);

            if (*end || x == 0 || x > 65535) {
                msg_Err(obj, "invalid RTCP port specification %s", rtcp);
                goto error;
            }

            rtp_port = media->port;
            rtcp_port = x;
        } else {
            /* Implicit RTCP port (next odd) */
            rtp_port = (media->port + 1) & ~1;
            rtcp_port = media->port | 1;
        }
    } else {
        /* RTCP muxed on same port RTP */
        rtp_port = media->port;
        rtcp_port = 0;
    }

    /* TODO: support other protocols */
    if (strcmp(media->proto, "RTP/AVP") != 0) {
        msg_Dbg(obj, "unsupported protocol %s", media->proto);
        goto error;
    }

    /* Determine source address */
    char srcbuf[256], *src = NULL;
    const char *sfilter = vlc_sdp_media_attr_value(media, "source-filter");
    if (sfilter == NULL)
        sfilter = vlc_sdp_attr_value(sdp, "source-filter");
    /* FIXME: handle multiple source-filter attributes, match destination,
     * check IP version */
    if (sfilter != NULL
     && sscanf(sfilter, " incl IN IP%*1[46] %*s %255s", srcbuf) == 1)
        src = srcbuf;

    /* FIXME: enforce address family */
    int fd = net_OpenDgram(obj, conn->addr, rtp_port, src, 0, IPPROTO_UDP);
    if (fd == -1)
        goto error;

    sys->input_sys.rtp_sock = vlc_datagram_CreateFD(fd);
    if (unlikely(sys->input_sys.rtp_sock == NULL)) {
        net_Close(fd);
        goto error;
    }

    if (rtcp_port > 0) {
        fd = net_OpenDgram(obj, conn->addr, rtcp_port, src, 0, IPPROTO_UDP);
        if (fd == -1)
            goto error;

        sys->input_sys.rtcp_sock = vlc_datagram_CreateFD(fd);
        if (unlikely(sys->input_sys.rtcp_sock == NULL)) {
            net_Close(fd);
            goto error;
        }
    }

    sys->logger = obj->logger;

    demux->pf_demux = NULL;
    demux->pf_control = Control;
    demux->p_sys = sys;

    sys->session = rtp_session_create_custom(var_InheritInteger(obj, "rtp-max-dropout"),
                                             var_InheritInteger(obj, "rtp-max-misorder"),
                                             var_InheritInteger(obj, "rtp-max-src"),
                                             vlc_tick_from_sec(var_InheritInteger(obj, "rtp-timeout")));
    if (sys->session == NULL)
        goto error;

    /* Parse payload types */
    const struct vlc_rtp_pt_owner pt_owner = { &vlc_rtp_pt_owner_ops, demux };
    int err = vlc_rtp_add_media_types(obj, sys->session, media, &pt_owner);
    if (err < 0) {
        msg_Err(obj, "SDP description parse error");
        goto error;
    }
    if (err > 0 && module_exists("live555")) /* Bail out to live555 */
        goto error;

    if (vlc_clone(&sys->thread, rtp_dgram_thread, sys)) {
        rtp_session_destroy(obj->logger, sys->session);
        goto error;
    }

    vlc_sdp_free(sdp);
    return VLC_SUCCESS;

error:
    if (sys->input_sys.rtcp_sock != NULL)
        vlc_dtls_Close(sys->input_sys.rtcp_sock);
    if (sys->input_sys.rtp_sock != NULL)
        vlc_dtls_Close(sys->input_sys.rtp_sock);
    vlc_sdp_free(sdp);
    return VLC_EGENERIC;
}

/**
 * Probes and initializes.
 */
static int OpenURL(vlc_object_t *obj)
{
    demux_t *demux = (demux_t *)obj;
    int tp; /* transport protocol */

    if (demux->out == NULL)
        return VLC_EGENERIC;

    if (!strcasecmp(demux->psz_name, "dccp"))
        tp = IPPROTO_DCCP;
    else
    if (!strcasecmp(demux->psz_name, "rtp"))
        tp = IPPROTO_UDP;
    else
    if (!strcasecmp(demux->psz_name, "udplite"))
        tp = IPPROTO_UDPLITE;
    else
        return VLC_EGENERIC;

    rtp_sys_t *p_sys = vlc_obj_malloc(obj, sizeof (*p_sys));
    if (unlikely(p_sys == NULL))
        return VLC_ENOMEM;

    char *tmp = strdup (demux->psz_location);
    if (tmp == NULL)
        return VLC_ENOMEM;

    char *shost;
    char *dhost = strchr (tmp, '@');
    if (dhost != NULL)
    {
        *(dhost++) = '\0';
        shost = tmp;
    }
    else
    {
        dhost = tmp;
        shost = NULL;
    }

    /* Parses the port numbers */
    int sport = 0, dport = 0;
    if (shost != NULL)
        sport = extract_port (&shost);
    if (dhost != NULL)
        dport = extract_port (&dhost);
    if (dport == 0)
        dport = 5004; /* avt-profile-1 port */

    int rtcp_dport = var_CreateGetInteger (obj, "rtcp-port");

    /* Try to connect */
    int fd = -1, rtcp_fd = -1;
    bool co = false;

    switch (tp)
    {
        case IPPROTO_UDP:
        case IPPROTO_UDPLITE:
            fd = net_OpenDgram (obj, dhost, dport, shost, sport, tp);
            if (fd == -1)
                break;
            if (rtcp_dport > 0) /* XXX: source port is unknown */
                rtcp_fd = net_OpenDgram (obj, dhost, rtcp_dport, shost, 0, tp);
            break;

         case IPPROTO_DCCP:
#ifndef SOCK_DCCP /* provisional API (FIXME) */
# ifdef __linux__
#  define SOCK_DCCP 6
# endif
#endif
#ifdef SOCK_DCCP
            var_Create (obj, "dccp-service", VLC_VAR_STRING);
            var_SetString (obj, "dccp-service", "RTPV"); /* FIXME: RTPA? */
            fd = net_Connect (obj, dhost, dport, SOCK_DCCP, tp);
            co = true;
#else
            msg_Err (obj, "DCCP support not included");
#endif
            break;
    }

    free (tmp);

    if(fd == -1)
        return VLC_EGENERIC;

    p_sys->input_sys.rtp_sock = (co ? vlc_dccp_CreateFD : vlc_datagram_CreateFD)(fd);
    if (p_sys->input_sys.rtp_sock == NULL) {
        if (rtcp_fd != -1)
            net_Close(rtcp_fd);
        return VLC_EGENERIC;
    }
    net_SetCSCov (fd, -1, 12);

    if (rtcp_fd != -1) {
        p_sys->input_sys.rtcp_sock = vlc_datagram_CreateFD(rtcp_fd);
        if (p_sys->input_sys.rtcp_sock == NULL)
            net_Close (rtcp_fd);
    } else
        p_sys->input_sys.rtcp_sock = NULL;

#ifdef HAVE_SRTP
    p_sys->input_sys.srtp         = NULL;
#endif
    p_sys->logger       = obj->logger;

    demux->pf_demux   = NULL;
    demux->pf_control = Control;
    demux->p_sys      = p_sys;

    p_sys->session = rtp_session_create_custom(
                        var_InheritInteger(obj, "rtp-max-dropout"),
                        var_InheritInteger(obj, "rtp-max-misorder"),
                        var_InheritInteger(obj, "rtp-max-src"),
                        vlc_tick_from_sec(var_InheritInteger(obj, "rtp-timeout")) );
    if (p_sys->session == NULL)
        goto error;

    const struct vlc_rtp_pt_owner pt_owner = { &vlc_rtp_pt_owner_ops, demux };
    rtp_autodetect(VLC_OBJECT(demux), p_sys->session, &pt_owner);

#ifdef HAVE_SRTP
    char *key = var_CreateGetNonEmptyString (demux, "srtp-key");
    if (key)
    {
        vlc_gcrypt_init ();
        p_sys->input_sys.srtp = srtp_create (SRTP_ENCR_AES_CM, SRTP_AUTH_HMAC_SHA1, 10,
                                   SRTP_PRF_AES_CM, SRTP_RCC_MODE1);
        if (p_sys->input_sys.srtp == NULL)
        {
            free (key);
            goto error;
        }

        char *salt = var_CreateGetNonEmptyString (demux, "srtp-salt");
        int val = srtp_setkeystring (p_sys->input_sys.srtp, key, salt ? salt : "");
        free (salt);
        free (key);
        if (val)
        {
            msg_Err (obj, "bad SRTP key/salt combination (%s)",
                     vlc_strerror_c(val));
            goto error;
        }
    }
#endif

    if (vlc_clone (&p_sys->thread, rtp_dgram_thread, p_sys))
        goto error;
    return VLC_SUCCESS;

error:
#ifdef HAVE_SRTP
    if (p_sys->input_sys.srtp != NULL)
        srtp_destroy(p_sys->input_sys.srtp);
#endif
    if (p_sys->session != NULL)
        rtp_session_destroy(obj->logger, p_sys->session);
    if (p_sys->input_sys.rtcp_sock != NULL)
        vlc_dtls_Close(p_sys->input_sys.rtcp_sock);
    vlc_dtls_Close(p_sys->input_sys.rtp_sock);
    return VLC_EGENERIC;
}

#define RTCP_PORT_TEXT N_("RTCP (local) port")
#define RTCP_PORT_LONGTEXT N_( \
    "RTCP packets will be received on this transport protocol port. " \
    "If zero, multiplexed RTP/RTCP is used.")

#define SRTP_KEY_TEXT N_("SRTP key (hexadecimal)")
#define SRTP_KEY_LONGTEXT N_( \
    "RTP packets will be authenticated and deciphered "\
    "with this Secure RTP master shared secret key. "\
    "This must be a 32-character-long hexadecimal string.")

#define SRTP_SALT_TEXT N_("SRTP salt (hexadecimal)")
#define SRTP_SALT_LONGTEXT N_( \
    "Secure RTP requires a (non-secret) master salt value. " \
    "This must be a 28-character-long hexadecimal string.")

#define RTP_MAX_SRC_TEXT N_("Maximum RTP sources")
#define RTP_MAX_SRC_LONGTEXT N_( \
    "How many distinct active RTP sources are allowed at a time." )

#define RTP_TIMEOUT_TEXT N_("RTP source timeout (sec)")
#define RTP_TIMEOUT_LONGTEXT N_( \
    "How long to wait for any packet before a source is expired.")

#define RTP_MAX_DROPOUT_TEXT N_("Maximum RTP sequence number dropout")
#define RTP_MAX_DROPOUT_LONGTEXT N_( \
    "RTP packets will be discarded if they are too much ahead (i.e. in the " \
    "future) by this many packets from the last received packet." )

#define RTP_MAX_MISORDER_TEXT N_("Maximum RTP sequence number misordering")
#define RTP_MAX_MISORDER_LONGTEXT N_( \
    "RTP packets will be discarded if they are too far behind (i.e. in the " \
    "past) by this many packets from the last received packet." )

/*
 * Module descriptor
 */
vlc_module_begin()
    set_shortname(N_("RTP"))
    set_description(N_("Real-Time Protocol (RTP) input"))
    set_subcategory(SUBCAT_INPUT_DEMUX)
    set_capability("demux", 55)
    set_callbacks(OpenSDP, Close)

    add_submodule()
    set_capability("access", 0)
    set_callbacks(OpenURL, Close)

    add_integer("rtcp-port", 0, RTCP_PORT_TEXT,
                 RTCP_PORT_LONGTEXT)
        change_integer_range(0, 65535)
        change_safe()
#ifdef HAVE_SRTP
    add_string ("srtp-key", "",
                SRTP_KEY_TEXT, SRTP_KEY_LONGTEXT)
        change_safe ()
    add_string("srtp-salt", "",
               SRTP_SALT_TEXT, SRTP_SALT_LONGTEXT)
        change_safe()
#endif
    add_integer("rtp-max-src", RTP_MAX_SRC_DEFAULT, RTP_MAX_SRC_TEXT,
                RTP_MAX_SRC_LONGTEXT)
        change_integer_range (1, 255)
    add_integer("rtp-timeout", RTP_MAX_TIMEOUT_DEFAULT, RTP_TIMEOUT_TEXT,
                RTP_TIMEOUT_LONGTEXT)
    add_integer("rtp-max-dropout", RTP_MAX_DROPOUT_DEFAULT, RTP_MAX_DROPOUT_TEXT,
                RTP_MAX_DROPOUT_LONGTEXT)
        change_integer_range (0, 32767)
    add_integer("rtp-max-misorder", RTP_MAX_MISORDER_DEFAULT, RTP_MAX_MISORDER_TEXT,
                RTP_MAX_MISORDER_LONGTEXT)
        change_integer_range (0, 32767)
    add_obsolete_string("rtp-dynamic-pt") /* since 4.0.0 */

    /*add_shortcut ("sctp")*/
    add_shortcut("dccp", "rtp", "udplite")
vlc_module_end()
