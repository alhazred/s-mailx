/*@ S-nail - a mail user agent derived from Berkeley Mail.
 *@ Account, macro and variable handling.
 *@ HOWTO add a new non-dynamic boolean or value option:
 *@ - add an entry to nail.h:enum okeys
 *@ - run mk-okey-map.pl
 *@ - update the manual!
 *@ TODO . should be recursive environment based.
 *@ TODO   Otherwise, the `localopts' should be an attribute of the lex_input.c
 *@ TODO   command context, so that it belongs to the execution context
 *@ TODO   we are running in, instead of being global data.  See, e.g.,
 *@ TODO   the a_LEX_SLICE comment in lex_input.c.
 *@ TODO . undefining and overwriting a macro should always be possible:
 *@ TODO   simply place the thing in a delete-later list and replace the
 *@ TODO   accessible entry!  (instant delete if on top recursion level.)
 *@ TODO . Likewise, overwriting an existing should be like delete+create
 *@ TODO . once we can have non-fatal !0 returns for commands, we should
 *@ TODO   return error if "(environ)? unset" goes for non-existent.
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
#define n_FILE accmacvar

#ifndef HAVE_AMALGAMATION
# include "nail.h"
#endif

#if !defined HAVE_SETENV && !defined HAVE_PUTENV
# error Exactly one of HAVE_SETENV and HAVE_PUTENV
#endif

/* Note: changing the hash function must be reflected in mk-okey-map.pl */
#define a_AMV_PRIME HSHSIZE
#define a_AMV_NAME2HASH(N) torek_hash(N)
#define a_AMV_HASH2PRIME(H) ((H) % a_AMV_PRIME)

enum a_amv_mac_flags{
   a_AMV_MF_NONE = 0,
   a_AMV_MF_ACC = 1<<0,    /* This macro is an `account' */
   a_AMV_MF_TYPE_MASK = a_AMV_MF_ACC,
   a_AMV_MF_UNDEF = 1<<1,  /* Unlink after lookup */
   a_AMV_MF_DEL = 1<<7,    /* Current `account': deleted while active */
   a_AMV_MF__MAX = 0xFF
};

/* mk-okey-map.pl ensures that _VIRT implies _RDONLY and _NODEL, and that
 * _IMPORT implies _ENV; it doesn't verify anything... */
enum a_amv_var_flags{
   a_AMV_VF_NONE = 0,
   a_AMV_VF_BOOL = 1<<0,      /* ok_b_* */
   a_AMV_VF_VIRT = 1<<1,      /* "Stateless" automatic variable */
   a_AMV_VF_NOLOPTS = 1<<2,   /* May not be tracked by `localopts' */
   a_AMV_VF_RDONLY = 1<<3,    /* May not be set by user */
   a_AMV_VF_NODEL = 1<<4,     /* May not be deleted */
   a_AMV_VF_NOTEMPTY = 1<<5,  /* May not be assigned an empty value */
   a_AMV_VF_NOCNTRLS = 1<<6,  /* Value may not contain control characters */
   a_AMV_VF_NUM = 1<<7,       /* Value must be a 32-bit number */
   a_AMV_VF_POSNUM = 1<<8,    /* Value must be positive 32-bit number */
   a_AMV_VF_LOWER = 1<<9,     /* Values will be stored in a lowercase version */
   a_AMV_VF_VIP = 1<<10,      /* Wants _var_check_vips() evaluation */
   a_AMV_VF_IMPORT = 1<<11,   /* Import ONLY from environ (pre n_PSO_STARTED) */
   a_AMV_VF_ENV = 1<<12,      /* Update environment on change */
   a_AMV_VF_I3VAL = 1<<13,    /* Has an initial value */
   a_AMV_VF_DEFVAL = 1<<14,   /* Has a default value */
   a_AMV_VF_LINKED = 1<<15,   /* `environ' linked */
   a_AMV_VF__MASK = (1<<(15+1)) - 1
};


/* We support some special parameter names for one-letter variable names;
 * note these have counterparts in the code that manages shell expansion! */
enum a_amv_var_special_type{
   a_AMV_VST_STAR,   /* * */
   a_AMV_VST_AT,     /* @ */
   a_AMV_VST_NOSIGN  /* # */
   /* ?  This is different in that it is not a macro-local variable, but in
    * fact a real variable! */
   /*a_AMV_VST_QM*/
};

struct a_amv_mac{
   struct a_amv_mac *am_next;
   ui32_t am_maxlen;             /* of any line in .am_line_dat */
   ui32_t am_line_cnt;           /* of *.am_line_dat (but NULL terminated) */
   struct a_amv_mac_line **am_line_dat; /* TODO use deque? */
   struct a_amv_var *am_lopts;   /* `localopts' unroll list */
   ui8_t am_flags;               /* enum a_amv_mac_flags */
   char am_name[n_VFIELD_SIZE(7)]; /* of this macro */
};
n_CTA(a_AMV_MF__MAX <= UI8_MAX, "Enumeration excesses storage datatype");

struct a_amv_mac_line{
   ui32_t aml_len;
   ui32_t aml_prespc;   /* Number of leading SPC, for display purposes */
   char aml_dat[n_VFIELD_SIZE(0)];
};

struct a_amv_mac_call_args{
   char const *amca_name;
   struct a_amv_mac const *amca_amp;
   struct a_amv_var **amca_unroller;
   void (*amca_hook_pre)(void *);
   void *amca_hook_arg;
   bool_t amca_lopts_on;
   bool_t amca_ps_hook_mask;
   ui8_t amca__pad[4];
   ui16_t amca_argc;                /* Max is SI16_MAX */
   char const **amca_argv;
};

struct a_amv_lostack{
   struct a_amv_lostack *as_global_saved; /* Saved global XXX due to jump */
   struct a_amv_mac_call_args *as_amcap;
   struct a_amv_lostack *as_up;  /* Outer context */
   struct a_amv_var *as_lopts;
   bool_t as_unroll;             /* Unrolling enabled? */
   ui8_t avs__pad[7];
};

struct a_amv_var{
   struct a_amv_var *av_link;
   char *av_value;
#ifdef HAVE_PUTENV
   char *av_env;              /* Actively managed putenv(3) memory */
#endif
   ui16_t av_flags;           /* enum a_amv_var_flags */
   char av_name[n_VFIELD_SIZE(6)];
};
n_CTA(a_AMV_VF__MASK <= UI16_MAX, "Enumeration excesses storage datatype");

struct a_amv_var_map{
   ui32_t avm_hash;
   ui16_t avm_keyoff;
   ui16_t avm_flags;    /* enum a_amv_var_flags */
};
n_CTA(a_AMV_VF__MASK <= UI16_MAX, "Enumeration excesses storage datatype");

struct a_amv_var_virt{
   ui32_t avv_okey;
   ui8_t avv__dummy[4];
   struct a_amv_var const *avv_var;
};

struct a_amv_var_defval{
   ui32_t avdv_okey;
   ui8_t avdv__pad[4];
   char const *avdv_value; /* Only for !BOOL (otherwise plain existence) */
};

struct a_amv_var_carrier{
   char const *avc_name;
   ui32_t avc_hash;
   ui32_t avc_prime;
   struct a_amv_var *avc_var;
   struct a_amv_var_map const *avc_map;
   enum okeys avc_okey;
   ui8_t avc__pad[1];
   bool_t avc_is_special;     /* Only if avc_okey == ok_v___special_param */
   /* Numeric if .avc_is_special==TRUM1, else enum a_amv_var_special_type */
   ui16_t avc_special_prop;
};

/* Include the constant mk-okey-map.pl output */
#include "version.h"
#include "okeys.h"

/* True boolean visualization: this string will not be copied to heap memory
 * in a_amv_var_copy(), but we must avoid confusion with identical user data.
 * While here, add a special "0" one and speed up *?* assignments! */
static char const a_amv_var_1[] = "1";
static char const a_amv_var_0[] = "0";

/* The currently active account */
static struct a_amv_mac *a_amv_acc_curr;

static struct a_amv_mac *a_amv_macs[a_AMV_PRIME]; /* TODO dynamically spaced */

/* Unroll list of currently running macro stack */
static struct a_amv_lostack *a_amv_lopts;

static struct a_amv_var *a_amv_vars[a_AMV_PRIME]; /* TODO dynamically spaced */

/* TODO We really deserve localopts support for *folder-hook*s, so hack it in
 * TODO today via a static lostack, it should be a field in mailbox, once that
 * TODO is a real multi-instance object */
static struct a_amv_var *a_amv_folder_hook_lopts;

/* TODO Rather ditto (except for storage -> cmd_ctx), compose hooks */
static struct a_amv_var *a_amv_compose_lopts;

/* Lookup for macros/accounts */
static struct a_amv_mac *a_amv_mac_lookup(char const *name,
                           struct a_amv_mac *newamp, enum a_amv_mac_flags amf);

/* `call', `call_if' */
static int a_amv_mac_call(void *v, bool_t silent_nexist);

/* Execute a macro; amcap must reside in LOFI memory */
static bool_t a_amv_mac_exec(struct a_amv_mac_call_args *amcap);

static void a_amv_mac__finalize(void *vp);

/* User display helpers */
static bool_t a_amv_mac_show(enum a_amv_mac_flags amf);

/* _def() returns error for faulty definitions and already existing * names,
 * _undef() returns error if a named thing doesn't exist */
static bool_t a_amv_mac_def(char const *name, enum a_amv_mac_flags amf);
static bool_t a_amv_mac_undef(char const *name, enum a_amv_mac_flags amf);

/* */
static void a_amv_mac_free(struct a_amv_mac *amp);

/* Update replay-log */
static void a_amv_lopts_add(struct a_amv_lostack *alp, char const *name,
               struct a_amv_var *oavp);
static void a_amv_lopts_unroll(struct a_amv_var **avpp);

/* Special cased value string allocation */
static char *a_amv_var_copy(char const *str);
static void a_amv_var_free(char *cp);

/* Check for special housekeeping */
static bool_t a_amv_var_check_vips(enum okeys okey, bool_t enable, char **val);

/* _VF_NOCNTRLS, _VF_NUM / _VF_POSNUM */
static bool_t a_amv_var_check_nocntrls(char const *val);
static bool_t a_amv_var_check_num(char const *val, bool_t pos);

/* If a variable name begins with a lowercase-character and contains at
 * least one '@', it is converted to all-lowercase. This is necessary
 * for lookups of names based on email addresses.
 * Following the standard, only the part following the last '@' should
 * be lower-cased, but practice has established otherwise here */
static char const *a_amv_var_canonify(char const *vn);

/* Try to reverse lookup an option name to an enum okeys mapping.
 * Updates .avc_name and .avc_hash; .avc_map is NULL if none found */
static bool_t a_amv_var_revlookup(struct a_amv_var_carrier *avcp,
               char const *name);

/* Lookup a variable from .avc_(map|name|hash), return whether it was found.
 * Sets .avc_prime; .avc_var is NULL if not found.
 * Here it is where we care for _I3VAL and _DEFVAL, too.
 * An _I3VAL will be "consumed" as necessary anyway, but it won't be used to
 * create a new variable if i3val_nonew is true; if i3val_nonew is TRUM1 then
 * we set .avc_var to -1 and return true if that was the case, otherwise we'll
 * return FAL0, then! */
static bool_t a_amv_var_lookup(struct a_amv_var_carrier *avcp,
               bool_t i3val_nonew);

/* Set var from .avc_(map|name|hash), return success */
static bool_t a_amv_var_set(struct a_amv_var_carrier *avcp, char const *value,
               bool_t force_env);

static bool_t a_amv_var__putenv(struct a_amv_var_carrier *avcp,
               struct a_amv_var *avp);

/* Clear var from .avc_(map|name|hash); sets .avc_var=NULL, return success */
static bool_t a_amv_var_clear(struct a_amv_var_carrier *avcp, bool_t force_env);

static bool_t a_amv_var__clearenv(char const *name, char *value);

/* List all variables */
static void a_amv_var_show_all(void);

static int a_amv_var__show_cmp(void const *s1, void const *s2);

/* Actually do print one, return number of lines written */
static size_t a_amv_var_show(char const *name, FILE *fp, struct n_string *msgp);

/* Shared c_set() and c_environ():set impl, return success */
static bool_t a_amv_var_c_set(char **ap, bool_t issetenv);

static struct a_amv_mac *
a_amv_mac_lookup(char const *name, struct a_amv_mac *newamp,
      enum a_amv_mac_flags amf){
   struct a_amv_mac *amp, **ampp;
   ui32_t h;
   enum a_amv_mac_flags save_amf;
   NYD2_ENTER;

   save_amf = amf;
   amf &= a_AMV_MF_TYPE_MASK;
   h = a_AMV_NAME2HASH(name);
   h = a_AMV_HASH2PRIME(h);
   ampp = &a_amv_macs[h];

   for(amp = *ampp; amp != NULL; ampp = &(*ampp)->am_next, amp = amp->am_next){
      if((amp->am_flags & a_AMV_MF_TYPE_MASK) == amf &&
            !strcmp(amp->am_name, name)){
         if(n_LIKELY((save_amf & a_AMV_MF_UNDEF) == 0))
            goto jleave;

         *ampp = amp->am_next;

         if((amf & a_AMV_MF_ACC) &&
               account_name != NULL && !strcmp(account_name, name)){
            amp->am_flags |= a_AMV_MF_DEL;
            n_err(_("Delayed deletion of active account: %s\n"), name);
         }else{
            a_amv_mac_free(amp);
            amp = (struct a_amv_mac*)-1;
         }
         goto jleave;
      }
   }

   if(newamp != NULL){
      ampp = &a_amv_macs[h];
      newamp->am_next = *ampp;
      *ampp = newamp;
      amp = NULL;
   }
jleave:
   NYD2_LEAVE;
   return amp;
}

static int
a_amv_mac_call(void *v, bool_t silent_nexist){
   int rv;
   struct a_amv_mac *amp;
   char const *name;
   NYD_ENTER;

   name = *(char const**)v;

   if((amp = a_amv_mac_lookup(name, NULL, a_AMV_MF_NONE)) != NULL){
      struct a_amv_mac_call_args *amcap;

      amcap = n_lofi_alloc(sizeof *amcap);
      memset(amcap, 0, sizeof *amcap);
      amcap->amca_name = name;
      amcap->amca_amp = amp;
      /* C99 */{
         char const **argv;
         ui32_t argc;

         for(argc = 0, argv = v; *++argv != NULL; ++argc)
            ;
         if(argc > 0){
            amcap->amca_argc = argc;
            amcap->amca_argv = &(argv = v)[1];
         }
      }
      rv = (a_amv_mac_exec(amcap) == FAL0);
   }else if((rv = (silent_nexist == FAL0)))
      n_err(_("Undefined macro `call'ed: %s\n"), n_shexp_quote_cp(name, FAL0));
   NYD_LEAVE;
   return rv;
}

static bool_t
a_amv_mac_exec(struct a_amv_mac_call_args *amcap){
   struct a_amv_lostack *losp;
   struct a_amv_mac_line **amlp;
   char **args_base, **args;
   struct a_amv_mac const *amp;
   bool_t rv;
   NYD2_ENTER;

   amp = amcap->amca_amp;
   /* XXX Unfortunately we yet need to dup the macro lines! :( */
   args_base = args = smalloc(sizeof(*args) * (amp->am_line_cnt +1));
   for(amlp = amp->am_line_dat; *amlp != NULL; ++amlp)
      *(args++) = sbufdup((*amlp)->aml_dat, (*amlp)->aml_len);
   *args = NULL;

   losp = n_lofi_alloc(sizeof *losp);
   losp->as_global_saved = a_amv_lopts;
   if((losp->as_amcap = amcap)->amca_unroller == NULL){
      losp->as_up = losp->as_global_saved;
      losp->as_lopts = NULL;
   }else{
      losp->as_up = NULL;
      losp->as_lopts = *amcap->amca_unroller;
   }
   losp->as_unroll = amcap->amca_lopts_on;

   a_amv_lopts = losp;
   if(amcap->amca_hook_pre != NULL){
      bool_t reset;

      reset = !(n_pstate & n_PS_ROOT);
      n_pstate |= n_PS_ROOT;
      (*amcap->amca_hook_pre)(amcap->amca_hook_arg);
      if(reset)
         n_pstate &= ~n_PS_ROOT;
   }
   rv = n_source_macro(n_LEXINPUT_NONE, amp->am_name, args_base,
         &a_amv_mac__finalize, losp);
   NYD2_LEAVE;
   return rv;
}

static void
a_amv_mac__finalize(void *vp){
   struct a_amv_mac_call_args *amcap;
   struct a_amv_lostack *losp;
   NYD2_ENTER;

   losp = vp;
   a_amv_lopts = losp->as_global_saved;

   if((amcap = losp->as_amcap)->amca_unroller == NULL){
      if(losp->as_lopts != NULL)
         a_amv_lopts_unroll(&losp->as_lopts);
   }else
      *amcap->amca_unroller = losp->as_lopts;

   if(amcap->amca_ps_hook_mask)
      n_pstate &= ~n_PS_HOOK_MASK;

   n_lofi_free(losp);
   n_lofi_free(amcap);
   NYD2_LEAVE;
}

static bool_t
a_amv_mac_show(enum a_amv_mac_flags amf){
   size_t lc, mc, ti, i;
   char const *typestr;
   FILE *fp;
   bool_t rv;
   NYD2_ENTER;

   rv = FAL0;

   if((fp = Ftmp(NULL, "deflist", OF_RDWR | OF_UNLINK | OF_REGISTER)) ==
         NULL){
      n_perr(_("Can't create temporary file for `define' or `account' listing"),
         0);
      goto jleave;
   }

   amf &= a_AMV_MF_TYPE_MASK;
   typestr = (amf & a_AMV_MF_ACC) ? "account" : "define";

   for(lc = mc = ti = 0; ti < a_AMV_PRIME; ++ti){
      struct a_amv_mac *amp;

      for(amp = a_amv_macs[ti]; amp != NULL; amp = amp->am_next){
         if((amp->am_flags & a_AMV_MF_TYPE_MASK) == amf){
            struct a_amv_mac_line **amlpp;

            if(++mc > 1){
               putc('\n', fp);
               ++lc;
            }
            ++lc;
            fprintf(fp, "%s %s {\n", typestr, amp->am_name);
            for(amlpp = amp->am_line_dat; *amlpp != NULL; ++lc, ++amlpp){
               for(i = (*amlpp)->aml_prespc; i > 0; --i)
                  putc(' ', fp);
               fputs((*amlpp)->aml_dat, fp);
               putc('\n', fp);
            }
            fputs("}\n", fp);
            ++lc;
         }
      }
   }
   if(mc > 0)
      page_or_print(fp, lc);

   rv = (ferror(fp) == 0);
   Fclose(fp);
jleave:
   NYD2_LEAVE;
   return rv;
}

static bool_t
a_amv_mac_def(char const *name, enum a_amv_mac_flags amf){
   struct str line;
   ui32_t line_cnt, maxlen;
   struct linelist{
      struct linelist *ll_next;
      struct a_amv_mac_line *ll_amlp;
   } *llp, *ll_head, *ll_tail;
   union {size_t s; int i; ui32_t ui; size_t l;} n;
   struct a_amv_mac *amp;
   bool_t rv;
   NYD2_ENTER;

   memset(&line, 0, sizeof line);
   rv = FAL0;
   amp = NULL;

   /* TODO We should have our input state machine which emits Line events,
    * TODO and hook different consumers dependent on our content, as state
    * TODO in i think lex_input; */
   /* Read in the lines which form the macro content */
   for(ll_tail = ll_head = NULL, line_cnt = maxlen = 0;;){
      ui32_t leaspc;
      char *cp;

      n.i = n_lex_input(n_LEXINPUT_CTX_DEFAULT | n_LEXINPUT_NL_ESC, n_empty,
            &line.s, &line.l, NULL);
      if(n.ui == 0)
         continue;
      if(n.i < 0){
         n_err(_("Unterminated %s definition: %s\n"),
            (amf & a_AMV_MF_ACC ? "account" : "macro"), name);
         goto jerr;
      }

      /* Trim WS, remember amount of leading spaces for display purposes */
      for(cp = line.s, leaspc = 0; n.ui > 0; ++cp, --n.ui)
         if(*cp == '\t')
            leaspc = (leaspc + 8u) & ~7u;
         else if(*cp == ' ')
            ++leaspc;
         else
            break;
      for(; n.ui > 0 && spacechar(cp[n.ui - 1]); --n.ui)
         ;
      if(n.ui == 0)
         continue;

      maxlen = n_MAX(maxlen, n.ui);
      cp[n.ui++] = '\0';

      /* Is is the closing brace? */
      if(*cp == '}')
         break;

      if(n_LIKELY(++line_cnt < UI32_MAX)){
         struct a_amv_mac_line *amlp;

         llp = salloc(sizeof *llp);
         if(ll_head == NULL)
            ll_head = llp;
         else
            ll_tail->ll_next = llp;
         ll_tail = llp;
         llp->ll_next = NULL;
         llp->ll_amlp = amlp = smalloc(n_VSTRUCT_SIZEOF(struct a_amv_mac_line,
               aml_dat) + n.ui);
         amlp->aml_len = n.ui -1;
         amlp->aml_prespc = leaspc;
         memcpy(amlp->aml_dat, cp, n.ui);
      }else{
         n_err(_("Too much content in %s definition: %s\n"),
            (amf & a_AMV_MF_ACC ? "account" : "macro"), name);
         goto jerr;
      }
   }

   /* Create the new macro */
   n.s = strlen(name) +1;
   amp = smalloc(n_VSTRUCT_SIZEOF(struct a_amv_mac, am_name) + n.s);
   amp->am_next = NULL;
   amp->am_maxlen = maxlen;
   amp->am_line_cnt = line_cnt;
   amp->am_flags = amf;
   amp->am_lopts = NULL;
   memcpy(amp->am_name, name, n.s);
   /* C99 */{
      struct a_amv_mac_line **amlpp;

      amp->am_line_dat = amlpp = smalloc(sizeof(*amlpp) * ++line_cnt);
      for(llp = ll_head; llp != NULL; llp = llp->ll_next)
         *amlpp++ = llp->ll_amlp;
      *amlpp = NULL;
   }

   /* Finally check whether such a macro already exists, in which case we throw
    * it all away again.  At least we know it would have worked */
   if(a_amv_mac_lookup(name, amp, amf) != NULL){
      n_err(_("There is already a %s of name: %s\n"),
         (amf & a_AMV_MF_ACC ? "account" : "macro"), name);
      goto jerr;
   }

   rv = TRU1;
jleave:
   if(line.s != NULL)
      free(line.s);
   NYD2_LEAVE;
   return rv;

jerr:
   for(llp = ll_head; llp != NULL; llp = llp->ll_next)
      free(llp->ll_amlp);
   if(amp != NULL){
      free(amp->am_line_dat);
      free(amp);
   }
   goto jleave;
}

static bool_t
a_amv_mac_undef(char const *name, enum a_amv_mac_flags amf){
   struct a_amv_mac *amp;
   bool_t rv;
   NYD2_ENTER;

   rv = TRU1;

   if(n_LIKELY(name[0] != '*' || name[1] != '\0')){
      if((amp = a_amv_mac_lookup(name, NULL, amf | a_AMV_MF_UNDEF)) == NULL){
         n_err(_("%s not defined: %s\n"),
            (amf & a_AMV_MF_ACC ? "Account" : "Macro"), name);
         rv = FAL0;
      }
   }else{
      struct a_amv_mac **ampp, *lamp;

      for(ampp = a_amv_macs; PTRCMP(ampp, <, &a_amv_macs[n_NELEM(a_amv_macs)]);
            ++ampp)
         for(lamp = NULL, amp = *ampp; amp != NULL;){
            if((amp->am_flags & a_AMV_MF_TYPE_MASK) == amf){
               /* xxx Expensive but rare: be simple */
               a_amv_mac_lookup(amp->am_name, NULL, amf | a_AMV_MF_UNDEF);
               amp = (lamp == NULL) ? *ampp : lamp->am_next;
            }else{
               lamp = amp;
               amp = amp->am_next;
            }
         }
   }
   NYD2_LEAVE;
   return rv;
}

static void
a_amv_mac_free(struct a_amv_mac *amp){
   struct a_amv_mac_line **amlpp;
   NYD2_ENTER;

   for(amlpp = amp->am_line_dat; *amlpp != NULL; ++amlpp)
      free(*amlpp);
   free(amp->am_line_dat);
   free(amp);
   NYD2_LEAVE;
}

static void
a_amv_lopts_add(struct a_amv_lostack *alp, char const *name,
      struct a_amv_var *oavp){
   struct a_amv_var *avp;
   size_t nl, vl;
   NYD2_ENTER;

   /* Propagate unrolling up the stack, as necessary */
   assert(alp != NULL);
   for(;;){
      if(alp->as_unroll)
         break;
      if((alp = alp->as_up) == NULL)
         goto jleave;
   }

   /* Check whether this variable is handled yet */
   for(avp = alp->as_lopts; avp != NULL; avp = avp->av_link)
      if(!strcmp(avp->av_name, name))
         goto jleave;

   nl = strlen(name) +1;
   vl = (oavp != NULL) ? strlen(oavp->av_value) +1 : 0;
   avp = smalloc(n_VSTRUCT_SIZEOF(struct a_amv_var, av_name) + nl + vl);
   avp->av_link = alp->as_lopts;
   alp->as_lopts = avp;
   memcpy(avp->av_name, name, nl);
   if(vl == 0){
      avp->av_value = NULL;
      avp->av_flags = 0;
#ifdef HAVE_PUTENV
      avp->av_env = NULL;
#endif
   }else{
      avp->av_value = &avp->av_name[nl];
      avp->av_flags = oavp->av_flags;
      memcpy(avp->av_value, oavp->av_value, vl);
#ifdef HAVE_PUTENV
      avp->av_env = (oavp->av_env == NULL) ? NULL : sstrdup(oavp->av_env);
#endif
   }
jleave:
   NYD2_LEAVE;
}

static void
a_amv_lopts_unroll(struct a_amv_var **avpp){
   struct a_amv_lostack *save_alp;
   bool_t reset;
   struct a_amv_var *x, *avp;
   NYD2_ENTER;

   avp = *avpp;
   *avpp = NULL;
   reset = !(n_pstate & n_PS_ROOT);

   save_alp = a_amv_lopts;
   a_amv_lopts = NULL;
   while(avp != NULL){
      x = avp;
      avp = avp->av_link;
      n_pstate |= n_PS_ROOT;
      n_var_vset(x->av_name, (uintptr_t)x->av_value);
      if(reset)
         n_pstate &= ~n_PS_ROOT;
      free(x);
   }
   a_amv_lopts = save_alp;
   NYD2_LEAVE;
}

static char *
a_amv_var_copy(char const *str){
   char *news;
   size_t len;
   NYD2_ENTER;

   if(*str == '\0')
      news = n_UNCONST(n_empty);
   else if(str[1] == '\0'){
      if(str[0] == '1')
         news = n_UNCONST(a_amv_var_1);
      else if(str[0] == '0')
         news = n_UNCONST(a_amv_var_0);
      else
         goto jheap;
   }else{
jheap:
      len = strlen(str) +1;
      news = smalloc(len);
      memcpy(news, str, len);
   }
   NYD2_LEAVE;
   return news;
}

static void
a_amv_var_free(char *cp){
   NYD2_ENTER;
   if(cp[0] != '\0' && cp != a_amv_var_1 && cp != a_amv_var_0)
      free(cp);
   NYD2_LEAVE;
}

static bool_t
a_amv_var_check_vips(enum okeys okey, bool_t enable, char **val){
   int flag;
   bool_t ok, reset;
   NYD2_ENTER;

   ok = TRU1;
   reset = !(n_pstate & n_PS_ROOT);
   flag = 0;

   switch(okey){
   case ok_b_debug:
      flag = n_PO_DEBUG;
      break;
   case ok_v_HOME:
      /* Invalidate any resolved folder then, too
       * FALLTHRU */
   case ok_v_folder:
      n_pstate |= n_PS_ROOT;
      ok_vclear(_folder_resolved);
      if(reset)
         n_pstate &= ~n_PS_ROOT;
      break;
   case ok_b_header:
      flag = n_PO_N_FLAG;
      enable = !enable;
      break;
   case ok_b_memdebug:
      flag = n_PO_MEMDEBUG;
      break;
   case ok_b_POSIXLY_CORRECT:
      if(!(n_pstate & n_PS_ROOT)){
         n_pstate |= n_PS_ROOT;
         if(enable)
            ok_bset(posix);
         else
            ok_bclear(posix);
         if(reset)
            n_pstate &= ~n_PS_ROOT;
      }
      break;
   case ok_b_posix:
      if(!(n_pstate & n_PS_ROOT)){
         n_pstate |= n_PS_ROOT;
         if(enable)
            ok_bset(POSIXLY_CORRECT);
         else
            ok_bclear(POSIXLY_CORRECT);
         if(reset)
            n_pstate &= ~n_PS_ROOT;
      }
      break;
   case ok_b_skipemptybody:
      flag = n_PO_E_FLAG;
      break;
   case ok_b_typescript_mode:
      if(enable){
         ok_bset(colour_disable);
         ok_bset(line_editor_disable);
         if(!(n_psonce & n_PSO_STARTED))
            ok_bset(termcap_disable);
      }
   case ok_v_umask:
      assert(enable);
      if(**val != '\0'){
         ul_i ul;

         if((ul = strtoul(*val, NULL, 0)) & ~0777u){ /* (is valid _VF_POSNUM) */
            n_err(_("Invalid *umask* setting: %s\n"), *val);
            ok = FAL0;
         }else
            umask((mode_t)ul);
      }
      break;
   case ok_b_verbose:
      flag = (enable && !(n_poption & n_PO_VERB))
            ? n_PO_VERB : n_PO_VERB | n_PO_VERBVERB;
      break;
   default:
      DBG( n_err("Implementation error: never heard of %u\n", ok); )
      break;
   }

   if(flag){
      if(enable)
         n_poption |= flag;
      else
         n_poption &= ~flag;
   }
   NYD2_LEAVE;
   return ok;
}

static bool_t
a_amv_var_check_nocntrls(char const *val){
   char c;
   NYD2_ENTER;

   while((c = *val++) != '\0')
      if(cntrlchar(c))
         break;
   NYD2_LEAVE;
   return (c == '\0');
}

static bool_t
a_amv_var_check_num(char const *val, bool_t pos){ /* TODO intmax_t anywhere! */
   /* TODO The internal/environment  variables which are num= or posnum= should
    * TODO gain special lookup functions, or the return should be void* and
    * TODO castable to integer; i.e. no more strtoX() should be needed.
    * TODO I.e., the result of this function should instead be stored.
    * TODO Use intmax_t IF that is sizeof(void*) only? */
   bool_t rv;
   NYD2_ENTER;

   rv = TRU1;

   if(*val != '\0'){ /* Would be _VF_NOTEMPTY if not allowed */
      char *eptr;
      union {long s; unsigned long u;} i;

      if(!pos){
         i.s = strtol(val, &eptr, 0); /* TODO strtoimax() */

         if(*eptr != '\0' ||
               ((i.s == LONG_MIN || i.s == LONG_MAX) && errno == ERANGE))
            rv = FAL0;
#if INT_MIN != LONG_MIN
         else if(i.s < INT_MIN)
            rv = FAL0;
#endif
#if INT_MAX != LONG_MAX
         else if(i.s > INT_MAX)
            rv = FAL0;
#endif
      }else{
         i.u = strtoul(val, &eptr, 0); /* TODO strtoumax() */

         if(*eptr != '\0' || (i.u == ULONG_MAX && errno == ERANGE))
            rv = FAL0;
#if UINT_MAX != ULONG_MAX
         else if(i.u > UINT_MAX)
            rv = FAL0;
#endif
      }
   }
   NYD2_LEAVE;
   return rv;
}

static char const *
a_amv_var_canonify(char const *vn){
   NYD2_ENTER;
   if(!upperchar(*vn)){
      char const *vp;

      for(vp = vn; *vp != '\0' && *vp != '@'; ++vp)
         ;
      vn = (*vp == '@') ? i_strdup(vn) : vn;
   }
   NYD2_LEAVE;
   return vn;
}

static bool_t
a_amv_var_revlookup(struct a_amv_var_carrier *avcp, char const *name){
   ui32_t hash, i, j;
   struct a_amv_var_map const *avmp;
   char c;
   NYD2_ENTER;

   /* It may be a special a.k.a. macro-local or one-letter parameter */
   c = name[0];
   if(n_UNLIKELY(digitchar(c))){
      /* (Inline dec. atoi, ugh) */
      for(j = (ui8_t)c - '0', i = 1;; ++i){
         c = name[i];
         if(c == '\0')
            break;
         if(!digitchar(c))
            goto jno_special_param;
         j = j * 10 + (ui8_t)c - '0';
      }
      if(j == 0){
         /* Not function local, could also simply look it up, but faster */
         avcp->avc_name = name;
         avmp = &a_amv_var_map[a_AMV_VAR_RV_MAP_IDX];
         avcp->avc_hash = avmp->avm_hash;
         avcp->avc_map = avmp;
         avcp->avc_okey = ok_v___rv;
         avcp->avc_is_special = FAL0;
         goto jleave;
      }else if(j <= SI16_MAX){
         avcp->avc_is_special = TRUM1;
         goto jspecial_param_m1;
      }
   }else if(n_UNLIKELY(name[1] == '\0')){
      switch(c){
      case '*':
         j = a_AMV_VST_STAR;
         goto jspecial_param;
      case '@':
         j = a_AMV_VST_AT;
         goto jspecial_param;
      case '#':
         j = a_AMV_VST_NOSIGN;
         goto jspecial_param;
      case '?':
         /* Not function local, could also simply look it up, but faster */
         avcp->avc_name = name;
         avmp = &a_amv_var_map[a_AMV_VAR_QM_MAP_IDX];
         avcp->avc_hash = avmp->avm_hash;
         avcp->avc_map = avmp;
         avcp->avc_okey = ok_v___qm;
         avcp->avc_is_special = FAL0;
         goto jleave;
      default:
         break;
      }
   }

   /* Normal reverse lookup, walk over the hashtable */
jno_special_param:
   avcp->avc_is_special = FAL0;
   avcp->avc_name = name = a_amv_var_canonify(name);
   avcp->avc_hash = hash = a_AMV_NAME2HASH(name);

   for(i = hash % a_AMV_VAR_REV_PRIME, j = 0; j <= a_AMV_VAR_REV_LONGEST; ++j){
      ui32_t x;

      if((x = a_amv_var_revmap[i]) == a_AMV_VAR_REV_ILL)
         break;

      avmp = &a_amv_var_map[x];
      if(avmp->avm_hash == hash &&
            !strcmp(&a_amv_var_names[avmp->avm_keyoff], name)){
         avcp->avc_map = avmp;
         avcp->avc_okey = (enum okeys)x;
         goto jleave;
      }

      if(++i == a_AMV_VAR_REV_PRIME){
#ifdef a_AMV_VAR_REV_WRAPAROUND
         i = 0;
#else
         break;
#endif
      }
   }
   avcp->avc_map = NULL;
   avcp = NULL;
jleave:
   NYD2_LEAVE;
   return (avcp != NULL);

   /* All these are mapped to *--special-param* */
jspecial_param:
   avcp->avc_is_special = TRU1;
jspecial_param_m1:
   avcp->avc_name = name;
   avcp->avc_special_prop = (ui16_t)j;
   avmp = &a_amv_var_map[a_AMV_VAR_SPECIAL_PARAM_MAP_IDX];
   avcp->avc_hash = avmp->avm_hash;
   avcp->avc_map = avmp;
   avcp->avc_okey = ok_v___special_param;
   goto jleave;
}

static bool_t
a_amv_var_lookup(struct a_amv_var_carrier *avcp, bool_t i3val_nonew){
   size_t i;
   char const *cp;
   struct a_amv_var_map const *avmp;
   struct a_amv_var *avp;
   NYD2_ENTER;

   /* C99 */{
      struct a_amv_var **avpp, *lavp;

      avpp = &a_amv_vars[avcp->avc_prime = a_AMV_HASH2PRIME(avcp->avc_hash)];

      for(lavp = NULL, avp = *avpp; avp != NULL; lavp = avp, avp = avp->av_link)
         if(!strcmp(avp->av_name, avcp->avc_name)){
            /* Relink as head, hope it "sorts on usage" over time.
             * _clear() relies on this behaviour */
            if(lavp != NULL){
               lavp->av_link = avp->av_link;
               avp->av_link = *avpp;
               *avpp = avp;
            }
            goto jleave;
         }
   }

   /* If this is not an assembled variable we need to consider some special
    * initialization cases and eventually create the variable anew */
   if(n_LIKELY((avmp = avcp->avc_map) != NULL)){
      /* Does it have an import-from-environment flag? */
      if(n_UNLIKELY((avmp->avm_flags & (a_AMV_VF_IMPORT | a_AMV_VF_ENV)) != 0)){
         if(n_LIKELY((cp = getenv(avcp->avc_name)) != NULL)){
            /* May be better not to use that one, though? */
            bool_t isempty, isbltin;

            isempty = (*cp == '\0' &&
                  (avmp->avm_flags & a_AMV_VF_NOTEMPTY) != 0);
            isbltin = ((avmp->avm_flags & (a_AMV_VF_I3VAL | a_AMV_VF_DEFVAL)
                  ) != 0);

            if(n_UNLIKELY(isempty)){
               if(!isbltin)
                  goto jerr;
            }else if(n_LIKELY(*cp != '\0')){
                if(n_UNLIKELY((avmp->avm_flags & a_AMV_VF_NOCNTRLS) &&
                     !a_amv_var_check_nocntrls(cp))){
                  n_err(_("Ignoring environment, control characters "
                     "invalid in variable: %s\n"), avcp->avc_name);
                  goto jerr;
               }
               if(n_UNLIKELY((avmp->avm_flags & a_AMV_VF_NUM) &&
                     !a_amv_var_check_num(cp, FAL0))){
                  n_err(_("Environment variable value not a number "
                     "or out of range: %s\n"), avcp->avc_name);
                  goto jerr;
               }
               if(n_UNLIKELY((avmp->avm_flags & a_AMV_VF_POSNUM) &&
                     !a_amv_var_check_num(cp, TRU1))){
                  n_err(_("Environment variable value not a number, "
                     "negative or out of range: %s\n"), avcp->avc_name);
                  goto jerr;
               }
               goto jnewval;
            }else
               goto jnewval;
         }
      }

      /* A first-time init switch is to be handled now and here */
      if(n_UNLIKELY((avmp->avm_flags & a_AMV_VF_I3VAL) != 0)){
         static struct a_amv_var_defval const **arr,
            *arr_base[a_AMV_VAR_I3VALS_CNT +1];

         if(arr == NULL){
            arr = &arr_base[0];
            arr[i = a_AMV_VAR_I3VALS_CNT] = NULL;
            while(i-- > 0)
               arr[i] = &a_amv_var_i3vals[i];
         }

         for(i = 0; arr[i] != NULL; ++i)
            if(arr[i]->avdv_okey == avcp->avc_okey){
               cp = (avmp->avm_flags & a_AMV_VF_BOOL) ? n_empty
                     : arr[i]->avdv_value;
               /* Remove this entry, hope entire block becomes no-op asap */
               do
                  arr[i] = arr[i + 1];
               while(arr[i++] != NULL);

               if(!i3val_nonew)
                  goto jnewval;
               if(i3val_nonew == TRUM1)
                  avp = (struct a_amv_var*)-1;
               goto jleave;
            }
      }

      /* The virtual variables */
      if(n_UNLIKELY((avmp->avm_flags & a_AMV_VF_VIRT) != 0)){
         for(i = 0; i < a_AMV_VAR_VIRTS_CNT; ++i)
            if(a_amv_var_virts[i].avv_okey == avcp->avc_okey){
               avp = n_UNCONST(a_amv_var_virts[i].avv_var);
               goto jleave;
            }
         /* Not reached */
      }

      /* Place this last because once it is set first the variable will never
       * be removed again and thus match in the first block above */
      if(n_UNLIKELY(avmp->avm_flags & a_AMV_VF_DEFVAL) != 0){
         for(i = 0; i < a_AMV_VAR_DEFVALS_CNT; ++i)
            if(a_amv_var_defvals[i].avdv_okey == avcp->avc_okey){
               cp = (avmp->avm_flags & a_AMV_VF_BOOL) ? n_empty
                     : a_amv_var_defvals[i].avdv_value;
               goto jnewval;
            }
      }
   }

jerr:
   avp = NULL;
jleave:
   avcp->avc_var = avp;
   NYD2_LEAVE;
   return (avp != NULL);

jnewval: /* C99 */{
      struct a_amv_var **avpp;
      size_t l;

      l = strlen(avcp->avc_name) +1;
      avcp->avc_var =
      avp = smalloc(n_VSTRUCT_SIZEOF(struct a_amv_var, av_name) + l);
      avp->av_link = *(avpp = &a_amv_vars[avcp->avc_prime]);
      *avpp = avp;
      memcpy(avp->av_name, avcp->avc_name, l);
      avp->av_value = a_amv_var_copy(cp);
#ifdef HAVE_PUTENV
      avp->av_env = NULL;
#endif
      avp->av_flags = avmp->avm_flags;

      if(avp->av_flags & a_AMV_VF_VIP)
         a_amv_var_check_vips(avcp->avc_okey, TRU1, &avp->av_value);
      if(avp->av_flags & a_AMV_VF_ENV)
         a_amv_var__putenv(avcp, avp);
      goto jleave;
   }
}

static bool_t
a_amv_var_set(struct a_amv_var_carrier *avcp, char const *value,
      bool_t force_env){
   struct a_amv_var *avp;
   char *oval;
   struct a_amv_var_map const *avmp;
   bool_t rv;
   NYD2_ENTER;

   if(value == NULL){
      rv = a_amv_var_clear(avcp, force_env);
      goto jleave;
   }

   if((avmp = avcp->avc_map) != NULL){
      rv = FAL0;

      /* Validity checks */
      if(n_UNLIKELY((avmp->avm_flags & a_AMV_VF_RDONLY) != 0 &&
            !(n_pstate & n_PS_ROOT))){
         value = N_("Variable is readonly: %s\n");
         goto jeavmp;
      }
      if(n_UNLIKELY((avmp->avm_flags & a_AMV_VF_NOTEMPTY) && *value == '\0')){
         value = N_("Variable must not be empty: %s\n");
         goto jeavmp;
      }
      if(n_UNLIKELY((avmp->avm_flags & a_AMV_VF_NOCNTRLS) != 0 &&
            !a_amv_var_check_nocntrls(value))){
         value = N_("Variable forbids control characters: %s\n");
         goto jeavmp;
      }
      if(n_UNLIKELY((avmp->avm_flags & a_AMV_VF_NUM) &&
            !a_amv_var_check_num(value, FAL0))){
         value = N_("Variable value not a number or out of range: %s\n");
         goto jeavmp;
      }
      if(n_UNLIKELY((avmp->avm_flags & a_AMV_VF_POSNUM) &&
            !a_amv_var_check_num(value, TRU1))){
         value = _("Variable value not a number, negative, "
               "or out of range: %s\n");
         goto jeavmp;
      }
      if(n_UNLIKELY((avmp->avm_flags & a_AMV_VF_IMPORT) != 0 &&
            !(n_psonce & n_PSO_STARTED) && !(n_pstate & n_PS_ROOT))){
         value = N_("Variable cannot be set in a resource file: %s\n");
jeavmp:
         n_err(V_(value), avcp->avc_name);
         goto jleave;
      }

      /* Transformations */
      if(n_UNLIKELY(avmp->avm_flags & a_AMV_VF_LOWER)){
         char c;

         oval = savestr(value);
         value = oval;
         for(; (c = *oval) != '\0'; ++oval)
            *oval = lowerconv(c);
      }
   }

   rv = TRU1;
   a_amv_var_lookup(avcp, TRU1);

   /* Don't care what happens later on, store this in the unroll list */
   if(a_amv_lopts != NULL &&
         (avmp == NULL || !(avmp->avm_flags & a_AMV_VF_NOLOPTS)))
      a_amv_lopts_add(a_amv_lopts, avcp->avc_name, avcp->avc_var);

   if((avp = avcp->avc_var) == NULL){
      struct a_amv_var **avpp;
      size_t l;

      l = strlen(avcp->avc_name) +1;
      avcp->avc_var = avp = smalloc(n_VSTRUCT_SIZEOF(struct a_amv_var, av_name
            ) + l);
      avp->av_link = *(avpp = &a_amv_vars[avcp->avc_prime]);
      *avpp = avp;
#ifdef HAVE_PUTENV
      avp->av_env = NULL;
#endif
      memcpy(avp->av_name, avcp->avc_name, l);
      avp->av_flags = (avmp != NULL) ? avmp->avm_flags : 0;
      oval = n_UNCONST(n_empty);
   }else
      oval = avp->av_value;

   if(avmp == NULL)
      avp->av_value = a_amv_var_copy(value);
   else{
      /* Via `set' etc. the user may give even boolean options non-boolean
       * values, ignore that and force boolean */
      if(avp->av_flags & a_AMV_VF_BOOL){
         if(!(n_pstate & n_PS_ROOT) && (n_poption & n_PO_D_VV) &&
               *value != '\0')
            n_err(_("Ignoring value of boolean variable: %s: %s\n"),
               avcp->avc_name, value);
         avp->av_value = n_UNCONST(a_amv_var_1);
      }else
         avp->av_value = a_amv_var_copy(value);

      /* Check if update allowed XXX wasteful on error! */
      if((avp->av_flags & a_AMV_VF_VIP) &&
            !(rv = a_amv_var_check_vips(avcp->avc_okey, TRU1, &avp->av_value))){
         char *cp = avp->av_value;

         avp->av_value = oval;
         oval = cp;
      }
   }

   if(force_env && !(avp->av_flags & a_AMV_VF_ENV))
      avp->av_flags |= a_AMV_VF_LINKED;
   if(avp->av_flags & (a_AMV_VF_ENV | a_AMV_VF_LINKED))
      rv = a_amv_var__putenv(avcp, avp);

   a_amv_var_free(oval);
jleave:
   NYD2_LEAVE;
   return rv;
}

static bool_t
a_amv_var__putenv(struct a_amv_var_carrier *avcp, struct a_amv_var *avp){
#ifndef HAVE_SETENV
   char *cp;
#endif
   bool_t rv;
   NYD2_ENTER;

#ifdef HAVE_SETENV
   rv = (setenv(avcp->avc_name, avp->av_value, 1) == 0);
#else
   cp = sstrdup(savecatsep(avcp->avc_name, '=', avp->av_value));

   if((rv = (putenv(cp) == 0))){
      char *ocp;

      if((ocp = avp->av_env) != NULL)
         free(ocp);
      avp->av_env = cp;
   }else
      free(cp);
#endif
   NYD2_LEAVE;
   return rv;
}

static bool_t
a_amv_var_clear(struct a_amv_var_carrier *avcp, bool_t force_env){
   struct a_amv_var **avpp, *avp;
   struct a_amv_var_map const *avmp;
   bool_t rv;
   NYD2_ENTER;

   rv = FAL0;

   if(n_LIKELY((avmp = avcp->avc_map) != NULL)){
      if(n_UNLIKELY((avmp->avm_flags & a_AMV_VF_NODEL) != 0 &&
            !(n_pstate & n_PS_ROOT))){
         n_err(_("Variable may not be unset: %s\n"), avcp->avc_name);
         goto jleave;
      }
      if((avmp->avm_flags & a_AMV_VF_VIP) &&
            !a_amv_var_check_vips(avcp->avc_okey, FAL0, NULL))
         goto jleave;
   }

   rv = TRU1;

   if(n_UNLIKELY(!a_amv_var_lookup(avcp, TRUM1))){
      if(force_env){
jforce_env:
         rv = a_amv_var__clearenv(avcp->avc_name, NULL);
      }else if(!(n_pstate & (n_PS_ROOT | n_PS_ROBOT)) && (n_poption & n_PO_D_V))
         n_err(_("Can't unset undefined variable: %s\n"), avcp->avc_name);
      goto jleave;
   }else if(avcp->avc_var == (struct a_amv_var*)-1){
      avcp->avc_var = NULL;
      if(force_env)
         goto jforce_env;
      goto jleave;
   }

   if(a_amv_lopts != NULL &&
         (avmp == NULL || !(avmp->avm_flags & a_AMV_VF_NOLOPTS)))
      a_amv_lopts_add(a_amv_lopts, avcp->avc_name, avcp->avc_var);

   avp = avcp->avc_var;
   avcp->avc_var = NULL;
   avpp = &a_amv_vars[avcp->avc_prime];
   assert(*avpp == avp); /* (always listhead after lookup()) */
   *avpp = (*avpp)->av_link;

   /* C99 */{
#ifdef HAVE_SETENV
      char *envval = NULL;
#else
      char *envval = avp->av_env;
#endif
      if((avp->av_flags & (a_AMV_VF_ENV | a_AMV_VF_LINKED)) || envval != NULL)
         rv = a_amv_var__clearenv(avp->av_name, envval);
   }
   a_amv_var_free(avp->av_value);
   free(avp);

   /* XXX Fun part, extremely simple-minded for now: if this variable has
    * XXX a default value, immediately reinstantiate it!  TODO Heh? */
   if(n_UNLIKELY(avmp != NULL && (avmp->avm_flags & a_AMV_VF_DEFVAL) != 0))
      a_amv_var_lookup(avcp, TRU1);
jleave:
   NYD2_LEAVE;
   return rv;
}

static bool_t
a_amv_var__clearenv(char const *name, char *value){
#ifndef HAVE_SETENV
   extern char **environ;
   char **ecpp;
#endif
   bool_t rv;
   NYD2_ENTER;
   n_UNUSED(value);

#ifdef HAVE_SETENV
   unsetenv(name);
   rv = TRU1;
#else
   if(value != NULL)
      for(ecpp = environ; *ecpp != NULL; ++ecpp)
         if(*ecpp == value){
            free(value);
            do
               ecpp[0] = ecpp[1];
            while(*ecpp++ != NULL);
            break;
         }
   rv = TRU1;
#endif
   NYD2_LEAVE;
   return rv;
}

static void
a_amv_var_show_all(void){
   struct n_string msg, *msgp;
   FILE *fp;
   size_t no, i;
   struct a_amv_var *avp;
   char const **vacp, **cap;
   NYD2_ENTER;

   if((fp = Ftmp(NULL, "setlist", OF_RDWR | OF_UNLINK | OF_REGISTER)) == NULL){
      n_perr(_("Can't create temporary file for `set' listing"), 0);
      goto jleave;
   }

   /* We need to instantiate first-time-inits and default values here, so that
    * they will be regular members of our _vars[] table */
   for(i = a_AMV_VAR_I3VALS_CNT; i-- > 0;)
      n_var_oklook(a_amv_var_i3vals[i].avdv_okey);
   for(i = a_AMV_VAR_DEFVALS_CNT; i-- > 0;)
      n_var_oklook(a_amv_var_defvals[i].avdv_okey);

   for(no = i = 0; i < a_AMV_PRIME; ++i)
      for(avp = a_amv_vars[i]; avp != NULL; avp = avp->av_link)
         ++no;
   no += a_AMV_VAR_VIRTS_CNT;

   vacp = salloc(no * sizeof(*vacp));

   for(cap = vacp, i = 0; i < a_AMV_PRIME; ++i)
      for(avp = a_amv_vars[i]; avp != NULL; avp = avp->av_link)
         *cap++ = avp->av_name;
   for(i = a_AMV_VAR_VIRTS_CNT; i-- > 0;)
      *cap++ = a_amv_var_virts[i].avv_var->av_name;

   if(no > 1)
      qsort(vacp, no, sizeof *vacp, &a_amv_var__show_cmp);

   msgp = &msg;
   msgp = n_string_reserve(n_string_creat(msgp), 80);
   for(i = 0, cap = vacp; no != 0; ++cap, --no)
      i += a_amv_var_show(*cap, fp, msgp);
   n_string_gut(&msg);

   page_or_print(fp, i);
   Fclose(fp);
jleave:
   NYD2_LEAVE;
}

static int
a_amv_var__show_cmp(void const *s1, void const *s2){
   int rv;
   NYD2_ENTER;

   rv = strcmp(*(char**)n_UNCONST(s1), *(char**)n_UNCONST(s2));
   NYD2_LEAVE;
   return rv;
}

static size_t
a_amv_var_show(char const *name, FILE *fp, struct n_string *msgp){
   struct a_amv_var_carrier avc;
   char const *quote;
   size_t i;
   NYD2_ENTER;

   msgp = n_string_trunc(msgp, 0);
   i = 0;

   a_amv_var_revlookup(&avc, name);
   if(!a_amv_var_lookup(&avc, FAL0)){
      struct str s;

      msgp = n_string_assign_cp(msgp, _("No such variable: "));
      s.s = n_UNCONST(name);
      s.l = UIZ_MAX;
      msgp = n_shexp_quote(msgp, &s, FAL0);
      goto jleave;
   }

   if(n_poption & n_PO_D_V){
      if(avc.avc_map == NULL){
         msgp = n_string_push_c(msgp, '#');
         msgp = n_string_push_cp(msgp, "assembled");
         i = 1;
      }
      /* C99 */{
         struct{
            ui16_t flags;
            char msg[22];
         } const tbase[] = {
            {a_AMV_VF_VIRT, "virtual"},
            {a_AMV_VF_RDONLY, "readonly"},
            {a_AMV_VF_NODEL, "nodelete"},
            {a_AMV_VF_NOTEMPTY, "notempty"},
            {a_AMV_VF_NOCNTRLS, "no-control-chars"},
            {a_AMV_VF_NUM, "number"},
            {a_AMV_VF_POSNUM, "positive-number"},
            {a_AMV_VF_IMPORT, "import-environ-first\0"},
            {a_AMV_VF_ENV, "sync-environ"},
            {a_AMV_VF_I3VAL, "initial-value"},
            {a_AMV_VF_DEFVAL, "default-value"},
            {a_AMV_VF_LINKED, "`environ' link"}
         }, *tp;

         for(tp = tbase; PTRCMP(tp, <, &tbase[n_NELEM(tbase)]); ++tp)
            if(avc.avc_var->av_flags & tp->flags){
               msgp = n_string_push_c(msgp, (i++ == 0 ? '#' : ','));
               msgp = n_string_push_cp(msgp, tp->msg);
            }

      }
      if(i > 0)
         msgp = n_string_push_cp(msgp, "\n  ");
   }

   if(avc.avc_var->av_flags & a_AMV_VF_RDONLY)
      msgp = n_string_push_cp(msgp, "# ");
   n_UNINIT(quote, NULL);
   if(!(avc.avc_var->av_flags & a_AMV_VF_BOOL)){
      quote = n_shexp_quote_cp(avc.avc_var->av_value, TRU1);
      if(strcmp(quote, avc.avc_var->av_value))
         msgp = n_string_push_cp(msgp, "wysh ");
   }
   if(avc.avc_var->av_flags & a_AMV_VF_LINKED)
      msgp = n_string_push_cp(msgp, "environ ");
   msgp = n_string_push_cp(msgp, "set ");
   msgp = n_string_push_cp(msgp, name);
   if(!(avc.avc_var->av_flags & a_AMV_VF_BOOL)){
      msgp = n_string_push_c(msgp, '=');
      msgp = n_string_push_cp(msgp, quote);
   }

jleave:
   msgp = n_string_push_c(msgp, '\n');
   fputs(n_string_cp(msgp), fp);
   NYD2_ENTER;
   return (i > 0 ? 2 : 1);
}

static bool_t
a_amv_var_c_set(char **ap, bool_t issetenv){
   char *cp, *cp2, *varbuf, c;
   size_t errs;
   NYD2_ENTER;

   errs = 0;
jouter:
   while((cp = *ap++) != NULL){
      /* Isolate key */
      cp2 = varbuf = salloc(strlen(cp) +1);

      for(; (c = *cp) != '=' && c != '\0'; ++cp){
         if(cntrlchar(c) || spacechar(c)){
            n_err(_("Variable name with control character ignored: %s\n"),
               ap[-1]);
            ++errs;
            goto jouter;
         }
         *cp2++ = c;
      }
      *cp2 = '\0';
      if(c == '\0')
         cp = n_UNCONST(n_empty);
      else
         ++cp;

      if(varbuf == cp2){
         n_err(_("Empty variable name ignored\n"));
         ++errs;
      }else{
         struct a_amv_var_carrier avc;
         bool_t isunset;

         if((isunset = (varbuf[0] == 'n' && varbuf[1] == 'o')))
            varbuf = &varbuf[2];

         a_amv_var_revlookup(&avc, varbuf);

         if(isunset)
            errs += !a_amv_var_clear(&avc, issetenv);
         else
            errs += !a_amv_var_set(&avc, cp, issetenv);
      }
   }
   NYD2_LEAVE;
   return (errs == 0);
}

FL int
c_define(void *v){
   int rv;
   char **args;
   NYD_ENTER;

   rv = 1;

   if((args = v)[0] == NULL){
      rv = (a_amv_mac_show(a_AMV_MF_NONE) == FAL0);
      goto jleave;
   }

   if(args[1] == NULL || args[1][0] != '{' || args[1][1] != '\0' ||
         args[2] != NULL){
      n_err(_("Synopsis: define: <name> {\n"));
      goto jleave;
   }

   rv = (a_amv_mac_def(args[0], a_AMV_MF_NONE) == FAL0);
jleave:
   NYD_LEAVE;
   return rv;
}

FL int
c_undefine(void *v){
   int rv;
   char **args;
   NYD_ENTER;

   rv = 0;
   args = v;
   do
      rv |= !a_amv_mac_undef(*args, a_AMV_MF_NONE);
   while(*++args != NULL);
   NYD_LEAVE;
   return rv;
}

FL int
c_call(void *v){
   int rv;
   NYD_ENTER;

   rv = a_amv_mac_call(v, FAL0);
   NYD_LEAVE;
   return rv;
}

FL int
c_call_if(void *v){
   int rv;
   NYD_ENTER;

   rv = a_amv_mac_call(v, TRU1);
   NYD_LEAVE;
   return rv;
}

FL int
c_account(void *v){
   struct a_amv_mac_call_args *amcap;
   struct a_amv_mac *amp;
   char **args;
   int rv, i, oqf, nqf;
   NYD_ENTER;

   rv = 1;

   if((args = v)[0] == NULL){
      rv = (a_amv_mac_show(a_AMV_MF_ACC) == FAL0);
      goto jleave;
   }

   if(args[1] && args[1][0] == '{' && args[1][1] == '\0'){
      if(args[2] != NULL){
         n_err(_("Synopsis: account: <name> {\n"));
         goto jleave;
      }
      if(!asccasecmp(args[0], ACCOUNT_NULL)){
         n_err(_("`account': cannot use reserved name: %s\n"),
            ACCOUNT_NULL);
         goto jleave;
      }
      rv = (a_amv_mac_def(args[0], a_AMV_MF_ACC) == FAL0);
      goto jleave;
   }

   if(n_pstate & n_PS_HOOK_MASK){
      n_err(_("`account': can't change account from within a hook\n"));
      goto jleave;
   }

   save_mbox_for_possible_quitstuff();

   amp = NULL;
   if(asccasecmp(args[0], ACCOUNT_NULL) != 0 &&
         (amp = a_amv_mac_lookup(args[0], NULL, a_AMV_MF_ACC)) == NULL) {
      n_err(_("`account': account does not exist: %s\n"), args[0]);
      goto jleave;
   }

   oqf = savequitflags();

   if(a_amv_acc_curr != NULL){
      if(a_amv_acc_curr->am_lopts != NULL)
         a_amv_lopts_unroll(&a_amv_acc_curr->am_lopts);
      if(a_amv_acc_curr->am_flags & a_AMV_MF_DEL)
         a_amv_mac_free(a_amv_acc_curr);
   }

   account_name = (amp != NULL) ? amp->am_name : NULL;
   a_amv_acc_curr = amp;

   if(amp != NULL){
      bool_t ok;
      assert(amp->am_lopts == NULL);
      amcap = n_lofi_alloc(sizeof *amcap);
      memset(amcap, 0, sizeof *amcap);
      amcap->amca_name = amp->am_name;
      amcap->amca_amp = amp;
      amcap->amca_unroller = &amp->am_lopts;
      amcap->amca_lopts_on = TRU1;
      ok = a_amv_mac_exec(amcap);
      if(!ok){
         /* XXX account switch incomplete, unroll? */
         n_err(_("`account': failed to switch to account: %s\n"), amp->am_name);
         goto jleave;
      }
   }

   /* C99 */{
      bool_t reset = !(n_pstate & n_PS_ROOT);

      n_pstate |= n_PS_ROOT;
      if(amp != NULL)
         ok_vset(_account, amp->am_name);
      else
         ok_vclear(_account);
      if(reset)
         n_pstate &= ~n_PS_ROOT;
   }

   if((n_psonce & n_PSO_STARTED) && !(n_pstate & n_PS_HOOK_MASK)){
      nqf = savequitflags(); /* TODO obsolete (leave -> void -> new box!) */
      restorequitflags(oqf);
      if((i = setfile("%", 0)) < 0)
         goto jleave;
      temporary_folder_hook_check(FAL0);
      if(i > 0 && !ok_blook(emptystart))
         goto jleave;
      announce(ok_blook(bsdcompat) || ok_blook(bsdannounce));
      restorequitflags(nqf);
   }
   rv = 0;
jleave:
   NYD_LEAVE;
   return rv;
}

FL int
c_unaccount(void *v){
   int rv;
   char **args;
   NYD_ENTER;

   rv = 0;
   args = v;
   do
      rv |= !a_amv_mac_undef(*args, a_AMV_MF_ACC);
   while(*++args != NULL);
   NYD_LEAVE;
   return rv;
}

FL int
c_localopts(void *v){
   char **argv;
   int rv;
   NYD_ENTER;

   rv = 1;

   if(a_amv_lopts == NULL){
      n_err(_("Cannot use `localopts' in this context "
         "(not in `define' or `account', nor special hook)\n"));
      goto jleave;
   }

   rv = 0;

   if(n_pstate & (n_PS_HOOK | n_PS_COMPOSE_MODE)){
      if(n_poption & n_PO_D_V)
         n_err(_("Cannot turn off `localopts' for compose-mode hooks\n"));
      goto jleave;
   }

   a_amv_lopts->as_unroll = (boolify(*(argv = v), UIZ_MAX, FAL0) > 0);
jleave:
   NYD_LEAVE;
   return rv;
}

FL int
c_shift(void *v){
   int rv;
   NYD_ENTER;

   rv = 1;

   if(a_amv_lopts != NULL){
      ui16_t i, j;
      struct a_amv_mac const *amp;
      struct a_amv_mac_call_args *amcap;

      amp = (amcap = a_amv_lopts->as_amcap)->amca_amp;
      if((amp->am_flags & a_AMV_MF_TYPE_MASK) == a_AMV_MF_ACC)
         goto jerr;

      v = *(char**)v;
      if(v == NULL)
         i = 1;
      else{
         char *eptr;
         long l;

         l = strtol(v, &eptr, 10);
         if(*eptr != '\0' || l < 0 || l > SI16_MAX){
            n_err(_("`shift': invalid argument: %s\n"), v);
            goto jleave;
         }
         i = (ui16_t)l;
      }

      if(i > (j = amcap->amca_argc)){
         n_err(_("`shift': cannot shift %hu of %hu parameters\n"), i, j);
         goto jleave;
      }else{
         rv = 0;
         if(j > 0){
            j -= i;
            amcap->amca_argc = j;
            amcap->amca_argv += i;
         }
      }
   }else
jerr:
      n_err(_("Can only use `shift' in a `call'ed macro\n"));
jleave:
   NYD_LEAVE;
   return rv;
}

FL int
c_return(void *v){
   int rv;
   NYD_ENTER;

   rv = 1;

   if(a_amv_lopts != NULL){
      char const * const m1 = "-1", **argv, *mrv;

      n_source_force_eof();

      if((argv = v)[0] != NULL){
         char *eptr;
         long l;

         l = strtol(argv[0], &eptr, 10);
         if(*eptr != '\0' || l < 0 || l > SI32_MAX){
            n_err(_("`return': argument one is invalid: %s\n"), argv[0]);
            mrv = m1;
         }else
            mrv = argv[0];

         if(argv[1] != NULL){
            l = strtol(argv[1], &eptr, 10);
            if(*eptr != '\0' || l < 0 || l > SI32_MAX){
               n_err(_("`return': argument two is invalid: %s\n"), argv[1]);
               mrv = m1;
            }else
               rv = (int)l;
         }else
            rv = 0;
      }else{
         rv = 0;
         mrv = "0";
      }

      /* C99 */{
         bool_t reset;

         reset = !(n_pstate & n_PS_ROOT);
         n_pstate |= n_PS_ROOT;
         ok_vset(__rv, mrv);
         if(reset)
            n_pstate &= ~n_PS_ROOT;
      }
   }else
      n_err(_("Can only use `return' in a macro\n"));
   NYD_LEAVE;
   return rv;
}

FL bool_t
temporary_folder_hook_check(bool_t nmail){ /* TODO temporary, v15: drop */
   struct a_amv_mac_call_args *amcap;
   struct a_amv_mac *amp;
   size_t len;
   char const *cp;
   char *var;
   bool_t rv;
   NYD_ENTER;

   rv = TRU1;
   var = salloc(len = strlen(mailname) + sizeof("folder-hook-") -1  +1);

   /* First try the fully resolved path */
   snprintf(var, len, "folder-hook-%s", mailname);
   if((cp = n_var_vlook(var, FAL0)) != NULL)
      goto jmac;

   /* If we are under *folder*, try the usual +NAME syntax, too */
   if(displayname[0] == '+'){
      char *x;

      for(x = &mailname[len]; x != mailname; --x)
         if(x[-1] == '/'){
            snprintf(var, len, "folder-hook-+%s", x);
            if((cp = n_var_vlook(var, FAL0)) != NULL)
               goto jmac;
            break;
         }
   }

   /* Plain *folder-hook* is our last try */
   if((cp = ok_vlook(folder_hook)) == NULL)
      goto jleave;

jmac:
   if((amp = a_amv_mac_lookup(cp, NULL, a_AMV_MF_NONE)) == NULL){
      n_err(_("Cannot call *folder-hook* for %s: macro does not exist: %s\n"),
         n_shexp_quote_cp(displayname, FAL0), cp);
      rv = FAL0;
      goto jleave;
   }

   amcap = n_lofi_alloc(sizeof *amcap);
   memset(amcap, 0, sizeof *amcap);
   amcap->amca_name = cp;
   amcap->amca_amp = amp;
   n_pstate &= ~n_PS_HOOK_MASK;
   if(nmail){
      amcap->amca_unroller = NULL;
      n_pstate |= n_PS_HOOK_NEWMAIL;
   }else{
      amcap->amca_unroller = &a_amv_folder_hook_lopts;
      n_pstate |= n_PS_HOOK;
   }
   amcap->amca_lopts_on = TRU1;
   amcap->amca_ps_hook_mask = TRU1;
   rv = a_amv_mac_exec(amcap);
   n_pstate &= ~n_PS_HOOK_MASK;

jleave:
   NYD_LEAVE;
   return rv;
}

FL void
temporary_folder_hook_unroll(void){ /* XXX intermediate hack */
   NYD_ENTER;
   if(a_amv_folder_hook_lopts != NULL){
      void *save = a_amv_lopts;

      a_amv_lopts = NULL;
      a_amv_lopts_unroll(&a_amv_folder_hook_lopts);
      a_amv_folder_hook_lopts = NULL;
      a_amv_lopts = save;
   }
   NYD_LEAVE;
}

FL void
temporary_compose_mode_hook_call(char const *macname,
      void (*hook_pre)(void *), void *hook_arg){
   /* TODO compose_mode_hook_call() temporary, v15: generalize; see a_LEX_SLICE
    * TODO comment in lex_input.c for the right way of doing things! */
   static struct a_amv_lostack *cmh_losp;
   struct a_amv_mac_call_args *amcap;
   struct a_amv_mac *amp;
   NYD_ENTER;

   amp = NULL;

   if(macname == (char*)-1){
      a_amv_mac__finalize(cmh_losp);
      cmh_losp = NULL;
   }else if(macname != NULL &&
         (amp = a_amv_mac_lookup(macname, NULL, a_AMV_MF_NONE)) == NULL)
      n_err(_("Cannot call *on-compose-**: macro does not exist: %s\n"),
         macname);
   else{
      amcap = n_lofi_alloc(sizeof *amcap);
      memset(amcap, 0, sizeof *amcap);
      amcap->amca_name = (macname != NULL) ? macname : "on-compose-done-shell";
      amcap->amca_amp = amp;
      amcap->amca_unroller = &a_amv_compose_lopts;
      amcap->amca_hook_pre = hook_pre;
      amcap->amca_hook_arg = hook_arg;
      amcap->amca_lopts_on = TRU1;
      amcap->amca_ps_hook_mask = TRU1;
      n_pstate &= ~n_PS_HOOK_MASK;
      n_pstate |= n_PS_HOOK;
      if(macname != NULL)
         a_amv_mac_exec(amcap);
      else{
         cmh_losp = n_lofi_alloc(sizeof *cmh_losp);
         cmh_losp->as_global_saved = a_amv_lopts;
         cmh_losp->as_up = NULL;
         cmh_losp->as_lopts = *(cmh_losp->as_amcap = amcap)->amca_unroller;
         cmh_losp->as_unroll = TRU1;
         a_amv_lopts = cmh_losp;
      }
   }
   NYD_LEAVE;
}

FL void
temporary_compose_mode_hook_unroll(void){ /* XXX intermediate hack */
   NYD_ENTER;
   if(a_amv_compose_lopts != NULL){
      void *save = a_amv_lopts;

      a_amv_lopts = NULL;
      a_amv_lopts_unroll(&a_amv_compose_lopts);
      a_amv_compose_lopts = NULL;
      a_amv_lopts = save;
   }
   NYD_LEAVE;
}

FL char *
n_var_oklook(enum okeys okey){
   struct a_amv_var_carrier avc;
   char *rv;
   struct a_amv_var_map const *avmp;
   NYD_ENTER;

   avc.avc_map = avmp = &a_amv_var_map[okey];
   avc.avc_name = a_amv_var_names + avmp->avm_keyoff;
   avc.avc_hash = avmp->avm_hash;
   avc.avc_okey = okey;

   if(a_amv_var_lookup(&avc, FAL0))
      rv = avc.avc_var->av_value;
   else
      rv = NULL;
   NYD_LEAVE;
   return rv;
}

FL bool_t
n_var_okset(enum okeys okey, uintptr_t val){
   struct a_amv_var_carrier avc;
   bool_t ok;
   struct a_amv_var_map const *avmp;
   NYD_ENTER;

   avc.avc_map = avmp = &a_amv_var_map[okey];
   avc.avc_name = a_amv_var_names + avmp->avm_keyoff;
   avc.avc_hash = avmp->avm_hash;
   avc.avc_okey = okey;

   ok = a_amv_var_set(&avc, (val == 0x1 ? n_empty : (char const*)val), FAL0);
   NYD_LEAVE;
   return ok;
}

FL bool_t
n_var_okclear(enum okeys okey){
   struct a_amv_var_carrier avc;
   bool_t rv;
   struct a_amv_var_map const *avmp;
   NYD_ENTER;

   avc.avc_map = avmp = &a_amv_var_map[okey];
   avc.avc_name = a_amv_var_names + avmp->avm_keyoff;
   avc.avc_hash = avmp->avm_hash;
   avc.avc_okey = okey;

   rv = a_amv_var_clear(&avc, FAL0);
   NYD_LEAVE;
   return rv;
}

FL char const *
n_var_vlook(char const *vokey, bool_t try_getenv){
   struct a_amv_var_carrier avc;
   char const *rv;
   NYD_ENTER;

   a_amv_var_revlookup(&avc, vokey);
   rv = NULL;

   /* Here, and only here we need to take care for the special macro-local
    * parameters... */
   if(n_LIKELY(!avc.avc_is_special)){
      if(a_amv_var_lookup(&avc, FAL0))
         rv = avc.avc_var->av_value;
      /* Only check the environment for something that is otherwise unknown */
      else if(try_getenv && avc.avc_map == NULL)
         rv = getenv(vokey);
   }else{
      /* These may occur only in a macro.. */
      if(n_LIKELY(a_amv_lopts != NULL)){
         struct a_amv_mac const *amp;
         struct a_amv_mac_call_args *amcap;

         /* ..in a `call'ed macro only, to be exact */
         amp = (amcap = a_amv_lopts->as_amcap)->amca_amp;
         if((amp->am_flags & a_AMV_MF_TYPE_MASK) != a_AMV_MF_ACC){
            if(avc.avc_is_special == TRUM1){
               if(avc.avc_special_prop > 0){
                  if(amcap->amca_argc >= avc.avc_special_prop)
                     rv = amcap->amca_argv[avc.avc_special_prop - 1];
               }
            }else switch(avc.avc_special_prop){
            case a_AMV_VST_STAR:
            case a_AMV_VST_AT:{
               /* TODO Expansion of $* and $@ not shell compatible, if
                * TODO that occurs within double quotes.
                * TODO Same notes on that in accmacvar.c, shexp.c */
               ui32_t i, l;

               for(i = l = 0; i < amcap->amca_argc; ++i)
                  l += strlen(amcap->amca_argv[i]) + 1;
               if(l == 0)
                  rv = n_empty;
               else{
                  char *cp;

                  rv = cp = salloc(l);
                  for(i = l = 0; i < amcap->amca_argc; ++i){
                     l = strlen(amcap->amca_argv[i]);
                     memcpy(cp, amcap->amca_argv[i], l);
                     cp += l;
                     *cp++ = ' ';
                  }
                  *--cp = '\0';
               }
            }  break;
            case a_AMV_VST_NOSIGN:{
               char *cp;

               rv = cp = salloc(sizeof("65535"));
               snprintf(cp, sizeof("65535"), "%hu", amcap->amca_argc);
            }  break;
            default:
               rv = n_empty;
               break;
            }
         }
      }else if(n_poption & n_PO_D_V)
         n_err(_("Cannot use macro local variable in this context: %s\n"),
            n_shexp_quote_cp(vokey, FAL0));
   }
   NYD_LEAVE;
   return rv;
}

FL bool_t
n_var_vset(char const *vokey, uintptr_t val){
   struct a_amv_var_carrier avc;
   bool_t ok;
   NYD_ENTER;

   a_amv_var_revlookup(&avc, vokey);

   ok = a_amv_var_set(&avc, (val == 0x1 ? n_empty : (char const*)val), FAL0);
   NYD_LEAVE;
   return ok;
}

FL bool_t
n_var_vclear(char const *vokey){
   struct a_amv_var_carrier avc;
   bool_t ok;
   NYD_ENTER;

   a_amv_var_revlookup(&avc, vokey);

   ok = a_amv_var_clear(&avc, FAL0);
   NYD_LEAVE;
   return ok;
}

#ifdef HAVE_SOCKETS
FL char *
n_var_xoklook(enum okeys okey, struct url const *urlp,
      enum okey_xlook_mode oxm){
   struct a_amv_var_carrier avc;
   struct str const *us;
   size_t nlen;
   char *nbuf, *rv;
   struct a_amv_var_map const *avmp;
   NYD_ENTER;

   assert(oxm & (OXM_PLAIN | OXM_H_P | OXM_U_H_P));

   /* For simplicity: allow this case too */
   if(!(oxm & (OXM_H_P | OXM_U_H_P))){
      nbuf = NULL;
      goto jplain;
   }

   avc.avc_map = avmp = &a_amv_var_map[okey];
   avc.avc_name = a_amv_var_names + avmp->avm_keyoff;
   avc.avc_okey = okey;

   us = (oxm & OXM_U_H_P) ? &urlp->url_u_h_p : &urlp->url_h_p;
   nlen = strlen(avc.avc_name);
   nbuf = n_lofi_alloc(nlen + 1 + us->l +1);
   memcpy(nbuf, avc.avc_name, nlen);
   nbuf[nlen++] = '-';

   /* One of .url_u_h_p and .url_h_p we test in here */
   memcpy(nbuf + nlen, us->s, us->l +1);
   avc.avc_name = a_amv_var_canonify(nbuf);
   avc.avc_hash = a_AMV_NAME2HASH(avc.avc_name);
   if(a_amv_var_lookup(&avc, FAL0))
      goto jvar;

   /* The second */
   if(oxm & OXM_H_P){
      us = &urlp->url_h_p;
      memcpy(nbuf + nlen, us->s, us->l +1);
      avc.avc_name = a_amv_var_canonify(nbuf);
      avc.avc_hash = a_AMV_NAME2HASH(avc.avc_name);
      if(a_amv_var_lookup(&avc, FAL0)){
jvar:
         rv = avc.avc_var->av_value;
         goto jleave;
      }
   }

jplain:
   rv = (oxm & OXM_PLAIN) ? n_var_oklook(okey) : NULL;
jleave:
   if(nbuf != NULL)
      n_lofi_free(nbuf);
   NYD_LEAVE;
   return rv;
}
#endif /* HAVE_SOCKETS */

FL int
c_set(void *v){
   char **ap;
   int err;
   NYD_ENTER;

   if(*(ap = v) == NULL){
      a_amv_var_show_all();
      err = 0;
   }else
      err = !a_amv_var_c_set(ap, FAL0);
   NYD_LEAVE;
   return err;
}

FL int
c_unset(void *v){
   char **ap;
   int err;
   NYD_ENTER;

   for(err = 0, ap = v; *ap != NULL; ++ap)
      err |= !n_var_vclear(*ap);
   NYD_LEAVE;
   return err;
}

FL int
c_varshow(void *v){
   char **ap;
   NYD_ENTER;

   if(*(ap = v) == NULL)
      v = NULL;
   else{
      struct n_string msg, *msgp = &msg;

      msgp = n_string_creat(msgp);
      for(; *ap != NULL; ++ap)
         a_amv_var_show(*ap, n_stdout, msgp);
      n_string_gut(msgp);
   }
   NYD_LEAVE;
   return (v == NULL ? !STOP : !OKAY); /* xxx 1:bad 0:good -- do some */
}

FL int
c_varedit(void *v){
   struct a_amv_var_carrier avc;
   FILE *of, *nf;
   char *val, **argv;
   int err;
   sighandler_type sigint;
   NYD_ENTER;

   sigint = safe_signal(SIGINT, SIG_IGN);

   for(err = 0, argv = v; *argv != NULL; ++argv){
      memset(&avc, 0, sizeof avc);

      a_amv_var_revlookup(&avc, *argv);

      if(avc.avc_map != NULL){
         if(avc.avc_map->avm_flags & a_AMV_VF_BOOL){
            n_err(_("`varedit': cannot edit boolean variable: %s\n"),
               avc.avc_name);
            continue;
         }
         if(avc.avc_map->avm_flags & a_AMV_VF_RDONLY){
            n_err(_("`varedit': cannot edit readonly variable: %s\n"),
               avc.avc_name);
            continue;
         }
      }

      a_amv_var_lookup(&avc, FAL0);

      if((of = Ftmp(NULL, "varedit", OF_RDWR | OF_UNLINK | OF_REGISTER)) ==
            NULL){
         n_perr(_("`varedit': can't create temporary file, bailing out"), 0);
         err = 1;
         break;
      }else if(avc.avc_var != NULL && *(val = avc.avc_var->av_value) != '\0' &&
            sizeof *val != fwrite(val, strlen(val), sizeof *val, of)){
         n_perr(_("`varedit' failed to write old value to temporary file"), 0);
         Fclose(of);
         err = 1;
         continue;
      }

      fflush_rewind(of);
      nf = run_editor(of, (off_t)-1, 'e', FAL0, NULL, NULL, SEND_MBOX, sigint);
      Fclose(of);

      if(nf != NULL){
         int c;
         char *base;
         off_t l;

         l = fsize(nf);
         assert(l >= 0);
         base = salloc((size_t)l +1);

         for(l = 0, val = base; (c = getc(nf)) != EOF; ++val)
            if(c == '\n' || c == '\r'){
               *val = ' ';
               ++l;
            }else{
               *val = (char)(uc_i)c;
               l = 0;
            }
         val -= l;
         *val = '\0';

         if(!a_amv_var_set(&avc, base, FAL0))
            err = 1;

         Fclose(nf);
      }else{
         n_err(_("`varedit': can't start $EDITOR, bailing out\n"));
         err = 1;
         break;
      }
   }

   safe_signal(SIGINT, sigint);
   NYD_LEAVE;
   return err;
}

FL int
c_environ(void *v){
   struct a_amv_var_carrier avc;
   int err;
   char **ap;
   bool_t islnk;
   NYD_ENTER;

   if((islnk = is_asccaseprefix(*(ap = v), "link")) ||
         is_asccaseprefix(*ap, "unlink")){
      for(err = 0; *++ap != NULL;){
         a_amv_var_revlookup(&avc, *ap);

         if(a_amv_var_lookup(&avc, FAL0) && (islnk ||
               (avc.avc_var->av_flags & a_AMV_VF_LINKED))){
            if(!islnk){
               avc.avc_var->av_flags &= ~a_AMV_VF_LINKED;
               continue;
            }else if(avc.avc_var->av_flags & (a_AMV_VF_ENV | a_AMV_VF_LINKED)){
               if(n_poption & n_PO_D_V)
                  n_err(_("`environ': link: already established: %s\n"), *ap);
               continue;
            }
            avc.avc_var->av_flags |= a_AMV_VF_LINKED;
            if(!(avc.avc_var->av_flags & a_AMV_VF_ENV))
               a_amv_var__putenv(&avc, avc.avc_var);
         }else if(!islnk){
            n_err(_("`environ': unlink: no link established: %s\n"), *ap);
            err = 1;
         }else{
            char const *evp = getenv(*ap);

            if(evp != NULL)
               err |= !a_amv_var_set(&avc, evp, TRU1);
            else{
               n_err(_("`environ': link: cannot link to non-existent: %s\n"),
                  *ap);
               err = 1;
            }
         }
      }
   }else if(is_asccaseprefix(*ap, "set"))
      err = !a_amv_var_c_set(++ap, TRU1);
   else if(is_asccaseprefix(*ap, "unset")){
      for(err = 0; *++ap != NULL;){
         a_amv_var_revlookup(&avc, *ap);

         if(!a_amv_var_clear(&avc, TRU1))
            err = 1;
      }
   }else{
      n_err(_("Synopsis: environ: <link|set|unset> <variable>...\n"));
      err = 1;
   }
   NYD_LEAVE;
   return err;
}

/* s-it-mode */
