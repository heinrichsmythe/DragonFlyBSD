/*-
 * Copyright (c) 1986, 1989, 1991, 1993
 *	The Regents of the University of California.  All rights reserved.
 * (c) UNIX System Laboratories, Inc.
 * All or some portions of this file are derived from material licensed
 * to the University of California by American Telephone and Telegraph
 * Co. or Unix System Laboratories, Inc. and are reproduced herein with
 * the permission of UNIX System Laboratories, Inc.
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
 *
 *	@(#)proc.h	8.15 (Berkeley) 5/19/95
 * $FreeBSD: src/sys/sys/proc.h,v 1.99.2.9 2003/06/06 20:21:32 tegge Exp $
 * $DragonFly: src/sys/sys/proc.h,v 1.19 2003/06/30 19:50:32 dillon Exp $
 */

#ifndef _SYS_PROC_H_
#define	_SYS_PROC_H_

#include <sys/callout.h>		/* For struct callout_handle. */
#include <sys/filedesc.h>
#include <sys/queue.h>
#include <sys/rtprio.h>			/* For struct rtprio. */
#include <sys/signal.h>
#ifndef _KERNEL
#include <sys/time.h>			/* For structs itimerval, timeval. */
#endif
#include <sys/ucred.h>
#include <sys/event.h>			/* For struct klist */
#include <sys/thread.h>
#include <machine/proc.h>		/* Machine-dependent proc substruct. */

/*
 * One structure allocated per session.
 */
struct	session {
	int	s_count;		/* Ref cnt; pgrps in session. */
	struct	proc *s_leader;		/* Session leader. */
	struct	vnode *s_ttyvp;		/* Vnode of controlling terminal. */
	struct	tty *s_ttyp;		/* Controlling terminal. */
	pid_t	s_sid;			/* Session ID */
	char	s_login[roundup(MAXLOGNAME, sizeof(long))];	/* Setlogin() name. */
};

/*
 * One structure allocated per process group.
 */
struct	pgrp {
	LIST_ENTRY(pgrp) pg_hash;	/* Hash chain. */
	LIST_HEAD(, proc) pg_members;	/* Pointer to pgrp members. */
	struct	session *pg_session;	/* Pointer to session. */
	struct  sigiolst pg_sigiolst;	/* List of sigio sources. */
	pid_t	pg_id;			/* Pgrp id. */
	int	pg_jobc;	/* # procs qualifying pgrp for job control */
};

struct	procsig {
	sigset_t ps_sigignore;	/* Signals being ignored. */
	sigset_t ps_sigcatch;	/* Signals being caught by user. */
	int      ps_flag;
	struct	 sigacts *ps_sigacts;
	int	 ps_refcnt;
};

#define	PS_NOCLDWAIT	0x0001	/* No zombies if child dies */
#define	PS_NOCLDSTOP	0x0002	/* No SIGCHLD when children stop. */

/*
 * pargs, used to hold a copy of the command line, if it had a sane
 * length
 */
struct	pargs {
	u_int	ar_ref;		/* Reference count */
	u_int	ar_length;	/* Length */
	u_char	ar_args[0];	/* Arguments */
};

/*
 * Description of a process.
 *
 * This structure contains the information needed to manage a thread of
 * control, known in UN*X as a process; it has references to substructures
 * containing descriptions of things that the process uses, but may share
 * with related processes.  The process structure and the substructures
 * are always addressable except for those marked "(PROC ONLY)" below,
 * which might be addressable only on a processor on which the process
 * is running.
 */

struct jail;

struct	proc {
	TAILQ_ENTRY(proc) p_procq;	/* run/sleep queue. */
	LIST_ENTRY(proc) p_list;	/* List of all processes. */

	/* substructures: */
	struct	ucred *p_ucred;		/* Process owner's identity. */
	struct	filedesc *p_fd;		/* Ptr to open files structure. */
	struct filedesc_to_leader *p_fdtol; /* Ptr to tracking node */
	struct	pstats *p_stats;	/* Accounting/statistics (PROC ONLY). */
	struct	plimit *p_limit;	/* Process limits. */
#if 0
	struct	vm_object *p_upages_obj;/* Upages object */
#else
	void		*p_dummy1;
#endif
	struct	procsig *p_procsig;
#define p_sigacts	p_procsig->ps_sigacts
#define p_sigignore	p_procsig->ps_sigignore
#define p_sigcatch	p_procsig->ps_sigcatch
#define	p_rlimit	p_limit->pl_rlimit

	int	p_flag;			/* P_* flags. */
	char	p_stat;			/* S* process status. */
	char	p_pad1[3];

	pid_t	p_pid;			/* Process identifier. */
	LIST_ENTRY(proc) p_hash;	/* Hash chain. */
	LIST_ENTRY(proc) p_pglist;	/* List of processes in pgrp. */
	struct	proc *p_pptr;	 	/* Pointer to parent process. */
	LIST_ENTRY(proc) p_sibling;	/* List of sibling processes. */
	LIST_HEAD(, proc) p_children;	/* Pointer to list of children. */

	struct callout_handle p_ithandle; /*
					      * Callout handle for scheduling
					      * p_realtimer.
					      */
/* The following fields are all zeroed upon creation in fork. */
#define	p_startzero	p_oppid

	pid_t	p_oppid;	 /* Save parent pid during ptrace. XXX */
	int	p_dupfd;	 /* Sideways return value from fdopen. XXX */

	struct	vmspace *p_vmspace;	/* Address space. */

	/* scheduling */
	u_int	p_estcpu;	 /* Time averaged value of p_cpticks. */
	int	p_cpticks;	 /* Ticks of cpu time. */
	fixpt_t	p_pctcpu;	 /* %cpu for this process during p_swtime */
	u_int	p_swtime;	 /* Time swapped in or out. */
	u_int	p_slptime;	 /* Time since last blocked. */

	struct	itimerval p_realtimer;	/* Alarm timer. */

	int	p_traceflag;		/* Kernel trace points. */
	struct	vnode *p_tracep;	/* Trace to vnode. */

	sigset_t p_siglist;		/* Signals arrived but not delivered. */

	struct	vnode *p_textvp;	/* Vnode of executable. */

	char	p_lock;			/* Process lock (prevent swap) count. */
	u_char	p_oncpu;		/* Which cpu we are on */
	u_char	p_lastcpu;		/* Last cpu we were on */
	char	p_rqindex;		/* Run queue index */

	short	p_locks;		/* DEBUG: lockmgr count of held locks */
	short	p_simple_locks;		/* DEBUG: count of held simple locks */
	unsigned int	p_stops;	/* procfs event bitmask */
	unsigned int	p_stype;	/* procfs stop event type */
	char	p_step;			/* procfs stop *once* flag */
	unsigned char	p_pfsflags;	/* procfs flags */
	char	p_pad3[2];		/* padding for alignment */
	register_t p_retval[2];		/* syscall aux returns */
	struct	sigiolst p_sigiolst;	/* list of sigio sources */
	int	p_sigparent;		/* signal to parent on exit */
	sigset_t p_oldsigmask;		/* saved mask from before sigpause */
	int	p_sig;			/* for core dump/debugger XXX */
        u_long	p_code;	  	        /* for core dump/debugger XXX */
	struct	klist p_klist;		/* knotes attached to this process */

/* End area that is zeroed on creation. */
#define	p_endzero	p_startcopy

/* The following fields are all copied upon creation in fork. */
#define	p_startcopy	p_sigmask

	sigset_t p_sigmask;	/* Current signal mask. */
	stack_t	p_sigstk;	/* sp & on stack state variable */
	u_char	p_priority;	/* Tracks user sched queue */
	char	p_nice;		/* Process "nice" value. */
	char	p_comm[MAXCOMLEN+1];

	struct 	pgrp *p_pgrp;	/* Pointer to process group. */

	struct 	sysentvec *p_sysent; /* System call dispatch information. */

	struct	rtprio p_rtprio;	/* Realtime priority. */
	struct	pargs *p_args;
/* End area that is copied on creation. */
#define	p_endcopy	p_addr
	struct	user *p_addr;	/* Kernel virtual addr of u-area (PROC ONLY). */
	struct	mdproc p_md;	/* Any machine-dependent fields. */

	u_short	p_xstat;	/* Exit status for wait; also stop signal. */
	u_short	p_acflag;	/* Accounting flags. */
	struct	rusage *p_ru;	/* Exit information. XXX */

	int	p_nthreads;	/* number of threads (only in leader) */
	void	*p_aioinfo;	/* ASYNC I/O info */
	int	p_wakeup;	/* thread id */
	struct proc *p_peers;	
	struct proc *p_leader;
	void	*p_emuldata;	/* process-specific emulator state data */
	struct thread *p_thread; /* temporarily embed thread struct in proc */
};

#if defined(_KERNEL)
#define p_wchan		p_thread->td_wchan
#define p_wmesg		p_thread->td_wmesg
#define	p_session	p_pgrp->pg_session
#define	p_pgid		p_pgrp->pg_id
#endif

/* Status values. */
#define	SIDL	1		/* Process being created by fork. */
#define	SRUN	2		/* Currently runnable. */
#define	SSLEEP	3		/* Sleeping on an address. */
#define	SSTOP	4		/* Process debugging or suspension. */
#define	SZOMB	5		/* Awaiting collection by parent. */

/* These flags are kept in p_flags. */
#define	P_ADVLOCK	0x00001	/* Process may hold a POSIX advisory lock. */
#define	P_CONTROLT	0x00002	/* Has a controlling terminal. */
#define	P_INMEM		0x00004	/* Loaded into memory. */
#define	P_PPWAIT	0x00010	/* Parent is waiting for child to exec/exit. */
#define	P_PROFIL	0x00020	/* Has started profiling. */
#define P_SELECT	0x00040 /* Selecting; wakeup/waiting danger. */
#define	P_SINTR		0x00080	/* Sleep is interruptible. */
#define	P_SUGID		0x00100	/* Had set id privileges since last exec. */
#define	P_SYSTEM	0x00200	/* System proc: no sigs, stats or swapping. */
#define	P_CURPROC	0x00400	/* 'Current process' on this cpu */
#define	P_TRACED	0x00800	/* Debugged process being traced. */
#define	P_WAITED	0x01000	/* Debugging process has waited for child. */
#define	P_WEXIT		0x02000	/* Working on exiting. */
#define	P_EXEC		0x04000	/* Process called exec. */

/* Should probably be changed into a hold count. */
/* was	P_NOSWAP	0x08000	was: Do not swap upages; p->p_hold */
/* was	P_PHYSIO	0x10000	was: Doing physical I/O; use p->p_hold */

/* Should be moved to machine-dependent areas. */
#define	P_OWEUPC	0x20000	/* Owe process an addupc() call at next ast. */

#define	P_SWAPPING	0x40000	/* Process is being swapped. */
#define	P_SWAPINREQ	0x80000	/* Swapin request due to wakeup */

/* Marked a kernel thread */
#define	P_ONRUNQ	0x100000 /* LWKT scheduled (== not on user sched q) */
#define	P_KTHREADP	0x200000 /* Process is really a kernel thread */
#define P_XSLEEP	0x400000 /* process sitting on xwait_t structure */

#define	P_DEADLKTREAT   0x800000 /* lock aquisition - deadlock treatment */

#define	P_JAILED	0x1000000 /* Process is in jail */
#define	P_OLDMASK	0x2000000 /* need to restore mask before pause */
#define	P_ALTSTACK	0x4000000 /* have alternate signal stack */
#define	P_INEXEC	0x8000000 /* Process is in execve(). */

#ifdef _KERNEL

#ifdef MALLOC_DECLARE
MALLOC_DECLARE(M_SESSION);
MALLOC_DECLARE(M_SUBPROC);
MALLOC_DECLARE(M_ZOMBIE);
MALLOC_DECLARE(M_PARGS);
#endif

/* flags for suser_xxx() */
#define PRISON_ROOT	1

/* Handy macro to determine of p1 can mangle p2 */

#define PRISON_CHECK(cr1, cr2) \
	((!(cr1)->cr_prison) || (cr1)->cr_prison == (cr2)->cr_prison)

/*
 * We use process IDs <= PID_MAX; PID_MAX + 1 must also fit in a pid_t,
 * as it is used to represent "no process group".
 */
#define	PID_MAX		99999
#define	NO_PID		100000

#define SESS_LEADER(p)	((p)->p_session->s_leader == (p))
#define	SESSHOLD(s)	((s)->s_count++)
#define	SESSRELE(s) {							\
	if (--(s)->s_count == 0)					\
		FREE(s, M_SESSION);					\
}

/*
 * STOPEVENT is MP SAFE.
 */
extern void stopevent(struct proc*, unsigned int, unsigned int);
#define	STOPEVENT(p,e,v)			\
	do {					\
		if ((p)->p_stops & (e)) {	\
			get_mplock();		\
			stopevent(p,e,v);	\
			rel_mplock(); 		\
		}				\
	} while (0)

/* hold process U-area in memory, normally for ptrace/procfs work */
#define PHOLD(p) {							\
	if ((p)->p_lock++ == 0 && ((p)->p_flag & P_INMEM) == 0)	\
		faultin(p);						\
}
#define PRELE(p)	(--(p)->p_lock)

#define	PIDHASH(pid)	(&pidhashtbl[(pid) & pidhash])
extern LIST_HEAD(pidhashhead, proc) *pidhashtbl;
extern u_long pidhash;

#define	PGRPHASH(pgid)	(&pgrphashtbl[(pgid) & pgrphash])
extern LIST_HEAD(pgrphashhead, pgrp) *pgrphashtbl;
extern u_long pgrphash;

#if 0 
#ifndef SET_CURPROC
#define SET_CURPROC(p)	(curproc = (p))
#endif
#endif

extern struct proc proc0;		/* Process slot for swapper. */
extern struct thread thread0;		/* Thread slot for swapper. */
extern int hogticks;			/* Limit on kernel cpu hogs. */
extern int nprocs, maxproc;		/* Current and max number of procs. */
extern int maxprocperuid;		/* Max procs per uid. */
extern int sched_quantum;		/* Scheduling quantum in ticks */

LIST_HEAD(proclist, proc);
extern struct proclist allproc;		/* List of all processes. */
extern struct proclist zombproc;	/* List of zombie processes. */
extern struct proc *initproc;		/* Process slot for init */
extern struct thread *pagethread, *updatethread;

#define	NQS	32			/* 32 run queues. */
TAILQ_HEAD(rq, proc);
extern struct rq queues[];
extern struct rq rtqueues[];
extern struct rq idqueues[];
extern int	whichqs;	/* Bit mask summary of non-empty Q's. */
extern int	whichrtqs;	/* Bit mask summary of non-empty Q's. */
extern int	whichidqs;	/* Bit mask summary of non-empty Q's. */

/*
 * XXX macros for scheduler.  Shouldn't be here, but currently needed for
 * bounding the dubious p_estcpu inheritance in wait1().
 * INVERSE_ESTCPU_WEIGHT is only suitable for statclock() frequencies in
 * the range 100-256 Hz (approximately).
 */
#define	ESTCPULIM(e) \
    min((e), INVERSE_ESTCPU_WEIGHT * (NICE_WEIGHT * PRIO_MAX - PPQ) + \
	     INVERSE_ESTCPU_WEIGHT - 1)
#define	INVERSE_ESTCPU_WEIGHT	8	/* 1 / (priorities per estcpu level) */
#define	NICE_WEIGHT	2		/* priorities per nice level */
#define	PPQ		(128 / NQS)	/* priorities per queue */

extern	u_long ps_arg_cache_limit;
extern	int ps_argsopen;

struct proc *pfind __P((pid_t));	/* Find process by id. */
struct pgrp *pgfind __P((pid_t));	/* Find process group by id. */
struct proc *zpfind __P((pid_t));	/* Find zombie process by id. */

struct vm_zone;
extern struct vm_zone *proc_zone;

int	enterpgrp __P((struct proc *p, pid_t pgid, int mksess));
void	fixjobc __P((struct proc *p, struct pgrp *pgrp, int entering));
int	inferior __P((struct proc *p));
int	leavepgrp __P((struct proc *p));
void	mi_switch __P((void));
void	procinit __P((void));
void	relscurproc(struct proc *curp);
int	p_trespass __P((struct ucred *cr1, struct ucred *cr2));
void	resetpriority __P((struct proc *));
int	roundrobin_interval __P((void));
void	schedclock __P((struct proc *));
void	setrunnable __P((struct proc *));
void	clrrunnable __P((struct proc *, int stat));
void	setrunqueue __P((struct proc *));
void	sleepinit __P((void));
int	suser __P((struct thread *td));
int	suser_proc __P((struct proc *p));
int	suser_cred __P((struct ucred *cred, int flag));
void	remrunqueue __P((struct proc *));
void	cpu_heavy_switch __P((struct thread *));
void	cpu_lwkt_switch __P((struct thread *));
void	unsleep __P((struct thread *));

void	cpu_proc_exit __P((void)) __dead2;
void	cpu_thread_exit __P((void)) __dead2;
void	exit1 __P((int)) __dead2;
void	cpu_fork __P((struct proc *, struct proc *, int));
void	cpu_set_fork_handler __P((struct proc *, void (*)(void *), void *));
void	cpu_set_thread_handler(struct thread *td, void (*retfunc)(void), void *func, void *arg);
int	fork1 __P((struct proc *, int, struct proc **));
void	start_forked_proc __P((struct proc *, struct proc *));
int	trace_req __P((struct proc *));
void	cpu_proc_wait __P((struct proc *));
void	cpu_thread_wait __P((struct thread *));
int	cpu_coredump __P((struct thread *, struct vnode *, struct ucred *));
void	setsugid __P((void));
void	faultin __P((struct proc *p));

struct proc *	chooseproc __P((void));
u_int32_t	procrunnable __P((void));

#endif	/* _KERNEL */

#endif	/* !_SYS_PROC_H_ */
