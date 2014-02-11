/*@ S-nail - a mail user agent derived from Berkeley Mail.
 *@ Message (search a.k.a. argument) list handling.
 *
 * Copyright (c) 2000-2004 Gunnar Ritter, Freiburg i. Br., Germany.
 * Copyright (c) 2012 - 2014 Steffen (Daode) Nurpmeso <sdaoden@users.sf.net>.
 */
/*
 * Copyright (c) 1980, 1993
 * The Regents of the University of California.  All rights reserved.
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
 *    This product includes software developed by the University of
 *    California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
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

#ifndef HAVE_AMALGAMATION
# include "nail.h"
#endif

enum idfield {
   ID_REFERENCES,
   ID_IN_REPLY_TO
};

enum {
   CMNEW    = 1<<0,  /* New messages */
   CMOLD    = 1<<1,  /* Old messages */
   CMUNREAD = 1<<2,  /* Unread messages */
   CMDELETED =1<<3,  /* Deleted messages */
   CMREAD   = 1<<4,  /* Read messages */
   CMFLAG   = 1<<5,  /* Flagged messages */
   CMANSWER = 1<<6,  /* Answered messages */
   CMDRAFT  = 1<<7,  /* Draft messages */
   CMSPAM   = 1<<8   /* Spam messages */
};

struct coltab {
   char        co_char;    /* What to find past : */
   int         co_bit;     /* Associated modifier bit */
   int         co_mask;    /* m_status bits to mask */
   int         co_equal;   /* ... must equal this */
};

struct lex {
   char        l_char;
   enum ltoken l_token;
};

static struct coltab const _coltab[] = {
   { 'n',   CMNEW,      MNEW,       MNEW },
   { 'o',   CMOLD,      MNEW,       0 },
   { 'u',   CMUNREAD,   MREAD,      0 },
   { 'd',   CMDELETED,  MDELETED,   MDELETED },
   { 'r',   CMREAD,     MREAD,      MREAD },
   { 'f',   CMFLAG,     MFLAGGED,   MFLAGGED },
   { 'a',   CMANSWER,   MANSWERED,  MANSWERED },
   { 't',   CMDRAFT,    MDRAFTED,   MDRAFTED },
   { 's',   CMSPAM,     MSPAM,      MSPAM },
   { '\0',  0,          0,          0 }
};

static struct lex const    _singles[] = {
   { '$',   TDOLLAR },
   { '.',   TDOT },
   { '^',   TUP },
   { '*',   TSTAR },
   { '-',   TDASH },
   { '+',   TPLUS },
   { '(',   TOPEN },
   { ')',   TCLOSE },
   { ',',   TCOMMA },
   { ';',   TSEMI },
   { '`',   TBACK },
   { '\0',  0 }
};

static int     lastcolmod;
static size_t  STRINGLEN;
static int     lexnumber;              /* Number of TNUMBER from scan() */
static char    *lexstring;             /* String from TSTRING, scan() */
static int     regretp;                /* Pointer to TOS of regret tokens */
static int     regretstack[REGDEP];    /* Stack of regretted tokens */
static char    *string_stack[REGDEP];  /* Stack of regretted strings */
static int     numberstack[REGDEP];    /* Stack of regretted numbers */
static int     threadflag;             /* mark entire threads */

/* Append, taking care of resizes */
static char ** add_to_namelist(char ***namelist, size_t *nmlsize,
                  char **np, char *string);

/* Mark all messages that the user wanted from the command line in the message
 * structure.  Return 0 on success, -1 on error */
static int     markall(char *buf, int f);

/* Turn the character after a colon modifier into a bit value */
static int     evalcol(int col);

/* Check the passed message number for legality and proper flags.  If f is
 * MDELETED, then either kind will do.  Otherwise, the message has to be
 * undeleted */
static int     check(int mesg, int f);

/* Scan out a single lexical item and return its token number, updating the
 * string pointer passed **sp.  Also, store the value of the number or string
 * scanned in lexnumber or lexstring as appropriate.  In any event, store the
 * scanned `thing' in lexstring */
static int     scan(char **sp);

/* Unscan the named token by pushing it onto the regret stack */
static void    regret(int token);

/* Reset all the scanner global variables */
static void    scaninit(void);

/* See if the passed name sent the passed message number.  Return true if so */
static int     matchsender(char *str, int mesg, int allnet);

static int     matchmid(char *id, enum idfield idfield, int mesg);

/* See if the given string matches inside the subject field of the given
 * message.  For the purpose of the scan, we ignore case differences.  If it
 * does, return true.  The string search argument is assumed to have the form
 * "/string."  If it is of the form "/" we use the previous search string */
static int     matchsubj(char *str, int mesg);

/* Unmark the named message */
static void    unmark(int mesg);

/* Return the message number corresponding to the passed meta character */
static int     metamess(int meta, int f);

static char **
add_to_namelist(char ***namelist, size_t *nmlsize, char **np, char *string)
{
   size_t idx;
   NYD_ENTER;

   if ((idx = PTR2SIZE(np - *namelist)) >= *nmlsize) {
      *namelist = srealloc(*namelist, (*nmlsize += 8) * sizeof *np);
      np = &(*namelist)[idx];
   }
   *np++ = string;
   NYD_LEAVE;
   return np;
}

static int
markall(char *buf, int f)
{
#define markall_ret(i) do { rv = i; goto jleave; } while (0);

   /* TODO use a bit carrier for all the states */
   char **np, **nq;
   int i, rv, tok, beg, mc, other, valdot, colmod, colresult;
   struct message *mp, *mx;
   char **namelist, *bufp, *id = NULL, *cp;
   bool_t star, topen, tback;
   size_t nmlsize;
   enum idfield idfield = ID_REFERENCES;
#ifdef HAVE_IMAP
   int gotheaders;
#endif
   NYD_ENTER;

   lexstring = ac_alloc(STRINGLEN = 2 * strlen(buf) + 1);
   valdot = (int)PTR2SIZE(dot - message + 1);
   colmod = 0;

   for (i = 1; i <= msgCount; ++i) {
      enum mflag mf;

      mf = message[i - 1].m_flag;
      if (mf & MMARK)
         mf |= MOLDMARK;
      else
         mf &= ~MOLDMARK;
      mf &= ~MMARK;
      message[i - 1].m_flag = mf;
   }

   namelist = smalloc((nmlsize = 8) * sizeof *namelist);
   np = &namelist[0];
   scaninit();
   bufp = buf;
   beg = mc = star = other = topen = tback = FAL0;
#ifdef HAVE_IMAP
   gotheaders = 0;
#endif

   for (tok = scan(&bufp); tok != TEOL;) {
      switch (tok) {
      case TNUMBER:
number:
         if (star) {
            fprintf(stderr, tr(112, "No numbers mixed with *\n"));
            markall_ret(-1)
         }
         list_saw_numbers = TRU1;
         mc++;
         other++;
         if (beg != 0) {
            if (check(lexnumber, f))
               markall_ret(-1)
            i = beg;
            while (mb.mb_threaded ? 1 : i <= lexnumber) {
               if (!(message[i - 1].m_flag & MHIDDEN) &&
                     (f == MDELETED || !(message[i - 1].m_flag & MDELETED)))
                  mark(i, f);
               if (mb.mb_threaded) {
                  if (i == lexnumber)
                     break;
                  mx = next_in_thread(&message[i - 1]);
                  if (mx == NULL)
                     markall_ret(-1)
                  i = (int)PTR2SIZE(mx - message + 1);
               } else
                  ++i;
            }
            beg = 0;
            break;
         }
         beg = lexnumber;
         if (check(beg, f))
            markall_ret(-1)
         tok = scan(&bufp);
         regret(tok);
         if (tok != TDASH) {
            mark(beg, f);
            beg = 0;
         }
         break;

      case TPLUS:
         msglist_is_single = FAL0;
         if (beg != 0) {
            printf(tr(113, "Non-numeric second argument\n"));
            markall_ret(-1)
         }
         i = valdot;
         do {
            if (mb.mb_threaded) {
               mx = next_in_thread(&message[i - 1]);
               i = mx ? (int)PTR2SIZE(mx - message + 1) : msgCount + 1;
            } else
               ++i;
            if (i > msgCount) {
               fprintf(stderr, tr(114, "Referencing beyond EOF\n"));
               markall_ret(-1)
            }
         } while (message[i-1].m_flag == MHIDDEN ||
            (message[i - 1].m_flag & MDELETED) != (unsigned)f);
         mark(i, f);
         break;

      case TDASH:
         msglist_is_single = FAL0;
         if (beg == 0) {
            i = valdot;
            do {
               if (mb.mb_threaded) {
                  mx = prev_in_thread(&message[i - 1]);
                  i = mx ? (int)PTR2SIZE(mx - message + 1) : 0;
               } else
                  --i;
               if (i <= 0) {
                  fprintf(stderr, tr(115, "Referencing before 1\n"));
                  markall_ret(-1)
               }
            } while ((message[i - 1].m_flag & MHIDDEN) ||
                  (message[i - 1].m_flag & MDELETED) != (unsigned)f);
            mark(i, f);
         }
         break;

      case TSTRING:
         msglist_is_single = FAL0;
         if (beg != 0) {
            fprintf(stderr, tr(116, "Non-numeric second argument\n"));
            markall_ret(-1)
         }
         ++other;
         if (lexstring[0] == ':') {
            colresult = evalcol(lexstring[1]);
            if (colresult == 0) {
               fprintf(stderr, tr(117, "Unknown colon modifier \"%s\"\n"),
                  lexstring);
               markall_ret(-1)
            }
            colmod |= colresult;
         }
         else
            np = add_to_namelist(&namelist, &nmlsize, np, savestr(lexstring));
         break;

      case TOPEN:
#ifdef HAVE_IMAP_SEARCH
         msglist_is_single = FAL0;
         if (imap_search(lexstring, f) == STOP)
            markall_ret(-1)
         topen = TRU1;
#else
         fprintf(stderr, tr(42,
            "`%s': the used selector is optional and not available\n"),
            lexstring);
         markall_ret(-1)
#endif
         break;

      case TDOLLAR:
      case TUP:
      case TDOT:
      case TSEMI:
         msglist_is_single = FAL0;
         lexnumber = metamess(lexstring[0], f);
         if (lexnumber == -1)
            markall_ret(-1)
         goto number;

      case TBACK:
         msglist_is_single = FAL0;
         tback = TRU1;
         for (i = 1; i <= msgCount; i++) {
            if ((message[i - 1].m_flag & MHIDDEN) ||
                  (message[i - 1].m_flag & MDELETED) != (unsigned)f)
               continue;
            if (message[i - 1].m_flag & MOLDMARK)
               mark(i, f);
         }
         break;

      case TSTAR:
         msglist_is_single = FAL0;
         if (other) {
            fprintf(stderr, tr(118, "Can't mix \"*\" with anything\n"));
            markall_ret(-1)
         }
         star = TRU1;
         break;

      case TCOMMA:
         msglist_is_single = FAL0;
#ifdef HAVE_IMAP
         if (mb.mb_type == MB_IMAP && gotheaders++ == 0)
            imap_getheaders(1, msgCount);
#endif
         if (id == NULL && (cp = hfield1("in-reply-to", dot)) != NULL) {
            id = savestr(cp);
            idfield = ID_IN_REPLY_TO;
         }
         if (id == NULL && (cp = hfield1("references", dot)) != NULL) {
            struct name *enp;

            if ((enp = extract(cp, GREF)) != NULL) {
               while (enp->n_flink != NULL)
                  enp = enp->n_flink;
               id = savestr(enp->n_name);
               idfield = ID_REFERENCES;
            }
         }
         if (id == NULL) {
            printf(tr(227,
               "Cannot determine parent Message-ID of the current message\n"));
            markall_ret(-1)
         }
         break;

      case TERROR:
         list_saw_numbers = TRU1;
         msglist_is_single = FAL0;
         markall_ret(-1)
      }
      threadflag = 0;
      tok = scan(&bufp);
   }

   lastcolmod = colmod;
   np = add_to_namelist(&namelist, &nmlsize, np, NULL);
   --np;
   mc = 0;
   if (star) {
      for (i = 0; i < msgCount; ++i) {
         if (!(message[i].m_flag & MHIDDEN) &&
               (message[i].m_flag & MDELETED) == (unsigned)f) {
            mark(i + 1, f);
            ++mc;
         }
      }
      if (mc == 0) {
         if (!inhook)
            printf(tr(119, "No applicable messages.\n"));
         markall_ret(-1)
      }
      markall_ret(0)
   }

   if ((topen || tback) && mc == 0) {
      for (i = 0; i < msgCount; ++i)
         if (message[i].m_flag & MMARK)
            ++mc;
      if (mc == 0) {
         if (!inhook) {
            if (tback)
               fprintf(stderr, tr(131, "No previously marked messages.\n"));
            else
               printf("No messages satisfy (criteria).\n");/*TODO tr*/
         }
         markall_ret(-1)
      }
   }

   /* If no numbers were given, mark all messages, so that we can unmark
    * any whose sender was not selected if any user names were given */
   if ((np > namelist || colmod != 0 || id) && mc == 0)
      for (i = 1; i <= msgCount; ++i) {
         if (!(message[i - 1].m_flag & MHIDDEN) &&
               (message[i - 1].m_flag & MDELETED) == (unsigned)f)
            mark(i, f);
      }

   /* If any names were given, go through and eliminate any messages whose
    * senders were not requested */
   if (np > namelist || id) {
      bool_t allnet = ok_blook(allnet);

#ifdef HAVE_IMAP
      if (mb.mb_type == MB_IMAP && gotheaders++ == 0)
         imap_getheaders(1, msgCount);
#endif
      srelax_hold();
      for (i = 1; i <= msgCount; ++i) {
         mc = 0;
         if (np > namelist) {
            for (nq = namelist; *nq != NULL; ++nq)
               if (**nq == '/') {
                  if (matchsubj(*nq, i)) {
                     ++mc;
                     break;
                  }
               } else if (matchsender(*nq, i, allnet)) {
                  ++mc;
                  break;
               }
         }
         if (mc == 0 && id && matchmid(id, idfield, i))
            ++mc;
         if (mc == 0)
            unmark(i);
         srelax();
      }
      srelax_rele();

      /* Make sure we got some decent messages */
      mc = 0;
      for (i = 1; i <= msgCount; ++i)
         if (message[i - 1].m_flag & MMARK) {
            ++mc;
            break;
         }
      if (mc == 0) {
         if (!inhook && np > namelist) {
            printf(tr(120, "No applicable messages from {%s"), namelist[0]);
            for (nq = namelist + 1; *nq != NULL; ++nq)
               printf(tr(121, ", %s"), *nq);
            printf(tr(122, "}\n"));
         } else if (id)
            printf(tr(227, "Parent message not found\n"));
         markall_ret(-1)
      }
   }

   /* If any colon modifiers were given, go through and unmark any
    * messages which do not satisfy the modifiers */
   if (colmod != 0) {
      for (i = 1; i <= msgCount; ++i) {
         struct coltab const *colp;
         bool_t bad = TRU1;

         mp = &message[i - 1];
         for (colp = _coltab; colp->co_char != '\0'; ++colp)
            if ((colp->co_bit & colmod) &&
                  ((mp->m_flag & colp->co_mask) == (unsigned)colp->co_equal))
               bad = FAL0;
         if (bad)
            unmark(i);
      }
      for (mp = message; PTRCMP(mp, <, message + msgCount); ++mp)
         if (mp->m_flag & MMARK)
            break;
      if (PTRCMP(mp, >=, message + msgCount)) {
         struct coltab const *colp;

         if (!inhook) {
            printf(tr(123, "No messages satisfy"));
            for (colp = _coltab; colp->co_char != '\0'; ++colp)
               if (colp->co_bit & colmod)
                  printf(" :%c", colp->co_char);
            printf("\n");
         }
         markall_ret(-1)
      }
   }

   markall_ret(0)
jleave:
   free(namelist);
   ac_free(lexstring);
   NYD_LEAVE;
   return rv;

#undef markall_ret
}

static int
evalcol(int col)
{
   struct coltab const *colp;
   int rv;
   NYD_ENTER;

   if (col == 0)
      rv = lastcolmod;
   else {
      rv = 0;
      for (colp = _coltab; colp->co_char != '\0'; ++colp)
         if (colp->co_char == col) {
            rv = colp->co_bit;
            break;
         }
   }
   NYD_LEAVE;
   return rv;
}

static int
check(int mesg, int f)
{
   struct message *mp;
   NYD_ENTER;

   if (mesg < 1 || mesg > msgCount) {
      printf(tr(124, "%d: Invalid message number\n"), mesg);
      goto jem1;
   }
   mp = &message[mesg - 1];
   if (mp->m_flag & MHIDDEN ||
         (f != MDELETED && (mp->m_flag & MDELETED) != 0)) {
      fprintf(stderr, tr(125, "%d: Inappropriate message\n"), mesg);
      goto jem1;
   }
   f = 0;
jleave:
   NYD_LEAVE;
   return f;
jem1:
   f = -1;
   goto jleave;
}

static int
scan(char **sp)
{
   char *cp, *cp2;
   int rv, c, inquote, quotec;
   struct lex const *lp;
   NYD_ENTER;

   if (regretp >= 0) {
      strncpy(lexstring, string_stack[regretp], STRINGLEN);
      lexstring[STRINGLEN-1]='\0';
      lexnumber = numberstack[regretp];
      rv = regretstack[regretp--];
      goto jleave;
   }

   cp = *sp;
   cp2 = lexstring;
   c = *cp++;

   /* strip away leading white space */
   while (blankchar(c))
      c = *cp++;

   /* If no characters remain, we are at end of line, so report that */
   if (c == '\0') {
      *sp = --cp;
      rv = TEOL;
      goto jleave;
   }

   /* Select members of a message thread */
   if (c == '&') {
      threadflag = 1;
      if (*cp == '\0' || spacechar(*cp)) {
         lexstring[0] = '.';
         lexstring[1] = '\0';
         *sp = cp;
         rv = TDOT;
         goto jleave;
      }
      c = *cp++;
   }

   /* If the leading character is a digit, scan the number and convert it
    * on the fly.  Return TNUMBER when done */
   if (digitchar(c)) {
      lexnumber = 0;
      while (digitchar(c)) {
         lexnumber = lexnumber*10 + c - '0';
         *cp2++ = c;
         c = *cp++;
      }
      *cp2 = '\0';
      *sp = --cp;
      rv = TNUMBER;
      goto jleave;
   }

   /* An IMAP SEARCH list. Note that TOPEN has always been included in
    * singles[] in Mail and mailx. Thus although there is no formal
    * definition for (LIST) lists, they do not collide with historical
    * practice because a subject string (LIST) could never been matched
    * this way */
   if (c == '(') {
      ui32_t level = 1;
      inquote = 0;
      *cp2++ = c;
      do {
         if ((c = *cp++&0377) == '\0') {
jmtop:
            fprintf(stderr, "Missing \")\".\n");
            rv = TERROR;
            goto jleave;
         }
         if (inquote && c == '\\') {
            *cp2++ = c;
            c = *cp++&0377;
            if (c == '\0')
               goto jmtop;
         } else if (c == '"')
            inquote = !inquote;
         else if (inquote)
            /*EMPTY*/;
         else if (c == '(')
            level++;
         else if (c == ')')
            level--;
         else if (spacechar(c)) {
            /* Replace unquoted whitespace by single space characters, to make
             * the string IMAP SEARCH conformant */
            c = ' ';
            if (cp2[-1] == ' ')
               cp2--;
         }
         *cp2++ = c;
      } while (c != ')' || level > 0);
      *cp2 = '\0';
      *sp = cp;
      rv = TOPEN;
      goto jleave;
   }

   /* Check for single character tokens; return such if found */
   for (lp = _singles; lp->l_char != '\0'; ++lp)
      if (c == lp->l_char) {
         lexstring[0] = c;
         lexstring[1] = '\0';
         *sp = cp;
         rv = lp->l_token;
         goto jleave;
      }

   /* We've got a string!  Copy all the characters of the string into
    * lexstring, until we see a null, space, or tab.  If the lead character is
    * a " or ', save it and scan until you get another */
   quotec = 0;
   if (c == '\'' || c == '"') {
      quotec = c;
      c = *cp++;
   }
   while (c != '\0') {
      if (quotec == 0 && c == '\\' && *cp)
         c = *cp++;
      if (c == quotec) {
         cp++;
         break;
      }
      if (quotec == 0 && blankchar(c))
         break;
      if (PTRCMP(cp2 - lexstring, <, STRINGLEN - 1))
         *cp2++ = c;
      c = *cp++;
   }
   if (quotec && c == 0) {
      fprintf(stderr, tr(127, "Missing %c\n"), quotec);
      rv = TERROR;
      goto jleave;
   }
   *sp = --cp;
   *cp2 = '\0';
   rv = TSTRING;
jleave:
   NYD_LEAVE;
   return rv;
}

static void
regret(int token)
{
   NYD_ENTER;
   if (++regretp >= REGDEP)
      panic(tr(128, "Too many regrets"));
   regretstack[regretp] = token;
   lexstring[STRINGLEN - 1] = '\0';
   string_stack[regretp] = savestr(lexstring);
   numberstack[regretp] = lexnumber;
   NYD_LEAVE;
}

static void
scaninit(void)
{
   NYD_ENTER;
   regretp = -1;
   threadflag = 0;
   NYD_LEAVE;
}

static int
matchsender(char *str, int mesg, int allnet)
{
   int rv;
   NYD_ENTER;

   if (allnet) {
      char *cp = nameof(&message[mesg - 1], 0);

      do {
         if ((*cp == '@' || *cp == '\0') && (*str == '@' || *str == '\0')) {
            rv = 1;
            goto jleave;
         }
         if (*cp != *str)
            break;
      } while (cp++, *str++ != '\0');
      rv = 0;
      goto jleave;
   }
   rv = !strcmp(str,
         (ok_blook(showname) ? realname : skin)(name1(&message[mesg - 1], 0)));
jleave:
   NYD_LEAVE;
   return rv;
}

static int
matchmid(char *id, enum idfield idfield, int mesg)
{
   struct name *np;
   char *cp;
   int rv;
   NYD_ENTER;

   if ((cp = hfield1("message-id", &message[mesg - 1])) != NULL) {
      switch (idfield) {
      case ID_REFERENCES:
         rv = (msgidcmp(id, cp) == 0);
         goto jleave;
      case ID_IN_REPLY_TO:
         if ((np = extract(id, GREF)) != NULL)
            do {
               if (msgidcmp(np->n_name, cp) == 0) {
                  rv = 1;
                  goto jleave;
               }
            } while ((np = np->n_flink) != NULL);
         break;
      }
   }
   rv = 0;
jleave:
   NYD_LEAVE;
   return rv;
}

static int
matchsubj(char *str, int mesg) /* FIXME regex-enable; funbody=only matching!! */
{
   static char lastscan[128];

   struct str in, out;
   struct message *mp;
   char *cp, *cp2;
   int i;
   NYD_ENTER;

   ++str;
   if (strlen(str) == 0) {
      str = lastscan;
   } else {
      strncpy(lastscan, str, sizeof lastscan); /* XXX use new n_str object! */
      lastscan[sizeof lastscan - 1] = '\0';
   }

   mp = &message[mesg - 1];

   /* Now look, ignoring case, for the word in the string */
   if (ok_blook(searchheaders) && (cp = strchr(str, ':'))) {
      *cp++ = '\0';
      cp2 = hfieldX(str, mp);
      cp[-1] = ':';
   } else {
      cp = str;
      cp2 = hfield1("subject", mp);
   }
   if (cp2 == NULL) {
      i = 0;
      goto jleave;
   }

   in.s = cp2;
   in.l = strlen(cp2);
   mime_fromhdr(&in, &out, TD_ICONV);
   i = substr(out.s, cp);
   free(out.s);
jleave:
   NYD_LEAVE;
   return i;
}

static void
unmark(int mesg)
{
   size_t i;
   NYD_ENTER;

   i = (size_t)mesg;
   if (i < 1 || UICMP(z, i, >, msgCount))
      panic(tr(130, "Bad message number to unmark"));
   message[i - 1].m_flag &= ~MMARK;
   NYD_LEAVE;
}

static int
metamess(int meta, int f)
{
   int c, m;
   struct message *mp;
   NYD_ENTER;

   c = meta;
   switch (c) {
   case '^': /* First 'good' message left */
      mp = mb.mb_threaded ? threadroot : &message[0];
      while (PTRCMP(mp, <, message + msgCount)) {
         if (!(mp->m_flag & MHIDDEN) && (mp->m_flag & MDELETED) ==(unsigned)f){
            c = (int)PTR2SIZE(mp - message + 1);
            goto jleave;
         }
         if (mb.mb_threaded) {
            mp = next_in_thread(mp);
            if (mp == NULL)
               break;
         } else
            ++mp;
      }
      if (!inhook)
         printf(tr(132, "No applicable messages\n"));
      goto jem1;

   case '$': /* Last 'good message left */
      mp = mb.mb_threaded ? this_in_thread(threadroot, -1)
            : &message[msgCount-1];
      while (mp >= &message[0]) {
         if (!(mp->m_flag & MHIDDEN) && (mp->m_flag & MDELETED) == (ui32_t)f) {
            c = (int)PTR2SIZE(mp - message + 1);
            goto jleave;
         }
         if (mb.mb_threaded) {
            mp = prev_in_thread(mp);
            if (mp == NULL)
               break;
         } else
            --mp;
      }
      if (!inhook)
         printf(tr(132, "No applicable messages\n"));
      goto jem1;

   case '.':
      /* Current message */
      m = dot - message + 1;
      if ((dot->m_flag & MHIDDEN) || (dot->m_flag & MDELETED) != (unsigned)f) {
         printf(tr(133, "%d: Inappropriate message\n"), m);
         goto jem1;
      }
      c = m;
      break;

   case ';':
      /* Previously current message */
      if (prevdot == NULL) {
         fprintf(stderr, tr(228, "No previously current message\n"));
         goto jem1;
      }
      m = prevdot - message + 1;
      if ((prevdot->m_flag & MHIDDEN) ||
            (prevdot->m_flag & MDELETED) != (unsigned)f) {
         fprintf(stderr, tr(133, "%d: Inappropriate message\n"), m);
         goto jem1;
      }
      c = m;
      break;

   default:
      fprintf(stderr, tr(134, "Unknown metachar (%c)\n"), c);
      goto jem1;
   }
jleave:
   NYD_LEAVE;
   return c;
jem1:
   c = -1;
   goto jleave;
}

FL int
getmsglist(char *buf, int *vector, int flags)
{
   int *ip, mc;
   struct message *mp;
   NYD_ENTER;

   list_saw_numbers =
   msglist_is_single = FAL0;

   if (msgCount == 0) {
      *vector = 0;
      mc = 0;
      goto jleave;
   }

   msglist_is_single = TRU1;
   if (markall(buf, flags) < 0) {
      mc = -1;
      goto jleave;
   }

   ip = vector;
   if (inhook & 2) {
      mc = 0;
      for (mp = message; PTRCMP(mp, <, message + msgCount); ++mp)
         if (mp->m_flag & MMARK) {
            if ((mp->m_flag & MNEWEST) == 0)
               unmark((int)PTR2SIZE(mp - message + 1));
            else
               ++mc;
         }
      if (mc == 0) {
         mc = -1;
         goto jleave;
      }
   }

   if (mb.mb_threaded == 0) {
      for (mp = message; PTRCMP(mp, <, message + msgCount); ++mp)
         if (mp->m_flag & MMARK)
            *ip++ = (int)PTR2SIZE(mp - message + 1);
   } else {
      for (mp = threadroot; mp != NULL; mp = next_in_thread(mp))
         if (mp->m_flag & MMARK)
            *ip++ = (int)PTR2SIZE(mp - message + 1);
   }
   *ip = 0;
   mc = (int)PTR2SIZE(ip - vector);
   msglist_is_single = (mc == 1);
jleave:
   NYD_LEAVE;
   return mc;
}

FL int
getrawlist(char const *line, size_t linesize, char **argv, int argc,
   int echolist)
{
   char c, *cp2, quotec, *linebuf;
   char const *cp;
   int argn;
   NYD_ENTER;

   list_saw_numbers = FAL0;

   argn = 0;
   cp = line;
   linebuf = ac_alloc(linesize + 1);
   for (;;) {
      for (; blankchar(*cp); ++cp)
         ;
      if (*cp == '\0')
         break;
      if (argn >= argc - 1) {
         printf(tr(126, "Too many elements in the list; excess discarded.\n"));
         break;
      }
      cp2 = linebuf;
      quotec = '\0';
      while ((c = *cp) != '\0') {
         cp++;
         if (quotec != '\0') {
            if (c == quotec) {
               quotec = '\0';
               if (echolist)
                  *cp2++ = c;
            } else if (c == '\\')
               switch (c = *cp++) {
               case '\0':
                  *cp2++ = '\\';
                  cp--;
                  break;
               /*
               case '0': case '1': case '2': case '3':
               case '4': case '5': case '6': case '7':
                  c -= '0';
                  if (*cp >= '0' && *cp <= '7')
                     c = c * 8 + *cp++ - '0';
                  if (*cp >= '0' && *cp <= '7')
                     c = c * 8 + *cp++ - '0';
                  *cp2++ = c;
                  break;
               case 'b':
                  *cp2++ = '\b';
                  break;
               case 'f':
                  *cp2++ = '\f';
                  break;
               case 'n':
                  *cp2++ = '\n';
                  break;
               case 'r':
                  *cp2++ = '\r';
                  break;
               case 't':
                  *cp2++ = '\t';
                  break;
               case 'v':
                  *cp2++ = '\v';
                  break;
               */
               default:
                  if (cp[-1] != quotec || echolist)
                     *cp2++ = '\\';
                  *cp2++ = c;
               }
            /*else if (c == '^') {
               c = *cp++;
               if (c == '?')
                  *cp2++ = '\177';
               /\* null doesn't show up anyway *\/
               else if ((c >= 'A' && c <= '_') ||
                   (c >= 'a' && c <= 'z'))
                  *cp2++ = c & 037;
               else {
                  *cp2++ = '^';
                  cp--;
               }
            }*/ else
               *cp2++ = c;
         } else if (c == '"' || c == '\'') {
            if (echolist)
               *cp2++ = c;
            quotec = c;
         } else if (c == '\\' && !echolist) {
            if (*cp)
               *cp2++ = *cp++;
            else
               *cp2++ = c;
         } else if (blankchar(c))
            break;
         else
            *cp2++ = c;
      }
      *cp2 = '\0';
      argv[argn++] = savestr(linebuf);
   }
   argv[argn] = NULL;
   ac_free(linebuf);
   NYD_LEAVE;
   return argn;
}

FL int
first(int f, int m)
{
   struct message *mp;
   int rv;
   NYD_ENTER;

   if (msgCount == 0) {
      rv = 0;
      goto jleave;
   }

   f &= MDELETED;
   m &= MDELETED;
   for (mp = dot;
         mb.mb_threaded ? mp != NULL : PTRCMP(mp, <, message + msgCount);
         mb.mb_threaded ? mp = next_in_thread(mp) : ++mp) {
      if (!(mp->m_flag & MHIDDEN) && (mp->m_flag & m) == (unsigned)f) {
         rv = (int)PTR2SIZE(mp - message + 1);
         goto jleave;
      }
   }

   if (dot > message) {
      for (mp = dot-1; (mb.mb_threaded ? mp != NULL : mp >= message);
            mb.mb_threaded ? mp = prev_in_thread(mp) : --mp) {
         if (!(mp->m_flag & MHIDDEN) && (mp->m_flag & m) == (unsigned)f) {
            rv = (int)PTR2SIZE(mp - message + 1);
            goto jleave;
         }
      }
   }
   rv = 0;
jleave:
   NYD_LEAVE;
   return rv;
}

FL void
mark(int mesg, int f)
{
   struct message *mp;
   int i;
   NYD_ENTER;

   i = mesg;
   if (i < 1 || i > msgCount)
      panic(tr(129, "Bad message number to mark"));
   if (mb.mb_threaded == 1 && threadflag) {
      if ((message[i - 1].m_flag & MHIDDEN) == 0) {
         if (f == MDELETED || (message[i - 1].m_flag&MDELETED) == 0)
         message[i - 1].m_flag |= MMARK;
      }

      if (message[i - 1].m_child) {
         mp = message[i - 1].m_child;
         mark((int)PTR2SIZE(mp - message + 1), f);
         for (mp = mp->m_younger; mp; mp = mp->m_younger)
            mark((int)PTR2SIZE(mp - message + 1), f);
      }
   } else
      message[i - 1].m_flag |= MMARK;
   NYD_LEAVE;
}

/* vim:set fenc=utf-8:s-it-mode */
