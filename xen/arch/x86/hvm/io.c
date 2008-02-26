/*
 * io.c: Handling I/O and interrupts.
 *
 * Copyright (c) 2004, Intel Corporation.
 * Copyright (c) 2005, International Business Machines Corporation.
 * Copyright (c) 2008, Citrix Systems, Inc.
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
 */

#include <xen/config.h>
#include <xen/init.h>
#include <xen/mm.h>
#include <xen/lib.h>
#include <xen/errno.h>
#include <xen/trace.h>
#include <xen/event.h>
#include <xen/hypercall.h>
#include <asm/current.h>
#include <asm/cpufeature.h>
#include <asm/processor.h>
#include <asm/msr.h>
#include <asm/apic.h>
#include <asm/paging.h>
#include <asm/shadow.h>
#include <asm/p2m.h>
#include <asm/hvm/hvm.h>
#include <asm/hvm/support.h>
#include <asm/hvm/vpt.h>
#include <asm/hvm/vpic.h>
#include <asm/hvm/vlapic.h>
#include <asm/hvm/trace.h>
#include <asm/hvm/emulate.h>
#include <public/sched.h>
#include <xen/iocap.h>
#include <public/hvm/ioreq.h>

int hvm_buffered_io_send(ioreq_t *p)
{
    struct vcpu *v = current;
    struct hvm_ioreq_page *iorp = &v->domain->arch.hvm_domain.buf_ioreq;
    buffered_iopage_t *pg = iorp->va;
    buf_ioreq_t bp;
    /* Timeoffset sends 64b data, but no address. Use two consecutive slots. */
    int qw = 0;

    /* Ensure buffered_iopage fits in a page */
    BUILD_BUG_ON(sizeof(buffered_iopage_t) > PAGE_SIZE);

    /*
     * Return 0 for the cases we can't deal with:
     *  - 'addr' is only a 20-bit field, so we cannot address beyond 1MB
     *  - we cannot buffer accesses to guest memory buffers, as the guest
     *    may expect the memory buffer to be synchronously accessed
     *  - the count field is usually used with data_is_ptr and since we don't
     *    support data_is_ptr we do not waste space for the count field either
     */
    if ( (p->addr > 0xffffful) || p->data_is_ptr || (p->count != 1) )
        return 0;

    bp.type = p->type;
    bp.dir  = p->dir;
    switch ( p->size )
    {
    case 1:
        bp.size = 0;
        break;
    case 2:
        bp.size = 1;
        break;
    case 4:
        bp.size = 2;
        break;
    case 8:
        bp.size = 3;
        qw = 1;
        break;
    default:
        gdprintk(XENLOG_WARNING, "unexpected ioreq size:%"PRId64"\n", p->size);
        return 0;
    }
    
    bp.data = p->data;
    bp.addr = p->addr;
    
    spin_lock(&iorp->lock);

    if ( (pg->write_pointer - pg->read_pointer) >=
         (IOREQ_BUFFER_SLOT_NUM - qw) )
    {
        /* The queue is full: send the iopacket through the normal path. */
        spin_unlock(&iorp->lock);
        return 0;
    }
    
    memcpy(&pg->buf_ioreq[pg->write_pointer % IOREQ_BUFFER_SLOT_NUM],
           &bp, sizeof(bp));
    
    if ( qw )
    {
        bp.data = p->data >> 32;
        memcpy(&pg->buf_ioreq[(pg->write_pointer+1) % IOREQ_BUFFER_SLOT_NUM],
               &bp, sizeof(bp));
    }

    /* Make the ioreq_t visible /before/ write_pointer. */
    wmb();
    pg->write_pointer += qw ? 2 : 1;

    spin_unlock(&iorp->lock);
    
    return 1;
}

void send_pio_req(unsigned long port, unsigned long count, int size,
                  paddr_t value, int dir, int df, int value_is_ptr)
{
    struct vcpu *v = current;
    vcpu_iodata_t *vio = get_ioreq(v);
    ioreq_t *p = &vio->vp_ioreq;

    if ( p->state != STATE_IOREQ_NONE )
        gdprintk(XENLOG_WARNING,
                 "WARNING: send pio with something already pending (%d)?\n",
                 p->state);

    p->dir = dir;
    p->data_is_ptr = value_is_ptr;
    p->type = IOREQ_TYPE_PIO;
    p->size = size;
    p->addr = port;
    p->count = count;
    p->df = df;
    p->data = value;
    p->io_count++;

    if ( hvm_portio_intercept(p) )
    {
        p->state = STATE_IORESP_READY;
        hvm_io_assist();
    }
    else
    {
        hvm_send_assist_req(v);
    }
}

void send_mmio_req(unsigned char type, paddr_t gpa,
                   unsigned long count, int size, paddr_t value,
                   int dir, int df, int value_is_ptr)
{
    struct vcpu *v = current;
    vcpu_iodata_t *vio = get_ioreq(v);
    ioreq_t *p = &vio->vp_ioreq;

    if ( p->state != STATE_IOREQ_NONE )
        gdprintk(XENLOG_WARNING,
                 "WARNING: send mmio with something already pending (%d)?\n",
                 p->state);

    p->dir = dir;
    p->data_is_ptr = value_is_ptr;
    p->type = type;
    p->size = size;
    p->addr = gpa;
    p->count = count;
    p->df = df;
    p->data = value;
    p->io_count++;

    if ( hvm_mmio_intercept(p) || hvm_buffered_io_intercept(p) )
    {
        p->state = STATE_IORESP_READY;
        hvm_io_assist();
    }
    else
    {
        hvm_send_assist_req(v);
    }
}

void send_timeoffset_req(unsigned long timeoff)
{
    ioreq_t p[1];

    if ( timeoff == 0 )
        return;

    memset(p, 0, sizeof(*p));

    p->type = IOREQ_TYPE_TIMEOFFSET;
    p->size = 8;
    p->count = 1;
    p->dir = IOREQ_WRITE;
    p->data = timeoff;

    p->state = STATE_IOREQ_READY;

    if ( !hvm_buffered_io_send(p) )
        printk("Unsuccessful timeoffset update\n");
}

/* Ask ioemu mapcache to invalidate mappings. */
void send_invalidate_req(void)
{
    struct vcpu *v = current;
    vcpu_iodata_t *vio;
    ioreq_t *p;

    vio = get_ioreq(v);
    if ( vio == NULL )
    {
        printk("bad shared page: %lx\n", (unsigned long) vio);
        domain_crash_synchronous();
    }

    p = &vio->vp_ioreq;
    if ( p->state != STATE_IOREQ_NONE )
        printk("WARNING: send invalidate req with something "
               "already pending (%d)?\n", p->state);

    p->type = IOREQ_TYPE_INVALIDATE;
    p->size = 4;
    p->dir = IOREQ_WRITE;
    p->data = ~0UL; /* flush all */
    p->io_count++;

    hvm_send_assist_req(v);
}

int handle_mmio(void)
{
    struct hvm_emulate_ctxt ctxt;
    struct vcpu *curr = current;
    int rc;

    hvm_emulate_prepare(&ctxt, guest_cpu_user_regs());

    rc = hvm_emulate_one(&ctxt);

    switch ( rc )
    {
    case X86EMUL_UNHANDLEABLE:
        gdprintk(XENLOG_WARNING,
                 "MMIO emulation failed @ %04x:%lx: "
                 "%02x %02x %02x %02x %02x %02x\n",
                 hvmemul_get_seg_reg(x86_seg_cs, &ctxt)->sel,
                 ctxt.insn_buf_eip,
                 ctxt.insn_buf[0], ctxt.insn_buf[1],
                 ctxt.insn_buf[2], ctxt.insn_buf[3],
                 ctxt.insn_buf[4], ctxt.insn_buf[5]);
        return 0;
    case X86EMUL_EXCEPTION:
        if ( ctxt.exn_pending )
            hvm_inject_exception(ctxt.exn_vector, ctxt.exn_error_code, 0);
        break;
    default:
        break;
    }

    hvm_emulate_writeback(&ctxt);

    curr->arch.hvm_vcpu.mmio_in_progress = curr->arch.hvm_vcpu.io_in_progress;

    return 1;
}

void hvm_io_assist(void)
{
    struct vcpu *v = current;
    ioreq_t *p = &get_ioreq(v)->vp_ioreq;

    if ( p->state != STATE_IORESP_READY )
    {
        gdprintk(XENLOG_ERR, "Unexpected HVM iorequest state %d.\n", p->state);
        domain_crash(v->domain);
        goto out;
    }

    rmb(); /* see IORESP_READY /then/ read contents of ioreq */

    p->state = STATE_IOREQ_NONE;

    if ( v->arch.hvm_vcpu.io_in_progress )
    {
        v->arch.hvm_vcpu.io_in_progress = 0;
        if ( (p->dir == IOREQ_READ) && !p->data_is_ptr )
        {
            v->arch.hvm_vcpu.io_completed = 1;
            v->arch.hvm_vcpu.io_data = p->data;
            if ( v->arch.hvm_vcpu.mmio_in_progress )
                (void)handle_mmio();
        }
    }

 out:
    vcpu_end_shutdown_deferral(v);
}

void dpci_ioport_read(uint32_t mport, ioreq_t *p)
{
    uint64_t i;
    uint64_t z_data;
    uint64_t length = (p->count * p->size);

    for ( i = 0; i < length; i += p->size )
    {
        z_data = ~0ULL;
        
        switch ( p->size )
        {
        case 1:
            z_data = (uint64_t)inb(mport);
            break;
        case 2:
            z_data = (uint64_t)inw(mport);
            break;
        case 4:
            z_data = (uint64_t)inl(mport);
            break;
        default:
            gdprintk(XENLOG_ERR, "Error: unable to handle size: %"
                     PRId64 "\n", p->size);
            return;
        }

        p->data = z_data;
        if ( p->data_is_ptr &&
             hvm_copy_to_guest_phys(p->data + i, (void *)&z_data,
                                    (int)p->size) )
        {
            gdprintk(XENLOG_ERR, "Error: couldn't copy to hvm phys\n");
            return;
        }
    }
}

void dpci_ioport_write(uint32_t mport, ioreq_t *p)
{
    uint64_t i;
    uint64_t z_data = 0;
    uint64_t length = (p->count * p->size);

    for ( i = 0; i < length; i += p->size )
    {
        z_data = p->data;
        if ( p->data_is_ptr &&
             hvm_copy_from_guest_phys((void *)&z_data,
                                      p->data + i, (int)p->size) )
        {
            gdprintk(XENLOG_ERR, "Error: couldn't copy from hvm phys\n");
            return;
        }

        switch ( p->size )
        {
        case 1:
            outb((uint8_t) z_data, mport);
            break;
        case 2:
            outw((uint16_t) z_data, mport);
            break;
        case 4:
            outl((uint32_t) z_data, mport);
            break;
        default:
            gdprintk(XENLOG_ERR, "Error: unable to handle size: %"
                     PRId64 "\n", p->size);
            break;
        }
    }
}

int dpci_ioport_intercept(ioreq_t *p)
{
    struct domain *d = current->domain;
    struct hvm_iommu *hd = domain_hvm_iommu(d);
    struct g2m_ioport *g2m_ioport;
    unsigned int mport, gport = p->addr;
    unsigned int s = 0, e = 0;

    list_for_each_entry( g2m_ioport, &hd->g2m_ioport_list, list )
    {
        s = g2m_ioport->gport;
        e = s + g2m_ioport->np;
        if ( (gport >= s) && (gport < e) )
            goto found;
    }

    return 0;

 found:
    mport = (gport - s) + g2m_ioport->mport;

    if ( !ioports_access_permitted(d, mport, mport + p->size - 1) ) 
    {
        gdprintk(XENLOG_ERR, "Error: access to gport=0x%x denied!\n",
                 (uint32_t)p->addr);
        return 0;
    }

    switch ( p->dir )
    {
    case IOREQ_READ:
        dpci_ioport_read(mport, p);
        break;
    case IOREQ_WRITE:
        dpci_ioport_write(mport, p);
        break;
    default:
        gdprintk(XENLOG_ERR, "Error: couldn't handle p->dir = %d", p->dir);
    }

    return 1;
}

/*
 * Local variables:
 * mode: C
 * c-set-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
