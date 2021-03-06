/*@ (Yet) Manual config.h.
 *@ XXX Should be split into gen-config.h and config.h.
 *
 * Copyright (c) 2001 - 2020 Steffen (Daode) Nurpmeso <steffen@sdaoden.eu>.
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
#ifndef su_CODE_H
# error Please include su/code.h not su/config.h.
#endif
#ifndef su_CONFIG_H
#define su_CONFIG_H

/*#define su_HAVE_NSPC*/

/* For now thought of _MX, _ROFF; _SU: standalone library */
#ifndef su_USECASE_SU
# define su_USECASE_MX
#endif

#ifdef su_USECASE_MX
   /* In this case we get our config, error maps etc., all from here.
    * We must take care not to break OPT_AMALGAMATION though */
# ifndef mx_HAVE_AMALGAMATION
#  include <mx/gen-config.h>
# endif
#else
# include <su/gen-config.h>
#endif

/* Internal configurables: values */

/* Number of Not-Yet-Dead calls that are remembered */
#define su_NYD_ENTRIES (25 * 84)

/* Global configurables (code.h:CONFIG): features */

#ifdef mx_HAVE_DEBUG
# define su_HAVE_DEBUG
#endif
#ifdef mx_HAVE_DEVEL
# define su_HAVE_DEVEL
#endif

#ifdef mx_HAVE_DOCSTRINGS
# define su_HAVE_DOCSTRINGS
#endif

#define su_HAVE_MEM_BAG_AUTO
#define su_HAVE_MEM_BAG_LOFI
#ifdef mx_HAVE_NOMEMDBG
# define su_HAVE_MEM_CANARIES_DISABLE
#endif

#undef su_HAVE_MT
#undef su_HAVE_SMP

/* Global configurables (code.h:CONFIG): values */

/* Hardware page size (xxx additional dynamic lookup support) */
#ifndef su_PAGE_SIZE
# error Need su_PAGE_SIZE configuration
#endif

#endif /* !su_CONFIG_H */
/* s-it-mode */
