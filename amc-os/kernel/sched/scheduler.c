/* ============================================================
 * AMC OS — O(1) Öncelikli Zamanlayıcı + SMP Yük Dengeleme
 * kernel/sched/scheduler.c
 *   - 140 öncelik bandı (realtime + nice), per-CPU runqueue
 *   - vruntime tabanlı adil süre (CFS benzeri ama bağımsız yazım)
 *   - RT sınıfı: SCHED_FIFO/RR (ses iş parçacıkları için kritik!)
 *   - Yük dengeleme: çekirdekler arası task taşıma + idle pull
 *   - Günlük kullanım hedefi: etkileşim gecikmesi < 5ms garantisi
 * ============================================================ */

#include "scheduler.h"
#include "../kernel.h"

#define MAX_PRIO        140     /* 0..99 realtime, 100..139 normal(nice) */
#define DEFAULT_TIMESLICE_MS 4

struct cfs_runqueue {
    struct rb_root   tasks;      /* vruntime sıralı kırmızı-siyah ağaç */
    uint64_t         nr_running;
    uint64_t         min_vruntime;
    spinlock_t       lock;
};

struct rq {                          /* per-CPU runqueue */
    struct cfs_runqueue cfs;
    struct list_head    rt_queues[MAX_PRIO];
    struct task_struct *current;
    struct task_struct *idle_task;
    uint64_t           clock;
    int                cpu;
} rqs[MAX_CPUS];

/* ================== Init ================== */

void sched_init(uint64_t num_cpus) {
    for (int cpu = 0; cpu < num_cpus; cpu++) {
        rqs[cpu].cpu = cpu;
        RB_ROOT_INIT(&rqs[cpu].cfs.tasks);
        WQ_INIT(&rqs[cpu].cfs.wait);
    }
    register_idle_task();
}

void sched_enable_preemption(void) {
    /* timer tick ve syscall çıkışında preempt_check devreye girer */
    static_branch_enable(&preempt_enabled_key);
}

/* ================== Çalıştırılabilir yapma / uyutma ================== */

void wake_up_process(struct task_struct *p) {
    scoped_rq_lock(p->on_cpu ? cpu_of(p) : select_idle_cpu());
    p->state = TASK_RUNNING;
    if (!p->on_rq) enqueue_task(p);
    resched_curr_if_higher(p);       /* daha yüksek öncelikse preempt et */
}

static void enqueue_task(struct task_struct *p) {
    struct rq *rq = this_rq();
    if (p->policy >= SCHED_FIFO) {                 /* RT bandı */
        list_add_tail(&p->run_list, &rq->rt_queues[p->prio]);
        return;
    }
    /* CFS: vruntime'a göre ağaca yerleştir */
    p->vruntime = calc_vruntime_insert(p);
    rb_insert_cb(&rq->cfs.tasks, p);
    rq->cfs.nr_running++;
}

/* Tick başına: aktif görevin vruntime'ını artır, gerekirse preempt */
void scheduler_tick(int cpu) {
    struct rq *rq = &rqs[cpu];
    struct task_struct *curr = rq->current;
    curr->sum_exec_runtime += TICK_NS;
    curr->vruntime += calc_delta_fair(TICK_NS, curr);

    update_load_avg(rq);              /* PELT benzeri yük ortalaması */
    if (need_resched_check(rq))       /* en küçük vruntime > curr mi? */
        set_tsk_need_resched(curr);
}

/* ================== Seçim (pick_next) ================== */

struct task_struct *pick_next_task(struct rq *rq) {
    /* 1) RT kuyrukları önce (high → low) */
    for (int i = 0; i < 100; i++)
        if (!list_empty(&rq->rt_queues[i]))
            return list_first_entry(&rq->rt_queues[i], struct task_struct, run_list);
    /* 2) CFS: ağacın en soldaki (en az vruntime) düğümü */
    struct rb_node *n = rb_first(&rq->cfs.tasks);
    return n ? container_of(n, struct task_struct, rb_node) : rq->idle_task;
}

/* ================== SMP yük dengeleme ================== */

/* Her 2 tick'te bir: en dolu CPU'dan en boş CPU'ya taşıma */
static void balance_schedule(int this_cpu) {
    struct rq *src = busiest_rq(this_cpu), *dst = least_loaded_rq(this_cpu);
    if (src->cfs.nr_running - dst->cfs.nr_running < 2) return; /* eşik */

    struct task_struct *p = pick_migratable_task(src);
    if (!p || !cpus_available(p->cpus_ptr, dst)) return;

    stop_machine_move(p, src, dst);   /* güvenli geçiş (task_stop makinesi) */
    stats.migrated_tasks++;
}

/* Uyuyan CPU'yu IPI ile uyandır (idle pull) */
void push_tasks_at_idle(int cpu) {
    for_each_online_cpu(other)
        if (rq_weight(other) > rq_weight(cpu) + 1)
            send_ipi(other, IPI_BALANCE);
}

/* ================== Syscall arayüzü ================== */

long sys_sched_setscheduler(pid_t pid, int policy,
                            const struct sched_param_user *param) {
    struct task_struct *p = find_task_by_pid(pid);
    if (!p) return -ESRCH;
    if (!capable(CAP_SYS_NICE) && policy != SCHED_OTHER) return -EPERM;

    int prio = param->sched_priority;
    switch (policy) {
    case SCHED_FIFO: case SCHED_RR:
        if (prio < 1 || prio > 99) return -EINVAL;
        p->policy = policy; p->prio = 99 - prio; p->timeslice = prio; break;
    case SCHED_OTHER:
        p->policy = policy; p->prio = 100 + p->nice; break;
    default: return -EINVAL;
    }
    activate_task_requeue(p);
    return 0;
}

/* Yazılımcıya görünürlük: /proc/schedstat + `amctop` buradan okur */
void sched_get_stats(struct sched_stats *out) {
    out->ctx_switches_total = stats.ctx_switches;
    out->migrations         = stats.migrated_tasks;
    out->avg_wakeup_lat_ns  = stats.wake_lat_sum / MAX(1, stats.wake_cnt);
}
