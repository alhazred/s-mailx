/*@ S-nail - a mail user agent derived from Berkeley Mail.
 *@ Routines for processing and detecting headlines.
 *@ TODO Mostly a hackery, we need RFC compliant parsers instead.
 *
 * Copyright (c) 2000-2004 Gunnar Ritter, Freiburg i. Br., Germany.
 * Copyright (c) 2012 - 2018 Steffen (Daode) Nurpmeso <steffen@sdaoden.eu>.
 */
/*
 * Copyright (c) 1980, 1993
 *      The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#undef n_FILE
#define n_FILE head

#ifndef HAVE_AMALGAMATION
# include "nail.h"
#endif

#include <pwd.h>

struct cmatch_data {
   size_t      tlen;    /* Length of .tdata */
   char const  *tdata;  /* Template date - see _cmatch_data[] */
};

/* Template characters for cmatch_data.tdata:
 * 'A'   An upper case char
 * 'a'   A lower case char
 * ' '   A space
 * '0'   A digit
 * 'O'   An optional digit or space
 * ':'   A colon
 * '+'  Either a plus or a minus sign */
static struct cmatch_data const  _cmatch_data[] = {
   { 24, "Aaa Aaa O0 00:00:00 0000" },       /* BSD/ISO C90 ctime */
   { 28, "Aaa Aaa O0 00:00:00 AAA 0000" },   /* BSD tmz */
   { 21, "Aaa Aaa O0 00:00 0000" },          /* SysV ctime */
   { 25, "Aaa Aaa O0 00:00 AAA 0000" },      /* SysV tmz */
   /* RFC 822-alike From_ lines do not conform to RFC 4155, but seem to be used
    * in the wild (by UW-imap) */
   { 30, "Aaa Aaa O0 00:00:00 0000 +0000" },
   /* RFC 822 with zone spec; 1. military, 2. UT, 3. north america time
    * zone strings; note that 1. is strictly speaking not correct as some
    * letters are not used, and 2. is not because only "UT" is defined */
#define __reuse      "Aaa Aaa O0 00:00:00 0000 AAA"
   { 28 - 2, __reuse }, { 28 - 1, __reuse }, { 28 - 0, __reuse },
   { 0, NULL }
};
#define a_HEAD_DATE_MINLEN 21

/* Skip over "word" as found in From_ line */
static char const *        _from__skipword(char const *wp);

/* Match the date string against the date template (tp), return if match.
 * See _cmatch_data[] for template character description */
static int                 _cmatch(size_t len, char const *date,
                              char const *tp);

/* Check whether date is a valid 'From_' date.
 * (Rather ctime(3) generated dates, according to RFC 4155) */
static int                 _is_date(char const *date);

/* JulianDayNumber converter(s) */
static size_t a_head_gregorian_to_jdn(ui32_t y, ui32_t m, ui32_t d);
#if 0
static void a_head_jdn_to_gregorian(size_t jdn,
               ui32_t *yp, ui32_t *mp, ui32_t *dp);
#endif

/* Convert the domain part of a skinned address to IDNA.
 * If an error occurs before Unicode information is available, revert the IDNA
 * error to a normal CHAR one so that the error message doesn't talk Unicode */
#ifdef HAVE_IDNA
static struct n_addrguts *a_head_idna_apply(struct n_addrguts *agp);
#endif

/* Classify and check a (possibly skinned) header body according to RFC
 * *addr-spec* rules; if it (is assumed to has been) skinned it may however be
 * also a file or a pipe command, so check that first, then.
 * Otherwise perform content checking and isolate the domain part (for IDNA).
 * issingle_hack has the same meaning as for n_addrspec_with_guts() */
static bool_t a_head_addrspec_check(struct n_addrguts *agp, bool_t skinned,
               bool_t issingle_hack);

/* Return the next header field found in the given message.
 * Return >= 0 if something found, < 0 elsewise.
 * "colon" is set to point to the colon in the header.
 * Must deal with \ continuations & other such fraud */
static long a_gethfield(FILE *f, char **linebuf, size_t *linesize, long rem,
            char **colon, bool_t support_sh_comment);

static int                 msgidnextc(char const **cp, int *status);

static char const *        nexttoken(char const *cp);

static char const *
_from__skipword(char const *wp)
{
   char c = 0;
   NYD2_ENTER;

   if (wp != NULL) {
      while ((c = *wp++) != '\0' && !blankchar(c)) {
         if (c == '"') {
            while ((c = *wp++) != '\0' && c != '"')
               ;
            if (c != '"')
               --wp;
         }
      }
      for (; blankchar(c); c = *wp++)
         ;
   }
   NYD2_LEAVE;
   return (c == 0 ? NULL : wp - 1);
}

static int
_cmatch(size_t len, char const *date, char const *tp)
{
   int ret = 0;
   NYD2_ENTER;

   while (len--) {
      char c = date[len];
      switch (tp[len]) {
      case 'a':
         if (!lowerchar(c))
            goto jleave;
         break;
      case 'A':
         if (!upperchar(c))
            goto jleave;
         break;
      case ' ':
         if (c != ' ')
            goto jleave;
         break;
      case '0':
         if (!digitchar(c))
            goto jleave;
         break;
      case 'O':
         if (c != ' ' && !digitchar(c))
            goto jleave;
         break;
      case ':':
         if (c != ':')
            goto jleave;
         break;
      case '+':
         if (c != '+' && c != '-')
            goto jleave;
         break;
      }
   }
   ret = 1;
jleave:
   NYD2_LEAVE;
   return ret;
}

static int
_is_date(char const *date)
{
   struct cmatch_data const *cmdp;
   size_t dl;
   int rv = 0;
   NYD2_ENTER;

   if ((dl = strlen(date)) >= a_HEAD_DATE_MINLEN)
      for (cmdp = _cmatch_data; cmdp->tdata != NULL; ++cmdp)
         if (dl == cmdp->tlen && (rv = _cmatch(dl, date, cmdp->tdata)))
            break;
   NYD2_LEAVE;
   return rv;
}

static size_t
a_head_gregorian_to_jdn(ui32_t y, ui32_t m, ui32_t d){
   /* Algorithm is taken from Communications of the ACM, Vol 6, No 8.
    * (via third hand, plus adjustments).
    * This algorithm is supposed to work for all dates in between 1582-10-15
    * (0001-01-01 but that not Gregorian) and 65535-12-31 */
   size_t jdn;
   NYD2_ENTER;

#if 0
   if(y == 0)
      y = 1;
   if(m == 0)
      m = 1;
   if(d == 0)
      d = 1;
#endif

   if(m > 2)
      m -= 3;
   else{
      m += 9;
      --y;
   }
   jdn = y;
   jdn /= 100;
   y -= 100 * jdn;
   y *= 1461;
   y >>= 2;
   jdn *= 146097;
   jdn >>= 2;
   jdn += y;
   jdn += d;
   jdn += 1721119;
   m *= 153;
   m += 2;
   m /= 5;
   jdn += m;
   NYD2_LEAVE;
   return jdn;
}

#if 0
static void
a_head_jdn_to_gregorian(size_t jdn, ui32_t *yp, ui32_t *mp, ui32_t *dp){
   /* Algorithm is taken from Communications of the ACM, Vol 6, No 8.
    * (via third hand, plus adjustments) */
   size_t y, x;
   NYD2_ENTER;

   jdn -= 1721119;
   jdn <<= 2;
   --jdn;
   y =   jdn / 146097;
         jdn %= 146097;
   jdn |= 3;
   y *= 100;
   y +=  jdn / 1461;
         jdn %= 1461;
   jdn += 4;
   jdn >>= 2;
   x = jdn;
   jdn <<= 2;
   jdn += x;
   jdn -= 3;
   x =   jdn / 153;  /* x -> month */
         jdn %= 153;
   jdn += 5;
   jdn /= 5; /* jdn -> day */
   if(x < 10)
      x += 3;
   else{
      x -= 9;
      ++y;
   }

   *yp = (ui32_t)(y & 0xFFFF);
   *mp = (ui32_t)(x & 0xFF);
   *dp = (ui32_t)(jdn & 0xFF);
   NYD2_LEAVE;
}
#endif /* 0 */

#ifdef HAVE_IDNA
static struct n_addrguts *
a_head_idna_apply(struct n_addrguts *agp){
   struct n_string idna_ascii;
   NYD_ENTER;

   n_string_creat_auto(&idna_ascii);

   if(!n_idna_to_ascii(&idna_ascii, &agp->ag_skinned[agp->ag_sdom_start],
         agp->ag_slen - agp->ag_sdom_start))
      agp->ag_n_flags ^= NAME_ADDRSPEC_ERR_IDNA | NAME_ADDRSPEC_ERR_CHAR;
   else{
      /* Replace the domain part of .ag_skinned with IDNA version */
      n_string_unshift_buf(&idna_ascii, agp->ag_skinned, agp->ag_sdom_start);

      agp->ag_skinned = n_string_cp(&idna_ascii);
      agp->ag_slen = idna_ascii.s_len;
      NAME_ADDRSPEC_ERR_SET(agp->ag_n_flags,
         NAME_NAME_SALLOC | NAME_SKINNED | NAME_IDNA, 0);
   }
   NYD_LEAVE;
   return agp;
}
#endif /* HAVE_IDNA */

static bool_t
a_head_addrspec_check(struct n_addrguts *agp, bool_t skinned,
      bool_t issingle_hack)
{
   char *addr, *p;
   union {bool_t b; char c; unsigned char u; ui32_t ui32; si32_t si32;} c;
   enum{
      a_NONE,
      a_IDNA_ENABLE = 1u<<0,
      a_IDNA_APPLY = 1u<<1,
      a_REDO_NODE_AFTER_ADDR = 1u<<2,
      a_RESET_MASK = a_IDNA_ENABLE | a_IDNA_APPLY | a_REDO_NODE_AFTER_ADDR,
      a_IN_QUOTE = 1u<<8,
      a_IN_AT = 1u<<9,
      a_IN_DOMAIN = 1u<<10,
      a_DOMAIN_V6 = 1u<<11,
      a_DOMAIN_MASK = a_IN_DOMAIN | a_DOMAIN_V6
   } flags;
   NYD_ENTER;

   flags = a_NONE;
#ifdef HAVE_IDNA
   if(!ok_blook(idna_disable))
      flags = a_IDNA_ENABLE;
#endif

   if (agp->ag_iaddr_aend - agp->ag_iaddr_start == 0) {
      NAME_ADDRSPEC_ERR_SET(agp->ag_n_flags, NAME_ADDRSPEC_ERR_EMPTY, 0);
      goto jleave;
   }

   addr = agp->ag_skinned;

   /* If the field is not a recipient, it cannot be a file or a pipe */
   if (!skinned)
      goto jaddr_check;

   /* When changing any of the following adjust any RECIPIENTADDRSPEC;
    * grep the latter for the complete picture */
   if (*addr == '|') {
      agp->ag_n_flags |= NAME_ADDRSPEC_ISPIPE;
      goto jleave;
   }
   if (addr[0] == '/' || (addr[0] == '.' && addr[1] == '/') ||
         (addr[0] == '-' && addr[1] == '\0'))
      goto jisfile;
   if (memchr(addr, '@', agp->ag_slen) == NULL) {
      if (*addr == '+')
         goto jisfile;
      for (p = addr; (c.c = *p); ++p) {
         if (c.c == '!' || c.c == '%')
            break;
         if (c.c == '/') {
jisfile:
            agp->ag_n_flags |= NAME_ADDRSPEC_ISFILE;
            goto jleave;
         }
      }
   }

jaddr_check:
   /* TODO This is false.  If super correct this should work on wide
    * TODO characters, just in case (some bytes of) the ASCII set is (are)
    * TODO shared; it may yet tear apart multibyte sequences, possibly.
    * TODO All this should interact with mime_enc_mustquote(), too!
    * TODO That is: once this is an object, we need to do this in a way
    * TODO that it is valid for the wire format (instead)! */
   /* TODO addrspec_check: we need a real RFC 5322 (un)?structured parser!
    * TODO Note this correlats with addrspec_with_guts() which is in front
    * TODO of us and encapsulates (what it thinks is, sigh) the address
    * TODO boundary.  ALL THIS should be one object that knows how to deal */
   flags &= a_RESET_MASK;
   for (p = addr; (c.c = *p++) != '\0';) {
      if (c.c == '"') {
         flags ^= a_IN_QUOTE;
      } else if (c.u < 040 || c.u >= 0177) { /* TODO no magics: !bodychar()? */
#ifdef HAVE_IDNA
         if ((flags & (a_IN_DOMAIN | a_IDNA_ENABLE)) ==
               (a_IN_DOMAIN | a_IDNA_ENABLE))
            flags |= a_IDNA_APPLY;
         else
#endif
            break;
      } else if ((flags & a_DOMAIN_MASK) == a_DOMAIN_MASK) {
         if ((c.c == ']' && *p != '\0') || c.c == '\\' || whitechar(c.c))
            break;
      } else if ((flags & (a_IN_QUOTE | a_DOMAIN_MASK)) == a_IN_QUOTE) {
         /*EMPTY*/;
      } else if (c.c == '\\' && *p != '\0') {
         ++p;
      } else if (c.c == '@') {
         if(flags & a_IN_AT){
            NAME_ADDRSPEC_ERR_SET(agp->ag_n_flags, NAME_ADDRSPEC_ERR_ATSEQ,
               c.u);
            goto jleave;
         }
         agp->ag_sdom_start = PTR2SIZE(p - addr);
         agp->ag_n_flags |= NAME_ADDRSPEC_ISADDR; /* TODO .. really? */
         flags &= ~a_DOMAIN_MASK;
         flags |= (*p == '[') ? a_IN_AT | a_IN_DOMAIN | a_DOMAIN_V6
               : a_IN_AT | a_IN_DOMAIN;
         continue;
      }
      /* TODO This interferes with our alias handling, which allows :!
       * TODO Update manual on support (search the several ALIASCOLON)! */
      else if (c.c == '(' || c.c == ')' || c.c == '<' || c.c == '>' ||
            c.c == '[' || c.c == ']' || c.c == ':' || c.c == ';' ||
            c.c == '\\' || c.c == ',' || blankchar(c.c))
         break;
      flags &= ~a_IN_AT;
   }
   if (c.c != '\0') {
      NAME_ADDRSPEC_ERR_SET(agp->ag_n_flags, NAME_ADDRSPEC_ERR_CHAR, c.u);
      goto jleave;
   }

   /* If we do not think this is an address we may treat it as an alias name
    * if and only if the original input is identical to the skinned version */
   if(!(agp->ag_n_flags & NAME_ADDRSPEC_ISADDR) &&
         !strcmp(agp->ag_skinned, agp->ag_input)){
      /* TODO This may be an UUCP address */
      agp->ag_n_flags |= NAME_ADDRSPEC_ISNAME;
      if(!n_alias_is_valid_name(agp->ag_input))
         NAME_ADDRSPEC_ERR_SET(agp->ag_n_flags, NAME_ADDRSPEC_ERR_NAME, '.');
   }else{
      /* If we seem to know that this is an address.  Ensure this is correct
       * according to RFC 5322 TODO the entire address parser should be like
       * TODO that for one, and then we should know whether structured or
       * TODO unstructured, and just parse correctly overall!
       * TODO In addition, this can be optimised a lot.
       * TODO And it is far from perfect: it should not forget whether no
       * TODO whitespace followed some snippet, and it was written hastily.
       * TODO It is even wrong sometimes.  Not only for strange cases */
      struct a_token{
         struct a_token *t_last;
         struct a_token *t_next;
         enum{
            a_T_TATOM = 1u<<0,
            a_T_TCOMM = 1u<<1,
            a_T_TQUOTE = 1u<<2,
            a_T_TADDR = 1u<<3,
            a_T_TMASK = (1u<<4) - 1,

            a_T_SPECIAL = 1u<<8     /* An atom actually needs to go TQUOTE */
         } t_f;
         ui8_t t__pad[4];
         size_t t_start;
         size_t t_end;
      } *thead, *tcurr, *tp;

      struct n_string ost, *ostp;
      char const *cp, *cp1st, *cpmax, *xp;
      void *lofi_snap;

      /* Name and domain must be non-empty */
      if(*addr == '@' || &addr[2] >= p || p[-2] == '@'){
jeat:
         c.c = '@';
         NAME_ADDRSPEC_ERR_SET(agp->ag_n_flags, NAME_ADDRSPEC_ERR_ATSEQ, c.u);
         goto jleave;
      }

      cp = agp->ag_input;

      /* Nothing to do if there is only an address (in angle brackets) */
      /* TODO This is wrong since we allow invalid constructs in local-part
       * TODO and domain, AT LEAST in so far as a"bc"d@abc should become
       * TODO "abcd"@abc.  Etc. */
      if(agp->ag_iaddr_start == 0){
         /* No @ seen? */
         if(!(agp->ag_n_flags & NAME_ADDRSPEC_ISADDR))
            goto jeat;
         if(agp->ag_iaddr_aend == agp->ag_ilen)
            goto jleave;
      }else if(agp->ag_iaddr_start == 1 && *cp == '<' &&
            agp->ag_iaddr_aend == agp->ag_ilen - 1 &&
            cp[agp->ag_iaddr_aend] == '>'){
         /* No @ seen?  Possibly insert n_nodename() */
         if(!(agp->ag_n_flags & NAME_ADDRSPEC_ISADDR)){
            cp = &agp->ag_input[agp->ag_iaddr_start];
            cpmax = &agp->ag_input[agp->ag_iaddr_aend];
            goto jinsert_domain;
         }
         goto jleave;
      }

      /* It is not, so parse off all tokens, then resort and rejoin */
      lofi_snap = n_lofi_snap_create();

      cp1st = cp;
      if((c.ui32 = agp->ag_iaddr_start) > 0)
         --c.ui32;
      cpmax = &cp[c.ui32];

      thead = tcurr = NULL;
jnode_redo:
      for(tp = NULL; cp < cpmax;){
         switch((c.c = *cp)){
         case '(':
            if(tp != NULL)
               tp->t_end = PTR2SIZE(cp - cp1st);
            tp = n_lofi_alloc(sizeof *tp);
            tp->t_next = NULL;
            if((tp->t_last = tcurr) != NULL)
               tcurr->t_next = tp;
            else
               thead = tp;
            tcurr = tp;
            tp->t_f = a_T_TCOMM;
            tp->t_start = PTR2SIZE(++cp - cp1st);
            xp = skip_comment(cp);
            tp->t_end = PTR2SIZE(xp - cp1st);
            cp = xp;
            if(tp->t_end > tp->t_start){
               if(xp[-1] == ')')
                  --tp->t_end;
               else{
                  /* No closing comment - strip trailing whitespace */
                  while(blankchar(*--xp))
                     if(--tp->t_end == tp->t_start)
                        break;
               }
            }
            tp = NULL;
            break;

         case '"':
            if(tp != NULL)
               tp->t_end = PTR2SIZE(cp - cp1st);
            tp = n_lofi_alloc(sizeof *tp);
            tp->t_next = NULL;
            if((tp->t_last = tcurr) != NULL)
               tcurr->t_next = tp;
            else
               thead = tp;
            tcurr = tp;
            tp->t_f = a_T_TQUOTE;
            tp->t_start = PTR2SIZE(++cp - cp1st);
            for(xp = cp; xp < cpmax; ++xp){
               if((c.c = *xp) == '"')
                  break;
               if(c.c == '\\' && xp[1] != '\0')
                  ++xp;
            }
            tp->t_end = PTR2SIZE(xp - cp1st);
            cp = &xp[1];
            if(tp->t_end > tp->t_start){
               /* No closing quote - strip trailing whitespace */
               if(*xp != '"'){
                  while(blankchar(*xp--))
                     if(--tp->t_end == tp->t_start)
                        break;
               }
            }
            tp = NULL;
            break;

         default:
            if(blankchar(c.c)){
               if(tp != NULL)
                  tp->t_end = PTR2SIZE(cp - cp1st);
               tp = NULL;
               ++cp;
               break;
            }

            if(tp == NULL){
               tp = n_lofi_alloc(sizeof *tp);
               tp->t_next = NULL;
               if((tp->t_last = tcurr) != NULL)
                  tcurr->t_next = tp;
               else
                  thead = tp;
               tcurr = tp;
               tp->t_f = a_T_TATOM;
               tp->t_start = PTR2SIZE(cp - cp1st);
            }
            ++cp;

            /* Reverse solidus transforms the following into a quoted-pair, and
             * therefore (must occur in comment or quoted-string only) the
             * entire atom into a quoted string */
            if(c.c == '\\'){
               tp->t_f |= a_T_SPECIAL;
               if(cp < cpmax)
                  ++cp;
               break;
            }

            /* Is this plain RFC 5322 "atext", or "specials"?
             * TODO Because we don't know structured/unstructured, nor anything
             * TODO else, we need to treat "dot-atom" as being identical to
             * TODO "specials".
             * However, if the 8th bit is set, this will be RFC 2047 converted
             * and the entire sequence is skipped */
            if(!(c.u & 0x80) && !alnumchar(c.c) &&
                  c.c != '!' && c.c != '#' && c.c != '$' && c.c != '%' &&
                  c.c != '&' && c.c != '\'' && c.c != '*' && c.c != '+' &&
                  c.c != '-' && c.c != '/' && c.c != '=' && c.c != '?' &&
                  c.c != '^' && c.c != '_' && c.c != '`' && c.c != '{' &&
                  c.c != '}' && c.c != '|' && c.c != '}' && c.c != '~')
               tp->t_f |= a_T_SPECIAL;
            break;
         }
      }
      if(tp != NULL)
         tp->t_end = PTR2SIZE(cp - cp1st);

      if(!(flags & a_REDO_NODE_AFTER_ADDR)){
         flags |= a_REDO_NODE_AFTER_ADDR;

         /* The local-part may be in quotes.. */
         if((tp = tcurr) != NULL && (tp->t_f & a_T_TQUOTE) &&
               tp->t_end == agp->ag_iaddr_start - 1){
            /* ..so backward extend it, including the starting quote */
            /* TODO This is false and the code below #if 0 away.  We would
             * TODO need to create a properly quoted local-part HERE AND NOW
             * TODO and REPLACE the original data with that version, but the
             * TODO current code cannot do that.  The node needs the data,
             * TODO not only offsets for that, for example.  If we had all that
             * TODO the code below could produce a really valid thing */
            if(tp->t_start > 0)
               --tp->t_start;
            if(tp->t_start > 0 &&
                  (tp->t_last == NULL || tp->t_last->t_end < tp->t_start) &&
                     agp->ag_input[tp->t_start - 1] == '\\')
               --tp->t_start;
            tp->t_f = a_T_TADDR | a_T_SPECIAL;
         }else{
            tp = n_lofi_alloc(sizeof *tp);
            tp->t_next = NULL;
            if((tp->t_last = tcurr) != NULL)
               tcurr->t_next = tp;
            else
               thead = tp;
            tcurr = tp;
            tp->t_f = a_T_TADDR;
            tp->t_start = agp->ag_iaddr_start;
            /* TODO Very special case because of our hacky non-object-based and
             * TODO non-compliant address parser.  Note */
            if(tp->t_last == NULL && tp->t_start > 0)
               tp->t_start = 0;
            if(agp->ag_input[tp->t_start] == '<')
               ++tp->t_start;

            /* TODO Very special check for whether we need to massage the
             * TODO local part.  This is wrong, but otherwise even more so */
#if 0
            cp = &agp->ag_input[tp->t_start];
            cpmax = &agp->ag_input[agp->ag_iaddr_aend];
            while(cp < cpmax){
               c.c = *cp++;
               if(!(c.u & 0x80) && !alnumchar(c.c) &&
                     c.c != '!' && c.c != '#' && c.c != '$' && c.c != '%' &&
                     c.c != '&' && c.c != '\'' && c.c != '*' && c.c != '+' &&
                     c.c != '-' && c.c != '/' && c.c != '=' && c.c != '?' &&
                     c.c != '^' && c.c != '_' && c.c != '`' && c.c != '{' &&
                     c.c != '}' && c.c != '|' && c.c != '}' && c.c != '~'){
                  tp->t_f |= a_T_SPECIAL;
                  break;
               }
            }
#endif
         }
         tp->t_end = agp->ag_iaddr_aend;
         assert(tp->t_start <= tp->t_end);
         tp = NULL;

         cp = &agp->ag_input[agp->ag_iaddr_aend + 1];
         cpmax = &agp->ag_input[agp->ag_ilen];
         if(cp < cpmax)
            goto jnode_redo;
      }

      /* Nothing may follow the address, move it to the end */
      assert(tcurr != NULL);
      if(tcurr != NULL && !(tcurr->t_f & a_T_TADDR)){
         for(tp = thead; tp != NULL; tp = tp->t_next){
            if(tp->t_f & a_T_TADDR){
               if(tp->t_last != NULL)
                  tp->t_last->t_next = tp->t_next;
               else
                  thead = tp->t_next;
               if(tp->t_next != NULL)
                  tp->t_next->t_last = tp->t_last;

               tcurr = tp;
               while(tp->t_next != NULL)
                  tp = tp->t_next;
               tp->t_next = tcurr;
               tcurr->t_last = tp;
               tcurr->t_next = NULL;
               break;
            }
         }
      }

      /* Make ranges contiguous: ensure a continuous range of atoms is converted
       * to a SPECIAL one if at least one of them requires it */
      for(tp = thead; tp != NULL; tp = tp->t_next){
         if(tp->t_f & a_T_SPECIAL){
            tcurr = tp;
            while((tp = tp->t_last) != NULL && (tp->t_f & a_T_TATOM))
               tp->t_f |= a_T_SPECIAL;
            tp = tcurr;
            while((tp = tp->t_next) != NULL && (tp->t_f & a_T_TATOM))
               tp->t_f |= a_T_SPECIAL;
            if(tp == NULL)
               break;
         }
      }

      /* And yes, we want quotes to extend as much as possible */
      for(tp = thead; tp != NULL; tp = tp->t_next){
         if(tp->t_f & a_T_TQUOTE){
            tcurr = tp;
            while((tp = tp->t_last) != NULL && (tp->t_f & a_T_TATOM))
               tp->t_f |= a_T_SPECIAL;
            tp = tcurr;
            while((tp = tp->t_next) != NULL && (tp->t_f & a_T_TATOM))
               tp->t_f |= a_T_SPECIAL;
            if(tp == NULL)
               break;
         }
      }

      /* Then rejoin */
      ostp = n_string_creat_auto(&ost);
      if((c.ui32 = agp->ag_ilen) <= UI32_MAX >> 1)
         ostp = n_string_reserve(ostp, c.ui32 <<= 1);

      for(tcurr = thead; tcurr != NULL;){
         if(tcurr != thead)
            ostp = n_string_push_c(ostp, ' ');
         if(tcurr->t_f & a_T_TADDR){
            if(tcurr->t_last != NULL)
               ostp = n_string_push_c(ostp, '<');
            agp->ag_iaddr_start = ostp->s_len;
            /* Now it is terrible to say, but if that thing contained
             * quotes, then those may contain quoted-pairs! */
#if 0
            if(!(tcurr->t_f & a_T_SPECIAL)){
#endif
               ostp = n_string_push_buf(ostp, &cp1st[tcurr->t_start],
                     (tcurr->t_end - tcurr->t_start));
#if 0
            }else{
               bool_t quot, esc;

               ostp = n_string_push_c(ostp, '"');
               quot = TRU1;

               cp = &cp1st[tcurr->t_start];
               cpmax = &cp1st[tcurr->t_end];
               for(esc = FAL0; cp < cpmax;){
                  if((c.c = *cp++) == '\\' && !esc){
                     if(cp < cpmax && (*cp == '"' || *cp == '\\'))
                        esc = TRU1;
                  }else{
                     if(esc || c.c == '"')
                        ostp = n_string_push_c(ostp, '\\');
                     else if(c.c == '@'){
                        ostp = n_string_push_c(ostp, '"');
                        quot = FAL0;
                     }
                     ostp = n_string_push_c(ostp, c.c);
                     esc = FAL0;
                  }
               }
            }
#endif
            agp->ag_iaddr_aend = ostp->s_len;

            if(tcurr->t_last != NULL)
               ostp = n_string_push_c(ostp, '>');
            tcurr = tcurr->t_next;
         }else if(tcurr->t_f & a_T_TCOMM){
            ostp = n_string_push_c(ostp, '(');
            ostp = n_string_push_buf(ostp, &cp1st[tcurr->t_start],
                  (tcurr->t_end - tcurr->t_start));
            while((tp = tcurr->t_next) != NULL && (tp->t_f & a_T_TCOMM)){
               tcurr = tp;
               ostp = n_string_push_c(ostp, ' '); /* XXX may be artificial */
               ostp = n_string_push_buf(ostp, &cp1st[tcurr->t_start],
                     (tcurr->t_end - tcurr->t_start));
            }
            ostp = n_string_push_c(ostp, ')');
            tcurr = tcurr->t_next;
         }else if(tcurr->t_f & a_T_TQUOTE){
jput_quote:
            ostp = n_string_push_c(ostp, '"');
            tp = tcurr;
            do/* while tcurr && TATOM||TQUOTE */{
               cp = &cp1st[tcurr->t_start];
               cpmax = &cp1st[tcurr->t_end];
               if(cp == cpmax)
                  continue;

               if(tcurr != tp)
                  ostp = n_string_push_c(ostp, ' ');

               if((tcurr->t_f & (a_T_TATOM | a_T_SPECIAL)) == a_T_TATOM)
                  ostp = n_string_push_buf(ostp, cp, PTR2SIZE(cpmax - cp));
               else{
                  bool_t esc;

                  for(esc = FAL0; cp < cpmax;){
                     if((c.c = *cp++) == '\\' && !esc){
                        if(cp < cpmax && (*cp == '"' || *cp == '\\'))
                           esc = TRU1;
                     }else{
                        if(esc || c.c == '"'){
jput_quote_esc:
                           ostp = n_string_push_c(ostp, '\\');
                        }
                        ostp = n_string_push_c(ostp, c.c);
                        esc = FAL0;
                     }
                  }
                  if(esc){
                     c.c = '\\';
                     goto jput_quote_esc;
                  }
               }
            }while((tcurr = tcurr->t_next) != NULL &&
               (tcurr->t_f & (a_T_TATOM | a_T_TQUOTE)));
            ostp = n_string_push_c(ostp, '"');
         }else if(tcurr->t_f & a_T_SPECIAL)
            goto jput_quote;
         else{
            /* Can we use a fast join mode? */
            for(tp = tcurr; tcurr != NULL; tcurr = tcurr->t_next){
               if(!(tcurr->t_f & a_T_TATOM))
                  break;
               if(tcurr != tp)
                  ostp = n_string_push_c(ostp, ' ');
               ostp = n_string_push_buf(ostp, &cp1st[tcurr->t_start],
                     (tcurr->t_end - tcurr->t_start));
            }
         }
      }

      n_lofi_snap_unroll(lofi_snap);

      agp->ag_input = n_string_cp(ostp);
      agp->ag_ilen = ostp->s_len;
      /*ostp = n_string_drop_ownership(ostp);*/

      /* Name and domain must be non-empty, the second */
      cp = &agp->ag_input[agp->ag_iaddr_start];
      cpmax = &agp->ag_input[agp->ag_iaddr_aend];
      if(*cp == '@' || &cp[2] > cpmax || cpmax[-1] == '@'){
         c.c = '@';
         NAME_ADDRSPEC_ERR_SET(agp->ag_n_flags, NAME_ADDRSPEC_ERR_ATSEQ, c.u);
         goto jleave;
      }

      addr = agp->ag_skinned = savestrbuf(cp, PTR2SIZE(cpmax - cp));

      /* TODO This parser is a mess.  We do not know whether this is truly
       * TODO valid, and all our checks are not truly RFC conforming.
       * TODO Do check the skinned thing by itself once more, in order
       * TODO to catch problems from reordering, e.g., this additional
       * TODO test catches a final address without AT..
       * TODO This is a plain copy+paste of the weird thing above, no care */
      agp->ag_n_flags &= ~NAME_ADDRSPEC_ISADDR;
      flags &= a_RESET_MASK;
      for (p = addr; (c.c = *p++) != '\0';) {
         if(c.c == '"')
            flags ^= a_IN_QUOTE;
         else if (c.u < 040 || c.u >= 0177) {
#ifdef HAVE_IDNA
               if(!(flags & a_IN_DOMAIN))
#endif
                  break;
         } else if ((flags & a_DOMAIN_MASK) == a_DOMAIN_MASK) {
            if ((c.c == ']' && *p != '\0') || c.c == '\\' || whitechar(c.c))
               break;
         } else if ((flags & (a_IN_QUOTE | a_DOMAIN_MASK)) == a_IN_QUOTE) {
            /*EMPTY*/;
         } else if (c.c == '\\' && *p != '\0') {
            ++p;
         } else if (c.c == '@') {
            if(flags & a_IN_AT){
               NAME_ADDRSPEC_ERR_SET(agp->ag_n_flags, NAME_ADDRSPEC_ERR_ATSEQ,
                  c.u);
               goto jleave;
            }
            flags |= a_IN_AT;
            agp->ag_n_flags |= NAME_ADDRSPEC_ISADDR; /* TODO .. really? */
            flags &= ~a_DOMAIN_MASK;
            flags |= (*p == '[') ? a_IN_DOMAIN | a_DOMAIN_V6 : a_IN_DOMAIN;
            continue;
         } else if (c.c == '(' || c.c == ')' || c.c == '<' || c.c == '>' ||
               c.c == '[' || c.c == ']' || c.c == ':' || c.c == ';' ||
               c.c == '\\' || c.c == ',' || blankchar(c.c))
            break;
         flags &= ~a_IN_AT;
      }
      if(c.c != '\0')
         NAME_ADDRSPEC_ERR_SET(agp->ag_n_flags, NAME_ADDRSPEC_ERR_CHAR, c.u);
      else if(!(agp->ag_n_flags & NAME_ADDRSPEC_ISADDR)){
         /* This is not an address, but if we had seen angle brackets convert
          * it to a n_nodename() address if the name is a valid user */
jinsert_domain:
         if(cp > &agp->ag_input[0] && cp[-1] == '<' &&
               cpmax <= &agp->ag_input[agp->ag_ilen] && cpmax[0] == '>' &&
               (!strcmp(addr, ok_vlook(LOGNAME)) || getpwnam(addr) != NULL)){
            /* XXX However, if hostname is set to the empty string this
             * XXX indicates that the used *mta* will perform the
             * XXX auto-expansion instead.  Not so with `addrcodec' though */
            agp->ag_n_flags |= NAME_ADDRSPEC_ISADDR;
            if(!issingle_hack &&
                  (cp = ok_vlook(hostname)) != NULL && *cp == '\0')
               agp->ag_n_flags |= NAME_ADDRSPEC_WITHOUT_DOMAIN;
            else{
               c.ui32 = strlen(cp = n_nodename(TRU1));
               /* This is yet IDNA converted.. */
               ostp = n_string_creat_auto(&ost);
               ostp = n_string_assign_buf(ostp, agp->ag_input, agp->ag_ilen);
               ostp = n_string_insert_c(ostp, agp->ag_iaddr_aend++, '@');
               ostp = n_string_insert_buf(ostp, agp->ag_iaddr_aend, cp,
                     c.ui32);
               agp->ag_iaddr_aend += c.ui32;
               agp->ag_input = n_string_cp(ostp);
               agp->ag_ilen = ostp->s_len;
               /*ostp = n_string_drop_ownership(ostp);*/

               cp = &agp->ag_input[agp->ag_iaddr_start];
               cpmax = &agp->ag_input[agp->ag_iaddr_aend];
               agp->ag_skinned = savestrbuf(cp, PTR2SIZE(cpmax - cp));
            }
         }else
            NAME_ADDRSPEC_ERR_SET(agp->ag_n_flags, NAME_ADDRSPEC_ERR_ATSEQ,
               '@');
      }
   }

jleave:
#ifdef HAVE_IDNA
   if(!(agp->ag_n_flags & NAME_ADDRSPEC_INVALID) && (flags & a_IDNA_APPLY))
      agp = a_head_idna_apply(agp);
#endif
   NYD_LEAVE;
   return !(agp->ag_n_flags & NAME_ADDRSPEC_INVALID);
}

static long
a_gethfield(FILE *f, char **linebuf, size_t *linesize, long rem, char **colon,
   bool_t support_sh_comment)
{
   char *line2 = NULL, *cp, *cp2;
   size_t line2size = 0;
   int c, isenc;
   NYD2_ENTER;

   if (*linebuf == NULL)
      *linebuf = n_realloc(*linebuf, *linesize = 1);
   **linebuf = '\0';
   for (;;) {
      if (--rem < 0) {
         rem = -1;
         break;
      }
      if ((c = readline_restart(f, linebuf, linesize, 0)) <= 0) {
         rem = -1;
         break;
      }
      if(support_sh_comment && **linebuf == '#'){
         /* A comment may be last before body, too, ensure empty last line */
         **linebuf = '\0';
         continue;
      }

      for (cp = *linebuf; fieldnamechar(*cp); ++cp)
         ;
      if (cp > *linebuf)
         while (blankchar(*cp))
            ++cp;
      if (*cp != ':' || cp == *linebuf)
         continue;

      /* I guess we got a headline.  Handle wraparound */
      *colon = cp;
      cp = *linebuf + c;
      for (;;) {
         isenc = 0;
         while (PTRCMP(--cp, >=, *linebuf) && blankchar(*cp))
            ;
         cp++;
         if (rem <= 0)
            break;
         if (PTRCMP(cp - 8, >=, *linebuf) && cp[-1] == '=' && cp[-2] == '?')
            isenc |= 1;
         ungetc(c = getc(f), f);
         if (!blankchar(c))
            break;
         c = readline_restart(f, &line2, &line2size, 0); /* TODO linepool! */
         if (c < 0)
            break;
         --rem;
         for (cp2 = line2; blankchar(*cp2); ++cp2)
            ;
         c -= (int)PTR2SIZE(cp2 - line2);
         if (cp2[0] == '=' && cp2[1] == '?' && c > 8)
            isenc |= 2;
         if (PTRCMP(cp + c, >=, *linebuf + *linesize - 2)) {
            size_t diff = PTR2SIZE(cp - *linebuf),
               colondiff = PTR2SIZE(*colon - *linebuf);
            *linebuf = n_realloc(*linebuf, *linesize += c + 2);
            cp = &(*linebuf)[diff];
            *colon = &(*linebuf)[colondiff];
         }
         if (isenc != 3)
            *cp++ = ' ';
         memcpy(cp, cp2, c);
         cp += c;
      }
      *cp = '\0';

      if (line2 != NULL)
         n_free(line2);
      break;
   }
   NYD2_LEAVE;
   return rem;
}

static int
msgidnextc(char const **cp, int *status)
{
   int c;
   NYD2_ENTER;

   assert(cp != NULL);
   assert(*cp != NULL);
   assert(status != NULL);

   for (;;) {
      if (*status & 01) {
         if (**cp == '"') {
            *status &= ~01;
            (*cp)++;
            continue;
         }
         if (**cp == '\\') {
            (*cp)++;
            if (**cp == '\0')
               goto jeof;
         }
         goto jdfl;
      }
      switch (**cp) {
      case '(':
         *cp = skip_comment(&(*cp)[1]);
         continue;
      case '>':
      case '\0':
jeof:
         c = '\0';
         goto jleave;
      case '"':
         (*cp)++;
         *status |= 01;
         continue;
      case '@':
         *status |= 02;
         /*FALLTHRU*/
      default:
jdfl:
         c = *(*cp)++ & 0377;
         c = (*status & 02) ? lowerconv(c) : c;
         goto jleave;
      }
   }
jleave:
   NYD2_LEAVE;
   return c;
}

static char const *
nexttoken(char const *cp)
{
   NYD2_ENTER;
   for (;;) {
      if (*cp == '\0') {
         cp = NULL;
         break;
      }

      if (*cp == '(') {
         size_t nesting = 1;

         do switch (*++cp) {
         case '(':
            ++nesting;
            break;
         case ')':
            --nesting;
            break;
         } while (nesting > 0 && *cp != '\0'); /* XXX error? */
      } else if (blankchar(*cp) || *cp == ',')
         ++cp;
      else
         break;
   }
   NYD2_LEAVE;
   return cp;
}

FL char const *
myaddrs(struct header *hp) /* TODO */
{
   struct name *np;
   char const *rv, *mta;
   NYD_ENTER;

   if (hp != NULL && (np = hp->h_from) != NULL) {
      if ((rv = np->n_fullname) != NULL)
         goto jleave;
      if ((rv = np->n_name) != NULL)
         goto jleave;
   }

   /* Verified once variable had been set */
   if((rv = ok_vlook(from)) != NULL)
      goto jleave;

   /* When invoking *sendmail* directly, it's its task to generate an otherwise
    * undeterminable From: address.  However, if the user sets *hostname*,
    * accept his desire */
   if (ok_vlook(hostname) != NULL)
      goto jnodename;
   if (ok_vlook(smtp) != NULL || /* TODO obsolete -> mta */
         /* TODO pretty hacky for now (this entire fun), later: url_creat()! */
         ((mta = n_servbyname(ok_vlook(mta), NULL)) != NULL && *mta != '\0'))
      goto jnodename;
jleave:
   NYD_LEAVE;
   return rv;

jnodename:{
      char *cp;
      char const *hn, *ln;
      size_t i;

      hn = n_nodename(TRU1);
      ln = ok_vlook(LOGNAME);
      i = strlen(ln) + strlen(hn) + 1 +1;
      rv = cp = n_autorec_alloc(i);
      sstpcpy(sstpcpy(sstpcpy(cp, ln), n_at), hn);
   }
   goto jleave;
}

FL char const *
myorigin(struct header *hp) /* TODO */
{
   char const *rv = NULL, *ccp;
   struct name *np;
   NYD_ENTER;

   if((ccp = myaddrs(hp)) != NULL &&
         (np = lextract(ccp, GEXTRA | GFULL)) != NULL){
      if(np->n_flink == NULL)
         rv = ccp;
      /* Verified upon variable set time */
      else if((ccp = ok_vlook(sender)) != NULL)
         rv = ccp;
      /* TODO why not else rv = n_poption_arg_r; ?? */
   }
   NYD_LEAVE;
   return rv;
}

FL bool_t
is_head(char const *linebuf, size_t linelen, bool_t check_rfc4155)
{
   char date[n_FROM_DATEBUF];
   bool_t rv;
   NYD2_ENTER;

   if ((rv = (linelen >= 5 && !memcmp(linebuf, "From ", 5))) && check_rfc4155 &&
         (extract_date_from_from_(linebuf, linelen, date) <= 0 ||
          !_is_date(date)))
      rv = TRUM1;
   NYD2_LEAVE;
   return rv;
}

FL int
extract_date_from_from_(char const *line, size_t linelen,
   char datebuf[n_FROM_DATEBUF])
{
   int rv;
   char const *cp = line;
   NYD_ENTER;

   rv = 1;

   /* "From " */
   cp = _from__skipword(cp);
   if (cp == NULL)
      goto jerr;
   /* "addr-spec " */
   cp = _from__skipword(cp);
   if (cp == NULL)
      goto jerr;
   if((cp[0] == 't' || cp[0] == 'T') && (cp[1] == 't' || cp[1] == 'T') &&
         (cp[2] == 'y' || cp[2] == 'Y')){
      cp = _from__skipword(cp);
      if (cp == NULL)
         goto jerr;
   }
   /* It seems there are invalid MBOX archives in the wild, compare
    * . http://bugs.debian.org/624111
    * . [Mutt] #3868: mutt should error if the imported mailbox is invalid
    * What they do is that they obfuscate the address to "name at host",
    * and even "name at host dot dom dot dom.
    * The [Aa][Tt] is also RFC 733, so be tolerant */
   else if((cp[0] == 'a' || cp[0] == 'A') && (cp[1] == 't' || cp[1] == 'T') &&
         cp[2] == ' '){
      rv = -1;
      cp += 3;
jat_dot:
      cp = _from__skipword(cp);
      if (cp == NULL)
         goto jerr;
      if((cp[0] == 'd' || cp[0] == 'D') && (cp[1] == 'o' || cp[1] == 'O') &&
            (cp[2] == 't' || cp[2] == 'T') && cp[3] == ' '){
         cp += 4;
         goto jat_dot;
      }
   }

   linelen -= PTR2SIZE(cp - line);
   if (linelen < a_HEAD_DATE_MINLEN)
      goto jerr;
   if (cp[linelen - 1] == '\n') {
      --linelen;
      /* (Rather IMAP/POP3 only) */
      if (cp[linelen - 1] == '\r')
         --linelen;
      if (linelen < a_HEAD_DATE_MINLEN)
         goto jerr;
   }
   if (linelen >= n_FROM_DATEBUF)
      goto jerr;

jleave:
   memcpy(datebuf, cp, linelen);
   datebuf[linelen] = '\0';
   NYD_LEAVE;
   return rv;
jerr:
   cp = _("<Unknown date>");
   linelen = strlen(cp);
   if (linelen >= n_FROM_DATEBUF)
      linelen = n_FROM_DATEBUF;
   rv = 0;
   goto jleave;
}

FL void
n_header_extract(FILE *fp, struct header *hp, bool_t extended_list_of,
      si8_t *checkaddr_err)
{
   struct n_header_field **hftail;
   struct header nh, *hq = &nh;
   char *linebuf = NULL /* TODO line pool */, *colon;
   size_t linesize = 0, seenfields = 0;
   int c;
   long lc;
   char const *val, *cp;
   NYD_ENTER;

   memset(hq, 0, sizeof *hq);
   if(extended_list_of > FAL0){
      hq->h_to = hp->h_to;
      hq->h_cc = hp->h_cc;
      hq->h_bcc = hp->h_bcc;
   }
   hftail = &hq->h_user_headers;

   for (lc = 0; readline_restart(fp, &linebuf, &linesize, 0) > 0; ++lc)
      ;
   rewind(fp);

   /* TODO yippieia, cat(check(lextract)) :-) */
   while ((lc = a_gethfield(fp, &linebuf, &linesize, lc, &colon,
         extended_list_of)) >= 0) {
      struct name *np;

      /* We explicitly allow EAF_NAME for some addressees since aliases are not
       * yet expanded when we parse these! */
      if ((val = thisfield(linebuf, "to")) != NULL) {
         ++seenfields;
         hq->h_to = cat(hq->h_to, checkaddrs(lextract(val, GTO | GFULL),
               EACM_NORMAL | EAF_NAME | EAF_MAYKEEP, checkaddr_err));
      } else if ((val = thisfield(linebuf, "cc")) != NULL) {
         ++seenfields;
         hq->h_cc = cat(hq->h_cc, checkaddrs(lextract(val, GCC | GFULL),
               EACM_NORMAL | EAF_NAME | EAF_MAYKEEP, checkaddr_err));
      } else if ((val = thisfield(linebuf, "bcc")) != NULL) {
         ++seenfields;
         hq->h_bcc = cat(hq->h_bcc, checkaddrs(lextract(val, GBCC | GFULL),
               EACM_NORMAL | EAF_NAME | EAF_MAYKEEP, checkaddr_err));
      } else if ((val = thisfield(linebuf, "from")) != NULL) {
         if(extended_list_of > FAL0){
            ++seenfields;
            hq->h_from = cat(hq->h_from,
                  checkaddrs(lextract(val, GEXTRA | GFULL | GFULLEXTRA),
                     EACM_STRICT, NULL));
         }
      } else if ((val = thisfield(linebuf, "reply-to")) != NULL) {
         ++seenfields;
         hq->h_reply_to = cat(hq->h_reply_to,
               checkaddrs(lextract(val, GEXTRA | GFULL), EACM_STRICT, NULL));
      } else if ((val = thisfield(linebuf, "sender")) != NULL) {
         if(extended_list_of > FAL0){
            ++seenfields;
            hq->h_sender = cat(hq->h_sender, /* TODO cat? check! */
                  checkaddrs(lextract(val, GEXTRA | GFULL | GFULLEXTRA),
                     EACM_STRICT, NULL));
         } else
            goto jebadhead;
      } else if ((val = thisfield(linebuf, "subject")) != NULL ||
            (val = thisfield(linebuf, "subj")) != NULL) {
         ++seenfields;
         for (cp = val; blankchar(*cp); ++cp)
            ;
         hq->h_subject = (hq->h_subject != NULL)
               ? save2str(hq->h_subject, cp) : savestr(cp);
      }
      /* The remaining are mostly hacked in and thus TODO -- at least in
       * TODO respect to their content checking */
      else if((val = thisfield(linebuf, "message-id")) != NULL){
         if(extended_list_of){
            np = checkaddrs(lextract(val, GREF),
                  /*EACM_STRICT | TODO '/' valid!! */ EACM_NOLOG | EACM_NONAME,
                  NULL);
            if (np == NULL || np->n_flink != NULL)
               goto jebadhead;
            ++seenfields;
            hq->h_message_id = np;
         }else
            goto jebadhead;
      }else if((val = thisfield(linebuf, "in-reply-to")) != NULL){
         if(extended_list_of){
            np = checkaddrs(lextract(val, GREF),
                  /*EACM_STRICT | TODO '/' valid!! */ EACM_NOLOG | EACM_NONAME,
                  NULL);
            ++seenfields;
            hq->h_in_reply_to = np;
         }else
            goto jebadhead;
      }else if((val = thisfield(linebuf, "references")) != NULL){
         if(extended_list_of){
            ++seenfields;
            /* TODO Limit number of references TODO better on parser side */
            hq->h_ref = cat(hq->h_ref, checkaddrs(extract(val, GREF),
                  /*EACM_STRICT | TODO '/' valid!! */ EACM_NOLOG | EACM_NONAME,
                  NULL));
         }else
            goto jebadhead;
      }
      /* and that is very hairy */
      else if((val = thisfield(linebuf, "mail-followup-to")) != NULL){
         if(extended_list_of){
            ++seenfields;
            hq->h_mft = cat(hq->h_mft, checkaddrs(lextract(val, GEXTRA | GFULL),
                  /*EACM_STRICT | TODO '/' valid!! | EACM_NOLOG | */EACM_NONAME,
                  checkaddr_err));
         }else
            goto jebadhead;
      }
      /* A free-form header; a_gethfield() did some verification already.. */
      else{
         struct n_header_field *hfp;
         ui32_t nl, bl;
         char const *nstart;

         for(nstart = cp = linebuf;; ++cp)
            if(!fieldnamechar(*cp))
               break;
         nl = (ui32_t)PTR2SIZE(cp - nstart);

         while(blankchar(*cp))
            ++cp;
         if(*cp++ != ':'){
jebadhead:
            n_err(_("Ignoring header field: %s\n"), linebuf);
            continue;
         }
         while(blankchar(*cp))
            ++cp;
         bl = (ui32_t)strlen(cp) +1;

         ++seenfields;
         *hftail =
         hfp = n_autorec_alloc(n_VSTRUCT_SIZEOF(struct n_header_field,
               hf_dat) + nl +1 + bl);
            hftail = &hfp->hf_next;
         hfp->hf_next = NULL;
         hfp->hf_nl = nl;
         hfp->hf_bl = bl - 1;
         memcpy(hfp->hf_dat, nstart, nl);
            hfp->hf_dat[nl++] = '\0';
            memcpy(hfp->hf_dat + nl, cp, bl);
      }
   }

   /* In case the blank line after the header has been edited out.  Otherwise,
    * fetch the header separator */
   if (linebuf != NULL) {
      if (linebuf[0] != '\0') {
         for (cp = linebuf; *(++cp) != '\0';)
            ;
         fseek(fp, (long)-PTR2SIZE(1 + cp - linebuf), SEEK_CUR);
      } else {
         if ((c = getc(fp)) != '\n' && c != EOF)
            ungetc(c, fp);
      }
   }

   if (seenfields > 0 && (checkaddr_err == NULL || *checkaddr_err == 0)) {
      hp->h_to = hq->h_to;
      hp->h_cc = hq->h_cc;
      hp->h_bcc = hq->h_bcc;
      hp->h_from = hq->h_from;
      hp->h_reply_to = hq->h_reply_to;
      hp->h_sender = hq->h_sender;
      if(hq->h_subject != NULL || extended_list_of <= FAL0)
         hp->h_subject = hq->h_subject;
      hp->h_user_headers = hq->h_user_headers;

      if(extended_list_of){
         if(extended_list_of > 0)
            hp->h_ref = hq->h_ref;
         hp->h_message_id = hq->h_message_id;
         hp->h_in_reply_to = hq->h_in_reply_to;
         hp->h_mft = hq->h_mft;

         /* And perform additional validity checks so that we don't bail later
          * on TODO this is good and the place where this should occur,
          * TODO unfortunately a lot of other places do again and blabla */
         if(hp->h_from == NULL)
            hp->h_from = n_poption_arg_r;
         else if(extended_list_of > 0 &&
               hp->h_from->n_flink != NULL && hp->h_sender == NULL)
            hp->h_sender = lextract(ok_vlook(sender),
                  GEXTRA | GFULL | GFULLEXTRA);
      }
   } else
      n_err(_("Restoring deleted header lines\n"));

   if (linebuf != NULL)
      n_free(linebuf);
   NYD_LEAVE;
}

FL char *
hfield_mult(char const *field, struct message *mp, int mult)
{
   FILE *ibuf;
   struct str hfs;
   long lc;
   size_t linesize = 0; /* TODO line pool */
   char *linebuf = NULL, *colon;
   char const *hfield;
   NYD_ENTER;

   /* There are (spam) messages which have header bytes which are many KB when
    * joined, so resize a single heap storage until we are done if we shall
    * collect a field that may have multiple bodies; only otherwise use the
    * string dope directly */
   memset(&hfs, 0, sizeof hfs);

   if ((ibuf = setinput(&mb, mp, NEED_HEADER)) == NULL)
      goto jleave;
   if ((lc = mp->m_lines - 1) < 0)
      goto jleave;

   if ((mp->m_flag & MNOFROM) == 0 &&
         readline_restart(ibuf, &linebuf, &linesize, 0) < 0)
      goto jleave;
   while (lc > 0) {
      if ((lc = a_gethfield(ibuf, &linebuf, &linesize, lc, &colon, FAL0)) < 0)
         break;
      if ((hfield = thisfield(linebuf, field)) != NULL && *hfield != '\0') {
         if (mult)
            n_str_add_buf(&hfs, hfield, strlen(hfield));
         else {
            hfs.s = savestr(hfield);
            break;
         }
      }
   }

jleave:
   if (linebuf != NULL)
      n_free(linebuf);
   if (mult && hfs.s != NULL) {
      colon = savestrbuf(hfs.s, hfs.l);
      n_free(hfs.s);
      hfs.s = colon;
   }
   NYD_LEAVE;
   return hfs.s;
}

FL char const *
thisfield(char const *linebuf, char const *field)
{
   char const *rv = NULL;
   NYD2_ENTER;

   while (lowerconv(*linebuf) == lowerconv(*field)) {
      ++linebuf;
      ++field;
   }
   if (*field != '\0')
      goto jleave;

   while (blankchar(*linebuf))
      ++linebuf;
   if (*linebuf++ != ':')
      goto jleave;

   while (blankchar(*linebuf)) /* TODO header parser..  strip trailing WS?!? */
      ++linebuf;
   rv = linebuf;
jleave:
   NYD2_LEAVE;
   return rv;
}

FL char const *
skip_comment(char const *cp)
{
   size_t nesting;
   NYD_ENTER;

   for (nesting = 1; nesting > 0 && *cp; ++cp) {
      switch (*cp) {
      case '\\':
         if (cp[1])
            ++cp;
         break;
      case '(':
         ++nesting;
         break;
      case ')':
         --nesting;
         break;
      }
   }
   NYD_LEAVE;
   return cp;
}

FL char const *
routeaddr(char const *name)
{
   char const *np, *rp = NULL;
   NYD_ENTER;

   for (np = name; *np; np++) {
      switch (*np) {
      case '(':
         np = skip_comment(np + 1) - 1;
         break;
      case '"':
         while (*np) {
            if (*++np == '"')
               break;
            if (*np == '\\' && np[1])
               np++;
         }
         break;
      case '<':
         rp = np;
         break;
      case '>':
         goto jleave;
      }
   }
   rp = NULL;
jleave:
   NYD_LEAVE;
   return rp;
}

FL enum expand_addr_flags
expandaddr_to_eaf(void)
{
   struct eafdesc {
      char const  *eafd_name;
      bool_t      eafd_is_target;
      ui8_t       eafd_andoff;
      ui8_t       eafd_or;
   } const eafa[] = {
      {"restrict", FAL0, EAF_TARGET_MASK, EAF_RESTRICT | EAF_RESTRICT_TARGETS},
      {"fail", FAL0, EAF_NONE, EAF_FAIL},
      {"failinvaddr", FAL0, EAF_NONE, EAF_FAILINVADDR | EAF_ADDR},
      {"all", TRU1, EAF_NONE, EAF_TARGET_MASK},
         {"file", TRU1, EAF_NONE, EAF_FILE},
         {"pipe", TRU1, EAF_NONE, EAF_PIPE},
         {"name", TRU1, EAF_NONE, EAF_NAME},
         {"addr", TRU1, EAF_NONE, EAF_ADDR}
   }, *eafp;

   char *buf;
   enum expand_addr_flags rv;
   char const *cp;
   NYD2_ENTER;

   if ((cp = ok_vlook(expandaddr)) == NULL)
      rv = EAF_RESTRICT_TARGETS;
   else if (*cp == '\0')
      rv = EAF_TARGET_MASK;
   else {
      rv = EAF_TARGET_MASK;

      for (buf = savestr(cp); (cp = n_strsep(&buf, ',', TRU1)) != NULL;) {
         bool_t minus;

         if ((minus = (*cp == '-')) || *cp == '+')
            ++cp;
         for (eafp = eafa;; ++eafp) {
            if (eafp == eafa + n_NELEM(eafa)) {
               if (n_poption & n_PO_D_V)
                  n_err(_("Unknown *expandaddr* value: %s\n"), cp);
               break;
            } else if (!asccasecmp(cp, eafp->eafd_name)) {
               if (!minus) {
                  rv &= ~eafp->eafd_andoff;
                  rv |= eafp->eafd_or;
               } else {
                  if (eafp->eafd_is_target)
                     rv &= ~eafp->eafd_or;
                  else if (n_poption & n_PO_D_V)
                     n_err(_("minus - prefix invalid for *expandaddr* value: "
                        "%s\n"), --cp);
               }
               break;
            } else if (!asccasecmp(cp, "noalias")) { /* TODO v15 OBSOLETE */
               n_OBSOLETE(_("*expandaddr*: noalias is henceforth -name"));
               rv &= ~EAF_NAME;
               break;
            }
         }
      }

      if((rv & EAF_RESTRICT) && ((n_psonce & n_PSO_INTERACTIVE) ||
            (n_poption & n_PO_TILDE_FLAG)))
         rv |= EAF_TARGET_MASK;
      else if(n_poption & n_PO_D_V){
         if(!(rv & EAF_TARGET_MASK))
            n_err(_("*expandaddr* doesn't allow any addressees\n"));
         else if((rv & EAF_FAIL) && (rv & EAF_TARGET_MASK) == EAF_TARGET_MASK)
            n_err(_("*expandaddr* with fail, but no restrictions to apply\n"));
      }
   }
   NYD2_LEAVE;
   return rv;
}

FL si8_t
is_addr_invalid(struct name *np, enum expand_addr_check_mode eacm)
{
   char cbuf[sizeof "'\\U12340'"];
   char const *cs;
   int f;
   si8_t rv;
   enum expand_addr_flags eaf;
   NYD_ENTER;

   eaf = expandaddr_to_eaf();
   f = np->n_flags;

   if ((rv = ((f & NAME_ADDRSPEC_INVALID) != 0))) {
      if (eaf & EAF_FAILINVADDR)
         rv = -rv;

      if ((eacm & EACM_NOLOG) || (f & NAME_ADDRSPEC_ERR_EMPTY)) {
         ;
      } else {
         ui32_t c;
         char const *fmt = "'\\x%02X'";
         bool_t ok8bit = TRU1;

         if (f & NAME_ADDRSPEC_ERR_IDNA) {
            cs = _("Invalid domain name: %s, character %s\n");
            fmt = "'\\U%04X'";
            ok8bit = FAL0;
         } else if (f & NAME_ADDRSPEC_ERR_ATSEQ)
            cs = _("%s contains invalid %s sequence\n");
         else if (f & NAME_ADDRSPEC_ERR_NAME) {
            cs = _("%s is an invalid alias name\n");
         } else
            cs = _("%s contains invalid byte %s\n");

         c = NAME_ADDRSPEC_ERR_GETWC(f);
         snprintf(cbuf, sizeof cbuf,
            (ok8bit && c >= 040 && c <= 0177 ? "'%c'" : fmt), c);
         goto jprint;
      }
      goto jleave;
   }

   /* *expandaddr* stuff */
   if (!(rv = ((eacm & EACM_MODE_MASK) != EACM_NONE)))
      goto jleave;

   if ((eacm & EACM_STRICT) && (f & NAME_ADDRSPEC_ISFILEORPIPE)) {
      if (eaf & EAF_FAIL)
         rv = -rv;
      cs = _("%s%s: file or pipe addressees not allowed here\n");
      if (eacm & EACM_NOLOG)
         goto jleave;
      else
         goto j0print;
   }

   eaf |= (eacm & EAF_TARGET_MASK);
   if (eacm & EACM_NONAME)
      eaf &= ~EAF_NAME;

   if (eaf == EAF_NONE) {
      rv = FAL0;
      goto jleave;
   }
   if (eaf & EAF_FAIL)
      rv = -rv;

   if (!(eaf & EAF_FILE) && (f & NAME_ADDRSPEC_ISFILE)) {
      cs = _("%s%s: *expandaddr* doesn't allow file target\n");
      if (eacm & EACM_NOLOG)
         goto jleave;
   } else if (!(eaf & EAF_PIPE) && (f & NAME_ADDRSPEC_ISPIPE)) {
      cs = _("%s%s: *expandaddr* doesn't allow command pipe target\n");
      if (eacm & EACM_NOLOG)
         goto jleave;
   } else if (!(eaf & EAF_NAME) && (f & NAME_ADDRSPEC_ISNAME)) {
      cs = _("%s%s: *expandaddr* doesn't allow user name target\n");
      if (eacm & EACM_NOLOG)
         goto jleave;
   } else if (!(eaf & EAF_ADDR) && (f & NAME_ADDRSPEC_ISADDR)) {
      cs = _("%s%s: *expandaddr* doesn't allow mail address target\n");
      if (eacm & EACM_NOLOG)
         goto jleave;
   } else {
      rv = FAL0;
      goto jleave;
   }

j0print:
   cbuf[0] = '\0';
jprint:
   n_err(cs, n_shexp_quote_cp(np->n_name, TRU1), cbuf);
jleave:
   NYD_LEAVE;
   return rv;
}

FL char *
skin(char const *name)
{
   struct n_addrguts ag;
   char *rv;
   NYD_ENTER;

   if(name != NULL){
      /*name =*/ n_addrspec_with_guts(&ag, name, TRU1, FAL0);
      rv = ag.ag_skinned;
      if(!(ag.ag_n_flags & NAME_NAME_SALLOC))
         rv = savestrbuf(rv, ag.ag_slen);
   }else
      rv = NULL;
   NYD_LEAVE;
   return rv;
}

/* TODO addrspec_with_guts: RFC 5322
 * TODO addrspec_with_guts: trim whitespace ETC. ETC. ETC.!!! */
FL char const *
n_addrspec_with_guts(struct n_addrguts *agp, char const *name, bool_t doskin,
      bool_t issingle_hack){
   char const *cp;
   char *cp2, *bufend, *nbuf, c;
   enum{
      a_NONE,
      a_GOTLT = 1<<0,
      a_GOTADDR = 1<<1,
      a_GOTSPACE = 1<<2,
      a_LASTSP = 1<<3
   } flags;
   NYD_ENTER;

   memset(agp, 0, sizeof *agp);

   if((agp->ag_input = name) == NULL || (agp->ag_ilen = strlen(name)) == 0){
      agp->ag_skinned = n_UNCONST(n_empty); /* ok: NAME_SALLOC is not set */
      agp->ag_slen = 0;
      NAME_ADDRSPEC_ERR_SET(agp->ag_n_flags, NAME_ADDRSPEC_ERR_EMPTY, 0);
      goto jleave;
   }else if(!doskin){
      /*agp->ag_iaddr_start = 0;*/
      agp->ag_iaddr_aend = agp->ag_ilen;
      agp->ag_skinned = n_UNCONST(name); /* (NAME_SALLOC not set) */
      agp->ag_slen = agp->ag_ilen;
      agp->ag_n_flags = NAME_SKINNED;
      goto jcheck;
   }

   flags = a_NONE;
   nbuf = n_lofi_alloc(agp->ag_ilen +1);
   /*agp->ag_iaddr_start = 0;*/
   cp2 = bufend = nbuf;

   /* TODO This is complete crap and should use a token parser */
   for(cp = name++; (c = *cp++) != '\0';){
      switch (c) {
      case '(':
         cp = skip_comment(cp);
         flags &= ~a_LASTSP;
         break;
      case '"':
         /* Start of a "quoted-string".  Copy it in its entirety */
         /* XXX RFC: quotes are "semantically invisible"
          * XXX But it was explicitly added (Changelog.Heirloom,
          * XXX [9.23] released 11/15/00, "Do not remove quotes
          * XXX when skinning names"?  No more info.. */
         *cp2++ = c;
         while ((c = *cp) != '\0') { /* TODO improve */
            ++cp;
            if (c == '"') {
               *cp2++ = c;
               break;
            }
            if (c != '\\')
               *cp2++ = c;
            else if ((c = *cp) != '\0') {
               *cp2++ = c;
               ++cp;
            }
         }
         flags &= ~a_LASTSP;
         break;
      case ' ':
      case '\t':
         if((flags & (a_GOTADDR | a_GOTSPACE)) == a_GOTADDR){
            flags |= a_GOTSPACE;
            agp->ag_iaddr_aend = PTR2SIZE(cp - name);
         }
         if (cp[0] == 'a' && cp[1] == 't' && blankchar(cp[2]))
            cp += 3, *cp2++ = '@';
         else if (cp[0] == '@' && blankchar(cp[1]))
            cp += 2, *cp2++ = '@';
         else
            flags |= a_LASTSP;
         break;
      case '<':
         agp->ag_iaddr_start = PTR2SIZE(cp - (name - 1));
         cp2 = bufend;
         flags &= ~(a_GOTSPACE | a_LASTSP);
         flags |= a_GOTLT | a_GOTADDR;
         break;
      case '>':
         if(flags & a_GOTLT){
            /* (_addrspec_check() verifies these later!) */
            flags &= ~(a_GOTLT | a_LASTSP);
            agp->ag_iaddr_aend = PTR2SIZE(cp - name);

            /* Skip over the entire remaining field */
            while((c = *cp) != '\0' && c != ','){
               ++cp;
               if (c == '(')
                  cp = skip_comment(cp);
               else if (c == '"')
                  while ((c = *cp) != '\0') {
                     ++cp;
                     if (c == '"')
                        break;
                     if (c == '\\' && *cp != '\0')
                        ++cp;
                  }
            }
            break;
         }
         /* FALLTHRU */
      default:
         if(flags & a_LASTSP){
            flags &= ~a_LASTSP;
            if(flags & a_GOTADDR)
               *cp2++ = ' ';
         }
         *cp2++ = c;
         /* This character is forbidden here, but it may nonetheless be
          * present: ensure we turn this into something valid!  (E.g., if the
          * next character would be a "..) */
         if(c == '\\' && *cp != '\0')
            *cp2++ = *cp++;
         if(c == ',' && !issingle_hack){
            if(!(flags & a_GOTLT)){
               *cp2++ = ' ';
               for(; blankchar(*cp); ++cp)
                  ;
               flags &= ~a_LASTSP;
               bufend = cp2;
            }
         }else if(!(flags & a_GOTADDR)){
            flags |= a_GOTADDR;
            agp->ag_iaddr_start = PTR2SIZE(cp - name);
         }
      }
   }
   --name;
   agp->ag_slen = PTR2SIZE(cp2 - nbuf);
   if (agp->ag_iaddr_aend == 0)
      agp->ag_iaddr_aend = agp->ag_ilen;
   /* Misses > */
   else if (agp->ag_iaddr_aend < agp->ag_iaddr_start) {
      cp2 = n_autorec_alloc(agp->ag_ilen + 1 +1);
      memcpy(cp2, agp->ag_input, agp->ag_ilen);
      agp->ag_iaddr_aend = agp->ag_ilen;
      cp2[agp->ag_ilen++] = '>';
      cp2[agp->ag_ilen] = '\0';
      agp->ag_input = cp2;
   }
   agp->ag_skinned = savestrbuf(nbuf, agp->ag_slen);
   n_lofi_free(nbuf);
   agp->ag_n_flags = NAME_NAME_SALLOC | NAME_SKINNED;
jcheck:
   if(a_head_addrspec_check(agp, doskin, issingle_hack) <= FAL0)
      name = NULL;
   else
      name = agp->ag_input;
jleave:
   NYD_LEAVE;
   return name;
}

FL char *
realname(char const *name)
{
   char const *cp, *cq, *cstart = NULL, *cend = NULL;
   char *rname, *rp;
   struct str in, out;
   int quoted, good, nogood;
   NYD_ENTER;

   if ((cp = n_UNCONST(name)) == NULL)
      goto jleave;
   for (; *cp != '\0'; ++cp) {
      switch (*cp) {
      case '(':
         if (cstart != NULL) {
            /* More than one comment in address, doesn't make sense to display
             * it without context.  Return the entire field */
            cp = mime_fromaddr(name);
            goto jleave;
         }
         cstart = cp++;
         cp = skip_comment(cp);
         cend = cp--;
         if (cend <= cstart)
            cend = cstart = NULL;
         break;
      case '"':
         while (*cp) {
            if (*++cp == '"')
               break;
            if (*cp == '\\' && cp[1])
               ++cp;
         }
         break;
      case '<':
         if (cp > name) {
            cstart = name;
            cend = cp;
         }
         break;
      case ',':
         /* More than one address. Just use the first one */
         goto jbrk;
      }
   }

jbrk:
   if (cstart == NULL) {
      if (*name == '<') {
         /* If name contains only a route-addr, the surrounding angle brackets
          * don't serve any useful purpose when displaying, so remove */
         cp = prstr(skin(name));
      } else
         cp = mime_fromaddr(name);
      goto jleave;
   }

   /* Strip quotes. Note that quotes that appear within a MIME encoded word are
    * not stripped. The idea is to strip only syntactical relevant things (but
    * this is not necessarily the most sensible way in practice) */
   rp = rname = n_lofi_alloc(PTR2SIZE(cend - cstart +1));
   quoted = 0;
   for (cp = cstart; cp < cend; ++cp) {
      if (*cp == '(' && !quoted) {
         cq = skip_comment(++cp);
         if (PTRCMP(--cq, >, cend))
            cq = cend;
         while (cp < cq) {
            if (*cp == '\\' && PTRCMP(cp + 1, <, cq))
               ++cp;
            *rp++ = *cp++;
         }
      } else if (*cp == '\\' && PTRCMP(cp + 1, <, cend))
         *rp++ = *++cp;
      else if (*cp == '"') {
         quoted = !quoted;
         continue;
      } else
         *rp++ = *cp;
   }
   *rp = '\0';
   in.s = rname;
   in.l = rp - rname;
   mime_fromhdr(&in, &out, TD_ISPR | TD_ICONV);
   n_lofi_free(rname);
   rname = savestr(out.s);
   n_free(out.s);

   while (blankchar(*rname))
      ++rname;
   for (rp = rname; *rp != '\0'; ++rp)
      ;
   while (PTRCMP(--rp, >=, rname) && blankchar(*rp))
      *rp = '\0';
   if (rp == rname) {
      cp = mime_fromaddr(name);
      goto jleave;
   }

   /* mime_fromhdr() has converted all nonprintable characters to question
    * marks now. These and blanks are considered uninteresting; if the
    * displayed part of the real name contains more than 25% of them, it is
    * probably better to display the plain email address instead */
   good = 0;
   nogood = 0;
   for (rp = rname; *rp != '\0' && PTRCMP(rp, <, rname + 20); ++rp)
      if (*rp == '?' || blankchar(*rp))
         ++nogood;
      else
         ++good;
   cp = (good * 3 < nogood) ? prstr(skin(name)) : rname;
jleave:
   NYD_LEAVE;
   return n_UNCONST(cp);
}

FL char *
n_header_senderfield_of(struct message *mp){
   char *cp;
   NYD_ENTER;

   if((cp = hfield1("from", mp)) != NULL && *cp != '\0')
      ;
   else if((cp = hfield1("sender", mp)) != NULL && *cp != '\0')
      ;
   else{
      char *namebuf, *cp2, *linebuf = NULL /* TODO line pool */;
      size_t namesize, linesize = 0;
      FILE *ibuf;
      int f1st = 1;

      /* And fallback only works for MBOX */
      namebuf = n_alloc(namesize = 1);
      namebuf[0] = 0;
      if (mp->m_flag & MNOFROM)
         goto jout;
      if ((ibuf = setinput(&mb, mp, NEED_HEADER)) == NULL)
         goto jout;
      if (readline_restart(ibuf, &linebuf, &linesize, 0) < 0)
         goto jout;

jnewname:
      if (namesize <= linesize)
         namebuf = n_realloc(namebuf, namesize = linesize +1);
      for (cp = linebuf; *cp != '\0' && *cp != ' '; ++cp)
         ;
      for (; blankchar(*cp); ++cp)
         ;
      for (cp2 = namebuf + strlen(namebuf);
           *cp && !blankchar(*cp) && PTRCMP(cp2, <, namebuf + namesize -1);)
         *cp2++ = *cp++;
      *cp2 = '\0';

      if (readline_restart(ibuf, &linebuf, &linesize, 0) < 0)
         goto jout;
      if ((cp = strchr(linebuf, 'F')) == NULL)
         goto jout;
      if (strncmp(cp, "From", 4))
         goto jout;
      if (namesize <= linesize)
         namebuf = n_realloc(namebuf, namesize = linesize + 1);

      /* UUCP from 976 (we do not support anyway!) */
      while ((cp = strchr(cp, 'r')) != NULL) {
         if (!strncmp(cp, "remote", 6)) {
            if ((cp = strchr(cp, 'f')) == NULL)
               break;
            if (strncmp(cp, "from", 4) != 0)
               break;
            if ((cp = strchr(cp, ' ')) == NULL)
               break;
            cp++;
            if (f1st) {
               strncpy(namebuf, cp, namesize);
               f1st = 0;
            } else {
               cp2 = strrchr(namebuf, '!') + 1;
               strncpy(cp2, cp, PTR2SIZE(namebuf + namesize - cp2));
            }
            namebuf[namesize - 2] = '!';
            namebuf[namesize - 1] = '\0';
            goto jnewname;
         }
         cp++;
      }
jout:
      if (*namebuf != '\0' || ((cp = hfield1("return-path", mp))) == NULL ||
            *cp == '\0')
         cp = savestr(namebuf);

      if (linebuf != NULL)
         n_free(linebuf);
      n_free(namebuf);
   }

   NYD_LEAVE;
   return cp;
}

FL char const *
subject_re_trim(char const *s){
   struct{
      ui8_t len;
      char  dat[7];
   }const *pp, ignored[] = { /* Update *reply-strings* manual upon change! */
      {3, "re:"},
      {3, "aw:"}, {5, "antw:"}, /* de */
      {3, "wg:"}, /* Seen too often in the wild */
      {0, ""}
   };

   bool_t any;
   char *re_st, *re_st_x;
   char const *orig_s;
   size_t re_l;
   NYD_ENTER;

   any = FAL0;
   orig_s = s;
   re_st = NULL;
   n_UNINIT(re_l, 0);

   if((re_st_x = ok_vlook(reply_strings)) != NULL &&
         (re_l = strlen(re_st_x)) > 0){
      re_st = n_lofi_alloc(++re_l * 2);
      memcpy(re_st, re_st_x, re_l);
   }

jouter:
   while(*s != '\0'){
      while(spacechar(*s))
         ++s;

      for(pp = ignored; pp->len > 0; ++pp)
         if(is_asccaseprefix(pp->dat, s)){
            s += pp->len;
            any = TRU1;
            goto jouter;
         }

      if(re_st != NULL){
         char *cp;

         memcpy(re_st_x = &re_st[re_l], re_st, re_l);
         while((cp = n_strsep(&re_st_x, ',', TRU1)) != NULL)
            if(is_asccaseprefix(cp, s)){
               s += strlen(cp);
               any = TRU1;
               goto jouter;
            }
      }
      break;
   }

   if(re_st != NULL)
      n_lofi_free(re_st);
   NYD_LEAVE;
   return any ? s : orig_s;
}

FL int
msgidcmp(char const *s1, char const *s2)
{
   int q1 = 0, q2 = 0, c1, c2;
   NYD_ENTER;

   while(*s1 == '<')
      ++s1;
   while(*s2 == '<')
      ++s2;

   do {
      c1 = msgidnextc(&s1, &q1);
      c2 = msgidnextc(&s2, &q2);
      if (c1 != c2)
         break;
   } while (c1 && c2);
   NYD_LEAVE;
   return c1 - c2;
}

FL char const *
fakefrom(struct message *mp)
{
   char const *name;
   NYD_ENTER;

   if (((name = skin(hfield1("return-path", mp))) == NULL || *name == '\0' ) &&
         ((name = skin(hfield1("from", mp))) == NULL || *name == '\0'))
      /* XXX MAILER-DAEMON is what an old MBOX manual page says.
       * RFC 4155 however requires a RFC 5322 (2822) conforming
       * "addr-spec", but we simply can't provide that */
      name = "MAILER-DAEMON";
   NYD_LEAVE;
   return name;
}

#if defined HAVE_IMAP_SEARCH || defined HAVE_IMAP
FL time_t
unixtime(char const *fromline)
{
   char const *fp, *xp;
   time_t t, t2;
   si32_t i, year, month, day, hour, minute, second, tzdiff;
   struct tm *tmptr;
   NYD2_ENTER;

   for (fp = fromline; *fp != '\0' && *fp != '\n'; ++fp)
      ;
   fp -= 24;
   if (PTR2SIZE(fp - fromline) < 7)
      goto jinvalid;
   if (fp[3] != ' ')
      goto jinvalid;
   for (i = 0;;) {
      if (!strncmp(fp + 4, n_month_names[i], 3))
         break;
      if (n_month_names[++i][0] == '\0')
         goto jinvalid;
   }
   month = i + 1;
   if (fp[7] != ' ')
      goto jinvalid;
   n_idec_si32_cp(&day, &fp[8], 10, &xp);
   if (*xp != ' ' || xp != fp + 10)
      goto jinvalid;
   n_idec_si32_cp(&hour, &fp[11], 10, &xp);
   if (*xp != ':' || xp != fp + 13)
      goto jinvalid;
   n_idec_si32_cp(&minute, &fp[14], 10, &xp);
   if (*xp != ':' || xp != fp + 16)
      goto jinvalid;
   n_idec_si32_cp(&second, &fp[17], 10, &xp);
   if (*xp != ' ' || xp != fp + 19)
      goto jinvalid;
   n_idec_si32_cp(&year, &fp[20], 10, &xp);
   if (xp != fp + 24)
      goto jinvalid;
   if ((t = combinetime(year, month, day, hour, minute, second)) == (time_t)-1)
      goto jinvalid;
   if((t2 = mktime(gmtime(&t))) == (time_t)-1)
      goto jinvalid;
   tzdiff = t - t2;
   if((tmptr = localtime(&t)) == NULL)
      goto jinvalid;
   if (tmptr->tm_isdst > 0)
      tzdiff += 3600; /* TODO simply adding an hour for ISDST is .. buuh */
   t -= tzdiff;
jleave:
   NYD2_LEAVE;
   return t;
jinvalid:
   t = n_time_epoch();
   goto jleave;
}
#endif /* HAVE_IMAP_SEARCH || HAVE_IMAP */

FL time_t
rfctime(char const *date) /* TODO n_idec_ return tests */
{
   char const *cp, *x;
   time_t t;
   si32_t i, year, month, day, hour, minute, second;
   NYD2_ENTER;

   cp = date;

   if ((cp = nexttoken(cp)) == NULL)
      goto jinvalid;
   if (alphachar(cp[0]) && alphachar(cp[1]) && alphachar(cp[2]) &&
         cp[3] == ',') {
      if ((cp = nexttoken(&cp[4])) == NULL)
         goto jinvalid;
   }
   n_idec_si32_cp(&day, cp, 10, &x);
   if ((cp = nexttoken(x)) == NULL)
      goto jinvalid;
   for (i = 0;;) {
      if (!strncmp(cp, n_month_names[i], 3))
         break;
      if (n_month_names[++i][0] == '\0')
         goto jinvalid;
   }
   month = i + 1;
   if ((cp = nexttoken(&cp[3])) == NULL)
      goto jinvalid;
   /* RFC 5322, 4.3:
    *  Where a two or three digit year occurs in a date, the year is to be
    *  interpreted as follows: If a two digit year is encountered whose
    *  value is between 00 and 49, the year is interpreted by adding 2000,
    *  ending up with a value between 2000 and 2049.  If a two digit year
    *  is encountered with a value between 50 and 99, or any three digit
    *  year is encountered, the year is interpreted by adding 1900 */
   n_idec_si32_cp(&year, cp, 10, &x);
   i = (int)PTR2SIZE(x - cp);
   if (i == 2 && year >= 0 && year <= 49)
      year += 2000;
   else if (i == 3 || (i == 2 && year >= 50 && year <= 99))
      year += 1900;
   if ((cp = nexttoken(x)) == NULL)
      goto jinvalid;
   n_idec_si32_cp(&hour, cp, 10, &x);
   if (*x != ':')
      goto jinvalid;
   cp = &x[1];
   n_idec_si32_cp(&minute, cp, 10, &x);
   if (*x == ':') {
      cp = &x[1];
      n_idec_si32_cp(&second, cp, 10, &x);
   } else
      second = 0;

   if ((t = combinetime(year, month, day, hour, minute, second)) == (time_t)-1)
      goto jinvalid;
   if ((cp = nexttoken(x)) != NULL) {
      char buf[3];
      int sign = 1;

      switch (*cp) {
      case '+':
         sign = -1;
         /* FALLTHRU */
      case '-':
         ++cp;
         break;
      }
      if (digitchar(cp[0]) && digitchar(cp[1]) && digitchar(cp[2]) &&
            digitchar(cp[3])) {
         si64_t tadj;

         buf[2] = '\0';
         buf[0] = cp[0];
         buf[1] = cp[1];
         n_idec_si32_cp(&i, buf, 10, NULL);
         tadj = (si64_t)i * 3600; /* XXX */
         buf[0] = cp[2];
         buf[1] = cp[3];
         n_idec_si32_cp(&i, buf, 10, NULL);
         tadj += (si64_t)i * 60; /* XXX */
         if (sign < 0)
            tadj = -tadj;
         t += (time_t)tadj;
      }
      /* TODO WE DO NOT YET PARSE (OBSOLETE) ZONE NAMES
       * TODO once again, Christos Zoulas and NetBSD Mail have done
       * TODO a really good job already, but using strptime(3), which
       * TODO is not portable.  Nonetheless, WE must improve, not
       * TODO at last because we simply ignore obsolete timezones!!
       * TODO See RFC 5322, 4.3! */
   }
jleave:
   NYD2_LEAVE;
   return t;
jinvalid:
   t = 0;
   goto jleave;
}

FL time_t
combinetime(int year, int month, int day, int hour, int minute, int second){
   size_t const jdn_epoch = 2440588;
   bool_t const y2038p = (sizeof(time_t) == 4);

   size_t jdn;
   time_t t;
   NYD2_ENTER;

   if(UICMP(32, second, >/*XXX leap=*/, n_DATE_SECSMIN) ||
         UICMP(32, minute, >=, n_DATE_MINSHOUR) ||
         UICMP(32, hour, >=, n_DATE_HOURSDAY) ||
         day < 1 || day > 31 ||
         month < 1 || month > 12 ||
         year < 1970)
      goto jerr;

   if(year >= 1970 + ((y2038p ? SI32_MAX : SI64_MAX) /
         (n_DATE_SECSDAY * n_DATE_DAYSYEAR))){
      /* Be a coward regarding Y2038, many people (mostly myself, that is) do
       * test by stepping second-wise around the flip.  Don't care otherwise */
      if(!y2038p)
         goto jerr;
      if(year > 2038 || month > 1 || day > 19 ||
            hour > 3 || minute > 14 || second > 7)
         goto jerr;
   }

   t = second;
   t += minute * n_DATE_SECSMIN;
   t += hour * n_DATE_SECSHOUR;

   jdn = a_head_gregorian_to_jdn(year, month, day);
   jdn -= jdn_epoch;
   t += (time_t)jdn * n_DATE_SECSDAY;
jleave:
   NYD2_LEAVE;
   return t;
jerr:
   t = (time_t)-1;
   goto jleave;
}

FL void
substdate(struct message *m)
{
   char const *cp;
   NYD_ENTER;

   /* Determine the date to print in faked 'From ' lines. This is traditionally
    * the date the message was written to the mail file. Try to determine this
    * using RFC message header fields, or fall back to current time */
   m->m_time = 0;
   if ((cp = hfield1("received", m)) != NULL) {
      while ((cp = nexttoken(cp)) != NULL && *cp != ';') {
         do
            ++cp;
         while (alnumchar(*cp));
      }
      if (cp && *++cp)
         m->m_time = rfctime(cp);
   }
   if (m->m_time == 0 || m->m_time > time_current.tc_time) {
      if ((cp = hfield1("date", m)) != NULL)
         m->m_time = rfctime(cp);
   }
   if (m->m_time == 0 || m->m_time > time_current.tc_time)
      m->m_time = time_current.tc_time;
   NYD_LEAVE;
}

FL void
setup_from_and_sender(struct header *hp)
{
   char const *addr;
   struct name *np;
   NYD_ENTER;

   /* If -t parsed or composed From: then take it.  With -t we otherwise
    * want -r to be honoured in favour of *from* in order to have
    * a behaviour that is compatible with what users would expect from e.g.
    * postfix(1) */
   if ((np = hp->h_from) != NULL ||
         ((n_poption & n_PO_t_FLAG) && (np = n_poption_arg_r) != NULL)) {
      ;
   } else if ((addr = myaddrs(hp)) != NULL)
      np = lextract(addr, GEXTRA | GFULL | GFULLEXTRA);
   hp->h_from = np;

   if ((np = hp->h_sender) != NULL) {
      ;
   } else if ((addr = ok_vlook(sender)) != NULL)
      np = lextract(addr, GEXTRA | GFULL | GFULLEXTRA);
   hp->h_sender = np;

   NYD_LEAVE;
}

FL struct name const *
check_from_and_sender(struct name const *fromfield,
   struct name const *senderfield)
{
   struct name const *rv = NULL;
   NYD_ENTER;

   if (senderfield != NULL) {
      if (senderfield->n_flink != NULL) {
         n_err(_("The Sender: field may contain only one address\n"));
         goto jleave;
      }
      rv = senderfield;
   }

   if (fromfield != NULL) {
      if (fromfield->n_flink != NULL && senderfield == NULL) {
         n_err(_("A Sender: is required when there are multiple "
            "addresses in From:\n"));
         goto jleave;
      }
      if (rv == NULL)
         rv = fromfield;
   }

   if (rv == NULL)
      rv = (struct name*)0x1;
jleave:
   NYD_LEAVE;
   return rv;
}

#ifdef HAVE_XSSL
FL char *
getsender(struct message *mp)
{
   char *cp;
   struct name *np;
   NYD_ENTER;

   if ((cp = hfield1("from", mp)) == NULL ||
         (np = lextract(cp, GEXTRA | GSKIN)) == NULL)
      cp = NULL;
   else
      cp = (np->n_flink != NULL) ? skin(hfield1("sender", mp)) : np->n_name;
   NYD_LEAVE;
   return cp;
}
#endif

FL struct name *
n_header_setup_in_reply_to(struct header *hp){
   struct name *np;
   NYD_ENTER;

   np = NULL;

   if(hp != NULL)
      if((np = hp->h_in_reply_to) == NULL && (np = hp->h_ref) != NULL)
         while(np->n_flink != NULL)
            np = np->n_flink;
   NYD_LEAVE;
   return np;
}

FL int
grab_headers(enum n_go_input_flags gif, struct header *hp, enum gfield gflags,
      int subjfirst)
{
   /* TODO grab_headers: again, check counts etc. against RFC;
    * TODO (now assumes check_from_and_sender() is called afterwards ++ */
   int errs;
   int volatile comma;
   NYD_ENTER;

   errs = 0;
   comma = (ok_blook(bsdcompat) || ok_blook(bsdmsgs)) ? 0 : GCOMMA;

   if (gflags & GTO)
      hp->h_to = grab_names(gif, "To: ", hp->h_to, comma, GTO | GFULL);
   if (subjfirst && (gflags & GSUBJECT))
      hp->h_subject = n_go_input_cp(gif, "Subject: ", hp->h_subject);
   if (gflags & GCC)
      hp->h_cc = grab_names(gif, "Cc: ", hp->h_cc, comma, GCC | GFULL);
   if (gflags & GBCC)
      hp->h_bcc = grab_names(gif, "Bcc: ", hp->h_bcc, comma, GBCC | GFULL);

   if (gflags & GEXTRA) {
      if (hp->h_from == NULL)
         hp->h_from = lextract(myaddrs(hp), GEXTRA | GFULL | GFULLEXTRA);
      hp->h_from = grab_names(gif, "From: ", hp->h_from, comma,
            GEXTRA | GFULL | GFULLEXTRA);
      if (hp->h_reply_to == NULL) {
         struct name *v15compat;

         if((v15compat = lextract(ok_vlook(replyto), GEXTRA | GFULL)) != NULL)
            n_OBSOLETE(_("please use *reply-to*, not *replyto*"));
         hp->h_reply_to = lextract(ok_vlook(reply_to), GEXTRA | GFULL);
         if(hp->h_reply_to == NULL) /* v15 */
            hp->h_reply_to = v15compat;
      }
      hp->h_reply_to = grab_names(gif, "Reply-To: ", hp->h_reply_to, comma,
            GEXTRA | GFULL);
      if (hp->h_sender == NULL)
         hp->h_sender = extract(ok_vlook(sender), GEXTRA | GFULL);
      hp->h_sender = grab_names(gif, "Sender: ", hp->h_sender, comma,
            GEXTRA | GFULL);
   }

   if (!subjfirst && (gflags & GSUBJECT))
      hp->h_subject = n_go_input_cp(gif, "Subject: ", hp->h_subject);

   NYD_LEAVE;
   return errs;
}

FL bool_t
n_header_match(struct message *mp, struct search_expr const *sep){
   struct str fiter, in, out;
   char const *field;
   long lc;
   FILE *ibuf;
   size_t *linesize;
   char **linebuf, *colon;
   enum {a_NONE, a_ALL, a_ITER, a_RE} match;
   bool_t rv;
   NYD_ENTER;

   rv = FAL0;
   match = a_NONE;
   linebuf = &termios_state.ts_linebuf; /* XXX line pool */
   linesize = &termios_state.ts_linesize; /* XXX line pool */
   n_UNINIT(fiter.l, 0);
   n_UNINIT(fiter.s, NULL);

   if((ibuf = setinput(&mb, mp, NEED_HEADER)) == NULL)
      goto jleave;
   if((lc = mp->m_lines - 1) < 0)
      goto jleave;

   if((mp->m_flag & MNOFROM) == 0 &&
         readline_restart(ibuf, linebuf, linesize, 0) < 0)
      goto jleave;

   /* */
   if((field = sep->ss_field) != NULL){
      if(!asccasecmp(field, "header") || (field[0] == '<' && field[1] == '\0'))
         match = a_ALL;
      else{
         fiter.s = n_lofi_alloc((fiter.l = strlen(field)) +1);
         match = a_ITER;
      }
#ifdef HAVE_REGEX
   }else if(sep->ss_fieldre != NULL){
      match = a_RE;
#endif
   }else
      match = a_ALL;

   /* Iterate over all the headers */
   while(lc > 0){
      struct name *np;

      if((lc = a_gethfield(ibuf, linebuf, linesize, lc, &colon, FAL0)) <= 0)
         break;

      /* Is this a header we are interested in? */
      if(match == a_ITER){
         char *itercp;

         memcpy(itercp = fiter.s, sep->ss_field, fiter.l +1);
         while((field = n_strsep(&itercp, ',', TRU1)) != NULL){
            /* It may be an abbreviation */
            char const x[][8] = {"from", "to", "cc", "bcc", "subject"};
            size_t i;
            char c1;

            if(field[0] != '\0' && field[1] == '\0'){
               c1 = lowerconv(field[0]);
               for(i = 0; i < n_NELEM(x); ++i){
                  if(c1 == x[i][0]){
                     field = x[i];
                     break;
                  }
               }
            }

            if(!ascncasecmp(field, *linebuf, PTR2SIZE(colon - *linebuf)))
               break;
         }
         if(field == NULL)
            continue;
#ifdef HAVE_REGEX
      }else if(match == a_RE){
         char *cp;
         size_t i;

         i = PTR2SIZE(colon - *linebuf);
         cp = n_lofi_alloc(i +1);
         memcpy(cp, *linebuf, i);
         cp[i] = '\0';
         i = (regexec(sep->ss_fieldre, cp, 0,NULL, 0) != REG_NOMATCH);
         n_lofi_free(cp);
         if(!i)
            continue;
#endif
      }

      /* It could be a plain existence test */
      if(sep->ss_field_exists){
         rv = TRU1;
         break;
      }

      /* Need to check the body */
      while(blankchar(*++colon))
         ;
      in.s = colon;

      /* Shall we split into address list and match as/the addresses only?
       * TODO at some later time we should ignore and log efforts to search
       * TODO a skinned address list if we know the header has none such */
      if(sep->ss_skin){
         if((np = lextract(in.s, GSKIN)) == NULL)
            continue;
         out.s = np->n_name;
      }else{
         np = NULL;
         in.l = strlen(in.s);
         mime_fromhdr(&in, &out, TD_ICONV);
      }

jnext_name:
#ifdef HAVE_REGEX
      if(sep->ss_bodyre != NULL)
         rv = (regexec(sep->ss_bodyre, out.s, 0,NULL, 0) != REG_NOMATCH);
      else
#endif
         rv = substr(out.s, sep->ss_body);

      if(np == NULL)
         n_free(out.s);
      if(rv)
         break;
      if(np != NULL && (np = np->n_flink) != NULL){
         out.s = np->n_name;
         goto jnext_name;
      }
   }

jleave:
   if(match == a_ITER)
      n_lofi_free(fiter.s);
   NYD_LEAVE;
   return rv;
}

FL char const *
n_header_is_standard(char const *name, size_t len){
   static char const * const names[] = {
      "Bcc", "Cc", "From",
      "In-Reply-To", "Mail-Followup-To",
      "Message-ID", "References", "Reply-To",
      "Sender", "Subject", "To",
      "Mailx-Command",
      "Mailx-Orig-Bcc", "Mailx-Orig-Cc", "Mailx-Orig-From", "Mailx-Orig-To",
      "Mailx-Raw-Bcc", "Mailx-Raw-Cc", "Mailx-Raw-To",
      NULL
   };
   char const * const *rv;
   NYD_ENTER;

   if(len == UIZ_MAX)
      len = strlen(name);

   for(rv = names; *rv != NULL; ++rv)
      if(!ascncasecmp(*rv, name, len))
         break;
   NYD_LEAVE;
   return *rv;
}

FL bool_t
n_header_add_custom(struct n_header_field **hflp, char const *dat,
      bool_t heap){
   size_t i;
   ui32_t nl, bl;
   char const *cp;
   struct n_header_field *hfp;
   NYD_ENTER;

   hfp = NULL;

   /* For (-C) convenience, allow leading WS */
   while(blankchar(*dat))
      ++dat;

   /* Isolate the header field from the body */
   for(cp = dat;; ++cp){
      if(fieldnamechar(*cp))
         continue;
      if(*cp == '\0'){
         if(cp == dat)
            goto jename;
      }else if(*cp != ':' && !blankchar(*cp)){
jename:
         cp = N_("Invalid custom header (not \"field: body\"): %s\n");
         goto jerr;
      }
      break;
   }
   nl = (ui32_t)PTR2SIZE(cp - dat);
   if(nl == 0)
      goto jename;

   /* Verify the custom header does not use standard/managed field name */
   if(n_header_is_standard(dat, nl) != NULL){
      cp = N_("Custom headers cannot use standard header names: %s\n");
      goto jerr;
   }

   /* Skip on over to the body */
   while(blankchar(*cp))
      ++cp;
   if(*cp++ != ':')
      goto jename;
   while(blankchar(*cp))
      ++cp;
   bl = (ui32_t)strlen(cp);
   for(i = bl++; i-- != 0;)
      if(cntrlchar(cp[i])){
         cp = N_("Invalid custom header: contains control characters: %s\n");
         goto jerr;
      }

   i = n_VSTRUCT_SIZEOF(struct n_header_field, hf_dat) + nl +1 + bl;
   *hflp = hfp = heap ? n_alloc(i) : n_autorec_alloc(i);
   hfp->hf_next = NULL;
   hfp->hf_nl = nl;
   hfp->hf_bl = bl - 1;
   memcpy(hfp->hf_dat, dat, nl);
      hfp->hf_dat[nl++] = '\0';
      memcpy(hfp->hf_dat + nl, cp, bl);
jleave:
   NYD_LEAVE;
   return (hfp != NULL);

jerr:
   n_err(V_(cp), n_shexp_quote_cp(dat, FAL0));
   goto jleave;
}

/* s-it-mode */
