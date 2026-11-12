#include <list.h>
#include <sync.h>
#include <proc.h>
#include <sched.h>
#include <assert.h>

void
wakeup_proc(struct proc_struct *proc) {
    assert(proc->state != PROC_ZOMBIE && proc->state != PROC_RUNNABLE);
    proc->state = PROC_RUNNABLE;
}

//LAB4 function3 2411292
void schedule(void)
{
    bool intr_flag;
    local_intr_save(intr_flag);
    current->need_resched = 0;
    // idle 不在链表中；普通线程从后继开始，绕回自身时也允许再次选中。
    list_entry_t *start = current == idleproc
                          ? &proc_list : &current->list_link;
    list_entry_t *le = start;
    struct proc_struct *next = idleproc;
    do {
        le = list_next(le);
        // 头结点不是 PCB；空表时一步回到 start，保留 idle 作为目标。
        if (le != &proc_list) {
            struct proc_struct *proc = le2proc(le, list_link);
            if (proc->state == PROC_RUNNABLE) {
                next = proc;
                break;
            }
        }
    } while (le != start); // 最多一轮，只选择 PROC_RUNNABLE 线程。
    next->runs++;
    proc_run(next);
    // 切回本线程、从 proc_run 返回后，才恢复本次调度前的中断状态。
    local_intr_restore(intr_flag);
}
