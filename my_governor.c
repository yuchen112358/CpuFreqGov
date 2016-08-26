
#define DEBUG

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/cpufreq.h>
#include <linux/init.h>

/*
#define CPUFREQ_GOV_POLICY_INIT	4
#define CPUFREQ_GOV_POLICY_EXIT	5
#define CPUFREQ_GOV_START	1
#define CPUFREQ_GOV_STOP	2
#define CPUFREQ_GOV_LIMITS	3
*/
/*
执行结果如下：
<7>[ 7010.445314] [c5] study: Event 4 : Init policy successfully!
<7>[ 7010.446406] [c5] study: Event 1 : Start governor successfully!
<7>[ 7010.446420] [c5] study: Setting to 1400000 kHz because of event 3
<7>[ 7010.446431] [c5] study: Event 3 : Limits change successfully!

<7>[ 7172.299238] [c7] study: Event 2 : Stop governor successfully!
<7>[ 7172.299255] [c7] study: Event 5 : Policy exits successfully!
*/

static int cpufreq_governor_my(struct cpufreq_policy *policy,
                               unsigned int event)
{
	switch (event) {
	case CPUFREQ_GOV_POLICY_INIT:
		pr_debug("Event %u : Init policy successfully!\n", event);
		break;
	case CPUFREQ_GOV_POLICY_EXIT:
		pr_debug("Event %u : Policy exits successfully!\n", event);
		break;
	case CPUFREQ_GOV_START:
		__cpufreq_driver_target(policy, policy->max,
		                        CPUFREQ_RELATION_H);
		pr_debug("Event %u : Start governor successfully!\n", event);
		break;
	case CPUFREQ_GOV_STOP:
		pr_debug("Event %u : Stop governor successfully!\n", event);
		break;
	case CPUFREQ_GOV_LIMITS:
		pr_debug("Setting to %u kHz because of event %u .\n",
		         policy->max, event);
		__cpufreq_driver_target(policy, policy->max,
		                        CPUFREQ_RELATION_H);
		pr_debug("Event %u : Limits change successfully!\n", event);
		break;
	default:
		break;
	}
	return 0;
}

static struct cpufreq_governor cpufreq_gov_my = {
	.name		= "my_governor",
	.governor	= cpufreq_governor_my,
	.owner		= THIS_MODULE,
};


static int __init cpufreq_gov_my_init(void)
{
	return cpufreq_register_governor(&cpufreq_gov_my);
}


static void __exit cpufreq_gov_my_exit(void)
{
	cpufreq_unregister_governor(&cpufreq_gov_my);
}


module_init(cpufreq_gov_my_init);
module_exit(cpufreq_gov_my_exit);

MODULE_AUTHOR("Wang Zhen <wzzju@mail.ustc.edu.cn>");
MODULE_DESCRIPTION("'cpufreq_mygovernor' - A cpufreq governor for "
                   "Research");
MODULE_LICENSE("GPL");
