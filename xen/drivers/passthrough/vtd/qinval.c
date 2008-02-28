/*
 * Copyright (c) 2006, Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 59 Temple
 * Place - Suite 330, Boston, MA 02111-1307 USA.
 *
 * Copyright (C) Allen Kay <allen.m.kay@intel.com>
 * Copyright (C) Xiaohui Xin <xiaohui.xin@intel.com>
 */


#include <xen/init.h>
#include <xen/irq.h>
#include <xen/spinlock.h>
#include <xen/sched.h>
#include <xen/xmalloc.h>
#include <xen/domain_page.h>
#include <asm/delay.h>
#include <asm/string.h>
#include <asm/iommu.h>
#include "dmar.h"
#include "vtd.h"
#include "../pci-direct.h"
#include "../pci_regs.h"
#include "msi.h"
#include "extern.h"

static void print_qi_regs(struct iommu *iommu)
{
    u64 val;

    val = dmar_readq(iommu->reg, DMAR_IQA_REG);
    printk("DMAR_IAQ_REG = %"PRIx64"\n", val);

    val = dmar_readq(iommu->reg, DMAR_IQH_REG);
    printk("DMAR_IAH_REG = %"PRIx64"\n", val);

    val = dmar_readq(iommu->reg, DMAR_IQT_REG);
    printk("DMAR_IAT_REG = %"PRIx64"\n", val);
}

static int qinval_next_index(struct iommu *iommu)
{
    u64 val;
    val = dmar_readq(iommu->reg, DMAR_IQT_REG);
    return (val >> 4);
}

static int qinval_update_qtail(struct iommu *iommu, int index)
{
    u64 val;

    /* Need an ASSERT to insure that we have got register lock */
    val = (index < (QINVAL_ENTRY_NR-1)) ? (index + 1) : 0;
    dmar_writeq(iommu->reg, DMAR_IQT_REG, (val << 4));
    return 0;
}

static int gen_cc_inv_dsc(struct iommu *iommu, int index,
    u16 did, u16 source_id, u8 function_mask, u8 granu)
{
    u64 *ptr64;
    unsigned long flags;
    struct qinval_entry * qinval_entry = NULL;
    struct qi_ctrl *qi_ctrl = iommu_qi_ctrl(iommu);

    spin_lock_irqsave(&qi_ctrl->qinval_lock, flags);
    qinval_entry = &qi_ctrl->qinval[index];
    qinval_entry->q.cc_inv_dsc.lo.type = TYPE_INVAL_CONTEXT;
    qinval_entry->q.cc_inv_dsc.lo.granu = granu;
    qinval_entry->q.cc_inv_dsc.lo.res_1 = 0;
    qinval_entry->q.cc_inv_dsc.lo.did = did;
    qinval_entry->q.cc_inv_dsc.lo.sid = source_id;
    qinval_entry->q.cc_inv_dsc.lo.fm = function_mask;
    qinval_entry->q.cc_inv_dsc.lo.res_2 = 0;
    qinval_entry->q.cc_inv_dsc.hi.res = 0;
    spin_unlock_irqrestore(&qi_ctrl->qinval_lock, flags);

    ptr64 = (u64 *)qinval_entry;
    return 0;
}

int queue_invalidate_context(struct iommu *iommu,
    u16 did, u16 source_id, u8 function_mask, u8 granu)
{
    int ret = -1;
    unsigned long flags;
    int index = -1;

    spin_lock_irqsave(&iommu->register_lock, flags);
    index = qinval_next_index(iommu);
    if (index == -1)
        return -EBUSY;
    ret = gen_cc_inv_dsc(iommu, index, did, source_id,
                         function_mask, granu);
    ret |= qinval_update_qtail(iommu, index);
    spin_unlock_irqrestore(&iommu->register_lock, flags);
    return ret;
}

static int gen_iotlb_inv_dsc(struct iommu *iommu, int index,
    u8 granu, u8 dr, u8 dw, u16 did, u8 am, u8 ih, u64 addr)
{
    unsigned long flags;
    struct qinval_entry * qinval_entry = NULL;
    struct qi_ctrl *qi_ctrl = iommu_qi_ctrl(iommu);

    if ( index == -1 )
        return -1;
    spin_lock_irqsave(&qi_ctrl->qinval_lock, flags);

    qinval_entry = &qi_ctrl->qinval[index];
    qinval_entry->q.iotlb_inv_dsc.lo.type = TYPE_INVAL_IOTLB;
    qinval_entry->q.iotlb_inv_dsc.lo.granu = granu;
    qinval_entry->q.iotlb_inv_dsc.lo.dr = 0;
    qinval_entry->q.iotlb_inv_dsc.lo.dw = 0;
    qinval_entry->q.iotlb_inv_dsc.lo.res_1 = 0;
    qinval_entry->q.iotlb_inv_dsc.lo.did = did;
    qinval_entry->q.iotlb_inv_dsc.lo.res_2 = 0;

    qinval_entry->q.iotlb_inv_dsc.hi.am = am;
    qinval_entry->q.iotlb_inv_dsc.hi.ih = ih;
    qinval_entry->q.iotlb_inv_dsc.hi.res_1 = 0;
    qinval_entry->q.iotlb_inv_dsc.hi.addr = addr;

    spin_unlock_irqrestore(&qi_ctrl->qinval_lock, flags);
    return 0;
}

int queue_invalidate_iotlb(struct iommu *iommu,
    u8 granu, u8 dr, u8 dw, u16 did, u8 am, u8 ih, u64 addr)
{
    int ret = -1;
    unsigned long flags;
    int index = -1;

    spin_lock_irqsave(&iommu->register_lock, flags);

    index = qinval_next_index(iommu);
    ret = gen_iotlb_inv_dsc(iommu, index, granu, dr, dw, did,
                            am, ih, addr);
    ret |= qinval_update_qtail(iommu, index);
    spin_unlock_irqrestore(&iommu->register_lock, flags);
    return ret;
}

static int gen_wait_dsc(struct iommu *iommu, int index,
    u8 iflag, u8 sw, u8 fn, u32 sdata, volatile u32 *saddr)
{
    u64 *ptr64;
    unsigned long flags;
    struct qinval_entry * qinval_entry = NULL;
    struct qi_ctrl *qi_ctrl = iommu_qi_ctrl(iommu);

    if ( index == -1 )
        return -1;
    spin_lock_irqsave(&qi_ctrl->qinval_lock, flags);
    qinval_entry = &qi_ctrl->qinval[index];
    qinval_entry->q.inv_wait_dsc.lo.type = TYPE_INVAL_WAIT;
    qinval_entry->q.inv_wait_dsc.lo.iflag = iflag;
    qinval_entry->q.inv_wait_dsc.lo.sw = sw;
    qinval_entry->q.inv_wait_dsc.lo.fn = fn;
    qinval_entry->q.inv_wait_dsc.lo.res_1 = 0;
    qinval_entry->q.inv_wait_dsc.lo.sdata = sdata;
    qinval_entry->q.inv_wait_dsc.hi.res_1 = 0;
    qinval_entry->q.inv_wait_dsc.hi.saddr = virt_to_maddr(saddr) >> 2;
    spin_unlock_irqrestore(&qi_ctrl->qinval_lock, flags);
    ptr64 = (u64 *)qinval_entry;
    return 0;
}

static int queue_invalidate_wait(struct iommu *iommu,
    u8 iflag, u8 sw, u8 fn, u32 sdata, volatile u32 *saddr)
{
    unsigned long flags;
    unsigned long start_time;
    int index = -1;
    int ret = -1;
    struct qi_ctrl *qi_ctrl = iommu_qi_ctrl(iommu);

    spin_lock_irqsave(&qi_ctrl->qinval_poll_lock, flags);
    spin_lock_irqsave(&iommu->register_lock, flags);
    index = qinval_next_index(iommu);
    if (*saddr == 1)
        *saddr = 0;
    ret = gen_wait_dsc(iommu, index, iflag, sw, fn, sdata, saddr);
    ret |= qinval_update_qtail(iommu, index);
    spin_unlock_irqrestore(&iommu->register_lock, flags);

    /* Now we don't support interrupt method */
    if ( sw )
    {
        /* In case all wait descriptor writes to same addr with same data */
        start_time = jiffies;
        while ( *saddr != 1 ) {
            if (time_after(jiffies, start_time + DMAR_OPERATION_TIMEOUT)) {
                print_qi_regs(iommu);
                panic("queue invalidate wait descriptor was not executed\n");
            }
            cpu_relax();
        }
    }
    spin_unlock_irqrestore(&qi_ctrl->qinval_poll_lock, flags);
    return ret;
}

int invalidate_sync(struct iommu *iommu)
{
    int ret = -1;
    struct qi_ctrl *qi_ctrl = iommu_qi_ctrl(iommu);

    if (qi_ctrl->qinval)
    {
        ret = queue_invalidate_wait(iommu,
            0, 1, 1, 1, &qi_ctrl->qinval_poll_status);
        return ret;
    }
    return 0;
}

static int gen_dev_iotlb_inv_dsc(struct iommu *iommu, int index,
    u32 max_invs_pend, u16 sid, u16 size, u64 addr)
{
    unsigned long flags;
    struct qinval_entry * qinval_entry = NULL;
    struct qi_ctrl *qi_ctrl = iommu_qi_ctrl(iommu);

    if ( index == -1 )
        return -1;
    spin_lock_irqsave(&qi_ctrl->qinval_lock, flags);

    qinval_entry = &qi_ctrl->qinval[index];
    qinval_entry->q.dev_iotlb_inv_dsc.lo.type = TYPE_INVAL_DEVICE_IOTLB;
    qinval_entry->q.dev_iotlb_inv_dsc.lo.res_1 = 0;
    qinval_entry->q.dev_iotlb_inv_dsc.lo.max_invs_pend = max_invs_pend;
    qinval_entry->q.dev_iotlb_inv_dsc.lo.res_2 = 0;
    qinval_entry->q.dev_iotlb_inv_dsc.lo.sid = sid;
    qinval_entry->q.dev_iotlb_inv_dsc.lo.res_3 = 0;

    qinval_entry->q.dev_iotlb_inv_dsc.hi.size = size;
    qinval_entry->q.dev_iotlb_inv_dsc.hi.addr = addr;

    spin_unlock_irqrestore(&qi_ctrl->qinval_lock, flags);
    return 0;
}

int queue_invalidate_device_iotlb(struct iommu *iommu,
    u32 max_invs_pend, u16 sid, u16 size, u64 addr)
{
    int ret = -1;
    unsigned long flags;
    int index = -1;

    spin_lock_irqsave(&iommu->register_lock, flags);
    index = qinval_next_index(iommu);
    ret = gen_dev_iotlb_inv_dsc(iommu, index, max_invs_pend,
                                sid, size, addr);
    ret |= qinval_update_qtail(iommu, index);
    spin_unlock_irqrestore(&iommu->register_lock, flags);
    return ret;
}

static int gen_iec_inv_dsc(struct iommu *iommu, int index,
    u8 granu, u8 im, u16 iidx)
{
    unsigned long flags;
    struct qinval_entry * qinval_entry = NULL;
    struct qi_ctrl *qi_ctrl = iommu_qi_ctrl(iommu);

    if ( index == -1 )
        return -1;
    spin_lock_irqsave(&qi_ctrl->qinval_lock, flags);

    qinval_entry = &qi_ctrl->qinval[index];
    qinval_entry->q.iec_inv_dsc.lo.type = TYPE_INVAL_IEC;
    qinval_entry->q.iec_inv_dsc.lo.granu = granu;
    qinval_entry->q.iec_inv_dsc.lo.res_1 = 0;
    qinval_entry->q.iec_inv_dsc.lo.im = im;
    qinval_entry->q.iec_inv_dsc.lo.iidx = iidx;
    qinval_entry->q.iec_inv_dsc.lo.res_2 = 0;
    qinval_entry->q.iec_inv_dsc.hi.res = 0;

    spin_unlock_irqrestore(&qi_ctrl->qinval_lock, flags);
    return 0;
}

int queue_invalidate_iec(struct iommu *iommu, u8 granu, u8 im, u16 iidx)
{
    int ret;
    unsigned long flags;
    int index = -1;

    spin_lock_irqsave(&iommu->register_lock, flags);
    index = qinval_next_index(iommu);
    ret = gen_iec_inv_dsc(iommu, index, granu, im, iidx);
    ret |= qinval_update_qtail(iommu, index);
    spin_unlock_irqrestore(&iommu->register_lock, flags);
    return ret;
}

u64 iec_cap;
int __iommu_flush_iec(struct iommu *iommu, u8 granu, u8 im, u16 iidx)
{
    int ret;
    ret = queue_invalidate_iec(iommu, granu, im, iidx);
    ret |= invalidate_sync(iommu);

    /*
     * reading vt-d architecture register will ensure
     * draining happens in implementation independent way.
     */
    iec_cap = dmar_readq(iommu->reg, DMAR_CAP_REG);
    return ret;
}

int iommu_flush_iec_global(struct iommu *iommu)
{
    return __iommu_flush_iec(iommu, IEC_GLOBAL_INVL, 0, 0);
}

int iommu_flush_iec_index(struct iommu *iommu, u8 im, u16 iidx)
{
   return __iommu_flush_iec(iommu, IEC_INDEX_INVL, im, iidx);
}

static int flush_context_qi(
    void *_iommu, u16 did, u16 sid, u8 fm, u64 type,
    int non_present_entry_flush)
{
    int ret = 0;
    struct iommu *iommu = (struct iommu *)_iommu;
    struct qi_ctrl *qi_ctrl = iommu_qi_ctrl(iommu);

    /*
     * In the non-present entry flush case, if hardware doesn't cache
     * non-present entry we do nothing and if hardware cache non-present
     * entry, we flush entries of domain 0 (the domain id is used to cache
     * any non-present entries)
     */
    if ( non_present_entry_flush )
    {
        if ( !cap_caching_mode(iommu->cap) )
            return 1;
        else
            did = 0;
    }

    if (qi_ctrl->qinval)
    {
        ret = queue_invalidate_context(iommu, did, sid, fm,
                                       type >> DMA_CCMD_INVL_GRANU_OFFSET);
        ret |= invalidate_sync(iommu);
    }
    return ret;
}

static int flush_iotlb_qi(
    void *_iommu, u16 did,
    u64 addr, unsigned int size_order, u64 type,
    int non_present_entry_flush)
{
    u8 dr = 0, dw = 0;
    int ret = 0;
    struct iommu *iommu = (struct iommu *)_iommu;
    struct qi_ctrl *qi_ctrl = iommu_qi_ctrl(iommu);

    /*
     * In the non-present entry flush case, if hardware doesn't cache
     * non-present entry we do nothing and if hardware cache non-present
     * entry, we flush entries of domain 0 (the domain id is used to cache
     * any non-present entries)
     */
    if ( non_present_entry_flush )
    {
        if ( !cap_caching_mode(iommu->cap) )
            return 1;
        else
            did = 0;
    }

    if (qi_ctrl->qinval) {
        /* use queued invalidation */
        if (cap_write_drain(iommu->cap))
            dw = 1;
        if (cap_read_drain(iommu->cap))
            dr = 1;
        /* Need to conside the ih bit later */
        ret = queue_invalidate_iotlb(iommu,
                  (type >> DMA_TLB_FLUSH_GRANU_OFFSET), dr,
                  dw, did, (u8)size_order, 0, addr);
        ret |= invalidate_sync(iommu);
    }
    return ret;
}

int qinval_setup(struct iommu *iommu)
{
    unsigned long start_time;
    u64 paddr;
    u32 status = 0;
    struct qi_ctrl *qi_ctrl;
    struct iommu_flush *flush;

    qi_ctrl = iommu_qi_ctrl(iommu);
    flush = iommu_get_flush(iommu);

    if ( !ecap_queued_inval(iommu->ecap) )
        return -ENODEV;

    if (qi_ctrl->qinval == NULL) {
        qi_ctrl->qinval = alloc_xenheap_page();
        if (qi_ctrl->qinval == NULL)
            panic("Cannot allocate memory for qi_ctrl->qinval\n");
        memset((u8*)qi_ctrl->qinval, 0, PAGE_SIZE_4K);
        flush->context = flush_context_qi;
        flush->iotlb = flush_iotlb_qi;
    }
    paddr = virt_to_maddr(qi_ctrl->qinval);

    /* Setup Invalidation Queue Address(IQA) register with the
     * address of the page we just allocated.  QS field at
     * bits[2:0] to indicate size of queue is one 4KB page.
     * That's 256 entries.  Queued Head (IQH) and Queue Tail (IQT)
     * registers are automatically reset to 0 with write
     * to IQA register.
     */
    dmar_writeq(iommu->reg, DMAR_IQA_REG, paddr);

    /* enable queued invalidation hardware */
    iommu->gcmd |= DMA_GCMD_QIE;
    dmar_writel(iommu->reg, DMAR_GCMD_REG, iommu->gcmd);

    /* Make sure hardware complete it */
    start_time = jiffies;
    while (1) {
        status = dmar_readl(iommu->reg, DMAR_GSTS_REG);
        if (status & DMA_GSTS_QIES)
            break;
        if (time_after(jiffies, start_time + DMAR_OPERATION_TIMEOUT))
            panic("Cannot set QIE field for queue invalidation\n");
        cpu_relax();
    }
    status = 0;
    return status;
}