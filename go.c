/*@ S-nail - a mail user agent derived from Berkeley Mail.
 *@ Program input of all sorts, input lexing, event loops, command evaluation.
 *@ TODO n_PS_ROBOT requires yet n_PS_SOURCING, which REALLY sucks.
 *
 * Copyright (c) 2000-2004 Gunnar Ritter, Freiburg i. Br., Germany.
 * Copyright (c) 2012 - 2017 Steffen (Daode) Nurpmeso <steffen@sdaoden.eu>.
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
#define n_FILE go

#ifndef HAVE_AMALGAMATION
# include "nail.h"
#endif

enum a_go_flags{
   a_GO_NONE,
   a_GO_FREE = 1u<<0,         /* Structure was allocated, n_free() it */
   a_GO_PIPE = 1u<<1,         /* Open on a pipe */
   a_GO_FILE = 1u<<2,         /* Loading or sourcing a file */
   a_GO_MACRO = 1u<<3,        /* Running a macro */
   a_GO_MACRO_FREE_DATA = 1u<<4, /* Lines are allocated, n_free() once done */
   /* TODO For simplicity this is yet _MACRO plus specialization overlay
    * TODO (_X_OPTION, _CMD) -- these should be types on their own! */
   a_GO_MACRO_X_OPTION = 1u<<5, /* Macro indeed command line -X option */
   a_GO_MACRO_CMD = 1u<<6,    /* Macro indeed single-line: ~:COMMAND */
   /* TODO a_GO_SPLICE: the right way to support *on-compose-splice(-shell)?*
    * TODO would be a command_loop object that emits an on_read_line event, and
    * TODO have a special handler for the compose mode; with that, then,
    * TODO _event_loop() would not call _evaluate() but CTX->on_read_line,
    * TODO and _evaluate() would be the standard impl.,
    * TODO whereas the COMMAND ESCAPE switch in collect.c would be another one.
    * TODO With this generic accmacvar.c:temporary_compose_mode_hook_call()
    * TODO could be dropped, and n_go_macro() could become extended,
    * TODO and/or we would add a n_go_anything(), which would allow special
    * TODO input handlers, special I/O input and output, special `localopts'
    * TODO etc., to be glued to the new execution context.  And all I/O all
    * TODO over this software should not use stdin/stdout, but CTX->in/out.
    * TODO The pstate must be a property of the current execution context, too.
    * TODO This not today. :(  For now we invent a special SPLICE execution
    * TODO context overlay that at least allows to temporarily modify the
    * TODO global pstate, and the global stdin and stdout pointers.  HACK!
    * TODO This splice thing is very special and has to go again.  HACK!!
    * TODO a_go_input() will drop it once it sees EOF (HACK!), but care for
    * TODO jumps must be taken by splice creators.  HACK!!!  But works. ;} */
   a_GO_SPLICE = 1u<<7,
   /* If it is none of those, it must be the outermost, the global one */
   a_GO_TYPE_MASK = a_GO_PIPE | a_GO_FILE | a_GO_MACRO |
         /* a_GO_MACRO_X_OPTION | a_GO_MACRO_CMD | */ a_GO_SPLICE,

   a_GO_FORCE_EOF = 1u<<8,    /* go_input() shall return EOF next */
   a_GO_IS_EOF = 1u<<9,

   a_GO_SUPER_MACRO = 1u<<16, /* *Not* inheriting n_PS_SOURCING state */
   /* This context has inherited the memory pool from its parent.
    * In practice only used for resource file loading and -X args, which enter
    * a top level n_go_main_loop() and should (re)use the in practice already
    * allocated memory pool of the global context */
   a_GO_MEMPOOL_INHERITED = 1u<<17,

   a_GO_XCALL_IS_CALL = 1u<<24,  /* n_GO_INPUT_NO_XCALL */
   /* `xcall' optimization barrier: n_go_macro() has been finished with
    * a `xcall' request, and `xcall' set this in the parent a_go_input of the
    * said n_go_macro() to indicate a barrier: we teardown the a_go_input of
    * the n_go_macro() away after leaving its _event_loop(), but then,
    * back in n_go_macro(), that enters a for(;;) loop that directly calls
    * c_call() -- our `xcall' stack avoidance optimization --, yet this call
    * will itself end up in a new n_go_macro(), and if that again ends up with
    * `xcall' this should teardown and leave its own n_go_macro(), unrolling the
    * stack "up to the barrier level", but which effectively still is the
    * n_go_macro() that lost its a_go_input and is looping the `xcall'
    * optimization loop.  If no `xcall' is desired that loop is simply left and
    * the _event_loop() of the outer a_go_ctx will perform a loop tick and
    * clear this bit again OR become teardown itself */
   a_GO_XCALL_LOOP = 1u<<25,  /* `xcall' optimization barrier level */
   a_GO_XCALL_LOOP_ERROR = 1u<<26, /* .. state machine error transporter */
   a_GO_XCALL_LOOP_MASK = a_GO_XCALL_LOOP | a_GO_XCALL_LOOP_ERROR
};

enum a_go_cleanup_mode{
   a_GO_CLEANUP_UNWIND = 1u<<0,     /* Teardown all contexts except outermost */
   a_GO_CLEANUP_TEARDOWN = 1u<<1,   /* Teardown current context */
   a_GO_CLEANUP_LOOPTICK = 1u<<2,   /* Normal looptick cleanup */
   a_GO_CLEANUP_MODE_MASK = n_BITENUM_MASK(0, 2),

   a_GO_CLEANUP_ERROR = 1u<<8,      /* Error occurred on level */
   a_GO_CLEANUP_SIGINT = 1u<<9,     /* Interrupt signal received */
   a_GO_CLEANUP_HOLDALLSIGS = 1u<<10 /* hold_all_sigs() active TODO */
};

enum a_go_hist_flags{
   a_GO_HIST_NONE = 0,
   a_GO_HIST_ADD = 1u<<0,
   a_GO_HIST_GABBY = 1u<<1,
   a_GO_HIST_INIT = 1u<<2
};

struct a_go_eval_ctx{
   struct str gec_line;    /* The terminated data to _evaluate() */
   ui32_t gec_line_size;   /* May be used to store line memory size */
   bool_t gec_ever_seen;   /* Has ever been used (main_loop() only) */
   ui8_t gec__dummy[2];
   ui8_t gec_hist_flags;   /* enum a_go_hist_flags */
   char const *gec_hist_cmd; /* If a_GO_HIST_ADD only, cmd and args */
   char const *gec_hist_args;
};

struct a_go_input_inject{
   struct a_go_input_inject *gii_next;
   size_t gii_len;
   bool_t gii_commit;
   bool_t gii_no_history;
   char gii_dat[n_VFIELD_SIZE(6)];
};

struct a_go_ctx{
   struct a_go_ctx *gc_outer;
   sigset_t gc_osigmask;
   ui32_t gc_flags;           /* enum a_go_flags */
   ui32_t gc_loff;            /* Pseudo (macro): index in .gc_lines */
   char **gc_lines;           /* Pseudo content, lines unfolded */
   FILE *gc_file;             /* File we were in, if applicable */
   struct a_go_input_inject *gc_inject; /* To be consumed first */
   void (*gc_on_finalize)(void *);
   void *gc_finalize_arg;
   sigjmp_buf gc_eloop_jmp;   /* TODO one day...  for _event_loop() */
   /* SPLICE hacks: saved stdin/stdout, saved pstate */
   FILE *gc_splice_stdin;
   FILE *gc_splice_stdout;
   ui32_t gc_splice_psonce;
   ui8_t gc_splice__dummy[4];
   struct n_go_data_ctx gc_data;
   char gc_name[n_VFIELD_SIZE(0)]; /* Name of file or macro */
};

struct a_go_readctl_ctx{ /* TODO localize n_readctl_overlay, use OnForkEvent! */
   struct a_go_readctl_ctx *grc_last;
   struct a_go_readctl_ctx *grc_next;
   char const *grc_expand;          /* If filename based, expanded string */
   FILE *grc_fp;
   si32_t grc_fd;                   /* Based upon file-descriptor */
   char grc_name[n_VFIELD_SIZE(4)]; /* User input for identification purposes */
};

static sighandler_type a_go_oldpipe;
/* a_go_cmd_tab[] after fun protos */

/* Our current execution context, and the buffer backing the outermost level */
static struct a_go_ctx *a_go_ctx;

#define a_GO_MAINCTX_NAME "Main event loop"
static union{
   ui64_t align;
   char uf[n_VSTRUCT_SIZEOF(struct a_go_ctx, gc_name) +
         sizeof(a_GO_MAINCTX_NAME)];
} a_go__mainctx_b;

/* `xcall' stack-avoidance bypass optimization.  This actually is
 * a n_cmd_arg_save_to_heap() buffer with n_cmd_arg_ctx.cac_indat misused to
 * point to the a_go_ctx to unroll up to */
static void *a_go_xcall;

static sigjmp_buf a_go_srbuf; /* TODO GET RID */

/* n_PS_STATE_PENDMASK requires some actions */
static void a_go_update_pstate(void);

/* Evaluate a single command */
static bool_t a_go_evaluate(struct a_go_eval_ctx *gecp);

/* Branch here on hangup signal and simulate "exit" */
static void a_go_hangup(int s);

/* The following gets called on receipt of an interrupt */
static void a_go_onintr(int s);

/* Cleanup current execution context, update the program state.
 * If _CLEANUP_ERROR is set then we don't alert and error out if the stack
 * doesn't exist at all, unless _CLEANUP_HOLDALLSIGS we hold_all_sigs() */
static void a_go_cleanup(enum a_go_cleanup_mode gcm);

/* `source' and `source_if' (if silent_open_error: no pipes allowed, then).
 * Returns FAL0 if file is somehow not usable (unless silent_open_error) or
 * upon evaluation error, and TRU1 on success */
static bool_t a_go_file(char const *file, bool_t silent_open_error);

/* System resource file load()ing or -X command line option array traversal */
static bool_t a_go_load(struct a_go_ctx *gcp);

/* A simplified command loop for recursed state machines */
static bool_t a_go_event_loop(struct a_go_ctx *gcp, enum n_go_input_flags gif);

static void
a_go_update_pstate(void){
   bool_t act;
   NYD_ENTER;

   act = ((n_pstate & n_PS_SIGWINCH_PEND) != 0);
   n_pstate &= ~n_PS_PSTATE_PENDMASK;

   if(act){
      char buf[32];

      snprintf(buf, sizeof buf, "%d", n_scrnwidth);
      ok_vset(COLUMNS, buf);
      snprintf(buf, sizeof buf, "%d", n_scrnheight);
      ok_vset(LINES, buf);
   }
   NYD_LEAVE;
}

static bool_t
a_go_evaluate(struct a_go_eval_ctx *gecp){
   /* xxx old style(9), but also old code */
   /* TODO a_go_evaluate() should be splitted in multiple subfunctions,
    * TODO `eval' should be a prefix, etc., a Ctx should be passed along */
   struct str line;
   struct n_string s, *sp;
   struct str const *alias_exp;
   char _wordbuf[2], **arglist_base/*[n_MAXARGC]*/, **arglist, *cp, *word;
   char const *alias_name;
   struct n_cmd_desc const *cdp;
   si32_t nerrn, nexn;     /* TODO n_pstate_ex_no -> si64_t! */
   int rv, c;
   enum{
      a_NONE = 0,
      a_ALIAS_MASK = n_BITENUM_MASK(0, 2), /* Alias recursion counter bits */
      a_NOPREFIX = 1u<<4,  /* Modifier prefix not allowed right now */
      a_NOALIAS = 1u<<5,   /* "No alias!" expansion modifier */
      /* New modifier prefixes must be reflected in a_go_c_alias()! */
      a_IGNERR = 1u<<6,    /* ignerr modifier prefix */
      a_WYSH = 1u<<7,      /* XXX v15+ drop wysh modifier prefix */
      a_VPUT = 1u<<8,      /* vput modifier prefix */
      a_MODE_MASK = n_BITENUM_MASK(5, 8),
      a_NO_ERRNO = 1u<<16  /* Don't set n_pstate_err_no */
   } flags;
   NYD_ENTER;

   n_exit_status = n_EXIT_OK;

   flags = a_NONE;
   rv = 1;
   nerrn = n_ERR_NONE;
   nexn = n_EXIT_OK;
   cdp = NULL;
   alias_name = NULL;
   n_UNINIT(alias_exp, NULL);
   arglist =
   arglist_base = n_autorec_alloc(sizeof(*arglist_base) * n_MAXARGC);
   line = gecp->gec_line; /* TODO const-ify original (buffer)! */
   assert(line.s[line.l] == '\0');

   if(line.l > 0 && spacechar(line.s[0]))
      gecp->gec_hist_flags = a_GO_HIST_NONE;
   else if(gecp->gec_hist_flags & a_GO_HIST_ADD)
      gecp->gec_hist_cmd = gecp->gec_hist_args = NULL;
   sp = NULL;

   /* Aliases that refer to shell commands or macro expansion restart */
jrestart:
   if(n_str_trim_ifs(&line, TRU1)->l == 0){
      line.s[0] = '\0';
      gecp->gec_hist_flags = a_GO_HIST_NONE;
      goto jempty;
   }
   (cp = line.s)[line.l] = '\0';

   /* No-expansion modifier? */
   if(!(flags & a_NOPREFIX) && *cp == '\\'){
      line.s = ++cp;
      --line.l;
      flags |= a_NOALIAS;
   }

   /* Note: adding more special treatments must be reflected in the `help' etc.
    * output in cmd-tab.c! */

   /* Ignore null commands (comments) */
   if(*cp == '#'){
      gecp->gec_hist_flags = a_GO_HIST_NONE;
      goto jret0;
   }

   /* Handle ! differently to get the correct lexical conventions */
   if(*cp == '!')
      ++cp;
   /* Isolate the actual command; since it may not necessarily be
    * separated from the arguments (as in `p1') we need to duplicate it to
    * be able to create a NUL terminated version.
    * We must be aware of several special one letter commands here */
   else if((cp = n_UNCONST(n_cmd_isolate(cp))) == line.s &&
         (*cp == '|' || *cp == '?'))
      ++cp;
   c = (int)PTR2SIZE(cp - line.s);
   word = UICMP(z, c, <, sizeof _wordbuf) ? _wordbuf : n_autorec_alloc(c +1);
   memcpy(word, arglist[0] = line.s, c);
   word[c] = '\0';
   line.l -= c;
   line.s = cp;

   /* It may be a modifier.
    * Note: adding modifiers must be reflected in commandalias handling code */
   if(c == sizeof("ignerr") -1 && !asccasecmp(word, "ignerr")){
      flags |= a_NOPREFIX | a_IGNERR;
      goto jrestart;
   }else if(c == sizeof("wysh") -1 && !asccasecmp(word, "wysh")){
      flags |= a_NOPREFIX | a_WYSH;
      goto jrestart;
   }else if(c == sizeof("vput") -1 && !asccasecmp(word, "vput")){
      flags |= a_NOPREFIX | a_VPUT;
      goto jrestart;
   }

   /* We need to trim for a possible history entry, but do it anyway and insert
    * a space for argument separation in case of alias expansion.  Also, do
    * terminate again because nothing prevents aliases from introducing WS */
   n_str_trim_ifs(&line, TRU1);
   line.s[line.l] = '\0';

   /* Lengthy history entry setup, possibly even redundant.  But having
    * normalized history entries is a good thing, and this is maybe still
    * cheaper than parsing a StrList of words per se */
   if((gecp->gec_hist_flags & (a_GO_HIST_ADD | a_GO_HIST_INIT)
         ) == a_GO_HIST_ADD){
      if(line.l > 0){
         sp = n_string_creat_auto(&s);
         sp = n_string_assign_buf(sp, line.s, line.l);
         gecp->gec_hist_args = n_string_cp(sp);
      }

      sp = n_string_creat_auto(&s);
      sp = n_string_reserve(sp, 32);

      if(flags & a_NOALIAS)
         sp = n_string_push_c(sp, '\\');
      if(flags & a_IGNERR)
         sp = n_string_push_buf(sp, "ignerr ", sizeof("ignerr ") -1);
      if(flags & a_WYSH)
         sp = n_string_push_buf(sp, "wysh ", sizeof("wysh ") -1);
      if(flags & a_VPUT)
         sp = n_string_push_buf(sp, "vput ", sizeof("vput ") -1);
      gecp->gec_hist_flags = a_GO_HIST_ADD | a_GO_HIST_INIT;
   }

   /* Look up the command; if not found, bitch.
    * Normally, a blank command would map to the first command in the
    * table; while n_PS_SOURCING, however, we ignore blank lines to eliminate
    * confusion; act just the same for aliases */
   if(*word == '\0'){
jempty:
      if((n_pstate & n_PS_ROBOT) || !(n_psonce & n_PSO_INTERACTIVE) ||
            alias_name != NULL){
         gecp->gec_hist_flags = a_GO_HIST_NONE;
         goto jret0;
      }
      cdp = n_cmd_default();
      goto jexec;
   }

   if(!(flags & a_NOALIAS) && (flags & a_ALIAS_MASK) != a_ALIAS_MASK){
      ui8_t expcnt;

      expcnt = (flags & a_ALIAS_MASK);
      ++expcnt;
      flags = (flags & ~(a_ALIAS_MASK | a_NOPREFIX)) | expcnt;

      /* Avoid self-recursion; yes, the user could use \ no-expansion, but.. */
      if(alias_name != NULL && !strcmp(word, alias_name)){
         if(n_poption & n_PO_D_V)
            n_err(_("Actively avoiding self-recursion of `commandalias': %s\n"),
               word);
      }else if((alias_name = n_commandalias_exists(word, &alias_exp)) != NULL){
         size_t i;

         if(sp != NULL){
            sp = n_string_push_cp(sp, word);
            gecp->gec_hist_cmd = n_string_cp(sp);
            sp = NULL;
         }

         /* And join arguments onto alias expansion */
         alias_name = word;
         i = alias_exp->l;
         cp = line.s;
         line.s = n_autorec_alloc(i + 1 + line.l +1);
         memcpy(line.s, alias_exp->s, i);
         if(line.l > 0){
            line.s[i++] = ' ';
            memcpy(&line.s[i], cp, line.l);
         }
         line.s[i += line.l] = '\0';
         line.l = i;
         goto jrestart;
      }
   }

   if((cdp = n_cmd_firstfit(word)) == NULL){
      bool_t doskip;

      if(!(doskip = n_cnd_if_isskip()) || (n_poption & n_PO_D_V))
         n_err(_("Unknown command%s: `%s'\n"),
            (doskip ? _(" (ignored due to `if' condition)") : n_empty),
            prstr(word));
      gecp->gec_hist_flags = a_GO_HIST_NONE;
      if(doskip)
         goto jret0;
      nerrn = n_ERR_NOSYS;
      goto jleave;
   }

   /* See if we should execute the command -- if a conditional we always
    * execute it, otherwise, check the state of cond */
jexec:
   if(!(cdp->cd_caflags & n_CMD_ARG_F) && n_cnd_if_isskip()){
      gecp->gec_hist_flags = a_GO_HIST_NONE;
      goto jret0;
   }

   if(sp != NULL){
      sp = n_string_push_cp(sp, cdp->cd_name);
      gecp->gec_hist_cmd = n_string_cp(sp);
      sp = NULL;
   }

   nerrn = n_ERR_INVAL;

   /* Process the arguments to the command, depending on the type it expects */
   if((cdp->cd_caflags & n_CMD_ARG_I) && !(n_psonce & n_PSO_INTERACTIVE) &&
         !(n_poption & n_PO_BATCH_FLAG)){
      n_err(_("May not execute `%s' unless interactive or in batch mode\n"),
         cdp->cd_name);
      goto jleave;
   }
   if(!(cdp->cd_caflags & n_CMD_ARG_M) && (n_psonce & n_PSO_SENDMODE)){
      n_err(_("May not execute `%s' while sending\n"), cdp->cd_name);
      goto jleave;
   }
   if(cdp->cd_caflags & n_CMD_ARG_R){
      if(n_pstate & n_PS_COMPOSE_MODE){
         /* TODO n_PS_COMPOSE_MODE: should allow `reply': ~:reply! */
         n_err(_("Cannot invoke `%s' when in compose mode\n"), cdp->cd_name);
         goto jleave;
      }
      /* TODO Nothing should prevent n_CMD_ARG_R in conjunction with
       * TODO n_PS_ROBOT|_SOURCING; see a.._may_yield_control()! */
      if(n_pstate & (n_PS_ROBOT | n_PS_SOURCING) && n_go_may_yield_control()){
         n_err(_("Cannot invoke `%s' from a macro or during file inclusion\n"),
            cdp->cd_name);
         goto jleave;
      }
   }
   if((cdp->cd_caflags & n_CMD_ARG_S) && !(n_psonce & n_PSO_STARTED)){
      n_err(_("May not execute `%s' during startup\n"), cdp->cd_name);
      goto jleave;
   }
   if(!(cdp->cd_caflags & n_CMD_ARG_X) && (n_pstate & n_PS_COMPOSE_FORKHOOK)){
      n_err(_("Cannot invoke `%s' from a hook running in a child process\n"),
         cdp->cd_name);
      goto jleave;
   }

   if((cdp->cd_caflags & n_CMD_ARG_A) && mb.mb_type == MB_VOID){
      n_err(_("Cannot execute `%s' without active mailbox\n"), cdp->cd_name);
      goto jleave;
   }
   if((cdp->cd_caflags & n_CMD_ARG_W) && !(mb.mb_perm & MB_DELE)){
      n_err(_("May not execute `%s' -- message file is read only\n"),
         cdp->cd_name);
      goto jleave;
   }

   if(cdp->cd_caflags & n_CMD_ARG_O)
      n_OBSOLETE2(_("this command will be removed"), cdp->cd_name);

   /* TODO v15: strip n_PS_ARGLIST_MASK off, just in case the actual command
    * TODO doesn't use any of those list commands which strip this mask,
    * TODO and for now we misuse bits for checking relation to history;
    * TODO argument state should be property of a per-command carrier instead */
   n_pstate &= ~n_PS_ARGLIST_MASK;

   if((flags & a_WYSH) &&
         (cdp->cd_caflags & n_CMD_ARG_TYPE_MASK) != n_CMD_ARG_TYPE_WYRA){
      n_err(_("`wysh' prefix does not affect `%s'\n"), cdp->cd_name);
      flags &= ~a_WYSH;
   }

   if(flags & a_VPUT){
      if(cdp->cd_caflags & n_CMD_ARG_V){
         char const *emsg;

         emsg = line.s; /* xxx Cannot pass &char* as char const**, so no cp */
         arglist[0] = n_shexp_parse_token_cp((n_SHEXP_PARSE_TRIM_SPACE |
               n_SHEXP_PARSE_TRIM_IFSSPACE | n_SHEXP_PARSE_LOG |
               n_SHEXP_PARSE_META_KEEP), &emsg);
         line.l -= PTR2SIZE(emsg - line.s);
         line.s = cp = n_UNCONST(emsg);
         if(cp == NULL)
            emsg = N_("could not parse input token");
         else if(!n_shexp_is_valid_varname(arglist[0]))
            emsg = N_("not a valid variable name");
         else if(!n_var_is_user_writable(arglist[0]))
            emsg = N_("either not a user writable, or a boolean variable");
         else
            emsg = NULL;
         if(emsg != NULL){
            n_err("`%s': vput: %s: %s\n",
                  cdp->cd_name, V_(emsg), n_shexp_quote_cp(arglist[0], FAL0));
            nerrn = n_ERR_NOTSUP;
            rv = -1;
            goto jleave;
         }
         ++arglist;
         n_pstate |= n_PS_ARGMOD_VPUT; /* TODO YET useless since stripped later
         * TODO on in getrawlist() etc., i.e., the argument vector producers,
         * TODO therefore yet needs to be set again based on flags&a_VPUT! */
      }else{
         n_err(_("`vput' prefix does not affect `%s'\n"), cdp->cd_name);
         flags &= ~a_VPUT;
      }
   }

   switch(cdp->cd_caflags & n_CMD_ARG_TYPE_MASK){
   case n_CMD_ARG_TYPE_MSGLIST:
      /* Message list defaulting to nearest forward legal message */
      if(n_msgvec == NULL)
         goto jemsglist;
      if((c = getmsglist(line.s, n_msgvec, cdp->cd_msgflag)) < 0){
         nerrn = n_ERR_NOMSG;
         flags |= a_NO_ERRNO;
         break;
      }
      if(c == 0){
         if((n_msgvec[0] = first(cdp->cd_msgflag, cdp->cd_msgmask)) != 0)
            n_msgvec[1] = 0;
      }
      if(n_msgvec[0] == 0){
jemsglist:
         if(!(n_pstate & n_PS_HOOK_MASK))
            fprintf(n_stdout, _("No applicable messages\n"));
         nerrn = n_ERR_NOMSG;
         flags |= a_NO_ERRNO;
         break;
      }
      if(!(flags & a_NO_ERRNO) && !(cdp->cd_caflags & n_CMD_ARG_EM)) /* XXX */
         n_err_no = 0;
      rv = (*cdp->cd_func)(n_msgvec);
      break;

   case n_CMD_ARG_TYPE_NDMLIST:
      /* Message list with no defaults, but no error if none exist */
      if(n_msgvec == NULL)
         goto jemsglist;
      if((c = getmsglist(line.s, n_msgvec, cdp->cd_msgflag)) < 0){
         nerrn = n_ERR_NOMSG;
         flags |= a_NO_ERRNO;
         break;
      }
      if(!(flags & a_NO_ERRNO) && !(cdp->cd_caflags & n_CMD_ARG_EM)) /* XXX */
         n_err_no = 0;
      rv = (*cdp->cd_func)(n_msgvec);
      break;

   case n_CMD_ARG_TYPE_STRING:
      /* Just the straight string, old style, with leading blanks removed */
      for(cp = line.s; spacechar(*cp);)
         ++cp;
      if(!(flags & a_NO_ERRNO) && !(cdp->cd_caflags & n_CMD_ARG_EM)) /* XXX */
         n_err_no = 0;
      rv = (*cdp->cd_func)(cp);
      break;
   case n_CMD_ARG_TYPE_RAWDAT:
      /* Just the straight string, placed in argv[] */
      *arglist++ = line.s;
      *arglist = NULL;
      if(!(flags & a_NO_ERRNO) && !(cdp->cd_caflags & n_CMD_ARG_EM)) /* XXX */
         n_err_no = 0;
      rv = (*cdp->cd_func)(arglist_base);
      break;

   case n_CMD_ARG_TYPE_WYSH:
      c = 1;
      if(0){
         /* FALLTHRU */
   case n_CMD_ARG_TYPE_WYRA:
         c = (flags & a_WYSH) ? 1 : 0;
         if(0){
   case n_CMD_ARG_TYPE_RAWLIST:
            c = 0;
         }
      }
      if((c = getrawlist((c != 0), arglist,
            n_MAXARGC - PTR2SIZE(arglist - arglist_base), line.s, line.l)) < 0){
         n_err(_("Invalid argument list\n"));
         flags |= a_NO_ERRNO;
         break;
      }

      if(c < cdp->cd_minargs){
         n_err(_("`%s' requires at least %u arg(s)\n"),
            cdp->cd_name, (ui32_t)cdp->cd_minargs);
         flags |= a_NO_ERRNO;
         break;
      }
#undef cd_minargs
      if(c > cdp->cd_maxargs){
         n_err(_("`%s' takes no more than %u arg(s)\n"),
            cdp->cd_name, (ui32_t)cdp->cd_maxargs);
         flags |= a_NO_ERRNO;
         break;
      }
#undef cd_maxargs

      if(flags & a_VPUT)
         n_pstate |= n_PS_ARGMOD_VPUT;

      if(!(flags & a_NO_ERRNO) && !(cdp->cd_caflags & n_CMD_ARG_EM)) /* XXX */
         n_err_no = 0;
      rv = (*cdp->cd_func)(arglist_base);
      if(a_go_xcall != NULL)
         goto jret0;
      break;

   case n_CMD_ARG_TYPE_ARG:{
      /* TODO The _ARG_TYPE_ARG is preliminary, in the end we should have a
       * TODO per command-ctx carrier that also has slots for it arguments,
       * TODO and that should be passed along all the way.  No more arglists
       * TODO here, etc. */
      struct n_cmd_arg_ctx cac;

      cac.cac_desc = cdp->cd_cadp;
      cac.cac_indat = line.s;
      cac.cac_inlen = line.l;
      if(!n_cmd_arg_parse(&cac)){
         flags |= a_NO_ERRNO;
         break;
      }

      if(flags & a_VPUT){
         cac.cac_vput = *arglist_base;
         n_pstate |= n_PS_ARGMOD_VPUT;
      }else
         cac.cac_vput = NULL;

      if(!(flags & a_NO_ERRNO) && !(cdp->cd_caflags & n_CMD_ARG_EM)) /* XXX */
         n_err_no = 0;
      rv = (*cdp->cd_func)(&cac);
      if(a_go_xcall != NULL)
         goto jret0;
   }  break;

   default:
      DBG( n_panic(_("Implementation error: unknown argument type: %d"),
         cdp->cd_caflags & n_CMD_ARG_TYPE_MASK); )
      nerrn = n_ERR_NOTOBACCO;
      nexn = 1;
      goto jret0;
   }

   if(gecp->gec_hist_flags & a_GO_HIST_ADD){
      if(cdp->cd_caflags & n_CMD_ARG_H)
         gecp->gec_hist_flags = a_GO_HIST_NONE;
      else if((cdp->cd_caflags & n_CMD_ARG_G) ||
            (n_pstate & n_PS_MSGLIST_GABBY))
         gecp->gec_hist_flags |= a_GO_HIST_GABBY;
   }

   if(rv != 0){
      if(!(flags & a_NO_ERRNO)){
         if(cdp->cd_caflags & n_CMD_ARG_EM)
            flags |= a_NO_ERRNO;
         else if((nerrn = n_err_no) == 0)
            nerrn = n_ERR_INVAL;
      }else
         flags ^= a_NO_ERRNO;
   }else if(cdp->cd_caflags & n_CMD_ARG_EM)
      flags |= a_NO_ERRNO;
   else
      nerrn = n_ERR_NONE;
jleave:
   nexn = rv;

   if(flags & a_IGNERR){
      n_pstate &= ~n_PS_ERR_EXIT_MASK;
      n_exit_status = n_EXIT_OK;
   }else if(rv != 0){
      bool_t bo;

      if((bo = ok_blook(batch_exit_on_error))){
         n_OBSOLETE(_("please use *errexit*, not *batch-exit-on-error*"));
         if(!(n_poption & n_PO_BATCH_FLAG))
            bo = FAL0;
      }
      if(ok_blook(errexit) || bo) /* TODO v15: drop bo */
         n_pstate |= n_PS_ERR_QUIT;
      else if(ok_blook(posix)){
         if(n_psonce & n_PSO_STARTED)
            rv = 0;
         else if(!(n_psonce & n_PSO_INTERACTIVE))
            n_pstate |= n_PS_ERR_XIT;
      }else
         rv = 0;

      if(rv != 0){
         if(n_exit_status == n_EXIT_OK)
            n_exit_status = n_EXIT_ERR;
         if((n_poption & n_PO_D_V) &&
               !(n_psonce & (n_PSO_INTERACTIVE | n_PSO_STARTED)))
            n_alert(_("Non-interactive, bailing out due to errors "
               "in startup load phase"));
         goto jret;
      }
   }

   if(cdp == NULL)
      goto jret0;
   if((cdp->cd_caflags & n_CMD_ARG_P) && ok_blook(autoprint))
      if(visible(dot))
         n_go_input_inject(n_GO_INPUT_INJECT_COMMIT, "\\type",
            sizeof("\\type") -1);

   if(!(n_pstate & (n_PS_SOURCING | n_PS_HOOK_MASK)) &&
         !(cdp->cd_caflags & n_CMD_ARG_T))
      n_pstate |= n_PS_SAW_COMMAND;
jret0:
   rv = 0;
jret:
   if(!(flags & a_NO_ERRNO))
      n_pstate_err_no = nerrn;
   n_pstate_ex_no = nexn;
   NYD_LEAVE;
   return (rv == 0);
}

static void
a_go_hangup(int s){
   NYD_X; /* Signal handler */
   n_UNUSED(s);
   /* nothing to do? */
   exit(n_EXIT_ERR);
}

#ifdef HAVE_IMAP
FL void n_go_onintr_for_imap(void){a_go_onintr(0);}
#endif
static void
a_go_onintr(int s){ /* TODO block signals while acting */
   NYD_X; /* Signal handler */
   n_UNUSED(s);

   safe_signal(SIGINT, a_go_onintr);

   termios_state_reset();

   a_go_cleanup(a_GO_CLEANUP_UNWIND | /* XXX FAKE */a_GO_CLEANUP_HOLDALLSIGS);

   if(interrupts != 1)
      n_err_sighdl(_("Interrupt\n"));
   safe_signal(SIGPIPE, a_go_oldpipe);
   siglongjmp(a_go_srbuf, 0); /* FIXME get rid */
}

static void
a_go_cleanup(enum a_go_cleanup_mode gcm){
   /* Signals blocked */
   struct a_go_ctx *gcp;
   NYD_ENTER;

   if(!(gcm & a_GO_CLEANUP_HOLDALLSIGS))
      hold_all_sigs();
jrestart:
   gcp = a_go_ctx;

   /* Free input injections of this level first */
   if(!(gcm & a_GO_CLEANUP_LOOPTICK)){
      struct a_go_input_inject **giipp, *giip;

      for(giipp = &gcp->gc_inject; (giip = *giipp) != NULL;){
         *giipp = giip->gii_next;
         n_free(giip);
      }
   }

   /* Cleanup non-crucial external stuff */
   n_COLOUR(
      if(gcp->gc_data.gdc_colour != NULL)
         n_colour_stack_del(&gcp->gc_data);
   )

   /* Work the actual context (according to cleanup mode) */
   if(gcp->gc_outer == NULL){
      if(gcm & (a_GO_CLEANUP_UNWIND | a_GO_CLEANUP_SIGINT)){
         if(a_go_xcall != NULL){
            n_free(a_go_xcall);
            a_go_xcall = NULL;
         }
         gcp->gc_flags &= ~a_GO_XCALL_LOOP_MASK;
         n_pstate &= ~n_PS_ERR_EXIT_MASK;
         close_all_files();
      }else{
         if(!(n_pstate & n_PS_SOURCING))
            close_all_files();
      }

      n_memory_reset();

      n_pstate &= ~(n_PS_SOURCING | n_PS_ROBOT);
      assert(a_go_xcall == NULL);
      assert(!(gcp->gc_flags & a_GO_XCALL_LOOP_MASK));
      assert(gcp->gc_on_finalize == NULL);
      assert(gcp->gc_data.gdc_colour == NULL);
      goto jxleave;
   }else if(gcm & a_GO_CLEANUP_LOOPTICK){
      n_memory_reset();
      goto jxleave;
   }else if(gcp->gc_flags & a_GO_SPLICE){ /* TODO Temporary hack */
      n_stdin = gcp->gc_splice_stdin;
      n_stdout = gcp->gc_splice_stdout;
      n_psonce = gcp->gc_splice_psonce;
      goto jstackpop;
   }

   /* Cleanup crucial external stuff */
   if(gcp->gc_data.gdc_ifcond != NULL){
      n_cnd_if_stack_del(&gcp->gc_data);
      if(!(gcm & (a_GO_CLEANUP_ERROR | a_GO_CLEANUP_SIGINT)) &&
            !(gcp->gc_flags & a_GO_FORCE_EOF) && a_go_xcall == NULL &&
            !(n_psonce & n_PSO_EXIT_MASK)){
         n_err(_("Unmatched `if' at end of %s %s\n"),
            ((gcp->gc_flags & a_GO_MACRO
             ? (gcp->gc_flags & a_GO_MACRO_CMD ? _("command") : _("macro"))
             : _("`source'd file"))),
            gcp->gc_name);
         gcm |= a_GO_CLEANUP_ERROR;
      }
   }

   /* Teardown context */
   if(gcp->gc_flags & a_GO_MACRO){
      if(gcp->gc_flags & a_GO_MACRO_FREE_DATA){
         char **lp;

         while(*(lp = &gcp->gc_lines[gcp->gc_loff]) != NULL){
            n_free(*lp);
            ++gcp->gc_loff;
         }
         /* Part of gcp's memory chunk, then */
         if(!(gcp->gc_flags & a_GO_MACRO_CMD))
            n_free(gcp->gc_lines);
      }
   }else if(gcp->gc_flags & a_GO_PIPE)
      /* XXX command manager should -TERM then -KILL instead of hoping
       * XXX for exit of provider due to n_ERR_PIPE / SIGPIPE */
      Pclose(gcp->gc_file, TRU1);
   else if(gcp->gc_flags & a_GO_FILE)
      Fclose(gcp->gc_file);

jstackpop:
   if(!(gcp->gc_flags & a_GO_MEMPOOL_INHERITED)){
      if(gcp->gc_data.gdc_mempool != NULL)
         n_memory_pool_pop(NULL);
   }else
      n_memory_reset();

   n_go_data = &(a_go_ctx = gcp->gc_outer)->gc_data;
   if((a_go_ctx->gc_flags & (a_GO_MACRO | a_GO_SUPER_MACRO)) ==
         (a_GO_MACRO | a_GO_SUPER_MACRO)){
      n_pstate &= ~n_PS_SOURCING;
      assert(n_pstate & n_PS_ROBOT);
   }else if(!(a_go_ctx->gc_flags & a_GO_TYPE_MASK))
      n_pstate &= ~(n_PS_SOURCING | n_PS_ROBOT);
   else
      assert(n_pstate & n_PS_ROBOT);

   if(gcp->gc_on_finalize != NULL)
      (*gcp->gc_on_finalize)(gcp->gc_finalize_arg);

   if(gcm & a_GO_CLEANUP_ERROR){
      if(a_go_ctx->gc_flags & a_GO_XCALL_LOOP)
         a_go_ctx->gc_flags |= a_GO_XCALL_LOOP_ERROR;
      goto jerr;
   }
jleave:
   if(gcp->gc_flags & a_GO_FREE)
      n_free(gcp);

   if(n_UNLIKELY((gcm & a_GO_CLEANUP_UNWIND) && gcp != a_go_ctx))
      goto jrestart;

jxleave:
   NYD_LEAVE;
   if(!(gcm & a_GO_CLEANUP_HOLDALLSIGS))
      rele_all_sigs();
   return;

jerr:
   /* With *posix* we follow what POSIX says:
    *    Any errors in the start-up file shall either cause mailx to
    *    terminate with a diagnostic message and a non-zero status or to
    *    continue after writing a diagnostic message, ignoring the
    *    remainder of the lines in the start-up file
    * Print the diagnostic only for the outermost resource unless the user
    * is debugging or in verbose mode */
   if((n_poption & n_PO_D_V) ||
         (!(n_psonce & n_PSO_STARTED) &&
          !(gcp->gc_flags & (a_GO_SPLICE | a_GO_MACRO)) &&
          !(gcp->gc_outer->gc_flags & a_GO_TYPE_MASK)))
      /* I18N: file inclusion, macro etc. evaluation has been stopped */
      n_alert(_("Stopped %s %s due to errors%s"),
         (n_psonce & n_PSO_STARTED
          ? (gcp->gc_flags & a_GO_SPLICE ? _("spliced in program")
          : (gcp->gc_flags & a_GO_MACRO
             ? (gcp->gc_flags & a_GO_MACRO_CMD
                ? _("evaluating command") : _("evaluating macro"))
             : (gcp->gc_flags & a_GO_PIPE
                ? _("executing `source'd pipe")
                : (gcp->gc_flags & a_GO_FILE
                  ? _("loading `source'd file") : _(a_GO_MAINCTX_NAME))))
          )
          : (gcp->gc_flags & a_GO_MACRO
             ? (gcp->gc_flags & a_GO_MACRO_X_OPTION
                ? _("evaluating command line") : _("evaluating macro"))
             : _("loading initialization resource"))),
         gcp->gc_name,
         (n_poption & n_PO_DEBUG ? n_empty : _(" (enable *debug* for trace)")));
   goto jleave;
}

static bool_t
a_go_file(char const *file, bool_t silent_open_error){
   struct a_go_ctx *gcp;
   sigset_t osigmask;
   size_t nlen;
   char *nbuf;
   bool_t ispipe;
   FILE *fip;
   NYD_ENTER;

   fip = NULL;

   /* Being a command argument file is space-trimmed *//* TODO v15 with
    * TODO WYRALIST this is no longer necessary true, and for that we
    * TODO don't set _PARSE_TRIM_SPACE because we cannot! -> cmd-tab.h!! */
#if 0
   ((ispipe = (!silent_open_error && (nlen = strlen(file)) > 0 &&
         file[--nlen] == '|')))
#else
   ispipe = FAL0;
   if(!silent_open_error){
      for(nlen = strlen(file); nlen > 0;){
         char c;

         c = file[--nlen];
         if(!spacechar(c)){
            if(c == '|'){
               nbuf = savestrbuf(file, nlen);
               ispipe = TRU1;
            }
            break;
         }
      }
   }
#endif

   if(ispipe){
      if((fip = Popen(nbuf /* #if 0 above = savestrbuf(file, nlen)*/, "r",
            ok_vlook(SHELL), NULL, n_CHILD_FD_NULL)) == NULL)
         goto jeopencheck;
   }else if((nbuf = fexpand(file, FEXP_LOCAL)) == NULL)
      goto jeopencheck;
   else if((fip = Fopen(nbuf, "r")) == NULL){
jeopencheck:
      if(!silent_open_error || (n_poption & n_PO_D_V))
         n_perr(nbuf, 0);
      if(silent_open_error)
         fip = (FILE*)-1;
      goto jleave;
   }

   sigprocmask(SIG_BLOCK, NULL, &osigmask);

   gcp = n_alloc(n_VSTRUCT_SIZEOF(struct a_go_ctx, gc_name) +
         (nlen = strlen(nbuf) +1));
   memset(gcp, 0, n_VSTRUCT_SIZEOF(struct a_go_ctx, gc_name));

   hold_all_sigs();

   gcp->gc_outer = a_go_ctx;
   gcp->gc_osigmask = osigmask;
   gcp->gc_file = fip;
   gcp->gc_flags = (ispipe ? a_GO_FREE | a_GO_PIPE : a_GO_FREE | a_GO_FILE) |
         (a_go_ctx->gc_flags & a_GO_SUPER_MACRO ? a_GO_SUPER_MACRO : 0);
   memcpy(gcp->gc_name, nbuf, nlen);

   a_go_ctx = gcp;
   n_go_data = &gcp->gc_data;
   n_pstate |= n_PS_SOURCING | n_PS_ROBOT;
   if(!a_go_event_loop(gcp, n_GO_INPUT_NONE | n_GO_INPUT_NL_ESC))
      fip = NULL;
jleave:
   NYD_LEAVE;
   return (fip != NULL);
}

static bool_t
a_go_load(struct a_go_ctx *gcp){
   NYD2_ENTER;

   assert(!(n_psonce & n_PSO_STARTED));
   assert(!(a_go_ctx->gc_flags & a_GO_TYPE_MASK));

   gcp->gc_flags |= a_GO_MEMPOOL_INHERITED;
   gcp->gc_data.gdc_mempool = n_go_data->gdc_mempool;

   hold_all_sigs();

   /* POSIX:
    *    Any errors in the start-up file shall either cause mailx to terminate
    *    with a diagnostic message and a non-zero status or to continue after
    *    writing a diagnostic message, ignoring the remainder of the lines in
    *    the start-up file. */
   gcp->gc_outer = a_go_ctx;
   a_go_ctx = gcp;
   n_go_data = &gcp->gc_data;
/* FIXME won't work for now (n_PS_ROBOT needs n_PS_SOURCING sofar)
   n_pstate |= n_PS_ROBOT |
         (gcp->gc_flags & a_GO_MACRO_X_OPTION ? 0 : n_PS_SOURCING);
*/
   n_pstate |= n_PS_ROBOT | n_PS_SOURCING;

   rele_all_sigs();

   n_go_main_loop();
   NYD2_LEAVE;
   return (((n_psonce & n_PSO_EXIT_MASK) |
      (n_pstate & n_PS_ERR_EXIT_MASK)) == 0);
}

static void
a_go__eloopint(int sig){ /* TODO one day, we don't need it no more */
   NYD_X; /* Signal handler */
   n_UNUSED(sig);
   siglongjmp(a_go_ctx->gc_eloop_jmp, 1);
}

static bool_t
a_go_event_loop(struct a_go_ctx *gcp, enum n_go_input_flags gif){
   sighandler_type soldhdl;
   struct a_go_eval_ctx gec;
   enum {a_RETOK = TRU1, a_TICKED = 1<<1} volatile f;
   volatile int hadint; /* TODO get rid of shitty signal stuff (see signal.c) */
   sigset_t osigmask;
   NYD2_ENTER;

   memset(&gec, 0, sizeof gec);
   osigmask = gcp->gc_osigmask;
   hadint = FAL0;
   f = a_RETOK;

   if((soldhdl = safe_signal(SIGINT, SIG_IGN)) != SIG_IGN){
      safe_signal(SIGINT, &a_go__eloopint);
      if(sigsetjmp(gcp->gc_eloop_jmp, 1)){
         hold_all_sigs();
         hadint = TRU1;
         f &= ~a_RETOK;
         gcp->gc_flags &= ~a_GO_XCALL_LOOP_MASK;
         goto jjump;
      }
   }

   for(;; f |= a_TICKED){
      int n;

      if(f & a_TICKED)
         n_memory_reset();

      /* Read a line of commands and handle end of file specially */
      gec.gec_line.l = gec.gec_line_size;
      rele_all_sigs();
      n = n_go_input(gif, NULL, &gec.gec_line.s, &gec.gec_line.l, NULL, NULL);
      hold_all_sigs();
      gec.gec_line_size = (ui32_t)gec.gec_line.l;
      gec.gec_line.l = (ui32_t)n;

      if(n < 0)
         break;

      rele_all_sigs();
      assert(gec.gec_hist_flags == a_GO_HIST_NONE);
      if(!a_go_evaluate(&gec))
         f &= ~a_RETOK;
      hold_all_sigs();

      if(!(f & a_RETOK) || a_go_xcall != NULL ||
            (n_psonce & n_PSO_EXIT_MASK) || (n_pstate & n_PS_ERR_EXIT_MASK))
         break;
   }

jjump: /* TODO Should be _CLEANUP_UNWIND not _TEARDOWN on signal if DOABLE! */
   a_go_cleanup(a_GO_CLEANUP_TEARDOWN |
      (f & a_RETOK ? 0 : a_GO_CLEANUP_ERROR) |
      (hadint ? a_GO_CLEANUP_SIGINT : 0) | a_GO_CLEANUP_HOLDALLSIGS);

   if(gec.gec_line.s != NULL)
      n_free(gec.gec_line.s);

   if(soldhdl != SIG_IGN)
      safe_signal(SIGINT, soldhdl);
   NYD2_LEAVE;
   rele_all_sigs();
   if(hadint){
      sigprocmask(SIG_SETMASK, &osigmask, NULL);
      n_raise(SIGINT);
   }
   return (f & a_RETOK);
}

FL void
n_go_init(void){
   struct a_go_ctx *gcp;
   NYD2_ENTER;

   assert(n_stdin != NULL);

   gcp = (void*)a_go__mainctx_b.uf;
   DBGOR( memset(gcp, 0, n_VSTRUCT_SIZEOF(struct a_go_ctx, gc_name)),
      memset(&gcp->gc_data, 0, sizeof gcp->gc_data) );
   gcp->gc_file = n_stdin;
   memcpy(gcp->gc_name, a_GO_MAINCTX_NAME, sizeof(a_GO_MAINCTX_NAME));
   a_go_ctx = gcp;
   n_go_data = &gcp->gc_data;

   n_child_manager_start();
   NYD2_LEAVE;
}

FL bool_t
n_go_main_loop(void){ /* FIXME */
   struct a_go_eval_ctx gec;
   int n, eofcnt;
   bool_t volatile rv;
   NYD_ENTER;

   rv = TRU1;

   if (!(n_pstate & n_PS_SOURCING)) {
      if (safe_signal(SIGINT, SIG_IGN) != SIG_IGN)
         safe_signal(SIGINT, &a_go_onintr);
      if (safe_signal(SIGHUP, SIG_IGN) != SIG_IGN)
         safe_signal(SIGHUP, &a_go_hangup);
   }
   a_go_oldpipe = safe_signal(SIGPIPE, SIG_IGN);
   safe_signal(SIGPIPE, a_go_oldpipe);

   memset(&gec, 0, sizeof gec);

   (void)sigsetjmp(a_go_srbuf, 1); /* FIXME get rid */
   hold_all_sigs();

   for (eofcnt = 0;; gec.gec_ever_seen = TRU1) {
      interrupts = 0;

      if(gec.gec_ever_seen)
         a_go_cleanup(a_GO_CLEANUP_LOOPTICK | a_GO_CLEANUP_HOLDALLSIGS);

      if (!(n_pstate & n_PS_SOURCING)) {
         char *cp;

         /* TODO Note: this buffer may contain a password.  We should redefine
          * TODO the code flow which has to do that */
         if ((cp = termios_state.ts_linebuf) != NULL) {
            termios_state.ts_linebuf = NULL;
            termios_state.ts_linesize = 0;
            n_free(cp); /* TODO pool give-back */
         }
         if (gec.gec_line.l > LINESIZE * 3) {
            n_free(gec.gec_line.s);
            gec.gec_line.s = NULL;
            gec.gec_line.l = gec.gec_line_size = 0;
         }
      }

      if (!(n_pstate & n_PS_SOURCING) && (n_psonce & n_PSO_INTERACTIVE)) {
         char *cp;

         if ((cp = ok_vlook(newmail)) != NULL) { /* TODO bla */
            struct stat st;

            if(mb.mb_type == MB_FILE){
               if(!stat(mailname, &st) && st.st_size > mailsize) Jnewmail:{
                  ui32_t odid;
                  size_t odot;

                  odot = PTR2SIZE(dot - message);
                  odid = (n_pstate & n_PS_DID_PRINT_DOT);

                  rele_all_sigs();
                  n = setfile(mailname,
                        (FEDIT_NEWMAIL |
                           ((mb.mb_perm & MB_DELE) ? 0 : FEDIT_RDONLY)));
                  hold_all_sigs();

                  if(n < 0) {
                     n_exit_status |= n_EXIT_ERR;
                     rv = FAL0;
                     break;
                  }
#ifdef HAVE_IMAP
                  if(mb.mb_type != MB_IMAP){
#endif
                     dot = &message[odot];
                     n_pstate |= odid;
#ifdef HAVE_IMAP
                  }
#endif
               }
            }else{
               n = (cp != NULL && strcmp(cp, "nopoll"));

               if(mb.mb_type == MB_MAILDIR){
                  if(n != 0)
                     goto Jnewmail;
               }
#ifdef HAVE_IMAP
               else if(mb.mb_type == MB_IMAP){
                  if(!n)
                     n = (cp != NULL && strcmp(cp, "noimap"));

                  if(imap_newmail(n) > (cp == NULL))
                     goto Jnewmail;
               }
#endif
            }
         }
      }

      /* Read a line of commands and handle end of file specially */
      gec.gec_line.l = gec.gec_line_size;
      /* C99 */{
         bool_t histadd;

         histadd = (!(n_pstate & n_PS_SOURCING) &&
               (n_psonce & n_PSO_INTERACTIVE));
         rele_all_sigs();
         n = n_go_input(n_GO_INPUT_CTX_DEFAULT | n_GO_INPUT_NL_ESC, NULL,
               &gec.gec_line.s, &gec.gec_line.l, NULL, &histadd);
         hold_all_sigs();

         gec.gec_hist_flags = histadd ? a_GO_HIST_ADD : a_GO_HIST_NONE;
      }
      gec.gec_line_size = (ui32_t)gec.gec_line.l;
      gec.gec_line.l = (ui32_t)n;

      if (n < 0) {
         if (!(n_pstate & n_PS_ROBOT) &&
               (n_psonce & n_PSO_INTERACTIVE) && ok_blook(ignoreeof) &&
               ++eofcnt < 4) {
            fprintf(n_stdout, _("*ignoreeof* set, use `quit' to quit.\n"));
            n_go_input_clearerr();
            continue;
         }
         break;
      }

      n_pstate &= ~n_PS_HOOK_MASK;
      rele_all_sigs();
      rv = a_go_evaluate(&gec);
      hold_all_sigs();

      if(gec.gec_hist_flags & a_GO_HIST_ADD){
         char const *cc, *ca;

         cc = gec.gec_hist_cmd;
         ca = gec.gec_hist_args;
         if(cc != NULL && ca != NULL)
            cc = savecatsep(cc, ' ', ca);
         else if(ca != NULL)
            cc = ca;
         n_tty_addhist(cc, ((gec.gec_hist_flags & a_GO_HIST_GABBY) != 0));
      }

      switch(n_pstate & n_PS_ERR_EXIT_MASK){
      case n_PS_ERR_XIT: n_psonce |= n_PSO_XIT; break;
      case n_PS_ERR_QUIT: n_psonce |= n_PSO_QUIT; break;
      default: break;
      }
      if(n_psonce & n_PSO_EXIT_MASK)
         break;

      if(!rv)
         break;
   }

   a_go_cleanup(a_GO_CLEANUP_TEARDOWN | a_GO_CLEANUP_HOLDALLSIGS |
      (rv ? 0 : a_GO_CLEANUP_ERROR));

   if (gec.gec_line.s != NULL)
      n_free(gec.gec_line.s);

   rele_all_sigs();
   NYD_LEAVE;
   return rv;
}

FL void
n_go_input_clearerr(void){
   FILE *fp;
   NYD2_ENTER;

   fp = NULL;

   if(!(a_go_ctx->gc_flags & (a_GO_FORCE_EOF |
         a_GO_PIPE | a_GO_MACRO | a_GO_SPLICE)))
      fp = a_go_ctx->gc_file;

   if(fp != NULL){
      a_go_ctx->gc_flags &= ~a_GO_IS_EOF;
      clearerr(fp);
   }
   NYD2_LEAVE;
}

FL void
n_go_input_force_eof(void){
   NYD2_ENTER;
   a_go_ctx->gc_flags |= a_GO_FORCE_EOF;
   NYD2_LEAVE;
}

FL bool_t
n_go_input_is_eof(void){
   bool_t rv;
   NYD2_ENTER;

   rv = ((a_go_ctx->gc_flags & a_GO_IS_EOF) != 0);
   NYD2_LEAVE;
   return rv;
}

FL void
n_go_input_inject(enum n_go_input_inject_flags giif, char const *buf,
      size_t len){
   NYD_ENTER;
   if(len == UIZ_MAX)
      len = strlen(buf);

   if(UIZ_MAX - n_VSTRUCT_SIZEOF(struct a_go_input_inject, gii_dat) -1 > len &&
         len > 0){
      struct a_go_input_inject *giip,  **giipp;

      hold_all_sigs();

      giip = n_alloc(n_VSTRUCT_SIZEOF(struct a_go_input_inject, gii_dat
            ) + 1 + len +1);
      giipp = &a_go_ctx->gc_inject;
      giip->gii_next = *giipp;
      giip->gii_commit = ((giif & n_GO_INPUT_INJECT_COMMIT) != 0);
      giip->gii_no_history = ((giif & n_GO_INPUT_INJECT_HISTORY) == 0);
      memcpy(&giip->gii_dat[0], buf, len);
      giip->gii_dat[giip->gii_len = len] = '\0';
      *giipp = giip;

      rele_all_sigs();
   }
   NYD_LEAVE;
}

FL int
(n_go_input)(enum n_go_input_flags gif, char const *prompt, char **linebuf,
      size_t *linesize, char const *string, bool_t *histok_or_null
      n_MEMORY_DEBUG_ARGS){
   /* TODO readline: linebuf pool!; n_go_input should return si64_t */
   struct n_string xprompt;
   FILE *ifile;
   bool_t doprompt, dotty;
   char const *iftype;
   struct a_go_input_inject *giip;
   int nold, n;
   bool_t histok;
   NYD2_ENTER;

   if(!(gif & n_GO_INPUT_HOLDALLSIGS))
      hold_all_sigs();

   histok = FAL0;

   if(a_go_ctx->gc_flags & a_GO_FORCE_EOF){
      a_go_ctx->gc_flags |= a_GO_IS_EOF;
      n = -1;
      goto jleave;
   }

   if(gif & n_GO_INPUT_FORCE_STDIN)
      goto jforce_stdin;

   /* Special case macro mode: never need to prompt, lines have always been
    * unfolded already */
   if(a_go_ctx->gc_flags & a_GO_MACRO){
      if(*linebuf != NULL)
         n_free(*linebuf);

      /* Injection in progress?  Don't care about the autocommit state here */
      if((giip = a_go_ctx->gc_inject) != NULL){
         a_go_ctx->gc_inject = giip->gii_next;

         /* Simply "reuse" allocation, copy string to front of it */
jinject:
         *linesize = giip->gii_len;
         *linebuf = (char*)giip;
         memmove(*linebuf, giip->gii_dat, giip->gii_len +1);
         iftype = "INJECTION";
      }else{
         if((*linebuf = a_go_ctx->gc_lines[a_go_ctx->gc_loff]) == NULL){
            *linesize = 0;
            a_go_ctx->gc_flags |= a_GO_IS_EOF;
            n = -1;
            goto jleave;
         }

         ++a_go_ctx->gc_loff;
         *linesize = strlen(*linebuf);
         if(!(a_go_ctx->gc_flags & a_GO_MACRO_FREE_DATA))
            *linebuf = sbufdup(*linebuf, *linesize);

         iftype = (a_go_ctx->gc_flags & a_GO_MACRO_X_OPTION)
               ? "-X OPTION"
               : (a_go_ctx->gc_flags & a_GO_MACRO_CMD) ? "CMD" : "MACRO";
      }
      n = (int)*linesize;
      n_pstate |= n_PS_READLINE_NL;
      goto jhave_dat;
   }else{
      /* Injection in progress? */
      struct a_go_input_inject **giipp;

      giipp = &a_go_ctx->gc_inject;

      if((giip = *giipp) != NULL){
         *giipp = giip->gii_next;

         if(giip->gii_commit){
            if(*linebuf != NULL)
               n_free(*linebuf);
            histok = !giip->gii_no_history;
            goto jinject; /* (above) */
         }else{
            string = savestrbuf(giip->gii_dat, giip->gii_len);
            n_free(giip);
         }
      }
   }

jforce_stdin:
   n_pstate &= ~n_PS_READLINE_NL;
   iftype = (!(n_psonce & n_PSO_STARTED) ? "LOAD"
          : (n_pstate & n_PS_SOURCING) ? "SOURCE" : "READ");
   histok = (n_psonce & (n_PSO_INTERACTIVE | n_PSO_STARTED)) ==
         (n_PSO_INTERACTIVE | n_PSO_STARTED) && !(n_pstate & n_PS_ROBOT);
   doprompt = !(gif & n_GO_INPUT_FORCE_STDIN) && histok;
   dotty = (doprompt && !ok_blook(line_editor_disable));
   if(!doprompt)
      gif |= n_GO_INPUT_PROMPT_NONE;
   else{
      if(!dotty)
         n_string_creat_auto(&xprompt);
      if(prompt == NULL)
         gif |= n_GO_INPUT_PROMPT_EVAL;
   }

   /* Ensure stdout is flushed first anyway (partial lines, maybe?) */
   if(!dotty && (gif & n_GO_INPUT_PROMPT_NONE))
      fflush(n_stdout);

   if(gif & n_GO_INPUT_FORCE_STDIN){
      struct a_go_readctl_ctx *grcp;

      grcp = n_readctl_overlay;
      ifile = (grcp == NULL || grcp->grc_fp == NULL) ? n_stdin : grcp->grc_fp;
   }else
      ifile = a_go_ctx->gc_file;
   if(ifile == NULL){
      assert((n_pstate & n_PS_COMPOSE_FORKHOOK) &&
         (a_go_ctx->gc_flags & a_GO_MACRO));
      ifile = n_stdin;
   }

   for(nold = n = 0;;){
      if(dotty){
         assert(ifile == n_stdin);
         if(string != NULL && (n = (int)strlen(string)) > 0){
            if(*linesize > 0)
               *linesize += n +1;
            else
               *linesize = (size_t)n + LINESIZE +1;
            *linebuf = (n_realloc)(*linebuf, *linesize n_MEMORY_DEBUG_ARGSCALL);
           memcpy(*linebuf, string, (size_t)n +1);
         }
         string = NULL;

         rele_all_sigs();

         n = (n_tty_readline)(gif, prompt, linebuf, linesize, n, histok_or_null
               n_MEMORY_DEBUG_ARGSCALL);

         hold_all_sigs();
      }else{
         if(!(gif & n_GO_INPUT_PROMPT_NONE))
            n_tty_create_prompt(&xprompt, prompt, gif);

         rele_all_sigs();

         if(!(gif & n_GO_INPUT_PROMPT_NONE) && xprompt.s_len > 0){
            fwrite(xprompt.s_dat, 1, xprompt.s_len, n_stdout);
            fflush(n_stdout);
         }

         n = (readline_restart)(ifile, linebuf, linesize, n
               n_MEMORY_DEBUG_ARGSCALL);

         hold_all_sigs();

         if(n < 0 && feof(ifile))
            a_go_ctx->gc_flags |= a_GO_IS_EOF;

         if(n > 0 && nold > 0){
            char const *cp;
            int i;

            i = 0;
            cp = &(*linebuf)[nold];
            while(spacechar(*cp) && n - i >= nold)
               ++cp, ++i;
            if(i > 0){
               memmove(&(*linebuf)[nold], cp, n - nold - i);
               n -= i;
               (*linebuf)[n] = '\0';
            }
         }
      }

      if(n <= 0)
         break;

      /* POSIX says:
       *    An unquoted <backslash> at the end of a command line shall
       *    be discarded and the next line shall continue the command */
      if(!(gif & n_GO_INPUT_NL_ESC) || (*linebuf)[n - 1] != '\\'){
         if(dotty)
            n_pstate |= n_PS_READLINE_NL;
         break;
      }
      /* Definitely outside of quotes, thus the quoting rules are so that an
       * uneven number of successive reverse solidus at EOL is a continuation */
      if(n > 1){
         size_t i, j;

         for(j = 1, i = (size_t)n - 1; i-- > 0; ++j)
            if((*linebuf)[i] != '\\')
               break;
         if(!(j & 1))
            break;
      }
      (*linebuf)[nold = --n] = '\0';
      gif |= n_GO_INPUT_NL_FOLLOW;
   }

   if(n < 0)
      goto jleave;
   (*linebuf)[*linesize = n] = '\0';

jhave_dat:
   if(n_poption & n_PO_D_VV)
      n_err(_("%s %d bytes <%s>\n"), iftype, n, *linebuf);
jleave:
   if (n_pstate & n_PS_PSTATE_PENDMASK)
      a_go_update_pstate();

   /* TODO We need to special case a_GO_SPLICE, since that is not managed by us
    * TODO but only established from the outside and we need to drop this
    * TODO overlay context somehow */
   if(n < 0 && (a_go_ctx->gc_flags & a_GO_SPLICE))
      a_go_cleanup(a_GO_CLEANUP_TEARDOWN | a_GO_CLEANUP_HOLDALLSIGS);

   if(histok_or_null != NULL && !histok)
      *histok_or_null = FAL0;

   if(!(gif & n_GO_INPUT_HOLDALLSIGS))
      rele_all_sigs();
   NYD2_LEAVE;
   return n;
}

FL char *
n_go_input_cp(enum n_go_input_flags gif, char const *prompt,
      char const *string){
   struct n_sigman sm;
   bool_t histadd;
   size_t linesize;
   char *linebuf, * volatile rv;
   int n;
   NYD2_ENTER;

   linesize = 0;
   linebuf = NULL;
   rv = NULL;

   n_SIGMAN_ENTER_SWITCH(&sm, n_SIGMAN_ALL){
   case 0:
      break;
   default:
      goto jleave;
   }

   histadd = TRU1;
   n = n_go_input(gif, prompt, &linebuf, &linesize, string, &histadd);
   if(n > 0 && *(rv = savestrbuf(linebuf, (size_t)n)) != '\0' &&
         (gif & n_GO_INPUT_HIST_ADD) && (n_psonce & n_PSO_INTERACTIVE) &&
         histadd)
      n_tty_addhist(rv, ((gif & n_GO_INPUT_HIST_GABBY) != 0));

   n_sigman_cleanup_ping(&sm);
jleave:
   if(linebuf != NULL)
      n_free(linebuf);
   NYD2_LEAVE;
   n_sigman_leave(&sm, n_SIGMAN_VIPSIGS_NTTYOUT);
   return rv;
}

FL bool_t
n_go_load(char const *name){
   struct a_go_ctx *gcp;
   size_t i;
   FILE *fip;
   bool_t rv;
   NYD_ENTER;

   rv = TRU1;

   if(name == NULL || *name == '\0')
      goto jleave;
   else if((fip = Fopen(name, "r")) == NULL){
      if(n_poption & n_PO_D_V)
         n_err(_("No such file to load: %s\n"), n_shexp_quote_cp(name, FAL0));
      goto jleave;
   }

   i = strlen(name) +1;
   gcp = n_alloc(n_VSTRUCT_SIZEOF(struct a_go_ctx, gc_name) + i);
   memset(gcp, 0, n_VSTRUCT_SIZEOF(struct a_go_ctx, gc_name));

   gcp->gc_file = fip;
   gcp->gc_flags = a_GO_FREE | a_GO_FILE;
   memcpy(gcp->gc_name, name, i);

   if(n_poption & n_PO_D_V)
      n_err(_("Loading %s\n"), n_shexp_quote_cp(gcp->gc_name, FAL0));
   rv = a_go_load(gcp);
jleave:
   NYD_LEAVE;
   return rv;
}

FL bool_t
n_go_Xargs(char const **lines, size_t cnt){
   static char const name[] = "-X";

   union{
      bool_t rv;
      ui64_t align;
      char uf[n_VSTRUCT_SIZEOF(struct a_go_ctx, gc_name) + sizeof(name)];
   } b;
   char const *srcp, *xsrcp;
   char *cp;
   size_t imax, i, len;
   struct a_go_ctx *gcp;
   NYD_ENTER;

   gcp = (void*)b.uf;
   memset(gcp, 0, n_VSTRUCT_SIZEOF(struct a_go_ctx, gc_name));

   gcp->gc_flags = a_GO_MACRO | a_GO_MACRO_X_OPTION |
         a_GO_SUPER_MACRO | a_GO_MACRO_FREE_DATA;
   memcpy(gcp->gc_name, name, sizeof name);

   /* The problem being that we want to support reverse solidus newline
    * escaping also within multiline -X, i.e., POSIX says:
    *    An unquoted <backslash> at the end of a command line shall
    *    be discarded and the next line shall continue the command
    * Therefore instead of "gcp->gc_lines = n_UNCONST(lines)", duplicate the
    * entire lines array and set _MACRO_FREE_DATA */
   imax = cnt + 1;
   gcp->gc_lines = n_alloc(sizeof(*gcp->gc_lines) * imax);

   /* For each of the input lines.. */
   for(i = len = 0, cp = NULL; cnt > 0;){
      bool_t keep;
      size_t j;

      if((j = strlen(srcp = *lines)) == 0){
         ++lines, --cnt;
         continue;
      }

      /* Separate one line from a possible multiline input string */
      if((xsrcp = memchr(srcp, '\n', j)) != NULL){
         *lines = &xsrcp[1];
         j = PTR2SIZE(xsrcp - srcp);
      }else
         ++lines, --cnt;

      /* The (separated) string may itself indicate soft newline escaping */
      if((keep = (srcp[j - 1] == '\\'))){
         size_t xj, xk;

         /* Need an uneven number of reverse solidus */
         for(xk = 1, xj = j - 1; xj-- > 0; ++xk)
            if(srcp[xj] != '\\')
               break;
         if(xk & 1)
            --j;
         else
            keep = FAL0;
      }

      /* Strip any leading WS from follow lines, then */
      if(cp != NULL)
         while(j > 0 && spacechar(*srcp))
            ++srcp, --j;

      if(j > 0){
         if(i + 2 >= imax){ /* TODO need a vector (main.c, here, ++) */
            imax += 4;
            gcp->gc_lines = n_realloc(gcp->gc_lines, sizeof(*gcp->gc_lines) *
                  imax);
         }
         gcp->gc_lines[i] = cp = n_realloc(cp, len + j +1);
         memcpy(&cp[len], srcp, j);
         cp[len += j] = '\0';

         if(!keep)
            ++i;
      }
      if(!keep)
         cp = NULL, len = 0;
   }
   if(cp != NULL){
      assert(i + 1 < imax);
      gcp->gc_lines[i++] = cp;
   }
   gcp->gc_lines[i] = NULL;

   b.rv = a_go_load(gcp);
   NYD_LEAVE;
   return b.rv;
}

FL int
c_source(void *v){
   int rv;
   NYD_ENTER;

   rv = (a_go_file(*(char**)v, FAL0) == TRU1) ? 0 : 1;
   NYD_LEAVE;
   return rv;
}

FL int
c_source_if(void *v){ /* XXX obsolete?, support file tests in `if' etc.! */
   int rv;
   NYD_ENTER;

   rv = (a_go_file(*(char**)v, TRU1) == TRU1) ? 0 : 1;
   NYD_LEAVE;
   return rv;
}

FL bool_t
n_go_macro(enum n_go_input_flags gif, char const *name, char **lines,
      void (*on_finalize)(void*), void *finalize_arg){
   struct a_go_ctx *gcp;
   size_t i;
   int rv;
   sigset_t osigmask;
   NYD_ENTER;

   sigprocmask(SIG_BLOCK, NULL, &osigmask);

   gcp = n_alloc(n_VSTRUCT_SIZEOF(struct a_go_ctx, gc_name) +
         (i = strlen(name) +1));
   memset(gcp, 0, n_VSTRUCT_SIZEOF(struct a_go_ctx, gc_name));

   hold_all_sigs();

   gcp->gc_outer = a_go_ctx;
   gcp->gc_osigmask = osigmask;
   gcp->gc_flags = a_GO_FREE | a_GO_MACRO | a_GO_MACRO_FREE_DATA |
         ((!(a_go_ctx->gc_flags & a_GO_TYPE_MASK) ||
            (a_go_ctx->gc_flags & a_GO_SUPER_MACRO)) ? a_GO_SUPER_MACRO : 0) |
         ((gif & n_GO_INPUT_NO_XCALL) ? a_GO_XCALL_IS_CALL : 0);
   gcp->gc_lines = lines;
   gcp->gc_on_finalize = on_finalize;
   gcp->gc_finalize_arg = finalize_arg;
   memcpy(gcp->gc_name, name, i);

   a_go_ctx = gcp;
   n_go_data = &gcp->gc_data;
   n_pstate |= n_PS_ROBOT;
   rv = a_go_event_loop(gcp, gif);

   /* Shall this enter a `xcall' stack avoidance optimization (loop)? */
   if(a_go_xcall != NULL){
      void *vp;
      struct n_cmd_arg_ctx *cacp;

      if(a_go_xcall == (void*)-1)
         a_go_xcall = NULL;
      else if(((void const*)(cacp = a_go_xcall)->cac_indat) == gcp){
         /* Indicate that "our" (ex-) parent now hosts xcall optimization */
         a_go_ctx->gc_flags |= a_GO_XCALL_LOOP;
         while(a_go_xcall != NULL){
            hold_all_sigs();

            a_go_ctx->gc_flags &= ~a_GO_XCALL_LOOP_ERROR;

            vp = a_go_xcall;
            a_go_xcall = NULL;
            cacp = n_cmd_arg_restore_from_heap(vp);
            n_free(vp);

            rele_all_sigs();

            (void)c_call(cacp);
         }
         rv = ((a_go_ctx->gc_flags & a_GO_XCALL_LOOP_ERROR) == 0);
         a_go_ctx->gc_flags &= ~a_GO_XCALL_LOOP_MASK;
      }
   }
   NYD_LEAVE;
   return rv;
}

FL bool_t
n_go_command(enum n_go_input_flags gif, char const *cmd){
   struct a_go_ctx *gcp;
   bool_t rv;
   size_t i, ial;
   sigset_t osigmask;
   NYD_ENTER;

   sigprocmask(SIG_BLOCK, NULL, &osigmask);

   i = strlen(cmd) +1;
   ial = n_ALIGN(i);
   gcp = n_alloc(n_VSTRUCT_SIZEOF(struct a_go_ctx, gc_name) +
         ial + 2*sizeof(char*));
   memset(gcp, 0, n_VSTRUCT_SIZEOF(struct a_go_ctx, gc_name));

   hold_all_sigs();

   gcp->gc_outer = a_go_ctx;
   gcp->gc_osigmask = osigmask;
   gcp->gc_flags = a_GO_FREE | a_GO_MACRO | a_GO_MACRO_CMD |
         ((!(a_go_ctx->gc_flags & a_GO_TYPE_MASK) ||
            (a_go_ctx->gc_flags & a_GO_SUPER_MACRO)) ? a_GO_SUPER_MACRO : 0);
   gcp->gc_lines = (void*)&gcp->gc_name[ial];
   memcpy(gcp->gc_lines[0] = &gcp->gc_name[0], cmd, i);
   gcp->gc_lines[1] = NULL;

   a_go_ctx = gcp;
   n_go_data = &gcp->gc_data;
   n_pstate |= n_PS_ROBOT;
   rv = a_go_event_loop(gcp, gif);
   NYD_LEAVE;
   return rv;
}

FL void
n_go_splice_hack(char const *cmd, FILE *new_stdin, FILE *new_stdout,
      ui32_t new_psonce, void (*on_finalize)(void*), void *finalize_arg){
   struct a_go_ctx *gcp;
   size_t i;
   sigset_t osigmask;
   NYD_ENTER;

   sigprocmask(SIG_BLOCK, NULL, &osigmask);

   gcp = n_alloc(n_VSTRUCT_SIZEOF(struct a_go_ctx, gc_name) +
         (i = strlen(cmd) +1));
   memset(gcp, 0, n_VSTRUCT_SIZEOF(struct a_go_ctx, gc_name));

   hold_all_sigs();

   gcp->gc_outer = a_go_ctx;
   gcp->gc_osigmask = osigmask;
   gcp->gc_file = new_stdin;
   gcp->gc_flags = a_GO_FREE | a_GO_SPLICE;
   gcp->gc_on_finalize = on_finalize;
   gcp->gc_finalize_arg = finalize_arg;
   gcp->gc_splice_stdin = n_stdin;
   gcp->gc_splice_stdout = n_stdout;
   gcp->gc_splice_psonce = n_psonce;
   memcpy(gcp->gc_name, cmd, i);

   n_stdin = new_stdin;
   n_stdout = new_stdout;
   n_psonce = new_psonce;
   a_go_ctx = gcp;
   n_pstate |= n_PS_ROBOT;

   rele_all_sigs();
   NYD_LEAVE;
}

FL void
n_go_splice_hack_remove_after_jump(void){
   a_go_cleanup(a_GO_CLEANUP_TEARDOWN);
}

FL bool_t
n_go_may_yield_control(void){ /* TODO this is a terrible hack */
   struct a_go_ctx *gcp;
   bool_t rv;
   NYD2_ENTER;

   rv = FAL0;

   /* Only when startup completed */
   if(!(n_psonce & n_PSO_STARTED))
      goto jleave;
   /* Only interactive or batch mode (assuming that is ok) */
   if(!(n_psonce & n_PSO_INTERACTIVE) && !(n_poption & n_PO_BATCH_FLAG))
      goto jleave;

   /* Not when running any hook */
   if(n_pstate & n_PS_HOOK_MASK)
      goto jleave;

   /* Traverse up the stack:
    * . not when controlled by a child process
    * TODO . not when there are pipes involved, we neither handle job control,
    * TODO   nor process groups, that is, controlling terminal acceptably
    * . not when sourcing a file */
   for(gcp = a_go_ctx; gcp != NULL; gcp = gcp->gc_outer){
      if(gcp->gc_flags & (a_GO_PIPE | a_GO_FILE | a_GO_SPLICE))
         goto jleave;
   }

   rv = TRU1;
jleave:
   NYD2_LEAVE;
   return rv;
}

FL int
c_eval(void *vp){
   /* TODO HACK! `eval' should be nothing else but a command prefix, evaluate
    * TODO ARGV with shell rules, but if that is not possible then simply
    * TODO adjust argv/argc of "the CmdCtx" that we will have "exec" real cmd */
   struct a_go_eval_ctx gec;
   struct n_string s_b, *sp;
   size_t i, j;
   char const **argv, *cp;
   NYD_ENTER;

   argv = vp;

   for(j = i = 0; (cp = argv[i]) != NULL; ++i)
      j += strlen(cp);

   sp = n_string_creat_auto(&s_b);
   sp = n_string_reserve(sp, j);

   for(i = 0; (cp = argv[i]) != NULL; ++i){
      if(i > 0)
         sp = n_string_push_c(sp, ' ');
      sp = n_string_push_cp(sp, cp);
   }

   memset(&gec, 0, sizeof gec);
   gec.gec_line.s = n_string_cp(sp);
   gec.gec_line.l = sp->s_len;
   if(n_poption & n_PO_D_VV)
      n_err(_("EVAL %" PRIuZ " bytes <%s>\n"), gec.gec_line.l, gec.gec_line.s);
   (void)/* XXX */a_go_evaluate(&gec);
   NYD_LEAVE;
   return (a_go_xcall != NULL ? 0 : n_pstate_ex_no);
}

FL int
c_xcall(void *vp){
   int rv;
   struct a_go_ctx *gcp;
   NYD2_ENTER;

   /* The context can only be a macro context, except that possibly a single
    * level of `eval' (TODO: yet) was used to double-expand our arguments */
   if((gcp = a_go_ctx)->gc_flags & a_GO_MACRO_CMD)
      gcp = gcp->gc_outer;
   if((gcp->gc_flags & (a_GO_MACRO | a_GO_MACRO_X_OPTION | a_GO_MACRO_CMD)
         ) != a_GO_MACRO){
      if(n_poption & n_PO_D_V_VV)
         n_err(_("`xcall': can only be used inside a macro, using `call'\n"));
      rv = c_call(vp);
      goto jleave;
   }

   /* Try to roll up the stack as much as possible.
    * See a_GO_XCALL_LOOP flag description for more */
   if(!(gcp->gc_flags & a_GO_XCALL_IS_CALL) && gcp->gc_outer != NULL){
      if(gcp->gc_outer->gc_flags & a_GO_XCALL_LOOP)
         gcp = gcp->gc_outer;
   }else{
      /* Otherwise this macro is "invoked from the top level", in which case we
       * silently act as if we were `call'... */
      rv = c_call(vp);
      /* ...which means we must ensure the rest of the macro that was us
       * doesn't become evaluated! */
      a_go_xcall = (void*)-1;
      goto jleave;
   }

   /* C99 */{
      struct n_cmd_arg_ctx *cacp;

      cacp = n_cmd_arg_save_to_heap(vp);
      cacp->cac_indat = (char*)gcp;
      a_go_xcall = cacp;
   }
   rv = 0;
jleave:
   NYD2_LEAVE;
   return rv;
}

FL int
c_exit(void *vp){
   char const **argv;
   NYD_ENTER;

   if(*(argv = vp) != NULL && (n_idec_si32_cp(&n_exit_status, *argv, 0, NULL) &
            (n_IDEC_STATE_EMASK | n_IDEC_STATE_CONSUMED)
         ) != n_IDEC_STATE_CONSUMED)
      n_exit_status |= n_EXIT_ERR;

   if(n_pstate & n_PS_COMPOSE_FORKHOOK){ /* TODO sic */
      fflush(NULL);
      _exit(n_exit_status);
   }else if(n_pstate & n_PS_COMPOSE_MODE) /* XXX really.. */
      n_err(_("`exit' delayed until compose mode is left\n")); /* XXX ..log? */
   n_psonce |= n_PSO_XIT;
   NYD_LEAVE;
   return 0;
}

FL int
c_quit(void *vp){
   char const **argv;
   NYD_ENTER;

   if(*(argv = vp) != NULL && (n_idec_si32_cp(&n_exit_status, *argv, 0, NULL) &
            (n_IDEC_STATE_EMASK | n_IDEC_STATE_CONSUMED)
         ) != n_IDEC_STATE_CONSUMED)
      n_exit_status |= n_EXIT_ERR;

   if(n_pstate & n_PS_COMPOSE_FORKHOOK){ /* TODO sic */
      fflush(NULL);
      _exit(n_exit_status);
   }else if(n_pstate & n_PS_COMPOSE_MODE) /* XXX really.. */
      n_err(_("`exit' delayed until compose mode is left\n")); /* XXX ..log? */
   n_psonce |= n_PSO_QUIT;
   NYD_LEAVE;
   return 0;
}

FL int
c_readctl(void *vp){
   /* TODO We would need OnForkEvent and then simply remove some internal
    * TODO management; we don't have this, therefore we need global
    * TODO n_readctl_overlay to be accessible via =NULL, and to make that
    * TODO work in turn we need an instance for default STDIN!  Sigh. */
   static ui8_t a_buf[n_VSTRUCT_SIZEOF(struct a_go_readctl_ctx, grc_name)+1 +1];
   static struct a_go_readctl_ctx *a_stdin;

   struct a_go_readctl_ctx *grcp;
   char const *emsg;
   enum{
      a_NONE = 0,
      a_ERR = 1u<<0,
      a_SET = 1u<<1,
      a_CREATE = 1u<<2,
      a_REMOVE = 1u<<3
   } f;
   struct n_cmd_arg *cap;
   struct n_cmd_arg_ctx *cacp;
   NYD_ENTER;

   if(a_stdin == NULL){
      a_stdin = (struct a_go_readctl_ctx*)a_buf;
      a_stdin->grc_name[0] = '-';
      n_readctl_overlay = a_stdin;
   }

   n_pstate_err_no = n_ERR_NONE;
   cacp = vp;
   cap = cacp->cac_arg;

   if(cacp->cac_no == 0 || is_asccaseprefix(cap->ca_arg.ca_str.s, "show"))
      goto jshow;
   else if(is_asccaseprefix(cap->ca_arg.ca_str.s, "set"))
      f = a_SET;
   else if(is_asccaseprefix(cap->ca_arg.ca_str.s, "create"))
      f = a_CREATE;
   else if(is_asccaseprefix(cap->ca_arg.ca_str.s, "remove"))
      f = a_REMOVE;
   else{
      emsg = N_("`readctl': invalid subcommand: %s\n");
      goto jeinval_quote;
   }

   if(cacp->cac_no == 1){ /* TODO better option parser <> subcommand */
      n_err(_("`readctl': %s: requires argument\n"), cap->ca_arg.ca_str.s);
      goto jeinval;
   }
   cap = cap->ca_next;

   /* - is special TODO unfortunately also regarding storage */
   if(cap->ca_arg.ca_str.l == 1 && *cap->ca_arg.ca_str.s == '-'){
      if(f & (a_CREATE | a_REMOVE)){
         n_err(_("`readctl': cannot create nor remove -\n"));
         goto jeinval;
      }
      n_readctl_overlay = a_stdin;
      goto jleave;
   }

   /* Try to find a yet existing instance */
   if((grcp = n_readctl_overlay) != NULL){
      for(; grcp != NULL; grcp = grcp->grc_next)
         if(!strcmp(grcp->grc_name, cap->ca_arg.ca_str.s))
            goto jfound;
      for(grcp = n_readctl_overlay; (grcp = grcp->grc_last) != NULL;)
         if(!strcmp(grcp->grc_name, cap->ca_arg.ca_str.s))
            goto jfound;
   }

   if(f & (a_SET | a_REMOVE)){
      emsg = N_("`readctl': no such channel: %s\n");
      goto jeinval_quote;
   }

jfound:
   if(f & a_SET)
      n_readctl_overlay = grcp;
   else if(f & a_REMOVE){
      if(n_readctl_overlay == grcp)
         n_readctl_overlay = a_stdin;

      if(grcp->grc_last != NULL)
         grcp->grc_last->grc_next = grcp->grc_next;
      if(grcp->grc_next != NULL)
         grcp->grc_next->grc_last = grcp->grc_last;
      fclose(grcp->grc_fp);
      n_free(grcp);
   }else{
      FILE *fp;
      size_t elen;
      si32_t fd;

      if(grcp != NULL){
         n_err(_("`readctl': channel already exists: %s\n"), /* TODO reopen */
            n_shexp_quote_cp(cap->ca_arg.ca_str.s, FAL0));
         n_pstate_err_no = n_ERR_EXIST;
         f = a_ERR;
         goto jleave;
      }

      if((n_idec_si32_cp(&fd, cap->ca_arg.ca_str.s, 0, NULL
               ) & (n_IDEC_STATE_EMASK | n_IDEC_STATE_CONSUMED)
            ) != n_IDEC_STATE_CONSUMED){
         if((emsg = fexpand(cap->ca_arg.ca_str.s, FEXP_LOCAL | FEXP_NVAR)
               ) == NULL){
            emsg = N_("`readctl': cannot expand filename %s\n");
            goto jeinval_quote;
         }
         fd = -1;
         elen = strlen(emsg);
         fp = safe_fopen(emsg, "r", NULL);
      }else if(fd == STDIN_FILENO || fd == STDOUT_FILENO ||
            fd == STDERR_FILENO){
         n_err(_("`readctl': create: standard descriptors are not allowed"));
         goto jeinval;
      }else{
         /* xxx Avoid */
         _CLOEXEC_SET(fd);
         emsg = NULL;
         elen = 0;
         fp = fdopen(fd, "r");
      }

      if(fp != NULL){
         size_t i;

         if((i = UIZ_MAX - elen) <= cap->ca_arg.ca_str.l ||
               (i -= cap->ca_arg.ca_str.l) <=
                  n_VSTRUCT_SIZEOF(struct a_go_readctl_ctx, grc_name) +2){
            n_err(_("`readctl': failed to create storage for %s\n"),
               cap->ca_arg.ca_str.s);
            n_pstate_err_no = n_ERR_OVERFLOW;
            f = a_ERR;
            goto jleave;
         }

         grcp = n_alloc(n_VSTRUCT_SIZEOF(struct a_go_readctl_ctx, grc_name) +
               cap->ca_arg.ca_str.l +1 + elen +1);
         grcp->grc_last = NULL;
         if((grcp->grc_next = n_readctl_overlay) != NULL)
            grcp->grc_next->grc_last = grcp;
         n_readctl_overlay = grcp;
         grcp->grc_fp = fp;
         grcp->grc_fd = fd;
         memcpy(grcp->grc_name, cap->ca_arg.ca_str.s, cap->ca_arg.ca_str.l +1);
         if(elen == 0)
            grcp->grc_expand = NULL;
         else{
            char *cp;

            grcp->grc_expand = cp = &grcp->grc_name[cap->ca_arg.ca_str.l +1];
            memcpy(cp, emsg, ++elen);
         }
      }else{
         emsg = N_("`readctl': failed to create file for %s\n");
         goto jeinval_quote;
      }
   }

jleave:
   NYD_LEAVE;
   return (f & a_ERR) ? 1 : 0;
jeinval_quote:
   n_err(V_(emsg), n_shexp_quote_cp(cap->ca_arg.ca_str.s, FAL0));
jeinval:
   n_pstate_err_no = n_ERR_INVAL;
   f = a_ERR;
   goto jleave;

jshow:
   if((grcp = n_readctl_overlay) == NULL)
      fprintf(n_stdout, _("`readctl': no channels registered\n"));
   else{
      while(grcp->grc_last != NULL)
         grcp = grcp->grc_last;

      fprintf(n_stdout, _("`readctl': registered channels:\n"));
      for(; grcp != NULL; grcp = grcp->grc_next)
         fprintf(n_stdout, _("%c%s %s%s%s%s\n"),
            (grcp == n_readctl_overlay ? '*' : ' '),
            (grcp->grc_fd != -1 ? _("descriptor") : _("name")),
            n_shexp_quote_cp(grcp->grc_name, FAL0),
            (grcp->grc_expand != NULL ? " (" : n_empty),
            (grcp->grc_expand != NULL ? grcp->grc_expand : n_empty),
            (grcp->grc_expand != NULL ? ")" : n_empty));
   }
   f = a_NONE;
   goto jleave;
}

/* s-it-mode */
