/*@ S-nail - a mail user agent derived from Berkeley Mail.
 *@ Generic SSL / S/MIME commands.
 *
 * Copyright (c) 2000-2004 Gunnar Ritter, Freiburg i. Br., Germany.
 * Copyright (c) 2012 - 2017 Steffen (Daode) Nurpmeso <steffen@sdaoden.eu>.
 */
/*
 * Copyright (c) 2002
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
 * This product includes software developed by Gunnar Ritter
 * and his contributors.
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
#define n_FILE ssl

#ifndef HAVE_AMALGAMATION
# include "nail.h"
#endif

EMPTY_FILE()
#ifdef HAVE_SSL
struct ssl_verify_levels {
   char const              sv_name[8];
   enum ssl_verify_level   sv_level;
};

/* Supported SSL/TLS verification methods: update manual on change! */
static struct ssl_verify_levels const  _ssl_verify_levels[] = {
   {"strict", SSL_VERIFY_STRICT},
   {"ask", SSL_VERIFY_ASK},
   {"warn", SSL_VERIFY_WARN},
   {"ignore", SSL_VERIFY_IGNORE}
};

FL void
ssl_set_verify_level(struct url const *urlp)
{
   size_t i;
   char *cp;
   NYD_ENTER;

   ssl_verify_level = SSL_VERIFY_ASK;
   cp = xok_vlook(ssl_verify, urlp, OXM_ALL);

   if (cp != NULL) {
      for (i = 0; i < n_NELEM(_ssl_verify_levels); ++i)
         if (!asccasecmp(_ssl_verify_levels[i].sv_name, cp)) {
            ssl_verify_level = _ssl_verify_levels[i].sv_level;
            goto jleave;
         }
      n_err(_("Invalid value of *ssl-verify*: %s\n"), cp);
   }
jleave:
   NYD_LEAVE;
}

FL enum okay
ssl_verify_decide(void)
{
   enum okay rv = STOP;
   NYD_ENTER;

   switch (ssl_verify_level) {
   case SSL_VERIFY_STRICT:
      rv = STOP;
      break;
   case SSL_VERIFY_ASK:
      rv = getapproval(NULL, FAL0) ? OKAY : STOP;
      break;
   case SSL_VERIFY_WARN:
   case SSL_VERIFY_IGNORE:
      rv = OKAY;
      break;
   }
   NYD_LEAVE;
   return rv;
}

FL enum okay
smime_split(FILE *ip, FILE **hp, FILE **bp, long xcount, int keep)
{
   struct myline {
      struct myline  *ml_next;
      size_t         ml_len;
      char           ml_buf[n_VFIELD_SIZE(0)];
   } *head, *tail;
   char *buf;
   size_t bufsize, buflen, cnt;
   int c;
   enum okay rv = STOP;
   NYD_ENTER;

   if ((*hp = Ftmp(NULL, "smimeh", OF_RDWR | OF_UNLINK | OF_REGISTER)) == NULL)
      goto jetmp;
   if ((*bp = Ftmp(NULL, "smimeb", OF_RDWR | OF_UNLINK | OF_REGISTER)) ==NULL) {
      Fclose(*hp);
jetmp:
      n_perr(_("tempfile"), 0);
      goto jleave;
   }

   head = tail = NULL;
   buf = smalloc(bufsize = LINESIZE);
   cnt = (xcount < 0) ? fsize(ip) : xcount;

   while (fgetline(&buf, &bufsize, &cnt, &buflen, ip, 0) != NULL &&
         *buf != '\n') {
      if (!ascncasecmp(buf, "content-", 8)) {
         if (keep)
            fputs("X-Encoded-", *hp);
         for (;;) {
            struct myline *ml = smalloc(n_VSTRUCT_SIZEOF(struct myline, ml_buf
                  ) + buflen +1);
            if (tail != NULL)
               tail->ml_next = ml;
            else
               head = ml;
            tail = ml;
            ml->ml_next = NULL;
            ml->ml_len = buflen;
            memcpy(ml->ml_buf, buf, buflen +1);
            if (keep)
               fwrite(buf, sizeof *buf, buflen, *hp);
            c = getc(ip);
            ungetc(c, ip);
            if (!blankchar(c))
               break;
            fgetline(&buf, &bufsize, &cnt, &buflen, ip, 0);
         }
         continue;
      }
      fwrite(buf, sizeof *buf, buflen, *hp);
   }
   fflush_rewind(*hp);

   while (head != NULL) {
      fwrite(head->ml_buf, sizeof *head->ml_buf, head->ml_len, *bp);
      tail = head;
      head = head->ml_next;
      free(tail);
   }
   putc('\n', *bp);
   while (fgetline(&buf, &bufsize, &cnt, &buflen, ip, 0) != NULL)
      fwrite(buf, sizeof *buf, buflen, *bp);
   fflush_rewind(*bp);

   free(buf);
   rv = OKAY;
jleave:
   NYD_LEAVE;
   return rv;
}

FL FILE *
smime_sign_assemble(FILE *hp, FILE *bp, FILE *sp, char const *message_digest)
{
   char *boundary;
   int c, lastc = EOF;
   FILE *op;
   NYD_ENTER;

   if ((op = Ftmp(NULL, "smimea", OF_RDWR | OF_UNLINK | OF_REGISTER)) == NULL) {
      n_perr(_("tempfile"), 0);
      goto jleave;
   }

   while ((c = getc(hp)) != EOF) {
      if (c == '\n' && lastc == '\n')
         break;
      putc(c, op);
      lastc = c;
   }

   boundary = mime_param_boundary_create();
   fprintf(op, "Content-Type: multipart/signed;\n"
      " protocol=\"application/pkcs7-signature\"; micalg=%s;\n"
      " boundary=\"%s\"\n\n", message_digest, boundary);
   fprintf(op, "This is a S/MIME signed message.\n\n--%s\n", boundary);
   while ((c = getc(bp)) != EOF)
      putc(c, op);

   fprintf(op, "\n--%s\n", boundary);
   fputs("Content-Type: application/pkcs7-signature; name=\"smime.p7s\"\n"
      "Content-Transfer-Encoding: base64\n"
      "Content-Disposition: attachment; filename=\"smime.p7s\"\n"
      "Content-Description: S/MIME digital signature\n\n", op);
   while ((c = getc(sp)) != EOF) {
      if (c == '-') {
         while ((c = getc(sp)) != EOF && c != '\n');
         continue;
      }
      putc(c, op);
   }

   fprintf(op, "\n--%s--\n", boundary);

   Fclose(hp);
   Fclose(bp);
   Fclose(sp);

   fflush(op);
   if (ferror(op)) {
      n_perr(_("signed output data"), 0);
      Fclose(op);
      op = NULL;
      goto jleave;
   }
   rewind(op);
jleave:
   NYD_LEAVE;
   return op;
}

FL FILE *
smime_encrypt_assemble(FILE *hp, FILE *yp)
{
   FILE *op;
   int c, lastc = EOF;
   NYD_ENTER;

   if ((op = Ftmp(NULL, "smimee", OF_RDWR | OF_UNLINK | OF_REGISTER)) == NULL) {
      n_perr(_("tempfile"), 0);
      goto jleave;
   }

   while ((c = getc(hp)) != EOF) {
      if (c == '\n' && lastc == '\n')
         break;
      putc(c, op);
      lastc = c;
   }

   fputs("Content-Type: application/pkcs7-mime; name=\"smime.p7m\"\n"
      "Content-Transfer-Encoding: base64\n"
      "Content-Disposition: attachment; filename=\"smime.p7m\"\n"
      "Content-Description: S/MIME encrypted message\n\n", op);
   while ((c = getc(yp)) != EOF) {
      if (c == '-') {
         while ((c = getc(yp)) != EOF && c != '\n');
         continue;
      }
      putc(c, op);
   }

   Fclose(hp);
   Fclose(yp);

   fflush(op);
   if (ferror(op)) {
      n_perr(_("encrypted output data"), 0);
      Fclose(op);
      op = NULL;
      goto jleave;
   }
   rewind(op);
jleave:
   NYD_LEAVE;
   return op;
}

FL struct message *
smime_decrypt_assemble(struct message *m, FILE *hp, FILE *bp)
{
   ui32_t lastnl = 0;
   int binary = 0;
   char *buf = NULL;
   size_t bufsize = 0, buflen, cnt;
   long lines = 0, octets = 0;
   struct message *x;
   off_t offset;
   NYD_ENTER;

   x = salloc(sizeof *x);
   *x = *m;
   fflush(mb.mb_otf);
   fseek(mb.mb_otf, 0L, SEEK_END);
   offset = ftell(mb.mb_otf);

   cnt = fsize(hp);
   while (fgetline(&buf, &bufsize, &cnt, &buflen, hp, 0) != NULL) {
      char const *cp;
      if (buf[0] == '\n')
         break;
      if ((cp = thisfield(buf, "content-transfer-encoding")) != NULL)
         if (!ascncasecmp(cp, "binary", 7))
            binary = 1;
      fwrite(buf, sizeof *buf, buflen, mb.mb_otf);
      octets += buflen;
      ++lines;
   }

   {  struct time_current save = time_current;
      time_current_update(&time_current, TRU1);
      octets += mkdate(mb.mb_otf, "X-Decoding-Date");
      time_current = save;
   }
   ++lines;

   cnt = fsize(bp);
   while (fgetline(&buf, &bufsize, &cnt, &buflen, bp, 0) != NULL) {
      lines++;
      if (!binary && buf[buflen - 1] == '\n' && buf[buflen - 2] == '\r')
         buf[--buflen - 1] = '\n';
      fwrite(buf, sizeof *buf, buflen, mb.mb_otf);
      octets += buflen;
      if (buf[0] == '\n')
         ++lastnl;
      else if (buf[buflen - 1] == '\n')
         lastnl = 1;
      else
         lastnl = 0;
   }

   while (!binary && lastnl < 2) {
      putc('\n', mb.mb_otf);
      ++lines;
      ++octets;
      ++lastnl;
   }

   Fclose(hp);
   Fclose(bp);
   free(buf);

   fflush(mb.mb_otf);
   if (ferror(mb.mb_otf)) {
      n_perr(_("decrypted output data"), 0);
      x = NULL;
      goto jleave;
   }
   x->m_size = x->m_xsize = octets;
   x->m_lines = x->m_xlines = lines;
   x->m_block = mailx_blockof(offset);
   x->m_offset = mailx_offsetof(offset);
jleave:
   NYD_LEAVE;
   return x;
}

FL int
c_certsave(void *v)
{
   int *ip, *msgvec, val;
   char *file = NULL, *str = v;
   FILE *fp;
   bool_t f;
   NYD_ENTER;

   msgvec = salloc((msgCount + 2) * sizeof *msgvec);
   val = 1;

   if ((file = laststring(str, &f, TRU1)) == NULL ||
         (file = fexpand(file, FEXP_LOCAL | FEXP_NOPROTO)) == NULL) {
      n_err(_("No file to save certificate given\n"));
      goto jleave;
   }

   if (!f) {
      msgvec[1] = 0;
      *msgvec = first(0, MMNORM);
   } else if (getmsglist(str, msgvec, 0) < 0)
      goto jleave;
   if (*msgvec == 0) {
      if (n_pstate & (n_PS_HOOK_MASK | n_PS_ROBOT))
         val = 0;
      else
         n_err(_("No applicable messages\n"));
      goto jleave;
   }

   if ((fp = Fopen(file, "a")) == NULL) {
      n_perr(file, 0);
      goto jleave;
   }
   for (val = 0, ip = msgvec;
         *ip != 0 && UICMP(z, PTR2SIZE(ip - msgvec), <, msgCount); ++ip)
      if (smime_certsave(&message[*ip - 1], *ip, fp) != OKAY)
         val = 1;
   Fclose(fp);

   if (val == 0)
      fprintf(n_stdout, "Certificate(s) saved\n");
jleave:
   NYD_LEAVE;
   return val;
}

FL enum okay
rfc2595_hostname_match(char const *host, char const *pattern)
{
   enum okay rv;
   NYD_ENTER;

   if (pattern[0] == '*' && pattern[1] == '.') {
      ++pattern;
      while (*host && *host != '.')
         ++host;
   }
   rv = !asccasecmp(host, pattern) ? OKAY : STOP;
   NYD_LEAVE;
   return rv;
}
#endif /* HAVE_SSL */

/* s-it-mode */
