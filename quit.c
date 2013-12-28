/*@ S-nail - a mail user agent derived from Berkeley Mail.
 *@ Termination processing.
 *
 * Copyright (c) 2000-2004 Gunnar Ritter, Freiburg i. Br., Germany.
 * Copyright (c) 2012 - 2013 Steffen "Daode" Nurpmeso <sdaoden@users.sf.net>.
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

#ifndef HAVE_AMALGAMATION
# include "nail.h"
#endif

#include <fcntl.h>
#include <utime.h>

static char	_mboxname[MAXPATHLEN];	/* Name of mbox */

/* Touch the indicated file */
static void	alter(char const *name);

static int writeback(FILE *res, FILE *obuf);
static void edstop(void);

static void
alter(char const *name)
{
	struct stat sb;
	struct utimbuf utb;

	if (stat(name, &sb))
		return;
	utb.actime = time((time_t *)0) + 1;
	utb.modtime = sb.st_mtime;
	utime(name, &utb);
}

/*
 * The "quit" command.
 */
/*ARGSUSED*/
FL int
quitcmd(void *v)
{
	(void)v;
	/*
	 * If we are sourcing, then return 1 so execute() can handle it.
	 * Otherwise, return -1 to abort command loop.
	 */
	if (sourcing)
		return 1;
	return -1;
}

/*
 * Preserve all the appropriate messages back in the system
 * mailbox, and print a nice message indicated how many were
 * saved.  On any error, just return -1.  Else return 0.
 * Incorporate the any new mail that we found.
 */
static int
writeback(FILE *res, FILE *obuf)
{
	struct message *mp;
	int p, c;

	p = 0;
	if (fseek(obuf, 0L, SEEK_SET) < 0)
		return -1;
#ifndef APPEND
	if (res != NULL)
		while ((c = getc(res)) != EOF)
			putc(c, obuf);
#endif
	srelax_hold();
	for (mp = &message[0]; mp < &message[msgCount]; mp++)
		if ((mp->m_flag&MPRESERVE)||(mp->m_flag&MTOUCH)==0) {
			p++;
			if (sendmp(mp, obuf, NULL, NULL, SEND_MBOX, NULL) < 0) {
				perror(mailname);
				(void)fseek(obuf, 0L, SEEK_SET);
				srelax_rele();
				return(-1);
			}
			srelax();
		}
	srelax_rele();

#ifdef APPEND
	if (res != NULL)
		while ((c = getc(res)) != EOF)
			putc(c, obuf);
#endif
	fflush(obuf);
	ftrunc(obuf);
	if (ferror(obuf)) {
		perror(mailname);
		(void)fseek(obuf, 0L, SEEK_SET);
		return(-1);
	}
	if (res != NULL)
		Fclose(res);
	if (fseek(obuf, 0L, SEEK_SET) < 0)
		return -1;
	alter(mailname);
	if (p == 1)
		printf(tr(155, "Held 1 message in %s\n"), displayname);
	else
		printf(tr(156, "Held %d messages in %s\n"), p, displayname);
	return 0;
}

/*
 * Save all of the undetermined messages at the top of "mbox"
 * Save all untouched messages back in the system mailbox.
 * Remove the system mailbox, if none saved there.
 */
FL void
quit(void)
{
	int p, modify, anystat;
	FILE *fbuf, *rbuf, *abuf;
	struct message *mp;
	int c;
	char *tempResid;
	struct stat minfo;

	/*
	 * If we are read only, we can't do anything,
	 * so just return quickly. IMAP can set some
	 * flags (e.g. "\\Seen") so imap_quit must be
	 * called even then.
	 */
	if (mb.mb_perm == 0 && mb.mb_type != MB_IMAP)
		return;
	/* TODO lex.c:setfile() has just called hold_sigs(); before it called
	 * TODO us, but this causes uninterruptible hangs due to blocked sigs
	 * TODO anywhere except for MB_FILE (all others install their own
	 * TODO handlers, as it seems, properly); marked YYY */
	switch (mb.mb_type) {
	case MB_FILE:
		break;
	case MB_MAILDIR:
		rele_sigs(); /* YYY */
		maildir_quit();
		hold_sigs(); /* YYY */
		return;
#ifdef HAVE_POP3
	case MB_POP3:
		rele_sigs(); /* YYY */
		pop3_quit();
		hold_sigs(); /* YYY */
		return;
#endif
#ifdef HAVE_IMAP
	case MB_IMAP:
	case MB_CACHE:
		rele_sigs(); /* YYY */
		imap_quit();
		hold_sigs(); /* YYY */
		return;
#endif
	case MB_VOID:
	default:
		return;
	}
	/*
	 * If editing (not reading system mail box), then do the work
	 * in edstop()
	 */
	if (edit) {
		edstop();
		return;
	}

	/*
	 * See if there any messages to save in mbox.  If no, we
	 * can save copying mbox to /tmp and back.
	 *
	 * Check also to see if any files need to be preserved.
	 * Delete all untouched messages to keep them out of mbox.
	 * If all the messages are to be preserved, just exit with
	 * a message.
	 */

	fbuf = Zopen(mailname, "r+", &mb.mb_compressed);
	if (fbuf == NULL) {
		if (errno == ENOENT)
			return;
		goto newmail;
	}
	if (fcntl_lock(fileno(fbuf), F_WRLCK) == -1) {
nolock:
		perror(tr(157, "Unable to lock mailbox"));
		Fclose(fbuf);
		return;
	}
	if (dot_lock(mailname, fileno(fbuf), 1, stdout, ".") == -1)
		goto nolock;
	rbuf = NULL;
	if (fstat(fileno(fbuf), &minfo) >= 0 && minfo.st_size > mailsize) {
		printf(tr(158, "New mail has arrived.\n"));
		rbuf = Ftemp(&tempResid, "Rq", "w", 0600, 1);
		if (rbuf == NULL || fbuf == NULL)
			goto newmail;
#ifdef APPEND
		fseek(fbuf, (long)mailsize, SEEK_SET);
		while ((c = getc(fbuf)) != EOF)
			putc(c, rbuf);
#else
		p = minfo.st_size - mailsize;
		while (p-- > 0) {
			c = getc(fbuf);
			if (c == EOF)
				goto newmail;
			putc(c, rbuf);
		}
#endif
		Fclose(rbuf);
		if ((rbuf = Fopen(tempResid, "r")) == NULL)
			goto newmail;
		rm(tempResid);
		Ftfree(&tempResid);
	}

	anystat = holdbits();
	modify = 0;
	for (c = 0, p = 0, mp = &message[0]; mp < &message[msgCount]; mp++) {
		if (mp->m_flag & MBOX)
			c++;
		if (mp->m_flag & MPRESERVE)
			p++;
		if (mp->m_flag & MODIFY)
			modify++;
	}
	if (p == msgCount && !modify && !anystat) {
		if (p == 1)
			printf(tr(155, "Held 1 message in %s\n"), displayname);
		else if (p > 1)
			printf(tr(156, "Held %d messages in %s\n"),
				p, displayname);
		Fclose(fbuf);
		dot_unlock(mailname);
		return;
	}
	if (c == 0) {
		if (p != 0) {
			writeback(rbuf, fbuf);
			Fclose(fbuf);
			dot_unlock(mailname);
			return;
		}
		goto cream;
	}

	if (makembox() == STOP) {
		Fclose(fbuf);
		dot_unlock(mailname);
		return;
	}
	/*
	 * Now we are ready to copy back preserved files to
	 * the system mailbox, if any were requested.
	 */

	if (p != 0) {
		writeback(rbuf, fbuf);
		Fclose(fbuf);
		dot_unlock(mailname);
		return;
	}

	/*
	 * Finally, remove his /usr/mail file.
	 * If new mail has arrived, copy it back.
	 */

cream:
	if (rbuf != NULL) {
		abuf = fbuf;
		fseek(abuf, 0L, SEEK_SET);
		while ((c = getc(rbuf)) != EOF)
			putc(c, abuf);
		Fclose(rbuf);
		ftrunc(abuf);
		alter(mailname);
		Fclose(fbuf);
		dot_unlock(mailname);
		return;
	}
	demail();
	Fclose(fbuf);
	dot_unlock(mailname);
	return;

newmail:
	printf(tr(166, "Thou hast new mail.\n"));
	if (fbuf != NULL) {
		Fclose(fbuf);
		dot_unlock(mailname);
	}
}

/*
 * Adjust the message flags in each message.
 */
FL int
holdbits(void)
{
	struct message *mp;
	int anystat, autohold, holdbit, nohold;

	anystat = 0;
	autohold = value("hold") != NULL;
	holdbit = autohold ? MPRESERVE : MBOX;
	nohold = MBOX|MSAVED|MDELETED|MPRESERVE;
	if (value("keepsave") != NULL)
		nohold &= ~MSAVED;
	for (mp = &message[0]; mp < &message[msgCount]; mp++) {
		if (mp->m_flag & MNEW) {
			mp->m_flag &= ~MNEW;
			mp->m_flag |= MSTATUS;
		}
		if (mp->m_flag & (MSTATUS|MFLAG|MUNFLAG|MANSWER|MUNANSWER|
					MDRAFT|MUNDRAFT))
			anystat++;
		if ((mp->m_flag & MTOUCH) == 0)
			mp->m_flag |= MPRESERVE;
		if ((mp->m_flag & nohold) == 0)
			mp->m_flag |= holdbit;
	}
	return anystat;
}

FL void
save_mbox_for_possible_quitstuff(void) /* TODO try to get rid of that */
{
	char const *cp;

	if ((cp = expand("&")) == NULL)
		cp = "";
	n_strlcpy(_mboxname, cp, sizeof _mboxname);
}

/*
 * Create another temporary file and copy user's mbox file
 * darin.  If there is no mbox, copy nothing.
 * If he has specified "append" don't copy his mailbox,
 * just copy saveable entries at the end.
 */
FL enum okay
makembox(void)
{
	struct message *mp;
	char *mbox, *tempQuit;
	int mcount, c;
	FILE *ibuf = NULL, *obuf, *abuf;
	enum protocol	prot;

	mbox = _mboxname;
	mcount = 0;
	if (value("append") == NULL) {
		if ((obuf = Ftemp(&tempQuit, "Rm", "w", 0600, 1)) == NULL) {
			perror(tr(163, "temporary mail quit file"));
			return STOP;
		}
		if ((ibuf = Fopen(tempQuit, "r")) == NULL) {
			perror(tempQuit);
			Fclose(obuf);
		}
		rm(tempQuit);
		Ftfree(&tempQuit);
		if (ibuf == NULL)
			return STOP;

		if ((abuf = Zopen(mbox, "r", NULL)) != NULL) {
			while ((c = getc(abuf)) != EOF)
				putc(c, obuf);
			Fclose(abuf);
		}
		if (ferror(obuf)) {
			perror(tr(163, "temporary mail quit file"));
			Fclose(ibuf);
			Fclose(obuf);
			return STOP;
		}
		Fclose(obuf);

		if ((c = open(mbox, O_CREAT|O_TRUNC|O_WRONLY, 0600)) >= 0)
			close(c);
		if ((obuf = Zopen(mbox, "r+", NULL)) == NULL) {
			perror(mbox);
			Fclose(ibuf);
			return STOP;
		}
	}
	else {
		if ((obuf = Zopen(mbox, "a", NULL)) == NULL) {
			perror(mbox);
			return STOP;
		}
		fchmod(fileno(obuf), 0600);
	}

	srelax_hold();
	prot = which_protocol(mbox);
	for (mp = &message[0]; mp < &message[msgCount]; mp++) {
		if (mp->m_flag & MBOX) {
			mcount++;
			if (prot == PROTO_IMAP &&
					saveignore[0].i_count == 0 &&
					saveignore[1].i_count == 0
#ifdef HAVE_IMAP /* TODO revisit */
					&& imap_thisaccount(mbox)
#endif
			) {
#ifdef HAVE_IMAP
				if (imap_copy(mp, mp-message+1, mbox) == STOP)
#endif
					goto jerr;
			} else if (sendmp(mp, obuf, saveignore,
						NULL, SEND_MBOX, NULL) < 0) {
				perror(mbox);
jerr:
				if (ibuf)
					Fclose(ibuf);
				Fclose(obuf);
				srelax_rele();
				return STOP;
			}
			mp->m_flag |= MBOXED;
			srelax();
		}
	}
	srelax_rele();

	/*
	 * Copy the user's old mbox contents back
	 * to the end of the stuff we just saved.
	 * If we are appending, this is unnecessary.
	 */

	if (value("append") == NULL) {
		rewind(ibuf);
		c = getc(ibuf);
		while (c != EOF) {
			putc(c, obuf);
			if (ferror(obuf))
				break;
			c = getc(ibuf);
		}
		Fclose(ibuf);
		fflush(obuf);
	}
	ftrunc(obuf);
	if (ferror(obuf)) {
		perror(mbox);
		Fclose(obuf);
		return STOP;
	}
	if (Fclose(obuf) != 0) {
		if (prot != PROTO_IMAP)
			perror(mbox);
		return STOP;
	}
	if (mcount == 1)
		printf(tr(164, "Saved 1 message in mbox\n"));
	else
		printf(tr(165, "Saved %d messages in mbox\n"), mcount);
	return OKAY;
}

/*
 * Terminate an editing session by attempting to write out the user's
 * file from the temporary.  Save any new stuff appended to the file.
 */
static void
edstop(void)
{
	int gotcha, c;
	struct message *mp;
	FILE *obuf, *ibuf = NULL;
	struct stat statb;

	if (mb.mb_perm == 0)
		return;
	hold_sigs();
	for (mp = &message[0], gotcha = 0; mp < &message[msgCount]; mp++) {
		if (mp->m_flag & MNEW) {
			mp->m_flag &= ~MNEW;
			mp->m_flag |= MSTATUS;
		}
		if (mp->m_flag & (MODIFY|MDELETED|MSTATUS|MFLAG|MUNFLAG|
					MANSWER|MUNANSWER|MDRAFT|MUNDRAFT))
			gotcha++;
	}
	if (!gotcha)
		goto done;
	ibuf = NULL;
	if (stat(mailname, &statb) >= 0 && statb.st_size > mailsize) {
		char *tempname;

		if ((obuf = Ftemp(&tempname, "edstop", "w", 0600, 1)) == NULL) {
			perror(tr(167, "tmpfile"));
			rele_sigs();
			reset(0);
		}
		if ((ibuf = Zopen(mailname, "r", &mb.mb_compressed)) == NULL) {
			perror(mailname);
			Fclose(obuf);
			rm(tempname);
			Ftfree(&tempname);
			rele_sigs();
			reset(0);
		}
		fseek(ibuf, (long)mailsize, SEEK_SET);
		while ((c = getc(ibuf)) != EOF)
			putc(c, obuf);
		Fclose(ibuf);
		Fclose(obuf);
		if ((ibuf = Fopen(tempname, "r")) == NULL) {
			perror(tempname);
			rm(tempname);
			Ftfree(&tempname);
			rele_sigs();
			reset(0);
		}
		rm(tempname);
		Ftfree(&tempname);
	}
	printf(tr(168, "\"%s\" "), displayname);
	fflush(stdout);
	if ((obuf = Zopen(mailname, "r+", &mb.mb_compressed)) == NULL) {
		perror(mailname);
		rele_sigs();
		reset(0);
	}
	ftrunc(obuf);

	srelax_hold();
	c = 0;
	for (mp = &message[0]; mp < &message[msgCount]; mp++) {
		if ((mp->m_flag & MDELETED) != 0)
			continue;
		c++;
		if (sendmp(mp, obuf, NULL, NULL, SEND_MBOX, NULL) < 0) {
			perror(mailname);
			rele_sigs();
			srelax_rele();
			reset(0);/* XXX jump aways are terrible */
		}
		srelax();
	}
	srelax_rele();

	gotcha = (c == 0 && ibuf == NULL);
	if (ibuf != NULL) {
		while ((c = getc(ibuf)) != EOF)
			putc(c, obuf);
		Fclose(ibuf);
	}
	fflush(obuf);
	if (ferror(obuf)) {
		perror(mailname);
		rele_sigs();
		reset(0);
	}
	Fclose(obuf);
	if (gotcha && value("emptybox") == NULL) {
		rm(mailname);
		printf((value("bsdcompat") || value("bsdmsgs"))
			? tr(169, "removed\n") : tr(211, "removed.\n"));
	} else
		printf((value("bsdcompat") || value("bsdmsgs"))
			? tr(170, "complete\n") : tr(212, "updated.\n"));
	fflush(stdout);

done:
	rele_sigs();
}

enum quitflags {
	QUITFLAG_HOLD      = 001,
	QUITFLAG_KEEPSAVE  = 002,
	QUITFLAG_APPEND    = 004,
	QUITFLAG_EMPTYBOX  = 010
};

static const struct quitnames {
	enum quitflags	flag;
	const char	*name;
} quitnames[] = {
	{ QUITFLAG_HOLD,	"hold" },
	{ QUITFLAG_KEEPSAVE,	"keepsave" },
	{ QUITFLAG_APPEND,	"append" },
	{ QUITFLAG_EMPTYBOX,	"emptybox" },
	{ 0,			NULL }
};

FL int
savequitflags(void)
{
	enum quitflags	qf = 0;
	int	i;

	for (i = 0; quitnames[i].name; i++)
		if (value(quitnames[i].name))
			qf |= quitnames[i].flag;
	return qf;
}

FL void
restorequitflags(int qf)
{
	int	i;

	for (i = 0; quitnames[i].name; i++)
		if (qf & quitnames[i].flag) {
			if (value(quitnames[i].name) == NULL)
				var_assign(quitnames[i].name, "");
		} else if (value(quitnames[i].name))
			var_unset(quitnames[i].name);
}
