/*@ S-nail - a mail user agent derived from Berkeley Mail.
 *@ Message, message array, getmsglist(), and related operations.
 *
 * Copyright (c) 2000-2004 Gunnar Ritter, Freiburg i. Br., Germany.
 * Copyright (c) 2012 - 2016 Steffen (Daode) Nurpmeso <steffen@sdaoden.eu>.
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
#define n_FILE message

#ifndef HAVE_AMALGAMATION
# include "nail.h"
#endif

/* Token values returned by the scanner used for argument lists.
 * Also, sizes of scanner-related things */
enum a_message_token{
   a_MESSAGE_T_EOL,     /* End of the command line */
   a_MESSAGE_T_NUMBER,  /* Message number */
   a_MESSAGE_T_MINUS,   /* - */
   a_MESSAGE_T_STRING,  /* A string (possibly containing -) */
   a_MESSAGE_T_DOT,     /* . */
   a_MESSAGE_T_UP,      /* ^ */
   a_MESSAGE_T_DOLLAR,  /* $ */
   a_MESSAGE_T_ASTER,   /* * */
   a_MESSAGE_T_OPEN,    /* ( */
   a_MESSAGE_T_CLOSE,   /* ) */
   a_MESSAGE_T_PLUS,    /* + */
   a_MESSAGE_T_COMMA,   /* , */
   a_MESSAGE_T_SEMI,    /* ; */
   a_MESSAGE_T_BACK,    /* ` */
   a_MESSAGE_T_ERROR    /* Lexical error */
};

enum a_message_idfield{
   a_MESSAGE_ID_REFERENCES,
   a_MESSAGE_ID_IN_REPLY_TO
};

enum a_message_state{
   a_MESSAGE_S_NEW = 1<<0,
   a_MESSAGE_S_OLD = 1<<1,
   a_MESSAGE_S_UNREAD = 1<<2,
   a_MESSAGE_S_DELETED = 1<<3,
   a_MESSAGE_S_READ = 1<<4,
   a_MESSAGE_S_FLAG = 1<<5,
   a_MESSAGE_S_ANSWERED = 1<<6,
   a_MESSAGE_S_DRAFT = 1<<7,
   a_MESSAGE_S_SPAM = 1<<8,
   a_MESSAGE_S_SPAMUNSURE = 1<<9
};

struct a_message_coltab{
   char mco_char; /* What to find past : */
   ui8_t mco__dummy[3];
   int mco_bit;   /* Associated modifier bit */
   int mco_mask;  /* m_status bits to mask */
   int mco_equal; /* ... must equal this */
};

struct a_message_lex{
   char ml_char;
   ui8_t ml_token;
};

static struct a_message_coltab const a_message_coltabs[] = {
   {'n', {0,}, a_MESSAGE_S_NEW, MNEW, MNEW},
   {'o', {0,}, a_MESSAGE_S_OLD, MNEW, 0},
   {'u', {0,}, a_MESSAGE_S_UNREAD, MREAD, 0},
   {'d', {0,}, a_MESSAGE_S_DELETED, MDELETED, MDELETED},
   {'r', {0,}, a_MESSAGE_S_READ, MREAD, MREAD},
   {'f', {0,}, a_MESSAGE_S_FLAG, MFLAGGED, MFLAGGED},
   {'a', {0,}, a_MESSAGE_S_ANSWERED, MANSWERED, MANSWERED},
   {'t', {0,}, a_MESSAGE_S_DRAFT, MDRAFTED, MDRAFTED},
   {'s', {0,}, a_MESSAGE_S_SPAM, MSPAM, MSPAM},
   {'S', {0,}, a_MESSAGE_S_SPAMUNSURE, MSPAMUNSURE, MSPAMUNSURE}
};

static struct a_message_lex const a_message_singles[] = {
   {'$', a_MESSAGE_T_DOLLAR},
   {'.', a_MESSAGE_T_DOT},
   {'^', a_MESSAGE_T_UP},
   {'*', a_MESSAGE_T_ASTER},
   {'-', a_MESSAGE_T_MINUS},
   {'+', a_MESSAGE_T_PLUS},
   {'(', a_MESSAGE_T_OPEN},
   {')', a_MESSAGE_T_CLOSE},
   {',', a_MESSAGE_T_COMMA},
   {';', a_MESSAGE_T_SEMI},
   {'`', a_MESSAGE_T_BACK}
};

/* Slots in ::message */
static size_t a_message_mem_space;

/* Mark entire threads */
static bool_t a_message_threadflag;

/* :d on its way HACK TODO */
static bool_t a_message_list_saw_d, a_message_list_last_saw_d;

/* String from a_MESSAGE_T_STRING, scan() */
static struct str a_message_lexstr;
/* Number of a_MESSAGE_T_NUMBER from scan() */
static int a_message_lexno;

/* Lazy load message header fields */
static enum okay a_message_get_header(struct message *mp);

/* Append, taking care of resizes TODO vector */
static char **a_message_add_to_namelist(char ***namelist, size_t *nmlsize,
               char **np, char *string);

/* Mark all messages that the user wanted from the command line in the message
 * structure.  Return 0 on success, -1 on error */
static int a_message_markall(char *buf, int f);

/* Turn the character after a colon modifier into a bit value */
static int a_message_evalcol(int col);

/* Check the passed message number for legality and proper flags.  Unless f is
 * MDELETED the message has to be undeleted */
static bool_t a_message_check(int mno, int f);

/* Scan out a single lexical item and return its token number, updating the
 * string pointer passed *sp.  Also, store the value of the number or string
 * scanned in a_message_lexno or a_message_lexstr as appropriate.
 * In any event, store the scanned "thing" in a_message_lexstr.
 * Returns the token as a negative number when we also saw & to mark a thread */
static int a_message_scan(char **sp);

/* See if the passed name sent the passed message */
static bool_t a_message_match_sender(struct message *mp, char const *str,
               bool_t allnet);

/* Check whether the given message-id or references match */
static bool_t a_message_match_mid(struct message *mp, char const *id,
               enum a_message_idfield idfield);

/* See if the given string matches.
 * For the purpose of the scan, we ignore case differences.
 * This is the engine behind the "/" search */
static bool_t a_message_match_dash(struct message *mp, char const *str);

/* See if the given search expression matches.
 * For the purpose of the scan, we ignore case differences.
 * This is the engine behind the "@[..@].." search */
static bool_t a_message_match_at(struct message *mp, struct search_expr *sep);

/* Unmark the named message */
static void a_message_unmark(int mesg);

/* Return the message number corresponding to the passed meta character */
static int a_message_metamess(int meta, int f);

/* Helper for mark(): self valid, threading enabled */
static void a_message__threadmark(struct message *self, int f);

static enum okay
a_message_get_header(struct message *mp){
   enum okay rv;
   NYD2_ENTER;
   UNUSED(mp);

   switch(mb.mb_type){
   case MB_FILE:
   case MB_MAILDIR:
      rv = OKAY;
      break;
#ifdef HAVE_POP3
   case MB_POP3:
      rv = pop3_header(mp);
      break;
#endif
   case MB_VOID:
   default:
      rv = STOP;
      break;
   }
   NYD2_LEAVE;
   return rv;
}

static char **
a_message_add_to_namelist(char ***namelist, size_t *nmlsize, char **np,
      char *string){
   size_t idx;
   NYD2_ENTER;

   if((idx = PTR2SIZE(np - *namelist)) >= *nmlsize){
      *namelist = srealloc(*namelist, (*nmlsize += 8) * sizeof *np);
      np = &(*namelist)[idx];
   }
   *np++ = string;
   NYD2_LEAVE;
   return np;
}

static int
a_message_markall(char *buf, int f){
   struct message *mp, *mx;
   enum a_message_idfield idfield;
   size_t j, nmlsize;
   char const *id;
   char **np, **nq, **namelist, *bufp, *cp;
   int i, valdot, beg, colmod, tok, colresult;
   enum{
      a_NONE = 0,
      a_ALLNET = 1u<<0,    /* Must be TRU1 */
      a_ALLOC = 1u<<1,     /* Have allocated something */
      a_THREADED = 1u<<2,
      a_ERROR = 1u<<3,
      a_ANY = 1u<<4,       /* Have marked just ANY */
      a_RANGE = 1u<<5,     /* Seen dash, await close */
      a_ASTER = 1u<<8,
      a_TOPEN = 1u<<9,     /* ( used (and didn't match) */
      a_TBACK = 1u<<10,    /* ` used (and didn't match) */
      a_TMP = 1u<<15
   } flags;
   NYD_ENTER;
   n_LCTA((ui32_t)a_ALLNET == (ui32_t)TRU1,
      "Constant is converted to bool_t via AND, thus");

   /* Update message array: clear MMARK but remember its former state for `.
    * An empty selector input is identical to * asterisk */
   for(i = msgCount; i-- > 0;){
      enum mflag mf;

      mf = (mp = &message[i])->m_flag;
      if(mf & MMARK)
         mf |= MOLDMARK;
      else
         mf &= ~MOLDMARK;
      mf &= ~MMARK;
      mp->m_flag = mf;
   }

   /* Strip all leading WS from user buffer */
   while(blankspacechar(*buf))
      ++buf;
   /* If there is no input buffer, we are done! */
   if(buf[0] == '\0'){
      flags = a_NONE;
      goto jleave;
   }

   UNINIT(beg, 0);
   UNINIT(idfield, a_MESSAGE_ID_REFERENCES);
   a_message_threadflag = FAL0;
   a_message_lexstr.s = ac_alloc(a_message_lexstr.l = 2 * strlen(buf) +1);
   np = namelist = smalloc((nmlsize = 8) * sizeof *namelist); /* TODO vector */
   bufp = buf;
   valdot = (int)PTR2SIZE(dot - message + 1);
   colmod = 0;
   id = NULL;
   flags = a_ALLOC | (mb.mb_threaded ? a_THREADED : 0);

   while((tok = a_message_scan(&bufp)) != a_MESSAGE_T_EOL){
      if((a_message_threadflag = (tok < 0)))
         tok &= INT_MAX;

      switch(tok){
      case a_MESSAGE_T_NUMBER:
         pstate |= PS_MSGLIST_GABBY;
jnumber:
         if(flags & a_RANGE){
            int i_base;

            flags ^= a_RANGE;

            if(!a_message_check(a_message_lexno, f))
               goto jerr;
            if(beg < a_message_lexno){
               i = beg;
               beg = 1; /* TODO does not work: (i < a_message_lexno)
               * TODO we need to detect whether both ends of a range
               * TODO belong to the same thread first, then iterate
               * TODO over the subset in between those points */
            }else{
               i = a_message_lexno;
               a_message_lexno = beg;
            }

            /* Problem: until the TODO above can be worked and we simply get an
             * iterator object for the thread (-range), we need to walk
             * a threaded list two times */
            i_base = (flags & a_THREADED) ? i : -1;
jnumber__thr:
            while((flags & a_THREADED) || i <= a_message_lexno){
               mp = &message[i - 1];
               if(i_base < 0 && !(mp->m_flag & MHIDDEN) &&
                      (f == MDELETED || !(mp->m_flag & MDELETED))){
                  mark(i, f);
                  flags |= a_ANY;
               }
               if(flags & a_THREADED){
                  if(i == a_message_lexno)
                     break;
                  mx = beg ? next_in_thread(mp) : prev_in_thread(mp);
                  if(mx == NULL){ 
                     id = N_("Range crosses multiple threads\n");
                     goto jerrmsg;
                  }
                  i = (int)PTR2SIZE(mx - message + 1);
               }else
                  ++i;
            }
            if(i_base >= 0){
               i = i_base;
               i_base = -1;
               goto jnumber__thr;
            }
            beg = 0;
            break;
         }else{
            if(!a_message_check(a_message_lexno, f))
               goto jerr;
            /* Inclusive range? */
            if(bufp[0] == '-'){
               ++bufp;
               beg = a_message_lexno;
               flags |= a_RANGE;
            }else{
               mark(a_message_lexno, f);
               flags |= a_ANY;
            }
         }
         break;
      case a_MESSAGE_T_PLUS:
         pstate &= ~PS_MSGLIST_DIRECT;
         pstate |= PS_MSGLIST_GABBY;
         i = valdot;
         do{
            if(flags & a_THREADED){
               mx = next_in_thread(&message[i - 1]);
               i = mx ? (int)PTR2SIZE(mx - message + 1) : msgCount + 1;
            }else
               ++i;
            if(i > msgCount){
               id = N_("Referencing beyond last message\n");
               goto jerrmsg;
            }
         }while(message[i - 1].m_flag == MHIDDEN ||
            (message[i - 1].m_flag & MDELETED) != (unsigned)f);
         a_message_lexno = i;
         goto jnumber;
      case a_MESSAGE_T_MINUS:
         pstate &= ~PS_MSGLIST_DIRECT;
         pstate |= PS_MSGLIST_GABBY;
         i = valdot;
         do{
            if(flags & a_THREADED){
               mx = prev_in_thread(&message[i - 1]);
               i = mx ? (int)PTR2SIZE(mx - message + 1) : 0;
            }else
               --i;
            if(i <= 0){
               id = N_("Referencing before first message\n");
               goto jerrmsg;
            }
         }while(message[i - 1].m_flag == MHIDDEN ||
            (message[i - 1].m_flag & MDELETED) != (unsigned)f);
         a_message_lexno = i;
         goto jnumber;
      case a_MESSAGE_T_STRING:
         pstate &= ~PS_MSGLIST_DIRECT;
         if(flags & a_RANGE)
            goto jebadrange;

         /* This may be a colon modifier */
         if((cp = a_message_lexstr.s)[0] != ':')
            np = a_message_add_to_namelist(&namelist, &nmlsize, np,
                  savestr(a_message_lexstr.s));
         else{
            while(*++cp != '\0'){
               colresult = a_message_evalcol(*cp);
               if(colresult == 0){
                  n_err(_("Unknown colon modifier: %s\n"), a_message_lexstr.s);
                  goto jerr;
               }
               if(colresult == a_MESSAGE_S_DELETED){
                  a_message_list_saw_d = TRU1;
                  f |= MDELETED;
               }
               colmod |= colresult;
            }
         }
         break;
      case a_MESSAGE_T_OPEN:
         pstate &= ~PS_MSGLIST_DIRECT;
         if(flags & a_RANGE)
            goto jebadrange;
         flags |= a_TOPEN;

#ifdef HAVE_IMAP_SEARCH
         /* C99 */{
            ssize_t ires;

            if((ires = imap_search(a_message_lexstr.s, f)) >= 0){
               if(ires > 0)
                  flags |= a_ANY;
               break;
            }
         }
#else
         n_err(_("Optional selector is not available: %s\n"),
            a_message_lexstr.s);
#endif
         goto jerr;
      case a_MESSAGE_T_DOLLAR:
      case a_MESSAGE_T_UP:
      case a_MESSAGE_T_SEMI:
         pstate |= PS_MSGLIST_GABBY;
         /* FALLTHRU */
      case a_MESSAGE_T_DOT: /* Don't set _GABBY for dot, to _allow_ history.. */
         pstate &= ~PS_MSGLIST_DIRECT;
         a_message_lexno = a_message_metamess(a_message_lexstr.s[0], f);
         if(a_message_lexno == -1)
            goto jerr;
         goto jnumber;
      case a_MESSAGE_T_BACK:
         pstate &= ~PS_MSGLIST_DIRECT;
         if(flags & a_RANGE)
            goto jebadrange;

         flags |= a_TBACK;
         for(i = 0; i < msgCount; ++i){
            if((mp = &message[i])->m_flag & MHIDDEN)
               continue;
            if((mp->m_flag & MDELETED) != (unsigned)f){
               if(!a_message_list_last_saw_d)
                  continue;
               a_message_list_saw_d = TRU1;
            }
            if(mp->m_flag & MOLDMARK){
               mark(i + 1, f);
               flags &= ~a_TBACK;
               flags |= a_ANY;
            }
         }
         break;
      case a_MESSAGE_T_ASTER:
         pstate &= ~PS_MSGLIST_DIRECT;
         if(flags & a_RANGE)
            goto jebadrange;
         flags |= a_ASTER;
         break;
      case a_MESSAGE_T_COMMA:
         pstate &= ~PS_MSGLIST_DIRECT;
         pstate |= PS_MSGLIST_GABBY;
         if(flags & a_RANGE)
            goto jebadrange;

         if(id == NULL){
            if((cp = hfield1("in-reply-to", dot)) != NULL)
               idfield = a_MESSAGE_ID_IN_REPLY_TO;
            else if((cp = hfield1("references", dot)) != NULL){
               struct name *enp;

               if((enp = extract(cp, GREF)) != NULL){
                  while(enp->n_flink != NULL)
                     enp = enp->n_flink;
                  cp = enp->n_name;
                  idfield = a_MESSAGE_ID_REFERENCES;
               }else
                  cp = NULL;
            }

            if(cp != NULL)
               id = savestr(cp);
            else{
               id = N_("Message-ID of parent of \"dot\" is indeterminable\n");
               goto jerrmsg;
            }
         }else if(!(pstate & PS_HOOK) && (options & OPT_D_V))
            n_err(_("Ignoring redundant specification of , selector\n"));
         break;
      case a_MESSAGE_T_ERROR:
         pstate &= ~PS_MSGLIST_DIRECT;
         pstate |= PS_MSGLIST_GABBY;
         goto jerr;
      }

      /* Explicitly disallow invalid ranges for future safety */
      if(bufp[0] == '-' && !(flags & a_RANGE)){
         if(!(pstate & PS_HOOK))
            n_err(_("Ignoring invalid range before: %s\n"), bufp);
         ++bufp;
      }
   }
   if(flags & a_RANGE){
      id = N_("Missing second range argument\n");
      goto jerrmsg;
   }

   np = a_message_add_to_namelist(&namelist, &nmlsize, np, NULL);
   --np;

   /* * is special at this point, after we have parsed the entire line */
   if(flags & a_ASTER){
      for(i = 0; i < msgCount; ++i){
         if((mp = &message[i])->m_flag & MHIDDEN)
            continue;
         if(!a_message_list_saw_d && (mp->m_flag & MDELETED) != (unsigned)f)
            continue;
         mark(i + 1, f);
         flags |= a_ANY;
      }
      if(!(flags & a_ANY))
         goto jenoapp;
      goto jleave;
   }

   /* If any names were given, add any messages which match */
   if(np > namelist || id != NULL){
      struct search_expr *sep = NULL;

      /* The @ search works with struct search_expr, so build an array.
       * To simplify array, i.e., regex_t destruction, and optimize for the
       * common case we walk the entire array even in case of errors */
      if(np > namelist){
         sep = scalloc(PTR2SIZE(np - namelist), sizeof(*sep));
         for(j = 0, nq = namelist; *nq != NULL; ++j, ++nq){
            char *x, *y;

            sep[j].ss_sexpr = x = *nq;
            if(*x != '@' || (flags & a_ERROR))
               continue;

            for(y = &x[1];; ++y){
               if(*y == '\0' || !fieldnamechar(*y)){
                  x = NULL;
                  break;
               }
               if(*y == '@'){
                  x = y;
                  break;
               }
            }
            sep[j].ss_where = (x == NULL || x - 1 == *nq)
                  ? "subject" : savestrbuf(&(*nq)[1], PTR2SIZE(x - *nq) - 1);

            x = (x == NULL ? *nq : x) + 1;
            if(*x == '\0'){ /* XXX Simply remove from list instead? */
               n_err(_("Empty [@..]@ search expression\n"));
               flags |= a_ERROR;
               continue;
            }
#ifdef HAVE_REGEX
            if(is_maybe_regex(x)){
               sep[j].ss_sexpr = NULL;
               if(regcomp(&sep[j].ss_regex, x,
                     REG_EXTENDED | REG_ICASE | REG_NOSUB) != 0){
                  if(!(pstate & PS_HOOK) && (options & OPT_D_V))
                     n_err(_("Invalid regular expression: >>> %s <<<\n"), x);
                  flags |= a_ERROR;
                  continue;
               }
            }else
#endif
               sep[j].ss_sexpr = x;
         }
         if(flags & a_ERROR)
            goto jnamesearch_sepfree;
      }

      /* Iterate the entire message array */
      srelax_hold();
      if(ok_blook(allnet))
         flags |= a_ALLNET;
      for(i = 0; i < msgCount; ++i){
         if((mp = &message[i])->m_flag & (MMARK | MHIDDEN))
            continue;
         if(!a_message_list_saw_d && (mp->m_flag & MDELETED) != (unsigned)f)
            continue;

         flags &= ~a_TMP;
         if(np > namelist){
            for(nq = namelist; *nq != NULL; ++nq){
               if(**nq == '@'){
                  if(a_message_match_at(mp, sep + PTR2SIZE(nq - namelist))){
                     flags |= a_TMP;
                     break;
                  }
               }else if(**nq == '/'){
                  if(a_message_match_dash(mp, *nq)){
                     flags |= a_TMP;
                     break;
                  }
               }else if(a_message_match_sender(mp, *nq, (flags & a_ALLNET))){
                  flags |= a_TMP;
                  break;
               }
            }
         }
         if(!(flags & a_TMP) &&
               id != NULL && a_message_match_mid(mp, id, idfield))
            flags |= a_TMP;

         if(flags & a_TMP){
            mark(i + 1, f);
            flags |= a_ANY;
         }
         srelax();
      }
      srelax_rele();

jnamesearch_sepfree:
      if(sep != NULL){
#ifdef HAVE_REGEX
         for(j = PTR2SIZE(np - namelist); j-- != 0;)
            if(sep[j].ss_sexpr == NULL)
               regfree(&sep[j].ss_regex);
#endif
         free(sep);
      }
      if(flags & a_ERROR)
         goto jerr;
   }

   /* If any colon modifiers were given, go through and mark any messages which
    * do satisfy the modifiers */
   if(colmod != 0){
      for(i = 0; i < msgCount; ++i){
         struct a_message_coltab const *colp;

         if((mp = &message[i])->m_flag & (MMARK | MHIDDEN))
            continue;
         if(!a_message_list_saw_d && (mp->m_flag & MDELETED) != (unsigned)f)
            continue;

         for(colp = a_message_coltabs;
               PTRCMP(colp, <, &a_message_coltabs[NELEM(a_message_coltabs)]);
               ++colp)
            if((colp->mco_bit & colmod) &&
                  ((mp->m_flag & colp->mco_mask) == (unsigned)colp->mco_equal)){
               mark(i + 1, f);
               flags |= a_ANY;
               break;
            }
      }
   }

   /* It shall be an error if ` didn't match anything, and nothing else did */
   if((flags & (a_TBACK | a_ANY)) == a_TBACK){
      id = N_("No previously marked messages\n");
      goto jerrmsg;
   }else if(!(flags & a_ANY))
      goto jenoapp;

   assert(!(flags & a_ERROR));
jleave:
   if(flags & a_ALLOC){
      free(namelist);
      ac_free(a_message_lexstr.s);
   }
   NYD_LEAVE;
   return (flags & a_ERROR) ? -1 : 0;

jebadrange:
   id = N_("Invalid range endpoint\n");
   goto jerrmsg;
jenoapp:
   id = N_("No applicable messages\n");
jerrmsg:
   if(!(pstate & PS_HOOK_MASK))
      n_err(V_(id));
jerr:
   flags |= a_ERROR;
   goto jleave;
}

static int
a_message_evalcol(int col){
   struct a_message_coltab const *colp;
   int rv;
   NYD2_ENTER;

   rv = 0;
   for(colp = a_message_coltabs;
         PTRCMP(colp, <, &a_message_coltabs[NELEM(a_message_coltabs)]); ++colp)
      if(colp->mco_char == col){
         rv = colp->mco_bit;
         break;
      }
   NYD2_LEAVE;
   return rv;
}

static bool_t
a_message_check(int mno, int f){
   struct message *mp;
   NYD2_ENTER;

   if(mno < 1 || mno > msgCount){
      n_err(_("%d: Invalid message number\n"), mno);
      mno = 1;
   }else if(((mp = &message[mno - 1])->m_flag & MHIDDEN) ||
         (f != MDELETED && (mp->m_flag & MDELETED) != 0))
      n_err(_("%d: inappropriate message\n"), mno);
   else
      mno = 0;
   NYD2_LEAVE;
   return (mno == 0);
}

static int
a_message_scan(char **sp)
{
   char *cp, *cp2;
   struct a_message_lex const *lp;
   int rv, c, inquote, quotec;
   NYD_ENTER;

   rv = a_MESSAGE_T_EOL;

   cp = *sp;
   cp2 = a_message_lexstr.s;
   c = *cp++;

   /* strip away leading white space */
   while(blankchar(c))
      c = *cp++;

   /* If no characters remain, we are at end of line, so report that */
   if(c == '\0'){
      *sp = --cp;
      goto jleave;
   }

   /* Select members of a message thread */
   if(c == '&'){
      if(*cp == '\0' || spacechar(*cp)){
         a_message_lexstr.s[0] = '.';
         a_message_lexstr.s[1] = '\0';
         *sp = cp;
         rv = a_MESSAGE_T_DOT | INT_MIN;
         goto jleave;
      }
      rv = INT_MIN;
      c = *cp++;
   }

   /* If the leading character is a digit, scan the number and convert it
    * on the fly.  Return a_MESSAGE_T_NUMBER when done */
   if(digitchar(c)){
      a_message_lexno = 0;
      do{
         a_message_lexno = (a_message_lexno * 10) + c - '0';
         *cp2++ = c;
      }while((c = *cp++, digitchar(c)));
      *cp2 = '\0';
      *sp = --cp;
      rv |= a_MESSAGE_T_NUMBER;
      goto jleave;
   }

   /* An IMAP SEARCH list. Note that a_MESSAGE_T_OPEN has always been included
    * in singles[] in Mail and mailx. Thus although there is no formal
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
            n_err(_("Missing )\n"));
            rv = a_MESSAGE_T_ERROR;
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
            ++level;
         else if (c == ')')
            --level;
         else if (spacechar(c)) {
            /* Replace unquoted whitespace by single space characters, to make
             * the string IMAP SEARCH conformant */
            c = ' ';
            if (cp2[-1] == ' ')
               --cp2;
         }
         *cp2++ = c;
      } while (c != ')' || level > 0);
      *cp2 = '\0';
      *sp = cp;
      rv |= a_MESSAGE_T_OPEN;
      goto jleave;
   }

   /* Check for single character tokens; return such if found */
   for(lp = a_message_singles;
         PTRCMP(lp, <, &a_message_singles[NELEM(a_message_singles)]); ++lp)
      if(c == lp->ml_char){
         a_message_lexstr.s[0] = c;
         a_message_lexstr.s[1] = '\0';
         *sp = cp;
         rv |= lp->ml_token;
         goto jleave;
      }

   /* We've got a string!  Copy all the characters of the string into
    * a_message_lexstr, until we see a null, space, or tab.  If the lead
    * character is a " or ', save it and scan until you get another */
   quotec = 0;
   if (c == '\'' || c == '"') {
      quotec = c;
      c = *cp++;
   }
   while (c != '\0') {
      if (quotec == 0 && c == '\\' && *cp != '\0')
         c = *cp++;
      if (c == quotec) {
         ++cp;
         break;
      }
      if (quotec == 0 && blankchar(c))
         break;
      if (PTRCMP(cp2 - a_message_lexstr.s, <, a_message_lexstr.l))
         *cp2++ = c;
      c = *cp++;
   }
   if (quotec && c == 0) {
      n_err(_("Missing %c\n"), quotec);
      rv = a_MESSAGE_T_ERROR;
      goto jleave;
   }
   *sp = --cp;
   *cp2 = '\0';
   rv |= a_MESSAGE_T_STRING;
jleave:
   NYD_LEAVE;
   return rv;
}

static bool_t
a_message_match_sender(struct message *mp, char const *str, bool_t allnet){
   char const *str_base, *np_base, *np;
   char sc, nc;
   bool_t rv;
   NYD2_ENTER;

   /* Empty string doesn't match */
   if(*(str_base = str) == '\0'){
      rv = FAL0;
      goto jleave;
   }

   /* *allnet* is POSIX and, since it explicitly mentions login and user names,
    * most likely case-sensitive.  XXX Still allow substr matching, though
    * XXX possibly the first letter should be case-insensitive, then? */
   if(allnet){
      for(np_base = np = nameof(mp, 0);;){
         if((sc = *str++) == '@')
            sc = '\0';
         if((nc = *np++) == '@' || nc == '\0' || sc == '\0')
            break;
         if(sc != nc){
            np = ++np_base;
            str = str_base;
         }
      }
      rv = (sc == '\0');
   }else{
      /* TODO POSIX says ~"match any address as shown in header overview",
       * TODO but a normalized match would be more sane i guess.
       * TODO struct name should gain a comparison method, normalize realname
       * TODO content (in TODO) and thus match as likewise
       * TODO "Buddy (Today) <here>" and "(Now) Buddy <here>" */
      char const *real_base;
      bool_t again;

      real_base = name1(mp, 0);
      again = ok_blook(showname);
jagain:
      np_base = np = again ? realname(real_base) : skin(real_base);
      str = str_base;
      for(;;){
         sc = *str++;
         if((nc = *np++) == '\0' || sc == '\0')
            break;
         sc = upperconv(sc);
         nc = upperconv(nc);
         if(sc != nc){
            np = ++np_base;
            str = str_base;
         }
      }

      /* And really if i want to match 'on@' then i want it to match even if
       * *showname* is set! */
      if(!(rv = (sc == '\0')) && again){
         again = FAL0;
         goto jagain;
      }
   }
jleave:
   NYD2_LEAVE;
   return rv;
}

static bool_t
a_message_match_mid(struct message *mp, char const *id,
      enum a_message_idfield idfield){
   char const *cp;
   bool_t rv;
   NYD2_ENTER;

   rv = FAL0;

   if((cp = hfield1("message-id", mp)) != NULL){
      switch(idfield){
      case a_MESSAGE_ID_REFERENCES:
         if(!msgidcmp(id, cp))
            rv = TRU1;
         break;
      case a_MESSAGE_ID_IN_REPLY_TO:{
         struct name *np;

         if((np = extract(id, GREF)) != NULL)
            do{
               if(!msgidcmp(np->n_name, cp)){
                  rv = TRU1;
                  break;
               }
            }while((np = np->n_flink) != NULL);
         break;
      }
      }
   }
   NYD2_LEAVE;
   return rv;
}

static bool_t
a_message_match_dash(struct message *mp, char const *str){
   static char lastscan[128];

   struct str in, out;
   char *hfield, *hbody;
   bool_t rv;
   NYD2_ENTER;

   rv = FAL0;

   if(*++str == '\0')
      str = lastscan;
   else
      n_strscpy(lastscan, str, sizeof lastscan); /* XXX use new n_str object! */

   /* Now look, ignoring case, for the word in the string */
   if(ok_blook(searchheaders) && (hfield = strchr(str, ':'))){
      size_t l;

      l = PTR2SIZE(hfield - str);
      hfield = ac_alloc(l +1);
      memcpy(hfield, str, l);
      hfield[l] = '\0';
      hbody = hfieldX(hfield, mp);
      ac_free(hfield);
      hfield = UNCONST(str + l + 1);
   }else{
      hfield = UNCONST(str);
      hbody = hfield1("subject", mp);
   }
   if(hbody == NULL)
      goto jleave;

   in.l = strlen(in.s = hbody);
   mime_fromhdr(&in, &out, TD_ICONV);
   rv = substr(out.s, hfield);
   free(out.s);
jleave:
   NYD2_LEAVE;
   return rv;
}

static bool_t
a_message_match_at(struct message *mp, struct search_expr *sep){
   struct str in, out;
   char *nfield;
   char const *cfield;
   bool_t rv;
   NYD2_ENTER;

   rv = FAL0;
   nfield = savestr(sep->ss_where);

   while((cfield = n_strsep(&nfield, ',', TRU1)) != NULL){
      if(!asccasecmp(cfield, "body") ||
            (cfield[1] == '\0' && cfield[0] == '>')){
         rv = FAL0;
jmsg:
         if((rv = message_match(mp, sep, rv)))
            break;
         continue;
      }else if(!asccasecmp(cfield, "text") ||
            (cfield[1] == '\0' && cfield[0] == '=')){
         rv = TRU1;
         goto jmsg;
      }

      if(!asccasecmp(cfield, "header") ||
            (cfield[1] == '\0' && cfield[0] == '<')){
         if((rv = header_match(mp, sep)))
            break;
         continue;
      }

      /* This is not a special name, so take care for the "skin" prefix !
       * and possible abbreviations */
      /* C99 */{
         struct name *np;
         bool_t doskin;

         if((doskin = (*cfield == '~')))
            ++cfield;
         if(cfield[0] != '\0' && cfield[1] == '\0'){
            char const x[][8] = {"from", "to", "cc", "bcc", "subject"};
            size_t i;
            char c1;

            c1 = lowerconv(cfield[0]);
            for(i = 0; i < NELEM(x); ++i){
               if(c1 == x[i][0]){
                  cfield = x[i];
                  break;
               }
            }
         }
         if((in.s = hfieldX(cfield, mp)) == NULL)
            continue;

         /* Shall we split into address list and match the addresses only? */
         if(doskin){
            np = lextract(in.s, GSKIN);
            if(np == NULL)
               continue;
            out.s = np->n_name;
         }else{
            np = NULL;
            in.l = strlen(in.s);
            mime_fromhdr(&in, &out, TD_ICONV);
         }

jnext_name:
#ifdef HAVE_REGEX
         if(sep->ss_sexpr == NULL)
            rv = (regexec(&sep->ss_regex, out.s, 0,NULL, 0) != REG_NOMATCH);
         else
#endif
            rv = substr(out.s, sep->ss_sexpr);
         if(np == NULL)
            free(out.s);
         if(rv)
            break;
         if(np != NULL && (np = np->n_flink) != NULL){
            out.s = np->n_name;
            goto jnext_name;
         }
      }
   }
   NYD2_LEAVE;
   return rv;
}

static void
a_message_unmark(int mesg){
   size_t i;
   NYD2_ENTER;

   i = (size_t)mesg;
   if(i < 1 || UICMP(z, i, >, msgCount))
      n_panic(_("Bad message number to unmark"));
   message[--i].m_flag &= ~MMARK;
   NYD2_LEAVE;
}

static int
a_message_metamess(int meta, int f)
{
   int c, m;
   struct message *mp;
   NYD2_ENTER;

   c = meta;
   switch (c) {
   case '^': /* First 'good' message left */
      mp = mb.mb_threaded ? threadroot : message;
      while (PTRCMP(mp, <, message + msgCount)) {
         if (!(mp->m_flag & MHIDDEN) && (mp->m_flag & MDELETED) == (ui32_t)f) {
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
      if (!(pstate & PS_HOOK_MASK))
         n_err(_("No applicable messages\n"));
      goto jem1;

   case '$': /* Last 'good message left */
      mp = mb.mb_threaded
            ? this_in_thread(threadroot, -1) : message + msgCount - 1;
      while (mp >= message) {
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
      if (!(pstate & PS_HOOK_MASK))
         n_err(_("No applicable messages\n"));
      goto jem1;

   case '.':
      /* Current message */
      m = dot - message + 1;
      if ((dot->m_flag & MHIDDEN) || (dot->m_flag & MDELETED) != (ui32_t)f) {
         n_err(_("%d: inappropriate message\n"), m);
         goto jem1;
      }
      c = m;
      break;

   case ';':
      /* Previously current message */
      if (prevdot == NULL) {
         n_err(_("No previously current message\n"));
         goto jem1;
      }
      m = prevdot - message + 1;
      if ((prevdot->m_flag & MHIDDEN) ||
            (prevdot->m_flag & MDELETED) != (ui32_t)f) {
         n_err(_("%d: inappropriate message\n"), m);
         goto jem1;
      }
      c = m;
      break;

   default:
      n_err(_("Unknown selector: %c\n"), c);
      goto jem1;
   }
jleave:
   NYD2_LEAVE;
   return c;
jem1:
   c = -1;
   goto jleave;
}

static void
a_message__threadmark(struct message *self, int f){
   NYD2_ENTER;
   if(!(self->m_flag & MHIDDEN) &&
         (f == MDELETED || !(self->m_flag & MDELETED) || a_message_list_saw_d))
      self->m_flag |= MMARK;

   if((self = self->m_child) != NULL){
      goto jcall;
      while((self = self->m_younger) != NULL)
         if(self->m_child != NULL)
jcall:
            a_message__threadmark(self, f);
         else
            self->m_flag |= MMARK;
   }
   NYD2_LEAVE;
}

FL FILE *
setinput(struct mailbox *mp, struct message *m, enum needspec need){
   FILE *rv;
   enum okay ok;
   NYD_ENTER;

   rv = NULL;
   ok = STOP;

   switch(need){
   case NEED_HEADER:
      ok = (m->m_have & HAVE_HEADER) ? OKAY : a_message_get_header(m);
      break;
   case NEED_BODY:
      ok = (m->m_have & HAVE_BODY) ? OKAY : get_body(m);
      break;
   case NEED_UNSPEC:
      ok = OKAY;
      break;
   }
   if(ok != OKAY)
      goto jleave;

   fflush(mp->mb_otf);
   if(fseek(mp->mb_itf, (long)mailx_positionof(m->m_block, m->m_offset),
         SEEK_SET) == -1){
      n_perr(_("fseek"), 0);
      n_panic(_("temporary file seek"));
   }
   rv = mp->mb_itf;
jleave:
   NYD_LEAVE;
   return rv;
}

FL enum okay
get_body(struct message *mp){
   enum okay rv;
   NYD_ENTER;
   UNUSED(mp);

   switch(mb.mb_type){
   case MB_FILE:
   case MB_MAILDIR:
      rv = OKAY;
      break;
#ifdef HAVE_POP3
   case MB_POP3:
      rv = pop3_body(mp);
      break;
#endif
   case MB_VOID:
   default:
      rv = STOP;
      break;
   }
   NYD_LEAVE;
   return rv;
}

FL void
message_reset(void){
   NYD_ENTER;
   if(message != NULL){
      free(message);
      message = NULL;
   }
   msgCount = 0;
   a_message_mem_space = 0;
   NYD_LEAVE;
}

FL void
message_append(struct message *mp){
   NYD_ENTER;
   if(UICMP(z, msgCount + 1, >=, a_message_mem_space)){
      /* XXX remove _mem_space magics (or use s_Vector) */
      a_message_mem_space = ((a_message_mem_space >= 128 &&
               a_message_mem_space <= 1000000)
            ? a_message_mem_space << 1 : a_message_mem_space + 64);
      message = srealloc(message, a_message_mem_space * sizeof(*message));
   }
   if(msgCount > 0){
      if(mp != NULL)
         message[msgCount - 1] = *mp;
      else
         memset(&message[msgCount - 1], 0, sizeof *message);
   }
   NYD_LEAVE;
}

FL void
message_append_null(void){
   NYD_ENTER;
   if(msgCount == 0)
      message_append(NULL);
   setdot(message);
   message[msgCount].m_size = 0;
   message[msgCount].m_lines = 0;
   NYD_LEAVE;
}

FL bool_t
message_match(struct message *mp, struct search_expr const *sep,
      bool_t with_headers){
   char **line;
   size_t *linesize, cnt;
   FILE *fp;
   bool_t rv;
   NYD_ENTER;

   rv = FAL0;

   if((fp = Ftmp(NULL, "mpmatch", OF_RDWR | OF_UNLINK | OF_REGISTER)) == NULL)
      goto j_leave;

   if(sendmp(mp, fp, NULL, NULL, SEND_TOSRCH, NULL) < 0)
      goto jleave;
   fflush_rewind(fp);

   cnt = fsize(fp);
   line = &termios_state.ts_linebuf; /* XXX line pool */
   linesize = &termios_state.ts_linesize; /* XXX line pool */

   if(!with_headers)
      while(fgetline(line, linesize, &cnt, NULL, fp, 0))
         if (**line == '\n')
            break;

   while(fgetline(line, linesize, &cnt, NULL, fp, 0)){
#ifdef HAVE_REGEX
      if(sep->ss_sexpr == NULL){
         if(regexec(&sep->ss_regex, *line, 0,NULL, 0) == REG_NOMATCH)
            continue;
      }else
#endif
      if(!substr(*line, sep->ss_sexpr))
         continue;
      rv = TRU1;
      break;
   }

jleave:
   Fclose(fp);
j_leave:
   NYD_LEAVE;
   return rv;
}

FL struct message *
setdot(struct message *mp){
   NYD_ENTER;
   if(dot != mp){
      prevdot = dot;
      pstate &= ~PS_DID_PRINT_DOT;
   }
   dot = mp;
   uncollapse1(dot, 0);
   NYD_LEAVE;
   return dot;
}

FL void
touch(struct message *mp){
   NYD_ENTER;
   mp->m_flag |= MTOUCH;
   if(!(mp->m_flag & MREAD))
      mp->m_flag |= MREAD | MSTATUS;
   NYD_LEAVE;
}

FL int
getmsglist(char *buf, int *vector, int flags)
{
   int *ip, mc;
   struct message *mp;
   NYD_ENTER;

   pstate &= ~PS_ARGLIST_MASK;
   a_message_list_last_saw_d = a_message_list_saw_d;
   a_message_list_saw_d = FAL0;

   if(msgCount == 0){
      *vector = 0;
      mc = 0;
      goto jleave;
   }

   pstate |= PS_MSGLIST_DIRECT;

   if(a_message_markall(buf, flags) < 0){
      mc = -1;
      goto jleave;
   }

   ip = vector;
   if(pstate & PS_HOOK_NEWMAIL){
      mc = 0;
      for(mp = message; mp < &message[msgCount]; ++mp)
         if(mp->m_flag & MMARK){
            if(!(mp->m_flag & MNEWEST))
               a_message_unmark((int)PTR2SIZE(mp - message + 1));
            else
               ++mc;
         }
      if(mc == 0){
         mc = -1;
         goto jleave;
      }
   }

   if(mb.mb_threaded == 0){
      for(mp = message; mp < &message[msgCount]; ++mp)
         if(mp->m_flag & MMARK)
            *ip++ = (int)PTR2SIZE(mp - message + 1);
   }else{
      for(mp = threadroot; mp != NULL; mp = next_in_thread(mp))
         if(mp->m_flag & MMARK)
            *ip++ = (int)PTR2SIZE(mp - message + 1);
   }
   *ip = 0;
   mc = (int)PTR2SIZE(ip - vector);
   if(mc != 1)
      pstate &= ~PS_MSGLIST_DIRECT;
jleave:
   NYD_LEAVE;
   return mc;
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
         mb.mb_threaded ? (mp != NULL) : PTRCMP(mp, <, message + msgCount);
         mb.mb_threaded ? (mp = next_in_thread(mp)) : ++mp) {
      if (!(mp->m_flag & MHIDDEN) && (mp->m_flag & m) == (ui32_t)f) {
         rv = (int)PTR2SIZE(mp - message + 1);
         goto jleave;
      }
   }

   if (dot > message) {
      for (mp = dot - 1; (mb.mb_threaded ? (mp != NULL) : (mp >= message));
            mb.mb_threaded ? (mp = prev_in_thread(mp)) : --mp) {
         if (!(mp->m_flag & MHIDDEN) && (mp->m_flag & m) == (ui32_t)f) {
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
mark(int mno, int f){
   struct message *mp;
   int i;
   NYD_ENTER;

   i = mno;
   if(i < 1 || i > msgCount)
      n_panic(_("Bad message number to mark"));
   mp = &message[--i];

   if(mb.mb_threaded == 1 && a_message_threadflag)
      a_message__threadmark(mp, f);
   else{
      assert(!(mp->m_flag & MHIDDEN));
      mp->m_flag |= MMARK;
   }
   NYD_LEAVE;
}

/* s-it-mode */