/*
 * waketrace.d — discriminate the P0.3 bimodal wake-max hypotheses.
 *
 * For every wakelat thread: measure enqueue→on-cpu latency. For slow events
 * (>200ms) print where it was enqueued vs where it finally ran (migration =
 * steal story) . Also count idle-thread on-cpu per CPU: with 16 spinners the
 * machine should never idle — substantial idle while a waker waits seconds
 * means a runnable thread was invisible to the picker (stale shard-min).
 *
 * usage: dtrace -s waketrace.d -c "./wakelat 16 4 10 1000"
 */
#pragma D option quiet
#pragma D option dynvarsize=16m

sched:::enqueue
/args[1]->p_comm == "wakelat"/
{
	enq[args[0]->td_tid] = timestamp;
	enqcpu[args[0]->td_tid] = cpu;
}

sched:::on-cpu
/enq[curthread->td_tid]/
{
	this->lat_us = (timestamp - enq[curthread->td_tid]) / 1000;
	this->ecpu = enqcpu[curthread->td_tid];
	enq[curthread->td_tid] = 0;
	@dist = quantize(this->lat_us);
	@maxlat = max(this->lat_us);
}

sched:::on-cpu
/this->lat_us > 200000/
{
	printf("SLOW tid=%d lat_ms=%d enqcpu=%d runcpu=%d\n",
	    curthread->td_tid, this->lat_us / 1000, this->ecpu, cpu);
}

sched:::on-cpu
/curthread->td_proc->p_comm == "idle"/
{
	@idle[cpu] = count();
}

END
{
	printf("\nmax enqueue->oncpu latency (us): ");
	printa("%@d\n", @maxlat);
	printf("idle on-cpu counts per cpu (should be ~0 at spin=16):\n");
	printa("  cpu %d: %@d\n", @idle);
	printf("latency distribution (us):\n");
	printa(@dist);
}
