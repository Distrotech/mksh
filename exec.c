/*	$OpenBSD: exec.c,v 1.50 2013/06/10 21:09:27 millert Exp $	*/

/*-
 * Copyright (c) 2003, 2004, 2005, 2006, 2007, 2008, 2009, 2010,
 *		 2011, 2012, 2013, 2014
 *	Thorsten Glaser <tg@mirbsd.org>
 *
 * Provided that these terms and disclaimer and all copyright notices
 * are retained or reproduced in an accompanying document, permission
 * is granted to deal in this work without restriction, including un-
 * limited rights to use, publicly perform, distribute, sell, modify,
 * merge, give away, or sublicence.
 *
 * This work is provided "AS IS" and WITHOUT WARRANTY of any kind, to
 * the utmost extent permitted by applicable law, neither express nor
 * implied; without malicious intent or gross negligence. In no event
 * may a licensor, author or contributor be held liable for indirect,
 * direct, other damage, loss, or other issues arising in any way out
 * of dealing in the work, even if advised of the possibility of such
 * damage or existence of a defect, except proven that it results out
 * of said person's immediate fault when using the work as intended.
 */

#include "sh.h"

__RCSID("$MirOS: src/bin/mksh/exec.c,v 1.129 2014/01/11 16:26:27 tg Exp $");

#ifndef MKSH_DEFAULT_EXECSHELL
#define MKSH_DEFAULT_EXECSHELL	"/bin/sh"
#endif

static int comexec(struct op *, struct tbl * volatile, const char **,
    int volatile, volatile int *);
static void scriptexec(struct op *, const char **) MKSH_A_NORETURN;
static int call_builtin(struct tbl *, const char **, const char *);
static int iosetup(struct ioword *, struct tbl *);
static int herein(struct ioword *, char **);
static const char *do_selectargs(const char **, bool);
static Test_op dbteste_isa(Test_env *, Test_meta);
static const char *dbteste_getopnd(Test_env *, Test_op, bool);
static void dbteste_error(Test_env *, int, const char *);
static int search_access(const char *, int);
/* XXX: horrible kludge to fit within the framework */
static char *plain_fmt_entry(char *, size_t, unsigned int, const void *);
static char *select_fmt_entry(char *, size_t, unsigned int, const void *);

/*
 * execute command tree
 */
int
execute(struct op * volatile t,
    /* if XEXEC don't fork */
    volatile int flags,
    volatile int * volatile xerrok)
{
	int i;
	volatile int rv = 0, dummy = 0;
	int pv[2];
	const char ** volatile ap = NULL;
	char ** volatile up;
	const char *s, *ccp;
	struct ioword **iowp;
	struct tbl *tp = NULL;
	char *cp;

	if (t == NULL)
		return (0);

	/* Caller doesn't care if XERROK should propagate. */
	if (xerrok == NULL)
		xerrok = &dummy;

	if ((flags&XFORK) && !(flags&XEXEC) && t->type != TPIPE)
		/* run in sub-process */
		return (exchild(t, flags & ~XTIME, xerrok, -1));

	newenv(E_EXEC);
	if (trap)
		runtraps(0);

	/* we want to run an executable, do some variance checks */
	if (t->type == TCOM) {
		/* check if this is 'var=<<EOF' */
		if (
		    /* we have zero arguments, i.e. no programme to run */
		    t->args[0] == NULL &&
		    /* we have exactly one variable assignment */
		    t->vars[0] != NULL && t->vars[1] == NULL &&
		    /* we have exactly one I/O redirection */
		    t->ioact != NULL && t->ioact[0] != NULL &&
		    t->ioact[1] == NULL &&
		    /* of type "here document" (or "here string") */
		    (t->ioact[0]->flag & IOTYPE) == IOHERE &&
		    /* the variable assignment begins with a valid varname */
		    (ccp = skip_wdvarname(t->vars[0], true)) != t->vars[0] &&
		    /* and has no right-hand side (i.e. "varname=") */
		    ccp[0] == CHAR && ccp[1] == '=' && ccp[2] == EOS &&
		    /* plus we can have a here document content */
		    herein(t->ioact[0], &cp) == 0 && cp && *cp) {
			char *sp = cp, *dp;
			size_t n = ccp - t->vars[0] + 2, z;

			/* drop redirection (will be garbage collected) */
			t->ioact = NULL;

			/* set variable to its expanded value */
			z = strlen(cp) + 1;
			if (notoktomul(z, 2) || notoktoadd(z * 2, n))
				internal_errorf(Toomem, (size_t)-1);
			dp = alloc(z * 2 + n, ATEMP);
			memcpy(dp, t->vars[0], n);
			t->vars[0] = dp;
			dp += n;
			while (*sp) {
				*dp++ = QCHAR;
				*dp++ = *sp++;
			}
			*dp = EOS;
			/* free the expanded value */
			afree(cp, APERM);
		}

		/*
		 * Clear subst_exstat before argument expansion. Used by
		 * null commands (see comexec() and c_eval()) and by c_set().
		 */
		subst_exstat = 0;

		/* for $LINENO */
		current_lineno = t->lineno;

		/*
		 * POSIX says expand command words first, then redirections,
		 * and assignments last..
		 */
		up = eval(t->args, t->u.evalflags | DOBLANK | DOGLOB | DOTILDE);
		if (flags & XTIME)
			/* Allow option parsing (bizarre, but POSIX) */
			timex_hook(t, &up);
		ap = (const char **)up;
		if (ap[0])
			tp = findcom(ap[0], FC_BI|FC_FUNC);
	}
	flags &= ~XTIME;

	if (t->ioact != NULL || t->type == TPIPE || t->type == TCOPROC) {
		e->savefd = alloc2(NUFILE, sizeof(short), ATEMP);
		/* initialise to not redirected */
		memset(e->savefd, 0, NUFILE * sizeof(short));
	}

	/* mark for replacement later (unless TPIPE) */
	vp_pipest->flag |= INT_L;

	/* do redirection, to be restored in quitenv() */
	if (t->ioact != NULL)
		for (iowp = t->ioact; *iowp != NULL; iowp++) {
			if (iosetup(*iowp, tp) < 0) {
				exstat = rv = 1;
				/*
				 * Redirection failures for special commands
				 * cause (non-interactive) shell to exit.
				 */
				if (tp && tp->type == CSHELL &&
				    (tp->flag & SPEC_BI))
					errorfz();
				/* Deal with FERREXIT, quitenv(), etc. */
				goto Break;
			}
		}

	switch (t->type) {
	case TCOM:
		rv = comexec(t, tp, (const char **)ap, flags, xerrok);
		break;

	case TPAREN:
		rv = execute(t->left, flags | XFORK, xerrok);
		break;

	case TPIPE:
		flags |= XFORK;
		flags &= ~XEXEC;
		e->savefd[0] = savefd(0);
		e->savefd[1] = savefd(1);
		while (t->type == TPIPE) {
			openpipe(pv);
			/* stdout of curr */
			ksh_dup2(pv[1], 1, false);
			/**
			 * Let exchild() close pv[0] in child
			 * (if this isn't done, commands like
			 *	(: ; cat /etc/termcap) | sleep 1
			 * will hang forever).
			 */
			exchild(t->left, flags | XPIPEO | XCCLOSE,
			    NULL, pv[0]);
			/* stdin of next */
			ksh_dup2(pv[0], 0, false);
			closepipe(pv);
			flags |= XPIPEI;
			t = t->right;
		}
		/* stdout of last */
		restfd(1, e->savefd[1]);
		/* no need to re-restore this */
		e->savefd[1] = 0;
		/* Let exchild() close 0 in parent, after fork, before wait */
		i = exchild(t, flags | XPCLOSE | XPIPEST, xerrok, 0);
		if (!(flags&XBGND) && !(flags&XXCOM))
			rv = i;
		break;

	case TLIST:
		while (t->type == TLIST) {
			execute(t->left, flags & XERROK, NULL);
			t = t->right;
		}
		rv = execute(t, flags & XERROK, xerrok);
		break;

	case TCOPROC: {
#ifndef MKSH_NOPROSPECTOFWORK
		sigset_t omask;

		/*
		 * Block sigchild as we are using things changed in the
		 * signal handler
		 */
		sigprocmask(SIG_BLOCK, &sm_sigchld, &omask);
		e->type = E_ERRH;
		if ((i = kshsetjmp(e->jbuf))) {
			sigprocmask(SIG_SETMASK, &omask, NULL);
			quitenv(NULL);
			unwind(i);
			/* NOTREACHED */
		}
#endif
		/* Already have a (live) co-process? */
		if (coproc.job && coproc.write >= 0)
			errorf("coprocess already exists");

		/* Can we re-use the existing co-process pipe? */
		coproc_cleanup(true);

		/* do this before opening pipes, in case these fail */
		e->savefd[0] = savefd(0);
		e->savefd[1] = savefd(1);

		openpipe(pv);
		if (pv[0] != 0) {
			ksh_dup2(pv[0], 0, false);
			close(pv[0]);
		}
		coproc.write = pv[1];
		coproc.job = NULL;

		if (coproc.readw >= 0)
			ksh_dup2(coproc.readw, 1, false);
		else {
			openpipe(pv);
			coproc.read = pv[0];
			ksh_dup2(pv[1], 1, false);
			/* closed before first read */
			coproc.readw = pv[1];
			coproc.njobs = 0;
			/* create new coprocess id */
			++coproc.id;
		}
#ifndef MKSH_NOPROSPECTOFWORK
		sigprocmask(SIG_SETMASK, &omask, NULL);
		/* no more need for error handler */
		e->type = E_EXEC;
#endif

		/*
		 * exchild() closes coproc.* in child after fork,
		 * will also increment coproc.njobs when the
		 * job is actually created.
		 */
		flags &= ~XEXEC;
		exchild(t->left, flags | XBGND | XFORK | XCOPROC | XCCLOSE,
		    NULL, coproc.readw);
		break;
	}

	case TASYNC:
		/*
		 * XXX non-optimal, I think - "(foo &)", forks for (),
		 * forks again for async... parent should optimise
		 * this to "foo &"...
		 */
		rv = execute(t->left, (flags&~XEXEC)|XBGND|XFORK, xerrok);
		break;

	case TOR:
	case TAND:
		rv = execute(t->left, XERROK, xerrok);
		if ((rv == 0) == (t->type == TAND))
			rv = execute(t->right, flags & XERROK, xerrok);
		else {
			flags |= XERROK;
			if (xerrok)
				*xerrok = 1;
		}
		break;

	case TBANG:
		rv = !execute(t->right, XERROK, xerrok);
		flags |= XERROK;
		if (xerrok)
			*xerrok = 1;
		break;

	case TDBRACKET: {
		Test_env te;

		te.flags = TEF_DBRACKET;
		te.pos.wp = t->args;
		te.isa = dbteste_isa;
		te.getopnd = dbteste_getopnd;
		te.eval = test_eval;
		te.error = dbteste_error;

		rv = test_parse(&te);
		break;
	}

	case TFOR:
	case TSELECT: {
		volatile bool is_first = true;

		ap = (t->vars == NULL) ? e->loc->argv + 1 :
		    (const char **)eval((const char **)t->vars,
		    DOBLANK | DOGLOB | DOTILDE);
		e->type = E_LOOP;
		while ((i = kshsetjmp(e->jbuf))) {
			if ((e->flags&EF_BRKCONT_PASS) ||
			    (i != LBREAK && i != LCONTIN)) {
				quitenv(NULL);
				unwind(i);
			} else if (i == LBREAK) {
				rv = 0;
				goto Break;
			}
		}
		/* in case of a continue */
		rv = 0;
		if (t->type == TFOR) {
			while (*ap != NULL) {
				setstr(global(t->str), *ap++, KSH_UNWIND_ERROR);
				rv = execute(t->left, flags & XERROK, xerrok);
			}
		} else {
			/* TSELECT */
			for (;;) {
				if (!(ccp = do_selectargs(ap, is_first))) {
					rv = 1;
					break;
				}
				is_first = false;
				setstr(global(t->str), ccp, KSH_UNWIND_ERROR);
				execute(t->left, flags & XERROK, xerrok);
			}
		}
		break;
	}

	case TWHILE:
	case TUNTIL:
		e->type = E_LOOP;
		while ((i = kshsetjmp(e->jbuf))) {
			if ((e->flags&EF_BRKCONT_PASS) ||
			    (i != LBREAK && i != LCONTIN)) {
				quitenv(NULL);
				unwind(i);
			} else if (i == LBREAK) {
				rv = 0;
				goto Break;
			}
		}
		/* in case of a continue */
		rv = 0;
		while ((execute(t->left, XERROK, NULL) == 0) ==
		    (t->type == TWHILE))
			rv = execute(t->right, flags & XERROK, xerrok);
		break;

	case TIF:
	case TELIF:
		if (t->right == NULL)
			/* should be error */
			break;
		rv = execute(t->left, XERROK, NULL) == 0 ?
		    execute(t->right->left, flags & XERROK, xerrok) :
		    execute(t->right->right, flags & XERROK, xerrok);
		break;

	case TCASE:
		i = 0;
		ccp = evalstr(t->str, DOTILDE);
		for (t = t->left; t != NULL && t->type == TPAT; t = t->right) {
			for (ap = (const char **)t->vars; *ap; ap++) {
				if (i || ((s = evalstr(*ap, DOTILDE|DOPAT)) &&
				    gmatchx(ccp, s, false))) {
					rv = execute(t->left, flags & XERROK,
					    xerrok);
					i = 0;
					switch (t->u.charflag) {
					case '&':
						i = 1;
						/* FALLTHROUGH */
					case '|':
						goto TCASE_next;
					}
					goto TCASE_out;
				}
			}
			i = 0;
 TCASE_next:
			/* empty */;
		}
 TCASE_out:
		break;

	case TBRACE:
		rv = execute(t->left, flags & XERROK, xerrok);
		break;

	case TFUNCT:
		rv = define(t->str, t);
		break;

	case TTIME:
		/*
		 * Clear XEXEC so nested execute() call doesn't exit
		 * (allows "ls -l | time grep foo").
		 */
		rv = timex(t, flags & ~XEXEC, xerrok);
		break;

	case TEXEC:
		/* an eval'd TCOM */
		s = t->args[0];
		up = makenv();
		restoresigs();
		cleanup_proc_env();
		{
			union mksh_ccphack cargs;

			cargs.ro = t->args;
			execve(t->str, cargs.rw, up);
			rv = errno;
		}
		if (rv == ENOEXEC)
			scriptexec(t, (const char **)up);
		else
			errorf("%s: %s", s, cstrerror(rv));
	}
 Break:
	exstat = rv & 0xFF;
	if (vp_pipest->flag & INT_L) {
		unset(vp_pipest, 1);
		vp_pipest->flag = DEFINED | ISSET | INTEGER | RDONLY |
		    ARRAY | INT_U;
		vp_pipest->val.i = rv;
	}

	/* restores IO */
	quitenv(NULL);
	if ((flags&XEXEC))
		/* exit child */
		unwind(LEXIT);
	if (rv != 0 && !(flags & XERROK) &&
	    (xerrok == NULL || !*xerrok)) {
		if (Flag(FERREXIT) & 0x80) {
			/* inside eval */
			Flag(FERREXIT) = 0;
		} else {
			trapsig(ksh_SIGERR);
			if (Flag(FERREXIT))
				unwind(LERROR);
		}
	}
	return (rv);
}

/*
 * execute simple command
 */

static int
comexec(struct op *t, struct tbl * volatile tp, const char **ap,
    volatile int flags, volatile int *xerrok)
{
	int i;
	volatile int rv = 0;
	const char *cp;
	const char **lastp;
	/* Must be static (XXX but why?) */
	static struct op texec;
	int type_flags;
	bool keepasn_ok;
	int fcflags = FC_BI|FC_FUNC|FC_PATH;
	bool bourne_function_call = false;
	struct block *l_expand, *l_assign;

	/*
	 * snag the last argument for $_ XXX not the same as AT&T ksh,
	 * which only seems to set $_ after a newline (but not in
	 * functions/dot scripts, but in interactive and script) -
	 * perhaps save last arg here and set it in shell()?.
	 */
	if (Flag(FTALKING) && *(lastp = ap)) {
		while (*++lastp)
			;
		/* setstr() can't fail here */
		setstr(typeset("_", LOCAL, 0, INTEGER, 0), *--lastp,
		    KSH_RETURN_ERROR);
	}

	/**
	 * Deal with the shell builtins builtin, exec and command since
	 * they can be followed by other commands. This must be done before
	 * we know if we should create a local block which must be done
	 * before we can do a path search (in case the assignments change
	 * PATH).
	 * Odd cases:
	 *	FOO=bar exec >/dev/null		FOO is kept but not exported
	 *	FOO=bar exec foobar		FOO is exported
	 *	FOO=bar command exec >/dev/null	FOO is neither kept nor exported
	 *	FOO=bar command			FOO is neither kept nor exported
	 *	PATH=... foobar			use new PATH in foobar search
	 */
	keepasn_ok = true;
	while (tp && tp->type == CSHELL) {
		/* undo effects of command */
		fcflags = FC_BI|FC_FUNC|FC_PATH;
		if (tp->val.f == c_builtin) {
			if ((cp = *++ap) == NULL ||
			    (!strcmp(cp, "--") && (cp = *++ap) == NULL)) {
				tp = NULL;
				break;
			}
			if ((tp = findcom(cp, FC_BI)) == NULL)
				errorf("%s: %s: %s", Tbuiltin, cp, "not a builtin");
			continue;
		} else if (tp->val.f == c_exec) {
			if (ap[1] == NULL)
				break;
			ap++;
			flags |= XEXEC;
		} else if (tp->val.f == c_command) {
			int optc, saw_p = 0;

			/*
			 * Ugly dealing with options in two places (here
			 * and in c_command(), but such is life)
			 */
			ksh_getopt_reset(&builtin_opt, 0);
			while ((optc = ksh_getopt(ap, &builtin_opt, ":p")) == 'p')
				saw_p = 1;
			if (optc != EOF)
				/* command -vV or something */
				break;
			/* don't look for functions */
			fcflags = FC_BI|FC_PATH;
			if (saw_p) {
				if (Flag(FRESTRICTED)) {
					warningf(true, "%s: %s",
					    "command -p", "restricted");
					rv = 1;
					goto Leave;
				}
				fcflags |= FC_DEFPATH;
			}
			ap += builtin_opt.optind;
			/*
			 * POSIX says special builtins lose their status
			 * if accessed using command.
			 */
			keepasn_ok = false;
			if (!ap[0]) {
				/* ensure command with no args exits with 0 */
				subst_exstat = 0;
				break;
			}
#ifndef MKSH_NO_EXTERNAL_CAT
		} else if (tp->val.f == c_cat) {
			/*
			 * if we have any flags, do not use the builtin
			 * in theory, we could allow -u, but that would
			 * mean to use ksh_getopt here and possibly ad-
			 * ded complexity and more code and isn't worth
			 * additional hassle (and the builtin must call
			 * ksh_getopt already but can't come back here)
			 */
			if (ap[1] && ap[1][0] == '-' && ap[1][1] != '\0' &&
			    /* argument, begins with -, is not - or -- */
			    (ap[1][1] != '-' || ap[1][2] != '\0'))
				/* don't look for builtins or functions */
				fcflags = FC_PATH;
			else
				/* go on, use the builtin */
				break;
#endif
		} else if (tp->val.f == c_trap) {
			t->u.evalflags &= ~DOTCOMEXEC;
			break;
		} else
			break;
		tp = findcom(ap[0], fcflags & (FC_BI|FC_FUNC));
	}
	if (t->u.evalflags & DOTCOMEXEC)
		flags |= XEXEC;
	l_expand = e->loc;
	if (keepasn_ok && (!ap[0] || (tp && (tp->flag & KEEPASN))))
		type_flags = 0;
	else {
		/* create new variable/function block */
		newblock();
		/* ksh functions don't keep assignments, POSIX functions do. */
		if (keepasn_ok && tp && tp->type == CFUNC &&
		    !(tp->flag & FKSH)) {
			bourne_function_call = true;
			type_flags = EXPORT;
		} else
			type_flags = LOCAL|LOCAL_COPY|EXPORT;
	}
	l_assign = e->loc;
	if (Flag(FEXPORT))
		type_flags |= EXPORT;
	if (Flag(FXTRACE))
		change_xtrace(2, false);
	for (i = 0; t->vars[i]; i++) {
		/* do NOT lookup in the new var/fn block just created */
		e->loc = l_expand;
		cp = evalstr(t->vars[i], DOASNTILDE);
		e->loc = l_assign;
		if (Flag(FXTRACE)) {
			const char *ccp;

			ccp = skip_varname(cp, true);
			if (*ccp == '+')
				++ccp;
			if (*ccp == '=')
				++ccp;
			shf_write(cp, ccp - cp, shl_xtrace);
			print_value_quoted(shl_xtrace, ccp);
			shf_putc(' ', shl_xtrace);
		}
		/* but assign in there as usual */
		typeset(cp, type_flags, 0, 0, 0);
		if (bourne_function_call && !(type_flags & EXPORT))
			typeset(cp, LOCAL|LOCAL_COPY|EXPORT, 0, 0, 0);
	}

	if (Flag(FXTRACE)) {
		change_xtrace(2, false);
		if (ap[rv = 0]) {
 xtrace_ap_loop:
			print_value_quoted(shl_xtrace, ap[rv]);
			if (ap[++rv]) {
				shf_putc(' ', shl_xtrace);
				goto xtrace_ap_loop;
			}
		}
		change_xtrace(1, false);
	}

	if ((cp = *ap) == NULL) {
		rv = subst_exstat;
		goto Leave;
	} else if (!tp) {
		if (Flag(FRESTRICTED) && vstrchr(cp, '/')) {
			warningf(true, "%s: %s", cp, "restricted");
			rv = 1;
			goto Leave;
		}
		tp = findcom(cp, fcflags);
	}

	switch (tp->type) {

	/* shell built-in */
	case CSHELL:
		rv = call_builtin(tp, (const char **)ap, null);
		if (!keepasn_ok && tp->val.f == c_shift) {
			l_expand->argc = l_assign->argc;
			l_expand->argv = l_assign->argv;
		}
		break;

	/* function call */
	case CFUNC: {
		volatile unsigned char old_xflag;
		volatile uint32_t old_inuse;
		const char * volatile old_kshname;

		if (!(tp->flag & ISSET)) {
			struct tbl *ftp;

			if (!tp->u.fpath) {
				rv = (tp->u2.errnov == ENOENT) ? 127 : 126;
				warningf(true, "%s: %s %s: %s", cp,
				    "can't find", "function definition file",
				    cstrerror(tp->u2.errnov));
				break;
			}
			if (include(tp->u.fpath, 0, NULL, false) < 0) {
				warningf(true, "%s: %s %s %s: %s", cp,
				    "can't open", "function definition file",
				    tp->u.fpath, cstrerror(errno));
				rv = 127;
				break;
			}
			if (!(ftp = findfunc(cp, hash(cp), false)) ||
			    !(ftp->flag & ISSET)) {
				warningf(true, "%s: %s %s", cp,
				    "function not defined by", tp->u.fpath);
				rv = 127;
				break;
			}
			tp = ftp;
		}

		/*
		 * ksh functions set $0 to function name, POSIX
		 * functions leave $0 unchanged.
		 */
		old_kshname = kshname;
		if (tp->flag & FKSH)
			kshname = ap[0];
		else
			ap[0] = kshname;
		e->loc->argv = ap;
		for (i = 0; *ap++ != NULL; i++)
			;
		e->loc->argc = i - 1;
		/*
		 * ksh-style functions handle getopts sanely,
		 * Bourne/POSIX functions are insane...
		 */
		if (tp->flag & FKSH) {
			e->loc->flags |= BF_DOGETOPTS;
			e->loc->getopts_state = user_opt;
			getopts_reset(1);
		}

		old_xflag = Flag(FXTRACE) ? 1 : 0;
		change_xtrace((Flag(FXTRACEREC) ? old_xflag : 0) |
		    ((tp->flag & TRACE) ? 1 : 0), false);
		old_inuse = tp->flag & FINUSE;
		tp->flag |= FINUSE;

		e->type = E_FUNC;
		if (!(i = kshsetjmp(e->jbuf))) {
			execute(tp->val.t, flags & XERROK, NULL);
			i = LRETURN;
		}

		kshname = old_kshname;
		change_xtrace(old_xflag, false);
		tp->flag = (tp->flag & ~FINUSE) | old_inuse;

		/*
		 * Were we deleted while executing? If so, free the
		 * execution tree. TODO: Unfortunately, the table entry
		 * is never re-used until the lookup table is expanded.
		 */
		if ((tp->flag & (FDELETE|FINUSE)) == FDELETE) {
			if (tp->flag & ALLOC) {
				tp->flag &= ~ALLOC;
				tfree(tp->val.t, tp->areap);
			}
			tp->flag = 0;
		}
		switch (i) {
		case LRETURN:
		case LERROR:
			rv = exstat & 0xFF;
			break;
		case LINTR:
		case LEXIT:
		case LLEAVE:
		case LSHELL:
			quitenv(NULL);
			unwind(i);
			/* NOTREACHED */
		default:
			quitenv(NULL);
			internal_errorf("%s %d", "CFUNC", i);
		}
		break;
	}

	/* executable command */
	case CEXEC:
	/* tracked alias */
	case CTALIAS:
		if (!(tp->flag&ISSET)) {
			if (tp->u2.errnov == ENOENT) {
				rv = 127;
				warningf(true, "%s: %s", cp, "not found");
			} else {
				rv = 126;
				warningf(true, "%s: %s: %s", cp, "can't execute",
				    cstrerror(tp->u2.errnov));
			}
			break;
		}

		/* set $_ to programme's full path */
		/* setstr() can't fail here */
		setstr(typeset("_", LOCAL|EXPORT, 0, INTEGER, 0),
		    tp->val.s, KSH_RETURN_ERROR);

		if (flags&XEXEC) {
			j_exit();
			if (!(flags&XBGND)
#ifndef MKSH_UNEMPLOYED
			    || Flag(FMONITOR)
#endif
			    ) {
				setexecsig(&sigtraps[SIGINT], SS_RESTORE_ORIG);
				setexecsig(&sigtraps[SIGQUIT], SS_RESTORE_ORIG);
			}
		}

		/* to fork we set up a TEXEC node and call execute */
		texec.type = TEXEC;
		/* for tprint */
		texec.left = t;
		texec.str = tp->val.s;
		texec.args = ap;
		rv = exchild(&texec, flags, xerrok, -1);
		break;
	}
 Leave:
	if (flags & XEXEC) {
		exstat = rv & 0xFF;
		unwind(LLEAVE);
	}
	return (rv);
}

static void
scriptexec(struct op *tp, const char **ap)
{
	const char *sh;
#ifndef MKSH_SMALL
	unsigned char *cp;
	/* 64 == MAXINTERP in MirBSD <sys/param.h> */
	char buf[64];
	int fd;
#endif
	union mksh_ccphack args, cap;

	sh = str_val(global("EXECSHELL"));
	if (sh && *sh)
		sh = search_path(sh, path, X_OK, NULL);
	if (!sh || !*sh)
		sh = MKSH_DEFAULT_EXECSHELL;

	*tp->args-- = tp->str;

#ifndef MKSH_SMALL
	if ((fd = open(tp->str, O_RDONLY | O_BINARY)) >= 0) {
		/* read first MAXINTERP octets from file */
		if (read(fd, buf, sizeof(buf)) <= 0)
			/* read error -> no good */
			buf[0] = '\0';
		close(fd);

		/* skip UTF-8 Byte Order Mark, if present */
		cp = (unsigned char *)buf;
		if ((cp[0] == 0xEF) && (cp[1] == 0xBB) && (cp[2] == 0xBF))
			cp += 3;
		/* save begin of shebang for later */
		fd = (char *)cp - buf;		/* either 0 or (if BOM) 3 */

		/* scan for newline (or CR) or NUL _before_ end of buffer */
		while ((size_t)((char *)cp - buf) < sizeof(buf))
			if (*cp == '\0' || *cp == '\n' || *cp == '\r') {
				*cp = '\0';
				break;
			} else
				++cp;
		/* if the shebang line is longer than MAXINTERP, bail out */
		if ((size_t)((char *)cp - buf) >= sizeof(buf))
			goto noshebang;

		/* restore begin of shebang position (buf+0 or buf+3) */
		cp = (unsigned char *)(buf + fd);
		/* bail out if read error (above) or no shebang */
		if ((cp[0] != '#') || (cp[1] != '!'))
			goto noshebang;

		cp += 2;
		/* skip whitespace before shell name */
		while (*cp == ' ' || *cp == '\t')
			++cp;
		/* just whitespace on the line? */
		if (*cp == '\0')
			goto noshebang;
		/* no, we actually found an interpreter name */
		sh = (char *)cp;
		/* look for end of shell/interpreter name */
		while (*cp != ' ' && *cp != '\t' && *cp != '\0')
			++cp;
		/* any arguments? */
		if (*cp) {
			*cp++ = '\0';
			/* skip spaces before arguments */
			while (*cp == ' ' || *cp == '\t')
				++cp;
			/* pass it all in ONE argument (historic reasons) */
			if (*cp)
				*tp->args-- = (char *)cp;
		}
 noshebang:
		fd = buf[0] << 8 | buf[1];
		if ((fd == /* OMAGIC */ 0407) ||
		    (fd == /* NMAGIC */ 0410) ||
		    (fd == /* ZMAGIC */ 0413) ||
		    (fd == /* QMAGIC */ 0314) ||
		    (fd == /* ECOFF_I386 */ 0x4C01) ||
		    (fd == /* ECOFF_M68K */ 0x0150 || fd == 0x5001) ||
		    (fd == /* ECOFF_SH */   0x0500 || fd == 0x0005) ||
		    (fd == 0x7F45 && buf[2] == 'L' && buf[3] == 'F') ||
		    (fd == /* "MZ" */ 0x4D5A) ||
		    (fd == /* gzip */ 0x1F8B))
			errorf("%s: not executable: magic %04X", tp->str, fd);
	}
#endif
	args.ro = tp->args;
	*args.ro = sh;

	cap.ro = ap;
	execve(args.rw[0], args.rw, cap.rw);

	/* report both the programme that was run and the bogus interpreter */
	errorf("%s: %s: %s", tp->str, sh, cstrerror(errno));
}

int
shcomexec(const char **wp)
{
	struct tbl *tp;

	tp = ktsearch(&builtins, *wp, hash(*wp));
	return (call_builtin(tp, wp, "shcomexec"));
}

/*
 * Search function tables for a function. If create set, a table entry
 * is created if none is found.
 */
struct tbl *
findfunc(const char *name, uint32_t h, bool create)
{
	struct block *l;
	struct tbl *tp = NULL;

	for (l = e->loc; l; l = l->next) {
		tp = ktsearch(&l->funs, name, h);
		if (tp)
			break;
		if (!l->next && create) {
			tp = ktenter(&l->funs, name, h);
			tp->flag = DEFINED;
			tp->type = CFUNC;
			tp->val.t = NULL;
			break;
		}
	}
	return (tp);
}

/*
 * define function. Returns 1 if function is being undefined (t == 0) and
 * function did not exist, returns 0 otherwise.
 */
int
define(const char *name, struct op *t)
{
	uint32_t nhash;
	struct tbl *tp;
	bool was_set = false;

	nhash = hash(name);

	if (t != NULL && !tobool(t->u.ksh_func)) {
		/* drop same-name aliases for POSIX functions */
		if ((tp = ktsearch(&aliases, name, nhash)))
			ktdelete(tp);
	}

	while (/* CONSTCOND */ 1) {
		tp = findfunc(name, nhash, true);
		/* because findfunc:create=true */
		mkssert(tp != NULL);

		if (tp->flag & ISSET)
			was_set = true;
		/*
		 * If this function is currently being executed, we zap
		 * this table entry so findfunc() won't see it
		 */
		if (tp->flag & FINUSE) {
			tp->name[0] = '\0';
			/* ensure it won't be found */
			tp->flag &= ~DEFINED;
			tp->flag |= FDELETE;
		} else
			break;
	}

	if (tp->flag & ALLOC) {
		tp->flag &= ~(ISSET|ALLOC);
		tfree(tp->val.t, tp->areap);
	}

	if (t == NULL) {
		/* undefine */
		ktdelete(tp);
		return (was_set ? 0 : 1);
	}

	tp->val.t = tcopy(t->left, tp->areap);
	tp->flag |= (ISSET|ALLOC);
	if (t->u.ksh_func)
		tp->flag |= FKSH;

	return (0);
}

/*
 * add builtin
 */
const char *
builtin(const char *name, int (*func) (const char **))
{
	struct tbl *tp;
	uint32_t flag = DEFINED;

	/* see if any flags should be set for this builtin */
	while (1) {
		if (*name == '=')
			/* command does variable assignment */
			flag |= KEEPASN;
		else if (*name == '*')
			/* POSIX special builtin */
			flag |= SPEC_BI;
		else
			break;
		name++;
	}

	tp = ktenter(&builtins, name, hash(name));
	tp->flag = flag;
	tp->type = CSHELL;
	tp->val.f = func;

	return (name);
}

/*
 * find command
 * either function, hashed command, or built-in (in that order)
 */
struct tbl *
findcom(const char *name, int flags)
{
	static struct tbl temp;
	uint32_t h = hash(name);
	struct tbl *tp = NULL, *tbi;
	/* insert if not found */
	unsigned char insert = Flag(FTRACKALL);
	/* for function autoloading */
	char *fpath;
	union mksh_cchack npath;

	if (vstrchr(name, '/')) {
		insert = 0;
		/* prevent FPATH search below */
		flags &= ~FC_FUNC;
		goto Search;
	}
	tbi = (flags & FC_BI) ? ktsearch(&builtins, name, h) : NULL;
	/*
	 * POSIX says special builtins first, then functions, then
	 * regular builtins, then search path...
	 */
	if ((flags & FC_SPECBI) && tbi && (tbi->flag & SPEC_BI))
		tp = tbi;
	if (!tp && (flags & FC_FUNC)) {
		tp = findfunc(name, h, false);
		if (tp && !(tp->flag & ISSET)) {
			if ((fpath = str_val(global("FPATH"))) == null) {
				tp->u.fpath = NULL;
				tp->u2.errnov = ENOENT;
			} else
				tp->u.fpath = search_path(name, fpath, R_OK,
				    &tp->u2.errnov);
		}
	}
	if (!tp && (flags & FC_NORMBI) && tbi)
		tp = tbi;
	if (!tp && (flags & FC_PATH) && !(flags & FC_DEFPATH)) {
		tp = ktsearch(&taliases, name, h);
		if (tp && (tp->flag & ISSET) &&
		    ksh_access(tp->val.s, X_OK) != 0) {
			if (tp->flag & ALLOC) {
				tp->flag &= ~ALLOC;
				afree(tp->val.s, APERM);
			}
			tp->flag &= ~ISSET;
		}
	}

 Search:
	if ((!tp || (tp->type == CTALIAS && !(tp->flag&ISSET))) &&
	    (flags & FC_PATH)) {
		if (!tp) {
			if (insert && !(flags & FC_DEFPATH)) {
				tp = ktenter(&taliases, name, h);
				tp->type = CTALIAS;
			} else {
				tp = &temp;
				tp->type = CEXEC;
			}
			/* make ~ISSET */
			tp->flag = DEFINED;
		}
		npath.ro = search_path(name,
		    (flags & FC_DEFPATH) ? def_path : path,
		    X_OK, &tp->u2.errnov);
		if (npath.ro) {
			strdupx(tp->val.s, npath.ro, APERM);
			if (npath.ro != name)
				afree(npath.rw, ATEMP);
			tp->flag |= ISSET|ALLOC;
		} else if ((flags & FC_FUNC) &&
		    (fpath = str_val(global("FPATH"))) != null &&
		    (npath.ro = search_path(name, fpath, R_OK,
		    &tp->u2.errnov)) != NULL) {
			/*
			 * An undocumented feature of AT&T ksh is that
			 * it searches FPATH if a command is not found,
			 * even if the command hasn't been set up as an
			 * autoloaded function (ie, no typeset -uf).
			 */
			tp = &temp;
			tp->type = CFUNC;
			/* make ~ISSET */
			tp->flag = DEFINED;
			tp->u.fpath = npath.ro;
		}
	}
	return (tp);
}

/*
 * flush executable commands with relative paths
 * (just relative or all?)
 */
void
flushcom(bool all)
{
	struct tbl *tp;
	struct tstate ts;

	for (ktwalk(&ts, &taliases); (tp = ktnext(&ts)) != NULL; )
		if ((tp->flag&ISSET) && (all || tp->val.s[0] != '/')) {
			if (tp->flag&ALLOC) {
				tp->flag &= ~(ALLOC|ISSET);
				afree(tp->val.s, APERM);
			}
			tp->flag &= ~ISSET;
		}
}

/* check if path is something we want to find */
static int
search_access(const char *fn, int mode)
{
	struct stat sb;

	if (stat(fn, &sb) < 0)
		/* file does not exist */
		return (ENOENT);
	/* LINTED use of access */
	if (access(fn, mode) < 0)
		/* file exists, but we can't access it */
		return (errno);
	if (mode == X_OK && (!S_ISREG(sb.st_mode) ||
	    !(sb.st_mode & (S_IXUSR|S_IXGRP|S_IXOTH))))
		/* access(2) may say root can execute everything */
		return (S_ISDIR(sb.st_mode) ? EISDIR : EACCES);
	return (0);
}

/*
 * search for command with PATH
 */
const char *
search_path(const char *name, const char *lpath,
    /* R_OK or X_OK */
    int mode,
    /* set if candidate found, but not suitable */
    int *errnop)
{
	const char *sp, *p;
	char *xp;
	XString xs;
	size_t namelen;
	int ec = 0, ev;

	if (vstrchr(name, '/')) {
		if ((ec = search_access(name, mode)) == 0) {
 search_path_ok:
			if (errnop)
				*errnop = 0;
			return (name);
		}
		goto search_path_err;
	}

	namelen = strlen(name) + 1;
	Xinit(xs, xp, 128, ATEMP);

	sp = lpath;
	while (sp != NULL) {
		xp = Xstring(xs, xp);
		if (!(p = cstrchr(sp, ':')))
			p = sp + strlen(sp);
		if (p != sp) {
			XcheckN(xs, xp, p - sp);
			memcpy(xp, sp, p - sp);
			xp += p - sp;
			*xp++ = '/';
		}
		sp = p;
		XcheckN(xs, xp, namelen);
		memcpy(xp, name, namelen);
		if ((ev = search_access(Xstring(xs, xp), mode)) == 0) {
			name = Xclose(xs, xp + namelen);
			goto search_path_ok;
		}
		/* accumulate non-ENOENT errors only */
		if (ev != ENOENT && ec == 0)
			ec = ev;
		if (*sp++ == '\0')
			sp = NULL;
	}
	Xfree(xs, xp);
 search_path_err:
	if (errnop)
		*errnop = ec ? ec : ENOENT;
	return (NULL);
}

static int
call_builtin(struct tbl *tp, const char **wp, const char *where)
{
	int rv;

	if (!tp)
		internal_errorf("%s: %s", where, wp[0]);
	builtin_argv0 = wp[0];
	builtin_flag = tp->flag;
	shf_reopen(1, SHF_WR, shl_stdout);
	shl_stdout_ok = true;
	ksh_getopt_reset(&builtin_opt, GF_ERROR);
	rv = (*tp->val.f)(wp);
	shf_flush(shl_stdout);
	shl_stdout_ok = false;
	builtin_flag = 0;
	builtin_argv0 = NULL;
	return (rv);
}

/*
 * set up redirection, saving old fds in e->savefd
 */
static int
iosetup(struct ioword *iop, struct tbl *tp)
{
	int u = -1;
	char *cp = iop->name;
	int iotype = iop->flag & IOTYPE;
	bool do_open = true, do_close = false;
	int flags = 0;
	struct ioword iotmp;
	struct stat statb;

	if (iotype != IOHERE)
		cp = evalonestr(cp, DOTILDE|(Flag(FTALKING_I) ? DOGLOB : 0));

	/* Used for tracing and error messages to print expanded cp */
	iotmp = *iop;
	iotmp.name = (iotype == IOHERE) ? NULL : cp;
	iotmp.flag |= IONAMEXP;

	if (Flag(FXTRACE)) {
		change_xtrace(2, false);
		fptreef(shl_xtrace, 0, "%R", &iotmp);
		change_xtrace(1, false);
	}

	switch (iotype) {
	case IOREAD:
		flags = O_RDONLY;
		break;

	case IOCAT:
		flags = O_WRONLY | O_APPEND | O_CREAT;
		break;

	case IOWRITE:
		flags = O_WRONLY | O_CREAT | O_TRUNC;
		/*
		 * The stat() is here to allow redirections to
		 * things like /dev/null without error.
		 */
		if (Flag(FNOCLOBBER) && !(iop->flag & IOCLOB) &&
		    (stat(cp, &statb) < 0 || S_ISREG(statb.st_mode)))
			flags |= O_EXCL;
		break;

	case IORDWR:
		flags = O_RDWR | O_CREAT;
		break;

	case IOHERE:
		do_open = false;
		/* herein() returns -2 if error has been printed */
		u = herein(iop, NULL);
		/* cp may have wrong name */
		break;

	case IODUP: {
		const char *emsg;

		do_open = false;
		if (*cp == '-' && !cp[1]) {
			/* prevent error return below */
			u = 1009;
			do_close = true;
		} else if ((u = check_fd(cp,
		    X_OK | ((iop->flag & IORDUP) ? R_OK : W_OK),
		    &emsg)) < 0) {
			char *sp;

			warningf(true, "%s: %s",
			    (sp = snptreef(NULL, 32, "%R", &iotmp)), emsg);
			afree(sp, ATEMP);
			return (-1);
		}
		if (u == iop->unit)
			/* "dup from" == "dup to" */
			return (0);
		break;
	    }
	}

	if (do_open) {
		if (Flag(FRESTRICTED) && (flags & O_CREAT)) {
			warningf(true, "%s: %s", cp, "restricted");
			return (-1);
		}
		u = open(cp, flags | O_BINARY, 0666);
	}
	if (u < 0) {
		/* herein() may already have printed message */
		if (u == -1) {
			u = errno;
			warningf(true, "can't %s %s: %s",
			    iotype == IODUP ? "dup" :
			    (iotype == IOREAD || iotype == IOHERE) ?
			    "open" : "create", cp, cstrerror(u));
		}
		return (-1);
	}
	/* Do not save if it has already been redirected (i.e. "cat >x >y"). */
	if (e->savefd[iop->unit] == 0) {
		/* If these are the same, it means unit was previously closed */
		if (u == iop->unit)
			e->savefd[iop->unit] = -1;
		else
			/*
			 * c_exec() assumes e->savefd[fd] set for any
			 * redirections. Ask savefd() not to close iop->unit;
			 * this allows error messages to be seen if iop->unit
			 * is 2; also means we can't lose the fd (eg, both
			 * dup2 below and dup2 in restfd() failing).
			 */
			e->savefd[iop->unit] = savefd(iop->unit);
	}

	if (do_close)
		close(iop->unit);
	else if (u != iop->unit) {
		if (ksh_dup2(u, iop->unit, true) < 0) {
			int eno;
			char *sp;

			eno = errno;
			warningf(true, "%s %s %s",
			    "can't finish (dup) redirection",
			    (sp = snptreef(NULL, 32, "%R", &iotmp)),
			    cstrerror(eno));
			afree(sp, ATEMP);
			if (iotype != IODUP)
				close(u);
			return (-1);
		}
		if (iotype != IODUP)
			close(u);
		/*
		 * Touching any co-process fd in an empty exec
		 * causes the shell to close its copies
		 */
		else if (tp && tp->type == CSHELL && tp->val.f == c_exec) {
			if (iop->flag & IORDUP)
				/* possible exec <&p */
				coproc_read_close(u);
			else
				/* possible exec >&p */
				coproc_write_close(u);
		}
	}
	if (u == 2)
		/* Clear any write errors */
		shf_reopen(2, SHF_WR, shl_out);
	return (0);
}

/*
 * Process here documents by providing the content, either as
 * result (globally allocated) string or in a temp file; if
 * unquoted, the string is expanded first.
 */
static int
hereinval(const char *content, int sub, char **resbuf, struct shf *shf)
{
	const char * volatile ccp = content;
	struct source *s, *osource;

	osource = source;
	newenv(E_ERRH);
	if (kshsetjmp(e->jbuf)) {
		source = osource;
		quitenv(shf);
		/* special to iosetup(): don't print error */
		return (-2);
	}
	if (sub) {
		/* do substitutions on the content of heredoc */
		s = pushs(SSTRING, ATEMP);
		s->start = s->str = ccp;
		source = s;
		if (yylex(sub) != LWORD)
			internal_errorf("%s: %s", "herein", "yylex");
		source = osource;
		ccp = evalstr(yylval.cp, 0);
	}

	if (resbuf == NULL)
		shf_puts(ccp, shf);
	else
		strdupx(*resbuf, ccp, APERM);

	quitenv(NULL);
	return (0);
}

static int
herein(struct ioword *iop, char **resbuf)
{
	int fd = -1;
	struct shf *shf;
	struct temp *h;
	int i;

	/* ksh -c 'cat << EOF' can cause this... */
	if (iop->heredoc == NULL) {
		warningf(true, "%s missing", "here document");
		/* special to iosetup(): don't print error */
		return (-2);
	}

	/* lexer substitution flags */
	i = (iop->flag & IOEVAL) ? (ONEWORD | HEREDOC) : 0;

	/* skip all the fd setup if we just want the value */
	if (resbuf != NULL)
		return (hereinval(iop->heredoc, i, resbuf, NULL));

	/*
	 * Create temp file to hold content (done before newenv
	 * so temp doesn't get removed too soon).
	 */
	h = maketemp(ATEMP, TT_HEREDOC_EXP, &e->temps);
	if (!(shf = h->shf) || (fd = open(h->tffn, O_RDONLY | O_BINARY, 0)) < 0) {
		i = errno;
		warningf(true, "can't %s temporary file %s: %s",
		    !shf ? "create" : "open", h->tffn, cstrerror(i));
		if (shf)
			shf_close(shf);
		/* special to iosetup(): don't print error */
		return (-2);
	}

	if (hereinval(iop->heredoc, i, NULL, shf) == -2) {
		close(fd);
		/* special to iosetup(): don't print error */
		return (-2);
	}

	if (shf_close(shf) == EOF) {
		i = errno;
		close(fd);
		warningf(true, "can't %s temporary file %s: %s",
		    "write", h->tffn, cstrerror(i));
		/* special to iosetup(): don't print error */
		return (-2);
	}

	return (fd);
}

/*
 *	ksh special - the select command processing section
 *	print the args in column form - assuming that we can
 */
static const char *
do_selectargs(const char **ap, bool print_menu)
{
	static const char *read_args[] = {
		"read", "-r", "REPLY", NULL
	};
	char *s;
	int i, argct;

	for (argct = 0; ap[argct]; argct++)
		;
	while (/* CONSTCOND */ 1) {
		/*-
		 * Menu is printed if
		 *	- this is the first time around the select loop
		 *	- the user enters a blank line
		 *	- the REPLY parameter is empty
		 */
		if (print_menu || !*str_val(global("REPLY")))
			pr_menu(ap);
		shellf("%s", str_val(global("PS3")));
		if (call_builtin(findcom("read", FC_BI), read_args, Tselect))
			return (NULL);
		s = str_val(global("REPLY"));
		if (*s && getn(s, &i))
			return ((i >= 1 && i <= argct) ? ap[i - 1] : null);
		print_menu = true;
	}
}

struct select_menu_info {
	const char * const *args;
	int num_width;
};

/* format a single select menu item */
static char *
select_fmt_entry(char *buf, size_t buflen, unsigned int i, const void *arg)
{
	const struct select_menu_info *smi =
	    (const struct select_menu_info *)arg;

	shf_snprintf(buf, buflen, "%*u) %s",
	    smi->num_width, i + 1, smi->args[i]);
	return (buf);
}

/*
 *	print a select style menu
 */
void
pr_menu(const char * const *ap)
{
	struct select_menu_info smi;
	const char * const *pp;
	size_t acols = 0, aocts = 0, i;
	unsigned int n;

	/*
	 * width/column calculations were done once and saved, but this
	 * means select can't be used recursively so we re-calculate
	 * each time (could save in a structure that is returned, but
	 * it's probably not worth the bother)
	 */

	/*
	 * get dimensions of the list
	 */
	for (n = 0, pp = ap; *pp; n++, pp++) {
		i = strlen(*pp);
		if (i > aocts)
			aocts = i;
		i = utf_mbswidth(*pp);
		if (i > acols)
			acols = i;
	}

	/*
	 * we will print an index of the form "%d) " in front of
	 * each entry, so get the maximum width of this
	 */
	for (i = n, smi.num_width = 1; i >= 10; i /= 10)
		smi.num_width++;

	smi.args = ap;
	print_columns(shl_out, n, select_fmt_entry, (void *)&smi,
	    smi.num_width + 2 + aocts, smi.num_width + 2 + acols,
	    true);
}

static char *
plain_fmt_entry(char *buf, size_t buflen, unsigned int i, const void *arg)
{
	strlcpy(buf, ((const char * const *)arg)[i], buflen);
	return (buf);
}

void
pr_list(char * const *ap)
{
	size_t acols = 0, aocts = 0, i;
	unsigned int n;
	char * const *pp;

	for (n = 0, pp = ap; *pp; n++, pp++) {
		i = strlen(*pp);
		if (i > aocts)
			aocts = i;
		i = utf_mbswidth(*pp);
		if (i > acols)
			acols = i;
	}

	print_columns(shl_out, n, plain_fmt_entry, (const void *)ap,
	    aocts, acols, false);
}

/*
 *	[[ ... ]] evaluation routines
 */

/*
 * Test if the current token is a whatever. Accepts the current token if
 * it is. Returns 0 if it is not, non-zero if it is (in the case of
 * TM_UNOP and TM_BINOP, the returned value is a Test_op).
 */
static Test_op
dbteste_isa(Test_env *te, Test_meta meta)
{
	Test_op ret = TO_NONOP;
	bool uqword;
	const char *p;

	if (!*te->pos.wp)
		return (meta == TM_END ? TO_NONNULL : TO_NONOP);

	/* unquoted word? */
	for (p = *te->pos.wp; *p == CHAR; p += 2)
		;
	uqword = *p == EOS;

	if (meta == TM_UNOP || meta == TM_BINOP) {
		if (uqword) {
			/* longer than the longest operator */
			char buf[8];
			char *q = buf;

			p = *te->pos.wp;
			while (*p++ == CHAR &&
			    (size_t)(q - buf) < sizeof(buf) - 1)
				*q++ = *p++;
			*q = '\0';
			ret = test_isop(meta, buf);
		}
	} else if (meta == TM_END)
		ret = TO_NONOP;
	else
		ret = (uqword && !strcmp(*te->pos.wp,
		    dbtest_tokens[(int)meta])) ? TO_NONNULL : TO_NONOP;

	/* Accept the token? */
	if (ret != TO_NONOP)
		te->pos.wp++;

	return (ret);
}

static const char *
dbteste_getopnd(Test_env *te, Test_op op, bool do_eval)
{
	const char *s = *te->pos.wp;

	if (!s)
		return (NULL);

	te->pos.wp++;

	if (!do_eval)
		return (null);

	if (op == TO_STEQL || op == TO_STNEQ)
		s = evalstr(s, DOTILDE | DOPAT);
	else
		s = evalstr(s, DOTILDE);

	return (s);
}

static void
dbteste_error(Test_env *te, int offset, const char *msg)
{
	te->flags |= TEF_ERROR;
	internal_warningf("dbteste_error: %s (offset %d)", msg, offset);
}
