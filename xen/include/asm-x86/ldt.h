/* -*-  Mode:C; c-basic-offset:4; tab-width:4; indent-tabs-mode:nil -*- */

#ifndef __ARCH_LDT_H
#define __ARCH_LDT_H

#ifndef __ASSEMBLY__

static inline void load_LDT(struct exec_domain *ed)
{
    unsigned int cpu;
    struct desc_struct *desc;
    unsigned long ents;

    if ( (ents = ed->arch.ldt_ents) == 0 )
    {
        __asm__ __volatile__ ( "lldt %%ax" : : "a" (0) );
    }
    else
    {
        cpu = smp_processor_id();
        desc = (struct desc_struct *)GET_GDT_ADDRESS(ed) + __LDT(cpu);
        desc->a = ((LDT_VIRT_START(ed)&0xffff)<<16) | (ents*8-1);
        desc->b = (LDT_VIRT_START(ed)&(0xff<<24)) | 0x8200 |
            ((LDT_VIRT_START(ed)&0xff0000)>>16);
        __asm__ __volatile__ ( "lldt %%ax" : : "a" (__LDT(cpu)<<3) );
    }
}

#endif /* !__ASSEMBLY__ */

#endif
