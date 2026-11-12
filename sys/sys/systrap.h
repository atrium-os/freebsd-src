/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Atrium Authors
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifndef _SYS_SYSTRAP_H_
#define	_SYS_SYSTRAP_H_

/*
 * Syscall traps: a process names address ranges of its own, and a system
 * call instruction executed from inside one does not run.  The thread gets
 * a synchronous SIGSYS instead, with the registers as they were at the
 * instruction, and its handler decides what the call returns (as Linux's
 * syscall user dispatch does).  A foreign-library host uses it for a
 * library that makes system calls of another ABI directly.
 *
 * The policy is in a module (hospes_trap.ko).  The kernel's part is this
 * hook, the p_systrap pointer it owns, and one predicted branch on that
 * pointer in syscallenter().
 */

/* si_code of the SIGSYS a trapped call raises. */
#define	SYS_TRAPPED	1

#ifdef _KERNEL
struct thread;

/*
 * Called for a thread whose process has p_systrap set, before the call's
 * arguments are fetched.  Returns true if the call was trapped: the
 * module has queued the SIGSYS, and the kernel returns to user space
 * leaving every register as it was.
 */
typedef bool systrap_hook_t(struct thread *td);
extern systrap_hook_t *systrap_hook;
#endif

#endif /* !_SYS_SYSTRAP_H_ */
