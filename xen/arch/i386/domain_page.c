/******************************************************************************
 * domain_page.h
 * 
 * Allow temporary mapping of domain pages. Based on ideas from the
 * Linux PKMAP code -- the copyrights and credits are retained below.
 */

/*
 * (C) 1999 Andrea Arcangeli, SuSE GmbH, andrea@suse.de
 *          Gerhard Wichert, Siemens AG, Gerhard.Wichert@pdb.siemens.de *
 * Copyright (C) 1999 Ingo Molnar <mingo@redhat.com>
 */

#include <xen/config.h>
#include <xen/sched.h>
#include <xen/mm.h>
#include <xen/perfc.h>
#include <asm/domain_page.h>
#include <asm/pgalloc.h>

unsigned long *mapcache;
static unsigned int map_idx, shadow_map_idx[NR_CPUS];
static spinlock_t map_lock = SPIN_LOCK_UNLOCKED;

/* Use a spare PTE bit to mark entries ready for recycling. */
#define READY_FOR_TLB_FLUSH (1<<10)

static void flush_all_ready_maps(void)
{
    unsigned long *cache = mapcache;

    /* A bit skanky -- depends on having an aligned PAGE_SIZE set of PTEs. */
    do { if ( (*cache & READY_FOR_TLB_FLUSH) ) *cache = 0; }
    while ( ((unsigned long)(++cache) & ~PAGE_MASK) != 0 );

    perfc_incrc(domain_page_tlb_flush);
    local_flush_tlb();
}


void *map_domain_mem(unsigned long pa)
{
    unsigned long va;
    unsigned int idx, cpu = smp_processor_id();
    unsigned long *cache = mapcache;
    unsigned long flags;

    spin_lock_irqsave(&map_lock, flags);

    /* Has some other CPU caused a wrap? We must flush if so. */
    if ( map_idx < shadow_map_idx[cpu] )
    {
        perfc_incrc(domain_page_tlb_flush);
        local_flush_tlb();
    }

    for ( ; ; )
    {
        idx = map_idx = (map_idx + 1) & (MAPCACHE_ENTRIES - 1);
        if ( idx == 0 ) flush_all_ready_maps();
        if ( cache[idx] == 0 ) break;
    }

    cache[idx] = (pa & PAGE_MASK) | __PAGE_HYPERVISOR;

    spin_unlock_irqrestore(&map_lock, flags);

    shadow_map_idx[cpu] = idx;

    va = MAPCACHE_VIRT_START + (idx << PAGE_SHIFT) + (pa & ~PAGE_MASK);
    return (void *)va;
}

void unmap_domain_mem(void *va)
{
    unsigned int idx;
    idx = ((unsigned long)va - MAPCACHE_VIRT_START) >> PAGE_SHIFT;
    mapcache[idx] |= READY_FOR_TLB_FLUSH;
}
