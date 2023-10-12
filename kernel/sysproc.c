#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"

extern void backtrace(void);

uint64
sys_exit(void)
{
  int n;
  argint(0, &n);
  exit(n);
  return 0;  // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return fork();
}

uint64
sys_wait(void)
{
  uint64 p;
  argaddr(0, &p);
  return wait(p);
}

uint64
sys_sbrk(void)
{
  uint64 addr;
  int n;

  argint(0, &n);
  addr = myproc()->sz;
  if(growproc(n) < 0)
    return -1;
  return addr;
}

uint64
sys_sleep(void)
{
  int n;
  uint ticks0;
  backtrace();
  argint(0, &n);
  if(n < 0)
    n = 0;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(killed(myproc())){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

uint64
sys_kill(void)
{
  int pid;

  argint(0, &pid);
  return kill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

uint64
sys_sigalarm(void)
{
  struct proc *p = myproc();
  int ticks;
  uint64 handler;
  argint(0, &ticks);
  argaddr(1, &handler);
  p->sig_alarm_frame.interval = ticks;
  p->sig_alarm_frame.ticks = 0;
  p->sig_alarm_frame.handler = handler;
  return 0;
}

uint64
sys_sigreturn(void)
{
  struct proc *p = myproc();
  
  // restore callee saved registers
  p->trapframe->ra = p->sig_alarm_frame.ra;
  p->trapframe->t0 = p->sig_alarm_frame.t0;
  p->trapframe->t1 = p->sig_alarm_frame.t1;
  p->trapframe->t2 = p->sig_alarm_frame.t2;
  p->trapframe->t3 = p->sig_alarm_frame.t3;
  p->trapframe->t4 = p->sig_alarm_frame.t4;
  p->trapframe->t5 = p->sig_alarm_frame.t5;
  p->trapframe->t6 = p->sig_alarm_frame.t6;
  // p->trapframe->a0 = p->sig_alarm_frame.a0;
  p->trapframe->a1 = p->sig_alarm_frame.a1;
  p->trapframe->a2 = p->sig_alarm_frame.a2;
  p->trapframe->a3 = p->sig_alarm_frame.a3;
  p->trapframe->a4 = p->sig_alarm_frame.a4;
  p->trapframe->a5 = p->sig_alarm_frame.a5;
  p->trapframe->a6 = p->sig_alarm_frame.a6;
  p->trapframe->a7 = p->sig_alarm_frame.a7;

  // restore frame pointer and stack pointer
  // since the code does not reach ret
  // they will not be restored in handler
  p->trapframe->sp = p->sig_alarm_frame.sp;
  p->trapframe->s0 = p->sig_alarm_frame.s0;

  // out of handler;
  p->sig_alarm_frame.in_handler = 0;

  // restore epc
  p->trapframe->epc = p->sig_alarm_frame.epc;

  return p->sig_alarm_frame.a0; // syscall will set the return val to a0
}