/*
 * Nail - a mail user agent derived from Berkeley Mail.
 *
 * Copyright (c) 2000-2002 Gunnar Ritter, Freiburg i. Br., Germany.
 */
/*
 * Copyright (c) 1980, 1993
 *	The Regents of the University of California.  All rights reserved.
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
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
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

#ifndef lint
#ifdef	DOSCCS
static char sccsid[] = "@(#)edit.c	2.13 (gritter) 9/4/04";
#endif
#endif /* not lint */

#include "rcv.h"
#include "extern.h"
#include <sys/stat.h>
#include <unistd.h>

/*
 * Mail -- a mail program
 *
 * Perform message editing functions.
 */
static int	edit1 __P((int *, int));

/*
 * Edit a message list.
 */
int
editor(v)
	void *v;
{
	int *msgvec = v;

	return edit1(msgvec, 'e');
}

/*
 * Invoke the visual editor on a message list.
 */
int
visual(v)
	void *v;
{
	int *msgvec = v;

	return edit1(msgvec, 'v');
}

/*
 * Edit a message by writing the message into a funnily-named file
 * (which should not exist) and forking an editor on it.
 * We get the editor from the stuff above.
 */
static int
edit1(msgvec, type)
	int *msgvec;
	int type;
{
	int c;
	int i;
	FILE *fp;
	struct message *mp;
	off_t size;
	char *line = NULL, *fromline = NULL;
	size_t linesize;

	/*
	 * Deal with each message to be edited . . .
	 */
	for (i = 0; msgvec[i] && i < msgCount; i++) {
		sighandler_type sigint;

		if (i > 0) {
			char *p;

			printf(catgets(catd, CATSET, 72,
					"Edit message %d [ynq]? "), msgvec[i]);
			fflush(stdout);
			if (readline(stdin, &line, &linesize) < 0)
				break;
			for (p = line; blankchar(*p & 0377); p++);
			if (*p == 'q')
				break;
			if (*p == 'n')
				continue;
		}
		setdot(mp = &message[msgvec[i] - 1]);
		did_print_dot = 1;
		touch(mp);
		sigint = safe_signal(SIGINT, SIG_IGN);
		if (mp->m_flag & MNOFROM) {
			char *from, *date;
			size_t sz;

			from = fakefrom(mp);
			date = fakedate(mp->m_time);
			sz = strlen(from) + strlen(date) + 8;
			fromline = salloc(sz);
			snprintf(fromline, sz, "From %s %s\n", from, date);
		}
		if ((fp = setinput(&mb, mp, NEED_BODY)) == NULL)
			return 1;
		fp = run_editor(fp, mp->m_size, type,
				(mb.mb_perm & MB_EDIT) == 0, fromline, NULL);
		if (fp != NULL) {
			fseek(mb.mb_otf, 0L, SEEK_END);
			size = ftell(mb.mb_otf);
			mp->m_block = nail_blockof(size);
			mp->m_offset = nail_offsetof(size);
			mp->m_size = fsize(fp);
			mp->m_lines = 0;
			mp->m_flag |= MODIFY;
			rewind(fp);
			while ((c = getc(fp)) != EOF) {
				if (c == '\n')
					mp->m_lines++;
				if (putc(c, mb.mb_otf) == EOF)
					break;
			}
			if (ferror(mb.mb_otf))
				perror("/tmp");
			Fclose(fp);
		}
		safe_signal(SIGINT, sigint);
	}
	if (line)
		free(line);
	return 0;
}

/*
 * Run an editor on the file at "fpp" of "size" bytes,
 * and return a new file pointer.
 * Signals must be handled by the caller.
 * "Type" is 'e' for ed, 'v' for vi.
 */
FILE *
run_editor(fp, size, type, readonly, fromline, hp)
	FILE *fp;
	off_t size;
	int type, readonly;
	char *fromline;
	struct header *hp;
{
	FILE *nf = NULL;
	int t;
	time_t modtime;
	char *edit;
	struct stat statb;
	char *tempEdit;

	if ((nf = Ftemp(&tempEdit, "Re", "w", readonly ? 0400 : 0600, 1))
			== NULL) {
		perror(catgets(catd, CATSET, 73, "temporary mail edit file"));
		goto out;
	}
	if (fromline)
		fputs(fromline, nf);
	if (hp)
		puthead(hp, nf, GTO|GSUBJECT|GCC|GBCC|GNL|GCOMMA, CONV_TODISP,
				NULL, NULL);
	if (size >= 0)
		while (--size >= 0 && (t = getc(fp)) != EOF)
			putc(t, nf);
	else
		while ((t = getc(fp)) != EOF)
			putc(t, nf);
	fflush(nf);
	if (fstat(fileno(nf), &statb) < 0)
		modtime = 0;
	else
		modtime = statb.st_mtime;
	if (ferror(nf)) {
		Fclose(nf);
		perror(tempEdit);
		unlink(tempEdit);
		Ftfree(&tempEdit);
		nf = NULL;
		goto out;
	}
	if (Fclose(nf) < 0) {
		perror(tempEdit);
		unlink(tempEdit);
		Ftfree(&tempEdit);
		nf = NULL;
		goto out;
	}
	nf = NULL;
	if ((edit = value(type == 'e' ? "EDITOR" : "VISUAL")) == NULL)
		edit = type == 'e' ? "ed" : "vi";
	if (run_command(edit, 0, -1, -1, tempEdit, NULL, NULL) < 0) {
		unlink(tempEdit);
		Ftfree(&tempEdit);
		goto out;
	}
	/*
	 * If in read only mode or file unchanged, just remove the editor
	 * temporary and return.
	 */
	if (readonly) {
		unlink(tempEdit);
		Ftfree(&tempEdit);
		goto out;
	}
	if (stat(tempEdit, &statb) < 0) {
		perror(tempEdit);
		Ftfree(&tempEdit);
		goto out;
	}
	if (modtime == statb.st_mtime) {
		unlink(tempEdit);
		Ftfree(&tempEdit);
		goto out;
	}
	/*
	 * Now switch to new file.
	 */
	if ((nf = Fopen(tempEdit, "a+")) == NULL)
		perror(tempEdit);
out:
	unlink(tempEdit);
	Ftfree(&tempEdit);
	return nf;
}
