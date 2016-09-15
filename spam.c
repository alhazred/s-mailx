/*@ S-nail - a mail user agent derived from Berkeley Mail.
 *@ Spam related facilities.
 *
 * Copyright (c) 2013 - 2015 Steffen (Daode) Nurpmeso <sdaoden@users.sf.net>.
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
#undef n_FILE
#define n_FILE spam

#ifndef HAVE_AMALGAMATION
# include "nail.h"
#endif

EMPTY_FILE()
#ifdef HAVE_SPAM
#if defined HAVE_SPAM_SPAMC || defined HAVE_SPAM_FILTER
# include <sys/wait.h>
#endif

#ifdef HAVE_SPAM_SPAMD
# include <sys/socket.h>
# include <sys/un.h>
#endif

/* This is chosen rather arbitrarily.
 * It must be able to swallow the first line of a rate response,
 * and an entire CHECK/TELL spamd(1) response */
#if BUFFER_SIZE < 1024
# error *spam-interface* BUFFER_SIZE constraints are not matched
#endif

#ifdef HAVE_SPAM_SPAMD
# define SPAMD_IDENT          "SPAMC/1.5"
# ifndef SUN_LEN
#  define SUN_LEN(SUP) \
        (sizeof(*(SUP)) - sizeof((SUP)->sun_path) + strlen((SUP)->sun_path))
# endif
#endif

#ifdef HAVE_SPAM_FILTER
  /* NELEM() of regmatch_t groups */
# define SPAM_FILTER_MATCHES  32u
#endif

enum spam_action {
   _SPAM_RATE,
   _SPAM_HAM,
   _SPAM_SPAM,
   _SPAM_FORGET
};

#if defined HAVE_SPAM_SPAMC || defined HAVE_SPAM_FILTER
struct spam_cf {
   char const        *cf_cmd;
   char              *cf_result; /* _SPAM_RATE: first response line */
   int               cf_waitstat;
   ui8_t             __pad[3];
   bool_t            cf_useshell;
   /* .cf_cmd may be adjusted for each call (`spamforget')... */
   char const        *cf_acmd;
   char const        *cf_a0;
   char const        *cf_env[4];
   sighandler_type   cf_otstp;
   sighandler_type   cf_ottin;
   sighandler_type   cf_ottou;
   sighandler_type   cf_ohup;
   sighandler_type   cf_opipe;
   sighandler_type   cf_oint;
   sighandler_type   cf_oquit;
};
#endif

#ifdef HAVE_SPAM_SPAMC
struct spam_spamc {
   struct spam_cf    c_super;
   char const        *c_cmd_arr[8];
};
#endif

#ifdef HAVE_SPAM_SPAMD
struct spam_spamd {
   struct str        d_user;
   sighandler_type   d_otstp;
   sighandler_type   d_ottin;
   sighandler_type   d_ottou;
   sighandler_type   d_ohup;
   sighandler_type   d_opipe;
   sighandler_type   d_oint;
   sighandler_type   d_oquit;
   struct sockaddr_un d_sun;
};
#endif

#ifdef HAVE_SPAM_FILTER
struct spam_filter {
   struct spam_cf    f_super;
   char const        *f_cmd_nospam; /* Working relative to current message.. */
   char const        *f_cmd_noham;
# ifdef HAVE_REGEX
   ui8_t             __pad[4];
   ui32_t            f_score_grpno; /* 0 for not set */
   regex_t           f_score_regex;
# endif
};
#endif

struct spam_vc {
   enum spam_action  vc_action;
   bool_t            vc_verbose;    /* Verbose output */
   bool_t            vc_progress;   /* "Progress meter" (mutual verbose) */
   ui8_t             __pad[2];
   bool_t            (*vc_act)(struct spam_vc *);
   void              (*vc_dtor)(struct spam_vc *);
   char              *vc_buffer;    /* I/O buffer, BUFFER_SIZE bytes */
   size_t            vc_mno;        /* Current message number */
   struct message    *vc_mp;        /* Current message */
   FILE              *vc_ifp;       /* Input stream on .vc_mp */
   union {
#ifdef HAVE_SPAM_SPAMC
      struct spam_spamc    spamc;
#endif
#ifdef HAVE_SPAM_SPAMD
      struct spam_spamd    spamd;
#endif
#ifdef HAVE_SPAM_FILTER
      struct spam_filter   filter;
#endif
#if defined HAVE_SPAM_SPAMC || defined HAVE_SPAM_FILTER
   struct spam_cf          cf;
#endif
   }                 vc_t;
   char const        *vc_esep;      /* Error separator for progress mode */
};

/* Indices according to enum spam_action */
static char const _spam_cmds[][16] = {
   "spamrate", "spamham", "spamspam", "spamforget"
};

/* Shared action setup */
static bool_t  _spam_action(enum spam_action sa, int *ip);

/* *spam-interface*=spamc: initialize, communicate */
#ifdef HAVE_SPAM_SPAMC
static bool_t  _spamc_setup(struct spam_vc *vcp);
static bool_t  _spamc_interact(struct spam_vc *vcp);
static void    _spamc_dtor(struct spam_vc *vcp);
#endif

/* *spam-interface*=spamd: initialize, communicate */
#ifdef HAVE_SPAM_SPAMD
static bool_t  _spamd_setup(struct spam_vc *vcp);
static bool_t  _spamd_interact(struct spam_vc *vcp);
#endif

/* *spam-interface*=filter: initialize, communicate */
#ifdef HAVE_SPAM_FILTER
static bool_t  _spamfilter_setup(struct spam_vc *vcp);
static bool_t  _spamfilter_interact(struct spam_vc *vcp);
static void    _spamfilter_dtor(struct spam_vc *vcp);
#endif

/* *spam-interface*=(spamc|filter): create child + communication */
#if defined HAVE_SPAM_SPAMC || defined HAVE_SPAM_FILTER
static void    _spam_cf_setup(struct spam_vc *vcp, bool_t useshell);
static bool_t  _spam_cf_interact(struct spam_vc *vcp);
#endif

/* Convert a floating-point spam rate into message.m_spamscore */
#if defined HAVE_SPAM_SPAMC || defined HAVE_SPAM_SPAMD ||\
   (defined HAVE_SPAM_FILTER && defined HAVE_REGEX)
static void    _spam_rate2score(struct spam_vc *vcp, char *buf);
#endif

static bool_t
_spam_action(enum spam_action sa, int *ip)
{
   struct spam_vc vc;
   size_t maxsize, skipped, cnt, curr;
   char const *cp;
   bool_t ok = FAL0;
   NYD_ENTER;

   memset(&vc, 0, sizeof vc);
   vc.vc_action = sa;
   vc.vc_verbose = ((options & OPT_VERB) != 0);
   vc.vc_progress = (!vc.vc_verbose && ((options & OPT_INTERACTIVE) != 0));
   vc.vc_esep = vc.vc_progress ? "\n" : "";

   /* Check and setup the desired spam interface */
   if ((cp = ok_vlook(spam_interface)) == NULL) {
      n_err(_("`%s': no *spam-interface* set\n"), _spam_cmds[sa]);
      goto jleave;
#ifdef HAVE_SPAM_SPAMC
   } else if (!asccasecmp(cp, "spamc")) {
       if (!_spamc_setup(&vc))
         goto jleave;
#endif
#ifdef HAVE_SPAM_SPAMD
   } else if (!asccasecmp(cp, "spamd")) {
      if (!_spamd_setup(&vc))
         goto jleave;
#endif
#ifdef HAVE_SPAM_FILTER
   } else if (!asccasecmp(cp, "filter")) {
      if (!_spamfilter_setup(&vc))
         goto jleave;
#endif
   } else {
      n_err(_("`%s': unknown / unsupported *spam-interface*: \"%s\"\n"),
         _spam_cmds[sa], cp);
      goto jleave;
   }

   /* *spam-maxsize* we do handle ourselfs instead */
   if ((cp = ok_vlook(spam_maxsize)) == NULL ||
         (maxsize = (size_t)strtoul(cp, NULL, 0)) == 0)
      maxsize = SPAM_MAXSIZE;

   /* Finally get an I/O buffer */
   vc.vc_buffer = salloc(BUFFER_SIZE);

   skipped = cnt = 0;
   if (vc.vc_progress) {
      while (ip[cnt] != 0)
         ++cnt;
   }
   for (curr = 0, ok = TRU1; *ip != 0; --cnt, ++curr, ++ip) {
      vc.vc_mno = (size_t)*ip;
      vc.vc_mp = message + vc.vc_mno - 1;
      if (sa == _SPAM_RATE)
         vc.vc_mp->m_spamscore = 0;

      if (vc.vc_mp->m_size > maxsize) {
         if (vc.vc_verbose)
            n_err(_("`%s': message %lu exceeds maxsize (%lu > %lu), skip\n"),
               _spam_cmds[sa], (ul_i)vc.vc_mno, (ul_i)(size_t)vc.vc_mp->m_size,
               (ul_i)maxsize);
         else if (vc.vc_progress) {
            fprintf(stdout, "\r%s: !%-6" PRIuZ " %6" PRIuZ "/%-6" PRIuZ,
               _spam_cmds[sa], vc.vc_mno, cnt, curr);
            fflush(stdout);
         }
         ++skipped;
      } else {
         if (vc.vc_verbose)
            n_err(_("`%s': message %lu\n"), _spam_cmds[sa], (ul_i)vc.vc_mno);
         else if (vc.vc_progress) {
            fprintf(stdout, "\r%s: .%-6" PRIuZ " %6" PRIuZ "/%-6" PRIuZ,
               _spam_cmds[sa], vc.vc_mno, cnt, curr);
            fflush(stdout);
         }

         setdot(vc.vc_mp);
         if ((vc.vc_ifp = setinput(&mb, vc.vc_mp, NEED_BODY)) == NULL) {
            n_err(_("%s`%s': cannot load message %lu: %s\n"),
               vc.vc_esep, _spam_cmds[sa], (ul_i)vc.vc_mno, strerror(errno));
            ok = FAL0;
            break;
         }

         if (!(ok = (*vc.vc_act)(&vc)))
            break;
      }
   }
   if (vc.vc_progress) {
      if (curr > 0)
         fprintf(stdout, _(" %s (%" PRIuZ "/%" PRIuZ " all/skipped)\n"),
            (ok ? "done" : "ERROR"), curr, skipped);
      fflush(stdout);
   }

   if (vc.vc_dtor != NULL)
      (*vc.vc_dtor)(&vc);
jleave:
   NYD_LEAVE;
   return !ok;
}

#ifdef HAVE_SPAM_SPAMC
static bool_t
_spamc_setup(struct spam_vc *vcp)
{
   struct spam_spamc *sscp;
   struct str str;
   char const **args, *cp;
   bool_t rv = FAL0;
   NYD2_ENTER;

   sscp = &vcp->vc_t.spamc;
   args = sscp->c_cmd_arr;

   if ((cp = ok_vlook(spamc_command)) == NULL) {
# ifdef SPAM_SPAMC_PATH
      cp = SPAM_SPAMC_PATH;
# else
      n_err(_("`%s': *spamc-command* is not set\n"),
         _spam_cmds[vcp->vc_action]);
      goto jleave;
# endif
   }
   *args++ = cp;

   switch (vcp->vc_action) {
   case _SPAM_RATE:
      *args = "-c";
      break;
   case _SPAM_HAM:
      args[1] = "ham";
      goto jlearn;
   case _SPAM_SPAM:
      args[1] = "spam";
      goto jlearn;
   case _SPAM_FORGET:
      args[1] = "forget";
jlearn:
      *args = "-L";
      ++args;
      break;
   }
   ++args;

   *args++ = "-l"; /* --log-to-stderr */
   *args++ = "-x"; /* No "safe callback", we need to react on errors! */

   if ((cp = ok_vlook(spamc_arguments)) != NULL)
      *args++ = cp;

   if ((cp = ok_vlook(spamc_user)) != NULL) {
      if (*cp == '\0')
         cp = myname;
      *args++ = "-u";
      *args++ = cp;
   }
   assert(PTR2SIZE(args - sscp->c_cmd_arr) <= NELEM(sscp->c_cmd_arr));

   *args = NULL;
   sscp->c_super.cf_cmd = str_concat_cpa(&str, sscp->c_cmd_arr, " ")->s;
   if (vcp->vc_verbose)
      n_err(_("spamc(1) via \"%s\"\n"), sscp->c_super.cf_cmd);

   _spam_cf_setup(vcp, FAL0);

   vcp->vc_act = &_spamc_interact;
   vcp->vc_dtor = &_spamc_dtor;
   rv = TRU1;
# ifndef SPAM_SPAMC_PATH
jleave:
# endif
   NYD2_LEAVE;
   return rv;
}

static bool_t
_spamc_interact(struct spam_vc *vcp)
{
   bool_t rv;
   NYD2_ENTER;

   if (!(rv = _spam_cf_interact(vcp)))
      goto jleave;

   vcp->vc_mp->m_flag &= ~(MSPAM | MSPAMUNSURE);
   if (vcp->vc_action != _SPAM_RATE) {
      if (vcp->vc_action == _SPAM_SPAM)
         vcp->vc_mp->m_flag |= MSPAM;
   } else {
      char *buf, *cp;

      switch (WEXITSTATUS(vcp->vc_t.spamc.c_super.cf_waitstat)) {
      case 1:
         vcp->vc_mp->m_flag |= MSPAM;
         /* FALLTHRU */
      case 0:
         break;
      default:
         rv = FAL0;
         goto jleave;
      }

      if ((cp = strchr(buf = vcp->vc_t.spamc.c_super.cf_result, '/')) != NULL)
         buf[PTR2SIZE(cp - buf)] = '\0';
      _spam_rate2score(vcp, buf);
   }
jleave:
   NYD2_LEAVE;
   return rv;
}

static void
_spamc_dtor(struct spam_vc *vcp)
{
   NYD2_ENTER;
   if (vcp->vc_t.spamc.c_super.cf_result != NULL)
      free(vcp->vc_t.spamc.c_super.cf_result);
   NYD2_LEAVE;
}
#endif /* HAVE_SPAM_SPAMC */

#ifdef HAVE_SPAM_SPAMD
static bool_t
_spamd_setup(struct spam_vc *vcp)
{
   struct spam_spamd *ssdp;
   char const *cp;
   size_t l;
   bool_t rv = FAL0;
   NYD2_ENTER;

   ssdp = &vcp->vc_t.spamd;

   if ((cp = ok_vlook(spamd_user)) != NULL) {
      if (*cp == '\0')
         cp = myname;
      ssdp->d_user.l = strlen(ssdp->d_user.s = UNCONST(cp));
   }

   if ((cp = ok_vlook(spamd_socket)) == NULL) {
      n_err(_("`%s': required *spamd-socket* is not set\n"),
         _spam_cmds[vcp->vc_action]);
      goto jleave;
   }
   if ((l = strlen(cp) +1) >= sizeof(ssdp->d_sun.sun_path)) {
      n_err(_("`%s': *spamd-socket* too long: \"%s\"\n"),
         _spam_cmds[vcp->vc_action], cp);
      goto jleave;
   }
   ssdp->d_sun.sun_family = AF_UNIX;
   memcpy(ssdp->d_sun.sun_path, cp, l);

   vcp->vc_act = &_spamd_interact;
   rv = TRU1;
jleave:
   NYD2_LEAVE;
   return rv;
}

static sigjmp_buf    __spamd_actjmp; /* TODO oneday, we won't need it no more */
static int volatile  __spamd_sig; /* TODO oneday, we won't need it no more */
static void
__spamd_onsig(int sig) /* TODO someday, we won't need it no more */
{
   NYD_X; /* Signal handler */
   __spamd_sig = sig;
   siglongjmp(__spamd_actjmp, 1);
}

static bool_t
_spamd_interact(struct spam_vc *vcp)
{
   struct spam_spamd *ssdp;
   size_t size, i;
   char *lp, *cp, * volatile headbuf = NULL;
   int volatile dsfd = -1;
   bool_t volatile rv = FAL0;
   NYD2_ENTER;

   ssdp = &vcp->vc_t.spamd;

   __spamd_sig = 0;
   hold_sigs();
   ssdp->d_otstp = safe_signal(SIGTSTP, SIG_DFL);
   ssdp->d_ottin = safe_signal(SIGTTIN, SIG_DFL);
   ssdp->d_ottou = safe_signal(SIGTTOU, SIG_DFL);
   ssdp->d_opipe = safe_signal(SIGPIPE, SIG_IGN);
   ssdp->d_ohup = safe_signal(SIGHUP, &__spamd_onsig);
   ssdp->d_oint = safe_signal(SIGINT, &__spamd_onsig);
   ssdp->d_oquit = safe_signal(SIGQUIT, &__spamd_onsig);
   if (sigsetjmp(__spamd_actjmp, 1)) {
      if (*vcp->vc_esep != '\0')
         n_err(vcp->vc_esep);
      goto jleave;
   }
   rele_sigs();

   if ((dsfd = socket(PF_UNIX, SOCK_STREAM, 0)) == -1) {
      n_err(_("%s`%s': can't create unix(4) socket: %s\n"),
         vcp->vc_esep, _spam_cmds[vcp->vc_action], strerror(errno));
      goto jleave;
   }

   if (connect(dsfd, (struct sockaddr*)&ssdp->d_sun, SUN_LEN(&ssdp->d_sun)) ==
         -1) {
      n_err(_("%s`%s': can't connect to *spam-socket*: %s\n"),
         vcp->vc_esep, _spam_cmds[vcp->vc_action], strerror(errno));
      close(dsfd);
      goto jleave;
   }

   /* The command header, finalized with an empty line.
    * This needs to be written in a single write(2)! */
# undef _X
# define _X(X) do {memcpy(lp, X, sizeof(X) -1); lp += sizeof(X) -1;} while (0)

   i = ((cp = ssdp->d_user.s) != NULL) ? ssdp->d_user.l : 0;
   lp = headbuf = ac_alloc(
         sizeof(NETLINE("A_VERY_LONG_COMMAND " SPAMD_IDENT)) +
         sizeof(NETLINE("Content-length: 9223372036854775807")) +
         ((cp != NULL) ? sizeof("User: ") + i + sizeof(NETNL) : 0) +
         sizeof(NETLINE("Message-class: spam")) +
         sizeof(NETLINE("Set: local")) +
         sizeof(NETLINE("Remove: local")) +
         sizeof(NETNL) /*+1*/);

   switch (vcp->vc_action) {
   case _SPAM_RATE:
      _X(NETLINE("CHECK " SPAMD_IDENT));
      break;
   case _SPAM_HAM:
   case _SPAM_SPAM:
   case _SPAM_FORGET:
      _X(NETLINE("TELL " SPAMD_IDENT));
      break;
   }

   lp += snprintf(lp, 0x7FFF, NETLINE("Content-length: %" PRIuZ),
         (size_t)vcp->vc_mp->m_size);

   if (cp != NULL) {
      _X("User: ");
      memcpy(lp, cp, i);
      lp += i;
      _X(NETNL);
   }

   switch (vcp->vc_action) {
   case _SPAM_RATE:
      _X(NETNL);
      break;
   case _SPAM_HAM:
      _X(NETLINE("Message-class: ham")
         NETLINE("Set: local")
         NETNL);
      break;
   case _SPAM_SPAM:
      _X(NETLINE("Message-class: spam")
         NETLINE("Set: local")
         NETNL);
      break;
   case _SPAM_FORGET:
      if (vcp->vc_mp->m_flag & MSPAM)
         _X(NETLINE("Message-class: spam"));
      else
         _X(NETLINE("Message-class: ham"));
      _X(NETLINE("Remove: local")
         NETNL);
      break;
   }
# undef _X

   i = PTR2SIZE(lp - headbuf);
   if (options & OPT_VERBVERB)
      n_err(">>> %.*s <<<\n", (int)i, headbuf);
   if (i != (size_t)write(dsfd, headbuf, i))
      goto jeso;

   /* Then simply pass through the message "as-is" */
   for (size = vcp->vc_mp->m_size; size > 0;) {
      i = fread(vcp->vc_buffer, sizeof *vcp->vc_buffer,
            MIN(size, BUFFER_SIZE), vcp->vc_ifp);
      if (i == 0) {
         if (ferror(vcp->vc_ifp))
            goto jeso;
         break;
      }
      size -= i;

      if (i != (size_t)write(dsfd, vcp->vc_buffer, i)) {
jeso:
         n_err(_("%s`%s': I/O on *spamd-socket* failed: %s\n"),
            vcp->vc_esep, _spam_cmds[vcp->vc_action], strerror(errno));
         goto jleave;
      }
   }

   /* We are finished, say so */
   shutdown(dsfd, SHUT_WR);

   /* Be aware on goto: i will be a line counter after this loop! */
   for (size = 0, i = BUFFER_SIZE -1;;) {
      ssize_t j = read(dsfd, vcp->vc_buffer + size, i);
      if (j == -1)
         goto jeso;
      if (j == 0)
         break;
      size += j;
      i -= j;
      /* For the current way of doing things a single read will suffice.
       * Note we'll be "penaltized" when awaiting EOF on the socket, at least
       * in blocking mode, so do avoid that and break off */
      break;
   }
   i = 0;
   vcp->vc_buffer[size] = '\0';

   if (size == 0 || size == BUFFER_SIZE) {
jebogus:
      n_err(_("%s`%s': bogus spamd(1) I/O interaction (%lu)\n"),
         vcp->vc_esep, _spam_cmds[vcp->vc_action], (ul_i)i);
# ifdef HAVE_DEVEL
      if (options & OPT_VERBVERB)
         n_err(">>> BUFFER: %s <<<\n", vcp->vc_buffer);
# endif
      goto jleave;
   }

   /* From the response, read those lines that interest us */
   for (lp = vcp->vc_buffer; size > 0; ++i) {
      cp = lp;
      lp = strchr(lp, NETNL[0]);
      if (lp == NULL)
         goto jebogus;
      lp[0] = '\0';
      if (lp[1] != NETNL[1])
         goto jebogus;
      lp += 2;
      size -= PTR2SIZE(lp - cp);

      if (i == 0) {
         if (!strncmp(cp, "SPAMD/1.1 0 EX_OK", sizeof("SPAMD/1.1 0 EX_OK") -1))
            continue;
         if (vcp->vc_action != _SPAM_RATE ||
               strstr(cp, "Service Unavailable") == NULL)
            goto jebogus;
         else {
            /* Unfortunately a missing --allow-tell drops connection.. */
            n_err(_("%s`%s': service not available in spamd(1) instance\n"),
               vcp->vc_esep, _spam_cmds[vcp->vc_action]);
            goto jleave;
         }
      } else if (i == 1) {
         switch (vcp->vc_action) {
         case _SPAM_RATE:
            if (strncmp(cp, "Spam: ", sizeof("Spam: ") -1))
               goto jebogus;
            cp += sizeof("Spam: ") -1;

            if (!strncmp(cp, "False", sizeof("False") -1)) {
               cp += sizeof("False") -1;
               vcp->vc_mp->m_flag &= ~(MSPAM | MSPAMUNSURE);
            } else if (!strncmp(cp, "True", sizeof("True") -1)) {
               cp += sizeof("True") -1;
               vcp->vc_mp->m_flag &= ~(MSPAM | MSPAMUNSURE);
               vcp->vc_mp->m_flag |= MSPAM;
            } else
               goto jebogus;

            while (blankspacechar(*cp))
               ++cp;

            if (*cp++ != ';')
               goto jebogus;
            else {
               char *xcp = strchr(cp, '/');
               if (xcp != NULL) {
                  size = PTR2SIZE(xcp - cp);
                  cp[size] = '\0';
               }
               _spam_rate2score(vcp, cp);
            }
            goto jdone;

         case _SPAM_HAM:
         case _SPAM_SPAM:
            /* Empty response means ok but "did nothing" */
            if (*cp != '\0' &&
                  strncmp(cp, "DidSet: local", sizeof("DidSet: local") -1))
               goto jebogus;
            if (*cp == '\0' && vcp->vc_verbose)
               n_err(_("\tBut spamd(1) \"did nothing\" for message\n"));
            vcp->vc_mp->m_flag &= ~(MSPAM | MSPAMUNSURE);
            if (vcp->vc_action == _SPAM_SPAM)
               vcp->vc_mp->m_flag |= MSPAM;
            goto jdone;

         case _SPAM_FORGET:
            if (*cp != '\0' &&
                  strncmp(cp, "DidRemove: local", sizeof("DidSet: local") -1))
               goto jebogus;
            if (*cp == '\0' && vcp->vc_verbose)
               n_err(_("\tBut spamd(1) \"did nothing\" for message\n"));
            vcp->vc_mp->m_flag &= ~(MSPAM | MSPAMUNSURE);
            goto jdone;
         }
      }
   }

jdone:
   rv = TRU1;
jleave:
   if (headbuf != NULL)
      ac_free(headbuf);
   if (dsfd >= 0)
      close(dsfd);

   safe_signal(SIGQUIT, ssdp->d_oquit);
   safe_signal(SIGINT, ssdp->d_oint);
   safe_signal(SIGHUP, ssdp->d_ohup);
   safe_signal(SIGPIPE, ssdp->d_opipe);
   safe_signal(SIGTSTP, ssdp->d_otstp);
   safe_signal(SIGTTIN, ssdp->d_ottin);
   safe_signal(SIGTTOU, ssdp->d_ottou);

   NYD2_LEAVE;
   if (__spamd_sig != 0) {
      sigset_t cset;
      sigemptyset(&cset);
      sigaddset(&cset, __spamd_sig);
      sigprocmask(SIG_UNBLOCK, &cset, NULL);
      n_raise(__spamd_sig);
      assert(rv == FAL0);
   }
   return rv;
}
#endif /* HAVE_SPAM_SPAMD */

#ifdef HAVE_SPAM_FILTER
static bool_t
_spamfilter_setup(struct spam_vc *vcp)
{
   struct spam_filter *sfp;
   char const *cp, *var;
   bool_t rv = FAL0;
   NYD2_ENTER;

   sfp = &vcp->vc_t.filter;

   switch (vcp->vc_action) {
   case _SPAM_RATE:
      cp = ok_vlook(spamfilter_rate);
      var = "spam-filter-rate";
      goto jonecmd;
   case _SPAM_HAM:
      cp = ok_vlook(spamfilter_ham);
      var = "spam-filter-ham";
      goto jonecmd;
   case _SPAM_SPAM:
      cp = ok_vlook(spamfilter_spam);
      var = "spam-filter-spam";
jonecmd:
      if (cp == NULL) {
jecmd:
         n_err(_("`%s': *%s* is not set\n"), _spam_cmds[vcp->vc_action], var);
         goto jleave;
      }
      sfp->f_super.cf_cmd = savestr(cp);
      break;
   case _SPAM_FORGET:
      var = "spam-filter-nospam";
      if ((cp =  ok_vlook(spamfilter_nospam)) == NULL)
         goto jecmd;
      sfp->f_cmd_nospam = savestr(cp);
      if ((cp =  ok_vlook(spamfilter_noham)) == NULL)
         goto jecmd;
      sfp->f_cmd_noham = savestr(cp);
      break;
   }

# ifdef HAVE_REGEX
   if (vcp->vc_action == _SPAM_RATE &&
         (cp = ok_vlook(spamfilter_rate_scanscore)) != NULL) {
      char const *bp;
      char *ep;

      var = strchr(cp, ';');
      if (var == NULL) {
         n_err(_("`%s': *spamfilter-rate-scanscore*: no `;' in \"%s\"\n"),
            _spam_cmds[vcp->vc_action], cp);
         goto jleave;
      }
      bp = var + 1;

      var = savestrbuf(cp, PTR2SIZE(var - cp));
      sfp->f_score_grpno = (ui32_t)strtoul(var, &ep, 0);
      if (var == ep || *ep != '\0') {
         n_err(_("`%s': *spamfilter-rate-scanscore*: bad group in \"%s\"\n"),
            _spam_cmds[vcp->vc_action], cp);
         goto jleave;
      }
      if (sfp->f_score_grpno >= SPAM_FILTER_MATCHES) {
         n_err(_("`%s': *spamfilter-rate-scanscore*: "
            "group %u excesses limit %u\n"),
            _spam_cmds[vcp->vc_action], sfp->f_score_grpno,
            SPAM_FILTER_MATCHES);
         goto jleave;
      }

      if (regcomp(&sfp->f_score_regex, bp, REG_EXTENDED | REG_ICASE)) {
         n_err(_("`%s': invalid *spamfilter-rate-scanscore* regex: \"%s\"\n"),
            _spam_cmds[vcp->vc_action], cp);
         goto jleave;
      }
      if (sfp->f_score_grpno > sfp->f_score_regex.re_nsub) {
         regfree(&sfp->f_score_regex);
         n_err(_("`%s': *spamfilter-rate-scanscore*: "
            "no group %u in \"%s\"\n"),
            _spam_cmds[vcp->vc_action], sfp->f_score_grpno, cp);
         goto jleave;
      }
   }
# endif /* HAVE_REGEX */

   _spam_cf_setup(vcp, TRU1);

   vcp->vc_act = &_spamfilter_interact;
   vcp->vc_dtor = &_spamfilter_dtor;
   rv = TRU1;
jleave:
   NYD2_LEAVE;
   return rv;
}

static bool_t
_spamfilter_interact(struct spam_vc *vcp)
{
# ifdef HAVE_REGEX
   regmatch_t rem[SPAM_FILTER_MATCHES], *remp;
   struct spam_filter *sfp;
   char *cp;
# endif
   bool_t rv;
   NYD2_ENTER;

   if (vcp->vc_action == _SPAM_FORGET)
      vcp->vc_t.cf.cf_cmd = (vcp->vc_mp->m_flag & MSPAM)
            ? vcp->vc_t.filter.f_cmd_nospam : vcp->vc_t.filter.f_cmd_noham;

   if (!(rv = _spam_cf_interact(vcp)))
      goto jleave;

   vcp->vc_mp->m_flag &= ~(MSPAM | MSPAMUNSURE);
   if (vcp->vc_action != _SPAM_RATE) {
      if (vcp->vc_action == _SPAM_SPAM)
         vcp->vc_mp->m_flag |= MSPAM;
      goto jleave;
   } else switch (WEXITSTATUS(vcp->vc_t.filter.f_super.cf_waitstat)) {
   case 2:
      vcp->vc_mp->m_flag |= MSPAMUNSURE;
      /* FALLTHRU */
   case 1:
      break;
   case 0:
      vcp->vc_mp->m_flag |= MSPAM;
      break;
   default:
      rv = FAL0;
      goto jleave;
   }

# ifdef HAVE_REGEX
   sfp = &vcp->vc_t.filter;

   if (sfp->f_score_grpno == 0)
      goto jleave;

   assert(sfp->f_super.cf_result != NULL);
   remp = rem + sfp->f_score_grpno;

   if (regexec(&sfp->f_score_regex, sfp->f_super.cf_result, NELEM(rem), rem,
         0) == REG_NOMATCH || (remp->rm_so | remp->rm_eo) < 0) {
      n_err(_("`%s': *spamfilter-rate-scanscore* "
         "doesn't match filter output!\n"),
         _spam_cmds[vcp->vc_action]);
      sfp->f_score_grpno = 0;
      goto jleave;
   }

   cp = sfp->f_super.cf_result;
   cp[remp->rm_eo] = '\0';
   cp += remp->rm_so;
   _spam_rate2score(vcp, cp);
# endif /* HAVE_REGEX */

jleave:
   NYD2_LEAVE;
   return rv;
}

static void
_spamfilter_dtor(struct spam_vc *vcp)
{
   struct spam_filter *sfp;
   NYD2_ENTER;

   sfp = &vcp->vc_t.filter;

   if (sfp->f_super.cf_result != NULL)
      free(sfp->f_super.cf_result);
# ifdef HAVE_REGEX
   if (sfp->f_score_grpno > 0)
      regfree(&sfp->f_score_regex);
# endif
   NYD2_LEAVE;
}
#endif /* HAVE_SPAM_FILTER */

#if defined HAVE_SPAM_SPAMC || defined HAVE_SPAM_FILTER
static void
_spam_cf_setup(struct spam_vc *vcp, bool_t useshell)
{
   struct str s;
   struct spam_cf *scfp;
   NYD2_ENTER;
   LCTA(3 < NELEM(scfp->cf_env));

   scfp = &vcp->vc_t.cf;

   if ((scfp->cf_useshell = useshell)) {
      scfp->cf_acmd = ok_vlook(SHELL);
      scfp->cf_a0 = "-c";
   }

   /* NAIL_FILENAME_GENERATED *//* TODO pathconf NAME_MAX; but user can create
    * TODO a file wherever he wants!  *Do* create a zero-size temporary file
    * TODO and give *that* path as NAIL_FILENAME_TEMPORARY, clean it up once
    * TODO the pipe returns?  Like this we *can* verify path/name issues! */
   scfp->cf_env[0] = str_concat_csvl(&s, NAILENV_FILENAME_GENERATED, "=",
         getrandstring(MIN(NAME_MAX / 4, 16)), NULL)->s;

   scfp->cf_env[1] = str_concat_csvl(&s, NAILENV_TMPDIR, "=", tempdir, NULL)->s;
   scfp->cf_env[2] = str_concat_csvl(&s, "TMPDIR", "=", tempdir, NULL)->s;
   scfp->cf_env[3] = NULL;
   NYD2_LEAVE;
}

static sigjmp_buf    __spam_cf_actjmp; /* TODO someday, we won't need it */
static int volatile  __spam_cf_sig; /* TODO someday, we won't need it */
static void
__spam_cf_onsig(int sig) /* TODO someday, we won't need it no more */
{
   NYD_X; /* Signal handler */
   __spam_cf_sig = sig;
   siglongjmp(__spam_cf_actjmp, 1);
}

static bool_t
_spam_cf_interact(struct spam_vc *vcp)
{
   struct spam_cf *scfp;
   int p2c[2], c2p[2];
   sigset_t cset;
   char const *cp;
   size_t size;
   pid_t volatile pid;
   enum {
      _NONE    = 0,
      _SIGHOLD = 1<<0,
      _P2C_0   = 1<<1,
      _P2C_1   = 1<<2,
      _P2C     = _P2C_0 | _P2C_1,
      _C2P_0   = 1<<3,
      _C2P_1   = 1<<4,
      _C2P     = _C2P_0 | _C2P_1,
      _JUMPED  = 1<<5,
      _RUNNING = 1<<6,
      _GOODRUN = 1<<7,
      _ERRORS  = 1<<8
   } volatile state = _NONE;
   NYD2_ENTER;

   scfp = &vcp->vc_t.cf;
   if (scfp->cf_result != NULL) {
      free(scfp->cf_result);
      scfp->cf_result = NULL;
   }

   /* TODO Avoid that we jump away; yet necessary signal mess */
   /*__spam_cf_sig = 0;*/
   hold_sigs();
   state |= _SIGHOLD;
   scfp->cf_otstp = safe_signal(SIGTSTP, SIG_DFL);
   scfp->cf_ottin = safe_signal(SIGTTIN, SIG_DFL);
   scfp->cf_ottou = safe_signal(SIGTTOU, SIG_DFL);
   scfp->cf_opipe = safe_signal(SIGPIPE, SIG_IGN);
   scfp->cf_ohup = safe_signal(SIGHUP, &__spam_cf_onsig);
   scfp->cf_oint = safe_signal(SIGINT, &__spam_cf_onsig);
   scfp->cf_oquit = safe_signal(SIGQUIT, &__spam_cf_onsig);
   /* Keep sigs blocked */
   pid = 0; /* cc uninit */

   if (!pipe_cloexec(p2c)) {
      n_err(_("%s`%s': cannot create parent pipe: %s\n"),
         vcp->vc_esep, _spam_cmds[vcp->vc_action], strerror(errno));
      goto jtail;
   }
   state |= _P2C;

   if (!pipe_cloexec(c2p)) {
      n_err(_("%s`%s': cannot create child pipe: %s\n"),
         vcp->vc_esep, _spam_cmds[vcp->vc_action], strerror(errno));
      goto jtail;
   }
   state |= _C2P;

   if (sigsetjmp(__spam_cf_actjmp, 1)) {
      if (*vcp->vc_esep != '\0')
         n_err(vcp->vc_esep);
      state |= _JUMPED;
      goto jtail;
   }
   rele_sigs();
   state &= ~_SIGHOLD;

   /* Start our command as requested */
   sigemptyset(&cset);
   if ((pid = start_command(
         (scfp->cf_acmd != NULL ? scfp->cf_acmd : scfp->cf_cmd),
         &cset, p2c[0], c2p[1],
         scfp->cf_a0, (scfp->cf_acmd != NULL ? scfp->cf_cmd : NULL), NULL,
         scfp->cf_env)) < 0) {
      state |= _ERRORS;
      goto jtail;
   }
   state |= _RUNNING;
   close(p2c[0]);
   state &= ~_P2C_0;

   /* Yes, we could sendmp(SEND_MBOX), but simply passing through the MBOX
    * content does the same in effect, however much more efficiently.
    * XXX NOTE: this may mean we pass a message without From_ line! */
   for (size = vcp->vc_mp->m_size; size > 0;) {
      size_t i = fread(vcp->vc_buffer, 1, MIN(size, BUFFER_SIZE), vcp->vc_ifp);
      if (i == 0) {
         if (ferror(vcp->vc_ifp))
            state |= _ERRORS;
         break;
      }
      size -= i;
      if (i != (size_t)write(p2c[1], vcp->vc_buffer, i)) {
         state |= _ERRORS;
         break;
      }
   }

jtail:
   /* TODO Quite racy -- block anything for a while? */
   if (state & _SIGHOLD) {
      state &= ~_SIGHOLD;
      rele_sigs();
   }

   if (state & _P2C_0) {
      state &= ~_P2C_0;
      close(p2c[0]);
   }
   if (state & _C2P_1) {
      state &= ~_C2P_1;
      close(c2p[1]);
   }
   /* And cause EOF for the reader */
   if (state & _P2C_1) {
      state &= ~_P2C_1;
      close(p2c[1]);
   }

   if (state & _RUNNING) {
      if (!(state & _ERRORS) &&
            vcp->vc_action == _SPAM_RATE && !(state & (_JUMPED | _ERRORS))) {
         ssize_t i = read(c2p[0], vcp->vc_buffer, BUFFER_SIZE - 1);
         if (i > 0) {
            vcp->vc_buffer[i] = '\0';
            if ((cp = strchr(vcp->vc_buffer, NETNL[0])) == NULL &&
                  (cp = strchr(vcp->vc_buffer, NETNL[1])) == NULL) {
               n_err(_("%s`%s': program generates too much output: \"%s\"\n"),
                  vcp->vc_esep, _spam_cmds[vcp->vc_action], scfp->cf_cmd);
               state |= _ERRORS;
            } else {
               scfp->cf_result = sbufdup(vcp->vc_buffer,
                     PTR2SIZE(cp - vcp->vc_buffer));
/* FIXME consume child output until EOF??? */
            }
         } else if (i != 0)
            state |= _ERRORS;
      }

      state &= ~_RUNNING;
      wait_child(pid, &scfp->cf_waitstat);
      if (WIFEXITED(scfp->cf_waitstat))
         state |= _GOODRUN;
   }

   if (state & _C2P_0) {
      state &= ~_C2P_0;
      close(c2p[0]);
   }

   safe_signal(SIGQUIT, scfp->cf_oquit);
   safe_signal(SIGINT, scfp->cf_oint);
   safe_signal(SIGHUP, scfp->cf_ohup);
   safe_signal(SIGPIPE, scfp->cf_opipe);
   safe_signal(SIGTSTP, scfp->cf_otstp);
   safe_signal(SIGTTIN, scfp->cf_ottin);
   safe_signal(SIGTTOU, scfp->cf_ottou);

   NYD2_LEAVE;
   if (state & _JUMPED) {
      assert(vcp->vc_dtor != NULL);
      (*vcp->vc_dtor)(vcp);

      sigemptyset(&cset);
      sigaddset(&cset, __spam_cf_sig);
      sigprocmask(SIG_UNBLOCK, &cset, NULL);
      n_raise(__spam_cf_sig);
   }
   return !(state & (_JUMPED | _ERRORS));
}
#endif /* HAVE_SPAM_SPAMC || HAVE_SPAM_FILTER */

#if defined HAVE_SPAM_SPAMC || defined HAVE_SPAM_SPAMD ||\
   (defined HAVE_SPAM_FILTER && defined HAVE_REGEX)
static void
_spam_rate2score(struct spam_vc *vcp, char *buf)
{
   char *cp;
   ui32_t m, s;
   NYD2_ENTER;

   m = (ui32_t)strtol(buf, &cp, 10);
   if (cp == buf)
      goto jleave;

   s = 0;
   if (*cp++ != '\0') {
      /* Floating-point rounding for non-mathematicians */
      char c1, c2, c3;
      if ((c1 = cp[0]) != '\0' && (c2 = cp[1]) != '\0' &&
            (c3 = cp[2]) != '\0') {
         cp[2] = '\0';
         if (c3 >= '5') {
            if (c2 == '9') {
               if (c1 == '9') {
                  ++m;
                  goto jscore_ok;
               } else
                  cp[0] = ++c1;
               c2 = '0';
            } else
               ++c2;
            cp[1] = c2;
         }
      }
      s = (ui32_t)strtol(cp, NULL, 10);
   }

jscore_ok:
   vcp->vc_mp->m_spamscore = (m << 8) | s;
jleave:
   NYD2_LEAVE;
}
#endif /* _SPAM_SPAMC || _SPAM_SPAMD || (_SPAM_FILTER && HAVE_REGEX) */

FL int
c_spam_clear(void *v)
{
   int *ip;
   NYD_ENTER;

   for (ip = v; *ip != 0; ++ip)
      message[(size_t)*ip - 1].m_flag &= ~(MSPAM | MSPAMUNSURE);
   NYD_LEAVE;
   return 0;
}

FL int
c_spam_set(void *v)
{
   int *ip;
   NYD_ENTER;

   for (ip = v; *ip != 0; ++ip) {
      message[(size_t)*ip - 1].m_flag &= ~(MSPAM | MSPAMUNSURE);
      message[(size_t)*ip - 1].m_flag |= MSPAM;
   }
   NYD_LEAVE;
   return 0;
}

FL int
c_spam_forget(void *v)
{
   int rv;
   NYD_ENTER;

   rv = _spam_action(_SPAM_FORGET, (int*)v) ? OKAY : STOP;
   NYD_LEAVE;
   return rv;
}

FL int
c_spam_ham(void *v)
{
   int rv;
   NYD_ENTER;

   rv = _spam_action(_SPAM_HAM, (int*)v) ? OKAY : STOP;
   NYD_LEAVE;
   return rv;
}

FL int
c_spam_rate(void *v)
{
   int rv;
   NYD_ENTER;

   rv = _spam_action(_SPAM_RATE, (int*)v) ? OKAY : STOP;
   NYD_LEAVE;
   return rv;
}

FL int
c_spam_spam(void *v)
{
   int rv;
   NYD_ENTER;

   rv = _spam_action(_SPAM_SPAM, (int*)v) ? OKAY : STOP;
   NYD_LEAVE;
   return rv;
}
#endif /* HAVE_SPAM */

/* s-it-mode */
