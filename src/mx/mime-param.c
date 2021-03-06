/*@ S-nail - a mail user agent derived from Berkeley Mail.
 *@ MIME parameter handling.
 *
 * Copyright (c) 2016 - 2020 Steffen (Daode) Nurpmeso <steffen@sdaoden.eu>.
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
#define su_FILE mime_param
#define mx_SOURCE

#ifndef mx_HAVE_AMALGAMATION
# include "mx/nail.h"
#endif

#include <su/cs.h>
#include <su/icodec.h>
#include <su/mem.h>

#include "mx/random.h"

#include "mx/iconv.h"

/* TODO fake */
#include "su/code-in.h"

struct rfc2231_joiner {
   struct rfc2231_joiner *rj_next;
   u32      rj_no;            /* Continuation number */
   u32      rj_len;           /* of useful data in .rj_dat */
   u32      rj_val_off;       /* Start of value data therein */
   u32      rj_cs_len;        /* Length of charset part */
   boole      rj_is_enc;        /* Is percent encoded */
   u8       __pad[7];
   char const  *rj_dat;
};

struct mime_param_builder {
   struct mime_param_builder *mpb_next;
   struct str  *mpb_result;
   u32      mpb_level;        /* of recursion (<-> continuation number) */
   u32      mpb_name_len;     /* of the parameter .mpb_name */
   u32      mpb_value_len;    /* of remaining value */
   u32      mpb_charset_len;  /* of .mpb_charset (iff in outermost level) */
   u32      mpb_buf_len;      /* Usable result of this level in .mpb_buf */
   boole      mpb_is_enc;       /* Level requires encoding */
   u8       __dummy[1];
   boole      mpb_is_utf8;      /* Encoding is UTF-8 */
   s8       mpb_rv;
   char const  *mpb_name;
   char const  *mpb_value;       /* Remains of, once the level was entered */
   char const  *mpb_charset;     /* *ttycharset* */
   char        *mpb_buf;         /* Pointer to on-stack buffer */
};

/* All ASCII characters which cause RFC 2231 to be applied XXX check -1 slots*/
static boole const        _rfc2231_etab[] = {
    1, 1, 1, 1,  1, 1, 1, 1,  1, 1,-1,-1,  1,-1, 1, 1,   /* NUL..SI */
    1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,   /* DLE..US */
    1, 0, 1, 0,  0, 1, 0, 1,  1, 1, 1, 0,  1, 0, 0, 1,   /* CAN.. / */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 1, 1,  1, 1, 1, 1,   /*   0.. ? */

    1, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,   /*   @.. O */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 1,  1, 1, 0, 0,   /*   P.. _ */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,   /*   `.. o */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 1,   /*   p..DEL */
};

/* In a headerbody, at a "param=XY" that we're not interested in, skip over the
 * entire construct, return pointer to the first byte thereafter or to NUL */
static char const * _mime_param_skip(char const *hbp);

/* Trim value, which points to after the "name[RFC 2231 stuff]=".
 * On successful return (1,-1; -1 is returned if the value was quoted via
 * double quotation marks) a set end_or_null points to after the value and any
 * possible separator and result->s is the autorec_alloc()d normalized value */
static s8      _mime_param_value_trim(struct str *result, char const *start,
                     char const **end_or_null);

/* mime_param_get() found the desired parameter but it seems to use RFC 2231
 * extended syntax: perform full RFC 2231 parsing starting at this point.
 * Note that _join() returns is-error */
static char *     _rfc2231_param_parse(char const *param, uz plen,
                     char const *hbp);
static boole     __rfc2231_join(struct rfc2231_joiner *head, char **result,
                     char const **emsg);

/* Recursive parameter builder.  Note we have a magic limit of 999 levels.
 * Prepares a portion of output in self->mpb_buf;
 * once >mpb_value is worked completely the deepmost level joins the result
 * into >mpb_result and unrolls the stack. */
static void       _mime_param_create(struct mime_param_builder *self);
static void       __mime_param_join(struct mime_param_builder *head);

static char const *
_mime_param_skip(char const *hbp)
{
   char co, cn;
   NYD2_IN;

   /* Skip over parameter name - note we may have skipped over an entire
    * parameter name and thus point to a "="; i haven't yet truly checked
    * against MIME RFCs, just test for ";" in the meanwhile XXX */
   while ((cn = *hbp) != '\0' && cn != '=' && cn != ';')
      ++hbp;
   if (cn == '\0')
      goto jleave;
   ++hbp;
   if (cn == ';')
      goto jleave;

   while (su_cs_is_white((cn = *hbp))) /* XXX */
      ++hbp;
   if (cn == '\0')
      goto jleave;

   if (cn == '"') {
      co = '\0';
      while ((cn = *++hbp) != '\0' && (cn != '"' || co == '\\'))
         co = (co == '\\') ? '\0' : cn;
      if (cn != '\0' && (cn = *++hbp) == ';')
         ++hbp;
   } else {
      for (;; cn = *++hbp)
         if (cn == '\0' || cn == ';' || su_cs_is_white(cn))
            break;
      if (cn != '\0')
         ++hbp;
   }
jleave:
   NYD2_OU;
   return hbp;
}

static s8
_mime_param_value_trim(struct str *result, char const *start,
   char const **end_or_null)
{
   char const *e;
   char co, cn;
   uz i;
   s8 rv;
   NYD2_IN;

   while (su_cs_is_white(*start)) /* XXX? */
      ++start;

   if (*start == '"') {
      for (co = '\0', e = ++start;; ++e)
         if ((cn = *e) == '\0')
            goto jerr;
         else if (cn == '"' && co != '\\')
            break;
         else if (cn == '\\' && co == '\\')
            co = '\0';
         else
            co = cn;
      i = P2UZ(e++ - start);
      rv = -TRU1;
   } else {
      for (e = start; (cn = *e) != '\0' && !su_cs_is_white(cn) && cn != ';';
            ++e)
         ;
      i = P2UZ(e - start);
      rv = TRU1;
   }

   result->s = n_autorec_alloc(i +1);
   if (rv > 0) {
      su_mem_copy(result->s, start, result->l = i);
      result->s[i] = '\0';
   } else {
      uz j;
      char *cp;

      for (j = 0, cp = result->s, co = '\0'; i-- > 0; co = cn) {
         cn = *start++;
         if (cn != '\\' || co == '\\') {
            cp[j++] = cn;
            if (cn == '\\')
               cn = '\0';
         }
      }
      cp[j] = '\0';

      result->s = cp;
      result->l = j;
   }

   if (end_or_null != NULL) {
      while (*e != '\0' && *e == ';')
         ++e;
      *end_or_null = e;
   }
jleave:
   NYD2_OU;
   return rv;
jerr:
   rv = FAL0;
   goto jleave;
}

static char *
_rfc2231_param_parse(char const *param, uz plen, char const *hbp)
{
   /* TODO Do it for real and unite with mime_param_get() */
   struct str xval;
   char nobuf[32], *eptr, *rv = NULL, c;
   char const *hbp_base, *cp, *emsg = NULL;
   struct rfc2231_joiner *head = NULL, *np;
   boole errors = FAL0;
   uz i;
   NYD2_IN;

   /* We were called by mime_param_get() after a param name match that
    * involved "*", so jump to the matching code */
   hbp_base = hbp;
   goto jumpin;

   for (; *hbp != '\0'; hbp_base = hbp) {
      while (su_cs_is_white(*hbp))
         ++hbp;

      if (!su_cs_cmp_case_n(hbp, param, plen)) {
         hbp += plen;
         while (su_cs_is_white(*hbp))
            ++hbp;
         if (*hbp++ != '*')
               goto jerr;

         /* RFC 2231 extensions: "NAME[*DIGITS][*]=", where "*DIGITS" indicates
          * parameter continuation and the lone asterisk "*" percent encoded
          * values -- if encoding is used the "*0" or lone parameter value
          * MUST be encoded and start with a "CHARSET'LANGUAGE'" construct,
          * where both of CHARSET and LANGUAGE are optional (we do effectively
          * generate error if CHARSET is missing though).
          * Continuations may not use that "C'L'" construct, but be tolerant
          * and ignore those.  Also encoded and non-encoded continuations may
          * occur, i.e., perform percent en-/decoding only as necessary.
          * Continuations may occur in any order */
         /* xxx RFC 2231 parsing ignores language tags */
jumpin:
         for (cp = hbp; su_cs_is_digit(*cp); ++cp)
            ;
         i = P2UZ(cp - hbp);
         if (i != 0) {
            if (i >= sizeof(nobuf)) {
               emsg = N_("too many digits to form a valid number");
               goto jerr;
            } else if ((c = *cp) != '=' && c != '*') {
               emsg = N_("expected = or * after leading digits");
               goto jerr;
            }
            su_mem_copy(nobuf, hbp, i);
            nobuf[i] = '\0';
            if((su_idec_uz_cp(&i, nobuf, 10, NULL
                     ) & (su_IDEC_STATE_EMASK | su_IDEC_STATE_CONSUMED)
                  ) != su_IDEC_STATE_CONSUMED || i >= 999){
               emsg = N_("invalid continuation sequence number");
               goto jerr;
            }
            hbp = ++cp;

            /* Value encoded? */
            if (c == '*') {
               if (*hbp++ != '=')
                  goto jeeqaaster;
            } else if (c != '=') {
jeeqaaster:
               emsg = N_("expected = after asterisk *");
               goto jerr;
            }
         } else {
            /* In continuation mode that is an error, however */
            if (head != NULL) {
               emsg = N_("missing continuation sequence number");
               goto jerr;
            }
            /* Parameter value is encoded, may define encoding */
            c = '*';
            if (*cp != '=')
               goto jeeqaaster;
            hbp = ++cp;
            i = 0;
         }

         /* Create new node and insert it sorted; should be faster than
          * creating an unsorted list and sorting it after parsing */
         np = n_alloc(sizeof *np);
         np->rj_next = NULL;
         np->rj_no = (u32)i;
         np->rj_is_enc = (c == '*');
         np->rj_val_off = np->rj_cs_len = 0;

         if (head == NULL)
            head = np;
         else if (i < head->rj_no) {
            np->rj_next = head;
            head = np;
         } else {
            struct rfc2231_joiner *l = NULL, *x = head;

            while (x != NULL && i > x->rj_no)
               l = x, x = x->rj_next;
            if (x != NULL)
               np->rj_next = x;
            ASSERT(l != NULL);
            l->rj_next = np;
         }

         switch (_mime_param_value_trim(&xval, hbp, &cp)) {
         default:
            emsg = (c == '*') ? N_("invalid value encoding")/* XXX fake */
                  : N_("faulty value - missing closing quotation mark \"?");
            goto jerr;
         case -1:
            /* XXX if (np->is_enc && su_mem_find(np->dat, '\'', i) != NULL) {
             * XXX    emsg = N_("character set info not allowed here");
             * XXX    goto jerr;
             * XXX } */np->rj_is_enc = FAL0; /* Silently ignore */
            /* FALLTHRU */
         case 1:
            if (xval.l >= U32_MAX) {
               emsg = N_("parameter value too long");
               goto jerr;
            }
            np->rj_len = (u32)xval.l;
            np->rj_dat = xval.s;
            break;
         }

         /* Watch out for character set and language info */
         if (np->rj_is_enc &&
               (eptr = su_mem_find(xval.s, '\'', xval.l)) != NULL) {
            np->rj_cs_len = P2UZ(eptr - xval.s);
            if ((eptr = su_mem_find(eptr + 1, '\'', xval.l - np->rj_cs_len - 1)
                  ) == NULL) {
               emsg = N_("faulty RFC 2231 parameter extension");
               goto jerr;
            }
            np->rj_val_off = P2UZ(++eptr - xval.s);
         }

         hbp = cp;
      } else
         hbp = _mime_param_skip(hbp);
   }
   ASSERT(head != NULL); /* (always true due to jumpin:, but..) */

   errors |= __rfc2231_join(head, &rv, &emsg);
   if (errors /*&& (n_poption & n_PO_D_V)*/) {
      /* TODO should set global flags so that at the end of an operation
       * TODO (for a message) a summary can be printed: faulty MIME, xy */
      if (emsg == NULL)
         emsg = N_("multiple causes");
      n_err(_("Message had MIME errors: %s\n"), V_(emsg));
   }
jleave:
   NYD2_OU;
   return rv;

jerr:
   while ((np = head) != NULL) {
      head = np->rj_next;
      n_free(np);
   }
   /*if (n_poption & n_PO_D_V)*/ {
      if (emsg == NULL)
         emsg = N_("expected asterisk *");
      n_err(_("Faulty RFC 2231 MIME parameter value: %s: %s\n"
         "Near: %s\n"), param, V_(emsg), hbp_base);
   }
   rv = NULL;
   goto jleave;
}

static boole
__rfc2231_join(struct rfc2231_joiner *head, char **result, char const **emsg)
{
   struct str sin, sou;
   struct rfc2231_joiner *np;
   char const *cp;
   uz i;
   enum {
      _NONE       = 0,
      _HAVE_ENC   = 1<<0,
      _HAVE_ICONV = 1<<1,
      _SEEN_ANY   = 1<<2,
      _ERRORS     = 1<<3
   } f = _NONE;
   u32 no;
#ifdef mx_HAVE_ICONV
   iconv_t fhicd;
#endif
   NYD2_IN;

#ifdef mx_HAVE_ICONV
   UNINIT(fhicd, (iconv_t)-1);

   if (head->rj_is_enc) {
      char const *tcs;

      f |= _HAVE_ENC;
      if (head->rj_cs_len == 0) {
         /* It is an error if the character set is not set, the language alone
          * cannot convert characters, let aside that we don't use it at all */
         *emsg = N_("MIME RFC 2231 invalidity: missing character set\n");
         f |= _ERRORS;
      } else if (su_cs_cmp_case_n(tcs = ok_vlook(ttycharset),
            head->rj_dat, head->rj_cs_len)) {
         char *cs = n_lofi_alloc(head->rj_cs_len +1);

         su_mem_copy(cs, head->rj_dat, head->rj_cs_len);
         cs[head->rj_cs_len] = '\0';
         if ((fhicd = n_iconv_open(tcs, cs)) != (iconv_t)-1)
            f |= _HAVE_ICONV;
         else {
            *emsg = N_("necessary character set conversion missing");
            f |= _ERRORS;
         }
         n_lofi_free(cs);
      }
   }
#endif

   if (head->rj_no != 0) {
      if (!(f & _ERRORS))
         *emsg = N_("First RFC 2231 parameter value chunk number is not 0");
      f |= _ERRORS;
   }

   for (sou.s = NULL, sou.l = 0, no = 0; (np = head) != NULL; n_free(np)) {
      head = np->rj_next;

      if (np->rj_no != no++) {
         if (!(f & _ERRORS))
            *emsg = N_("RFC 2231 parameter value chunks are not contiguous");
         f |= _ERRORS;
      }

      /* RFC 2231 allows such info only in the first continuation, and
       * furthermore MUSTs the first to be encoded, then */
      if (/*np->rj_is_enc &&*/ np->rj_val_off > 0 &&
            (f & (_HAVE_ENC | _SEEN_ANY)) != _HAVE_ENC) {
         if (!(f & _ERRORS))
            *emsg = N_("invalid redundant RFC 2231 charset/language ignored");
         f |= _ERRORS;
      }
      f |= _SEEN_ANY;

      i = np->rj_len - np->rj_val_off;
      if (!np->rj_is_enc)
         n_str_add_buf(&sou, np->rj_dat + np->rj_val_off, i);
      else {
         /* Perform percent decoding */
         sin.s = n_alloc(i +1);
         sin.l = 0;

         for (cp = np->rj_dat + np->rj_val_off; i > 0;) {
            char c;

            if ((c = *cp++) == '%') {
               s32 cc;

               if (i < 3 || (cc = n_c_from_hex_base16(cp)) < 0) {
                  if (!(f & _ERRORS))
                     *emsg = N_("invalid RFC 2231 percent encoded sequence");
                  f |= _ERRORS;
                  goto jhex_putc;
               }
               sin.s[sin.l++] = (char)cc;
               cp += 2;
               i -= 3;
            } else {
jhex_putc:
               sin.s[sin.l++] = c;
               --i;
            }
         }
         sin.s[sin.l] = '\0';

         n_str_add_buf(&sou, sin.s, sin.l);
         n_free(sin.s);
      }
   }

   /* And add character set conversion on top as necessary.
    * RFC 2231 is pragmatic: encode only mentions percent encoding and the
    * character set for the entire string ("[no] facility for using more
    * than one character set or language"), therefore "continuations may
    * contain a mixture of encoded and unencoded segments" applies to
    * a contiguous string of a single character set that has been torn in
    * pieces due to space restrictions, and it happened that some pieces
    * didn't need to be percent encoded.
    *
    * _In particular_ it therefore doesn't repeat the RFC 2047 paradigm
    * that encoded-words-are-atomic, meaning that a single character-set
    * conversion run over the final, joined, partially percent-decoded value
    * should be sufficient */
#ifdef mx_HAVE_ICONV
   if (f & _HAVE_ICONV) {
      sin.s = NULL;
      sin.l = 0;
      if (n_iconv_str(fhicd, n_ICONV_UNIDEFAULT, &sin, &sou, NULL) != 0) {
         if (!(f & _ERRORS)) /* XXX won't be reported with _UNIDFEFAULT */
            *emsg = N_("character set conversion failed on value");
         f |= _ERRORS;
      }
      n_free(sou.s);
      sou = sin;

      n_iconv_close(fhicd);
   }
#endif

   su_mem_copy(*result = n_autorec_alloc(sou.l +1), sou.s, sou.l +1);
   n_free(sou.s);
   NYD2_OU;
   return ((f & _ERRORS) != 0);
}

static void
_mime_param_create(struct mime_param_builder *self)
{
   struct mime_param_builder next;
   /* Don't use MIME_LINELEN_(MAX|LIMIT) stack buffer sizes: normally we won't
    * exceed plain MIME_LINELEN, so that this would be a factor 10 wastage.
    * On the other hand we may excess _LINELEN to avoid breaking up possible
    * multibyte sequences until sizeof(buf) is reached, but since we (a) don't
    * support stateful encodings and (b) will try to synchronize on UTF-8 this
    * problem is scarce, possibly even artificial */
   char buf[MIN(MIME_LINELEN_MAX >> 1, MIME_LINELEN * 2)],
      *bp, *bp_max, *bp_xmax, *bp_lanoenc;
   char const *vb, *vb_lanoenc;
   uz vl;
   enum {
      _NONE    = 0,
      _ISENC   = 1<<0,
      _HADRAW  = 1<<1,
      _RAW     = 1<<2
   } f = _NONE;
   NYD2_IN;
   LCTA(sizeof(buf) >= MIME_LINELEN * 2, "Buffer to small for operation");

jneed_enc:
   self->mpb_buf = bp = bp_lanoenc = buf;
   self->mpb_buf_len = 0;
   self->mpb_is_enc = ((f & _ISENC) != 0);
   vb_lanoenc = vb = self->mpb_value;
   vl = self->mpb_value_len;

   /* Configure bp_max to fit in SHOULD, bp_xmax to extent */
   bp_max = (buf + MIME_LINELEN) -
         (1 + self->mpb_name_len + sizeof("*999*='';") -1 + 2);
   bp_xmax = (buf + sizeof(buf)) -
         (1 + self->mpb_name_len + sizeof("*999*='';") -1 + 2);
   if ((f & _ISENC) && self->mpb_level == 0) {
      bp_max -= self->mpb_charset_len;
      bp_xmax -= self->mpb_charset_len;
   }
   if (PCMP(bp_max, <=, buf + sizeof("Hunky Dory"))) {
      su_DBG( n_alert("_mime_param_create(): Hunky Dory!"); )
      bp_max = buf + (MIME_LINELEN >> 1); /* And then it is SHOULD, anyway */
   }
   ASSERT(PCMP(bp_max + (4 * 3), <=, bp_xmax)); /* UTF-8 extra pad, below */

   f &= _ISENC;
   while (vl > 0) {
      union {char c; u8 uc;} u; u.c = *vb;

      f |= _RAW;
      if (!(f & _ISENC)) {
         if (u.uc > 0x7F || su_cs_is_cntrl(u.c)) { /* XXX reject _is_cntrl? */
            /* We need to percent encode this character, possibly changing
             * overall strategy, but anyway the one of this level, possibly
             * rendering invalid any output byte we yet have produced here.
             * Instead of throwing away that work just recurse if some fancy
             * magic condition is true */
             /* *However*, many tested MUAs fail to deal with parameters that
              * are split across "too many" fields, including ones that
              * misread RFC 2231 to allow only one digit, i.e., a maximum of
              * ten.  This is plain wrong, but that won't help their users */
            if (P2UZ(bp - buf) > /*10 (strawberry) COMPAT*/MIME_LINELEN>>1)
               goto jrecurse;
            f |= _ISENC;
            goto jneed_enc;
         }

         if (u.uc == '"' || u.uc == '\\') {
            f ^= _RAW;
            bp[0] = '\\';
            bp[1] = u.c;
            bp += 2;
         }
      } else if (u.uc > 0x7F || _rfc2231_etab[u.uc]) {
         f ^= _RAW;
         bp[0] = '%';
         n_c_to_hex_base16(bp + 1, u.c);
         bp += 3;
      }

      ++vb;
      --vl;
      if (f & _RAW) {
         f |= _HADRAW;
         vb_lanoenc = vb;
         *bp++ = u.c;
         bp_lanoenc = bp;
      }

      /* If all available space has been consumed we must split.
       * Due to compatibility reasons we must take care not to break up
       * multibyte sequences -- even though RFC 2231 rather implies that the
       * split value should be joined (after percent encoded fields have
       * been percent decoded) and the resulting string be treated in the
       * specified character set / language, MUAs have been seen which apply
       * the RFC 2047 encoded-words-are-atomic even to RFC 2231 values, even
       * if stateful encodings cannot truly be supported like that?!..
       *
       * So split at 7-bit character if we have seen any and the wastage isn't
       * too large; recall that we need to keep the overall number of P=V
       * values as low as possible due to compatibility reasons.
       * If we haven't seen any plain bytes be laxe and realize that bp_max
       * reflects SHOULD lines, and try to extend this as long as possible.
       * However, with UTF-8, try to backward synchronize on sequence start */
      if (bp <= bp_max)
         continue;

      if ((f & _HADRAW) && (PCMP(bp - bp_lanoenc, <=, bp_lanoenc - buf) ||
            (!self->mpb_is_utf8 &&
             P2UZ(bp_lanoenc - buf) >= (MIME_LINELEN >> 2)))) {
         bp = bp_lanoenc;
         vl += P2UZ(vb - vb_lanoenc);
         vb = vb_lanoenc;
         goto jrecurse;
      }

      if (self->mpb_is_utf8 && ((u8)(vb[-1]) & 0xC0) != 0x80) {
         bp -= 3;
         --vb;
         ++vl;
         goto jrecurse;
      }

      if (bp <= bp_xmax)
         continue;
      /* (Shit.) */
      goto jrecurse;
   }

   /* That level made the great and completed encoding.  Build result */
   self->mpb_is_enc = ((f & _ISENC) != 0);
   self->mpb_buf_len = P2UZ(bp - buf);
   __mime_param_join(self);
jleave:
   NYD2_OU;
   return;

   /* Need to recurse, take care not to excess magical limit of 999 levels */
jrecurse:
   if (self->mpb_level == 999) {
      /*if (n_poption & n_PO_D_V)*/
         n_err(_("Message RFC 2231 parameters nested too deeply!\n"));
      goto jleave;
   }

   self->mpb_is_enc = ((f & _ISENC) != 0);
   self->mpb_buf_len = P2UZ(bp - buf);

   su_mem_set(&next, 0, sizeof next);
   next.mpb_next = self;
   next.mpb_level = self->mpb_level + 1;
   next.mpb_name_len = self->mpb_name_len;
   next.mpb_value_len = vl;
   next.mpb_is_utf8 = self->mpb_is_utf8;
   next.mpb_name = self->mpb_name;
   next.mpb_value = vb;
   _mime_param_create(&next);
   goto jleave;
}

static void
__mime_param_join(struct mime_param_builder *head)
{
   char nobuf[16];
   struct mime_param_builder *np;
   uz i, ll;  ASSERT_INJ( uz len_max; )
   struct str *result;
   char *cp;
   enum {
      _NONE    = 0,
      _ISENC   = 1<<0,
      _ISQUOTE = 1<<1,
      _ISCONT  = 1<<2
   } f = _NONE;
   NYD2_IN;

   /* Traverse the stack upwards to find out result length (worst case).
    * Reverse the list while doing so */
   for (i = 0, np = head, head = NULL; np != NULL;) {
      struct mime_param_builder *tmp;

      i += np->mpb_buf_len + np->mpb_name_len + sizeof(" *999*=\"\";\n") -1;
      if (np->mpb_is_enc)
         f |= _ISENC;

      tmp = np->mpb_next;
      np->mpb_next = head;
      head = np;
      np = tmp;
   }
   if (f & _ISENC)
      i += head->mpb_charset_len; /* sizeof("''") -1 covered by \"\" above */
   ASSERT_INJ( len_max = i; )
   head->mpb_rv = TRU1;

   result = head->mpb_result;
   if (head->mpb_next != NULL)
      f |= _ISCONT;
   cp = result->s = n_autorec_alloc(i +1);

   for (ll = 0, np = head;;) {
      /* Name part */
      su_mem_copy(cp, np->mpb_name, i = np->mpb_name_len);
      cp += i;
      ll += i;

      if (f & _ISCONT) {
         char *cpo = cp, *nop = nobuf + sizeof(nobuf);
         u32 noi = np->mpb_level;

         *--nop = '\0';
         do
            *--nop = "0123456789"[noi % 10];
         while ((noi /= 10) != 0);

         *cp++ = '*';
         while (*nop != '\0')
            *cp++ = *nop++;

         ll += P2UZ(cp - cpo);
      }

      if ((f & _ISENC) || np->mpb_is_enc) {
         *cp++ = '*';
         ++ll;
      }
      *cp++ = '=';
      ++ll;

      /* Value part */
      if (f & _ISENC) {
         f &= ~_ISENC;
         su_mem_copy(cp, np->mpb_charset, i = np->mpb_charset_len);
         cp += i;
         cp[0] = '\'';
         cp[1] = '\'';
         cp += 2;
         ll += i + 2;
      } else if (!np->mpb_is_enc) {
         f |= _ISQUOTE;
         *cp++ = '"';
         ++ll;
      }

      su_mem_copy(cp, np->mpb_buf, i = np->mpb_buf_len);
      cp += i;
      ll += i;

      if (f & _ISQUOTE) {
         f ^= _ISQUOTE;
         *cp++ = '"';
         ++ll;
      }

      if ((np = np->mpb_next) == NULL)
         break;
      *cp++ = ';';
      ++ll;

      i = ll;
      i += np->mpb_name_len + np->mpb_buf_len + sizeof(" *999*=\"\";\n") -1;
      if (i >= MIME_LINELEN) {
         head->mpb_rv = -TRU1;
         *cp++ = '\n';
         ll = 0;
      }

      *cp++ = ' ';
      ++ll;
   }
   *cp = '\0';
   result->l = P2UZ(cp - result->s);
   ASSERT(result->l < len_max);
   NYD2_OU;
}

FL char *
mime_param_get(char const *param, char const *headerbody) /* TODO rewr. */
{
   struct str xval;
   char *rv = NULL;
   uz plen;
   char const *p;
   NYD_IN;

   plen = su_cs_len(param);
   p = headerbody;

   /* At the beginning of headerbody there is no parameter=value pair xxx */
   if (!su_cs_is_white(*p))
      goto jskip1st;

   for (;;) {
      while (su_cs_is_white(*p))
         ++p;

      if (!su_cs_cmp_case_n(p, param, plen)) {
         p += plen;
         while (su_cs_is_white(*p)) /* XXX? */
            ++p;
         switch (*p++) {
         case '*':
            rv = _rfc2231_param_parse(param, plen, p);
            goto jleave;
         case '=':
            if (!_mime_param_value_trim(&xval, p, NULL)) {
               /* XXX LOG? */
               goto jleave;
            }
            rv = xval.s;

            /* We do have a result, but some (elder) software (S-nail <v14.8)
             * will use RFC 2047 encoded words in  parameter values, too */
            /* TODO Automatically check whether the value seems to be RFC 2047
             * TODO encwd. -- instead use *rfc2047_parameters* like mutt(1)? */
            if ((p = su_cs_find(rv, "=?")) != NULL &&
                  su_cs_find(p, "?=") != NULL) {
               struct str ti, to;

               ti.l = su_cs_len(ti.s = rv);
               mime_fromhdr(&ti, &to, TD_ISPR | TD_ICONV | TD_DELCTRL);
               rv = savestrbuf(to.s, to.l);
               n_free(to.s);
            }
            goto jleave;
         default:
            /* Not our desired parameter, skip and continue */
            break;
         }
      }

jskip1st:
      if (*(p = _mime_param_skip(p)) == '\0')
         goto jleave;
   }

jleave:
   NYD_OU;
   return rv;
}

FL s8
mime_param_create(struct str *result, char const *name, char const *value)
{
   /* TODO All this needs rework when we have (1) a real string and even more
    * TODO (2) use objects instead of stupid string concat; it's temporary
    * TODO I.e., this function should return a HeaderBodyParam */
   struct mime_param_builder top;
   uz i;
   NYD_IN;

   su_mem_set(result, 0, sizeof *result);

   su_mem_set(&top, 0, sizeof top);
   top.mpb_result = result;
   if ((i = su_cs_len(top.mpb_name = name)) >= U32_MAX)
      goto jleave;
   top.mpb_name_len = (u32)i;
   if ((i = su_cs_len(top.mpb_value = value)) >= U32_MAX)
      goto jleave;
   top.mpb_value_len = (u32)i;
   if ((i = su_cs_len(name = ok_vlook(ttycharset))) >= U32_MAX)
      goto jleave;
   top.mpb_charset_len = (u32)i;
   top.mpb_charset = n_autorec_alloc(++i);
   su_mem_copy(n_UNCONST(top.mpb_charset), name, i);
   if(top.mpb_charset_len >= 4 && !su_mem_cmp(top.mpb_charset, "utf", 3) &&
         ((top.mpb_charset[3] == '-' && top.mpb_charset[4] == '8' &&
          top.mpb_charset_len == 5) || (top.mpb_charset[3] == '8' &&
          top.mpb_charset_len == 4)))
      top.mpb_is_utf8 = TRU1;
   else
      top.mpb_is_utf8 = FAL0;

   _mime_param_create(&top);
jleave:
   NYD_OU;
   return top.mpb_rv;
}

FL char *
mime_param_boundary_get(char const *headerbody, uz *len)
{
   char *q = NULL, *p;
   NYD_IN;

   if ((p = mime_param_get("boundary", headerbody)) != NULL) {
      uz i = su_cs_len(p);

      if (len != NULL)
         *len = i + 2;
      q = n_autorec_alloc(i + 2 +1);
      q[0] = q[1] = '-';
      su_mem_copy(q + 2, p, i);
      *(q + i + 2) = '\0';
   }
   NYD_OU;
   return q;
}

FL char *
mime_param_boundary_create(void)
{
   static u32 reprocnt;
   char *bp;
   NYD_IN;

   bp = n_autorec_alloc(36 + 6 +1);
   bp[0] = bp[2] = bp[39] = bp[41] = '=';
   bp[1] = bp[40] = '-';
   su_mem_copy(bp + 3, mx_random_create_cp(36, &reprocnt), 36);
   bp[42] = '\0';
   NYD_OU;
   return bp;
}

#include "su/code-ou.h"
/* s-it-mode */
