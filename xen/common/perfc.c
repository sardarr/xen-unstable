
#include <xen/lib.h>
#include <xen/smp.h>
#include <xen/time.h>
#include <xen/perfc.h>
#include <xen/keyhandler.h> 

#undef  PERFCOUNTER
#undef  PERFCOUNTER_CPU
#undef  PERFCOUNTER_ARRAY
#define PERFCOUNTER( var, name )              { name, TYPE_SINGLE, 0 },
#define PERFCOUNTER_CPU( var, name )          { name, TYPE_CPU,    0 },
#define PERFCOUNTER_ARRAY( var, name, size )  { name, TYPE_ARRAY,  size },
static struct {
    char *name;
    enum { TYPE_SINGLE, TYPE_CPU, TYPE_ARRAY } type;
    int nr_elements;
} perfc_info[] = {
#include <xen/perfc_defn.h>
};

#define NR_PERFCTRS (sizeof(perfc_info) / sizeof(perfc_info[0]))

struct perfcounter_t perfcounters;

void perfc_printall(u_char key, void *dev_id, struct pt_regs *regs)
{
    int i, j, sum;
    s_time_t now = NOW();
    atomic_t *counters = (atomic_t *)&perfcounters;

    printk("Xen performance counters SHOW  (now = 0x%08X:%08X)\n",
           (u32)(now>>32), (u32)now);

    for ( i = 0; i < NR_PERFCTRS; i++ ) 
    {
        printk("%-32s  ",  perfc_info[i].name);
        switch ( perfc_info[i].type )
        {
        case TYPE_SINGLE:
            printk("TOTAL[%10d]", atomic_read(&counters[0]));
            counters += 1;
            break;
        case TYPE_CPU:
            for ( j = sum = 0; j < smp_num_cpus; j++ )
                sum += atomic_read(&counters[j]);
            printk("TOTAL[%10d]  ", sum);
            for ( j = 0; j < smp_num_cpus; j++ )
                printk("CPU%02d[%10d]  ", j, atomic_read(&counters[j]));
            counters += NR_CPUS;
            break;
        case TYPE_ARRAY:
            for ( j = sum = 0; j < perfc_info[i].nr_elements; j++ )
                sum += atomic_read(&counters[j]);
            printk("TOTAL[%10d]  ", sum);
            for ( j = 0; j < perfc_info[i].nr_elements; j++ )
                printk("ARR%02d[%10d]  ", j, atomic_read(&counters[j]));
            counters += j;
            break;
        }
        printk("\n");
    }
}

void perfc_reset(u_char key, void *dev_id, struct pt_regs *regs)
{
    s_time_t now = NOW();
    printk("Xen performance counters RESET (now = 0x%08X:%08X)\n",
           (u32)(now>>32), (u32)now);
    memset(&perfcounters, 0, sizeof(perfcounters));
}

