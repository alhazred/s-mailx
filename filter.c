/*@ S-nail - a mail user agent derived from Berkeley Mail.
 *@ Filter objects.
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
#define n_FILE filter

#ifndef HAVE_AMALGAMATION
# include "nail.h"
#endif

/*
 * Quotation filter
 */

/*
 * TODO quotation filter: anticipate in future data: don't break if only WS
 * TODO or a LF escaping \ follows on the line (simply reuse the latter).
 */

#ifdef HAVE_QUOTE_FOLD
CTA(QUOTE_MAX > 3);

enum qf_state {
   _QF_CLEAN,
   _QF_PREFIX,
   _QF_DATA
};

struct qf_vc {
   struct quoteflt   *self;
   char const        *buf;
   size_t            len;
};

/* Print out prefix and current quote */
static ssize_t _qf_dump_prefix(struct quoteflt *self);

/* Add one data character */
static ssize_t _qf_add_data(struct quoteflt *self, wchar_t wc);

/* State machine handlers */
static ssize_t _qf_state_prefix(struct qf_vc *vc);
static ssize_t _qf_state_data(struct qf_vc *vc);

static ssize_t
_qf_dump_prefix(struct quoteflt *self)
{
   ssize_t rv;
   size_t i;
   NYD_ENTER;

   if ((i = self->qf_pfix_len) > 0 && i != fwrite(self->qf_pfix, 1, i,
         self->qf_os))
      goto jerr;
   rv = i;

   if ((i = self->qf_currq.l) > 0 && i != fwrite(self->qf_currq.s, 1, i,
         self->qf_os))
      goto jerr;
   rv += i;
jleave:
   NYD_LEAVE;
   return rv;
jerr:
   rv = -1;
   goto jleave;
}

static ssize_t
_qf_add_data(struct quoteflt *self, wchar_t wc)
{
   char *save_b;
   ui32_t save_l, save_w;
   ssize_t rv = 0;
   int w, l;
   NYD_ENTER;

   save_l = save_w = 0; /* silence cc */
   save_b = NULL;
   /* <newline> ends state */
   if (wc == L'\n')
      goto jflush;
   if (wc == L'\r') /* TODO CR should be stripped in lower level!! */
      goto jleave;

   /* Unroll <tab> to spaces */
   if (wc == L'\t') {
      save_l = self->qf_datw;
      save_w = (save_l + QUOTE_TAB_SPACES) & ~(QUOTE_TAB_SPACES - 1);
      save_w -= save_l;
      while (save_w-- > 0) {
         ssize_t j = _qf_add_data(self, L' ');
         if (j < 0) {
            rv = j;
            break;
         }
         rv += j;
      }
      goto jleave;
   }

   w = wcwidth(wc);
   if (w == -1) {
jbad:
      ++self->qf_datw;
      self->qf_dat.s[self->qf_dat.l++] = '?';
   } else {
      l = wctomb(self->qf_dat.s + self->qf_dat.l, wc);
      if (l < 0)
         goto jbad;
      self->qf_datw += (ui32_t)w;
      self->qf_dat.l += (size_t)l;
   }

   /* TODO The last visual may excess (adjusted!) *qfold-max* if it's a wide;
    * TODO place it on the next line, break before */
   if (self->qf_datw >= self->qf_qfold_max) {
      /* If we have seen a nice breakpoint during traversal, shuffle data
       * around a bit so as to restore the trailing part after flushing */
      if (self->qf_brkl > 0) {
         save_w = self->qf_datw - self->qf_brkw;
         save_l = self->qf_dat.l - self->qf_brkl;
         save_b = self->qf_dat.s + self->qf_brkl + 2;
         memmove(save_b, save_b - 2, save_l);
         self->qf_dat.l = self->qf_brkl;
      }

      self->qf_dat.s[self->qf_dat.l++] = '\\';
jflush:
      self->qf_dat.s[self->qf_dat.l++] = '\n';
      rv = quoteflt_flush(self);

      /* Restore takeovers, if any */
      if (save_b != NULL) {
         self->qf_brk_isws = FAL0;
         self->qf_datw += save_w;
         self->qf_dat.l = save_l;
         memmove(self->qf_dat.s, save_b, save_l);
      }
   } else if (self->qf_datw >= self->qf_qfold_min && !self->qf_brk_isws) {
      bool_t isws = iswspace(wc);

      if ((isws && !self->qf_brk_isws) || self->qf_brkl == 0) {
         self->qf_brkl = self->qf_dat.l;
         self->qf_brkw = self->qf_datw;
         self->qf_brk_isws = isws;
      }
   }

   /* If state changed to prefix, perform full reset (note this implies that
    * quoteflt_flush() performs too much work..) */
   if (wc == '\n') {
      self->qf_state = _QF_PREFIX;
      self->qf_wscnt = self->qf_datw = 0;
      self->qf_currq.l = 0;
   }
jleave:
   NYD_LEAVE;
   return rv;
}

static ssize_t
_qf_state_prefix(struct qf_vc *vc)
{
   struct quoteflt *self;
   ssize_t rv;
   char const *buf;
   size_t len, i;
   wchar_t wc;
   NYD_ENTER;

   self = vc->self;
   rv = 0;

   for (buf = vc->buf, len = vc->len; len > 0;) {
      /* xxx NULL BYTE! */
      i = mbrtowc(&wc, buf, len, self->qf_mbps);
      if (i == (size_t)-1) {
         /* On hard error, don't modify mbstate_t and step one byte */
         self->qf_mbps[0] = self->qf_mbps[1];
         ++buf;
         --len;
         self->qf_wscnt = 0;
         continue;
      }
      self->qf_mbps[1] = self->qf_mbps[0];
      if (i == (size_t)-2) {
         /* Redundant shift sequence, out of buffer */
         len = 0;
         break;
      }
      buf += i;
      len -= i;

      if (wc == L'\n')
         goto jfin;
      if (iswspace(wc)) {
         ++self->qf_wscnt;
         continue;
      }
      if (i == 1 && ISQUOTE(wc)) {
         self->qf_wscnt = 0;
         if (self->qf_currq.l >= QUOTE_MAX - 3) {
            self->qf_currq.s[QUOTE_MAX - 3] = '.';
            self->qf_currq.s[QUOTE_MAX - 2] = '.';
            self->qf_currq.s[QUOTE_MAX - 1] = '.';
            self->qf_currq.l = QUOTE_MAX;
         } else
            self->qf_currq.s[self->qf_currq.l++] = buf[-1];
         continue;
      }

      /* The quote is parsed and compressed; dump it */
jfin:
      self->qf_state = _QF_DATA;
      /* Overtake WS to the current quote in order to preserve it for eventual
       * necessary follow lines, too */
      /* TODO we de-facto "normalize" to ASCII SP here which MESSES tabs!! */
      while (self->qf_wscnt-- > 0 && self->qf_currq.l < QUOTE_MAX)
         self->qf_currq.s[self->qf_currq.l++] = ' ';
      self->qf_datw = self->qf_pfix_len + self->qf_currq.l;
      self->qf_wscnt = 0;
      rv = _qf_add_data(self, wc);
      break;
   }

   vc->buf = buf;
   vc->len = len;
   NYD_LEAVE;
   return rv;
}

static ssize_t
_qf_state_data(struct qf_vc *vc)
{
   struct quoteflt *self;
   ssize_t rv;
   char const *buf;
   size_t len, i;
   wchar_t wc;
   NYD_ENTER;

   self = vc->self;
   rv = 0;

   for (buf = vc->buf, len = vc->len; len > 0;) {
      /* xxx NULL BYTE! */
      i = mbrtowc(&wc, buf, len, self->qf_mbps);
      if (i == (size_t)-1) {
         /* On hard error, don't modify mbstate_t and step one byte */
         self->qf_mbps[0] = self->qf_mbps[1];
         ++buf;
         --len;
         continue;
      }
      self->qf_mbps[1] = self->qf_mbps[0];
      if (i == (size_t)-2) {
         /* Redundant shift sequence, out of buffer */
         len = 0;
         break;
      }
      buf += i;
      len -= i;

      {  ssize_t j = _qf_add_data(self, wc);
         if (j < 0) {
            rv = j;
            break;
         }
         rv += j;
      }

      if (self->qf_state != _QF_DATA)
         break;
   }

   vc->buf = buf;
   vc->len = len;
   NYD_LEAVE;
   return rv;
}
#endif /* HAVE_QUOTE_FOLD */

FL struct quoteflt *
quoteflt_dummy(void) /* TODO LEGACY (until filters are plugged when needed) */
{
   static struct quoteflt qf_i;

   return &qf_i;
}

FL void
quoteflt_init(struct quoteflt *self, char const *prefix)
{
#ifdef HAVE_QUOTE_FOLD
   char *xcp, *cp;
#endif
   NYD_ENTER;

   memset(self, 0, sizeof *self);

   if ((self->qf_pfix = prefix) != NULL)
      self->qf_pfix_len = (ui32_t)strlen(prefix);

   /* Check wether the user wants the more fancy quoting algorithm */
   /* TODO *quote-fold*: QUOTE_MAX may excess it! */
#ifdef HAVE_QUOTE_FOLD
   if (self->qf_pfix_len > 0 && (cp = ok_vlook(quote_fold)) != NULL) {
      ui32_t qmin, qmax = (ui32_t)strtol(cp, &xcp, 10);
      /* These magic values ensure we don't bail :) */
      if (qmax < self->qf_pfix_len + 6)
         qmax = self->qf_pfix_len + 6;
      --qmax; /* The newline escape */
      if (cp == xcp || *xcp == '\0')
         qmin = (qmax >> 1) + (qmax >> 2) + (qmax >> 5);
      else {
         qmin = (ui32_t)strtol(xcp + 1, NULL, 10);
         if (qmin < qmax >> 1)
            qmin = qmax >> 1;
         else if (qmin > qmax - 2)
            qmin = qmax - 2;
      }
      self->qf_qfold_min = qmin;
      self->qf_qfold_max = qmax;

      /* Add pad for takeover copies, backslash and newline */
      self->qf_dat.s = salloc((qmax + 3) * mb_cur_max);
      self->qf_currq.s = salloc((QUOTE_MAX + 1) * mb_cur_max);
   }
#endif
   NYD_LEAVE;
}

FL void
quoteflt_destroy(struct quoteflt *self) /* xxx inline */
{
   NYD_ENTER;
   UNUSED(self);
   NYD_LEAVE;
}

FL void
quoteflt_reset(struct quoteflt *self, FILE *f) /* xxx inline */
{
   NYD_ENTER;
   self->qf_os = f;
#ifdef HAVE_QUOTE_FOLD
   self->qf_state = _QF_CLEAN;
   self->qf_dat.l =
   self->qf_currq.l = 0;
   memset(self->qf_mbps, 0, sizeof self->qf_mbps);
#endif
   NYD_LEAVE;
}

FL ssize_t
quoteflt_push(struct quoteflt *self, char const *dat, size_t len)
{
   /* (xxx Ideally the actual push() [and flush()] would be functions on their
    * xxx own, via indirect vtbl call ..) */
   ssize_t rv = 0;
   NYD_ENTER;

   if (len == 0)
      goto jleave;

   /* Bypass? XXX Finally, this filter simply should not be used, then */
   if (self->qf_pfix_len == 0) {
      if (len != fwrite(dat, 1, len, self->qf_os))
         goto jerr;
      rv = len;
   }
   /* Normal: place *indentprefix* at every BOL */
   else
#ifdef HAVE_QUOTE_FOLD
      if (self->qf_qfold_max == 0)
#endif
   {
      void *vp;
      size_t ll;
      bool_t pxok = (self->qf_qfold_min != 0);

      for (;;) {
         if (!pxok) {
            ll = self->qf_pfix_len;
            if (ll != fwrite(self->qf_pfix, 1, ll, self->qf_os))
               goto jerr;
            rv += ll;
            pxok = TRU1;
         }

         /* xxx Strictly speaking this is invalid, because only `/' and `.' are
          * xxx mandated by POSIX.1-2008 as "invariant across all locales
          * xxx supported"; though there is no charset known which uses this
          * xxx control char as part of a multibyte character; note that S-nail
          * XXX (and the Mail codebase as such) do not support EBCDIC */
         if ((vp = memchr(dat, '\n', len)) == NULL)
            ll = len;
         else {
            pxok = FAL0;
            ll = PTR2SIZE((char*)vp - dat) + 1;
         }

         if (ll != fwrite(dat, sizeof *dat, ll, self->qf_os))
            goto jerr;
         rv += ll;
         if ((len -= ll) == 0)
            break;
         dat += ll;
      }

      self->qf_qfold_min = pxok;
   }
   /* Overly complicated, though still only line-per-line: *quote-fold*.
    * - If .qf_currq.l is 0, then we are in a clean state.  Reset .qf_mbps;
    *   TODO note this means we assume that lines start with reset escape seq,
    *   TODO but i don't think this is any worse than what we currently do;
    *   TODO in 15.0, with the value carrier, we should carry conversion states
    *   TODO all along, only resetting on error (or at words for header =???=);
    *   TODO this still is weird for error handling, but we need to act more
    *   TODO stream-alike (though in practice i don't think cross-line states
    *   TODO can be found, because of compatibility reasons; however, being
    *   TODO a problem rather than a solution is not a good thing (tm))
    * - Lookout for a newline */
#ifdef HAVE_QUOTE_FOLD
   else {
      struct qf_vc vc;
      ssize_t i;

      vc.self = self;
      vc.buf = dat;
      vc.len = len;
      while (vc.len > 0) {
         switch (self->qf_state) {
         case _QF_CLEAN:
         case _QF_PREFIX:
            i = _qf_state_prefix(&vc);
            break;
         default: /* silence cc (`i' unused) */
         case _QF_DATA:
            i = _qf_state_data(&vc);
            break;
         }
         if (i < 0)
            goto jerr;
         rv += i;
      }
   }
#endif /* HAVE_QUOTE_FOLD */

jleave:
   NYD_LEAVE;
   return rv;
jerr:
   rv = -1;
   goto jleave;
}

FL ssize_t
quoteflt_flush(struct quoteflt *self)
{
   ssize_t rv = 0;
   NYD_ENTER;
   UNUSED(self);

#ifdef HAVE_QUOTE_FOLD
   if (self->qf_dat.l > 0) {
      rv = _qf_dump_prefix(self);
      if (rv >= 0) {
         size_t i = self->qf_dat.l;
         if (i == fwrite(self->qf_dat.s, 1, i, self->qf_os))
            rv += i;
         else
            rv = -1;
         self->qf_dat.l = 0;
         self->qf_brk_isws = FAL0;
         self->qf_wscnt = self->qf_brkl = self->qf_brkw = 0;
         self->qf_datw = self->qf_pfix_len + self->qf_currq.l;
      }
   }
#endif
   NYD_LEAVE;
   return rv;
}

/*
 * HTML tagsoup filter
 * TODO . Numeric &#NO; entities should also be treated by struct hf_ent
 * TODO . Yes, we COULD support CSS based quoting when we'd check type="quote"
 * TODO   (nonstandard) and watch out for style="gmail_quote" (or so, VERY
 * TODO   nonstandard) and tracking a stack of such elements (to be popped
 * TODO   once the closing element is seen).  Then, after writing a newline,
 * TODO   place sizeof(stack) ">"s first.  But aren't these HTML mails rude?
 * TODO Interlocking and non-well-formed data will break us down
 */
#ifdef HAVE_FILTER_HTML_TAGSOUP

enum hf_limits {
   _HF_MINLEN  = 10,       /* Minimum line length (can't really be smaller) */
   _HF_BRKSUB  = 8         /* Start considering line break MAX - BRKSUB */
};

enum hf_flags {
   _HF_UTF8    = 1<<0,     /* Data is in UTF-8 */
   _HF_ERROR   = 1<<1,     /* A hard error occurred, bail as soon as possible */
   _HF_NOPUT   = 1<<2,     /* (In a tag,) Don't generate output */
   _HF_IGN     = 1<<3,     /* Ignore mode on */
   _HF_ANY     = 1<<4,     /* Yet seen just any output */
   _HF_PRE     = 1<<5,     /* In <pre>formatted mode */
   _HF_ENT     = 1<<6,     /* Currently parsing an entity */
   _HF_BLANK   = 1<<7,     /* Whitespace last */
   _HF_HREF    = 1<<8,     /* External <a href=> was the last href seen */

   _HF_NL_1    = 1<<9,     /* One \n seen */
   _HF_NL_2    = 2<<9,     /* We have produced an all empty line */
   _HF_NL_MASK = _HF_NL_1 | _HF_NL_2
};

enum hf_special_actions {
   _HFSA_NEEDSEP  = -1,    /* Need an empty line (paragraph separator) */
   _HFSA_NEEDNL   = -2,    /* Need a new line start (table row) */
   _HFSA_IGN      = -3,    /* Things like <style>..</style>, <script>.. */
   _HFSA_PRE      = -4,    /* <pre>.. */
   _HFSA_PRE_END  = -5,
   _HFSA_IMG      = -6,    /* <img> */
   _HFSA_HREF     = -7,    /* <a>.. */
   _HFSA_HREF_END = -8
};

enum hf_entity_flags {
   _HFE_HAVE_UNI  = 1<<6,  /* Have a Unicode replacement character */
   _HFE_HAVE_CSTR = 1<<7,  /* Have a string replacement */
   /* We store the length of the entity name in the flags, too */
   _HFE_LENGTH_MASK = (1<<6) - 1
};

struct htmlflt_href {
   struct htmlflt_href *hfh_next;
   ui32_t      hfh_no;     /* Running sequence */
   ui32_t      hfh_len;    /* of .hfh_dat */
   char        hfh_dat[VFIELD_SIZE(0)];
};

struct htmlflt_tag {
   si32_t      hft_act;    /* char or hf_special_actions */
   /* Not NUL: character to inject, with high bit set: place a space
    * afterwards.  Note: only recognized with _HFSA_NEEDSEP or _HFSA_NEEDNL */
   char        hft_injc;
   ui8_t       hft_len;    /* Useful bytes in (NUL terminated) .hft_tag */
   char const  hft_tag[10]; /* Tag less < and > surroundings (TR, /TR, ..) */
};
CTA(SIZEOF_FIELD(struct htmlflt_tag, hft_tag) < LINESIZE); /* .hf_ign_tag */

struct hf_ent {
   ui8_t       hfe_flags;  /* enum hf_entity_flags plus length of .hfe_ent */
   char        hfe_c;      /* Plain replacement character */
   ui16_t      hfe_uni;    /* Unicode codepoint if _HFE_HAVE_UNI */
   char        hfe_cstr[5]; /* _HFE_HAVE_CSTR (e.g., &hellip; -> ...) */
   char const  hfe_ent[7]; /* Entity less & and ; surroundings */
};

/* Tag list; not binary searched :(, so try to take care a bit */
static struct htmlflt_tag const  _hf_tags[] = {
# undef _X
# undef _XC
# define _X(S,A)     {A, '\0', sizeof(S) -1, S "\0"}
# define _XC(S,C,A)  {A, C, sizeof(S) -1, S "\0"}

   _X("P", _HFSA_NEEDSEP),       _X("/P", _HFSA_NEEDNL),
   _X("DIV", _HFSA_NEEDSEP),     _X("/DIV", _HFSA_NEEDNL),
   _X("TR", _HFSA_NEEDNL),
                                 _X("/TH", '\t'),
                                 _X("/TD", '\t'),
   /* Let it stand out; also since we don't support implicit paragraphs after
    * block elements, plain running text after a list (seen in Unicode
    * announcement via Firefox) */
   _X("UL", _HFSA_NEEDSEP),      _X("/UL", _HFSA_NEEDSEP),
   _XC("LI", (char)0x80 | '*', _HFSA_NEEDSEP),
   _X("DL", _HFSA_NEEDSEP),
   _X("DT", _HFSA_NEEDNL),

   _X("A", _HFSA_HREF),          _X("/A", _HFSA_HREF_END),
   _X("IMG", _HFSA_IMG),
   _X("BR", '\n'),
   _X("PRE", _HFSA_PRE),         _X("/PRE", _HFSA_PRE_END),
   _X("TITLE", _HFSA_NEEDSEP),   /*_X("/TITLE", '\n'),*/
   _X("H1", _HFSA_NEEDSEP),      /*_X("/H1", '\n'),*/
   _X("H2", _HFSA_NEEDSEP),      /*_X("/H2", '\n'),*/
   _X("H3", _HFSA_NEEDSEP),      /*_X("/H3", '\n'),*/
   _X("H4", _HFSA_NEEDSEP),      /*_X("/H4", '\n'),*/
   _X("H5", _HFSA_NEEDSEP),      /*_X("/H5", '\n'),*/
   _X("H6", _HFSA_NEEDSEP),      /*_X("/H6", '\n'),*/

   _X("STYLE", _HFSA_IGN),
   _X("SCRIPT", _HFSA_IGN),

# undef _X
};

/* Entity list; not binary searched.. */
static struct hf_ent const       _hf_ents[] = {
# undef _X
# undef _XU
# undef _XS
# undef _XUS
# define _X(E,C)     {(sizeof(E) -1), C, 0x0u, "", E "\0"}
# define _XU(E,C,U)  {(sizeof(E) -1) | _HFE_HAVE_UNI, C, U, "", E "\0"}
# define _XS(E,S)    {(sizeof(E) -1) | _HFE_HAVE_CSTR, '\0', 0x0u,S "\0",E "\0"}
# define _XSU(E,S,U) \
   {(sizeof(E) -1) | _HFE_HAVE_UNI | _HFE_HAVE_CSTR, '\0', U, S "\0", E "\0"}

   _X("quot", '"'),
   _X("amp", '&'),
   _X("lt", '<'),                _X("gt", '>'),

   _XU("nbsp", ' ', 0x0020 /* Note: not 0x00A0 seems to be better for us */),
   _XSU("hellip", "...", 0x2026),
   _XSU("mdash", "---", 0x2014), _XSU("ndash", "--", 0x2013),
   _XSU("laquo", "<<", 0x00AB),  _XSU("raquo", ">>", 0x00BB),
   _XSU("lsaquo", "<", 0x2039),  _XSU("rsaquo", ">", 0x203A),
   _XSU("lsquo", "'", 0x2018),   _XSU("rsquo", "'", 0x2019),
   _XSU("ldquo", "\"", 0x201C),  _XSU("rdquo", "\"", 0x201D),
   _XSU("uarr", "^|", 0x2191),   _XSU("darr", "|v", 0x2193),

   _XSU("cent", "CENT", 0x00A2),
   _XSU("copy", "(C)", 0x00A9),
   _XSU("euro", "EUR", 0x20AC),
   _XSU("infin", "INFY", 0x221E),
   _XSU("pound", "GBP", 0x00A3),
   _XSU("reg", "(R)", 0x00AE),
   _XSU("sect", "S:", 0x00A7),
   _XSU("yen", "JPY", 0x00A5),

   /* German umlauts */
   _XSU("Auml", "Ae", 0x00C4),   _XSU("auml", "ae", 0x00E4),
   _XSU("Ouml", "Oe", 0x00D6),   _XSU("ouml", "oe", 0x00F6),
   _XSU("Uuml", "Ue", 0x00DC),   _XSU("uuml", "ue", 0x00FC),
   _XSU("szlig", "ss", 0x00DF)

# undef _X
# undef _XU
# undef _XS
# undef _XSU
};

/* Real output */
static struct htmlflt * _hf_dump_hrefs(struct htmlflt *self);
static struct htmlflt * _hf_dump(struct htmlflt *self);
static struct htmlflt * _hf_store(struct htmlflt *self, char c);
# ifdef HAVE_NATCH_CHAR
static struct htmlflt * __hf_sync_mbstuff(struct htmlflt *self);
# endif

/* Virtual output */
static struct htmlflt * _hf_nl(struct htmlflt *self);
static struct htmlflt * _hf_nl_force(struct htmlflt *self);
static struct htmlflt * _hf_putc(struct htmlflt *self, char c);
static struct htmlflt * _hf_putc_premode(struct htmlflt *self, char c);
static struct htmlflt * _hf_puts(struct htmlflt *self, char const *cp);
static struct htmlflt * _hf_putbuf(struct htmlflt *self,
                           char const *cp, size_t len);

/* Try to locate a param'eter in >hf_bdat, store it (non-terminated!) or NULL */
static struct htmlflt * _hf_param(struct htmlflt *self, struct str *store,
                           char const *param);

/* Expand all entities in the given parameter */
static struct htmlflt * _hf_expand_all_ents(struct htmlflt *self,
                           struct str const *param);

/* Completely parsed over a tag / an entity, interpret that */
static struct htmlflt * _hf_check_tag(struct htmlflt *self, char const *s);
static struct htmlflt * _hf_check_ent(struct htmlflt *self, char const *s,
                           size_t l);

/* Input handler */
static ssize_t          _hf_add_data(struct htmlflt *self,
                           char const *dat, size_t len);

static struct htmlflt *
_hf_dump_hrefs(struct htmlflt *self)
{
   struct htmlflt_href *hhp;
   NYD2_ENTER;

   if (!(self->hf_flags & _HF_NL_2) && putc('\n', self->hf_os) == EOF) {
      self->hf_flags |= _HF_ERROR;
      goto jleave;
   }

   /* Reverse the list */
   for (hhp = self->hf_hrefs, self->hf_hrefs = NULL; hhp != NULL;) {
      struct htmlflt_href *tmp = hhp->hfh_next;
      hhp->hfh_next = self->hf_hrefs;
      self->hf_hrefs = hhp;
      hhp = tmp;
   }

   /* Then dump it */
   while ((hhp = self->hf_hrefs) != NULL) {
      self->hf_hrefs = hhp->hfh_next;

      if (!(self->hf_flags & _HF_ERROR)) {
         int w = fprintf(self->hf_os, "  [%u] %.*s\n",
               hhp->hfh_no, (int)hhp->hfh_len, hhp->hfh_dat);
         if (w < 0)
            self->hf_flags |= _HF_ERROR;
      }
      free(hhp);
   }

   self->hf_flags |= (putc('\n', self->hf_os) == EOF)
         ?  _HF_ERROR : _HF_NL_1 | _HF_NL_2;
   self->hf_href_dist = (ui32_t)realscreenheight >> 1;
jleave:
   NYD2_LEAVE;
   return self;
}

static struct htmlflt *
_hf_dump(struct htmlflt *self)
{
   ui32_t f, l;
   char c, *cp;
   NYD2_ENTER;

   f = self->hf_flags & ~_HF_BLANK;
   l = self->hf_len;
   cp = self->hf_line;
   self->hf_mbwidth = self->hf_mboff = self->hf_last_ws = self->hf_len = 0;

   for (c = '\0'; l > 0; --l) {
      c = *cp++;
jput:
      if (putc(c, self->hf_os) == EOF) {
         self->hf_flags = (f |= _HF_ERROR);
         goto jleave;
      }
   }

   if (c != '\n') {
      f |= (f & _HF_NL_1) ? _HF_NL_2 : _HF_NL_1;
      l = 1;
      c = '\n';
      goto jput;
   }
   self->hf_flags = f;

   /* Check wether there are HREFs to dump; there is so much messy tagsoup out
    * there that it seems best not to simply dump HREFs in each _dump(), but
    * only with some gap, let's say half the real screen height */
   if (--self->hf_href_dist < 0 && (f & _HF_NL_2) && self->hf_hrefs != NULL)
      self = _hf_dump_hrefs(self);
jleave:
   NYD2_LEAVE;
   return self;
}

static struct htmlflt *
_hf_store(struct htmlflt *self, char c)
{
   ui32_t f, l, i;
   NYD2_ENTER;

   assert(c != '\n');

   f = self->hf_flags;
   l = self->hf_len;
   self->hf_line[l] = (c == '\t' ? ' ' : c);
   self->hf_len = ++l;
   if (blankspacechar(c)) {
      if (c == '\t') {
         i = 8 - ((l - 1) & 7); /* xxx magic tab width of 8 */
         if (i > 0) {
            do
               self = _hf_store(self, ' ');
            while (--i > 0);
            goto jleave;
         }
      }
      self->hf_last_ws = l;
   } else if (/*c == '.' ||*/ c == ',' || c == ';' || c == '-')
      self->hf_last_ws = l;

   i = l;
# ifdef HAVE_NATCH_CHAR /* XXX This code is really ridiculous! */
   if (mb_cur_max > 1) { /* XXX should mbrtowc() and THEN store, at least.. */
      wchar_t wc;
      int x = mbtowc(&wc, self->hf_line + self->hf_mboff, l - self->hf_mboff);

      if (x > 0) {
         self->hf_mboff += x;
         if ((x = wcwidth(wc)) == -1)
            x = 1;
         else if (iswspace(wc))
            self->hf_last_ws = l;
         i = (self->hf_mbwidth += x);
      } else {
         if (x < 0) {
            mbtowc(&wc, NULL, mb_cur_max);
            if (UICMP(32, l - self->hf_mboff, >=, mb_cur_max)) { /* XXX */
               ++self->hf_mboff;
               ++self->hf_mbwidth;
            }
         }
         i = self->hf_mbwidth;
      }
   }
# endif

   /* Do we need to break the line? */
   if (i >= self->hf_lmax - _HF_BRKSUB) {
      ui32_t lim = self->hf_lmax >> 1;

      /* Let's hope we saw a sane place to break this line! */
      if (self->hf_last_ws >= lim) {
jput:
         i = self->hf_len = self->hf_last_ws;
         self = _hf_dump(self);
         if ((self->hf_len = (l -= i)) > 0) {
            self->hf_flags &= ~_HF_NL_MASK;
            memmove(self->hf_line, self->hf_line + i, l);
# ifdef HAVE_NATCH_CHAR
            __hf_sync_mbstuff(self);
# endif
         }
         goto jleave;
      }

      /* Any 7-bit characters? */
      for (i = l; i-- >= lim;)
         if (asciichar((c = self->hf_line[i]))) {
            self->hf_last_ws = ++i;
            goto jput;
         } else if ((f & _HF_UTF8) && ((ui8_t)c & 0xC0) != 0x80) {
            self->hf_last_ws = i;
            goto jput;
         }

      /* Hard break necessary!  xxx really badly done */
      if (l >= self->hf_lmax - 1)
         self = _hf_dump(self);
   }
jleave:
   NYD2_LEAVE;
   return self;
}

# ifdef HAVE_NATCH_CHAR
static struct htmlflt *
__hf_sync_mbstuff(struct htmlflt *self)
{
   wchar_t wc;
   char const *b;
   ui32_t o, w, l;
   NYD2_ENTER;

   b = self->hf_line;
   o = w = 0;
   l = self->hf_len;
   goto jumpin;

   while (l > 0) {
      int x = mbtowc(&wc, b, l);

      if (x == 0)
         break;

      if (x > 0) {
         b += x;
         l -= x;
         o += x;
         if ((x = wcwidth(wc)) == -1)
            x = 1;
         w += x;
         continue;
      }

      /* Bad, skip over a single character.. XXX very bad indeed */
      ++b;
      ++o;
      ++w;
      --l;
jumpin:
      mbtowc(&wc, NULL, mb_cur_max);
   }

   self->hf_mboff = o;
   self->hf_mbwidth = w;

   NYD2_LEAVE;
   return self;
}
# endif /* HAVE_NATCH_CHAR */

static struct htmlflt *
_hf_nl(struct htmlflt *self)
{
   ui32_t f;
   NYD2_ENTER;

   if (!((f = self->hf_flags) & _HF_ERROR)) {
      if (f & _HF_ANY) {
         if ((f & _HF_NL_MASK) != _HF_NL_MASK)
            self = _hf_dump(self);
      } else
         self->hf_flags = (f |= _HF_NL_MASK);
   }
   NYD2_LEAVE;
   return self;
}

static struct htmlflt *
_hf_nl_force(struct htmlflt *self)
{
   NYD2_ENTER;
   if (!(self->hf_flags & _HF_ERROR))
      self = _hf_dump(self);
   NYD2_LEAVE;
   return self;
}

static struct htmlflt *
_hf_putc(struct htmlflt *self, char c)
{
   ui32_t f;
   NYD2_ENTER;

   if ((f = self->hf_flags) & _HF_ERROR)
      goto jleave;

   if (c == '\n') {
      self = _hf_nl(self);
      goto jleave;
   } else if (c == ' ' || c == '\t') {
      if ((f & _HF_BLANK) || self->hf_len == 0)
         goto jleave;
      f |= _HF_BLANK;
   } else
      f &= ~_HF_BLANK;
   f &= ~_HF_NL_MASK;
   self->hf_flags = (f |= _HF_ANY);
   self = _hf_store(self, c);
jleave:
   NYD2_LEAVE;
   return self;
}

static struct htmlflt *
_hf_putc_premode(struct htmlflt *self, char c)
{
   ui32_t f;
   NYD2_ENTER;

   if ((f = self->hf_flags) & _HF_ERROR) {
      ;
   } else if (c == '\n')
      self = _hf_nl_force(self);
   else {
      f &= ~_HF_NL_MASK;
      self->hf_flags = (f |= _HF_ANY);
      self = _hf_store(self, c);
   }
   NYD2_LEAVE;
   return self;
}

static struct htmlflt *
_hf_puts(struct htmlflt *self, char const *cp)
{
   char c;
   NYD2_ENTER;

   while ((c = *cp++) != '\0')
      self = _hf_putc(self, c);
   NYD2_LEAVE;
   return self;
}

static struct htmlflt *
_hf_putbuf(struct htmlflt *self, char const *cp, size_t len)
{
   NYD2_ENTER;

   while (len-- > 0)
      self = _hf_putc(self, *cp++);
   NYD2_LEAVE;
   return self;
}

static struct htmlflt *
_hf_param(struct htmlflt *self, struct str *store, char const *param)
{
   char *cp, c;
   size_t i;
   NYD2_ENTER;

   store->s = NULL;
   store->l = 0;

   if ((cp = UNCONST(asccasestr(self->hf_bdat, param))) == NULL)
      goto jleave;
   cp += strlen(param);

   for (;;) {
      if ((c = *cp++) == '\0')
         goto jleave;
      if (c == '=')
         break;
   }
   if ((c = *cp) == '\0')
      goto jleave;

   if (c == '"' || c == '\'') {
      char quote = c;

      /* TODO oops i have forgotten wether backslash quoting is allowed in
       * TODO quoted HTML parameter values?  not supporting that for now.. */
      if ((c = *++cp) == '\0' || c == quote)
         goto jleave;
      store->s = cp;

      while ((c = *++cp) != '\0' && c != quote)
         ;
      /* XXX ... and we simply ignore missing trailing " :> */
   } else {
      if (!whitechar(c))
         while ((c = *++cp) != '\0' && !whitechar(c))
            ;
      store->s = cp;
   }
   i = PTR2SIZE(cp - store->s);

   /* Terrible tagsoup out there, e.g., groups.google.com produces href=""
    * parameter values prefixed and suffixed by newlines!  Therefore trim the
    * value content TODO join into the parse step above! */
   for (cp = store->s; i > 0 && spacechar(*cp); ++cp, --i)
      ;
   store->s = cp;
   for (cp += i - 1; i > 0 && spacechar(*cp); --cp, --i)
      ;
   if ((store->l = i) == 0)
      store->s = NULL;
jleave:
   NYD2_LEAVE;
   return self;
}

static struct htmlflt *
_hf_expand_all_ents(struct htmlflt *self, struct str const *param)
{
   char const *cp, *maxcp, *ep;
   char c;
   size_t i;
   NYD2_ENTER;

   for (cp = param->s, maxcp = cp + param->l; cp < maxcp;)
      if ((c = *cp++) != '&')
         self = _hf_putc(self, c);
      else {
         for (ep = cp--; ep < maxcp && (c = *ep++) != ';';)
            if (c == '\0') {
               self = _hf_puts(self, cp);
               goto jleave;
            }
         if ((i = PTR2SIZE(ep - cp)) > 1)
            self = _hf_check_ent(self, cp, i);
         cp = ep;
      }
jleave:
   NYD2_LEAVE;
   return self;
}

static struct htmlflt *
_hf_check_tag(struct htmlflt *self, char const *s)
{
   char nobuf[32], c;
   struct str param;
   size_t i;
   struct htmlflt_tag const *hftp;
   ui32_t f;
   NYD2_ENTER;

   /* Extra check only */
   assert(s != NULL);
   if (*s != '<') {
      DBG( n_alert("HTML tagsoup filter _hf_check_tag() called on soup!"); )
jput_as_is:
      self = _hf_puts(self, self->hf_bdat);
      goto jleave;
   }

   for (++s, i = 0; (c = s[i]) != '\0' && c != '>' && !whitechar(c); ++i)
      /* Special massage for things like <br/>: after the slash only whitespace
       * may separate us from the closing right angle! */
      if (c == '/') {
         size_t j = i + 1;

         while ((c = s[j]) != '\0' && c != '>' && whitechar(c))
            ++j;
         if (c == '>')
            break;
      }

   for (hftp = _hf_tags;;) {
      if (i == hftp->hft_len && !ascncasecmp(s, hftp->hft_tag, i)) {
         c = s[hftp->hft_len];
         if (c == '>' || c == '/' || whitechar(c))
            break;
      }
      if (PTRCMP(++hftp, >=, _hf_tags + NELEM(_hf_tags)))
         goto jnotknown;
   }

   f = self->hf_flags;
   switch (hftp->hft_act) {
   case _HFSA_PRE_END:
      f &= ~_HF_PRE;
      if (0) {
         /* FALLTHRU */
   case _HFSA_PRE:
         f |= _HF_PRE;
      }
      self->hf_flags = f;
      /* FALLTHRU */

   case _HFSA_NEEDSEP:
      if (!(self->hf_flags & _HF_NL_2))
         self = _hf_nl(self);
      /* FALLTHRU */
   case _HFSA_NEEDNL:
      if (!(f & _HF_NL_1))
         self = _hf_nl(self);
      if (hftp->hft_injc != '\0') {
         self = _hf_putc(self, hftp->hft_injc & 0x7F);
         if ((uc_i)hftp->hft_injc & 0x80)
            self = _hf_putc(self, ' ');
      }
      break;

   case _HFSA_IGN:
      self->hf_ign_tag = hftp;
      self->hf_flags = (f |= _HF_IGN | _HF_NOPUT);
      break;

   case _HFSA_IMG:
      self = _hf_param(self, &param, "alt");
      self = _hf_putc(self, '[');
      if (param.s == NULL) {
         param.s = UNCONST("IMG");
         param.l = 3;
         goto jimg_put;
      } /* else */ if (memchr(param.s, '&', param.l) != NULL)
         self = _hf_expand_all_ents(self, &param);
      else
jimg_put:
         self = _hf_putbuf(self, param.s, param.l);
      self = _hf_putc(self, ']');
      break;

   case _HFSA_HREF:
      self = _hf_param(self, &param, "href");
      /* Ignore non-external links */
      if (param.s != NULL && *param.s != '#') {
         struct htmlflt_href *hhp = smalloc(sizeof(*hhp) -
               VFIELD_SIZEOF(struct htmlflt_href, hfh_dat) + param.l +1);

         hhp->hfh_next = self->hf_hrefs;
         hhp->hfh_no = ++self->hf_href_no;
         hhp->hfh_len = (ui32_t)param.l;
         memcpy(hhp->hfh_dat, param.s, param.l);

         snprintf(nobuf, sizeof nobuf, "[%u]", hhp->hfh_no);
         self->hf_flags = (f |= _HF_HREF);
         self->hf_hrefs = hhp;
         self = _hf_puts(self, nobuf);
      } else
         self->hf_flags = (f &= ~_HF_HREF);
      break;
   case _HFSA_HREF_END:
      if (f & _HF_HREF) {
         snprintf(nobuf, sizeof nobuf, "[/%u]", self->hf_href_no);
         self = _hf_puts(self, nobuf);
      }
      break;

   default:
      c = (char)(hftp->hft_act & 0xFF);
      self = _hf_putc(self, c);
      break;
   case '\0':
      break;
   }
jleave:
   NYD2_LEAVE;
   return self;

   /* The problem is that even invalid tagsoup is widely used, without real
    * searching i have seen e-mail address in <N@H.D> notation, and more.
    * To protect us a bit look around and possibly write the content as such */
jnotknown:
   switch (*s) {
   case '!':
   case '?':
      /* Ignore <!DOCTYPE, <!-- comments, <? PIs.. */
      goto jleave;
   case '>':
      /* Print out an empty tag as such */
      if (s[1] == '\0') {
         --s;
         goto jput_as_is;
      }
      break;
   case '/':
      ++s;
      break;
   default:
      break;
   }

   /* Also skip over : in order to suppress v:roundrect, w:anchorlock.. */
   while ((c = *s++) != '\0' && c != '>' && !whitechar(c) && c != ':')
      if (!asciichar(c) || punctchar(c)) {
         self = _hf_puts(self, self->hf_bdat);
         break;
      }
   goto jleave;
}

static struct htmlflt *
_hf_check_ent(struct htmlflt *self, char const *s, size_t l)
{
   char nobuf[32];
   char const *s_save;
   size_t l_save;
   struct hf_ent const *hfep;
   long i;
   NYD2_ENTER;

   s_save = s;
   l_save = l;
   assert(*s == '&');
   assert(l > 0);
   assert(s[l - 1] == ';');
   ++s;
   l -= 2;

   /* Numeric entity, or try named search */
   if (*s == '#') {
      i = (*++s == 'x' ? 16 : 10);

      if ((i != 16 || (++s, --l) > 0) && l < sizeof(nobuf)) {
         memcpy(nobuf, s, l);
         nobuf[l] = '\0';
         i = strtol(nobuf, NULL, i);
         if (i <= 0x7F)
            self = _hf_putc(self, (char)i);
         else if (self->hf_flags & _HF_UTF8) {
jputuni:
            l = n_utf32_to_utf8((ui32_t)i, nobuf);
            self = _hf_putbuf(self, nobuf, l);
         } else
            goto jeent;
      } else
         goto jeent;
   } else {
      ui32_t f = self->hf_flags, hf;

      for (hfep = _hf_ents; PTRCMP(hfep, <, _hf_ents + NELEM(_hf_ents)); ++hfep)
         if (l == ((hf = hfep->hfe_flags) & _HFE_LENGTH_MASK) &&
               !strncmp(s, hfep->hfe_ent, l)) {
            if ((hf & _HFE_HAVE_UNI) && (f & _HF_UTF8)) {
               i = hfep->hfe_uni;
               goto jputuni;
            } else if (hf & _HFE_HAVE_CSTR)
               self = _hf_puts(self, hfep->hfe_cstr);
            else
               self = _hf_putc(self, hfep->hfe_c);
            goto jleave;
         }
jeent:
      self = _hf_putbuf(self, s_save, l_save);
   }
jleave:
   NYD2_LEAVE;
   return self;
}

static ssize_t
_hf_add_data(struct htmlflt *self, char const *dat, size_t len)
{
   char c, *cp, *cp_max;
   bool_t hot;
   ssize_t rv = 0;
   NYD_ENTER;

   /* Final put request? */
   if (dat == NULL) {
      if (self->hf_len > 0 || self->hf_hrefs != NULL) {
         self = _hf_dump(self);
         if (self->hf_hrefs != NULL)
            self = _hf_dump_hrefs(self);
         rv = 1;
      }
      goto jleave;
   }

   /* Always ensure some initial buffer */
   if ((cp = self->hf_curr) != NULL)
      cp_max = self->hf_bmax;
   else {
      cp = self->hf_curr = self->hf_bdat = smalloc(LINESIZE);
      cp_max = self->hf_bmax = cp + LINESIZE -1; /* (Always room for NUL!) */
   }
   hot = (cp != self->hf_bdat);

   for (rv = (ssize_t)len; len > 0; --len) {
      ui32_t f = self->hf_flags;

      if (f & _HF_ERROR)
         break;
      c = *dat++;

      /* Soup is really weird, and scripts may contain almost anything (and
       * newer CSS standards are also cryptic): therefore prefix the _HF_IGN
       * test and walk until we see the required end tag */
      /* TODO For real safety _HF_IGN soup condome would also need to know
       * TODO about quoted strings so that 'var i = "</script>";' couldn't
       * TODO fool it!   We really want this mode also for _HF_NOPUT to be
       * TODO able to *gracefully* detect the tag-closing '>', but then if
       * TODO that is a single mechanism we should have made it! */
      if (f & _HF_IGN) {
         struct htmlflt_tag const *hftp = self->hf_ign_tag;
         size_t i;

         if (c == '<') {
            hot = TRU1;
jcp_reset:
            cp = self->hf_bdat;
         } else if (c == '>') {
            if (hot) {
               if ((i = PTR2SIZE(cp - self->hf_bdat)) > 1 &&
                     --i == hftp->hft_len &&
                     !ascncasecmp(self->hf_bdat + 1, hftp->hft_tag, i))
                  self->hf_flags = (f &= ~(_HF_IGN | _HF_NOPUT));
               hot = FAL0;
               goto jcp_reset;
            }
         } else if (hot) {
            *cp++ = c;
            i = PTR2SIZE(cp - self->hf_bdat);
            if ((i == 1 && c != '/') || --i > hftp->hft_len) {
               hot = FAL0;
               goto jcp_reset;
            }
         }
      } else switch (c) {
      case '<':
         /* People are using & without &amp;ing it, ditto <; be aware */
         if (f & (_HF_NOPUT | _HF_ENT)) {
            f &= ~_HF_ENT;
            /* Special case "<!--" buffer content to deal with really weird
             * things that can be done with "<!--[if gte mso 9]>" syntax */
            if (PTR2SIZE(cp - self->hf_bdat) != 4 ||
                  memcmp(self->hf_bdat, "<!--", 4)) {
               self->hf_flags = f;
               *cp = '\0';
               self = _hf_puts(self, self->hf_bdat);
               f = self->hf_flags;
            }
         }
         cp = self->hf_bdat;
         *cp++ = c;
         self->hf_flags = (f |= _HF_NOPUT);
         break;
      case '>':
         /* Weird tagsoup around, do we actually parse a tag? */
         if (!(f & _HF_NOPUT))
            goto jdo_c;
         cp[0] = c;
         cp[1] = '\0';
         f &= ~(_HF_NOPUT | _HF_ENT);
         self->hf_flags = f;
         self = _hf_check_tag(self, self->hf_bdat);
         *(cp = self->hf_bdat) = '\0'; /* xxx extra safety */
         /* Quick hack to get rid of redundant newline after <pre> XXX */
         if (!(f & _HF_PRE) && (self->hf_flags & _HF_PRE) &&
               len > 1 && *dat == '\n')
            ++dat, --len;
         break;

      case '\r': /* TODO CR should be stripped in lower level!! (Only B64!?!) */
         break;
      case '\n':
         /* End of line is not considered unless we are in PRE section.
          * However, in _HF_NOPUT mode we must be aware of tagsoup which uses
          * newlines for separating parameters */
         if (f & _HF_NOPUT)
            goto jdo_c;
         self = (f & _HF_PRE) ? _hf_nl_force(self) : _hf_putc(self, ' ');
         break;

      case '\t':
         if (!(f & _HF_PRE))
            c = ' ';
         /* FALLTHRU */
      default:
jdo_c:
         /* If not currently parsing a tag and bypassing normal output.. */
         if (!(f & _HF_NOPUT)) {
            if (cntrlchar(c))
               break;
            if (c == '&') {
               cp = self->hf_bdat;
               *cp++ = c;
               self->hf_flags = (f |= _HF_NOPUT | _HF_ENT);
            } else if (f & _HF_PRE) {
               self = _hf_putc_premode(self, c);
               self->hf_flags &= ~_HF_BLANK;
            } else
              self = _hf_putc(self, c);
         } else if ((f & _HF_ENT) && c == ';') {
            cp[0] = c;
            cp[1] = '\0';
            f &= ~(_HF_NOPUT | _HF_ENT);
            self->hf_flags = f;
           self = _hf_check_ent(self, self->hf_bdat,
               PTR2SIZE(cp + 1 - self->hf_bdat));
         } else {
            /* We may need to grow the buffer */
            if (PTRCMP(cp + 42/2, >=, cp_max)) {
               size_t i = PTR2SIZE(cp - self->hf_bdat),
                  m = PTR2SIZE(self->hf_bmax - self->hf_bdat) + LINESIZE;

               cp = self->hf_bdat = srealloc(self->hf_bdat, m);
               self->hf_bmax = cp + m -1;
               self->hf_curr = (cp += i);
            }
            *cp++ = c;
         }
      }
   }
   self->hf_curr = cp;
jleave:
  NYD_LEAVE;
  return (self->hf_flags & _HF_ERROR) ? -1 : rv;
}

/*
 * TODO Because we don't support filter chains yet this filter will be run
 * TODO in a dedicated subprocess, driven via a special Popen() mode
 */
static bool_t __hf_hadpipesig;
static void
__hf_onpipe(int signo)
{
   NYD_X; /* Signal handler */
   UNUSED(signo);
   __hf_hadpipesig = TRU1;
}

FL int
htmlflt_process_main(void)
{
   char buf[BUFFER_SIZE];
   struct htmlflt hf;
   size_t i;
   int rv;
   NYD_ENTER;

   __hf_hadpipesig = FAL0;
   safe_signal(SIGPIPE, &__hf_onpipe);

   htmlflt_init(&hf);
   htmlflt_reset(&hf, stdout);

   for (;;) {
      if ((i = fread(buf, sizeof(buf[0]), NELEM(buf), stdin)) == 0) {
         rv = !feof(stdin);
         break;
      }

      if ((rv = __hf_hadpipesig))
         break;
      /* Just use this directly.. */
      if (htmlflt_push(&hf, buf, i) < 0) {
         rv = 1;
         break;
      }
   }
   if (rv == 0 && htmlflt_flush(&hf) < 0)
      rv = 1;

   htmlflt_destroy(&hf);

   rv |= __hf_hadpipesig;
   NYD_LEAVE;
   return rv;
}

FL void
htmlflt_init(struct htmlflt *self)
{
   NYD_ENTER;
   /* (Rather redundant though) */
   memset(self, 0, sizeof *self);
   NYD_LEAVE;
}

FL void
htmlflt_destroy(struct htmlflt *self)
{
   NYD_ENTER;
   htmlflt_reset(self, NULL);
   NYD_LEAVE;
}

FL void
htmlflt_reset(struct htmlflt *self, FILE *f)
{
   struct htmlflt_href *hfhp;
   NYD_ENTER;

   while ((hfhp = self->hf_hrefs) != NULL) {
      self->hf_hrefs = hfhp->hfh_next;
      free(hfhp);
   }

   if (self->hf_bdat != NULL)
      free(self->hf_bdat);
   if (self->hf_line != NULL)
      free(self->hf_line);

   memset(self, 0, sizeof *self);

   if (f != NULL) {
      ui32_t sw = MAX(_HF_MINLEN, (ui32_t)scrnwidth);

      self->hf_line = smalloc((size_t)sw * mb_cur_max +1);
      self->hf_lmax = sw;

      if (options & OPT_UNICODE) /* TODO not truly generic */
         self->hf_flags = _HF_UTF8;
      self->hf_os = f;
   }
   NYD_LEAVE;
}

FL ssize_t
htmlflt_push(struct htmlflt *self, char const *dat, size_t len)
{
   ssize_t rv;
   NYD_ENTER;

   rv = _hf_add_data(self, dat, len);
   NYD_LEAVE;
   return rv;
}

FL ssize_t
htmlflt_flush(struct htmlflt *self)
{
   ssize_t rv;
   NYD_ENTER;

   rv = _hf_add_data(self, NULL, 0);
   rv |= !fflush(self->hf_os) ? 0 : -1;
   NYD_LEAVE;
   return rv;
}
#endif /* HAVE_FILTER_HTML_TAGSOUP */

/* s-it-mode */
