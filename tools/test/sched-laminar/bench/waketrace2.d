/*
 * waketrace2.d — P0.3 round 2: the slow path is BEFORE enqueue (round 1 showed
 * a 5.2s wakelat max with zero >200ms enqueue->on-cpu gaps). Suspect: the
 * callout/softclock path — itself a scheduled thread. Track EVERY thread's
 * enqueue->on-cpu latency; report any >200ms with its name. If "clock"/"intr"
 * threads appear, Laminar is starving interrupt threads at saturation.
 *
 * usage: dtrace -s waketrace2.d -c "./wakelat 16 4 10 1000"
 */
#pragma D option quiet
#pragma D option dynvarsize=64m

sched:::enqueue
{
	enq[args[0]->td_tid] = timestamp;
}

sched:::on-cpu
/enq[curthread->td_tid]/
{
	this->lat_us = (timestamp - enq[curthread->td_tid]) / 1000;
	enq[curthread->td_tid] = 0;
	@maxby[curthread->td_proc->p_comm] = max(this->lat_us);
}

sched:::on-cpu
/this->lat_us > 200000/
{
	printf("SLOW comm=%s tid=%d pri=%d lat_ms=%d cpu=%d\n",
	    stringof(curthread->td_proc->p_comm), curthread->td_tid,
	    curthread->td_priority, this->lat_us / 1000, cpu);
}

END
{
	printf("max enqueue->oncpu (us) by comm:\n");
	printa("  %-16s %@d\n", @maxby);
}
