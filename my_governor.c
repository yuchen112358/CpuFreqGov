
/*
pr_debug("Event %u : Init policy successfully!\n", event);
printk("Event %u : Init policy successfully!\n", event);
注意pr_debug的信息不会通过串口传回到host端，
而printk的信息会通过串口传回到host端。
  */
#define DEBUG
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/cpufreq.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/rwsem.h>
#include <linux/sched.h>
#include <linux/sched/rt.h>
#include <linux/tick.h>
#include <linux/time.h>
#include <linux/timer.h>
#include <linux/workqueue.h>
#include <linux/kthread.h>
#include <linux/slab.h>
#include <linux/kernel_stat.h>
#include <asm/cputime.h>
#include <asm/uaccess.h>
#include <linux/syscalls.h>
#include <linux/android_aid.h>
#include <mach/cpufreq.h>
//proc文件系统
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>

#include "cpu_load_metric.h"

/*
#define CPUFREQ_GOV_POLICY_INIT	4
#define CPUFREQ_GOV_POLICY_EXIT	5
#define CPUFREQ_GOV_START	1
#define CPUFREQ_GOV_STOP	2
#define CPUFREQ_GOV_LIMITS	3
*/

/*
执行结果如下：
<7>[ 5688.499998] [c4] research: Event 4 : Init policy successfully!
<7>[ 5688.501055] [c4] research: Event 1 : Start governor successfully!
<7>[ 5688.501067] [c4] research: Setting to 1400000 kHz because of event 3.
<7>[ 5688.501077] [c4] research: Event 3 : Limits change successfully!
<7>[ 5729.387544] [c5] research: Event 2 : Stop governor successfully!
<7>[ 5729.387560] [c5] research: Event 5 : Policy exits successfully!
*/
/* 当创建一个Per-CPU变量时, 系统中每个处理器获得它自己的这个变量拷贝。
 * DEFINE_PER_CPU(type, name);*/

//sched_setscheduler_nocheck函数未导出到内核，故而使用函数指针定义
typedef typeof(sched_setscheduler_nocheck) *msched_setscheduler_nocheck;
typedef typeof(exynos_boot_cluster) *my_exynos_boot_cluster;

static msched_setscheduler_nocheck  msched;
static my_exynos_boot_cluster my_cluster;

static DEFINE_PER_CPU(struct cpufreq_research_cpuinfo, mcpuinfo);

//proc文件系统
#define LEN 16
static unsigned int load[LEN];
static struct proc_dir_entry *load_dir, *load_entry;

/*
CPU mask机制被用于表示系统中多个处理器的各种组合，正在被重新修改。
修改的原因是CPU masks通常放在堆栈上，但是随着处理器数量的增长将消耗堆栈上大量的空间。
*/
static cpumask_t speedchange_cpumask;
static spinlock_t speedchange_cpumask_lock;
static struct mutex gov_lock;

struct cpufreq_research_cpuinfo {
	struct timer_list cpu_timer; //cpu定时器
	spinlock_t load_lock; /* 保护接下来定义的四个阈值 */
	u64 time_in_idle; //记录系统启动以后运行的idle的总时间
	u64 time_in_idle_timestamp;//记录调用get_cpu_idle_time函数的时间戳
	u64 cputime_speedadj;//记录从定时开始到当前（可认为是定时结束时）的激活时间与当前频率值的乘积值
	u64 cputime_speedadj_timestamp;//记录定时器函数启动或重启时调用get_cpu_idle_time函数的时间戳
	struct cpufreq_policy *policy;//一种调频策略的各种限制条件的组合称之为policy
	struct cpufreq_frequency_table *freq_table;//记录频率表
	unsigned int target_freq;//记录上一次调频的目标频率
	struct rw_semaphore enable_sem;//enable_sem write semaphore to avoid any timer race.
	int governor_enabled;//标志governor是否使能
};

struct cpufreq_research_tunables {
	/*
	 * The minimum amount of time to spend at a frequency before we can ramp
	 * down.
	 */
#define DEFAULT_MIN_SAMPLE_TIME (80 * USEC_PER_MSEC)//80 * 1000L
	unsigned long min_sample_time;
	/*
	 * The sample rate of the timer used to increase frequency
	 */
#define DEFAULT_TIMER_RATE (20 * USEC_PER_MSEC) // 20 * 1000L
	unsigned long timer_rate;
	/*
	控制idle时间是否要加上iowait的时间
	*/
	bool io_is_busy;

#define TASK_NAME_LEN 12
	/* realtime thread handles frequency scaling */
	struct task_struct *speedchange_task;

	/* handle for get cpufreq_policy */
	unsigned int *policy;
};
//%%%%%%%%%%proc文件系统所需定义开始%%%%%%%%%%%
static int load_proc_show(struct seq_file *seq, void *v)
{
	unsigned int *ptr_var = seq->private;
	seq_printf(seq, "%u,%u,%u,%u,%u,%u,%u,%u\n",
	           *ptr_var, *(ptr_var + 1), *(ptr_var + 2), *(ptr_var + 3),
	           *(ptr_var + 4), *(ptr_var + 5), *(ptr_var + 6), *(ptr_var + 7));
	seq_printf(seq, "%u,%u,%u,%u,%u,%u,%u,%u\n",
	           *(ptr_var + 8), *(ptr_var + 9), *(ptr_var + 10), *(ptr_var + 11),
	           *(ptr_var + 12), *(ptr_var + 13), *(ptr_var + 14), *(ptr_var + 15));
	return 0;
}

static ssize_t load_proc_write(struct file *file, const char __user *buffer,
                               size_t count, loff_t *ppos)
{
	struct seq_file *seq = file->private_data;
	unsigned int *ptr_var = seq->private;
	const char *delim = ",";
	char *token, *cur = (char *)buffer;
	int i = 0;//防止ptr_var指针越界
	while ((token = strsep(&cur, delim)) && (i < LEN))
	{
		*ptr_var = simple_strtoul(token, NULL, 10);
		ptr_var++;
		i ++ ;
	}

	return count;
}

static int load_proc_open(struct inode *inode, struct file *file)
{
	 /* 
	 在open时，使用PDE_DATA(inode)作为私有数据向下传。
	 其实PDE_DATA(inode) 就是phydev。这个私有数据的保存在
	 seq_file的private里。在write和show函数中可以直接使用seq->private
	 来找到私有数据。
	  */
	return single_open(file, load_proc_show, PDE_DATA(inode));
}

static const struct file_operations load_proc_fops =
{
	.owner = THIS_MODULE,
	.open = load_proc_open,
	.read = seq_read,
	.write = load_proc_write,
	.llseek = seq_lseek,
	.release = single_release,
};

//%%%%%%%%%%proc文件系统所需定义结束%%%%%%%%%%%
static inline cputime64_t get_cpu_idle_time_jiffy(unsigned int cpu,
						  cputime64_t *wall)
{
	u64 idle_time;
	u64 cur_wall_time;
	u64 busy_time;

	cur_wall_time = jiffies64_to_cputime64(get_jiffies_64());

	busy_time  = kcpustat_cpu(cpu).cpustat[CPUTIME_USER];
	busy_time += kcpustat_cpu(cpu).cpustat[CPUTIME_SYSTEM];
	busy_time += kcpustat_cpu(cpu).cpustat[CPUTIME_IRQ];
	busy_time += kcpustat_cpu(cpu).cpustat[CPUTIME_SOFTIRQ];
	busy_time += kcpustat_cpu(cpu).cpustat[CPUTIME_STEAL];
	busy_time += kcpustat_cpu(cpu).cpustat[CPUTIME_NICE];

	idle_time = cur_wall_time - busy_time;
	if (wall)
		*wall = jiffies_to_usecs(cur_wall_time);

	return jiffies_to_usecs(idle_time);
}

static inline cputime64_t get_cpu_idle_time(
	unsigned int cpu,
	cputime64_t *wall,
	bool io_is_busy)
{
	//get_cpu_idle_time_us - get the total idle time of a cpu
	u64 idle_time = get_cpu_idle_time_us(cpu, wall);

	if (idle_time == -1ULL)
		idle_time = get_cpu_idle_time_jiffy(cpu, wall);
	else if (!io_is_busy)//get_cpu_iowait_time_us - get the total iowait time of a cpu
		idle_time += get_cpu_iowait_time_us(cpu, wall);

	return idle_time;
}

//更新负载
static u64 update_load(int cpu)
{
	struct cpufreq_research_cpuinfo *pcpu = &per_cpu(mcpuinfo, cpu);
	struct cpufreq_research_tunables *tunables =
		pcpu->policy->governor_data;
	u64 now;
	u64 now_idle;
	unsigned int delta_idle;
	unsigned int delta_time;
	u64 active_time;

	now_idle = get_cpu_idle_time(cpu, &now, tunables->io_is_busy);
	delta_idle = (unsigned int)(now_idle - pcpu->time_in_idle);
	delta_time = (unsigned int)(now - pcpu->time_in_idle_timestamp);

	if (delta_time <= delta_idle)
		active_time = 0;
	else
		active_time = delta_time - delta_idle;

	pcpu->cputime_speedadj += active_time * pcpu->policy->cur;

	update_cpu_metric(cpu, now, delta_idle, delta_time, pcpu->policy);

	pcpu->time_in_idle = now_idle;
	pcpu->time_in_idle_timestamp = now;
	return now;
}

//选频策略
static unsigned int choose_freq(struct cpufreq_research_cpuinfo *pcpu,
		int cpu_load)
{
	unsigned int freq = pcpu->policy->cur;
	if(freq == (pcpu->policy)->max){
		freq = (pcpu->policy)->min;
	}else {
		freq = (pcpu->policy)->max;
	}
	return freq;
}
//重新调度定时器
static void cpufreq_research_timer_resched(
    struct cpufreq_research_cpuinfo *pcpu)
{
	struct cpufreq_research_tunables *tunables =
		    pcpu->policy->governor_data;
	unsigned long expires;
	unsigned long flags;

	if (!tunables->speedchange_task)
		return;

	spin_lock_irqsave(&pcpu->load_lock, flags);
	pcpu->time_in_idle =
	    get_cpu_idle_time(smp_processor_id(),
	                      &pcpu->time_in_idle_timestamp,
	                      tunables->io_is_busy);
	pcpu->cputime_speedadj = 0;
	pcpu->cputime_speedadj_timestamp = pcpu->time_in_idle_timestamp;
	expires = jiffies + usecs_to_jiffies(tunables->timer_rate);
	mod_timer_pinned(&pcpu->cpu_timer, expires);

	spin_unlock_irqrestore(&pcpu->load_lock, flags);
}

//定时器执行函数，即定时器索要做的任务
static void cpufreq_research_timer(unsigned long data)
{
	u64 now;
	unsigned int delta_time;
	u64 cputime_speedadj;
	int cpu_load;
	unsigned int normal_cpu_load;//不乘扩放因子的负载
	unsigned int normal_freq;
	struct cpufreq_research_cpuinfo *pcpu =
		&per_cpu(mcpuinfo, data);
	struct cpufreq_research_tunables *tunables =
		pcpu->policy->governor_data;
	unsigned int new_freq;
	unsigned int loadadjfreq;
	unsigned int index;
	unsigned long flags;
	// pr_debug("------ Do timer work begin : ------\n");
	if (!down_read_trylock(&pcpu->enable_sem))
		return;
	if (!pcpu->governor_enabled)
		goto exit;

	spin_lock_irqsave(&pcpu->load_lock, flags);
	//下面计算并更新负载
	now = update_load(data);
	delta_time = (unsigned int)(now - pcpu->cputime_speedadj_timestamp);
	cputime_speedadj = pcpu->cputime_speedadj;
	spin_unlock_irqrestore(&pcpu->load_lock, flags);

	if (WARN_ON_ONCE(!delta_time))
		goto rearm;

	do_div(cputime_speedadj, delta_time);
	loadadjfreq = (unsigned int)cputime_speedadj * 100;
	cpu_load = loadadjfreq / pcpu->target_freq;
	// pr_debug("interactive cpu_load: cpu: %lu freq: %u load: %d\n", data, (pcpu->policy)->cur,cpu_load );
	//根据负载获取新的频率，更具自己的策略修改之
	one_cpu_load_metric_get(&normal_cpu_load, &normal_freq,data);
	// pr_debug("normal cpu_load: cpu: %lu freq: %u load: %u\n", data, normal_freq, normal_cpu_load);
	//proc文件系统
	load[data] = normal_cpu_load;
	load[data+8] =  normal_freq;

	new_freq = choose_freq(pcpu, normal_cpu_load);

	if (cpufreq_frequency_table_target(pcpu->policy, pcpu->freq_table,
					   new_freq, CPUFREQ_RELATION_L,
					   &index))
		goto rearm;

	new_freq = pcpu->freq_table[index].frequency;

	if (pcpu->policy->cur == new_freq) {
		goto rearm;
	}

	pcpu->target_freq = new_freq;
	spin_lock_irqsave(&speedchange_cpumask_lock, flags);
	  /*将cpu data 在speedchange_cpumask 中置位*/
	cpumask_set_cpu(data, &speedchange_cpumask);
	spin_unlock_irqrestore(&speedchange_cpumask_lock, flags);
	wake_up_process(tunables->speedchange_task);
rearm:
	if (!timer_pending(&pcpu->cpu_timer))
		cpufreq_research_timer_resched(pcpu);
exit:
	up_read(&pcpu->enable_sem);
	return;
}
/* The caller shall take enable_sem write semaphore to avoid any timer race.
 * The cpu_timer must be deactivated when calling this function.
 */
static void cpufreq_research_timer_start(
    struct cpufreq_research_tunables *tunables, int cpu)
{
	struct cpufreq_research_cpuinfo *pcpu = &per_cpu(mcpuinfo, cpu);
	// pr_debug("address of tunables is %p\n",tunables);
	/*
	 在文件<linux/jiffies.h>中：extern unsigned long volatile jiffies;
	 jiffies是内核中的一个全局变量，用来记录自系统启动一来产生的节拍数。
	 换言之，jiffies是记录着从电脑开机到现在总共的时钟中断次数。
	 在 Linux 2.6 中，系统时钟每 1 毫秒中断一次（时钟频率，用 HZ 宏表示，定义为 1000，即每秒中断 1000 次，
	 2.4 中定义为 100，很多应用程序也仍然沿用 100 的时钟频率），这个时间单位称为一个 jiffie。
	 */
	unsigned long expires = jiffies +
	                        usecs_to_jiffies(tunables->timer_rate);
	unsigned long flags;

	if (!tunables->speedchange_task)
		return;

	pcpu->cpu_timer.expires = expires;
	//start a timer on a particular CPU” ，即在指定的CPU上start一个定时器
	add_timer_on(&pcpu->cpu_timer, cpu);

	spin_lock_irqsave(&pcpu->load_lock, flags);
	pcpu->time_in_idle =
	    get_cpu_idle_time(cpu, &pcpu->time_in_idle_timestamp,
	                      tunables->io_is_busy);
	/* 
	示例输出：
	 cpu idle time = 2631782481.
	 cpu idle time + iowait time= 5264820859.
	*/
	// pr_debug("cpu idle time = %llu.\n", pcpu->time_in_idle);
	// pr_debug("cpu idle time + iowait time= %llu.\n", pcpu->time_in_idle +
	//  get_cpu_idle_time(cpu, &pcpu->time_in_idle_timestamp,
	//                       0));
	pcpu->cputime_speedadj = 0;
	pcpu->cputime_speedadj_timestamp = pcpu->time_in_idle_timestamp;
	spin_unlock_irqrestore(&pcpu->load_lock, flags);
	// pr_debug("------ Timer started! ------\n");
}

static int cpufreq_research_speedchange_task(void *data)
{
	unsigned int cpu;
	cpumask_t tmp_mask;
	unsigned long flags;
	struct cpufreq_research_cpuinfo *pcpu;
	// pr_debug("Enter in speedchange task.\n");
	while (!kthread_should_stop()) {
		set_current_state(TASK_INTERRUPTIBLE);
		spin_lock_irqsave(&speedchange_cpumask_lock, flags);

		if (cpumask_empty(&speedchange_cpumask)) {//掩码speedchange_cpumask中是否还有更多的CPU
			spin_unlock_irqrestore(&speedchange_cpumask_lock,
					       flags);
			schedule();

			if (kthread_should_stop())
				break;

			spin_lock_irqsave(&speedchange_cpumask_lock, flags);
		}

		set_current_state(TASK_RUNNING);
		tmp_mask = speedchange_cpumask;
		cpumask_clear(&speedchange_cpumask);
		spin_unlock_irqrestore(&speedchange_cpumask_lock, flags);

		for_each_cpu(cpu, &tmp_mask) {
			unsigned int j;
			unsigned int max_freq = 0;
			unsigned int smp_id = smp_processor_id();

			if (*my_cluster == CA7) {
				if ((smp_id == 0 && cpu >= NR_CA7) ||
					(smp_id == NR_CA7 && cpu < NR_CA7))
					continue;
			} else {
				if ((smp_id == 0 && cpu >= NR_CA15) ||
					(smp_id == NR_CA15 && cpu < NR_CA15))
					continue;
			}

			pcpu = &per_cpu(mcpuinfo, cpu);

			if (!down_read_trylock(&pcpu->enable_sem))
				continue;
			if (!pcpu->governor_enabled) {
				up_read(&pcpu->enable_sem);
				continue;
			}

			for_each_cpu(j, pcpu->policy->cpus) {
				struct cpufreq_research_cpuinfo *pjcpu =
					&per_cpu(mcpuinfo, j);

				if (pjcpu->target_freq > max_freq)
					max_freq = pjcpu->target_freq;
			}

			if (max_freq != pcpu->policy->cur)
				__cpufreq_driver_target(pcpu->policy,
							max_freq,
							CPUFREQ_RELATION_H);

			up_read(&pcpu->enable_sem);
		}
	}

	return 0;
}

static int cpufreq_governor_research(struct cpufreq_policy *policy,
                                     unsigned int event)
{
	unsigned int j;
	struct cpufreq_research_cpuinfo *pcpu = NULL;
	struct cpufreq_frequency_table *freq_table = NULL;
	struct cpufreq_research_tunables *tunables = NULL;
	struct sched_param param = { .sched_priority = MAX_RT_PRIO - 1 };
	char speedchange_task_name[TASK_NAME_LEN];
	//cpufreq_governor_research函数是回调函数，每个事件均调用一次该函数，
	//每次调用,若没有下面这句，则tunables的初始值都为NULL。
	tunables = policy->governor_data;//没有此句，则tunables为NULL
	switch (event) {
	case CPUFREQ_GOV_POLICY_INIT:
		tunables = kzalloc(sizeof(*tunables), GFP_KERNEL);
		if (!tunables) {
			pr_err("%s: POLICY_INIT: kzalloc failed\n", __func__);
			return -ENOMEM;
		}
		tunables->min_sample_time = DEFAULT_MIN_SAMPLE_TIME;
		tunables->timer_rate = DEFAULT_TIMER_RATE;
		tunables->io_is_busy = 1;
		/*
		 该变量(&policy->policy)可以取以下两个值：CPUFREQ_POLICY_POWERSAVE和CPUFREQ_POLICY_PERFORMANCE，
		 该变量只有当调频驱动支持setpolicy回调函数的时候有效，这时候由驱动根据policy变量的值来决定
		 系统的工作频率或状态。
		*/
		tunables->policy = &policy->policy;
		policy->governor_data = tunables;
		// pr_debug("init:address of tunables is %p\n",tunables);
		pr_debug("Event %u : Init policy successfully!\n", event);
		break;
	case CPUFREQ_GOV_POLICY_EXIT:
		if (!tunables) {
			kfree(tunables);
		}
		policy->governor_data = NULL;
		pr_debug("Event %u : Policy exits successfully!\n", event);
		break;
	case CPUFREQ_GOV_START:
		mutex_lock(&gov_lock);

		freq_table = cpufreq_frequency_get_table(policy->cpu);

		for_each_cpu(j, policy->cpus) {
			pcpu = &per_cpu(mcpuinfo, j);
			pcpu->policy = policy;
			pcpu->target_freq = policy->cur;
			pcpu->freq_table = freq_table;
			down_write(&pcpu->enable_sem);
			del_timer_sync(&pcpu->cpu_timer);
			// pr_debug("start:address of tunables is %p\n",tunables);
			cpufreq_research_timer_start(tunables, j);
			pcpu->governor_enabled = 1;
			up_write(&pcpu->enable_sem);
			// pr_debug("------ CPUFREQ_GOV_START: ------\n");
			// pr_debug("In cpu %u.\n", j);
		}
		/*
		snprintf()函数用于将格式化的数据写入字符串，其原型为：
		int snprintf(char *str, int n, char * format [, argument, ...]);
		GCC中的参数n表示向str中写入n个字符，包括'\0'字符，并且返回实际的字符串长度。
		*/
		snprintf(speedchange_task_name, TASK_NAME_LEN, "cfresearch%d\n",
		         policy->cpu);
		/*
		struct task_struct *kthread_create(int (*threadfn)(void *data),void *data,const char *namefmt, ...);
		线程创建后，不会马上运行，而是需要将kthread_create() 返回的task_struct指针传给wake_up_process()，
		然后通过此函数运行线程。
		*/
		tunables->speedchange_task =
		    kthread_create(cpufreq_research_speedchange_task, NULL,
		                   speedchange_task_name);
		if (IS_ERR(tunables->speedchange_task)) {
			mutex_unlock(&gov_lock);
			return PTR_ERR(tunables->speedchange_task);
		}

		msched(tunables->speedchange_task, SCHED_FIFO, &param);
		get_task_struct(tunables->speedchange_task);//增加speedchange_task进程的引用计数
		//bind a just-created kthread to a cpu.
		kthread_bind(tunables->speedchange_task, policy->cpu);

		/* NB: wake up so the thread does not look hung to the freezer */
		wake_up_process(tunables->speedchange_task);

		mutex_unlock(&gov_lock);

		pr_debug("Event %u : Start governor successfully!\n", event);
		break;
	case CPUFREQ_GOV_STOP:
		mutex_lock(&gov_lock);
		for_each_cpu(j, policy->cpus) {
			pcpu = &per_cpu(mcpuinfo, j);
			down_write(&pcpu->enable_sem);
			pcpu->governor_enabled = 0;
			del_timer_sync(&pcpu->cpu_timer);
			up_write(&pcpu->enable_sem);
			// pr_debug("------ CPUFREQ_GOV_STOP: ------\n");
			// pr_debug("In cpu %u .\n", j);
		}

		kthread_stop(tunables->speedchange_task);
		put_task_struct(tunables->speedchange_task);//减少speedchange_task进程的引用计数
		tunables->speedchange_task = NULL;
		mutex_unlock(&gov_lock);
		pr_debug("Event %u : Stop governor successfully!\n", event);
		break;
	case CPUFREQ_GOV_LIMITS:
		if (policy->max < policy->cur)
			__cpufreq_driver_target(policy,
			                        policy->max, CPUFREQ_RELATION_H);
		else if (policy->min > policy->cur)
			__cpufreq_driver_target(policy,
			                        policy->min, CPUFREQ_RELATION_L);
		for_each_cpu(j, policy->cpus) {
			pcpu = &per_cpu(mcpuinfo, j);

			/* hold write semaphore to avoid race */
			down_write(&pcpu->enable_sem);
			if (pcpu->governor_enabled == 0) {
				up_write(&pcpu->enable_sem);
				continue;
			}

			/* update target_freq firstly */
			if (policy->max < pcpu->target_freq)
				pcpu->target_freq = policy->max;
			else if (policy->min > pcpu->target_freq)
				pcpu->target_freq = policy->min;

			/* Reschedule timer.
			 * Delete the timer, else the timer callback may
			 * return without re-arm the timer when failed
			 * acquire the semaphore. This race may cause timer
			 * stopped unexpectedly.
			 */
			del_timer_sync(&pcpu->cpu_timer);
			cpufreq_research_timer_start(tunables, j);
			up_write(&pcpu->enable_sem);
			// pr_debug("------ CPUFREQ_GOV_LIMITS: ------\n");
			// pr_debug("In cpu %u .\n", j);
		}
		pr_debug("Event %u : Limits change successfully!\n", event);
		break;
	default:
		break;
	}
	return 0;
}

static struct cpufreq_governor cpufreq_gov_research = {
	.name		= "research",//不要超过16个字母
	.governor	= cpufreq_governor_research,
	.max_transition_latency = 10000000,//10000000 nano secs = 10 ms
	.owner		= THIS_MODULE,
};

static int __init cpufreq_gov_research_init(void)
{
	unsigned int i;
	struct cpufreq_research_cpuinfo *pcpu;
	printk(KERN_INFO "------ Init successfully! ------\n");
	msched =  (msched_setscheduler_nocheck)kallsyms_lookup_name("sched_setscheduler_nocheck");
	my_cluster = (my_exynos_boot_cluster)kallsyms_lookup_name("exynos_boot_cluster");
	/* Initalize per-cpu timers */
	for_each_possible_cpu(i) {
		pcpu = &per_cpu(mcpuinfo, i);
		init_timer_deferrable(&pcpu->cpu_timer);
		pcpu->cpu_timer.function = cpufreq_research_timer;
		pcpu->cpu_timer.data = i;
		spin_lock_init(&pcpu->load_lock);
		init_rwsem(&pcpu->enable_sem);
	}

	spin_lock_init(&speedchange_cpumask_lock);
	mutex_init(&gov_lock);
	//proc所需
	load_dir = proc_mkdir("cpu_load_dir", NULL);
	if (load_dir)
	{
		load_entry = proc_create_data("cpu_load", 0666, load_dir, &load_proc_fops, load);
		if (load_entry)
			return cpufreq_register_governor(&cpufreq_gov_research);
	}
	return -ENOMEM;	
}


static void __exit cpufreq_gov_research_exit(void)
{
	printk(KERN_INFO "------ Cleanup successfully! ------\n");
	//proc所需
	remove_proc_entry("cpu_load", load_dir);
	remove_proc_entry("cpu_load_dir", NULL);

	cpufreq_unregister_governor(&cpufreq_gov_research);
}


module_init(cpufreq_gov_research_init);
module_exit(cpufreq_gov_research_exit);

MODULE_AUTHOR("Wang Zhen <wzzju@mail.ustc.edu.cn>");
MODULE_DESCRIPTION("'cpufreq_research_governor' - A cpufreq governor for "
                   "Research");
MODULE_LICENSE("GPL");
