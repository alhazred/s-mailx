/*@ S-nail - a mail user agent derived from Berkeley Mail.
 *@ TTY (command line) editing interaction.
 *@ Because we have multiple line-editor implementations, including our own
 *@ M(ailx) L(ine) E(ditor), change the file layout a bit and place those
 *@ one after the other below the other externals.
 *
 * Copyright (c) 2012 - 2016 Steffen (Daode) Nurpmeso <steffen@sdaoden.eu>.
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
#define n_FILE tty

#ifndef HAVE_AMALGAMATION
# include "nail.h"
#endif

#if defined HAVE_MLE || defined HAVE_TERMCAP
# define a_TTY_SIGNALS
#endif

/* History support macros */
#ifdef HAVE_HISTORY
# define a_TTY_HISTFILE(S) \
do{\
   char const *__hist_obsolete = ok_vlook(NAIL_HISTFILE);\
   if(__hist_obsolete != NULL)\
      OBSOLETE(_("please use *history-file* instead of *NAIL_HISTFILE*"));\
   S = ok_vlook(history_file);\
   if((S) == NULL)\
      (S) = __hist_obsolete;\
   if((S) != NULL)\
      S = fexpand(S, FEXP_LOCAL | FEXP_NSHELL);\
}while(0)

# define a_TTY_HISTSIZE(V) \
do{\
   char const *__hist_obsolete = ok_vlook(NAIL_HISTSIZE);\
   char const *__sv = ok_vlook(history_size);\
   long __rv;\
   if(__hist_obsolete != NULL)\
      OBSOLETE(_("please use *history-size* instead of *NAIL_HISTSIZE*"));\
   if(__sv == NULL)\
      __sv = __hist_obsolete;\
   if(__sv == NULL || (__rv = strtol(__sv, NULL, 10)) == 0)\
      __rv = HIST_SIZE;\
   else if(__rv < 0)\
      __rv = 0;\
   (V) = __rv;\
}while(0)

# define a_TTY_CHECK_ADDHIST(S,ISGABBY,NOACT) \
do{\
   if(!(pstate & (PS_ROOT | PS_LINE_EDITOR_INIT)) ||\
         ok_blook(line_editor_disable) ||\
         ((ISGABBY) && !ok_blook(history_gabby)) ||\
         spacechar(*(S)) || *(S) == '\0')\
      NOACT;\
}while(0)

# define C_HISTORY_SHARED \
   char **argv = v;\
   long entry;\
   NYD_ENTER;\
\
   if(ok_blook(line_editor_disable)){\
      n_err(_("history: *line-editor-disable* is set\n"));\
      goto jerr;\
   }\
   if(!(pstate & PS_LINE_EDITOR_INIT)){\
      n_tty_init();\
      assert(pstate & PS_LINE_EDITOR_INIT);\
   }\
   if(*argv == NULL)\
      goto jlist;\
   if(argv[1] != NULL)\
      goto jerr;\
   if(!asccasecmp(*argv, "show"))\
      goto jlist;\
   if(!asccasecmp(*argv, "clear"))\
      goto jclear;\
   if((entry = strtol(*argv, argv, 10)) > 0 && **argv == '\0')\
      goto jentry;\
jerr:\
   n_err(_("Synopsis: history: %s\n"),\
      /* Same string as in cmd_tab.h, still hoping...) */\
      _("<show> (default), <clear> or select <NO> from editor history"));\
   v = NULL;\
jleave:\
   NYD_LEAVE;\
   return (v == NULL ? !STOP : !OKAY); /* xxx 1:bad 0:good -- do some */
#endif /* HAVE_HISTORY */

#ifdef a_TTY_SIGNALS
static sighandler_type a_tty_oint, a_tty_oquit, a_tty_oterm,
   a_tty_ohup,
   a_tty_otstp, a_tty_ottin, a_tty_ottou;
#endif

#ifdef a_TTY_SIGNALS
static void a_tty_sigs_up(void), a_tty_sigs_down(void);
#endif

#ifdef a_TTY_SIGNALS
static void
a_tty_sigs_up(void){
   sigset_t nset, oset;
   NYD2_ENTER;

   sigfillset(&nset);

   sigprocmask(SIG_BLOCK, &nset, &oset);
   a_tty_oint = safe_signal(SIGINT, &n_tty_signal);
   a_tty_oquit = safe_signal(SIGQUIT, &n_tty_signal);
   a_tty_oterm = safe_signal(SIGTERM, &n_tty_signal);
   a_tty_ohup = safe_signal(SIGHUP, &n_tty_signal);
   a_tty_otstp = safe_signal(SIGTSTP, &n_tty_signal);
   a_tty_ottin = safe_signal(SIGTTIN, &n_tty_signal);
   a_tty_ottou = safe_signal(SIGTTOU, &n_tty_signal);
   sigprocmask(SIG_SETMASK, &oset, NULL);
   NYD2_LEAVE;
}

static void
a_tty_sigs_down(void){
   sigset_t nset, oset;
   NYD2_ENTER;

   sigfillset(&nset);

   sigprocmask(SIG_BLOCK, &nset, &oset);
   safe_signal(SIGINT, a_tty_oint);
   safe_signal(SIGQUIT, a_tty_oquit);
   safe_signal(SIGTERM, a_tty_oterm);
   safe_signal(SIGHUP, a_tty_ohup);
   safe_signal(SIGTSTP, a_tty_otstp);
   safe_signal(SIGTTIN, a_tty_ottin);
   safe_signal(SIGTTOU, a_tty_ottou);
   sigprocmask(SIG_SETMASK, &oset, NULL);
   NYD2_LEAVE;
}
#endif /* a_TTY_SIGNALS */

static sigjmp_buf a_tty__actjmp; /* TODO someday, we won't need it no more */
static void
a_tty__acthdl(int s) /* TODO someday, we won't need it no more */
{
   NYD_X; /* Signal handler */
   termios_state_reset();
   siglongjmp(a_tty__actjmp, s);
}

FL bool_t
getapproval(char const * volatile prompt, bool_t noninteract_default)
{
   sighandler_type volatile oint, ohup;
   bool_t volatile rv;
   int volatile sig;
   NYD_ENTER;

   if (!(options & OPT_INTERACTIVE)) {
      sig = 0;
      rv = noninteract_default;
      goto jleave;
   }
   rv = FAL0;

   /* C99 */{
      char const *quest = noninteract_default
            ? _("[yes]/no? ") : _("[no]/yes? ");

      if (prompt == NULL)
         prompt = _("Continue");
      prompt = savecatsep(prompt, ' ', quest);
   }

   oint = safe_signal(SIGINT, SIG_IGN);
   ohup = safe_signal(SIGHUP, SIG_IGN);
   if ((sig = sigsetjmp(a_tty__actjmp, 1)) != 0)
      goto jrestore;
   safe_signal(SIGINT, &a_tty__acthdl);
   safe_signal(SIGHUP, &a_tty__acthdl);

   if (n_lex_input(n_LEXINPUT_CTX_BASE | n_LEXINPUT_NL_ESC, prompt,
         &termios_state.ts_linebuf, &termios_state.ts_linesize, NULL) >= 0)
      rv = (boolify(termios_state.ts_linebuf, UIZ_MAX,
            noninteract_default) > 0);
jrestore:
   termios_state_reset();

   safe_signal(SIGHUP, ohup);
   safe_signal(SIGINT, oint);
jleave:
   NYD_LEAVE;
   if (sig != 0)
      n_raise(sig);
   return rv;
}

#ifdef HAVE_SOCKETS
FL char *
getuser(char const * volatile query) /* TODO v15-compat obsolete */
{
   sighandler_type volatile oint, ohup;
   char * volatile user = NULL;
   int volatile sig;
   NYD_ENTER;

   if (query == NULL)
      query = _("User: ");

   oint = safe_signal(SIGINT, SIG_IGN);
   ohup = safe_signal(SIGHUP, SIG_IGN);
   if ((sig = sigsetjmp(a_tty__actjmp, 1)) != 0)
      goto jrestore;
   safe_signal(SIGINT, &a_tty__acthdl);
   safe_signal(SIGHUP, &a_tty__acthdl);

   if (n_lex_input(n_LEXINPUT_CTX_BASE | n_LEXINPUT_NL_ESC, query,
         &termios_state.ts_linebuf, &termios_state.ts_linesize, NULL) >= 0)
      user = termios_state.ts_linebuf;
jrestore:
   termios_state_reset();

   safe_signal(SIGHUP, ohup);
   safe_signal(SIGINT, oint);
   NYD_LEAVE;
   if (sig != 0)
      n_raise(sig);
   return user;
}

FL char *
getpassword(char const *query)
{
   sighandler_type volatile oint, ohup;
   struct termios tios;
   char * volatile pass = NULL;
   int volatile sig;
   NYD_ENTER;

   if (query == NULL)
      query = _("Password: ");
   fputs(query, stdout);
   fflush(stdout);

   /* FIXME everywhere: tcsetattr() generates SIGTTOU when we're not in
    * FIXME foreground pgrp, and can fail with EINTR!! also affects
    * FIXME termios_state_reset() */
   if (options & OPT_TTYIN) {
      tcgetattr(STDIN_FILENO, &termios_state.ts_tios);
      memcpy(&tios, &termios_state.ts_tios, sizeof tios);
      termios_state.ts_needs_reset = TRU1;
      tios.c_iflag &= ~(ISTRIP);
      tios.c_lflag &= ~(ECHO | ECHOE | ECHOK | ECHONL);
   }

   oint = safe_signal(SIGINT, SIG_IGN);
   ohup = safe_signal(SIGHUP, SIG_IGN);
   if ((sig = sigsetjmp(a_tty__actjmp, 1)) != 0)
      goto jrestore;
   safe_signal(SIGINT, &a_tty__acthdl);
   safe_signal(SIGHUP, &a_tty__acthdl);

   if (options & OPT_TTYIN)
      tcsetattr(STDIN_FILENO, TCSAFLUSH, &tios);

   if (readline_restart(stdin, &termios_state.ts_linebuf,
         &termios_state.ts_linesize, 0) >= 0)
      pass = termios_state.ts_linebuf;
jrestore:
   termios_state_reset();
   if (options & OPT_TTYIN)
      putc('\n', stdout);

   safe_signal(SIGHUP, ohup);
   safe_signal(SIGINT, oint);
   NYD_LEAVE;
   if (sig != 0)
      n_raise(sig);
   return pass;
}
#endif /* HAVE_SOCKETS */

/*
 * MLE: the Mailx-Line-Editor, our homebrew editor
 * (inspired from NetBSDs sh(1) and dash(1)s hetio.c).
 *
 * Only used in interactive mode, simply use STDIN_FILENO as point of interest.
 * TODO . This code should be splitted in funs/raw input/bind modules.
 * TODO . After I/O layer rewrite, also "output to STDIN_FILENO".
 * TODO . We work with wide characters, but not for buffer takeovers and
 * TODO   cell2save()ings.  This should be changed.  For the former the buffer
 * TODO   thus needs to be converted to wide first, and then simply be fed in.
 * TODO . We repaint too much.  To overcome this use the same approach that my
 * TODO   terminal library uses, add a true "virtual screen line" that stores
 * TODO   the actually visible content, keep a notion of "first modified slot"
 * TODO   and "last modified slot" (including "unknown" and "any" specials),
 * TODO   update that virtual instead, then synchronize what has truly changed.
 * TODO   I.e., add an indirection layer.
 * TODO . No BIDI support.
 * TODO . `bind': we currently use only one lookup tree.
 * TODO   For absolute graceful behaviour in conjunction (with HAVE_TERMCAP) we
 * TODO   need a lower level tree, which possibly combines bytes into "symbolic
 * TODO   wchar_t values", into "keys" that is, as applicable, and an upper
 * TODO   layer which only works on "keys" in order to possibly combine them
 * TODO   into key sequences.  We can reuse existent tree code for that.
 * TODO   We need an additional hashmap which maps termcap/terminfo names to
 * TODO   (their byte representations and) a dynamically assigned unique
 * TODO   "symbolic wchar_t value".  This implies we may have incompatibilities
 * TODO   when __STDC_ISO_10646__ is not defined.  Also we do need takeover-
 * TODO   bytes storage, but it can be a string_creat_auto in the line struct.
 * TODO   Until then we can run into ambiguities; in rare occasions.
 */
#ifdef HAVE_MLE
/* To avoid memory leaks etc. with the current codebase that simply longjmp(3)s
 * we're forced to use the very same buffer--the one that is passed through to
 * us from the outside--to store anything we need, i.e., a "struct cell[]", and
 * convert that on-the-fly back to the plain char* result once we're done.
 * To simplify our live, use savestr() buffers for all other needed memory */

# ifdef HAVE_KEY_BINDINGS
/* Default *bind-timeout* key-sequence continuation timeout, in tenths of
 * a second.  Must fit in 8-bit!  Update the manual upon change! */
#  define a_TTY_BIND_TIMEOUT 2
#  define a_TTY_BIND_TIMEOUT_MAX SI8_MAX

n_CTAV(a_TTY_BIND_TIMEOUT_MAX <= UI8_MAX);

/* We have a chicken-and-egg problem with `bind' and our termcap layer,
 * because we may not initialize the latter automatically to allow users to
 * specify *termcap-disable* and let it mean exactly that.
 * On the other hand users can be expected to use `bind' in resource file(s).
 * Therefore bindings which involve termcap/terminfo sequences, and which are
 * defined before PS_STARTED signals usability of termcap/terminfo, will be
 * (partially) delayed until tty_init() is called.
 * And we preallocate space for the expansion of the resolved capability */
#  define a_TTY_BIND_CAPNAME_MAX 15
#  define a_TTY_BIND_CAPEXP_ROUNDUP 16

n_CTAV(ISPOW2(a_TTY_BIND_CAPEXP_ROUNDUP));
n_CTA(a_TTY_BIND_CAPEXP_ROUNDUP <= SI8_MAX / 2, "Variable must fit in 6-bit");
n_CTA(a_TTY_BIND_CAPEXP_ROUNDUP >= 8, "Variable too small");
# endif /* HAVE_KEY_BINDINGS */

/* The maximum size (of a_tty_cell's) in a line */
# define a_TTY_LINE_MAX SI32_MAX

/* (Some more CTAs around) */
n_CTA(a_TTY_LINE_MAX <= SI32_MAX,
   "a_TTY_LINE_MAX larger than SI32_MAX, but the MLE uses 32-bit arithmetic");

/* When shall the visual screen be scrolled, in % of usable screen width */
# define a_TTY_SCROLL_MARGIN_LEFT 15
# define a_TTY_SCROLL_MARGIN_RIGHT 10

/* fexpand() flags for expand-on-tab */
# define a_TTY_TAB_FEXP_FL (FEXP_FULL | FEXP_SILENT | FEXP_MULTIOK)

/* Columns to ripoff: outermost may not be touched, plus position indicator.
 * Must thus be at least 1, but should be >= 1+4 to dig the position indicator
 * that we place (if there is sufficient space) */
# define a_TTY_WIDTH_RIPOFF 5

/* The implementation of the MLE functions always exists, and is based upon
 * the a_TTY_BIND_FUN_* constants, so most of this enum is always necessary */
enum a_tty_bind_flags{
# ifdef HAVE_KEY_BINDINGS
   a_TTY_BIND_RESOLVE = 1u<<8,   /* Term cap. yet needs to be resolved */
   a_TTY_BIND_DEFUNCT = 1u<<9,   /* Unicode/term cap. used but not avail. */
   a_TTY__BIND_MASK = a_TTY_BIND_RESOLVE | a_TTY_BIND_DEFUNCT,
   /* MLE fun assigned to a one-byte-sequence: this may be used for special
    * key-sequence bypass processing */
   a_TTY_BIND_MLE1CNTRL = 1u<<10,
   a_TTY_BIND_NOCOMMIT = 1u<<11, /* Expansion shall be editable */
# endif

   /* MLE internal commands */
   a_TTY_BIND_FUN_INTERNAL = 1u<<15,
   a_TTY__BIND_FUN_SHIFT = 16u,
   a_TTY__BIND_FUN_SHIFTMAX = 24u,
   a_TTY__BIND_FUN_MASK = ((1u << a_TTY__BIND_FUN_SHIFTMAX) - 1) &
         ~((1u << a_TTY__BIND_FUN_SHIFT) - 1),
# define a_TTY_BIND_FUN_REDUCE(X) \
   (((ui32_t)(X) & a_TTY__BIND_FUN_MASK) >> a_TTY__BIND_FUN_SHIFT)
# define a_TTY_BIND_FUN_EXPAND(X) \
   (((ui32_t)(X) & (a_TTY__BIND_FUN_MASK >> a_TTY__BIND_FUN_SHIFT)) << \
      a_TTY__BIND_FUN_SHIFT)
# undef a_X
# define a_X(N,I)\
   a_TTY_BIND_FUN_ ## N = a_TTY_BIND_FUN_EXPAND(I),

   a_X(BELL,  0)
   a_X(GO_BWD,  1) a_X(GO_FWD,  2)
   a_X(GO_WORD_BWD,  3) a_X(GO_WORD_FWD,  4)
   a_X(GO_HOME,  5) a_X(GO_END,  6)
   a_X(DEL_BWD,  7) a_X(DEL_FWD,   8)
   a_X(SNARF_WORD_BWD,  9) a_X(SNARF_WORD_FWD, 10)
   a_X(SNARF_END, 11) a_X(SNARF_LINE, 12)
   a_X(HIST_BWD, 13) a_X(HIST_FWD, 14)
   a_X(HIST_SRCH_BWD, 15) a_X(HIST_SRCH_FWD, 16)
   a_X(REPAINT, 17)
   a_X(QUOTE_RNDTRIP, 18)
   a_X(PROMPT_CHAR, 19)
   a_X(COMPLETE, 20)
   a_X(PASTE, 21)

   a_X(CANCEL, 22)
   a_X(RESET, 23)
   a_X(FULLRESET, 24)
   a_X(COMMIT, 25) /* Must be last one! */
# undef a_X

   a_TTY__BIND_LAST = 1<<25
};
# ifdef HAVE_KEY_BINDINGS
n_CTA((ui32_t)a_TTY_BIND_RESOLVE > (ui32_t)n__LEXINPUT_CTX_MAX,
   "Bit carrier lower boundary must be raised to avoid value sharing");
# endif
n_CTA(a_TTY_BIND_FUN_EXPAND(a_TTY_BIND_FUN_COMMIT) <
      (1 << a_TTY__BIND_FUN_SHIFTMAX),
   "Bit carrier range must be expanded to represent necessary bits");
n_CTA(a_TTY__BIND_LAST >= (1u << a_TTY__BIND_FUN_SHIFTMAX),
   "Bit carrier upper boundary must be raised to avoid value sharing");
n_CTA(UICMP(64, a_TTY__BIND_LAST, <=, SI32_MAX),
   "Flag bits excess storage datatype" /* And we need one bit free */);

enum a_tty_fun_status{
   a_TTY_FUN_STATUS_OK,       /* Worked, next character */
   a_TTY_FUN_STATUS_COMMIT,   /* Line done */
   a_TTY_FUN_STATUS_RESTART,  /* Complete restart, reset multibyte etc. */
   a_TTY_FUN_STATUS_END       /* End, return EOF */
};

enum a_tty_visual_flags{
   a_TTY_VF_NONE,
   a_TTY_VF_MOD_CURSOR = 1u<<0,  /* Cursor moved */
   a_TTY_VF_MOD_CONTENT = 1u<<1, /* Content modified */
   a_TTY_VF_MOD_DIRTY = 1u<<2,   /* Needs complete repaint */
   a_TTY_VF_MOD_SINGLE = 1u<<3,  /* TODO Drop when indirection as above comes */
   a_TTY_VF_REFRESH = a_TTY_VF_MOD_DIRTY | a_TTY_VF_MOD_CURSOR |
         a_TTY_VF_MOD_CONTENT | a_TTY_VF_MOD_SINGLE,
   a_TTY_VF_BELL = 1u<<8,        /* Ring the bell */
   a_TTY_VF_SYNC = 1u<<9,        /* Flush/Sync I/O channel */

   a_TTY_VF_ALL_MASK = a_TTY_VF_REFRESH | a_TTY_VF_BELL | a_TTY_VF_SYNC,
   a_TTY__VF_LAST = a_TTY_VF_SYNC
};

# ifdef HAVE_KEY_BINDINGS
struct a_tty_bind_ctx{
   struct a_tty_bind_ctx *tbc_next;
   char *tbc_seq;       /* quence as given (poss. re-quoted), in .tb__buf */
   char *tbc_exp;       /* ansion, in .tb__buf */
   /* The .tbc_seq'uence with any terminal capabilities resolved; in fact an
    * array of structures, the first entry of which is {si32_t buf_len_iscap;}
    * where the signed bit indicates whether the buffer is a resolved terminal
    * capability instead of a (possibly multibyte) character.  In .tbc__buf */
   char *tbc_cnv;
   ui32_t tbc_seq_len;
   ui32_t tbc_exp_len;
   ui32_t tbc_cnv_len;
   ui32_t tbc_flags;
   char tbc__buf[VFIELD_SIZE(0)];
};

struct a_tty_bind_ctx_map{
   enum n_lexinput_flags tbcm_ctx;
   char const tbcm_name[12];  /* Name of `bind' context */
};
# endif /* HAVE_KEY_BINDINGS */

struct a_tty_bind_default_tuple{
   bool_t tbdt_iskey;   /* Whether this is a control key; else termcap query */
   char tbdt_ckey;      /* Control code */
   ui16_t tbdt_query;   /* enum n_termcap_query (instead) */
   char tbdt_exp[12];   /* String or [0]=NUL/[1]=BIND_FUN_REDUCE() */
};
n_CTA(n__TERMCAP_QUERY_MAX <= UI16_MAX,
   "Enumeration cannot be stored in datatype");

# ifdef HAVE_KEY_BINDINGS
struct a_tty_bind_parse_ctx{
   char const *tbpc_cmd;      /* Command which parses */
   char const *tbpc_in_seq;   /* In: key sequence */
   struct str tbpc_exp;       /* In/Out: expansion (or NULL) */
   struct a_tty_bind_ctx *tbpc_tbcp;  /* Out: if yet existent */
   struct a_tty_bind_ctx *tbpc_ltbcp; /* Out: the one before .tbpc_tbcp */
   char *tbpc_seq;            /* Out: normalized sequence */
   char *tbpc_cnv;            /* Out: sequence when read(2)ing it */
   ui32_t tbpc_seq_len;
   ui32_t tbpc_cnv_len;
   ui32_t tbpc_cnv_align_mask; /* For creating a_tty_bind_ctx.tbc_cnv */
   ui32_t tbpc_flags;         /* n_lexinput_flags | a_tty_bind_flags */
};

/* Input character tree */
struct a_tty_bind_tree{
   struct a_tty_bind_tree *tbt_sibling; /* s at same level */
   struct a_tty_bind_tree *tbt_childs; /* Sequence continues.. here */
   struct a_tty_bind_tree *tbt_parent;
   struct a_tty_bind_ctx *tbt_bind;    /* NULL for intermediates */
   wchar_t tbt_char;                   /* acter this level represents */
   bool_t tbt_isseq;                   /* Belongs to multibyte sequence */
   bool_t tbt_isseq_trail;             /* ..is trailing byte of it */
   ui8_t tbt__dummy[2];
};
# endif /* HAVE_KEY_BINDINGS */

struct a_tty_cell{
   wchar_t tc_wc;
   ui16_t tc_count;  /* ..of bytes */
   ui8_t tc_width;   /* Visual width; TAB==UI8_MAX! */
   bool_t tc_novis;  /* Don't display visually as such (control character) */
   char tc_cbuf[MB_LEN_MAX * 2]; /* .. plus reset shift sequence */
};

struct a_tty_global{
   struct a_tty_line *tg_line;   /* To be able to access it from signal hdl */
# ifdef HAVE_HISTORY
   struct a_tty_hist *tg_hist;
   struct a_tty_hist *tg_hist_tail;
   size_t tg_hist_size;
   size_t tg_hist_size_max;
# endif
# ifdef HAVE_KEY_BINDINGS
   ui32_t tg_bind_cnt;           /* Overall number of bindings */
   bool_t tg_bind_isdirty;
   bool_t tg_bind_isbuild;
   char tg_bind_shcut_cancel[n__LEXINPUT_CTX_MAX][5];
   char tg_bind_shcut_prompt_char[n__LEXINPUT_CTX_MAX][5];
   struct a_tty_bind_ctx *tg_bind[n__LEXINPUT_CTX_MAX];
   struct a_tty_bind_tree *tg_bind_tree[n__LEXINPUT_CTX_MAX][HSHSIZE];
# endif
   struct termios tg_tios_old;
   struct termios tg_tios_new;
};
n_CTA(n__LEXINPUT_CTX_MAX == 2,
   "Value results in array sizes that results in bad structure layout");

# ifdef HAVE_HISTORY
struct a_tty_hist{
   struct a_tty_hist *th_older;
   struct a_tty_hist *th_younger;
#  ifdef HAVE_BYTE_ORDER_LITTLE
   ui32_t th_isgabby : 1;
#  endif
   ui32_t th_len : 31;
#  ifndef HAVE_BYTE_ORDER_LITTLE
   ui32_t th_isgabby : 1;
#  endif
   char th_dat[VFIELD_SIZE(sizeof(ui32_t))];
};
# endif

struct a_tty_line{
   /* Caller pointers */
   char **tl_x_buf;
   size_t *tl_x_bufsize;
   /* Input processing */
# ifdef HAVE_KEY_BINDINGS
   wchar_t tl_bind_takeover;     /* Leftover byte to consume next */
   ui8_t tl_bind_timeout;        /* In-seq. inter-byte-timer, in 1/10th secs */
   ui8_t tl__bind_dummy[3];
   char (*tl_bind_shcut_cancel)[5];       /* Special _CANCEL shortcut control */
   char (*tl_bind_shcut_prompt_char)[5];  /* ..for _PROMPT_CHAR */
   struct a_tty_bind_tree *(*tl_bind_tree_hmap)[HSHSIZE]; /* Bind lookup tree */
   struct a_tty_bind_tree *tl_bind_tree;
# endif
   char const *tl_reenter_after_cmd; /* `bind' cmd to exec, then re-readline */
   /* Line data / content handling */
   ui32_t tl_count;              /* ..of a_tty_cell's (<= a_TTY_LINE_MAX) */
   ui32_t tl_cursor;             /* Current a_tty_cell insertion point */
   union{
      char *cbuf;                /* *.tl_x_buf */
      struct a_tty_cell *cells;
   } tl_line;
   struct str tl_defc;           /* Current default content */
   size_t tl_defc_cursor_byte;   /* Desired position of cursor after takeover */
   struct str tl_savec;          /* Saved default content */
   struct str tl_pastebuf;       /* Last snarfed data */
# ifdef HAVE_HISTORY
   struct a_tty_hist *tl_hist;   /* History cursor */
# endif
   ui32_t tl_count_max;          /* ..before buffer needs to grow */
   /* Visual data representation handling */
   ui32_t tl_vi_flags;           /* enum a_tty_visual_flags */
   ui32_t tl_lst_count;          /* .tl_count after last sync */
   ui32_t tl_lst_cursor;         /* .tl_cursor after last sync */
   /* TODO Add another indirection layer by adding a tl_phy_line of
    * TODO a_tty_cell objects, incorporate changes in visual layer,
    * TODO then check what _really_ has changed, sync those changes only */
   struct a_tty_cell const *tl_phy_start; /* First visible cell, left border */
   ui32_t tl_phy_cursor;         /* Physical cursor position */
   bool_t tl_quote_rndtrip;      /* For _kht() expansion */
   ui8_t tl__dummy2[3];
   ui32_t tl_prompt_length;      /* Preclassified (TODO needed as a_tty_cell) */
   ui32_t tl_prompt_width;
   char const *tl_prompt;        /* Preformatted prompt (including colours) */
   /* .tl_pos_buf is a hack */
# ifdef HAVE_COLOUR
   char *tl_pos_buf;             /* mle-position colour-on, [4], reset seq. */
   char *tl_pos;                 /* Address of the [4] */
# endif
};

# ifdef HAVE_KEY_BINDINGS
/* C99: use [INDEX]={} */
n_CTAV(n_LEXINPUT_CTX_BASE == 0);
n_CTAV(n_LEXINPUT_CTX_COMPOSE == 1);
static struct a_tty_bind_ctx_map const
      a_tty_bind_ctx_maps[n__LEXINPUT_CTX_MAX] = {
   {n_LEXINPUT_CTX_BASE, "base"},
   {n_LEXINPUT_CTX_COMPOSE, "compose"}
};

/* Special functions which our MLE provides internally.
 * Update the manual upon change! */
static char const a_tty_bind_fun_names[][24] = {
#  undef a_X
#  define a_X(I,N) \
   n_FIELD_INITI(a_TTY_BIND_FUN_REDUCE(a_TTY_BIND_FUN_ ## I)) "mle-" N "\0",

   a_X(BELL, "bell")
   a_X(GO_BWD, "go-bwd") a_X(GO_FWD, "go-fwd")
   a_X(GO_WORD_BWD, "go-word-bwd") a_X(GO_WORD_FWD, "go-word-fwd")
   a_X(GO_HOME, "go-home") a_X(GO_END, "go-end")
   a_X(DEL_BWD, "del-bwd") a_X(DEL_FWD, "del-fwd")
   a_X(SNARF_WORD_BWD, "snarf-word-bwd") a_X(SNARF_WORD_FWD, "snarf-word-fwd")
   a_X(SNARF_END, "snarf-end") a_X(SNARF_LINE, "snarf-line")
   a_X(HIST_BWD, "hist-bwd") a_X(HIST_FWD, "hist-fwd")
   a_X(HIST_SRCH_BWD, "hist-srch-bwd") a_X(HIST_SRCH_FWD, "hist-srch-fwd")
   a_X(REPAINT, "repaint")
   a_X(QUOTE_RNDTRIP, "quote-rndtrip")
   a_X(PROMPT_CHAR, "prompt-char")
   a_X(COMPLETE, "complete")
   a_X(PASTE, "paste")

   a_X(CANCEL, "cancel")
   a_X(RESET, "reset")
   a_X(FULLRESET, "fullreset")
   a_X(COMMIT, "commit")

#  undef a_X
};
# endif /* HAVE_KEY_BINDINGS */

/* The default key bindings (unless disallowed).  Update manual upon change!
 * A logical subset of this table is also used if !HAVE_KEY_BINDINGS (more
 * expensive than a switch() on control codes directly, but less redundant) */
static struct a_tty_bind_default_tuple const a_tty_bind_default_tuples[] = {
# undef a_X
# define a_X(K,S) \
   {TRU1, K, 0, {'\0', (char)a_TTY_BIND_FUN_REDUCE(a_TTY_BIND_FUN_ ## S),}},

   a_X('A', GO_HOME)
   a_X('B', GO_BWD)
   /* C: SIGINT */
   a_X('D', DEL_FWD)
   a_X('E', GO_END)
   a_X('F', GO_FWD)
   a_X('G', RESET)
   a_X('H', DEL_BWD)
   a_X('I', COMPLETE)
   a_X('J', COMMIT)
   a_X('K', SNARF_END)
   a_X('L', REPAINT)
   /* M: same as J */
   a_X('N', HIST_FWD)
   /* O: below */
   a_X('P', HIST_BWD)
   a_X('Q', QUOTE_RNDTRIP)
   a_X('R', HIST_SRCH_BWD)
   a_X('S', HIST_SRCH_FWD)
   a_X('T', PASTE)
   a_X('U', SNARF_LINE)
   a_X('V', PROMPT_CHAR)
   a_X('W', SNARF_WORD_BWD)
   a_X('X', GO_WORD_FWD)
   a_X('Y', GO_WORD_BWD)
   /* Z: SIGTSTP */

   a_X('[', CANCEL)
   /* \: below */
   /* ]: below */
   /* ^: below */
   a_X('_', SNARF_WORD_FWD)

   a_X('?', DEL_BWD)

#  undef a_X
#  define a_X(K,S) {TRU1, K, 0, {S}},

   a_X('O', "dt")
   a_X('\\', "z+")
   a_X(']', "z$")
   a_X('^', "z0")

# ifdef HAVE_KEY_BINDINGS
#  undef a_X
#  define a_X(Q,S) \
   {FAL0, '\0', n_TERMCAP_QUERY_ ## Q,\
      {'\0', (char)a_TTY_BIND_FUN_REDUCE(a_TTY_BIND_FUN_ ## S),}},

   a_X(key_backspace, DEL_BWD) a_X(key_dc, DEL_FWD)
   a_X(key_eol, SNARF_END)
   a_X(key_home, GO_HOME) a_X(key_end, GO_END)
   a_X(key_left, GO_BWD) a_X(key_right, GO_FWD)
   a_X(key_sleft, GO_HOME) a_X(key_sright, GO_END)
   a_X(key_up, HIST_BWD) a_X(key_down, HIST_FWD)

#  undef a_X
#  define a_X(Q,S) {FAL0, '\0', n_TERMCAP_QUERY_ ## Q, {S}},

   a_X(key_shome, "z0") a_X(key_send, "z$")
   a_X(xkey_sup, "z0") a_X(xkey_sdown, "z$")
   a_X(key_ppage, "z-") a_X(key_npage, "z+")
   a_X(xkey_cup, "dotmove-") a_X(xkey_cdown, "dotmove+")

# endif /* HAVE_KEY_BINDINGS */
# undef a_X
};

static struct a_tty_global a_tty;

/* Change from canonical to raw, non-canonical mode, and way back */
static void a_tty_term_mode(bool_t raw);

/* Adjust an active raw mode to use / not use a timeout */
# ifdef HAVE_KEY_BINDINGS
static void a_tty_term_rawmode_timeout(struct a_tty_line *tlp, bool_t enable);
# endif

/* 0-X (2), UI8_MAX == \t / TAB */
static ui8_t a_tty_wcwidth(wchar_t wc);

/* Memory / cell / word generics */
static void a_tty_check_grow(struct a_tty_line *tlp, ui32_t no
               SMALLOC_DEBUG_ARGS);
static ssize_t a_tty_cell2dat(struct a_tty_line *tlp);
static void a_tty_cell2save(struct a_tty_line *tlp);

/* Save away data bytes of given range (max = non-inclusive) */
static void a_tty_copy2paste(struct a_tty_line *tlp, struct a_tty_cell *tcpmin,
               struct a_tty_cell *tcpmax);

/* Ask user for hexadecimal number, interpret as UTF-32 */
static wchar_t a_tty_vinuni(struct a_tty_line *tlp);

/* Visual screen synchronization */
static bool_t a_tty_vi_refresh(struct a_tty_line *tlp);

static bool_t a_tty_vi__paint(struct a_tty_line *tlp);

/* Search for word boundary, starting at tl_cursor, in "dir"ection (<> 0).
 * Return <0 when moving is impossible (backward direction but in position 0,
 * forward direction but in outermost column), and relative distance to
 * tl_cursor otherwise */
static si32_t a_tty_wboundary(struct a_tty_line *tlp, si32_t dir);

/* Most function implementations */
static void a_tty_khome(struct a_tty_line *tlp, bool_t dobell);
static void a_tty_kend(struct a_tty_line *tlp);
static void a_tty_kbs(struct a_tty_line *tlp);
static void a_tty_ksnarf(struct a_tty_line *tlp, bool_t cplline, bool_t dobell);
static si32_t a_tty_kdel(struct a_tty_line *tlp);
static void a_tty_kleft(struct a_tty_line *tlp);
static void a_tty_kright(struct a_tty_line *tlp);
static void a_tty_ksnarfw(struct a_tty_line *tlp, bool_t fwd);
static void a_tty_kgow(struct a_tty_line *tlp, si32_t dir);
static bool_t a_tty_kother(struct a_tty_line *tlp, wchar_t wc);
static ui32_t a_tty_kht(struct a_tty_line *tlp);

# ifdef HAVE_HISTORY
/* Return UI32_MAX on "exhaustion" */
static ui32_t a_tty_khist(struct a_tty_line *tlp, bool_t fwd);
static ui32_t a_tty_khist_search(struct a_tty_line *tlp, bool_t fwd);

static ui32_t a_tty__khist_shared(struct a_tty_line *tlp,
                  struct a_tty_hist *thp);
# endif

/* Handle a function */
static enum a_tty_fun_status a_tty_fun(struct a_tty_line *tlp,
                              enum a_tty_bind_flags tbf, size_t *len);

/* Readline core */
static ssize_t a_tty_readline(struct a_tty_line *tlp, size_t len
                  SMALLOC_DEBUG_ARGS);

# ifdef HAVE_KEY_BINDINGS
/* Find context or -1 */
static enum n_lexinput_flags a_tty_bind_ctx_find(char const *name);

/* Create (or replace, if allowed) a binding */
static bool_t a_tty_bind_create(struct a_tty_bind_parse_ctx *tbpcp,
               bool_t replace);

/* Shared implementation to parse `bind' and `unbind' "key-sequence" and
 * "expansion" command line arguments into something that we can work with */
static bool_t a_tty_bind_parse(bool_t isbindcmd,
               struct a_tty_bind_parse_ctx *tbpcp);

/* Lazy resolve a termcap(5)/terminfo(5) (or *termcap*!) capability */
static void a_tty_bind_resolve(struct a_tty_bind_ctx *tbcp);

/* Delete an existing binding */
static void a_tty_bind_del(struct a_tty_bind_parse_ctx *tbpcp);

/* Life cycle of all input node trees */
static void a_tty_bind_tree_build(void);
static void a_tty_bind_tree_teardown(void);

static void a_tty__bind_tree_add(ui32_t hmap_idx,
               struct a_tty_bind_tree *store[HSHSIZE],
               struct a_tty_bind_ctx *tbcp);
static struct a_tty_bind_tree *a_tty__bind_tree_add_wc(
               struct a_tty_bind_tree **treep, struct a_tty_bind_tree *parentp,
               wchar_t wc, bool_t isseq);
static void a_tty__bind_tree_free(struct a_tty_bind_tree *tbtp);
# endif /* HAVE_KEY_BINDINGS */

static void
a_tty_term_mode(bool_t raw){
   struct termios *tiosp;
   NYD2_ENTER;

   tiosp = &a_tty.tg_tios_old;
   if(!raw)
      goto jleave;

   /* Always requery the attributes, in case we've been moved from background
    * to foreground or however else in between sessions */
   /* XXX Always enforce ECHO and ICANON in the OLD attributes - do so as long
    * XXX as we don't properly deal with TTIN and TTOU etc. */
   tcgetattr(STDIN_FILENO, tiosp);
   tiosp->c_lflag |= ECHO | ICANON;

   memcpy(&a_tty.tg_tios_new, tiosp, sizeof *tiosp);
   tiosp = &a_tty.tg_tios_new;
   tiosp->c_cc[VMIN] = 1;
   tiosp->c_cc[VTIME] = 0;
   /* Enable ^\, ^Q and ^S to be used for key bindings */
   tiosp->c_cc[VQUIT] = tiosp->c_cc[VSTART] = tiosp->c_cc[VSTOP] = '\0';
   tiosp->c_iflag &= ~(ISTRIP | IGNCR);
   tiosp->c_lflag &= ~(ECHO /*| ECHOE | ECHONL */| ICANON | IEXTEN);
jleave:
   tcsetattr(STDIN_FILENO, TCSADRAIN, tiosp);
   NYD2_LEAVE;
}

# ifdef HAVE_KEY_BINDINGS
static void
a_tty_term_rawmode_timeout(struct a_tty_line *tlp, bool_t enable){
   NYD2_ENTER;
   if(enable){
      ui8_t bt;

      a_tty.tg_tios_new.c_cc[VMIN] = 0;
      if((bt = tlp->tl_bind_timeout) == 0)
         bt = a_TTY_BIND_TIMEOUT;
      a_tty.tg_tios_new.c_cc[VTIME] = bt;
   }else{
      a_tty.tg_tios_new.c_cc[VMIN] = 1;
      a_tty.tg_tios_new.c_cc[VTIME] = 0;
   }
   tcsetattr(STDIN_FILENO, TCSANOW, &a_tty.tg_tios_new);
   NYD2_LEAVE;
}
# endif /* HAVE_KEY_BINDINGS */

static ui8_t
a_tty_wcwidth(wchar_t wc){
   ui8_t rv;
   NYD2_ENTER;

   /* Special case the backslash at first */
   if(wc == '\t')
      rv = UI8_MAX;
   else{
      int i;

# ifdef HAVE_WCWIDTH
      rv = ((i = wcwidth(wc)) > 0) ? (ui8_t)i : 0;
# else
      rv = iswprint(wc) ? 1 + (wc >= 0x1100u) : 0; /* TODO use S-CText */
# endif
   }
   NYD2_LEAVE;
   return rv;
}

static void
a_tty_check_grow(struct a_tty_line *tlp, ui32_t no SMALLOC_DEBUG_ARGS){
   ui32_t cmax;
   NYD2_ENTER;

   if(UNLIKELY((cmax = tlp->tl_count + no) > tlp->tl_count_max)){
      size_t i;

      i = cmax * sizeof(struct a_tty_cell) + 2 * sizeof(struct a_tty_cell);
      if(LIKELY(i >= *tlp->tl_x_bufsize)){
         hold_all_sigs(); /* XXX v15 drop */
         i <<= 1;
         tlp->tl_line.cbuf =
         *tlp->tl_x_buf = (srealloc)(*tlp->tl_x_buf, i SMALLOC_DEBUG_ARGSCALL);
         rele_all_sigs(); /* XXX v15 drop */
      }
      tlp->tl_count_max = cmax;
      *tlp->tl_x_bufsize = i;
   }
   NYD2_LEAVE;
}

static ssize_t
a_tty_cell2dat(struct a_tty_line *tlp){
   size_t len, i;
   NYD2_ENTER;

   len = 0;

   if(LIKELY((i = tlp->tl_count) > 0)){
      struct a_tty_cell const *tcap;

      tcap = tlp->tl_line.cells;
      do{
         memcpy(tlp->tl_line.cbuf + len, tcap->tc_cbuf, tcap->tc_count);
         len += tcap->tc_count;
      }while(++tcap, --i > 0);
   }

   tlp->tl_line.cbuf[len] = '\0';
   NYD2_LEAVE;
   return (ssize_t)len;
}

static void
a_tty_cell2save(struct a_tty_line *tlp){
   size_t len, i;
   struct a_tty_cell *tcap;
   NYD2_ENTER;

   tlp->tl_savec.s = NULL;
   tlp->tl_savec.l = 0;

   if(UNLIKELY(tlp->tl_count == 0))
      goto jleave;

   for(tcap = tlp->tl_line.cells, len = 0, i = tlp->tl_count; i > 0;
         ++tcap, --i)
      len += tcap->tc_count;

   tlp->tl_savec.s = salloc((tlp->tl_savec.l = len) +1);

   for(tcap = tlp->tl_line.cells, len = 0, i = tlp->tl_count; i > 0;
         ++tcap, --i){
      memcpy(tlp->tl_savec.s + len, tcap->tc_cbuf, tcap->tc_count);
      len += tcap->tc_count;
   }
   tlp->tl_savec.s[len] = '\0';
jleave:
   NYD2_LEAVE;
}

static void
a_tty_copy2paste(struct a_tty_line *tlp, struct a_tty_cell *tcpmin,
      struct a_tty_cell *tcpmax){
   char *cp;
   struct a_tty_cell *tcp;
   size_t l;
   NYD2_ENTER;

   l = 0;
   for(tcp = tcpmin; tcp < tcpmax; ++tcp)
      l += tcp->tc_count;

   tlp->tl_pastebuf.s = cp = salloc((tlp->tl_pastebuf.l = l) +1);

   l = 0;
   for(tcp = tcpmin; tcp < tcpmax; cp += l, ++tcp)
      memcpy(cp, tcp->tc_cbuf, l = tcp->tc_count);
   *cp = '\0';
   NYD2_LEAVE;
}

static wchar_t
a_tty_vinuni(struct a_tty_line *tlp){
   char buf[16], *eptr;
   union {size_t i; long l;} u;
   wchar_t wc;
   NYD2_ENTER;

   wc = '\0';

   if(!n_termcap_cmdx(n_TERMCAP_CMD_cr) ||
         !n_termcap_cmd(n_TERMCAP_CMD_ce, 0, -1))
      goto jleave;

   /* C99 */{
      struct str const *cpre, *csuf;
#ifdef HAVE_COLOUR
      struct n_colour_pen *cpen;

      cpen = n_colour_pen_create(n_COLOUR_ID_MLE_PROMPT, NULL);
      if((cpre = n_colour_pen_to_str(cpen)) != NULL)
         csuf = n_colour_reset_to_str();
      else
         csuf = NULL;
#else
      cpre = csuf = NULL;
#endif
      printf(_("%sPlease enter Unicode code point:%s "),
         (cpre != NULL ? cpre->s : ""), (csuf != NULL ? csuf->s : ""));
   }
   fflush(stdout);

   buf[sizeof(buf) -1] = '\0';
   for(u.i = 0;;){
      if(read(STDIN_FILENO, &buf[u.i], 1) != 1){
         if(errno == EINTR) /* xxx #if !SA_RESTART ? */
            continue;
         goto jleave;
      }
      if(buf[u.i] == '\n')
         break;
      if(!hexchar(buf[u.i])){
         char const emsg[] = "[0-9a-fA-F]";

         LCTA(sizeof emsg <= sizeof(buf));
         memcpy(buf, emsg, sizeof emsg);
         goto jerr;
      }

      putc(buf[u.i], stdout);
      fflush(stdout);
      if(++u.i == sizeof buf)
         goto jerr;
   }
   buf[u.i] = '\0';

   u.l = strtol(buf, &eptr, 16);
   if(u.l <= 0 || u.l >= 0x10FFFF/* XXX magic; CText */ || *eptr != '\0'){
jerr:
      n_err(_("\nInvalid input: %s\n"), buf);
      goto jleave;
   }

   wc = (wchar_t)u.l;
jleave:
   tlp->tl_vi_flags |= a_TTY_VF_MOD_DIRTY | (wc == '\0' ? a_TTY_VF_BELL : 0);
   NYD2_LEAVE;
   return wc;
}

static bool_t
a_tty_vi_refresh(struct a_tty_line *tlp){
   bool_t rv;
   NYD2_ENTER;

   if(tlp->tl_vi_flags & a_TTY_VF_BELL){
      tlp->tl_vi_flags |= a_TTY_VF_SYNC;
      if(putchar('\a') == EOF)
         goto jerr;
   }

   if(tlp->tl_vi_flags & a_TTY_VF_REFRESH){
      /* kht may want to restore a cursor position after inserting some
       * data somewhere */
      if(tlp->tl_defc_cursor_byte > 0){
         size_t i, j;
         ssize_t k;

         a_tty_khome(tlp, FAL0);

         i = tlp->tl_defc_cursor_byte;
         tlp->tl_defc_cursor_byte = 0;
         for(j = 0; tlp->tl_cursor < tlp->tl_count; ++j){
            a_tty_kright(tlp);
            if((k = tlp->tl_line.cells[j].tc_count) > i)
               break;
            i -= k;
         }
      }

      if(!a_tty_vi__paint(tlp))
         goto jerr;
   }

   if(tlp->tl_vi_flags & a_TTY_VF_SYNC){
      tlp->tl_vi_flags &= ~a_TTY_VF_SYNC;
      if(fflush(stdout))
         goto jerr;
   }

   rv = TRU1;
jleave:
   tlp->tl_vi_flags &= ~a_TTY_VF_ALL_MASK;
   NYD2_LEAVE;
   return rv;

jerr:
   clearerr(stdout); /* xxx I/O layer rewrite */
   n_err(_("Visual refresh failed!  Is $TERM set correctly?\n"
      "  Setting *line-editor-disable* to get us through!\n"));
   ok_bset(line_editor_disable, TRU1);
   rv = FAL0;
   goto jleave;
}

static bool_t
a_tty_vi__paint(struct a_tty_line *tlp){
   enum{
      a_TRUE_RV = a_TTY__VF_LAST<<1,         /* Return value bit */
      a_HAVE_PROMPT = a_TTY__VF_LAST<<2,     /* Have a prompt */
      a_SHOW_PROMPT = a_TTY__VF_LAST<<3,     /* Shall print the prompt */
      a_MOVE_CURSOR = a_TTY__VF_LAST<<4,     /* Move visual cursor for user! */
      a_LEFT_MIN = a_TTY__VF_LAST<<5,        /* On left boundary */
      a_RIGHT_MAX = a_TTY__VF_LAST<<6,
      a_HAVE_POSITION = a_TTY__VF_LAST<<7,   /* Print the position indicator */

      /* We carry some flags over invocations (not worth a specific field) */
      a_VISIBLE_PROMPT = a_TTY__VF_LAST<<8,  /* The prompt is on the screen */
      a_PERSIST_MASK = a_VISIBLE_PROMPT,
      a__LAST = a_PERSIST_MASK
   };

   ui32_t f, w, phy_wid_base, phy_wid, phy_base, phy_cur, cnt, lstcur, cur,
      vi_left, vi_right, phy_nxtcur;
   struct a_tty_cell const *tccp, *tcp_left, *tcp_right, *tcxp;
   NYD2_ENTER;
   n_LCTA(UICMP(64, a__LAST, <, UI32_MAX), "Flag bits excess storage datatype");

   f = tlp->tl_vi_flags;
   tlp->tl_vi_flags = (f & ~(a_TTY_VF_REFRESH | a_PERSIST_MASK)) |
         a_TTY_VF_SYNC;
   f |= a_TRUE_RV;
   if((w = tlp->tl_prompt_length) > 0)
      f |= a_HAVE_PROMPT;
   f |= a_HAVE_POSITION;

   /* XXX We don't have a OnTerminalResize event (see main.c) yet, so we need
    * XXX to reevaluate our circumstances over and over again */
   /* Don't display prompt or position indicator on very small screens */
   if((phy_wid_base = (ui32_t)scrnwidth) <= a_TTY_WIDTH_RIPOFF)
      f &= ~(a_HAVE_PROMPT | a_HAVE_POSITION);
   else{
      phy_wid_base -= a_TTY_WIDTH_RIPOFF;

      /* Disable the prompt if the screen is too small; due to lack of some
       * indicator simply add a second ripoff */
      if((f & a_HAVE_PROMPT) && w + a_TTY_WIDTH_RIPOFF >= phy_wid_base)
         f &= ~a_HAVE_PROMPT;
   }

   phy_wid = phy_wid_base;
   phy_base = 0;
   phy_cur = tlp->tl_phy_cursor;
   cnt = tlp->tl_count;
   lstcur = tlp->tl_lst_cursor;

   /* XXX Assume dirty screen if shrunk */
   if(cnt < tlp->tl_lst_count)
      f |= a_TTY_VF_MOD_DIRTY;

   /* TODO Without HAVE_TERMCAP, it would likely be much cheaper to simply
    * TODO always "cr + paint + ce + ch", since ce is simulated via spaces.. */

   /* Quickshot: if the line is empty, possibly print prompt and out */
   if(cnt == 0){
      /* In that special case dirty anything if it seems better */
      if((f & a_TTY_VF_MOD_CONTENT) || tlp->tl_lst_count > 0)
         f |= a_TTY_VF_MOD_DIRTY;

      if((f & a_TTY_VF_MOD_DIRTY) && phy_cur != 0){
         if(!n_termcap_cmdx(n_TERMCAP_CMD_cr))
            goto jerr;
         phy_cur = 0;
      }

      if((f & (a_TTY_VF_MOD_DIRTY | a_HAVE_PROMPT)) ==
            (a_TTY_VF_MOD_DIRTY | a_HAVE_PROMPT)){
         if(fputs(tlp->tl_prompt, stdout) == EOF)
            goto jerr;
         phy_cur = tlp->tl_prompt_width + 1;
      }

      /* May need to clear former line content */
      if((f & a_TTY_VF_MOD_DIRTY) &&
            !n_termcap_cmd(n_TERMCAP_CMD_ce, phy_cur, -1))
         goto jerr;

      tlp->tl_phy_start = tlp->tl_line.cells;
      goto jleave;
   }

   /* Try to get an idea of the visual window */

   /* Find the left visual boundary */
   phy_wid = (phy_wid >> 1) + (phy_wid >> 2);
   if((cur = tlp->tl_cursor) == cnt)
      --cur;

   w = (tcp_left = tccp = tlp->tl_line.cells + cur)->tc_width;
   if(w == UI8_MAX) /* TODO yet TAB == SPC */
      w = 1;
   while(tcp_left > tlp->tl_line.cells){
      ui16_t cw = tcp_left[-1].tc_width;

      if(cw == UI8_MAX) /* TODO yet TAB == SPC */
         cw = 1;
      if(w + cw >= phy_wid)
         break;
      w += cw;
      --tcp_left;
   }
   vi_left = w;

   /* If the left hand side of our visual viewpoint consumes less than half
    * of the screen width, show the prompt */
   if(tcp_left == tlp->tl_line.cells)
      f |= a_LEFT_MIN;

   if((f & (a_LEFT_MIN | a_HAVE_PROMPT)) == (a_LEFT_MIN | a_HAVE_PROMPT) &&
         w + tlp->tl_prompt_width < phy_wid){
      phy_base = tlp->tl_prompt_width;
      f |= a_SHOW_PROMPT;
   }

   /* Then search for right boundary.  We always leave the rightmost column
    * empty because some terminals [cw]ould wrap the line if we write into
    * that.  XXX terminfo(5)/termcap(5) have the semi_auto_right_margin/sam/YE
    * XXX capability to indicate this, but we don't look at that */
   phy_wid = phy_wid_base - phy_base;
   tcp_right = tlp->tl_line.cells + cnt;

   while(&tccp[1] < tcp_right){
      ui16_t cw = tccp[1].tc_width;
      ui32_t i;

      if(cw == UI8_MAX) /* TODO yet TAB == SPC */
         cw = 1;
      i = w + cw;
      if(i > phy_wid)
         break;
      w = i;
      ++tccp;
   }
   vi_right = w - vi_left;

   /* If the complete line including prompt fits on the screen, show prompt */
   if(--tcp_right == tccp){
      f |= a_RIGHT_MAX;

      /* Since we did brute-force walk also for the left boundary we may end up
       * in a situation were anything effectively fits on the screen, including
       * the prompt that is, but were we don't recognize this since we
       * restricted the search to fit in some visual viewpoint.  Therefore try
       * again to extend the left boundary to overcome that */
      if(!(f & a_LEFT_MIN)){
         struct a_tty_cell const *tc1p = tlp->tl_line.cells;
         ui32_t vil1 = vi_left;

         assert(!(f & a_SHOW_PROMPT));
         w += tlp->tl_prompt_width;
         for(tcxp = tcp_left;;){
            ui32_t i = tcxp[-1].tc_width;

            if(i == UI8_MAX) /* TODO yet TAB == SPC */
               i = 1;
            vil1 += i;
            i += w;
            if(i > phy_wid)
               break;
            w = i;
            if(--tcxp == tc1p){
               tcp_left = tc1p;
               vi_left = vil1;
               f |= a_LEFT_MIN;
               break;
            }
         }
         /*w -= tlp->tl_prompt_width;*/
      }
   }
   tcp_right = tccp;
   tccp = tlp->tl_line.cells + cur;

   if((f & (a_LEFT_MIN | a_RIGHT_MAX | a_HAVE_PROMPT | a_SHOW_PROMPT)) ==
            (a_LEFT_MIN | a_RIGHT_MAX | a_HAVE_PROMPT) &&
         w + tlp->tl_prompt_width <= phy_wid){
      phy_wid -= (phy_base = tlp->tl_prompt_width);
      f |= a_SHOW_PROMPT;
   }

   /* Try to avoid repainting the complete line - this is possible if the
    * cursor "did not leave the screen" and the prompt status hasn't changed.
    * I.e., after clamping virtual viewpoint, compare relation to physical */
   if((f & (a_TTY_VF_MOD_SINGLE/*FIXME*/ |
            a_TTY_VF_MOD_CONTENT/* xxx */ | a_TTY_VF_MOD_DIRTY)) ||
         (tcxp = tlp->tl_phy_start) == NULL ||
         tcxp > tccp || tcxp <= tcp_right)
         f |= a_TTY_VF_MOD_DIRTY;
   else{
         f |= a_TTY_VF_MOD_DIRTY;
#if 0
      struct a_tty_cell const *tcyp;
      si32_t cur_displace;
      ui32_t phy_lmargin, phy_rmargin, fx, phy_displace;

      phy_lmargin = (fx = phy_wid) / 100;
      phy_rmargin = fx - (phy_lmargin * a_TTY_SCROLL_MARGIN_RIGHT);
      phy_lmargin *= a_TTY_SCROLL_MARGIN_LEFT;
      fx = (f & (a_SHOW_PROMPT | a_VISIBLE_PROMPT));

      if(fx == 0 || fx == (a_SHOW_PROMPT | a_VISIBLE_PROMPT)){
      }
#endif
   }
   goto jpaint;

   /* We know what we have to paint, start synchronizing */
jpaint:
   assert(phy_cur == tlp->tl_phy_cursor);
   assert(phy_wid == phy_wid_base - phy_base);
   assert(cnt == tlp->tl_count);
   assert(cnt > 0);
   assert(lstcur == tlp->tl_lst_cursor);
   assert(tccp == tlp->tl_line.cells + cur);

   phy_nxtcur = phy_base; /* FIXME only if repaint cpl. */

   /* Quickshot: is it only cursor movement within the visible screen? */
   if((f & a_TTY_VF_REFRESH) == a_TTY_VF_MOD_CURSOR){
      f |= a_MOVE_CURSOR;
      goto jcursor;
   }

   /* To be able to apply some quick jump offs, clear line if possible */
   if(f & a_TTY_VF_MOD_DIRTY){
      /* Force complete clearance and cursor reinitialization */
      if(!n_termcap_cmdx(n_TERMCAP_CMD_cr) ||
            !n_termcap_cmd(n_TERMCAP_CMD_ce, 0, -1))
         goto jerr;
      tlp->tl_phy_start = tcp_left;
      phy_cur = 0;
   }

   if((f & (a_TTY_VF_MOD_DIRTY | a_SHOW_PROMPT)) && phy_cur != 0){
      if(!n_termcap_cmdx(n_TERMCAP_CMD_cr))
         goto jerr;
      phy_cur = 0;
   }

   if(f & a_SHOW_PROMPT){
      assert(phy_base == tlp->tl_prompt_width);
      if(fputs(tlp->tl_prompt, stdout) == EOF)
         goto jerr;
      phy_cur = phy_nxtcur;
      f |= a_VISIBLE_PROMPT;
   }else
      f &= ~a_VISIBLE_PROMPT;

/* FIXME reposition cursor for paint */
   for(w = phy_nxtcur; tcp_left <= tcp_right; ++tcp_left){
      ui16_t cw;

      cw = tcp_left->tc_width;

      if(LIKELY(!tcp_left->tc_novis)){
         if(fwrite(tcp_left->tc_cbuf, sizeof *tcp_left->tc_cbuf,
               tcp_left->tc_count, stdout) != tcp_left->tc_count)
            goto jerr;
      }else{ /* XXX Shouldn't be here <-> CText, ui_str.c */
         char wbuf[8]; /* XXX magic */

         if(options & OPT_UNICODE){
            ui32_t wc;

            wc = (ui32_t)tcp_left->tc_wc;
            if((wc & ~0x1Fu) == 0)
               wc |= 0x2400;
            else if(wc == 0x7F)
               wc = 0x2421;
            else
               wc = 0x2426;
            n_utf32_to_utf8(wc, wbuf);
         }else
            wbuf[0] = '?', wbuf[1] = '\0';

         if(fputs(wbuf, stdout) == EOF)
            goto jerr;
         cw = 1;
      }

      if(cw == UI8_MAX) /* TODO yet TAB == SPC */
         cw = 1;
      w += cw;
      if(tcp_left == tccp)
         phy_nxtcur = w;
      phy_cur += cw;
   }

   /* Write something position marker alike if it doesn't fit on screen */
   if((f & a_HAVE_POSITION) &&
         ((f & (a_LEFT_MIN | a_RIGHT_MAX)) != (a_LEFT_MIN | a_RIGHT_MAX) ||
          ((f & a_HAVE_PROMPT) && !(f & a_SHOW_PROMPT)))){
# ifdef HAVE_COLOUR
      char *posbuf = tlp->tl_pos_buf, *pos = tlp->tl_pos;
# else
      char posbuf[5], *pos = posbuf;

      pos[4] = '\0';
# endif

      if(phy_cur != (w = phy_wid_base) &&
            !n_termcap_cmd(n_TERMCAP_CMD_ch, phy_cur = w, 0))
         goto jerr;

      *pos++ = '|';
      if((f & a_LEFT_MIN) && (!(f & a_HAVE_PROMPT) || (f & a_SHOW_PROMPT)))
         memcpy(pos, "^.+", 3);
      else if(f & a_RIGHT_MAX)
         memcpy(pos, ".+$", 3);
      else{
         /* Theoretical line length limit a_TTY_LINE_MAX, choose next power of
          * ten (10 ** 10) to represent 100 percent, since we don't have a macro
          * that generates a constant, and i don't trust the standard "u type
          * suffix automatically scales" calculate the large number */
         static char const itoa[] = "0123456789";

         ui64_t const fact100 = (ui64_t)0x3B9ACA00u * 10u, fact = fact100 / 100;
         ui32_t i = (ui32_t)(((fact100 / cnt) * tlp->tl_cursor) / fact);
         n_LCTA(a_TTY_LINE_MAX <= SI32_MAX, "a_TTY_LINE_MAX too large");

         if(i < 10)
            pos[0] = ' ', pos[1] = itoa[i];
         else
            pos[1] = itoa[i % 10], pos[0] = itoa[i / 10];
         pos[2] = '%';
      }

      if(fputs(posbuf, stdout) == EOF)
         goto jerr;
      phy_cur += 4;
   }

   /* Users are used to see the cursor right of the point of interest, so we
    * need some further adjustments unless in special conditions.  Be aware
    * that we may have adjusted cur at the beginning, too */
   if((cur = tlp->tl_cursor) == 0)
      phy_nxtcur = phy_base;
   else if(cur != cnt){
      ui16_t cw = tccp->tc_width;

      if(cw == UI8_MAX) /* TODO yet TAB == SPC */
         cw = 1;
      phy_nxtcur -= cw;
   }

jcursor:
   if(((f & a_MOVE_CURSOR) || phy_nxtcur != phy_cur) &&
         !n_termcap_cmd(n_TERMCAP_CMD_ch, phy_cur = phy_nxtcur, 0))
      goto jerr;

jleave:
   tlp->tl_vi_flags |= (f & a_PERSIST_MASK);
   tlp->tl_lst_count = tlp->tl_count;
   tlp->tl_lst_cursor = tlp->tl_cursor;
   tlp->tl_phy_cursor = phy_cur;

   NYD2_LEAVE;
   return ((f & a_TRUE_RV) != 0);
jerr:
   f &= ~a_TRUE_RV;
   goto jleave;
}

static si32_t
a_tty_wboundary(struct a_tty_line *tlp, si32_t dir){/* TODO shell token-wise */
   bool_t anynon;
   struct a_tty_cell *tcap;
   ui32_t cur, cnt;
   si32_t rv;
   NYD2_ENTER;

   assert(dir == 1 || dir == -1);

   rv = -1;
   cnt = tlp->tl_count;
   cur = tlp->tl_cursor;

   if(dir < 0){
      if(cur == 0)
         goto jleave;
   }else if(cur + 1 >= cnt)
      goto jleave;
   else
      --cnt, --cur; /* xxx Unsigned wrapping may occur (twice), then */

   for(rv = 0, tcap = tlp->tl_line.cells, anynon = FAL0;;){
      wchar_t wc;

      wc = tcap[cur += (ui32_t)dir].tc_wc;
      if(iswblank(wc) || iswpunct(wc)){
         if(anynon)
            break;
      }else
         anynon = TRU1;

      ++rv;

      if(dir < 0){
         if(cur == 0)
            break;
      }else if(cur + 1 >= cnt){
         ++rv;
         break;
      }
   }
jleave:
   NYD2_LEAVE;
   return rv;
}

static void
a_tty_khome(struct a_tty_line *tlp, bool_t dobell){
   ui32_t f;
   NYD2_ENTER;

   if(LIKELY(tlp->tl_cursor > 0)){
      tlp->tl_cursor = 0;
      f = a_TTY_VF_MOD_CURSOR;
   }else if(dobell)
      f = a_TTY_VF_BELL;
   else
      f = a_TTY_VF_NONE;

   tlp->tl_vi_flags |= f;
   NYD2_LEAVE;
}

static void
a_tty_kend(struct a_tty_line *tlp){
   ui32_t f;
   NYD2_ENTER;

   if(LIKELY(tlp->tl_cursor < tlp->tl_count)){
      tlp->tl_cursor = tlp->tl_count;
      f = a_TTY_VF_MOD_CURSOR;
   }else
      f = a_TTY_VF_BELL;

   tlp->tl_vi_flags |= f;
   NYD2_LEAVE;
}

static void
a_tty_kbs(struct a_tty_line *tlp){
   ui32_t f, cur, cnt;
   NYD2_ENTER;

   cur = tlp->tl_cursor;
   cnt = tlp->tl_count;

   if(LIKELY(cur > 0)){
      tlp->tl_cursor = --cur;
      tlp->tl_count = --cnt;

      if((cnt -= cur) > 0){
         struct a_tty_cell *tcap;

         tcap = tlp->tl_line.cells + cur;
         memmove(tcap, &tcap[1], cnt *= sizeof(*tcap));
      }
      f = a_TTY_VF_MOD_CURSOR | a_TTY_VF_MOD_CONTENT;
   }else
      f = a_TTY_VF_BELL;

   tlp->tl_vi_flags |= f;
   NYD2_LEAVE;
}

static void
a_tty_ksnarf(struct a_tty_line *tlp, bool_t cplline, bool_t dobell){
   ui32_t i, f;
   NYD2_ENTER;

   f = a_TTY_VF_NONE;
   i = tlp->tl_cursor;

   if(cplline && i > 0){
      tlp->tl_cursor = i = 0;
      f = a_TTY_VF_MOD_CURSOR;
   }

   if(LIKELY(i < tlp->tl_count)){
      struct a_tty_cell *tcap;

      tcap = &tlp->tl_line.cells[0];
      a_tty_copy2paste(tlp, &tcap[i], &tcap[tlp->tl_count]);
      tlp->tl_count = i;
      f = a_TTY_VF_MOD_CONTENT;
   }else if(dobell)
      f |= a_TTY_VF_BELL;

   tlp->tl_vi_flags |= f;
   NYD2_LEAVE;
}

static si32_t
a_tty_kdel(struct a_tty_line *tlp){
   ui32_t cur, cnt, f;
   si32_t i;
   NYD2_ENTER;

   cur = tlp->tl_cursor;
   cnt = tlp->tl_count;
   i = (si32_t)(cnt - cur);

   if(LIKELY(i > 0)){
      tlp->tl_count = --cnt;

      if(LIKELY(--i > 0)){
         struct a_tty_cell *tcap;

         tcap = &tlp->tl_line.cells[cur];
         memmove(tcap, &tcap[1], (ui32_t)i * sizeof(*tcap));
      }
      f = a_TTY_VF_MOD_CONTENT;
   }else if(cnt == 0 && !ok_blook(ignoreeof)){
      putchar('^');
      putchar('D');
      i = -1;
      f = a_TTY_VF_NONE;
   }else{
      i = 0;
      f = a_TTY_VF_BELL;
   }

   tlp->tl_vi_flags |= f;
   NYD2_LEAVE;
   return i;
}

static void
a_tty_kleft(struct a_tty_line *tlp){
   ui32_t f;
   NYD2_ENTER;

   if(LIKELY(tlp->tl_cursor > 0)){
      --tlp->tl_cursor;
      f = a_TTY_VF_MOD_CURSOR;
   }else
      f = a_TTY_VF_BELL;

   tlp->tl_vi_flags |= f;
   NYD2_LEAVE;
}

static void
a_tty_kright(struct a_tty_line *tlp){
   ui32_t i;
   NYD2_ENTER;

   if(LIKELY((i = tlp->tl_cursor + 1) <= tlp->tl_count)){
      tlp->tl_cursor = i;
      i = a_TTY_VF_MOD_CURSOR;
   }else
      i = a_TTY_VF_BELL;

   tlp->tl_vi_flags |= i;
   NYD2_LEAVE;
}

static void
a_tty_ksnarfw(struct a_tty_line *tlp, bool_t fwd){
   struct a_tty_cell *tcap;
   ui32_t cnt, cur, f;
   si32_t i;
   NYD2_ENTER;

   if(UNLIKELY((i = a_tty_wboundary(tlp, (fwd ? +1 : -1))) <= 0)){
      f = (i < 0) ? a_TTY_VF_BELL : a_TTY_VF_NONE;
      goto jleave;
   }

   cnt = tlp->tl_count - (ui32_t)i;
   cur = tlp->tl_cursor;
   if(!fwd)
      cur -= (ui32_t)i;
   tcap = &tlp->tl_line.cells[cur];

   a_tty_copy2paste(tlp, &tcap[0], &tcap[i]);

   if((tlp->tl_count = cnt) != (tlp->tl_cursor = cur)){
      cnt -= cur;
      memmove(&tcap[0], &tcap[i], cnt * sizeof(*tcap)); /* FIXME*/
   }

   f = a_TTY_VF_MOD_CURSOR | a_TTY_VF_MOD_CONTENT;
jleave:
   tlp->tl_vi_flags |= f;
   NYD2_LEAVE;
}

static void
a_tty_kgow(struct a_tty_line *tlp, si32_t dir){
   ui32_t f;
   si32_t i;
   NYD2_ENTER;

   if(UNLIKELY((i = a_tty_wboundary(tlp, dir)) <= 0))
      f = (i < 0) ? a_TTY_VF_BELL : a_TTY_VF_NONE;
   else{
      if(dir < 0)
         i = -i;
      tlp->tl_cursor += (ui32_t)i;
      f = a_TTY_VF_MOD_CURSOR;
   }

   tlp->tl_vi_flags |= f;
   NYD2_LEAVE;
}

static bool_t
a_tty_kother(struct a_tty_line *tlp, wchar_t wc){
   /* Append if at EOL, insert otherwise;
    * since we may move around character-wise, always use a fresh ps */
   mbstate_t ps;
   struct a_tty_cell tc, *tcap;
   ui32_t f, cur, cnt;
   bool_t rv;
   NYD2_ENTER;

   rv = FAL0;
   f = a_TTY_VF_NONE;

   n_LCTA(a_TTY_LINE_MAX <= SI32_MAX, "a_TTY_LINE_MAX too large");
   if(tlp->tl_count + 1 >= a_TTY_LINE_MAX){
      n_err(_("Stop here, we can't extend line beyond size limit\n"));
      goto jleave;
   }

   /* First init a cell and see whether we'll really handle this wc */
   memset(&ps, 0, sizeof ps);
   /* C99 */{
      size_t l;

      l = wcrtomb(tc.tc_cbuf, tc.tc_wc = wc, &ps);
      if(UNLIKELY(l > MB_LEN_MAX)){
jemb:
         n_err(_("wcrtomb(3) error: too many multibyte character bytes\n"));
         goto jleave;
      }
      tc.tc_count = (ui16_t)l;

      if(UNLIKELY((options & OPT_ENC_MBSTATE) != 0)){
         l = wcrtomb(&tc.tc_cbuf[l], L'\0', &ps);
         if(LIKELY(l == 1))
            /* Only NUL terminator */;
         else if(LIKELY(--l < MB_LEN_MAX))
            tc.tc_count += (ui16_t)l;
         else
            goto jemb;
      }
   }

   /* Yes, we will!  Place it in the array */
   tc.tc_novis = (iswprint(wc) == 0);
   tc.tc_width = a_tty_wcwidth(wc);
   /* TODO if(tc.tc_novis && tc.tc_width > 0) */

   cur = tlp->tl_cursor++;
   cnt = tlp->tl_count++ - cur;
   tcap = &tlp->tl_line.cells[cur];
   if(cnt >= 1){
      memmove(&tcap[1], tcap, cnt * sizeof(*tcap));
      f = a_TTY_VF_MOD_CONTENT;
   }else
      f = a_TTY_VF_MOD_SINGLE;
   memcpy(tcap, &tc, sizeof *tcap);

   f |= a_TTY_VF_MOD_CURSOR;
   rv = TRU1;
jleave:
   if(!rv)
      f |= a_TTY_VF_BELL;
   tlp->tl_vi_flags |= f;
   NYD2_LEAVE;
   return rv;
}

static ui32_t
a_tty_kht(struct a_tty_line *tlp){
   struct stat sb;
   struct str orig, bot, topp, sub, exp;
   struct n_string shou, *shoup;
   struct a_tty_cell *cword, *ctop, *cx;
   bool_t wedid, set_savec;
   ui32_t rv, f;
   NYD2_ENTER;

   f = a_TTY_VF_NONE;
   shoup = n_string_creat_auto(&shou);

   /* Get plain line data; if this is the first expansion/xy, update the
    * very original content so that ^G gets the origin back */
   orig = tlp->tl_savec;
   a_tty_cell2save(tlp);
   exp = tlp->tl_savec;
   if(orig.s != NULL){
      /*tlp->tl_savec = orig;*/
      set_savec = FAL0;
   }else
      set_savec = TRU1;
   orig = exp;

   /* Find the word to be expanded */

   cword = tlp->tl_line.cells;
   ctop = cword + tlp->tl_cursor;
   cx = cword + tlp->tl_count;

   /* topp: separate data right of cursor */
   if(cx > ctop){
      for(rv = 0; ctop < cx; ++ctop)
         rv += ctop->tc_count;
      topp.l = rv;
      topp.s = orig.s + orig.l - rv;
      ctop = cword + tlp->tl_cursor;
   }else
      topp.s = NULL, topp.l = 0;

   /* Find the shell token that corresponds to the cursor position */
   /* C99 */{
      size_t max;

      max = 0;
      if(ctop > cword){
         for(; cword < ctop; ++cword)
            max += cword->tc_count;
         cword = tlp->tl_line.cells;
      }
      bot = sub = orig;
      bot.l = 0;
      sub.l = max;

      if(max > 0){
         for(;;){
            enum n_shexp_state shs;

            exp = sub;
            shs = n_shexp_parse_token(NULL, &sub, n_SHEXP_PARSE_DRYRUN |
                  n_SHEXP_PARSE_TRIMSPACE | n_SHEXP_PARSE_IGNORE_EMPTY);
            if(sub.l != 0){
               size_t x;

               assert(max >= sub.l);
               x = max - sub.l;
               bot.l += x;
               max -= x;
               continue;
            }
            if(shs & n_SHEXP_STATE_ERR_MASK){
               n_err(_("Invalid completion pattern: %.*s\n"),
                  (int)exp.l, exp.s);
               goto jnope;
            }
            n_shexp_parse_token(shoup, &exp,
                  n_SHEXP_PARSE_TRIMSPACE | n_SHEXP_PARSE_IGNORE_EMPTY);
            break;
         }

         sub.s = n_string_cp(shoup);
         sub.l = shoup->s_len;
      }
   }

   /* Leave room for "implicit asterisk" expansion, as below */
   if(sub.l == 0){
      wedid = TRU1;
      sub.s = UNCONST("*");
      sub.l = 1;
   }

   wedid = FAL0;
jredo:
   /* TODO Super-Heavy-Metal: block all sigs, avoid leaks on jump */
   hold_all_sigs();
   exp.s = fexpand(sub.s, a_TTY_TAB_FEXP_FL);
   rele_all_sigs();

   if(exp.s == NULL || (exp.l = strlen(exp.s)) == 0)
      goto jnope;

   /* May be multi-return! */
   if(pstate & PS_EXPAND_MULTIRESULT)
      goto jmulti;

   /* xxx That is not really true since the limit counts characters not bytes */
   n_LCTA(a_TTY_LINE_MAX <= SI32_MAX, "a_TTY_LINE_MAX too large");
   if(exp.l + 1 >= a_TTY_LINE_MAX){
      n_err(_("Tabulator expansion would extend beyond line size limit\n"));
      goto jnope;
   }

   /* If the expansion equals the original string, assume the user wants what
    * is usually known as tab completion, append `*' and restart */
   if(!wedid && exp.l == sub.l && !memcmp(exp.s, sub.s, exp.l)){
      if(sub.s[sub.l - 1] == '*')
         goto jnope;

      wedid = TRU1;
      sub.s[sub.l++] = '*';
      sub.s[sub.l] = '\0';
      goto jredo;
   }
   /* If it is a directory, and there is not yet a / appended, then we want the
    * user to confirm that he wants to dive in -- with only a HT */
   else if(wedid && exp.l == --sub.l && !memcmp(exp.s, sub.s, exp.l) &&
         exp.s[exp.l - 1] != '/'){
      if(stat(exp.s, &sb) || !S_ISDIR(sb.st_mode))
         goto jnope;
      sub.s = salloc(exp.l + 1 +1);
      memcpy(sub.s, exp.s, exp.l);
      sub.s[exp.l++] = '/';
      sub.s[exp.l] = '\0';
      exp.s = sub.s;
      wedid = FAL0;
      goto jset;
   }else{
      if(wedid && (wedid = (exp.s[exp.l - 1] == '*')))
         --exp.l;
      exp.s[exp.l] = '\0';
jset:
      exp.l = strlen(exp.s = n_shexp_quote_cp(exp.s, tlp->tl_quote_rndtrip));
      tlp->tl_defc_cursor_byte = bot.l + exp.l -1;
      if(wedid)
         goto jnope;
   }

   orig.l = bot.l + exp.l + topp.l;
   orig.s = salloc(orig.l + 5 +1);
   if((rv = (ui32_t)bot.l) > 0)
      memcpy(orig.s, bot.s, rv);
   memcpy(orig.s + rv, exp.s, exp.l);
   rv += exp.l;
   if(topp.l > 0){
      memcpy(orig.s + rv, topp.s, topp.l);
      rv += topp.l;
   }
   orig.s[rv] = '\0';

   tlp->tl_defc = orig;
   tlp->tl_count = tlp->tl_cursor = 0;
   f |= a_TTY_VF_MOD_DIRTY;
jleave:
   n_string_gut(shoup);
   tlp->tl_vi_flags |= f;
   NYD2_LEAVE;
   return rv;

jmulti:{
      struct n_visual_info_ctx vic;
      struct str input;
      wc_t c2, c1;
      bool_t isfirst;
      char const *lococp;
      size_t locolen, scrwid, lnlen, lncnt, prefixlen;
      FILE *fp;

      if((fp = Ftmp(NULL, "tabex", OF_RDWR | OF_UNLINK | OF_REGISTER)) == NULL){
         n_perr(_("tmpfile"), 0);
         fp = stdout;
      }

      /* How long is the result string for real?  Search the NUL NUL
       * terminator.  While here, detect the longest entry to perform an
       * initial allocation of our accumulator string */
      locolen = 0;
      do{
         size_t i;

         i = strlen(&exp.s[++exp.l]);
         locolen = MAX(locolen, i);
         exp.l += i;
      }while(exp.s[exp.l + 1] != '\0');

      shoup = n_string_reserve(n_string_trunc(shoup, 0),
            locolen + (locolen >> 1));

      /* Iterate (once again) over all results */
      scrwid = (size_t)scrnwidth - ((size_t)scrnwidth >> 3);
      lnlen = lncnt = 0;
      UNINIT(prefixlen, 0);
      UNINIT(lococp, NULL);
      UNINIT(c1, '\0');
      for(isfirst = TRU1; exp.l > 0; isfirst = FAL0, c1 = c2){
         size_t i;
         char const *fullpath;

         /* Next result */
         sub = exp;
         sub.l = i = strlen(sub.s);
         assert(exp.l >= i);
         if((exp.l -= i) > 0)
            --exp.l;
         exp.s += ++i;

         /* Separate dirname and basename */
         fullpath = sub.s;
         if(isfirst){
            char const *cp;

            if((cp = strrchr(fullpath, '/')) != NULL)
               prefixlen = PTR2SIZE(++cp - fullpath);
            else
               prefixlen = 0;
         }
         if(prefixlen > 0 && prefixlen < sub.l){
            sub.l -= prefixlen;
            sub.s += prefixlen;
         }

         /* We want case-insensitive sort-order */
         memset(&vic, 0, sizeof vic);
         vic.vic_indat = sub.s;
         vic.vic_inlen = sub.l;
         c2 = n_visual_info(&vic, n_VISUAL_INFO_ONE_CHAR) ? vic.vic_waccu
               : (ui8_t)*sub.s;
#ifdef HAVE_C90AMEND1
         c2 = towlower(c2);
#else
         c2 = lowerconv(c2);
#endif

         /* Query longest common prefix along the way */
         if(isfirst){
            c1 = c2;
            lococp = sub.s;
            locolen = sub.l;
         }else if(locolen > 0){
            for(i = 0; i < locolen; ++i)
               if(lococp[i] != sub.s[i]){
                  i = field_detect_clip(i, lococp, i);
                  locolen = i;
                  break;
               }
         }

         /* Prepare display */
         input = sub;
         shoup = n_shexp_quote(n_string_trunc(shoup, 0), &input,
               tlp->tl_quote_rndtrip);
         memset(&vic, 0, sizeof vic);
         vic.vic_indat = shoup->s_dat;
         vic.vic_inlen = shoup->s_len;
         if(!n_visual_info(&vic,
               n_VISUAL_INFO_SKIP_ERRORS | n_VISUAL_INFO_WIDTH_QUERY))
            vic.vic_vi_width = shoup->s_len;

         /* Put on screen.  Indent follow lines of same sort slot */
         c1 = (c1 != c2);
         if(isfirst || c1 ||
               scrwid < lnlen || scrwid - lnlen <= vic.vic_vi_width + 2){
            putc('\n', fp);
            if(scrwid < lnlen)
               ++lncnt;
            ++lncnt, lnlen = 0;
            if(!isfirst && !c1)
               goto jsep;
         }else if(lnlen > 0){
jsep:
            fputs("  ", fp);
            lnlen += 2;
         }
         fputs(n_string_cp(shoup), fp);
         lnlen += vic.vic_vi_width;

         /* Support the known file name tagging
          * XXX *line-editor-completion-filetype* or so */
         if(!lstat(fullpath, &sb)){
            char c = '\0';

            if(S_ISDIR(sb.st_mode))
               c = '/';
            else if(S_ISLNK(sb.st_mode))
               c = '@';
# ifdef S_ISFIFO
            else if(S_ISFIFO(sb.st_mode))
               c = '|';
# endif
# ifdef S_ISSOCK
            else if(S_ISSOCK(sb.st_mode))
               c = '=';
# endif
# ifdef S_ISCHR
            else if(S_ISCHR(sb.st_mode))
               c = '%';
# endif
# ifdef S_ISBLK
            else if(S_ISBLK(sb.st_mode))
               c = '#';
# endif

            if(c != '\0'){
               putc(c, fp);
               ++lnlen;
            }
         }
      }
      putc('\n', fp);
      ++lncnt;

      page_or_print(fp, lncnt);
      if(fp != stdout)
         Fclose(fp);

      n_string_gut(shoup);

      /* A common prefix of 0 means we cannot provide the user any auto
       * completed characters */
      if(locolen == 0)
         goto jnope;

      /* Otherwise we can, so extend the visual line content by the common
       * prefix (in a reversible way) */
      (exp.s = UNCONST(lococp))[locolen] = '\0';
      exp.s -= prefixlen;
      exp.l = (locolen += prefixlen);

      /* XXX Indicate that there is multiple choice */
      /* XXX f |= a_TTY_VF_BELL; -> *line-editor-completion-bell*? or so */
      wedid = FAL0;
      goto jset;
   }

jnope:
   /* If we've provided a default content, but failed to expand, there is
    * nothing we can "revert to": drop that default again */
   if(set_savec){
      tlp->tl_savec.s = NULL;
      tlp->tl_savec.l = 0;
   }
   f = a_TTY_VF_NONE;
   rv = 0;
   goto jleave;
}

# ifdef HAVE_HISTORY
static ui32_t
a_tty__khist_shared(struct a_tty_line *tlp, struct a_tty_hist *thp){
   ui32_t f, rv;
   NYD2_ENTER;

   if(LIKELY((tlp->tl_hist = thp) != NULL)){
      tlp->tl_defc.s = savestrbuf(thp->th_dat, thp->th_len);
      rv = tlp->tl_defc.l = thp->th_len;
      f = (tlp->tl_count > 0) ? a_TTY_VF_MOD_DIRTY : a_TTY_VF_NONE;
      tlp->tl_count = tlp->tl_cursor = 0;
   }else{
      f = a_TTY_VF_BELL;
      rv = UI32_MAX;
   }

   tlp->tl_vi_flags |= f;
   NYD2_LEAVE;
   return rv;
}

static ui32_t
a_tty_khist(struct a_tty_line *tlp, bool_t fwd){
   struct a_tty_hist *thp;
   ui32_t rv;
   NYD2_ENTER;

   /* If we're not in history mode yet, save line content;
    * also, disallow forward search, then, and, of course, bail unless we
    * do have any history at all */
   if((thp = tlp->tl_hist) == NULL){
      if(fwd)
         goto jleave;
      if((thp = a_tty.tg_hist) == NULL)
         goto jleave;
      a_tty_cell2save(tlp);
      goto jleave;
   }

   thp = fwd ? thp->th_younger : thp->th_older;
jleave:
   rv = a_tty__khist_shared(tlp, thp);
   NYD2_LEAVE;
   return rv;
}

static ui32_t
a_tty_khist_search(struct a_tty_line *tlp, bool_t fwd){
   struct str orig_savec;
   struct a_tty_hist *thp;
   ui32_t rv;
   NYD2_ENTER;

   thp = NULL;

   /* We cannot complete an empty line */
   if(UNLIKELY(tlp->tl_count == 0)){
      /* XXX The upcoming hard reset would restore a set savec buffer,
       * XXX so forcefully reset that.  A cleaner solution would be to
       * XXX reset it whenever a restore is no longer desired */
      tlp->tl_savec.s = NULL;
      tlp->tl_savec.l = 0;
      goto jleave;
   }

   if((thp = tlp->tl_hist) == NULL){
      if((thp = a_tty.tg_hist) == NULL)
         goto jleave;
      /* We don't support wraparound, searching forward must always step */
      if(fwd)
         thp = thp->th_younger;
      orig_savec.s = NULL;
      orig_savec.l = 0; /* silence CC */
   }else if((thp = (fwd ? thp->th_younger : thp->th_older)) == NULL)
      goto jleave;
   else
      orig_savec = tlp->tl_savec;

   if(orig_savec.s == NULL)
      a_tty_cell2save(tlp);

   for(; thp != NULL; thp = (fwd ? thp->th_younger : thp->th_older))
      if(is_prefix(tlp->tl_savec.s, thp->th_dat))
         break;

   if(orig_savec.s != NULL)
      tlp->tl_savec = orig_savec;
jleave:
   rv = a_tty__khist_shared(tlp, thp);
   NYD2_LEAVE;
   return rv;
}
# endif /* HAVE_HISTORY */

static enum a_tty_fun_status
a_tty_fun(struct a_tty_line *tlp, enum a_tty_bind_flags tbf, size_t *len){
   enum a_tty_fun_status rv;
   NYD2_ENTER;

   rv = a_TTY_FUN_STATUS_OK;
# undef a_X
# define a_X(N) a_TTY_BIND_FUN_REDUCE(a_TTY_BIND_FUN_ ## N)
   switch(a_TTY_BIND_FUN_REDUCE(tbf)){
   case a_X(BELL):
      tlp->tl_vi_flags |= a_TTY_VF_BELL;
      break;
   case a_X(GO_BWD):
      a_tty_kleft(tlp);
      break;
   case a_X(GO_FWD):
      a_tty_kright(tlp);
      break;
   case a_X(GO_WORD_BWD):
      a_tty_kgow(tlp, -1);
      break;
   case a_X(GO_WORD_FWD):
      a_tty_kgow(tlp, +1);
      break;
   case a_X(GO_HOME):
      a_tty_khome(tlp, TRU1);
      break;
   case a_X(GO_END):
      a_tty_kend(tlp);
      break;
   case a_X(DEL_BWD):
      a_tty_kbs(tlp);
      break;
   case a_X(DEL_FWD):
      if(a_tty_kdel(tlp) < 0)
         rv = a_TTY_FUN_STATUS_END;
      break;
   case a_X(SNARF_WORD_BWD):
      a_tty_ksnarfw(tlp, FAL0);
      break;
   case a_X(SNARF_WORD_FWD):
      a_tty_ksnarfw(tlp, TRU1);
      break;
   case a_X(SNARF_END):
      a_tty_ksnarf(tlp, FAL0, TRU1);
      break;
   case a_X(SNARF_LINE):
      a_tty_ksnarf(tlp, TRU1, (tlp->tl_count == 0));
      break;

   case a_X(HIST_FWD):
# ifdef HAVE_HISTORY
      if(tlp->tl_hist != NULL){
         bool_t isfwd = TRU1;

         if(0){
# endif
      /* FALLTHRU */
   case a_X(HIST_BWD):
# ifdef HAVE_HISTORY
            isfwd = FAL0;
         }
         if((*len = a_tty_khist(tlp, isfwd)) != UI32_MAX){
            rv = a_TTY_FUN_STATUS_RESTART;
            break;
         }
         goto jreset;
# endif
      }
      tlp->tl_vi_flags |= a_TTY_VF_BELL;
      break;

   case a_X(HIST_SRCH_FWD):{
# ifdef HAVE_HISTORY
      bool_t isfwd = TRU1;

      if(0){
# endif
      /* FALLTHRU */
   case a_X(HIST_SRCH_BWD):
# ifdef HAVE_HISTORY
         isfwd = FAL0;
      }
      if((*len = a_tty_khist_search(tlp, isfwd)) != UI32_MAX){
         rv = a_TTY_FUN_STATUS_RESTART;
         break;
      }
      goto jreset;
# else
      tlp->tl_vi_flags |= a_TTY_VF_BELL;
# endif
   }  break;

   case a_X(REPAINT):
      tlp->tl_vi_flags |= a_TTY_VF_MOD_DIRTY;
      break;
   case a_X(QUOTE_RNDTRIP):
      tlp->tl_quote_rndtrip = !tlp->tl_quote_rndtrip;
      break;
   case a_X(PROMPT_CHAR):{
      wchar_t wc;

      if((wc = a_tty_vinuni(tlp)) > 0)
         a_tty_kother(tlp, wc);
   }  break;
   case a_X(COMPLETE):
      if((*len = a_tty_kht(tlp)) > 0)
         rv = a_TTY_FUN_STATUS_RESTART;
      break;

   case a_X(PASTE):
      if(tlp->tl_pastebuf.l > 0)
         *len = (tlp->tl_defc = tlp->tl_pastebuf).l;
      else
         tlp->tl_vi_flags |= a_TTY_VF_BELL;
      break;


   case a_X(CANCEL):
      /* Normally this just causes a restart and thus resets the state
       * machine  */
      if(tlp->tl_savec.l == 0 && tlp->tl_defc.l == 0){
      }
# ifdef HAVE_KEY_BINDINGS
      tlp->tl_bind_takeover = '\0';
# endif
      tlp->tl_vi_flags |= a_TTY_VF_BELL;
      rv = a_TTY_FUN_STATUS_RESTART;
      break;

   case a_X(RESET):
      if(tlp->tl_count == 0 && tlp->tl_savec.l == 0 && tlp->tl_defc.l == 0){
# ifdef HAVE_KEY_BINDINGS
         tlp->tl_bind_takeover = '\0';
# endif
         tlp->tl_vi_flags |= a_TTY_VF_MOD_DIRTY | a_TTY_VF_BELL;
         break;
      }else if(0){
   case a_X(FULLRESET):
         tlp->tl_savec.s = tlp->tl_defc.s = NULL;
         tlp->tl_savec.l = tlp->tl_defc.l = 0;
         tlp->tl_defc_cursor_byte = 0;
         tlp->tl_vi_flags |= a_TTY_VF_BELL;
      }
jreset:
# ifdef HAVE_KEY_BINDINGS
      tlp->tl_bind_takeover = '\0';
# endif
      tlp->tl_vi_flags |= a_TTY_VF_MOD_DIRTY;
      tlp->tl_cursor = tlp->tl_count = 0;
# ifdef HAVE_HISTORY
      tlp->tl_hist = NULL;
# endif
      if((*len = tlp->tl_savec.l) != 0){
         tlp->tl_defc = tlp->tl_savec;
         tlp->tl_savec.s = NULL;
         tlp->tl_savec.l = 0;
      }else
         *len = tlp->tl_defc.l;
      rv = a_TTY_FUN_STATUS_RESTART;
      break;

   default:
   case a_X(COMMIT):
      rv = a_TTY_FUN_STATUS_COMMIT;
      break;
   }
# undef a_X

   NYD2_LEAVE;
   return rv;
}

static ssize_t
a_tty_readline(struct a_tty_line *tlp, size_t len SMALLOC_DEBUG_ARGS){
   /* We want to save code, yet we may have to incorporate a lines'
    * default content and / or default input to switch back to after some
    * history movement; let "len > 0" mean "have to display some data
    * buffer" -> a_BUFMODE, and only otherwise read(2) it */
   mbstate_t ps[2];
   char cbuf_base[MB_LEN_MAX * 2], *cbuf, *cbufp;
   ssize_t rv;
   struct a_tty_bind_tree *tbtp;
   wchar_t wc;
   enum a_tty_bind_flags tbf;
   enum {a_NONE, a_WAS_HERE = 1<<0, a_BUFMODE = 1<<1, a_MAYBEFUN = 1<<2,
      a_TIMEOUT = 1<<3, a_TIMEOUT_EXPIRED = 1<<4,
         a_TIMEOUT_MASK = a_TIMEOUT | a_TIMEOUT_EXPIRED,
      a_READ_LOOP_MASK = ~(a_WAS_HERE | a_MAYBEFUN | a_TIMEOUT_MASK)
   } flags;
   NYD_ENTER;

   UNINIT(rv, 0);
# ifdef HAVE_KEY_BINDINGS
   assert(tlp->tl_bind_takeover == '\0');
# endif
jrestart:
   memset(ps, 0, sizeof ps);
   flags = a_NONE;
   tbf = 0;
   tlp->tl_vi_flags |= a_TTY_VF_REFRESH | a_TTY_VF_SYNC;

jinput_loop:
   for(;;){
      if(len != 0)
         flags |= a_BUFMODE;

      /* Ensure we have valid pointers, and room for grow */
      a_tty_check_grow(tlp, ((flags & a_BUFMODE) ? (ui32_t)len : 1)
         SMALLOC_DEBUG_ARGSCALL);

      /* Handle visual state flags, except in buffer mode */
      if(!(flags & a_BUFMODE) && (tlp->tl_vi_flags & a_TTY_VF_ALL_MASK))
         if(!a_tty_vi_refresh(tlp)){
            rv = -1;
            goto jleave;
         }

      /* Ready for messing around.
       * Normal read(2)?  Else buffer mode: speed this one up */
      if(!(flags & a_BUFMODE)){
         cbufp =
         cbuf = cbuf_base;
      }else{
         assert(tlp->tl_defc.l > 0 && tlp->tl_defc.s != NULL);
         assert(tlp->tl_defc.l >= len);
         cbufp =
         cbuf = tlp->tl_defc.s + (tlp->tl_defc.l - len);
         cbufp += len;
      }

      /* Read in the next complete multibyte character */
      /* C99 */{
# ifdef HAVE_KEY_BINDINGS
         struct a_tty_bind_tree *xtbtp;
         struct inseq{
            struct inseq *last;
            struct inseq *next;
            struct a_tty_bind_tree *tbtp;
         } *isp_head, *isp;

         isp_head = isp = NULL;
# endif

         for(flags &= a_READ_LOOP_MASK;;){
# ifdef HAVE_KEY_BINDINGS
            if(!(flags & a_BUFMODE) && tlp->tl_bind_takeover != '\0'){
               wc = tlp->tl_bind_takeover;
               tlp->tl_bind_takeover = '\0';
            }else
# endif
            {
               if(!(flags & a_BUFMODE)){
                  /* Let me at least once dream of iomon(itor), timer with
                   * one-shot, enwrapped with key_event and key_sequence_event,
                   * all driven by an event_loop */
                  /* TODO v15 Until we have SysV signal handling all through we
                   * TODO need to temporarily adjust our BSD signal handler with
                   * TODO a SysV one, here */
                  n_sighdl_t otstp, ottin, ottou;

                  otstp = n_signal(SIGTSTP, &n_tty_signal);
                  ottin = n_signal(SIGTTIN, &n_tty_signal);
                  ottou = n_signal(SIGTTOU, &n_tty_signal);
# ifdef HAVE_KEY_BINDINGS
                  flags &= ~a_TIMEOUT_MASK;
                  if(isp != NULL && (tbtp = isp->tbtp)->tbt_isseq &&
                        !tbtp->tbt_isseq_trail){
                     a_tty_term_rawmode_timeout(tlp, TRU1);
                     flags |= a_TIMEOUT;
                  }
# endif

                  while((rv = read(STDIN_FILENO, cbufp, 1)) < 1){
                     if(rv == -1){
                        if(errno == EINTR){
                           if((tlp->tl_vi_flags & a_TTY_VF_MOD_DIRTY) &&
                                 !a_tty_vi_refresh(tlp))
                              break;
                           continue;
                        }
                        break;
                     }

# ifdef HAVE_KEY_BINDINGS
                     /* Timeout expiration */
                     if(rv == 0){
                        assert(flags & a_TIMEOUT);
                        assert(isp != NULL);
                        a_tty_term_rawmode_timeout(tlp, FAL0);

                        /* Something "atomic" broke.  Maybe the current one can
                         * also be terminated already, by itself? xxx really? */
                        if((tbtp = isp->tbtp)->tbt_bind != NULL){
                           tlp->tl_bind_takeover = wc;
                           goto jhave_bind;
                        }

                        /* Or, maybe there is a second path without a timeout;
                         * this should be covered by .tbt_isseq_trail, but then
                         * again a single-layer implementation cannot "know" */
                        for(xtbtp = tbtp; (xtbtp = xtbtp->tbt_sibling) != NULL;)
                           if(xtbtp->tbt_char == tbtp->tbt_char){
                              assert(!xtbtp->tbt_isseq);
                              break;
                           }
                        /* Lay down on read(2)? */
                        if(xtbtp != NULL)
                           continue;
                        goto jtake_over;
                     }
# endif /* HAVE_KEY_BINDINGS */
                  }

# ifdef HAVE_KEY_BINDINGS
                  if(flags & a_TIMEOUT)
                     a_tty_term_rawmode_timeout(tlp, FAL0);
# endif
                  safe_signal(SIGTSTP, otstp);
                  safe_signal(SIGTTIN, ottin);
                  safe_signal(SIGTTOU, ottou);
                  if(rv < 0)
                     goto jleave;

                  ++cbufp;
               }

               rv = (ssize_t)mbrtowc(&wc, cbuf, PTR2SIZE(cbufp - cbuf), &ps[0]);
               if(rv <= 0){
                  /* Any error during buffer mode can only result in a hard
                   * reset;  Otherwise, if it's a hard error, or if too many
                   * redundant shift sequences overflow our buffer: perform
                   * hard reset */
                  if((flags & a_BUFMODE) || rv == -1 ||
                        sizeof cbuf_base == PTR2SIZE(cbufp - cbuf)){
                     a_tty_fun(tlp, a_TTY_BIND_FUN_FULLRESET, &len);
                     goto jrestart;
                  }
                  /* Otherwise, due to the way we deal with the buffer, we need
                   * to restore the mbstate_t from before this conversion */
                  ps[0] = ps[1];
                  continue;
               }
               cbufp = cbuf;
               ps[1] = ps[0];
            }

            /* Normal read(2)ing is subject to detection of key-bindings */
# ifdef HAVE_KEY_BINDINGS
            if(!(flags & a_BUFMODE)){
               /* Check for special bypass functions before we try to embed
                * this character into the tree */
               if(n_uasciichar(wc)){
                  char c;
                  char const *cp;

                  for(c = (char)wc, cp = &(*tlp->tl_bind_shcut_prompt_char)[0];
                        *cp != '\0'; ++cp){
                     if(c == *cp){
                        wc = a_tty_vinuni(tlp);
                        break;
                     }
                  }
                  if(wc == '\0'){
                     tlp->tl_vi_flags |= a_TTY_VF_BELL;
                     goto jinput_loop;
                  }
               }
               if(n_uasciichar(wc))
                  flags |= a_MAYBEFUN;
               else
                  flags &= ~a_MAYBEFUN;

               /* Search for this character in the bind tree */
               tbtp = (isp != NULL) ? isp->tbtp->tbt_childs
                     : (*tlp->tl_bind_tree_hmap)[wc % HSHSIZE];
               for(; tbtp != NULL; tbtp = tbtp->tbt_sibling){
                  if(tbtp->tbt_char == wc){
                     struct inseq *nisp;

                     /* If this one cannot continue we're likely finished! */
                     if(tbtp->tbt_childs == NULL){
                        assert(tbtp->tbt_bind != NULL);
                        tbf = tbtp->tbt_bind->tbc_flags;
                        goto jmle_fun;
                     }

                     /* This needs to read more characters */
                     nisp = salloc(sizeof *nisp);
                     if((nisp->last = isp) == NULL)
                        isp_head = nisp;
                     else
                        isp->next = nisp;
                     nisp->next = NULL;
                     nisp->tbtp = tbtp;
                     isp = nisp;
                     flags &= ~a_WAS_HERE;
                     break;
                  }
               }
               if(tbtp != NULL)
                  continue;

               /* Was there a binding active, but couldn't be continued? */
               if(isp != NULL){
                  /* A binding had a timeout, it didn't expire, but we saw
                   * something non-expected.  Something "atomic" broke.
                   * Maybe there is a second path without a timeout, that
                   * continues like we've seen it.  I.e., it may just have been
                   * the user, typing two fast.  We definitely want to allow
                   * bindings like \e,d etc. to succeed: users are so used to
                   * them that a timeout cannot be the mechanism to catch up!
                   * A single-layer implementation cannot "know" */
                  if((tbtp = isp->tbtp)->tbt_isseq && (isp->last == NULL ||
                        !(xtbtp = isp->last->tbtp)->tbt_isseq ||
                        xtbtp->tbt_isseq_trail)){
                     for(xtbtp = (tbtp = isp->tbtp);
                           (xtbtp = xtbtp->tbt_sibling) != NULL;)
                        if(xtbtp->tbt_char == tbtp->tbt_char){
                           assert(!xtbtp->tbt_isseq);
                           break;
                        }
                     if(xtbtp != NULL){
                        isp->tbtp = xtbtp;
                        tlp->tl_bind_takeover = wc;
                        continue;
                     }
                  }

                  /* Check for CANCEL shortcut now */
                  if(flags & a_MAYBEFUN){
                     char c;
                     char const *cp;

                     for(c = (char)wc, cp = &(*tlp->tl_bind_shcut_cancel)[0];
                           *cp != '\0'; ++cp)
                        if(c == *cp){
                           tbf = a_TTY_BIND_FUN_INTERNAL |a_TTY_BIND_FUN_CANCEL;
                           goto jmle_fun;
                        }
                  }

                  /* So: maybe the current sequence can be terminated here? */
                  if((tbtp = isp->tbtp)->tbt_bind != NULL){
jhave_bind:
                     tbf = tbtp->tbt_bind->tbc_flags;
jmle_fun:
                     if(tbf & a_TTY_BIND_FUN_INTERNAL){
                        switch(a_tty_fun(tlp, tbf, &len)){
                        case a_TTY_FUN_STATUS_OK:
                           goto jinput_loop;
                        case a_TTY_FUN_STATUS_COMMIT:
                           goto jdone;
                        case a_TTY_FUN_STATUS_RESTART:
                           goto jrestart;
                        case a_TTY_FUN_STATUS_END:
                           goto jleave;
                        }
                        assert(0);
                     }else if(tbtp->tbt_bind->tbc_flags & a_TTY_BIND_NOCOMMIT){
                        struct a_tty_bind_ctx *tbcp;

                        tbcp = tbtp->tbt_bind;
                        memcpy(tlp->tl_defc.s = salloc(
                           (tlp->tl_defc.l = len = tbcp->tbc_exp_len) +1),
                           tbcp->tbc_exp, tbcp->tbc_exp_len +1);
                        goto jrestart;
                     }else{
                        tlp->tl_reenter_after_cmd = tbtp->tbt_bind->tbc_exp;
                        goto jdone;
                     }
                  }
               }

               /* Otherwise take over all chars "as is" */
jtake_over:
               for(; isp_head != NULL; isp_head = isp_head->next)
                  if(a_tty_kother(tlp, isp_head->tbtp->tbt_char)){
                     /* FIXME */
                  }
               /* And the current one too */
               goto jkother;
            }
# endif /* HAVE_KEY_BINDINGS */

            if((flags & a_BUFMODE) && (len -= (size_t)rv) == 0){
               /* Buffer mode completed */
               tlp->tl_defc.s = NULL;
               tlp->tl_defc.l = 0;
               flags &= ~a_BUFMODE;
            }
            break;
         }

# ifndef HAVE_KEY_BINDINGS
         /* Don't interpret control bytes during buffer mode.
          * Otherwise, if it's a control byte check whether it is a MLE
          * function.  Remarks: initially a complete duplicate to be able to
          * switch(), later converted to simply iterate over (an #ifdef'd
          * subset of) the MLE default_tuple table in order to have "a SPOF" */
         if(cbuf == cbuf_base && n_uasciichar(wc) && cntrlchar((char)wc)){
            struct a_tty_bind_default_tuple const *tbdtp;
            char c;

            for(c = (char)wc ^ 0x40, tbdtp = a_tty_bind_default_tuples;
                  PTRCMP(tbdtp, <, &a_tty_bind_default_tuples[
                     NELEM(a_tty_bind_default_tuples)]);
                  ++tbdtp){
               /* Assert default_tuple table is properly subset'ed */
               assert(tbdtp->tbdt_iskey);
               if(tbdtp->tbdt_ckey == c){
                  if(tbdtp->tbdt_exp[0] == '\0'){
                     enum a_tty_bind_flags tbf;

                     tbf = a_TTY_BIND_FUN_EXPAND((ui8_t)tbdtp->tbdt_exp[1]);
                     switch(a_tty_fun(tlp, tbf, &len)){
                     case a_TTY_FUN_STATUS_OK:
                        goto jinput_loop;
                     case a_TTY_FUN_STATUS_COMMIT:
                        goto jdone;
                     case a_TTY_FUN_STATUS_RESTART:
                        goto jrestart;
                     case a_TTY_FUN_STATUS_END:
                        goto jleave;
                     }
                     assert(0);
                  }else{
                     tlp->tl_reenter_after_cmd = tbdtp->tbdt_exp;
                     goto jdone;
                  }
               }
            }
         }
#  endif /* !HAVE_KEY_BINDINGS */

# ifdef HAVE_KEY_BINDINGS
jkother:
# endif
         if(a_tty_kother(tlp, wc)){
            /* Don't clear the history during buffer mode.. */
# ifdef HAVE_HISTORY
            if(!(flags & a_BUFMODE) && cbuf == cbuf_base)
               tlp->tl_hist = NULL;
# endif
         }
      }
   }

   /* We have a completed input line, convert the struct cell data to its
    * plain character equivalent */
jdone:
   rv = a_tty_cell2dat(tlp);
jleave:
   putchar('\n');
   fflush(stdout);
   NYD_LEAVE;
   return rv;
}

# ifdef HAVE_KEY_BINDINGS
static enum n_lexinput_flags
a_tty_bind_ctx_find(char const *name){
   enum n_lexinput_flags rv;
   struct a_tty_bind_ctx_map const *tbcmp;
   NYD2_ENTER;

   tbcmp = a_tty_bind_ctx_maps;
   do if(!asccasecmp(tbcmp->tbcm_name, name)){
      rv = tbcmp->tbcm_ctx;
      goto jleave;
   }while(PTRCMP(++tbcmp, <, &a_tty_bind_ctx_maps[NELEM(a_tty_bind_ctx_maps)]));

   rv = (enum n_lexinput_flags)-1;
jleave:
   NYD2_LEAVE;
   return rv;
}

static bool_t
a_tty_bind_create(struct a_tty_bind_parse_ctx *tbpcp, bool_t replace){
   struct a_tty_bind_ctx *tbcp;
   bool_t rv;
   NYD2_ENTER;

   rv = FAL0;

   if(!a_tty_bind_parse(TRU1, tbpcp))
      goto jleave;

   /* Since we use a single buffer for it all, need to replace as such */
   if(tbpcp->tbpc_tbcp != NULL){
      if(!replace)
         goto jleave;
      a_tty_bind_del(tbpcp);
   }else if(a_tty.tg_bind_cnt == UI32_MAX){
      n_err(_("`bind': maximum number of bindings already established\n"));
      goto jleave;
   }

   /* C99 */{
      size_t i, j;

      tbcp = smalloc(sizeof(*tbcp) -
            VFIELD_SIZEOF(struct a_tty_bind_ctx, tbc__buf) +
            tbpcp->tbpc_seq_len + tbpcp->tbpc_exp.l +
            MAX(sizeof(si32_t), sizeof(wc_t)) + tbpcp->tbpc_cnv_len +3);
      if(tbpcp->tbpc_ltbcp != NULL){
         tbcp->tbc_next = tbpcp->tbpc_ltbcp->tbc_next;
         tbpcp->tbpc_ltbcp->tbc_next = tbcp;
      }else{
         enum n_lexinput_flags lif = tbpcp->tbpc_flags & n__LEXINPUT_CTX_MASK;

         tbcp->tbc_next = a_tty.tg_bind[lif];
         a_tty.tg_bind[lif] = tbcp;
      }
      memcpy(tbcp->tbc_seq = &tbcp->tbc__buf[0],
         tbpcp->tbpc_seq, i = (tbcp->tbc_seq_len = tbpcp->tbpc_seq_len) +1);
      memcpy(tbcp->tbc_exp = &tbcp->tbc__buf[i],
         tbpcp->tbpc_exp.s, j = (tbcp->tbc_exp_len = tbpcp->tbpc_exp.l) +1);
      i += j;
      i = (i + tbpcp->tbpc_cnv_align_mask) & ~tbpcp->tbpc_cnv_align_mask;
      memcpy(tbcp->tbc_cnv = &tbcp->tbc__buf[i],
         tbpcp->tbpc_cnv, (tbcp->tbc_cnv_len = tbpcp->tbpc_cnv_len) +1);
      tbcp->tbc_flags = tbpcp->tbpc_flags;
   }

   /* Directly resolve any termcap(5) symbol if we are already setup */
   if((pstate & PS_STARTED) &&
         (tbcp->tbc_flags & (a_TTY_BIND_RESOLVE | a_TTY_BIND_DEFUNCT)) ==
          a_TTY_BIND_RESOLVE)
      a_tty_bind_resolve(tbcp);

   ++a_tty.tg_bind_cnt;
   /* If this binding is usable invalidate the key input lookup trees */
   if(!(tbcp->tbc_flags & a_TTY_BIND_DEFUNCT))
      a_tty.tg_bind_isdirty = TRU1;
   rv = TRU1;
jleave:
   NYD2_LEAVE;
   return rv;
}

static bool_t
a_tty_bind_parse(bool_t isbindcmd, struct a_tty_bind_parse_ctx *tbpcp){
   enum{a_TRUE_RV = a_TTY__BIND_LAST<<1};

   struct n_visual_info_ctx vic;
   struct str shin_save, shin;
   struct n_string shou, *shoup;
   size_t i;
   struct kse{
      struct kse *next;
      char *seq_dat;
      wc_t *cnv_dat;
      ui32_t seq_len;
      ui32_t cnv_len;      /* High bit set if a termap to be resolved */
      ui32_t calc_cnv_len; /* Ditto, but aligned etc. */
      ui8_t kse__dummy[4];
   } *head, *tail;
   ui32_t f;
   NYD2_ENTER;
   n_LCTA(UICMP(64, a_TRUE_RV, <, UI32_MAX),
      "Flag bits excess storage datatype");

   f = n_LEXINPUT_NONE;
   shoup = n_string_creat_auto(&shou);
   head = tail = NULL;

   /* Parse the key-sequence */
   for(shin.s = UNCONST(tbpcp->tbpc_in_seq), shin.l = UIZ_MAX;;){
      struct kse *ep;
      enum n_shexp_state shs;

      shin_save = shin;
      shs = n_shexp_parse_token(shoup, &shin,
            n_SHEXP_PARSE_TRUNC | n_SHEXP_PARSE_TRIMSPACE |
            n_SHEXP_PARSE_IGNORE_EMPTY | n_SHEXP_PARSE_IFS_IS_COMMA);
      if(shs & n_SHEXP_STATE_ERR_UNICODE){
         f |= a_TTY_BIND_DEFUNCT;
         if(isbindcmd && (options & OPT_D_V))
            n_err(_("`%s': \\uNICODE not available in locale: %s\n"),
               tbpcp->tbpc_cmd, tbpcp->tbpc_in_seq);
      }
      if((shs & n_SHEXP_STATE_ERR_MASK) & ~n_SHEXP_STATE_ERR_UNICODE){
         n_err(_("`%s': failed to parse key-sequence: %s\n"),
            tbpcp->tbpc_cmd, tbpcp->tbpc_in_seq);
         goto jleave;
      }
      if((shs & (n_SHEXP_STATE_OUTPUT | n_SHEXP_STATE_STOP)) ==
            n_SHEXP_STATE_STOP)
         break;

      ep = salloc(sizeof *ep);
      if(head == NULL)
         head = ep;
      else
         tail->next = ep;
      tail = ep;
      ep->next = NULL;
      if(!(shs & n_SHEXP_STATE_ERR_UNICODE)){
         i = strlen(ep->seq_dat = n_shexp_quote_cp(n_string_cp(shoup), TRU1));
         if(i >= SI32_MAX - 1)
            goto jelen;
         ep->seq_len = (ui32_t)i;
      }else{
         /* Otherwise use the original buffer, _we_ can only quote it the wrong
          * way (e.g., an initial $'\u3a' becomes '\u3a'), _then_ */
         if((i = shin_save.l - shin.l) >= SI32_MAX - 1)
            goto jelen;
         ep->seq_len = (ui32_t)i;
         ep->seq_dat = savestrbuf(shin_save.s, i);
      }

      memset(&vic, 0, sizeof vic);
      vic.vic_inlen = shoup->s_len;
      vic.vic_indat = shoup->s_dat;
      if(!n_visual_info(&vic,
            n_VISUAL_INFO_WOUT_CREATE | n_VISUAL_INFO_WOUT_SALLOC)){
         n_err(_("`%s': key-sequence seems to contain invalid "
            "characters: %s: %s\n"),
            tbpcp->tbpc_cmd, n_string_cp(shoup), tbpcp->tbpc_in_seq);
         f |= a_TTY_BIND_DEFUNCT;
         goto jleave;
      }else if(vic.vic_woulen == 0 ||
            vic.vic_woulen >= (SI32_MAX - 2) / sizeof(wc_t)){
jelen:
         n_err(_("`%s': length of key-sequence unsupported: %s: %s\n"),
            tbpcp->tbpc_cmd, n_string_cp(shoup), tbpcp->tbpc_in_seq);
         f |= a_TTY_BIND_DEFUNCT;
         goto jleave;
      }
      ep->cnv_dat = vic.vic_woudat;
      ep->cnv_len = (ui32_t)vic.vic_woulen;

      /* A termcap(5)/terminfo(5) identifier? */
      if(ep->cnv_len > 1 && ep->cnv_dat[0] == ':'){
         i = --ep->cnv_len, ++ep->cnv_dat;
#  ifndef HAVE_TERMCAP
         if(options & OPT_D_V)
            n_err(_("`%s': no termcap(5)/terminfo(5) support: %s: %s\n"),
               tbpcp->tbpc_cmd, ep->seq_dat, tbpcp->tbpc_in_seq);
         f |= a_TTY_BIND_DEFUNCT;
#  endif
         if(i > a_TTY_BIND_CAPNAME_MAX){
            n_err(_("`%s': termcap(5)/terminfo(5) name too long: %s: %s\n"),
               tbpcp->tbpc_cmd, ep->seq_dat, tbpcp->tbpc_in_seq);
            f |= a_TTY_BIND_DEFUNCT;
         }
         while(i > 0)
            /* (We store it as char[]) */
            if((ui32_t)ep->cnv_dat[--i] & ~0x7Fu){
               n_err(_("`%s': invalid termcap(5)/terminfo(5) name content: "
                  "%s: %s\n"),
                  tbpcp->tbpc_cmd, ep->seq_dat, tbpcp->tbpc_in_seq);
               f |= a_TTY_BIND_DEFUNCT;
               break;
            }
         ep->cnv_len |= SI32_MIN; /* Needs resolve */
         f |= a_TTY_BIND_RESOLVE;
      }

      if(shs & n_SHEXP_STATE_STOP)
         break;
   }

   if(head == NULL){
jeempty:
      n_err(_("`%s': effectively empty key-sequence: %s\n"),
         tbpcp->tbpc_cmd, tbpcp->tbpc_in_seq);
      goto jleave;
   }

   if(isbindcmd) /* (Or always, just "1st time init") */
      tbpcp->tbpc_cnv_align_mask = MAX(sizeof(si32_t), sizeof(wc_t)) - 1;

   /* C99 */{
      struct a_tty_bind_ctx *ltbcp, *tbcp;
      char *cpbase, *cp, *cnv;
      size_t sl, cl;

      /* Unite the parsed sequence(s) into single string representations */
      for(sl = cl = 0, tail = head; tail != NULL; tail = tail->next){
         sl += tail->seq_len + 1;

         if(!isbindcmd)
            continue;

         /* Preserve room for terminal capabilities to be resolved.
          * Above we have ensured the buffer will fit in these calculations */
         if((i = tail->cnv_len) & SI32_MIN){
            /* For now
             * struct{si32_t buf_len_iscap; si32_t cap_len; wc_t name[]+NUL;}
             * later
             * struct{si32_t buf_len_iscap; si32_t cap_len; char buf[]+NUL;} */
            n_LCTAV(ISPOW2(a_TTY_BIND_CAPEXP_ROUNDUP));
            n_LCTA(a_TTY_BIND_CAPEXP_ROUNDUP >= sizeof(wc_t),
               "Aligning on this constant doesn't properly align wc_t");
            i &= SI32_MAX;
            i *= sizeof(wc_t);
            i += sizeof(si32_t);
            if(i < a_TTY_BIND_CAPEXP_ROUNDUP)
               i = (i + (a_TTY_BIND_CAPEXP_ROUNDUP - 1)) &
                     ~(a_TTY_BIND_CAPEXP_ROUNDUP - 1);
         }else
            /* struct{si32_t buf_len_iscap; wc_t buf[]+NUL;} */
            i *= sizeof(wc_t);
         i += sizeof(si32_t) + sizeof(wc_t); /* (buf_len_iscap, NUL) */
         cl += i;
         if(tail->cnv_len & SI32_MIN){
            tail->cnv_len &= SI32_MAX;
            i |= SI32_MIN;
         }
         tail->calc_cnv_len = (ui32_t)i;
      }
      --sl;

      tbpcp->tbpc_seq_len = sl;
      tbpcp->tbpc_cnv_len = cl;
      /* C99 */{
         size_t j;

         j = i = sl + 1; /* Room for comma separator */
         if(isbindcmd){
            i = (i + tbpcp->tbpc_cnv_align_mask) & ~tbpcp->tbpc_cnv_align_mask;
            j = i;
            i += cl;
         }
         tbpcp->tbpc_seq = cp = cpbase = salloc(i);
         tbpcp->tbpc_cnv = cnv = &cpbase[j];
      }

      for(tail = head; tail != NULL; tail = tail->next){
         memcpy(cp, tail->seq_dat, tail->seq_len);
         cp += tail->seq_len;
         *cp++ = ',';

         if(isbindcmd){
            char * const save_cnv = cnv;

            ((si32_t*)cnv)[0] = (si32_t)(i = tail->calc_cnv_len);
            cnv += sizeof(si32_t);
            if(i & SI32_MIN){
               /* For now
                * struct{si32_t buf_len_iscap; si32_t cap_len; wc_t name[];}
                * later
                * struct{si32_t buf_len_iscap; si32_t cap_len; char buf[];} */
               ((si32_t*)cnv)[0] = tail->cnv_len;
               cnv += sizeof(si32_t);
            }
            i = tail->cnv_len * sizeof(wc_t);
            memcpy(cnv, tail->cnv_dat, i);
            cnv += i;
            *(wc_t*)cnv = '\0';

            cnv = save_cnv + (tail->calc_cnv_len & SI32_MAX);
         }
      }
      *--cp = '\0';

      /* Search for a yet existing identical mapping */
      for(ltbcp = NULL, tbcp = a_tty.tg_bind[tbpcp->tbpc_flags]; tbcp != NULL;
            ltbcp = tbcp, tbcp = tbcp->tbc_next)
         if(tbcp->tbc_seq_len == sl && !memcmp(tbcp->tbc_seq, cpbase, sl)){
            tbpcp->tbpc_tbcp = tbcp;
            break;
         }
      tbpcp->tbpc_ltbcp = ltbcp;
      tbpcp->tbpc_flags |= (f & a_TTY__BIND_MASK);
   }

   /* Create single string expansion if so desired */
   if(isbindcmd){
      char *exp;

      exp = tbpcp->tbpc_exp.s;

      i = tbpcp->tbpc_exp.l;
      if(i > 0 && exp[i - 1] == '@'){
         while(--i > 0){
            if(!blankspacechar(exp[i - 1]))
               break;
         }
         if(i == 0)
            goto jeempty;

         exp[tbpcp->tbpc_exp.l = i] = '\0';
         tbpcp->tbpc_flags |= a_TTY_BIND_NOCOMMIT;
      }

      /* It may map to an internal MLE command! */
      for(i = 0; i < NELEM(a_tty_bind_fun_names); ++i)
         if(!asccasecmp(exp, a_tty_bind_fun_names[i])){
            tbpcp->tbpc_flags |= a_TTY_BIND_FUN_EXPAND(i) |
                  a_TTY_BIND_FUN_INTERNAL |
                  (head->next == NULL ? a_TTY_BIND_MLE1CNTRL : 0);
            if((options & OPT_D_V) && (tbpcp->tbpc_flags & a_TTY_BIND_NOCOMMIT))
               n_err(_("`%s': MLE commands can't be made editable via @: %s\n"),
                  tbpcp->tbpc_cmd, exp);
            tbpcp->tbpc_flags &= ~a_TTY_BIND_NOCOMMIT;
            break;
         }
   }

  f |= a_TRUE_RV; /* TODO because we only now true and false; DEFUNCT.. */
jleave:
   n_string_gut(shoup);
   NYD2_LEAVE;
   return (f & a_TRUE_RV) != 0;
}

static void
a_tty_bind_resolve(struct a_tty_bind_ctx *tbcp){
   char capname[a_TTY_BIND_CAPNAME_MAX +1];
   struct n_termcap_value tv;
   size_t len;
   bool_t isfirst; /* TODO For now: first char must be control! */
   char *cp, *next;
   NYD2_ENTER;

   UNINIT(next, NULL);
   for(cp = tbcp->tbc_cnv, isfirst = TRU1, len = tbcp->tbc_cnv_len;
         len > 0; isfirst = FAL0, cp = next){
      /* C99 */{
         si32_t i, j;

         i = ((si32_t*)cp)[0];
         j = i & SI32_MAX;
         next = &cp[j];
         len -= j;
         if(i == j)
            continue;

         /* struct{si32_t buf_len_iscap; si32_t cap_len; wc_t name[];} */
         cp += sizeof(si32_t);
         i = ((si32_t*)cp)[0];
         cp += sizeof(si32_t);
         for(j = 0; j < i; ++j)
            capname[j] = ((wc_t*)cp)[j];
         capname[j] = '\0';
      }

      /* Use generic lookup mechanism if not a known query */
      /* C99 */{
         si32_t tq;

         tq = n_termcap_query_for_name(capname, n_TERMCAP_CAPTYPE_STRING);
         if(tq == -1){
            tv.tv_data.tvd_string = capname;
            tq = n__TERMCAP_QUERY_MAX;
         }

         if(tq < 0 || !n_termcap_query(tq, &tv)){
            if(options & OPT_D_V)
               n_err(_("`bind': unknown or unsupported capability: %s: %s\n"),
                  capname, tbcp->tbc_seq);
            tbcp->tbc_flags |= a_TTY_BIND_DEFUNCT;
            break;
         }
      }

      /* struct{si32_t buf_len_iscap; si32_t cap_len; char buf[]+NUL;} */
      /* C99 */{
         size_t i;

         i = strlen(tv.tv_data.tvd_string);
         if(/*i > SI32_MAX ||*/ i >= PTR2SIZE(next - cp)){
            if(options & OPT_D_V)
               n_err(_("`bind': capability expansion too long: %s: %s\n"),
                  capname, tbcp->tbc_seq);
            tbcp->tbc_flags |= a_TTY_BIND_DEFUNCT;
            break;
         }else if(i == 0){
            if(options & OPT_D_V)
               n_err(_("`bind': empty capability expansion: %s: %s\n"),
                  capname, tbcp->tbc_seq);
            tbcp->tbc_flags |= a_TTY_BIND_DEFUNCT;
            break;
         }else if(isfirst && !cntrlchar(*tv.tv_data.tvd_string)){
            if(options & OPT_D_V)
               n_err(_("`bind': capability expansion doesn't start with "
                  "control: %s: %s\n"), capname, tbcp->tbc_seq);
            tbcp->tbc_flags |= a_TTY_BIND_DEFUNCT;
            break;
         }
         ((si32_t*)cp)[-1] = (si32_t)i;
         memcpy(cp, tv.tv_data.tvd_string, i);
         cp[i] = '\0';
      }
   }
   NYD2_LEAVE;
}

static void
a_tty_bind_del(struct a_tty_bind_parse_ctx *tbpcp){
   struct a_tty_bind_ctx *ltbcp, *tbcp;
   NYD2_ENTER;

   tbcp = tbpcp->tbpc_tbcp;

   if((ltbcp = tbpcp->tbpc_ltbcp) != NULL)
      ltbcp->tbc_next = tbcp->tbc_next;
   else
      a_tty.tg_bind[tbpcp->tbpc_flags] = tbcp->tbc_next;
   free(tbcp);

   --a_tty.tg_bind_cnt;
   a_tty.tg_bind_isdirty = TRU1;
   NYD2_LEAVE;
}

static void
a_tty_bind_tree_build(void){
   size_t i;
   NYD2_ENTER;

   for(i = 0; i < n__LEXINPUT_CTX_MAX; ++i){
      struct a_tty_bind_ctx *tbcp;
      n_LCTAV(n_LEXINPUT_CTX_BASE == 0);

      /* Somewhat wasteful, but easier to handle: simply clone the entire
       * primary key onto the secondary one, then only modify it */
      for(tbcp = a_tty.tg_bind[n_LEXINPUT_CTX_BASE]; tbcp != NULL;
            tbcp = tbcp->tbc_next)
         if(!(tbcp->tbc_flags & a_TTY_BIND_DEFUNCT))
            a_tty__bind_tree_add(n_LEXINPUT_CTX_BASE, &a_tty.tg_bind_tree[i][0],
               tbcp);

      if(i != n_LEXINPUT_CTX_BASE)
         for(tbcp = a_tty.tg_bind[i]; tbcp != NULL; tbcp = tbcp->tbc_next)
            if(!(tbcp->tbc_flags & a_TTY_BIND_DEFUNCT))
               a_tty__bind_tree_add(i, &a_tty.tg_bind_tree[i][0], tbcp);
   }

   a_tty.tg_bind_isbuild = TRU1;
   NYD2_LEAVE;
}

static void
a_tty_bind_tree_teardown(void){
   size_t i, j;
   NYD2_ENTER;

   memset(&a_tty.tg_bind_shcut_cancel[0], 0,
      sizeof(a_tty.tg_bind_shcut_cancel));
   memset(&a_tty.tg_bind_shcut_prompt_char[0], 0,
      sizeof(a_tty.tg_bind_shcut_prompt_char));

   for(i = 0; i < n__LEXINPUT_CTX_MAX; ++i)
      for(j = 0; j < HSHSIZE; ++j)
         a_tty__bind_tree_free(a_tty.tg_bind_tree[i][j]);
   memset(&a_tty.tg_bind_tree[0], 0, sizeof(a_tty.tg_bind_tree));

   a_tty.tg_bind_isdirty = a_tty.tg_bind_isbuild = FAL0;
   NYD2_LEAVE;
}

static void
a_tty__bind_tree_add(ui32_t hmap_idx, struct a_tty_bind_tree *store[HSHSIZE],
      struct a_tty_bind_ctx *tbcp){
   ui32_t cnvlen;
   char const *cnvdat;
   struct a_tty_bind_tree *ntbtp;
   NYD2_ENTER;
   UNUSED(hmap_idx);

   ntbtp = NULL;

   for(cnvdat = tbcp->tbc_cnv, cnvlen = tbcp->tbc_cnv_len; cnvlen > 0;){
      union {wchar_t const *wp; char const *cp;} u;
      si32_t entlen;

      /* {si32_t buf_len_iscap;} */
      entlen = *(si32_t const*)cnvdat;

      if(entlen & SI32_MIN){
         /* struct{si32_t buf_len_iscap; si32_t cap_len; char buf[]+NUL;}
          * Note that empty capabilities result in DEFUNCT */
         for(u.cp = (char const*)&((si32_t const*)cnvdat)[2];
               *u.cp != '\0'; ++u.cp)
            ntbtp = a_tty__bind_tree_add_wc(store, ntbtp, *u.cp, TRU1);
         assert(ntbtp != NULL);
         ntbtp->tbt_isseq_trail = TRU1;
         entlen &= SI32_MAX;
      }else{
         /* struct{si32_t buf_len_iscap; wc_t buf[]+NUL;} */
         bool_t isseq;

         u.wp = (wchar_t const*)&((si32_t const*)cnvdat)[1];

         /* May be a special shortcut function? */
         if(ntbtp == NULL && (tbcp->tbc_flags & a_TTY_BIND_MLE1CNTRL)){
            char *cp;
            ui32_t ctx, fun;

            ctx = tbcp->tbc_flags & n__LEXINPUT_CTX_MAX;
            fun = tbcp->tbc_flags & a_TTY__BIND_FUN_MASK;

            if(fun == a_TTY_BIND_FUN_CANCEL){
               for(cp = &a_tty.tg_bind_shcut_cancel[ctx][0];
                     PTRCMP(cp, <, &a_tty.tg_bind_shcut_cancel[ctx]
                        [NELEM(a_tty.tg_bind_shcut_cancel[ctx]) - 1]); ++cp)
                  if(*cp == '\0'){
                     *cp = (char)*u.wp;
                     break;
                  }
            }else if(fun == a_TTY_BIND_FUN_PROMPT_CHAR){
               for(cp = &a_tty.tg_bind_shcut_prompt_char[ctx][0];
                     PTRCMP(cp, <, &a_tty.tg_bind_shcut_prompt_char[ctx]
                        [NELEM(a_tty.tg_bind_shcut_prompt_char[ctx]) - 1]);
                     ++cp)
                  if(*cp == '\0'){
                     *cp = (char)*u.wp;
                     break;
                  }
            }
         }

         isseq = (u.wp[1] != '\0');
         for(; *u.wp != '\0'; ++u.wp)
            ntbtp = a_tty__bind_tree_add_wc(store, ntbtp, *u.wp, isseq);
         if(isseq)
            ntbtp->tbt_isseq_trail = TRU1;
      }

      cnvlen -= entlen;
      cnvdat += entlen;
   }

   /* Should have been rendered defunctional at first instead */
   assert(ntbtp != NULL);
   ntbtp->tbt_bind = tbcp;
   NYD2_LEAVE;
}

static struct a_tty_bind_tree *
a_tty__bind_tree_add_wc(struct a_tty_bind_tree **treep,
      struct a_tty_bind_tree *parentp, wchar_t wc, bool_t isseq){
   struct a_tty_bind_tree *tbtp, *xtbtp;
   NYD2_ENTER;

   if(parentp == NULL){
      treep += wc % HSHSIZE;

      /* Having no parent also means that the tree slot is possibly empty */
      for(tbtp = *treep; tbtp != NULL;
            parentp = tbtp, tbtp = tbtp->tbt_sibling){
         if(tbtp->tbt_char != wc)
            continue;
         if(tbtp->tbt_isseq == isseq)
            goto jleave;
         /* isseq MUST be linked before !isseq, so record this "parent"
          * sibling, but continue searching for now */
         if(!isseq)
            parentp = tbtp;
         /* Otherwise it is impossible that we'll find what we look for */
         else{
#ifdef HAVE_DEBUG
            while((tbtp = tbtp->tbt_sibling) != NULL)
               assert(tbtp->tbt_char != wc);
#endif
            break;
         }
      }

      tbtp = smalloc(sizeof *tbtp);
      memset(tbtp, 0, sizeof *tbtp);
      tbtp->tbt_char = wc;
      tbtp->tbt_isseq = isseq;

      if(parentp == NULL){
         tbtp->tbt_sibling = *treep;
         *treep = tbtp;
      }else{
         tbtp->tbt_sibling = parentp->tbt_sibling;
         parentp->tbt_sibling = tbtp;
      }
   }else{
      if((tbtp = *(treep = &parentp->tbt_childs)) != NULL){
         for(;; tbtp = xtbtp){
            if(tbtp->tbt_char == wc){
               if(tbtp->tbt_isseq == isseq)
                  goto jleave;
               /* isseq MUST be linked before, so it is impossible that we'll
                * find what we look for */
               if(isseq){
#ifdef HAVE_DEBUG
                  while((tbtp = tbtp->tbt_sibling) != NULL)
                     assert(tbtp->tbt_char != wc);
#endif
                  tbtp = NULL;
                  break;
               }
            }

            if((xtbtp = tbtp->tbt_sibling) == NULL){
               treep = &tbtp->tbt_sibling;
               break;
            }
         }
      }

      xtbtp = smalloc(sizeof *xtbtp);
      memset(xtbtp, 0, sizeof *xtbtp);
      xtbtp->tbt_parent = parentp;
      xtbtp->tbt_char = wc;
      xtbtp->tbt_isseq = isseq;
      tbtp = xtbtp;
      *treep = tbtp;
   }
jleave:
   NYD2_LEAVE;
   return tbtp;
}

static void
a_tty__bind_tree_free(struct a_tty_bind_tree *tbtp){
   NYD2_ENTER;
   while(tbtp != NULL){
      struct a_tty_bind_tree *tmp;

      if((tmp = tbtp->tbt_childs) != NULL)
         a_tty__bind_tree_free(tmp);

      tmp = tbtp->tbt_sibling;
      free(tbtp);
      tbtp = tmp;
   }
   NYD2_LEAVE;
}
# endif /* HAVE_KEY_BINDINGS */

FL void
n_tty_init(void){
   NYD_ENTER;

   if(ok_blook(line_editor_disable))
      goto jleave;

   /* Load the history file */
# ifdef HAVE_HISTORY
   do/* for break */{
      long hs;
      char const *v;
      char *lbuf;
      FILE *f;
      size_t lsize, cnt, llen;

      a_TTY_HISTSIZE(hs);
      a_tty.tg_hist_size = 0;
      a_tty.tg_hist_size_max = (size_t)hs;
      if(hs == 0)
         break;

      a_TTY_HISTFILE(v);
      if(v == NULL)
         break;

      hold_all_sigs(); /* TODO too heavy, yet we may jump even here!? */
      f = fopen(v, "r"); /* TODO HISTFILE LOAD: use linebuf pool */
      if(f == NULL)
         goto jhist_done;
      (void)n_file_lock(fileno(f), FLT_READ, 0,0, UIZ_MAX);

      assert(!(pstate & PS_ROOT));
      pstate |= PS_ROOT; /* Allow calling addhist() */
      lbuf = NULL;
      lsize = 0;
      cnt = (size_t)fsize(f);
      while(fgetline(&lbuf, &lsize, &cnt, &llen, f, FAL0) != NULL){
         if(llen > 0 && lbuf[llen - 1] == '\n')
            lbuf[--llen] = '\0';
         if(llen == 0 || lbuf[0] == '#') /* xxx comments? noone! */
            continue;
         else{
            bool_t isgabby;

            isgabby = (lbuf[0] == '*');
            n_tty_addhist(lbuf + isgabby, isgabby);
         }
      }
      if(lbuf != NULL)
         free(lbuf);
      pstate &= ~PS_ROOT;

      fclose(f);
jhist_done:
      rele_all_sigs(); /* XXX remove jumps */
   }while(0);
# endif /* HAVE_HISTORY */

   /* Force immediate resolve for anything which follows */
   pstate |= PS_LINE_EDITOR_INIT;

# ifdef HAVE_KEY_BINDINGS
   /* `bind's (and `unbind's) done from within resource files couldn't be
    * performed for real since our termcap driver wasn't yet loaded, and we
    * can't perform automatic init since the user may have disallowed so */
   /* C99 */{
      struct a_tty_bind_ctx *tbcp;
      enum n_lexinput_flags lif;

      for(lif = 0; lif < n__LEXINPUT_CTX_MAX; ++lif)
         for(tbcp = a_tty.tg_bind[lif]; tbcp != NULL; tbcp = tbcp->tbc_next)
            if((tbcp->tbc_flags & (a_TTY_BIND_RESOLVE | a_TTY_BIND_DEFUNCT)) ==
                  a_TTY_BIND_RESOLVE)
               a_tty_bind_resolve(tbcp);
   }

   /* And we want to (try to) install some default key bindings */
   if(!ok_blook(line_editor_no_defaults)){
      char buf[8];
      struct a_tty_bind_parse_ctx tbpc;
      struct a_tty_bind_default_tuple const *tbdtp;

      buf[0] = '$', buf[1] = '\'', buf[2] = '\\', buf[3] = 'c',
         buf[5] = '\'', buf[6] = '\0';
      for(tbdtp = a_tty_bind_default_tuples;
            PTRCMP(tbdtp, <,
               &a_tty_bind_default_tuples[NELEM(a_tty_bind_default_tuples)]);
            ++tbdtp){
         memset(&tbpc, 0, sizeof tbpc);
         tbpc.tbpc_cmd = "bind";
         if(tbdtp->tbdt_iskey){
            buf[4] = tbdtp->tbdt_ckey;
            tbpc.tbpc_in_seq = buf;
         }else
            tbpc.tbpc_in_seq = savecatsep(":", '\0',
               n_termcap_name_of_query(tbdtp->tbdt_query));
         tbpc.tbpc_exp.s = UNCONST(tbdtp->tbdt_exp[0] == '\0'
               ? a_tty_bind_fun_names[(ui8_t)tbdtp->tbdt_exp[1]]
               : tbdtp->tbdt_exp);
         tbpc.tbpc_exp.l = strlen(tbpc.tbpc_exp.s);
         tbpc.tbpc_flags = n_LEXINPUT_CTX_BASE;
         /* ..but don't want to overwrite any user settings */
         a_tty_bind_create(&tbpc, FAL0);
      }
   }
# endif /* HAVE_KEY_BINDINGS */

jleave:
   NYD_LEAVE;
}

FL void
n_tty_destroy(void){
   NYD_ENTER;

   if(!(pstate & PS_LINE_EDITOR_INIT))
      goto jleave;

# ifdef HAVE_HISTORY
   do/* for break */{
      long hs;
      char const *v;
      struct a_tty_hist *thp;
      bool_t dogabby;
      FILE *f;

      a_TTY_HISTSIZE(hs);
      if(hs == 0)
         break;

      a_TTY_HISTFILE(v);
      if(v == NULL)
         break;

      dogabby = ok_blook(history_gabby_persist);

      if((thp = a_tty.tg_hist) != NULL)
         for(; thp->th_older != NULL; thp = thp->th_older)
            if((dogabby || !thp->th_isgabby) && --hs == 0)
               break;

      hold_all_sigs(); /* TODO too heavy, yet we may jump even here!? */
      f = fopen(v, "w"); /* TODO temporary + rename?! */
      if(f == NULL)
         goto jhist_done;
      (void)n_file_lock(fileno(f), FLT_WRITE, 0,0, UIZ_MAX);

      for(; thp != NULL; thp = thp->th_younger){
         if(dogabby || !thp->th_isgabby){
            if(thp->th_isgabby)
               putc('*', f);
            fwrite(thp->th_dat, sizeof *thp->th_dat, thp->th_len, f);
            putc('\n', f);
         }
      }
      fclose(f);
jhist_done:
      rele_all_sigs(); /* XXX remove jumps */
   }while(0);
# endif /* HAVE_HISTORY */

# if defined HAVE_KEY_BINDINGS && defined HAVE_DEBUG
   c_unbind(UNCONST("* *"));
# endif

# ifdef HAVE_DEBUG
   memset(&a_tty, 0, sizeof a_tty);
# endif
   DBG( pstate &= ~PS_LINE_EDITOR_INIT; )
jleave:
   NYD_LEAVE;
}

FL void
n_tty_signal(int sig){
   sigset_t nset, oset;
   NYD_X; /* Signal handler */

   switch(sig){
# ifdef SIGWINCH
   case SIGWINCH:
      /* We don't deal with SIGWINCH, yet get called from main.c.
       * Note this case might get called even if !PS_LINE_EDITOR_INIT */
      break;
# endif
   default:
      a_tty_term_mode(FAL0);
      n_TERMCAP_SUSPEND(TRU1);
      a_tty_sigs_down();

      sigemptyset(&nset);
      sigaddset(&nset, sig);
      sigprocmask(SIG_UNBLOCK, &nset, &oset);
      n_raise(sig);
      /* When we come here we'll continue editing, so reestablish */
      sigprocmask(SIG_BLOCK, &oset, (sigset_t*)NULL);

      a_tty_sigs_up();
      n_TERMCAP_RESUME(TRU1);
      a_tty_term_mode(TRU1);
      a_tty.tg_line->tl_vi_flags |= a_TTY_VF_MOD_DIRTY;
      break;
   }
}

FL int
(n_tty_readline)(enum n_lexinput_flags lif, char const *prompt,
      char **linebuf, size_t *linesize, size_t n SMALLOC_DEBUG_ARGS){
   struct a_tty_line tl;
# ifdef HAVE_COLOUR
   char *posbuf, *pos;
# endif
   ui32_t plen, pwidth;
   ssize_t nn;
   char const *orig_prompt;
   NYD_ENTER;
   UNUSED(lif);

   assert(!ok_blook(line_editor_disable));
   if(!(pstate & PS_LINE_EDITOR_INIT))
      n_tty_init();
   assert(pstate & PS_LINE_EDITOR_INIT);

   orig_prompt = prompt;
   /* xxx Likely overkill: avoid "bind base a,b,c set-line-editor-disable"
    * xxx not being honoured at once: call n_lex_input() instead of goto */
# ifndef HAVE_KEY_BINDINGS
jredo:
   prompt = orig_prompt;
# endif

# ifdef HAVE_COLOUR
   n_colour_env_create(n_COLOUR_CTX_MLE, FAL0);
# endif

   /* Classify prompt */
   UNINIT(plen, 0);
   UNINIT(pwidth, 0);
   if(prompt != NULL){
      size_t i = strlen(prompt);

      if(i == 0 || i >= UI32_MAX)
         prompt = NULL;
      else{
         /* TODO *prompt* is in multibyte and not in a_tty_cell, therefore
          * TODO we cannot handle it in parts, it's all or nothing.
          * TODO Later (S-CText, SysV signals) the prompt should be some global
          * TODO carrier thing, fully evaluated and passed around as UI-enabled
          * TODO string, then we can print it character by character */
         struct n_visual_info_ctx vic;

         memset(&vic, 0, sizeof vic);
         vic.vic_indat = prompt;
         vic.vic_inlen = i;
         if(n_visual_info(&vic, n_VISUAL_INFO_WIDTH_QUERY)){
            pwidth = (ui32_t)vic.vic_vi_width;
            plen = (ui32_t)i;
         }else{
            n_err(_("Character set error in evaluation of prompt\n"));
            prompt = NULL;
         }
      }
   }

# ifdef HAVE_COLOUR
   /* C99 */{
      struct n_colour_pen *ccp;
      struct str const *sp;

      if(prompt != NULL &&
            (ccp = n_colour_pen_create(n_COLOUR_ID_MLE_PROMPT, NULL)) != NULL &&
            (sp = n_colour_pen_to_str(ccp)) != NULL){
         char const *ccol = sp->s;

         if((sp = n_colour_reset_to_str()) != NULL){
            size_t l1 = strlen(ccol), l2 = strlen(sp->s);
            ui32_t nplen = (ui32_t)(l1 + plen + l2);
            char *nprompt = salloc(nplen +1);

            memcpy(nprompt, ccol, l1);
            memcpy(&nprompt[l1], prompt, plen);
            memcpy(&nprompt[l1 += plen], sp->s, ++l2);

            prompt = nprompt;
            plen = nplen;
         }
      }

      /* .tl_pos_buf is a hack */
      posbuf = pos = NULL;
      if((ccp = n_colour_pen_create(n_COLOUR_ID_MLE_POSITION, NULL)) != NULL &&
            (sp = n_colour_pen_to_str(ccp)) != NULL){
         char const *ccol = sp->s;

         if((sp = n_colour_reset_to_str()) != NULL){
            size_t l1 = strlen(ccol), l2 = strlen(sp->s);

            posbuf = salloc(l1 + 4 + l2 +1);
            memcpy(posbuf, ccol, l1);
            pos = &posbuf[l1];
            memcpy(&pos[4], sp->s, ++l2);
         }
      }
      if(posbuf == NULL){
         posbuf = pos = salloc(4 +1);
         pos[4] = '\0';
      }
   }
# endif /* HAVE_COLOUR */

   memset(&tl, 0, sizeof tl);

# ifdef HAVE_KEY_BINDINGS
   /* C99 */{
      char const *cp = ok_vlook(bind_timeout);

      if(cp != NULL){
         ul_i ul;

         if((ul = strtoul(cp, NULL, 0)) > 0 &&
               /* Convert to tenths of a second, unfortunately */
               (ul = (ul + 99) / 100) <= a_TTY_BIND_TIMEOUT_MAX)
            tl.tl_bind_timeout = (ui8_t)ul;
         else if(options & OPT_D_V)
            n_err(_("Ignoring invalid *bind-timeout*: %s\n"), cp);
      }
   }

   if(a_tty.tg_bind_isdirty)
      a_tty_bind_tree_teardown();
   if(a_tty.tg_bind_cnt > 0 && !a_tty.tg_bind_isbuild)
      a_tty_bind_tree_build();
   tl.tl_bind_tree_hmap = &a_tty.tg_bind_tree[lif & n__LEXINPUT_CTX_MASK];
   tl.tl_bind_shcut_cancel =
         &a_tty.tg_bind_shcut_cancel[lif & n__LEXINPUT_CTX_MASK];
   tl.tl_bind_shcut_prompt_char =
         &a_tty.tg_bind_shcut_prompt_char[lif & n__LEXINPUT_CTX_MASK];
# endif /* HAVE_KEY_BINDINGS */

   if((tl.tl_prompt = prompt) != NULL){ /* XXX not re-evaluated */
      tl.tl_prompt_length = plen;
      tl.tl_prompt_width = pwidth;
   }
# ifdef HAVE_COLOUR
   tl.tl_pos_buf = posbuf;
   tl.tl_pos = pos;
# endif

   tl.tl_line.cbuf = *linebuf;
   if(n != 0){
      tl.tl_defc.s = savestrbuf(*linebuf, n);
      tl.tl_defc.l = n;
   }
   tl.tl_x_buf = linebuf;
   tl.tl_x_bufsize = linesize;

   a_tty.tg_line = &tl;
   a_tty_sigs_up();
   a_tty_term_mode(TRU1);
   nn = a_tty_readline(&tl, n SMALLOC_DEBUG_ARGSCALL);
   a_tty_term_mode(FAL0);
   a_tty_sigs_down();
   a_tty.tg_line = NULL;

# ifdef HAVE_COLOUR
   n_colour_env_gut(stdout);
# endif

   if(tl.tl_reenter_after_cmd != NULL){
      n_source_command(lif, tl.tl_reenter_after_cmd);
      /* TODO because of recursion we cannot use srelax()ation: would be good */
      /* See above for why not simply using goto */
      n = (nn <= 0) ? 0 : nn;
# ifdef HAVE_KEY_BINDINGS
      nn = (n_lex_input)(lif, orig_prompt, linebuf, linesize,
            (n == 0 ? "" : savestrbuf(*linebuf, n)) SMALLOC_DEBUG_ARGSCALL);
# else
      goto jredo;
# endif
   }
   NYD_LEAVE;
   return (int)nn;
}

FL void
n_tty_addhist(char const *s, bool_t isgabby){
# ifdef HAVE_HISTORY
   /* Super-Heavy-Metal: block all sigs, avoid leaks+ on jump */
   ui32_t l;
   struct a_tty_hist *thp, *othp, *ythp;
# endif
   NYD_ENTER;
   UNUSED(s);
   UNUSED(isgabby);

# ifdef HAVE_HISTORY
   a_TTY_CHECK_ADDHIST(s, isgabby, goto j_leave);
   if(a_tty.tg_hist_size_max == 0)
      goto j_leave;

   l = (ui32_t)strlen(s);

   /* Eliminating duplicates is expensive, but simply inacceptable so
    * during the load of a potentially large history file! */
   if(pstate & PS_LINE_EDITOR_INIT)
      for(thp = a_tty.tg_hist; thp != NULL; thp = thp->th_older)
         if(thp->th_len == l && !strcmp(thp->th_dat, s)){
            hold_all_sigs(); /* TODO */
            if(thp->th_isgabby)
               thp->th_isgabby = !!isgabby;
            othp = thp->th_older;
            ythp = thp->th_younger;
            if(othp != NULL)
               othp->th_younger = ythp;
            else
               a_tty.tg_hist_tail = ythp;
            if(ythp != NULL)
               ythp->th_older = othp;
            else
               a_tty.tg_hist = othp;
            goto jleave;
         }
   hold_all_sigs();

   ++a_tty.tg_hist_size;
   if((pstate & PS_LINE_EDITOR_INIT) &&
         a_tty.tg_hist_size > a_tty.tg_hist_size_max){
      --a_tty.tg_hist_size;
      if((thp = a_tty.tg_hist_tail) != NULL){
         if((a_tty.tg_hist_tail = thp->th_younger) == NULL)
            a_tty.tg_hist = NULL;
         else
            a_tty.tg_hist_tail->th_older = NULL;
         free(thp);
      }
   }

   thp = smalloc((sizeof(struct a_tty_hist) -
         VFIELD_SIZEOF(struct a_tty_hist, th_dat)) + l +1);
   thp->th_isgabby = !!isgabby;
   thp->th_len = l;
   memcpy(thp->th_dat, s, l +1);
jleave:
   if((thp->th_older = a_tty.tg_hist) != NULL)
      a_tty.tg_hist->th_younger = thp;
   else
      a_tty.tg_hist_tail = thp;
   thp->th_younger = NULL;
   a_tty.tg_hist = thp;

   rele_all_sigs();
j_leave:
# endif
   NYD_LEAVE;
}

# ifdef HAVE_HISTORY
FL int
c_history(void *v){
   C_HISTORY_SHARED;

jlist:{
   FILE *fp;
   size_t i, b;
   struct a_tty_hist *thp;

   if(a_tty.tg_hist == NULL)
      goto jleave;

   if((fp = Ftmp(NULL, "hist", OF_RDWR | OF_UNLINK | OF_REGISTER)) == NULL){
      n_perr(_("tmpfile"), 0);
      v = NULL;
      goto jleave;
   }

   i = a_tty.tg_hist_size;
   b = 0;
   for(thp = a_tty.tg_hist; thp != NULL;
         --i, b += thp->th_len, thp = thp->th_older)
      fprintf(fp,
         "%c%4" PRIuZ ". %-50.50s (%4" PRIuZ "+%2" PRIu32 " B)\n",
         (thp->th_isgabby ? '*' : ' '), i, thp->th_dat, b, thp->th_len);

   page_or_print(fp, i);
   Fclose(fp);
   }
   goto jleave;

jclear:{
   struct a_tty_hist *thp;

   while((thp = a_tty.tg_hist) != NULL){
      a_tty.tg_hist = thp->th_older;
      free(thp);
   }
   a_tty.tg_hist_tail = NULL;
   a_tty.tg_hist_size = 0;
   }
   goto jleave;

jentry:{
   struct a_tty_hist *thp;

   if(UICMP(z, entry, <=, a_tty.tg_hist_size)){
      entry = (long)a_tty.tg_hist_size - entry;
      for(thp = a_tty.tg_hist;; thp = thp->th_older)
         if(thp == NULL)
            break;
         else if(entry-- != 0)
            continue;
         else{
            v = temporary_arg_v_store = thp->th_dat;
            goto jleave;
         }
   }
   v = NULL;
   }
   goto jleave;
}
# endif /* HAVE_HISTORY */

# ifdef HAVE_KEY_BINDINGS
FL int
c_bind(void *v){
   n_CMD_ARG_DESC_SUBCLASS_DEF(bind, 3, a_tty_bind_cad) { /* TODO cmd_tab.h */
      {n_CMD_ARG_DESC_STRING, 0},
      {n_CMD_ARG_DESC_WYSH | n_CMD_ARG_DESC_OPTION |
            n_CMD_ARG_DESC_HONOUR_STOP,
         n_SHEXP_PARSE_DRYRUN | n_SHEXP_PARSE_LOG},
      {n_CMD_ARG_DESC_WYSH | n_CMD_ARG_DESC_OPTION | n_CMD_ARG_DESC_GREEDY |
            n_CMD_ARG_DESC_HONOUR_STOP,
         n_SHEXP_PARSE_IGNORE_EMPTY}
   } n_CMD_ARG_DESC_SUBCLASS_DEF_END;
   struct n_cmd_arg_ctx cac;
   struct a_tty_bind_ctx *tbcp;
   enum n_lexinput_flags lif;
   bool_t aster, show;
   union {char const *cp; char *p; char c;} c;
   NYD_ENTER;

   cac.cac_desc = n_CMD_ARG_DESC_SUBCLASS_CAST(&a_tty_bind_cad);
   cac.cac_indat = v;
   cac.cac_inlen = UIZ_MAX;
   if(!n_cmd_arg_parse(&cac)){
      v = NULL;
      goto jleave;
   }

   c.cp = cac.cac_arg->ca_arg.ca_str.s;
   if(cac.cac_no == 1)
      show = TRU1;
   else
      show = !asccasecmp(cac.cac_arg->ca_next->ca_arg.ca_str.s, "show");
   aster = FAL0;

   if((lif = a_tty_bind_ctx_find(c.cp)) == (enum n_lexinput_flags)-1){
      if(!(aster = n_is_all_or_aster(c.cp)) || !show){
         n_err(_("`bind': invalid context: %s\n"), c.cp);
         v = NULL;
         goto jleave;
      }
      lif = 0;
   }

   if(show){
      ui32_t lns;
      FILE *fp;

      if((fp = Ftmp(NULL, "bind", OF_RDWR | OF_UNLINK | OF_REGISTER)) == NULL){
         n_perr(_("tmpfile"), 0);
         v = NULL;
         goto jleave;
      }

      lns = 0;
      for(;;){
         for(tbcp = a_tty.tg_bind[lif]; tbcp != NULL;
               ++lns, tbcp = tbcp->tbc_next){
            /* Print the bytes of resolved terminal capabilities, then */
            if((options & OPT_D_V) &&
                  (tbcp->tbc_flags & (a_TTY_BIND_RESOLVE | a_TTY_BIND_DEFUNCT)
                  ) == a_TTY_BIND_RESOLVE){
               char cbuf[8];
               union {wchar_t const *wp; char const *cp;} u;
               si32_t entlen;
               ui32_t cnvlen;
               char const *cnvdat, *bsep, *cbufp;

               putc('#', fp);
               putc(' ', fp);

               cbuf[0] = '=', cbuf[2] = '\0';
               for(cnvdat = tbcp->tbc_cnv, cnvlen = tbcp->tbc_cnv_len;
                     cnvlen > 0;){
                  if(cnvdat != tbcp->tbc_cnv)
                     putc(',', fp);

                  /* {si32_t buf_len_iscap;} */
                  entlen = *(si32_t const*)cnvdat;
                  if(entlen & SI32_MIN){
                     /* struct{si32_t buf_len_iscap; si32_t cap_len;
                      * char buf[]+NUL;} */
                     for(bsep = "",
                              u.cp = (char const*)&((si32_t const*)cnvdat)[2];
                           (c.c = *u.cp) != '\0'; ++u.cp){
                        if(asciichar(c.c) && !cntrlchar(c.c))
                           cbuf[1] = c.c, cbufp = cbuf;
                        else
                           cbufp = "";
                        fprintf(fp, "%s%02X%s",
                           bsep, (ui32_t)(ui8_t)c.c, cbufp);
                        bsep = " ";
                     }
                     entlen &= SI32_MAX;
                  }else
                     putc('-', fp);

                  cnvlen -= entlen;
                  cnvdat += entlen;
               }

               fputs("\n  ", fp);
               ++lns;
            }

            fprintf(fp, "%sbind %s %s %s%s%s\n",
               ((tbcp->tbc_flags & a_TTY_BIND_DEFUNCT)
               /* I18N: `bind' sequence not working, either because it is
                * I18N: using Unicode and that is not available in the locale,
                * I18N: or a termcap(5)/terminfo(5) sequence won't work out */
                  ? _("# <Defunctional> ") : ""),
               a_tty_bind_ctx_maps[lif].tbcm_name, tbcp->tbc_seq,
               n_shexp_quote_cp(tbcp->tbc_exp, TRU1),
               (tbcp->tbc_flags & a_TTY_BIND_NOCOMMIT ? "@" : ""),
               (!(options & OPT_D_VV) ? ""
                  : (tbcp->tbc_flags & a_TTY_BIND_FUN_INTERNAL
                     ? _(" # MLE internal") : ""))
               );
         }
         if(!aster || ++lif >= n__LEXINPUT_CTX_MAX)
            break;
      }
      page_or_print(fp, lns);

      Fclose(fp);
   }else{
      struct a_tty_bind_parse_ctx tbpc;
      struct n_string store;

      memset(&tbpc, 0, sizeof tbpc);
      tbpc.tbpc_cmd = a_tty_bind_cad.cad_name;
      tbpc.tbpc_in_seq = cac.cac_arg->ca_next->ca_arg.ca_str.s;
      tbpc.tbpc_exp.s = n_string_cp(n_cmd_arg_join_greedy(&cac,
            n_string_creat_auto(&store)));
      tbpc.tbpc_exp.l = store.s_len;
      tbpc.tbpc_flags = lif;
      if(!a_tty_bind_create(&tbpc, TRU1))
         v = NULL;
      n_string_gut(&store);
   }
jleave:
   NYD_LEAVE;
   return (v != NULL) ? EXIT_OK : EXIT_ERR;
}

FL int
c_unbind(void *v){
   n_CMD_ARG_DESC_SUBCLASS_DEF(unbind, 2, a_tty_unbind_cad) {/* TODO cmd_tab.h*/
      {n_CMD_ARG_DESC_STRING, 0},
      {n_CMD_ARG_DESC_WYSH | n_CMD_ARG_DESC_HONOUR_STOP,
         n_SHEXP_PARSE_DRYRUN | n_SHEXP_PARSE_LOG}
   } n_CMD_ARG_DESC_SUBCLASS_DEF_END;
   struct a_tty_bind_parse_ctx tbpc;
   struct n_cmd_arg_ctx cac;
   struct a_tty_bind_ctx *tbcp;
   enum n_lexinput_flags lif;
   bool_t aster;
   union {char const *cp; char *p;} c;
   NYD_ENTER;

   cac.cac_desc = n_CMD_ARG_DESC_SUBCLASS_CAST(&a_tty_unbind_cad);
   cac.cac_indat = v;
   cac.cac_inlen = UIZ_MAX;
   if(!n_cmd_arg_parse(&cac)){
      v = NULL;
      goto jleave;
   }

   c.cp = cac.cac_arg->ca_arg.ca_str.s;
   aster = FAL0;

   if((lif = a_tty_bind_ctx_find(c.cp)) == (enum n_lexinput_flags)-1){
      if(!(aster = n_is_all_or_aster(c.cp))){
         n_err(_("`unbind': invalid context: %s\n"), c.cp);
         v = NULL;
         goto jleave;
      }
      lif = 0;
   }

   c.cp = cac.cac_arg->ca_next->ca_arg.ca_str.s;
jredo:
   if(n_is_all_or_aster(c.cp)){
      while((tbcp = a_tty.tg_bind[lif]) != NULL){
         memset(&tbpc, 0, sizeof tbpc);
         tbpc.tbpc_tbcp = tbcp;
         tbpc.tbpc_flags = lif;
         a_tty_bind_del(&tbpc);
      }
   }else{
      memset(&tbpc, 0, sizeof tbpc);
      tbpc.tbpc_cmd = a_tty_unbind_cad.cad_name;
      tbpc.tbpc_in_seq = c.cp;
      tbpc.tbpc_flags = lif;

      if(UNLIKELY(!a_tty_bind_parse(FAL0, &tbpc)))
         v = NULL;
      else if(UNLIKELY((tbcp = tbpc.tbpc_tbcp) == NULL)){
         n_err(_("`unbind': no such `bind'ing: %s  %s\n"),
            a_tty_bind_ctx_maps[lif].tbcm_name, c.cp);
         v = NULL;
      }else
         a_tty_bind_del(&tbpc);
   }

   if(aster && ++lif < n__LEXINPUT_CTX_MAX)
      goto jredo;
jleave:
   NYD_LEAVE;
   return (v != NULL) ? EXIT_OK : EXIT_ERR;
}
# endif /* HAVE_KEY_BINDINGS */

#else /* HAVE_MLE */
/*
 * The really-nothing-at-all implementation
 */

FL void
n_tty_init(void){
   NYD_ENTER;
   NYD_LEAVE;
}

FL void
n_tty_destroy(void){
   NYD_ENTER;
   NYD_LEAVE;
}

FL void
n_tty_signal(int sig){
   NYD_X; /* Signal handler */
   UNUSED(sig);

# ifdef HAVE_TERMCAP
   switch(sig){
   default:{
      sigset_t nset, oset;

      n_TERMCAP_SUSPEND(TRU1);
      a_tty_sigs_down();

      sigemptyset(&nset);
      sigaddset(&nset, sig);
      sigprocmask(SIG_UNBLOCK, &nset, &oset);
      n_raise(sig);
      /* When we come here we'll continue editing, so reestablish */
      sigprocmask(SIG_BLOCK, &oset, (sigset_t*)NULL);

      a_tty_sigs_up();
      n_TERMCAP_RESUME(TRU1);
      break;
   }
   }
# endif /* HAVE_TERMCAP */
}

FL int
(n_tty_readline)(char const *prompt, char **linebuf, size_t *linesize, size_t n
      SMALLOC_DEBUG_ARGS){
   int rv;
   NYD_ENTER;

   if(prompt != NULL){
      if(*prompt != '\0')
         fputs(prompt, stdout);
      fflush(stdout);
   }
# ifdef HAVE_TERMCAP
   a_tty_sigs_up();
# endif
   rv = (readline_restart)(stdin, linebuf, linesize,n SMALLOC_DEBUG_ARGSCALL);
# ifdef HAVE_TERMCAP
   a_tty_sigs_down();
# endif
   NYD_LEAVE;
   return rv;
}

FL void
n_tty_addhist(char const *s, bool_t isgabby){
   NYD_ENTER;
   UNUSED(s);
   UNUSED(isgabby);
   NYD_LEAVE;
}
#endif /* nothing at all */

#undef a_TTY_SIGNALS
/* s-it-mode */
