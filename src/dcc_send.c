/* dircproxy
 * Copyright (C) 2000 Scott James Remnant <scott@netsplit.com>.
 * All Rights Reserved.
 *
 * dcc_send.c
 *  - DCC send protocol
 * --
 * @(#) $Id: dcc_send.c,v 1.6 2000/12/07 17:17:06 keybuk Exp $
 *
 * This file is distributed according to the GNU General Public
 * License.  For full details, read the top of 'main.c' or the
 * file called COPYING that was distributed with this code.
 */

#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>

#include <dircproxy.h>
#include "sprintf.h"
#include "net.h"
#include "dns.h"
#include "timers.h"
#include "dcc_net.h"
#include "dcc_send.h"

/* forward declarations */
static void _dccsend_data(struct dccproxy *, int);
static void _dccsend_error(struct dccproxy *, int, int);
static int _dccsend_sendpacket(struct dccproxy *);

/* Called when we've connected to the sender */
void dccsend_connected(struct dccproxy *p, int sock) {
  if (sock != p->sender_sock) {
    error("Unexpected socket %d in dccsend_connected, expected %d", sock,
          p->sender_sock);
    net_close(sock);
    return;
  }

  debug("DCC Connection succeeded");
  p->sender_status |= DCC_SENDER_CONNECTED;
  net_hook(p->sender_sock, SOCK_NORMAL, (void *)p,
           ACTIVITY_FUNCTION(_dccsend_data),
           ERROR_FUNCTION(_dccsend_error));
}

/* Called when a connection fails */
void dccsend_connectfailed(struct dccproxy *p, int sock, int bad) {
  if (sock != p->sender_sock) {
    error("Unexpected socket %d in dccsend_connectfailed, expected %d", sock,
          p->sender_sock);
    net_close(sock);
    return;
  }

  debug("DCC Connection failed");
  p->sender_status &= ~(DCC_SENDER_CREATED);
  net_close(p->sender_sock);
  p->sender_sock = -1;
  p->dead = 1;
}

/* Called when the sendee has been accepted */
void dccsend_accepted(struct dccproxy *p) {
  net_hook(p->sendee_sock, SOCK_NORMAL, (void *)p,
           ACTIVITY_FUNCTION(_dccsend_data),
           ERROR_FUNCTION(_dccsend_error));

  /* If we've already got data, we better some */
  if (p->bufsz)
    _dccsend_sendpacket(p);
}

/* Called when we get data over a DCC link */
static void _dccsend_data(struct dccproxy *p, int sock) {
  if (sock == p->sender_sock) {
    int buflen, nr;

    /* Read the data into the buffer */
    buflen = net_read(p->sender_sock, 0, 0);
    p->buf = (char *)realloc(p->buf, p->bufsz + buflen);
    nr = net_read(p->sender_sock, (void *)(p->buf + p->bufsz), buflen);

    /* Check we read some */
    if (nr > 0) {
      uint32_t na;

      p->bufsz += nr;
      p->bytes_rcvd += nr;

      debug("Accepted %d bytes from sender (tot: %lu  buf: %lu)",
            nr, p->bytes_rcvd, p->bufsz);

      /* Acknowledge them */
      na = htonl(p->bytes_rcvd);
      net_queue(p->sender_sock, (void *)&na, sizeof(uint32_t));
    } else {
      p->buf = (char *)realloc(p->buf, p->bufsz);
    }

  } else if (sock == p->sendee_sock) {
    uint32_t ack;
    int len;

    /* We should only ever get ack's back */
    len = net_read(p->sendee_sock, (void *)&ack, sizeof(uint32_t));
    if (len == sizeof(uint32_t))
      p->bytes_ackd = ntohl(ack);

  } else {
    error("Unexpected socket %d in dccsend_data, expected %d or %d", sock,
          p->sender_sock, p->sendee_sock);
    net_close(sock);
    return;
  }

  debug("rcvd: %lu  sent: %lu  ackd: %lu  (buf: %lu)",
        p->bytes_rcvd, p->bytes_sent, p->bytes_ackd, p->bufsz);

  /* Receiving data is as good as trigger as any to check whether we can send
     more. */
  if (p->bufsz && ((p->type & DCC_SEND_FAST) || (p->type & DCC_SEND_CAPTURE) || 
                   (p->bytes_ackd >= p->bytes_sent))) {
    /* Capturing?  Just eat the buffer right here, right now */
    if (p->type & DCC_SEND_CAPTURE) {
      /* Write it to the file */
      fwrite((void *)p->buf, 1, p->bufsz, p->cap_file);

      /* Sent the whole thing */
      p->bytes_sent += p->bufsz;
      p->bufsz = 0;
      free(p->buf);
      p->buf = 0;

      /* Check we haven't exceeded the maximum size */
      if (p->bytes_max && (p->bytes_sent >= p->bytes_max)) {
        /* We have, kill it.  It'll automatically get unlinked */
        debug("Too big for my boots!");
        p->dead = 1;
      }

    } else if (p->sendee_status == DCC_SENDEE_ACTIVE) {
      /* Send packet to the client */
      _dccsend_sendpacket(p);
    }
  }
}

/* Called on DCC disconnection or error */
static void _dccsend_error(struct dccproxy *p, int sock, int bad) {
  char *who;

  if (sock == p->sender_sock) {
    who = "Sender";
    p->sender_status &= ~(DCC_SENDER_CREATED);
    net_close(p->sender_sock);
    p->sender_sock = -1;

    /* Not necessarily bad, just means the client has gone */
    if (p->bufsz && !(p->type & DCC_SEND_CAPTURE)) {
      debug("%lu bytes remaining", p->bufsz);
      p->sender_status = DCC_SENDER_GONE;
    } else {
      p->dead = 1;
    }
  } else if (sock == p->sendee_sock) {
    who = "Sendee";
    p->sendee_status &= ~(DCC_SENDEE_CREATED);
    net_close(p->sendee_sock);
    p->sendee_sock = -1;
    p->dead = 1;
  } else {
    error("Unexpected socket %d in dccsend_error, expected %d or %d", sock,
          p->sender_sock, p->sendee_sock);
    net_close(sock);
    return;
  }

  if (bad) {
    debug("Socket error with %s", who);
  } else {
    debug("%s disconnected", who);

    /* Close the file nicely if we're capturing, so it doesn't get unlinked */
    if (p->type & DCC_SEND_CAPTURE) {
      debug("%s closed", p->cap_filename);
      free(p->cap_filename);
      fclose(p->cap_file);
      p->cap_filename = 0;
      p->cap_file = 0;
    }
  }
}

/* Send a packet of buffered data to the client */
static int _dccsend_sendpacket(struct dccproxy *p) {
  unsigned long nr;

  /* If we're doing simple sends, we limit the amount we send, if doing fast
     just shove the whole lot to them */
  if (p->type & DCC_SEND_FAST) {
    nr = p->bufsz;
  } else {
    nr = (p->bufsz > DCC_BLOCK_SIZE ? DCC_BLOCK_SIZE : p->bufsz);
  }

  /* Send it to the sendee */
  if (nr) {
    net_queue(p->sendee_sock, (void *)p->buf, nr);
    debug("Sending %lu bytes to client", nr);
    
    /* Adjust or free the buffer */
    p->bytes_sent += nr;
    p->bufsz -= nr;
    if (p->bufsz) {
      memmove(p->buf, p->buf + nr, p->bufsz);
      p->buf = (char *)realloc(p->buf, p->bufsz);
    } else {
      free(p->buf);
      p->buf = 0;
    }
  }

  /* Out of buffer and the sender has gone */
  if (!p->bufsz && (p->sender_status == DCC_SENDER_GONE))
    p->dead = 1;

  return nr;
}
