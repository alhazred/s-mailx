/*@ S-nail - a mail user agent derived from Berkeley Mail.
 *@ SMTP (simple mail transfer protocol) MTA.
 *
 * Copyright (c) 2020 Steffen (Daode) Nurpmeso <steffen@sdaoden.eu>.
 * SPDX-License-Identifier: ISC
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#ifndef mx_NET_SMTP_H
#define mx_NET_SMTP_H

#include <mx/nail.h>
#ifdef mx_HAVE_SMTP

#define mx_HEADER
#include <su/code-in.h>

/* Send a message via SMTP */
EXPORT boole mx_smtp_mta(struct sendbundle *sbp);

#include <su/code-ou.h>
#endif /* mx_HAVE_SMTP */
#endif /* mx_NET_SMTP_H */
/* s-it-mode */
