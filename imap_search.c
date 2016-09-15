/*@ S-nail - a mail user agent derived from Berkeley Mail.
 *@ Client-side implementation of the IMAP SEARCH command. This is used
 *@ for folders not located on IMAP servers, or for IMAP servers that do
 *@ not implement the SEARCH command.
 *
 * Copyright (c) 2000-2004 Gunnar Ritter, Freiburg i. Br., Germany.
 * Copyright (c) 2012 - 2015 Steffen (Daode) Nurpmeso <sdaoden@users.sf.net>.
 */
/*
 * Copyright (c) 2004
 * Gunnar Ritter.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *    This product includes software developed by Gunnar Ritter
 *    and his contributors.
 * 4. Neither the name of Gunnar Ritter nor the names of his contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY GUNNAR RITTER AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL GUNNAR RITTER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#undef n_FILE
#define n_FILE imap_search

#ifndef HAVE_AMALGAMATION
# include "nail.h"
#endif

EMPTY_FILE()
#ifdef HAVE_IMAP_SEARCH

enum itoken {
   ITBAD, ITEOD, ITBOL, ITEOL, ITAND, ITSET, ITALL, ITANSWERED,
   ITBCC, ITBEFORE, ITBODY,
   ITCC,
   ITDELETED, ITDRAFT,
   ITFLAGGED, ITFROM,
   ITHEADER,
   ITKEYWORD,
   ITLARGER,
   ITNEW, ITNOT,
   ITOLD, ITON, ITOR,
   ITRECENT,
   ITSEEN, ITSENTBEFORE, ITSENTON, ITSENTSINCE, ITSINCE, ITSMALLER,
      ITSUBJECT,
   ITTEXT, ITTO,
   ITUID, ITUNANSWERED, ITUNDELETED, ITUNDRAFT, ITUNFLAGGED, ITUNKEYWORD,
      ITUNSEEN
};

struct itlex {
   char const     *s_string;
   enum itoken    s_token;
};

struct itnode {
   enum itoken    n_token;
   unsigned long  n_n;
   void           *n_v;
   void           *n_w;
   struct itnode  *n_x;
   struct itnode  *n_y;
};

static struct itlex const  _it_strings[] = {
   { "ALL",          ITALL },
   { "ANSWERED",     ITANSWERED },
   { "BCC",          ITBCC },
   { "BEFORE",       ITBEFORE },
   { "BODY",         ITBODY },
   { "CC",           ITCC },
   { "DELETED",      ITDELETED },
   { "DRAFT",        ITDRAFT },
   { "FLAGGED",      ITFLAGGED },
   { "FROM",         ITFROM },
   { "HEADER",       ITHEADER },
   { "KEYWORD",      ITKEYWORD },
   { "LARGER",       ITLARGER },
   { "NEW",          ITNEW },
   { "NOT",          ITNOT },
   { "OLD",          ITOLD },
   { "ON",           ITON },
   { "OR",           ITOR },
   { "RECENT",       ITRECENT },
   { "SEEN",         ITSEEN },
   { "SENTBEFORE",   ITSENTBEFORE },
   { "SENTON",       ITSENTON },
   { "SENTSINCE",    ITSENTSINCE },
   { "SINCE",        ITSINCE },
   { "SMALLER",      ITSMALLER },
   { "SUBJECT",      ITSUBJECT },
   { "TEXT",         ITTEXT },
   { "TO",           ITTO },
   { "UID",          ITUID },
   { "UNANSWERED",   ITUNANSWERED },
   { "UNDELETED",    ITUNDELETED },
   { "UNDRAFT",      ITUNDRAFT },
   { "UNFLAGGED",    ITUNFLAGGED },
   { "UNKEYWORD",    ITUNKEYWORD },
   { "UNSEEN",       ITUNSEEN },
   { NULL,           ITBAD }
};

static struct itnode    *_it_tree;
static char             *_it_begin;
static enum itoken      _it_token;
static unsigned long    _it_number;
static void             *_it_args[2];
static size_t           _it_need_headers;

static enum okay     itparse(char const *spec, char const **xp, int sub);
static enum okay     itscan(char const *spec, char const **xp);
static enum okay     itsplit(char const *spec, char const **xp);
static enum okay     itstring(void **tp, char const *spec, char const **xp);
static int           itexecute(struct mailbox *mp, struct message *m,
                        size_t c, struct itnode *n);

static time_t        _imap_read_date(char const *cp);
static char *        _imap_quotestr(char const *s);
static char *        _imap_unquotestr(char const *s);

static bool_t        matchfield(struct message *m, char const *field,
                        void const *what);
static int           matchenvelope(struct message *m, char const *field,
                        void const *what);
static char *        mkenvelope(struct name *np);
static char const *  around(char const *cp);

static enum okay
itparse(char const *spec, char const **xp, int sub)
{
   int level = 0;
   struct itnode n, *z, *ittree;
   enum okay rv;
   NYD_ENTER;

   _it_tree = NULL;
   while ((rv = itscan(spec, xp)) == OKAY && _it_token != ITBAD &&
         _it_token != ITEOD) {
      ittree = _it_tree;
      memset(&n, 0, sizeof n);
      spec = *xp;
      switch (_it_token) {
      case ITBOL:
         ++level;
         continue;
      case ITEOL:
         if (--level == 0)
            goto jleave;
         if (level < 0) {
            if (sub > 0) {
               --(*xp);
               goto jleave;
            }
            n_err(_("Excess in \")\"\n"));
            rv = STOP;
            goto jleave;
         }
         continue;
      case ITNOT:
         /* <search-key> */
         n.n_token = ITNOT;
         if ((rv = itparse(spec, xp, sub + 1)) == STOP)
            goto jleave;
         spec = *xp;
         if ((n.n_x = _it_tree) == NULL) {
            n_err(_("Criterion for NOT missing: >>> %s <<<\n"), around(*xp));
            rv = STOP;
            goto jleave;
         }
         _it_token = ITNOT;
         break;
      case ITOR:
         /* <search-key1> <search-key2> */
         n.n_token = ITOR;
         if ((rv = itparse(spec, xp, sub + 1)) == STOP)
            goto jleave;
         if ((n.n_x = _it_tree) == NULL) {
            n_err(_("First criterion for OR missing: >>> %s <<<\n"),
               around(*xp));
            rv = STOP;
            goto jleave;
         }
         spec = *xp;
         if ((rv = itparse(spec, xp, sub + 1)) == STOP)
            goto jleave;
         spec = *xp;
         if ((n.n_y = _it_tree) == NULL) {
            n_err(_("Second criterion for OR missing: >>> %s <<<\n"),
               around(*xp));
            rv = STOP;
            goto jleave;
         }
         break;
      default:
         n.n_token = _it_token;
         n.n_n = _it_number;
         n.n_v = _it_args[0];
         n.n_w = _it_args[1];
      }

      _it_tree = ittree;
      if (_it_tree == NULL) {
         _it_tree = salloc(sizeof *_it_tree);
         *_it_tree = n;
      } else {
         z = _it_tree;
         _it_tree = salloc(sizeof *_it_tree);
         _it_tree->n_token = ITAND;
         _it_tree->n_x = z;
         _it_tree->n_y = salloc(sizeof *_it_tree->n_y);
         *_it_tree->n_y = n;
      }
      if (sub && level == 0)
         break;
   }
jleave:
   NYD_LEAVE;
   return rv;
}

static enum okay
itscan(char const *spec, char const **xp)
{
   int i, n;
   enum okay rv = OKAY;
   NYD_ENTER;

   while (spacechar(*spec))
      ++spec;
   if (*spec == '(') {
      *xp = &spec[1];
      _it_token = ITBOL;
      goto jleave;
   }
   if (*spec == ')') {
      *xp = &spec[1];
      _it_token = ITEOL;
      goto jleave;
   }
   while (spacechar(*spec))
      ++spec;
   if (*spec == '\0') {
      _it_token = ITEOD;
      goto jleave;
   }

#define __GO(C)   ((C) != '\0' && (C) != '(' && (C) != ')' && !spacechar(C))
   for (i = 0; _it_strings[i].s_string != NULL; ++i) {
      n = strlen(_it_strings[i].s_string);
      if (!ascncasecmp(spec, _it_strings[i].s_string, n) && !__GO(spec[n])) {
         _it_token = _it_strings[i].s_token;
         spec += n;
         while (spacechar(*spec))
            ++spec;
         rv = itsplit(spec, xp);
         goto jleave;
      }
   }
   if (digitchar(*spec)) {
      _it_number = strtoul(spec, UNCONST(xp), 10);
      if (!__GO(**xp)) {
         _it_token = ITSET;
         goto jleave;
      }
   }

   n_err(_("Bad SEARCH criterion \""));
   for (i = 0; __GO(spec[i]); ++i)
      ;
   n_err(_("%.*s \": >>> %s <<<\n"), i, spec, around(*xp));
#undef __GO

   _it_token = ITBAD;
   rv = STOP;
jleave:
   NYD_LEAVE;
   return rv;
}

static enum okay
itsplit(char const *spec, char const **xp)
{
   char *cp;
   time_t t;
   enum okay rv;
   NYD_ENTER;

   switch (_it_token) {
   case ITBCC:
   case ITBODY:
   case ITCC:
   case ITFROM:
   case ITSUBJECT:
   case ITTEXT:
   case ITTO:
      /* <string> */
      ++_it_need_headers;
      rv = itstring(_it_args, spec, xp);
      break;
   case ITSENTBEFORE:
   case ITSENTON:
   case ITSENTSINCE:
      ++_it_need_headers;
      /*FALLTHRU*/
   case ITBEFORE:
   case ITON:
   case ITSINCE:
      /* <date> */
      if ((rv = itstring(_it_args, spec, xp)) != OKAY)
         break;
      if ((t = _imap_read_date(_it_args[0])) == (time_t)-1) {
         n_err(_("Invalid date \"%s\": >>> %s <<<\n"),
            (char*)_it_args[0], around(*xp));
         rv = STOP;
         break;
      }
      _it_number = t;
      rv = OKAY;
      break;
   case ITHEADER:
      /* <field-name> <string> */
      ++_it_need_headers;
      if ((rv = itstring(_it_args, spec, xp)) != OKAY)
         break;
      spec = *xp;
      if ((rv = itstring(_it_args + 1, spec, xp)) != OKAY)
         break;
      break;
   case ITKEYWORD:
   case ITUNKEYWORD:
      /* <flag> */ /* TODO use table->flag map search instead */
      if ((rv = itstring(_it_args, spec, xp)) != OKAY)
         break;
      if (!asccasecmp(_it_args[0], "\\Seen"))
         _it_number = MREAD;
      else if (!asccasecmp(_it_args[0], "\\Deleted"))
         _it_number = MDELETED;
      else if (!asccasecmp(_it_args[0], "\\Recent"))
         _it_number = MNEW;
      else if (!asccasecmp(_it_args[0], "\\Flagged"))
         _it_number = MFLAGGED;
      else if (!asccasecmp(_it_args[0], "\\Answered"))
         _it_number = MANSWERED;
      else if (!asccasecmp(_it_args[0], "\\Draft"))
         _it_number = MDRAFT;
      else
         _it_number = 0;
      break;
   case ITLARGER:
   case ITSMALLER:
      /* <n> */
      if ((rv = itstring(_it_args, spec, xp)) != OKAY)
         break;
      _it_number = strtoul(_it_args[0], &cp, 10);
      if (spacechar(*cp) || *cp == '\0')
         break;
      n_err(_("Invalid size: >>> %s <<<\n"), around(*xp));
      rv = STOP;
      break;
   case ITUID:
      /* <message set> */
      n_err(_("Searching for UIDs is not supported: >>> %s <<<\n"),
         around(*xp));
      rv = STOP;
      break;
   default:
      *xp = spec;
      rv = OKAY;
      break;
   }
   NYD_LEAVE;
   return rv;
}

static enum okay
itstring(void **tp, char const *spec, char const **xp) /* XXX lesser derefs */
{
   int inquote = 0;
   char *ap;
   enum okay rv = STOP;
   NYD_ENTER;

   while (spacechar(*spec))
      ++spec;
   if (*spec == '\0' || *spec == '(' || *spec == ')') {
      n_err(_("Missing string argument: >>> %s <<<\n"),
         around(&(*xp)[spec - *xp]));
      goto jleave;
   }
   ap = *tp = salloc(strlen(spec) +1);
   *xp = spec;
    do {
      if (inquote && **xp == '\\')
         *ap++ = *(*xp)++;
      else if (**xp == '"')
         inquote = !inquote;
      else if (!inquote && (spacechar(**xp) || **xp == '(' || **xp == ')')) {
         *ap++ = '\0';
         break;
      }
      *ap++ = **xp;
   } while (*(*xp)++);

   *tp = _imap_unquotestr(*tp);
   rv = OKAY;
jleave:
   NYD_LEAVE;
   return rv;
}

static int
itexecute(struct mailbox *mp, struct message *m, size_t c, struct itnode *n)
{
   struct search_expr se;
   char *cp, *line = NULL; /* TODO line pool */
   size_t linesize = 0;
   FILE *ibuf;
   int rv;
   NYD_ENTER;

   if (n == NULL) {
      n_err(_("Internal error: Empty node in SEARCH tree\n"));
      rv = 0;
      goto jleave;
   }

   switch (n->n_token) {
   case ITBEFORE:
   case ITON:
   case ITSINCE:
      if (m->m_time == 0 && !(m->m_flag & MNOFROM) &&
            (ibuf = setinput(mp, m, NEED_HEADER)) != NULL) {
         if (readline_restart(ibuf, &line, &linesize, 0) > 0)
            m->m_time = unixtime(line);
         free(line);
      }
      break;
   case ITSENTBEFORE:
   case ITSENTON:
   case ITSENTSINCE:
      if (m->m_date == 0)
         if ((cp = hfield1("date", m)) != NULL)
            m->m_date = rfctime(cp);
      break;
   default:
      break;
   }

   switch (n->n_token) {
   default:
      n_err(_("Internal SEARCH error: Lost token %d\n"), n->n_token);
      rv = 0;
      break;
   case ITAND:
      rv = itexecute(mp, m, c, n->n_x) & itexecute(mp, m, c, n->n_y);
      break;
   case ITSET:
      rv = UICMP(z, c, ==, n->n_n);
      break;
   case ITALL:
      rv = 1;
      break;
   case ITANSWERED:
      rv = ((m->m_flag & MANSWERED) != 0);
      break;
   case ITBCC:
      rv = matchenvelope(m, "bcc", n->n_v);
      break;
   case ITBEFORE:
      rv = UICMP(z, m->m_time, <, n->n_n);
      break;
   case ITBODY:
      se.ss_sexpr = n->n_v;
      se.ss_where = "body";
      rv = message_match(m, &se, FAL0);
      break;
   case ITCC:
      rv = matchenvelope(m, "cc", n->n_v);
      break;
   case ITDELETED:
      rv = ((m->m_flag & MDELETED) != 0);
      break;
   case ITDRAFT:
      rv = ((m->m_flag & MDRAFTED) != 0);
      break;
   case ITFLAGGED:
      rv = ((m->m_flag & MFLAGGED) != 0);
      break;
   case ITFROM:
      rv = matchenvelope(m, "from", n->n_v);
      break;
   case ITHEADER:
      rv = matchfield(m, n->n_v, n->n_w);
      break;
   case ITKEYWORD:
      rv = ((m->m_flag & n->n_n) != 0);
      break;
   case ITLARGER:
      rv = (m->m_xsize > n->n_n);
      break;
   case ITNEW:
      rv = ((m->m_flag & (MNEW | MREAD)) == MNEW);
      break;
   case ITNOT:
      rv = !itexecute(mp, m, c, n->n_x);
      break;
   case ITOLD:
      rv = !(m->m_flag & MNEW);
      break;
   case ITON:
      rv = (UICMP(z, m->m_time, >=, n->n_n) &&
            UICMP(z, m->m_time, <, n->n_n + 86400));
      break;
   case ITOR:
      rv = itexecute(mp, m, c, n->n_x) | itexecute(mp, m, c, n->n_y);
      break;
   case ITRECENT:
      rv = ((m->m_flag & MNEW) != 0);
      break;
   case ITSEEN:
      rv = ((m->m_flag & MREAD) != 0);
      break;
   case ITSENTBEFORE:
      rv = UICMP(z, m->m_date, <, n->n_n);
      break;
   case ITSENTON:
      rv = (UICMP(z, m->m_date, >=, n->n_n) &&
            UICMP(z, m->m_date, <, n->n_n + 86400));
      break;
   case ITSENTSINCE:
      rv = UICMP(z, m->m_date, >=, n->n_n);
      break;
   case ITSINCE:
      rv = UICMP(z, m->m_time, >=, n->n_n);
      break;
   case ITSMALLER:
      rv = UICMP(z, m->m_xsize, <, n->n_n);
      break;
   case ITSUBJECT:
      rv = matchfield(m, "subject", n->n_v);
      break;
   case ITTEXT:
      se.ss_sexpr = n->n_v;
      se.ss_where = "text";
      rv = message_match(m, &se, TRU1);
      break;
   case ITTO:
      rv = matchenvelope(m, "to", n->n_v);
      break;
   case ITUNANSWERED:
      rv = !(m->m_flag & MANSWERED);
      break;
   case ITUNDELETED:
      rv = !(m->m_flag & MDELETED);
      break;
   case ITUNDRAFT:
      rv = !(m->m_flag & MDRAFTED);
      break;
   case ITUNFLAGGED:
      rv = !(m->m_flag & MFLAGGED);
      break;
   case ITUNKEYWORD:
      rv = !(m->m_flag & n->n_n);
      break;
   case ITUNSEEN:
      rv = !(m->m_flag & MREAD);
      break;
   }
jleave:
   NYD_LEAVE;
   return rv;
}

static time_t
_imap_read_date(char const *cp)
{
   time_t t = (time_t)-1;
   int year, month, day, i, tzdiff;
   struct tm *tmptr;
   char *xp, *yp;
   NYD_ENTER;

   if (*cp == '"')
      ++cp;
   day = strtol(cp, &xp, 10);
   if (day <= 0 || day > 31 || *xp++ != '-')
      goto jleave;

   for (i = 0;;) {
      if (!ascncasecmp(xp, month_names[i], 3))
         break;
      if (month_names[++i][0] == '\0')
         goto jleave;
   }
   month = i + 1;
   if (xp[3] != '-')
      goto jleave;
   year = strtol(xp + 4, &yp, 10);
   if (year < 1970 || year > 2037 || PTRCMP(yp, !=, xp + 8))
      goto jleave;
   if (yp[0] != '\0' && (yp[1] != '"' || yp[2] != '\0'))
      goto jleave;
   if ((t = combinetime(year, month, day, 0, 0, 0)) == (time_t)-1)
      goto jleave;
   tzdiff = t - mktime(gmtime(&t));
   tmptr = localtime(&t);
   if (tmptr->tm_isdst > 0)
      tzdiff += 3600;
   t -= tzdiff;
jleave:
   NYD_LEAVE;
   return t;
}

static char *
_imap_quotestr(char const *s)
{
   char *n, *np;
   NYD2_ENTER;

   np = n = salloc(2 * strlen(s) + 3);
   *np++ = '"';
   while (*s) {
      if (*s == '"' || *s == '\\')
         *np++ = '\\';
      *np++ = *s++;
   }
   *np++ = '"';
   *np = '\0';
   NYD2_LEAVE;
   return n;
}

static char *
_imap_unquotestr(char const *s)
{
   char *n, *np;
   NYD2_ENTER;

   if (*s != '"') {
      n = savestr(s);
      goto jleave;
   }

   np = n = salloc(strlen(s) + 1);
   while (*++s) {
      if (*s == '\\')
         s++;
      else if (*s == '"')
         break;
      *np++ = *s;
   }
   *np = '\0';
jleave:
   NYD2_LEAVE;
   return n;
}

static bool_t
matchfield(struct message *m, char const *field, void const *what)
{
   struct str in, out;
   bool_t rv = FAL0;
   NYD_ENTER;

   if ((in.s = hfieldX(field, m)) == NULL)
      goto jleave;

   in.l = strlen(in.s);
   mime_fromhdr(&in, &out, TD_ICONV);
   rv = substr(out.s, what);
   free(out.s);
jleave:
   NYD_LEAVE;
   return rv;
}

static int
matchenvelope(struct message *m, char const *field, void const *what)
{
   struct name *np;
   char *cp;
   int rv = 0;
   NYD_ENTER;

   if ((cp = hfieldX(field, m)) == NULL)
      goto jleave;

   for (np = lextract(cp, GFULL); np != NULL; np = np->n_flink) {
      if (!substr(np->n_name, what) && !substr(mkenvelope(np), what))
         continue;
      rv = 1;
      break;
   }

jleave:
   NYD_LEAVE;
   return rv;
}

static char *
mkenvelope(struct name *np)
{
   size_t epsize;
   char *ep, *realnam = NULL, *sourceaddr = NULL, *localpart = NULL,
      *domainpart = NULL, *cp, *rp, *xp, *ip;
   struct str in, out;
   int level = 0;
   bool_t hadphrase = FAL0;
   NYD_ENTER;

   in.s = np->n_fullname;
   in.l = strlen(in.s);
   mime_fromhdr(&in, &out, TD_ICONV);
   rp = ip = ac_alloc(strlen(out.s) + 1);
   for (cp = out.s; *cp; cp++) {
      switch (*cp) {
      case '"':
         while (*cp) {
            if (*++cp == '"')
               break;
            if (cp[0] == '\\' && cp[1] != '\0')
               ++cp;
            *rp++ = *cp;
         }
         break;
      case '<':
         while (cp > out.s && blankchar(cp[-1]))
            --cp;
         rp = ip;
         xp = out.s;
         if (PTRCMP(xp, <, cp - 1) && *xp == '"' && cp[-1] == '"') {
            ++xp;
            --cp;
         }
         while (xp < cp)
            *rp++ = *xp++;
         hadphrase = TRU1;
         goto jdone;
      case '(':
         if (level++)
            goto jdfl;
         if (!hadphrase)
            rp = ip;
         hadphrase = TRU1;
         break;
      case ')':
         if (--level)
            goto jdfl;
         break;
      case '\\':
         if (level && cp[1] != '\0')
            cp++;
         goto jdfl;
      default:
jdfl:
         *rp++ = *cp;
      }
   }
jdone:
   *rp = '\0';
   if (hadphrase)
      realnam = ip;
   free(out.s);
   localpart = savestr(np->n_name);
   if ((cp = strrchr(localpart, '@')) != NULL) {
      *cp = '\0';
      domainpart = cp + 1;
   }

   ep = salloc(epsize = strlen(np->n_fullname) * 2 + 40);
   snprintf(ep, epsize, "(%s %s %s %s)",
      realnam ? _imap_quotestr(realnam) : "NIL",
      sourceaddr ? _imap_quotestr(sourceaddr) : "NIL",
      localpart ? _imap_quotestr(localpart) : "NIL",
      domainpart ? _imap_quotestr(domainpart) : "NIL");
   ac_free(ip);
   NYD_LEAVE;
   return ep;
}

#define SURROUNDING 16
static char const *
around(char const *cp)
{
   static char ab[2 * SURROUNDING +1];

   size_t i;
   NYD_ENTER;

   for (i = 0; i < SURROUNDING && cp > _it_begin; ++i)
      --cp;
   for (i = 0; i < sizeof(ab) -1; ++i)
      ab[i] = *cp++;
   ab[i] = '\0';
   NYD_LEAVE;
   return ab;
}

FL enum okay
imap_search(char const *spec, int f)
{
   static char *lastspec;

   char const *xp;
   size_t i;
   enum okay rv = STOP;
   NYD_ENTER;

   if (strcmp(spec, "()")) {
      if (lastspec != NULL)
         free(lastspec);
      i = strlen(spec);
      lastspec = sbufdup(spec, i);
   } else if (lastspec == NULL) {
      n_err(_("No last SEARCH criteria available\n"));
      goto jleave;
   }
   spec =
   _it_begin = lastspec;

   _it_need_headers = FAL0;
#if 0
   if ((rv = imap_search1(spec, f) == OKAY))
      goto jleave;
#endif
   if (itparse(spec, &xp, 0) == STOP)
      goto jleave;
   if (_it_tree == NULL) {
      rv = OKAY;
      goto jleave;
   }

#if 0
   if (mb.mb_type == MB_IMAP && _it_need_headers)
      imap_getheaders(1, msgCount);
#endif
   srelax_hold();
   for (i = 0; UICMP(z, i, <, msgCount); ++i) {
      if (message[i].m_flag & MHIDDEN)
         continue;
      if (f == MDELETED || !(message[i].m_flag & MDELETED)) {
         size_t j = (int)(i + 1);
         if (itexecute(&mb, message + i, j, _it_tree))
            mark((int)j, f);
         srelax();
      }
   }
   srelax_rele();

   rv = OKAY;
jleave:
   NYD_LEAVE;
   return rv;
}
#endif /* HAVE_IMAP_SEARCH */

/* s-it-mode */
