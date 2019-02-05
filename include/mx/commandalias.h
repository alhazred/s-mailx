/*@ S-nail - a mail user agent derived from Berkeley Mail.
 *@ `commandalias'.
 *
 * Copyright (c) 2017 - 2019 Steffen (Daode) Nurpmeso <steffen@sdaoden.eu>.
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
#ifndef mx_COMMANDALIAS_H
#define mx_COMMANDALIAS_H

#include <mx/nail.h>

#include <su/code-in.h>

/* `(un)?commandalias' */
FL int c_commandalias(void *vp);
FL int c_uncommandalias(void *vp);

/* Whether a `commandalias' name exists, returning name or NIL, pointing
 * expansion_or_nil to expansion if set: both point into internal storage */
FL char const *mx_commandalias_exists(char const *name,
      char const **expansion_or_nil);

#include <su/code-ou.h>
#endif /* mx_COMMANDALIAS_H */
/* s-it-mode */
