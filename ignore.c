/*@ S-nail - a mail user agent derived from Berkeley Mail.
 *@ `headerpick', `retain' and `ignore'.
 *@ XXX Should these be in nam_a_grp.c?!
 *
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
#define n_FILE ignore

#ifndef HAVE_AMALGAMATION
# include "nail.h"
#endif

struct a_ignore_type{
   ui32_t it_count;     /* Entries in .it_ht (and .it_re) */
   bool_t it_all;       /* _All_ fields ought to be _type_ (ignore/retain) */
   ui8_t it__dummy[3];
   struct a_ignore_field{
      struct a_ignore_field *if_next;
      char if_field[n_VFIELD_SIZE(0)]; /* Header field */
   } *it_ht[3]; /* TODO make hashmap dynamic */
#ifdef HAVE_REGEX
   struct a_ignore_re{
      struct a_ignore_re *ir_next;
      regex_t ir_regex;
      char ir_input[n_VFIELD_SIZE(0)]; /* Regex input text (for showing it) */
   } *it_re, *it_re_tail;
#endif
};

struct n_ignore{
   struct a_ignore_type i_retain;
   struct a_ignore_type i_ignore;
   bool_t i_auto;                   /* In auto-reclaimed, not heap memory */
   bool_t i_bltin;                  /* Is a builtin n_IGNORE* type */
   ui8_t i_ibm_idx;                 /* If .i_bltin: a_ignore_bltin_map[] idx */
   ui8_t i__dummy[5];
};

struct a_ignore_bltin_map{
   struct n_ignore *ibm_ip;
   char const ibm_name[8];
};

static struct a_ignore_bltin_map const a_ignore_bltin_map[] = {
   {n_IGNORE_TYPE, "type\0"},
   {n_IGNORE_SAVE, "save\0"},
   {n_IGNORE_FWD, "forward\0"},
   {n_IGNORE_TOP, "top\0"},

   {n_IGNORE_TYPE, "print\0"},
   {n_IGNORE_FWD, "fwd\0"}
};
#ifdef HAVE_DEVEL /* Avoid gcc warn cascade since n_ignore is defined locally */
n_CTAV(-(uintptr_t)n_IGNORE_TYPE - n__IGNORE_ADJUST == 0);
n_CTAV(-(uintptr_t)n_IGNORE_SAVE - n__IGNORE_ADJUST == 1);
n_CTAV(-(uintptr_t)n_IGNORE_FWD - n__IGNORE_ADJUST == 2);
n_CTAV(-(uintptr_t)n_IGNORE_TOP - n__IGNORE_ADJUST == 3);
n_CTAV(n__IGNORE_MAX == 3);
#endif

static struct n_ignore *a_ignore_bltin[n__IGNORE_MAX + 1];
/* Almost everyone uses `ignore'/`retain', put _TYPE in BSS */
static struct n_ignore a_ignore_type;

/* Return real self, which is xself unless that is one of the builtin specials,
 * in which case NULL is returned if nonexistent and docreate is false.
 * The other statics assume self has been resolved (unless noted) */
static struct n_ignore *a_ignore_resolve_self(struct n_ignore *xself,
                           bool_t docreate);

/* Lookup whether a mapping is contained: TRU1=retained, TRUM1=ignored.
 * If retain is _not_ TRUM1 then only the retained/ignored slot is inspected,
 * and regular expressions are not executed but instead their .ir_input is
 * text-compared against len bytes of dat.
 * Note it doesn't handle the .it_all "all fields" condition */
static bool_t a_ignore_lookup(struct n_ignore const *self, bool_t retain,
               char const *dat, size_t len);

/* Delete all retain( else ignor)ed members */
static void a_ignore_del_allof(struct n_ignore *ip, bool_t retain);

/* Try to map a string to one of the builtin types */
static struct a_ignore_bltin_map const *a_ignore_resolve_bltin(char const *cp);

/* Logic behind `headerpick T T add' (a.k.a. `retain'+) */
static bool_t a_ignore_addcmd_mux(struct n_ignore *ip, char const **list,
               bool_t retain);

static void a_ignore__show(struct n_ignore const *ip, bool_t retain);
static int a_ignore__cmp(void const *l, void const *r);

/* Logic behind `headerpick T T remove' (a.k.a. `unretain'+) */
static bool_t a_ignore_delcmd_mux(struct n_ignore *ip, char const **list,
               bool_t retain);

static bool_t a_ignore__delone(struct n_ignore *ip, bool_t retain,
               char const *field);

static struct n_ignore *
a_ignore_resolve_self(struct n_ignore *xself, bool_t docreate){
   uintptr_t suip;
   struct n_ignore *self;
   NYD2_ENTER;

   self = xself;
   suip = -(uintptr_t)self - n__IGNORE_ADJUST;

   if(suip <= n__IGNORE_MAX){
      if((self = a_ignore_bltin[suip]) == NULL && docreate){
         if(xself == n_IGNORE_TYPE){
            self = &a_ignore_type;
            /* LIB: memset(self, 0, sizeof *self);*/
         }else
            self = n_ignore_new(FAL0);
         self->i_bltin = TRU1;
         self->i_ibm_idx = (ui8_t)suip;
         a_ignore_bltin[suip] = self;
      }
   }
   NYD2_LEAVE;
   return self;
}

static bool_t
a_ignore_lookup(struct n_ignore const *self, bool_t retain,
      char const *dat, size_t len){
   bool_t rv;
#ifdef HAVE_REGEX
   struct a_ignore_re *irp;
#endif
   struct a_ignore_field *ifp;
   ui32_t hi;
   NYD2_ENTER;

   if(len == UIZ_MAX)
      len = strlen(dat);
   hi = torek_ihashn(dat, len) % n_NELEM(self->i_retain.it_ht);

   /* Again: doesn't handle .it_all conditions! */
   /* (Inner functions would be nice, again) */
   if(retain && self->i_retain.it_count > 0){
      rv = TRU1;
      for(ifp = self->i_retain.it_ht[hi]; ifp != NULL; ifp = ifp->if_next)
         if(!ascncasecmp(ifp->if_field, dat, len))
            goto jleave;
#ifdef HAVE_REGEX
      if(dat[len - 1] != '\0')
         dat = savestrbuf(dat, len);
      for(irp = self->i_retain.it_re; irp != NULL; irp = irp->ir_next)
         if((retain == TRUM1
               ? (regexec(&irp->ir_regex, dat, 0,NULL, 0) != REG_NOMATCH)
               : !strncmp(irp->ir_input, dat, len)))
            goto jleave;
#endif
      rv = (retain == TRUM1) ? TRUM1 : FAL0;
   }else if((retain == TRUM1 || !retain) && self->i_ignore.it_count > 0){
      rv = TRUM1;
      for(ifp = self->i_ignore.it_ht[hi]; ifp != NULL; ifp = ifp->if_next)
         if(!ascncasecmp(ifp->if_field, dat, len))
            goto jleave;
#ifdef HAVE_REGEX
      if(dat[len - 1] != '\0')
         dat = savestrbuf(dat, len);
      for(irp = self->i_ignore.it_re; irp != NULL; irp = irp->ir_next)
         if((retain == TRUM1
               ? (regexec(&irp->ir_regex, dat, 0,NULL, 0) != REG_NOMATCH)
               : !strncmp(irp->ir_input, dat, len)))
            goto jleave;
#endif
      rv = (retain == TRUM1) ? TRU1 : FAL0;
   }else
      rv = FAL0;
jleave:
   NYD2_LEAVE;
   return rv;
}

static void
a_ignore_del_allof(struct n_ignore *ip, bool_t retain){
#ifdef HAVE_REGEX
   struct a_ignore_re *irp;
#endif
   struct a_ignore_field *ifp;
   struct a_ignore_type *itp;
   NYD2_ENTER;

   itp = retain ? &ip->i_retain : &ip->i_ignore;

   if(!ip->i_auto){
      size_t i;

      for(i = 0; i < n_NELEM(itp->it_ht); ++i)
         for(ifp = itp->it_ht[i]; ifp != NULL;){
            struct a_ignore_field *x;

            x = ifp;
            ifp = ifp->if_next;
            n_free(x);
         }
   }

#ifdef HAVE_REGEX
   for(irp = itp->it_re; irp != NULL;){
      struct a_ignore_re *x;

      x = irp;
      irp = irp->ir_next;
      regfree(&x->ir_regex);
      if(!ip->i_auto)
         n_free(x);
   }
#endif

   memset(itp, 0, sizeof *itp);
   NYD2_LEAVE;
}

static struct a_ignore_bltin_map const *
a_ignore_resolve_bltin(char const *cp){
   struct a_ignore_bltin_map const *ibmp;
   NYD2_ENTER;

   for(ibmp = &a_ignore_bltin_map[0];;)
      if(!asccasecmp(cp, ibmp->ibm_name))
         break;
      else if(++ibmp == &a_ignore_bltin_map[n_NELEM(a_ignore_bltin_map)]){
         ibmp = NULL;
         break;
      }
   NYD2_LEAVE;
   return ibmp;
}

static bool_t
a_ignore_addcmd_mux(struct n_ignore *ip, char const **list, bool_t retain){
   char const **ap;
   bool_t rv;
   NYD2_ENTER;

   ip = a_ignore_resolve_self(ip, rv = (*list != NULL));

   if(!rv){
      if(ip != NULL && ip->i_bltin)
         a_ignore__show(ip, retain);
      rv = TRU1;
   }else{
      for(ap = list; *ap != 0; ++ap)
         switch(n_ignore_insert_cp(ip, retain, *ap)){
         case FAL0:
            n_err(_("Invalid field name cannot be %s: %s\n"),
               (retain ? _("retained") : _("ignored")), *ap);
            rv = FAL0;
            break;
         case TRUM1:
            if(options & OPT_D_V)
               n_err(_("Field already %s: %s\n"),
                  (retain ? _("retained") : _("ignored")), *ap);
            /* FALLTHRU */
         case TRU1:
            break;
         }
   }
   NYD2_LEAVE;
   return rv;
}

static void
a_ignore__show(struct n_ignore const *ip, bool_t retain){
#ifdef HAVE_REGEX
   struct a_ignore_re *irp;
#endif
   struct a_ignore_field *ifp;
   size_t i, sw;
   char const **ap, **ring;
   struct a_ignore_type const *itp;
   NYD2_ENTER;

   itp = retain ? &ip->i_retain : &ip->i_ignore;

   do{
      char const *pre, *attr;

      if(itp->it_all)
         pre = n_empty, attr = "*";
      else if(itp->it_count == 0)
         pre = "#", attr = _("currently covers no fields");
      else
         break;
      printf(_("%sheaderpick %s %s %s\n"),
         pre, a_ignore_bltin_map[ip->i_ibm_idx].ibm_name,
         (retain ? "retain" : "ignore"), attr);
      goto jleave;
   }while(0);

   ring = salloc((itp->it_count +1) * sizeof *ring);
   for(ap = ring, i = 0; i < n_NELEM(itp->it_ht); ++i)
      for(ifp = itp->it_ht[i]; ifp != NULL; ifp = ifp->if_next)
         *ap++ = ifp->if_field;
   *ap = NULL;

   qsort(ring, PTR2SIZE(ap - ring), sizeof *ring, &a_ignore__cmp);

   i = printf("headerpick %s %s add",
      a_ignore_bltin_map[ip->i_ibm_idx].ibm_name,
      (retain ? "retain" : "ignore"));
   sw = scrnwidth;

   for(ap = ring; *ap != NULL; ++ap){
      /* These fields are all ASCII, no visual width needed */
      size_t len;

      len = strlen(*ap) + 1;
      if(UICMP(z, len, >=, sw - i)){
         fputs(" \\\n ", stdout);
         i = 1;
      }
      i += len;
      putc(' ', stdout);
      fputs(*ap, stdout);
   }

   /* Regular expression in FIFO order */
#ifdef HAVE_REGEX
   for(irp = itp->it_re; irp != NULL; irp = irp->ir_next){
      size_t len;
      char const *cp;

      cp = n_shexp_quote_cp(irp->ir_input, FAL0);
      len = strlen(cp) + 1;
      if(UICMP(z, len, >=, sw - i)){
         fputs(" \\\n ", stdout);
         i = 1;
      }
      i += len;
      putc(' ', stdout);
      fputs(cp, stdout);
   }
#endif

   putchar('\n');
jleave:
   fflush(stdout);
   NYD2_LEAVE;
}

static int
a_ignore__cmp(void const *l, void const *r){
   int rv;

   rv = asccasecmp(*(char const * const *)l, *(char const * const *)r);
   return rv;
}

static bool_t
a_ignore_delcmd_mux(struct n_ignore *ip, char const **list, bool_t retain){
   char const *cp;
   struct a_ignore_type *itp;
   bool_t rv;
   NYD2_ENTER;

   rv = TRU1;

   ip = a_ignore_resolve_self(ip, rv = (*list != NULL));
   itp = retain ? &ip->i_retain : &ip->i_ignore;

   if(itp->it_count == 0 && !itp->it_all)
      n_err(_("No fields currently being %s\n"),
         (retain ? _("retained") : _("ignored")));
   else
      while((cp = *list++) != NULL)
         if(cp[0] == '*' && cp[1] == '\0')
            a_ignore_del_allof(ip, retain);
         else if(!a_ignore__delone(ip, retain, cp)){
            n_err(_("Field not %s: %s\n"),
               (retain ? _("retained") : _("ignored")), cp);
            rv = FAL0;
         }
   NYD2_LEAVE;
   return rv;
}

static bool_t
a_ignore__delone(struct n_ignore *ip, bool_t retain, char const *field){
   struct a_ignore_type *itp;
   NYD_ENTER;

   itp = retain ? &ip->i_retain : &ip->i_ignore;

#ifdef HAVE_REGEX
   if(n_is_maybe_regex(field)){
      struct a_ignore_re **lirp, *irp;

      for(irp = *(lirp = &itp->it_re); irp != NULL;
            lirp = &irp->ir_next, irp = irp->ir_next)
         if(!strcmp(field, irp->ir_input)){
            *lirp = irp->ir_next;
            if(irp == itp->it_re_tail)
               itp->it_re_tail = irp->ir_next;

            regfree(&irp->ir_regex);
            if(!ip->i_auto)
               n_free(irp);
            --itp->it_count;
            goto jleave;
         }
   }else
#endif /* HAVE_REGEX */
   {
      struct a_ignore_field **ifpp, *ifp;
      ui32_t hi;

      hi = torek_ihashn(field, UIZ_MAX) % n_NELEM(itp->it_ht);

      for(ifp = *(ifpp = &itp->it_ht[hi]); ifp != NULL;
            ifpp = &ifp->if_next, ifp = ifp->if_next)
         if(!asccasecmp(ifp->if_field, field)){
            *ifpp = ifp->if_next;
            if(!ip->i_auto)
               n_free(ifp);
            --itp->it_count;
           goto jleave;
         }
   }

   ip = NULL;
jleave:
   NYD_LEAVE;
   return (ip != NULL);
}

FL int
c_headerpick(void *v){
   bool_t retain;
   struct a_ignore_bltin_map const *ibmp;
   char const **argv;
   int rv;
   NYD_ENTER;

   rv = 1;
   argv = v;

   /* Without arguments, show all settings of all contexts */
   if(*argv == NULL){
      rv = 0;
      for(ibmp = &a_ignore_bltin_map[0];
            ibmp <= &a_ignore_bltin_map[n__IGNORE_MAX]; ++ibmp){
         rv |= !a_ignore_addcmd_mux(ibmp->ibm_ip, argv, TRU1);
         rv |= !a_ignore_addcmd_mux(ibmp->ibm_ip, argv, FAL0);
      }
      goto jleave;
   }

   if((ibmp = a_ignore_resolve_bltin(*argv)) == NULL){
      n_err(_("`headerpick': invalid context: %s\n"), *argv);
      goto jleave;
   }

   /* With only <context>, show all settings of it */
   if(*++argv == NULL){
      rv = 0;
      rv |= !a_ignore_addcmd_mux(ibmp->ibm_ip, argv, TRU1);
      rv |= !a_ignore_addcmd_mux(ibmp->ibm_ip, argv, FAL0);
      goto jleave;
   }

   if(is_asccaseprefix(*argv, "retain"))
      retain = TRU1;
   else if(is_asccaseprefix(*argv, "ignore"))
      retain = FAL0;
   else{
      n_err(_("`headerpick': invalid type (retain, ignore): %s\n"), *argv);
      goto jleave;
   }

   /* With only <context> and <type>, show its settings */
   if(*++argv == NULL){
      rv = !a_ignore_addcmd_mux(ibmp->ibm_ip, argv, retain);
      goto jleave;
   }

   if(argv[1] == NULL){
      n_err(_("Synopsis: headerpick: [<context> [<type> "
         "[<action> <header-list>]]]\n"));
      goto jleave;
   }else if(is_asccaseprefix(*argv, "add") ||
         (argv[0][0] == '+' && argv[0][1] == '\0'))
      rv = !a_ignore_addcmd_mux(ibmp->ibm_ip, ++argv, retain);
   else if(is_asccaseprefix(*argv, "remove") ||
         (argv[0][0] == '-' && argv[0][1] == '\0'))
      rv = !a_ignore_delcmd_mux(ibmp->ibm_ip, ++argv, retain);
   else
      n_err(_("`headerpick': invalid action (add, +, remove, -): %s\n"), *argv);
jleave:
   NYD_LEAVE;
   return rv;
}

FL int
c_retain(void *v){
   int rv;
   NYD_ENTER;

   rv = !a_ignore_addcmd_mux(n_IGNORE_TYPE, v, TRU1);
   NYD_LEAVE;
   return rv;
}

FL int
c_ignore(void *v){
   int rv;
   NYD_ENTER;

   rv = !a_ignore_addcmd_mux(n_IGNORE_TYPE, v, FAL0);
   NYD_LEAVE;
   return rv;
}

FL int
c_unretain(void *v){
   int rv;
   NYD_ENTER;

   rv = !a_ignore_delcmd_mux(n_IGNORE_TYPE, v, TRU1);
   NYD_LEAVE;
   return rv;
}

FL int
c_unignore(void *v){
   int rv;
   NYD_ENTER;

   rv = !a_ignore_delcmd_mux(n_IGNORE_TYPE, v, FAL0);
   NYD_LEAVE;
   return rv;
}

FL int
c_saveretain(void *v){ /* TODO v15 drop */
   int rv;
   NYD_ENTER;

   rv = !a_ignore_addcmd_mux(n_IGNORE_SAVE, v, TRU1);
   NYD_LEAVE;
   return rv;
}

FL int
c_saveignore(void *v){ /* TODO v15 drop */
   int rv;
   NYD_ENTER;

   rv = !a_ignore_addcmd_mux(n_IGNORE_SAVE, v, FAL0);
   NYD_LEAVE;
   return rv;
}

FL int
c_unsaveretain(void *v){ /* TODO v15 drop */
   int rv;
   NYD_ENTER;

   rv = !a_ignore_delcmd_mux(n_IGNORE_SAVE, v, TRU1);
   NYD_LEAVE;
   return rv;
}

FL int
c_unsaveignore(void *v){ /* TODO v15 drop */
   int rv;
   NYD_ENTER;

   rv = !a_ignore_delcmd_mux(n_IGNORE_SAVE, v, FAL0);
   NYD_LEAVE;
   return rv;
}

FL int
c_fwdretain(void *v){ /* TODO v15 drop */
   int rv;
   NYD_ENTER;

   rv = !a_ignore_addcmd_mux(n_IGNORE_FWD, v, TRU1);
   NYD_LEAVE;
   return rv;
}

FL int
c_fwdignore(void *v){ /* TODO v15 drop */
   int rv;
   NYD_ENTER;

   rv = !a_ignore_addcmd_mux(n_IGNORE_FWD, v, FAL0);
   NYD_LEAVE;
   return rv;
}

FL int
c_unfwdretain(void *v){ /* TODO v15 drop */
   int rv;
   NYD_ENTER;

   rv = !a_ignore_delcmd_mux(n_IGNORE_FWD, v, TRU1);
   NYD_LEAVE;
   return rv;
}

FL int
c_unfwdignore(void *v){ /* TODO v15 drop */
   int rv;
   NYD_ENTER;

   rv = !a_ignore_delcmd_mux(n_IGNORE_FWD, v, FAL0);
   NYD_LEAVE;
   return rv;
}

FL struct n_ignore *
n_ignore_new(bool_t isauto){
   struct n_ignore *self;
   NYD_ENTER;

   self = isauto ? n_autorec_calloc(NULL, 1, sizeof *self)
         : n_calloc(1, sizeof *self);
   self->i_auto = isauto;
   NYD_LEAVE;
   return self;
}

FL void
n_ignore_del(struct n_ignore *self){
   NYD_ENTER;
   a_ignore_del_allof(self, TRU1);
   a_ignore_del_allof(self, FAL0);
   if(!self->i_auto)
      n_free(self);
   NYD_LEAVE;
}

FL bool_t
n_ignore_is_any(struct n_ignore const *self){
   bool_t rv;
   NYD_ENTER;

   self = a_ignore_resolve_self(n_UNCONST(self), FAL0);
   rv = (self != NULL &&
         (self->i_retain.it_count != 0 || self->i_retain.it_all ||
          self->i_ignore.it_count != 0 || self->i_ignore.it_all));
   NYD_LEAVE;
   return rv;
}

FL bool_t
n_ignore_insert(struct n_ignore *self, bool_t retain,
      char const *dat, size_t len){
#ifdef HAVE_REGEX
   struct a_ignore_re *irp;
   bool_t isre;
#endif
   struct a_ignore_field *ifp;
   struct a_ignore_type *itp;
   bool_t rv;
   NYD_ENTER;

   retain = !!retain; /* Make it true bool, TRUM1 has special _lookup meaning */
   rv = FAL0;
   self = a_ignore_resolve_self(self, TRU1);

   if(len == UIZ_MAX)
      len = strlen(dat);

   /* Request to ignore or retain _anything_?  That is special-treated */
   if(len == 1 && dat[0] == '*'){
      itp = retain ? &self->i_retain : &self->i_ignore;
      if(itp->it_all)
         rv = TRUM1;
      else{
         itp->it_all = TRU1;
         a_ignore_del_allof(self, retain);
         rv = TRU1;
      }
      goto jleave;
   }

   /* Check for regular expression or valid fieldname */
#ifdef HAVE_REGEX
   if(!(isre = n_is_maybe_regex_buf(dat, len)))
#endif
   {
      char c;
      size_t i;

      for(i = 0; i < len; ++i){
         c = dat[i];
         if(!fieldnamechar(c))
            goto jleave;
      }
   }

   rv = TRUM1;
   if(a_ignore_lookup(self, retain, dat, len) == TRU1)
      goto jleave;

   itp = retain ? &self->i_retain : &self->i_ignore;

   if(itp->it_count == UI32_MAX){
      n_err(_("Header selection size limit reached, cannot insert: %.*s\n"),
         (int)n_MIN(len, SI32_MAX), dat);
      rv = FAL0;
      goto jleave;
   }

   rv = TRU1;
#ifdef HAVE_REGEX
   if(isre){
      struct a_ignore_re *x;
      size_t i;

      i = ++len + sizeof(*irp) - n_VFIELD_SIZEOF(struct a_ignore_re, ir_input);
      irp = self->i_auto ? n_autorec_alloc(NULL, i) : n_alloc(i);
      memcpy(irp->ir_input, dat, --len);
      irp->ir_input[len] = '\0';

      if(regcomp(&irp->ir_regex, irp->ir_input,
            REG_EXTENDED | REG_ICASE | REG_NOSUB)){
         n_err(_("Invalid regular expression: %s\n"), irp->ir_input);
         if(!self->i_auto)
            n_free(irp);
         rv = FAL0;
         goto jleave;
      }

      irp->ir_next = NULL;
      if((x = itp->it_re_tail) != NULL)
         x->ir_next = irp;
      else
         itp->it_re = irp;
      itp->it_re_tail = irp;
   }else
#endif /* HAVE_REGEX */
   {
      ui32_t hi;
      size_t i;

      i = sizeof(*ifp) - n_VFIELD_SIZEOF(struct a_ignore_field, if_field) +
            len + 1;
      ifp = self->i_auto ? n_autorec_alloc(NULL, i) : n_alloc(i);
      memcpy(ifp->if_field, dat, len);
      ifp->if_field[len] = '\0';
      hi = torek_ihashn(dat, len) % n_NELEM(itp->it_ht);
      ifp->if_next = itp->it_ht[hi];
      itp->it_ht[hi] = ifp;
   }
   ++itp->it_count;
jleave:
   NYD_LEAVE;
   return rv;
}

FL bool_t
n_ignore_lookup(struct n_ignore const *self, char const *dat, size_t len){
   bool_t rv;
   NYD_ENTER;

   if(self == n_IGNORE_ALL)
      rv = TRUM1;
   else if(len == 0 ||
         (self = a_ignore_resolve_self(n_UNCONST(self), FAL0)) == NULL)
      rv = FAL0;
   else if(self->i_retain.it_all)
      rv = TRU1;
   else if(self->i_retain.it_count == 0 && self->i_ignore.it_all)
      rv = TRUM1;
   else
      rv = a_ignore_lookup(self, TRUM1, dat, len);
   NYD_LEAVE;
   return rv;
}

/* s-it-mode */
