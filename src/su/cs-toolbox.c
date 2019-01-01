/*@ Implementation of cs.h: the toolboxes.
 *
 * Copyright (c) 2017 - 2018 Steffen (Daode) Nurpmeso <steffen@sdaoden.eu>.
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
#undef su_FILE
#define su_FILE su_cs_toolbox
#define su_SOURCE
#define su_SOURCE_CS_TOOLBOX

#include "su/code.h"

#include "su/mem.h"

#include "su/cs.h"
#include "su/code-in.h"

/**/
#if DVLOR(1, 0)
static void a_cstoolbox_free(void *t);
#else
# define a_cstoolbox_free su_mem_free
#endif

/**/
static void *a_cstoolbox_assign(void *self, void const *t);
static uz a_cstoolbox_hash(void *self);
static uz a_cstoolbox_hash_case(void *self);

#if DVLOR(1, 0)
static void
a_cstoolbox_free(void *t){
   NYD2_IN;
   su_FREE(t);
   NYD2_OU;
}
#endif

static void *
a_cstoolbox_assign(void *self, void const *t){
   NYD2_IN;
   su_FREE(self);
   self = su_cs_dup(t);
   NYD2_OU;
   return self;
}

static uz
a_cstoolbox_hash(void *self){
   uz rv;
   NYD2_IN;

   rv = su_cs_hash(self);
   NYD2_OU;
   return rv;
}

static uz
a_cstoolbox_hash_case(void *self){
   uz rv;
   NYD2_IN;

   rv = su_cs_hash_case(self);
   NYD2_OU;
   return rv;
}

struct su_toolbox const su_cs_toolbox = su_TOOLBOX_I9R(
   &su_cs_dup, &a_cstoolbox_free, &a_cstoolbox_assign,
   &su_cs_cmp, &a_cstoolbox_hash);

struct su_toolbox const su_cs_toolbox_case = su_TOOLBOX_I9R(
   &su_cs_dup, &a_cstoolbox_free, &a_cstoolbox_assign,
   &su_cs_cmp_case, &a_cstoolbox_hash_case);

#include "su/code-ou.h"
/* s-it-mode */
