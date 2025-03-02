/*
 * QEMU emulation of an Intel IOMMU (VT-d)
 *   (DMA Remapping device)
 *
 * Copyright (C) 2013 Knut Omang, Oracle <knut.omang@oracle.com>
 * Copyright (C) 2014 Le Tan, <tamlokveer@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License along
 * with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "qapi/error.h"
#include "hw/sysbus.h"
#include "intel_iommu_internal.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_bus.h"
#include "hw/qdev-properties.h"
#include "hw/i386/pc.h"
#include "hw/i386/apic-msidef.h"
#include "hw/i386/x86-iommu.h"
#include "hw/pci-host/q35.h"
#include "sysemu/kvm.h"
#include "sysemu/dma.h"
#include "sysemu/sysemu.h"
#include "hw/i386/apic_internal.h"
#include "kvm/kvm_i386.h"
#include "migration/vmstate.h"
#include "trace.h"
#include "qemu/jhash.h"
#include "sysemu/iommufd.h"

/* context entry operations */
#define VTD_CE_GET_RID2PASID(ce) \
    ((ce)->val[1] & VTD_SM_CONTEXT_ENTRY_RID2PASID_MASK)
#define VTD_CE_GET_PASID_DIR_TABLE(ce) \
    ((ce)->val[0] & VTD_PASID_DIR_BASE_ADDR_MASK)

/* pe operations */
#define VTD_PE_GET_TYPE(pe) ((pe)->val[0] & VTD_SM_PASID_ENTRY_PGTT)
#define VTD_PE_GET_LEVEL(pe) (2 + (((pe)->val[0] >> 2) & VTD_SM_PASID_ENTRY_AW))

/*
 * PCI bus number (or SID) is not reliable since the device is usaully
 * initialized before guest can configure the PCI bridge
 * (SECONDARY_BUS_NUMBER).
 */
struct vtd_as_key {
    PCIBus *bus;
    uint8_t devfn;
    uint32_t pasid;
};

struct vtd_iotlb_key {
    uint64_t gfn;
    uint32_t pasid;
    uint16_t sid;
    uint8_t level;
};

static void vtd_address_space_refresh_all(IntelIOMMUState *s);
static void vtd_address_space_unmap(VTDAddressSpace *as, IOMMUNotifier *n);
static void vtd_refresh_pasid_bind(IntelIOMMUState *s);

static void vtd_pasid_cache_reset(IntelIOMMUState *s);
static void vtd_pasid_cache_sync(IntelIOMMUState *s,
                                 VTDPASIDCacheInfo *pc_info);
static void vtd_pasid_cache_devsi(IntelIOMMUState *s,
                                  PCIBus *bus, uint16_t devfn);
static VTDPASIDAddressSpace *vtd_add_find_pasid_as(IntelIOMMUState *s,
                                                   PCIBus *bus,
                                                   int devfn,
                                                   uint32_t pasid);
static int vtd_dev_get_rid2pasid(IntelIOMMUState *s, uint8_t bus_num,
                                 uint8_t devfn, uint32_t *rid_pasid);
static gboolean vtd_hash_remove_by_pasid(gpointer key, gpointer value,
                                         gpointer user_data);

static void vtd_panic_require_caching_mode(void)
{
    error_report("We need to set caching-mode=on for intel-iommu to enable "
                 "device assignment with IOMMU protection.");
    exit(1);
}

static void vtd_define_quad(IntelIOMMUState *s, hwaddr addr, uint64_t val,
                            uint64_t wmask, uint64_t w1cmask)
{
    stq_le_p(&s->csr[addr], val);
    stq_le_p(&s->wmask[addr], wmask);
    stq_le_p(&s->w1cmask[addr], w1cmask);
}

static void vtd_define_quad_wo(IntelIOMMUState *s, hwaddr addr, uint64_t mask)
{
    stq_le_p(&s->womask[addr], mask);
}

static void vtd_define_long(IntelIOMMUState *s, hwaddr addr, uint32_t val,
                            uint32_t wmask, uint32_t w1cmask)
{
    stl_le_p(&s->csr[addr], val);
    stl_le_p(&s->wmask[addr], wmask);
    stl_le_p(&s->w1cmask[addr], w1cmask);
}

static void vtd_define_long_wo(IntelIOMMUState *s, hwaddr addr, uint32_t mask)
{
    stl_le_p(&s->womask[addr], mask);
}

/* "External" get/set operations */
static void vtd_set_quad(IntelIOMMUState *s, hwaddr addr, uint64_t val)
{
    uint64_t oldval = ldq_le_p(&s->csr[addr]);
    uint64_t wmask = ldq_le_p(&s->wmask[addr]);
    uint64_t w1cmask = ldq_le_p(&s->w1cmask[addr]);
    stq_le_p(&s->csr[addr],
             ((oldval & ~wmask) | (val & wmask)) & ~(w1cmask & val));
}

static void vtd_set_long(IntelIOMMUState *s, hwaddr addr, uint32_t val)
{
    uint32_t oldval = ldl_le_p(&s->csr[addr]);
    uint32_t wmask = ldl_le_p(&s->wmask[addr]);
    uint32_t w1cmask = ldl_le_p(&s->w1cmask[addr]);
    stl_le_p(&s->csr[addr],
             ((oldval & ~wmask) | (val & wmask)) & ~(w1cmask & val));
}

static uint64_t vtd_get_quad(IntelIOMMUState *s, hwaddr addr)
{
    uint64_t val = ldq_le_p(&s->csr[addr]);
    uint64_t womask = ldq_le_p(&s->womask[addr]);
    return val & ~womask;
}

static uint32_t vtd_get_long(IntelIOMMUState *s, hwaddr addr)
{
    uint32_t val = ldl_le_p(&s->csr[addr]);
    uint32_t womask = ldl_le_p(&s->womask[addr]);
    return val & ~womask;
}

/* "Internal" get/set operations */
static uint64_t vtd_get_quad_raw(IntelIOMMUState *s, hwaddr addr)
{
    return ldq_le_p(&s->csr[addr]);
}

static uint32_t vtd_get_long_raw(IntelIOMMUState *s, hwaddr addr)
{
    return ldl_le_p(&s->csr[addr]);
}

static void vtd_set_quad_raw(IntelIOMMUState *s, hwaddr addr, uint64_t val)
{
    stq_le_p(&s->csr[addr], val);
}

static uint32_t vtd_set_clear_mask_long(IntelIOMMUState *s, hwaddr addr,
                                        uint32_t clear, uint32_t mask)
{
    uint32_t new_val = (ldl_le_p(&s->csr[addr]) & ~clear) | mask;
    stl_le_p(&s->csr[addr], new_val);
    return new_val;
}

static uint64_t vtd_set_clear_mask_quad(IntelIOMMUState *s, hwaddr addr,
                                        uint64_t clear, uint64_t mask)
{
    uint64_t new_val = (ldq_le_p(&s->csr[addr]) & ~clear) | mask;
    stq_le_p(&s->csr[addr], new_val);
    return new_val;
}

static inline void vtd_iommu_lock(IntelIOMMUState *s)
{
    qemu_mutex_lock(&s->iommu_lock);
}

static inline void vtd_iommu_unlock(IntelIOMMUState *s)
{
    qemu_mutex_unlock(&s->iommu_lock);
}

static void vtd_update_scalable_state(IntelIOMMUState *s)
{
    uint64_t val = vtd_get_quad_raw(s, DMAR_RTADDR_REG);

    if (s->scalable_mode) {
        s->root_scalable = val & VTD_RTADDR_SMT;
    }
}

static void vtd_update_iq_dw(IntelIOMMUState *s)
{
    uint64_t val = vtd_get_quad_raw(s, DMAR_IQA_REG);

    if (s->ecap & VTD_ECAP_SMTS &&
        val & VTD_IQA_DW_MASK) {
        s->iq_dw = true;
    } else {
        s->iq_dw = false;
    }
}

/* Whether the address space needs to notify new mappings */
static inline gboolean vtd_as_has_map_notifier(VTDAddressSpace *as)
{
    return as->notifier_flags & IOMMU_NOTIFIER_MAP;
}

/* GHashTable functions */
static gboolean vtd_iotlb_equal(gconstpointer v1, gconstpointer v2)
{
    const struct vtd_iotlb_key *key1 = v1;
    const struct vtd_iotlb_key *key2 = v2;

    return key1->sid == key2->sid &&
           key1->pasid == key2->pasid &&
           key1->level == key2->level &&
           key1->gfn == key2->gfn;
}

static guint vtd_iotlb_hash(gconstpointer v)
{
    const struct vtd_iotlb_key *key = v;
    uint64_t hash64 = key->gfn | ((uint64_t)(key->sid) << VTD_IOTLB_SID_SHIFT) |
        (uint64_t)(key->level - 1) << VTD_IOTLB_LVL_SHIFT |
        (uint64_t)(key->pasid) << VTD_IOTLB_PASID_SHIFT;

    return (guint)((hash64 >> 32) ^ (hash64 & 0xffffffffU));
}

static gboolean vtd_as_equal(gconstpointer v1, gconstpointer v2)
{
    const struct vtd_as_key *key1 = v1;
    const struct vtd_as_key *key2 = v2;

    return (key1->bus == key2->bus) && (key1->devfn == key2->devfn) &&
           (key1->pasid == key2->pasid);
}

static gboolean vtd_as_idev_equal(gconstpointer v1, gconstpointer v2)
{
    const struct vtd_as_key *key1 = v1;
    const struct vtd_as_key *key2 = v2;

    return (key1->bus == key2->bus) && (key1->devfn == key2->devfn);
}
/*
 * Note that we use pointer to PCIBus as the key, so hashing/shifting
 * based on the pointer value is intended. Note that we deal with
 * collisions through vtd_as_equal().
 */
static guint vtd_as_hash(gconstpointer v)
{
    const struct vtd_as_key *key = v;
    guint value = (guint)(uintptr_t)key->bus;

    return (guint)(value << 8 | key->devfn);
}

static gboolean vtd_hash_remove_by_domain(gpointer key, gpointer value,
                                          gpointer user_data)
{
    VTDIOTLBEntry *entry = (VTDIOTLBEntry *)value;
    uint16_t domain_id = *(uint16_t *)user_data;
    return entry->domain_id == domain_id;
}

/* The shift of an addr for a certain level of paging structure */
static inline uint32_t vtd_slpt_level_shift(uint32_t level)
{
    assert(level != 0);
    return VTD_PAGE_SHIFT_4K + (level - 1) * VTD_SL_LEVEL_BITS;
}

static inline uint64_t vtd_slpt_level_page_mask(uint32_t level)
{
    return ~((1ULL << vtd_slpt_level_shift(level)) - 1);
}

static gboolean vtd_hash_remove_by_page(gpointer key, gpointer value,
                                        gpointer user_data)
{
    VTDIOTLBEntry *entry = (VTDIOTLBEntry *)value;
    VTDIOTLBPageInvInfo *info = (VTDIOTLBPageInvInfo *)user_data;
    uint64_t gfn = (info->addr >> VTD_PAGE_SHIFT_4K) & info->mask;
    uint64_t gfn_tlb = (info->addr & entry->mask) >> VTD_PAGE_SHIFT_4K;
    return (entry->domain_id == info->domain_id) &&
            (info->is_piotlb ? (entry->pasid == info->pasid) : 1) &&
            (((entry->gfn & info->mask) == gfn) ||
             (entry->gfn == gfn_tlb));
}

/* Reset all the gen of VTDAddressSpace to zero and set the gen of
 * IntelIOMMUState to 1.  Must be called with IOMMU lock held.
 */
static void vtd_reset_context_cache_locked(IntelIOMMUState *s)
{
    VTDAddressSpace *vtd_as;
    GHashTableIter as_it;

    trace_vtd_context_cache_reset();

    g_hash_table_iter_init(&as_it, s->vtd_address_spaces);

    while (g_hash_table_iter_next(&as_it, NULL, (void **)&vtd_as)) {
        vtd_as->context_cache_entry.context_cache_gen = 0;
    }
    s->context_cache_gen = 1;
}

/* Must be called with IOMMU lock held. */
static void vtd_reset_iotlb_locked(IntelIOMMUState *s)
{
    assert(s->iotlb);
    g_hash_table_remove_all(s->iotlb);
}

static void vtd_reset_iotlb(IntelIOMMUState *s)
{
    vtd_iommu_lock(s);
    vtd_reset_iotlb_locked(s);
    vtd_iommu_unlock(s);
}

static void vtd_reset_piotlb(IntelIOMMUState *s)
{
    assert(s->p_iotlb);
    g_hash_table_remove_all(s->p_iotlb);
}

static void vtd_reset_caches(IntelIOMMUState *s)
{
    vtd_iommu_lock(s);
    vtd_reset_iotlb_locked(s);
    vtd_reset_context_cache_locked(s);
    vtd_pasid_cache_reset(s);
    vtd_reset_piotlb(s);
    vtd_iommu_unlock(s);
}

static uint64_t vtd_get_iotlb_gfn(hwaddr addr, uint32_t level)
{
    return (addr & vtd_slpt_level_page_mask(level)) >> VTD_PAGE_SHIFT_4K;
}

/* Must be called with IOMMU lock held */
static VTDIOTLBEntry *vtd_lookup_iotlb(IntelIOMMUState *s, uint16_t source_id,
                                       uint32_t pasid, hwaddr addr)
{
    struct vtd_iotlb_key key;
    VTDIOTLBEntry *entry;
    int level;

    for (level = VTD_SL_PT_LEVEL; level < VTD_SL_PML4_LEVEL; level++) {
        key.gfn = vtd_get_iotlb_gfn(addr, level);
        key.level = level;
        key.sid = source_id;
        key.pasid = pasid;
        entry = g_hash_table_lookup(s->iotlb, &key);
        if (entry) {
            goto out;
        }
    }

out:
    return entry;
}

/* Must be with IOMMU lock held */
static void vtd_update_iotlb(IntelIOMMUState *s, uint16_t source_id,
                             uint16_t domain_id, hwaddr addr, uint64_t slpte,
                             uint8_t access_flags, uint32_t level,
                             uint32_t pasid)
{
    VTDIOTLBEntry *entry = g_malloc(sizeof(*entry));
    struct vtd_iotlb_key *key = g_malloc(sizeof(*key));
    uint64_t gfn = vtd_get_iotlb_gfn(addr, level);

    trace_vtd_iotlb_page_update(source_id, addr, slpte, domain_id);
    if (g_hash_table_size(s->iotlb) >= VTD_IOTLB_MAX_SIZE) {
        trace_vtd_iotlb_reset("iotlb exceeds size limit");
        vtd_reset_iotlb_locked(s);
    }

    entry->gfn = gfn;
    entry->domain_id = domain_id;
    entry->pte = slpte;
    entry->access_flags = access_flags;
    entry->mask = vtd_slpt_level_page_mask(level);
    entry->pasid = pasid;

    key->gfn = gfn;
    key->sid = source_id;
    key->level = level;
    key->pasid = pasid;

    g_hash_table_replace(s->iotlb, key, entry);
}

/* Given the reg addr of both the message data and address, generate an
 * interrupt via MSI.
 */
static void vtd_generate_interrupt(IntelIOMMUState *s, hwaddr mesg_addr_reg,
                                   hwaddr mesg_data_reg)
{
    MSIMessage msi;

    assert(mesg_data_reg < DMAR_REG_SIZE);
    assert(mesg_addr_reg < DMAR_REG_SIZE);

    msi.address = vtd_get_long_raw(s, mesg_addr_reg);
    msi.data = vtd_get_long_raw(s, mesg_data_reg);

    trace_vtd_irq_generate(msi.address, msi.data);

    apic_get_class(NULL)->send_msi(&msi);
}

/* Generate a fault event to software via MSI if conditions are met.
 * Notice that the value of FSTS_REG being passed to it should be the one
 * before any update.
 */
static void vtd_generate_fault_event(IntelIOMMUState *s, uint32_t pre_fsts)
{
    if (pre_fsts & VTD_FSTS_PPF || pre_fsts & VTD_FSTS_PFO ||
        pre_fsts & VTD_FSTS_IQE) {
        error_report_once("There are previous interrupt conditions "
                          "to be serviced by software, fault event "
                          "is not generated");
        return;
    }
    vtd_set_clear_mask_long(s, DMAR_FECTL_REG, 0, VTD_FECTL_IP);
    if (vtd_get_long_raw(s, DMAR_FECTL_REG) & VTD_FECTL_IM) {
        error_report_once("Interrupt Mask set, irq is not generated");
    } else {
        vtd_generate_interrupt(s, DMAR_FEADDR_REG, DMAR_FEDATA_REG);
        vtd_set_clear_mask_long(s, DMAR_FECTL_REG, VTD_FECTL_IP, 0);
    }
}

/* Check if the Fault (F) field of the Fault Recording Register referenced by
 * @index is Set.
 */
static bool vtd_is_frcd_set(IntelIOMMUState *s, uint16_t index)
{
    /* Each reg is 128-bit */
    hwaddr addr = DMAR_FRCD_REG_OFFSET + (((uint64_t)index) << 4);
    addr += 8; /* Access the high 64-bit half */

    assert(index < DMAR_FRCD_REG_NR);

    return vtd_get_quad_raw(s, addr) & VTD_FRCD_F;
}

/* Update the PPF field of Fault Status Register.
 * Should be called whenever change the F field of any fault recording
 * registers.
 */
static void vtd_update_fsts_ppf(IntelIOMMUState *s)
{
    uint32_t i;
    uint32_t ppf_mask = 0;

    for (i = 0; i < DMAR_FRCD_REG_NR; i++) {
        if (vtd_is_frcd_set(s, i)) {
            ppf_mask = VTD_FSTS_PPF;
            break;
        }
    }
    vtd_set_clear_mask_long(s, DMAR_FSTS_REG, VTD_FSTS_PPF, ppf_mask);
    trace_vtd_fsts_ppf(!!ppf_mask);
}

static void vtd_set_frcd_and_update_ppf(IntelIOMMUState *s, uint16_t index)
{
    /* Each reg is 128-bit */
    hwaddr addr = DMAR_FRCD_REG_OFFSET + (((uint64_t)index) << 4);
    addr += 8; /* Access the high 64-bit half */

    assert(index < DMAR_FRCD_REG_NR);

    vtd_set_clear_mask_quad(s, addr, 0, VTD_FRCD_F);
    vtd_update_fsts_ppf(s);
}

/* Must not update F field now, should be done later */
static void vtd_record_frcd(IntelIOMMUState *s, uint16_t index,
                            uint64_t hi, uint64_t lo)
{
    hwaddr frcd_reg_addr = DMAR_FRCD_REG_OFFSET + (((uint64_t)index) << 4);

    assert(index < DMAR_FRCD_REG_NR);

    vtd_set_quad_raw(s, frcd_reg_addr, lo);
    vtd_set_quad_raw(s, frcd_reg_addr + 8, hi);

    trace_vtd_frr_new(index, hi, lo);
}

/* Try to collapse multiple pending faults from the same requester */
static bool vtd_try_collapse_fault(IntelIOMMUState *s, uint16_t source_id)
{
    uint32_t i;
    uint64_t frcd_reg;
    hwaddr addr = DMAR_FRCD_REG_OFFSET + 8; /* The high 64-bit half */

    for (i = 0; i < DMAR_FRCD_REG_NR; i++) {
        frcd_reg = vtd_get_quad_raw(s, addr);
        if ((frcd_reg & VTD_FRCD_F) &&
            ((frcd_reg & VTD_FRCD_SID_MASK) == source_id)) {
            return true;
        }
        addr += 16; /* 128-bit for each */
    }
    return false;
}

/* Log and report an DMAR (address translation) fault to software */
static void vtd_report_frcd_fault(IntelIOMMUState *s, uint64_t source_id,
                                  uint64_t hi, uint64_t lo)
{
    uint32_t fsts_reg = vtd_get_long_raw(s, DMAR_FSTS_REG);

    if (fsts_reg & VTD_FSTS_PFO) {
        error_report_once("New fault is not recorded due to "
                          "Primary Fault Overflow");
        return;
    }

    if (vtd_try_collapse_fault(s, source_id)) {
        error_report_once("New fault is not recorded due to "
                          "compression of faults");
        return;
    }

    if (vtd_is_frcd_set(s, s->next_frcd_reg)) {
        error_report_once("Next Fault Recording Reg is used, "
                          "new fault is not recorded, set PFO field");
        vtd_set_clear_mask_long(s, DMAR_FSTS_REG, 0, VTD_FSTS_PFO);
        return;
    }

    vtd_record_frcd(s, s->next_frcd_reg, hi, lo);

    if (fsts_reg & VTD_FSTS_PPF) {
        error_report_once("There are pending faults already, "
                          "fault event is not generated");
        vtd_set_frcd_and_update_ppf(s, s->next_frcd_reg);
        s->next_frcd_reg++;
        if (s->next_frcd_reg == DMAR_FRCD_REG_NR) {
            s->next_frcd_reg = 0;
        }
    } else {
        vtd_set_clear_mask_long(s, DMAR_FSTS_REG, VTD_FSTS_FRI_MASK,
                                VTD_FSTS_FRI(s->next_frcd_reg));
        vtd_set_frcd_and_update_ppf(s, s->next_frcd_reg); /* Will set PPF */
        s->next_frcd_reg++;
        if (s->next_frcd_reg == DMAR_FRCD_REG_NR) {
            s->next_frcd_reg = 0;
        }
        /* This case actually cause the PPF to be Set.
         * So generate fault event (interrupt).
         */
         vtd_generate_fault_event(s, fsts_reg);
    }
}

/* Log and report an DMAR (address translation) fault to software */
static void vtd_report_dmar_fault(IntelIOMMUState *s, uint16_t source_id,
                                  hwaddr addr, VTDFaultReason fault,
                                  bool is_write, bool is_pasid,
                                  uint32_t pasid)
{
    uint64_t hi, lo;

    assert(fault < VTD_FR_MAX);

    trace_vtd_dmar_fault(source_id, fault, addr, is_write);

    lo = VTD_FRCD_FI(addr);
    hi = VTD_FRCD_SID(source_id) | VTD_FRCD_FR(fault) |
         VTD_FRCD_PV(pasid) | VTD_FRCD_PP(is_pasid);
    if (!is_write) {
        hi |= VTD_FRCD_T;
    }

    vtd_report_frcd_fault(s, source_id, hi, lo);
}


static void vtd_report_ir_fault(IntelIOMMUState *s, uint64_t source_id,
                                VTDFaultReason fault, uint16_t index)
{
    uint64_t hi, lo;

    lo = VTD_FRCD_IR_IDX(index);
    hi = VTD_FRCD_SID(source_id) | VTD_FRCD_FR(fault);

    vtd_report_frcd_fault(s, source_id, hi, lo);
}

/* Handle Invalidation Queue Errors of queued invalidation interface error
 * conditions.
 */
static void vtd_handle_inv_queue_error(IntelIOMMUState *s)
{
    uint32_t fsts_reg = vtd_get_long_raw(s, DMAR_FSTS_REG);

    vtd_set_clear_mask_long(s, DMAR_FSTS_REG, 0, VTD_FSTS_IQE);
    vtd_generate_fault_event(s, fsts_reg);
}

/* Set the IWC field and try to generate an invalidation completion interrupt */
static void vtd_generate_completion_event(IntelIOMMUState *s)
{
    if (vtd_get_long_raw(s, DMAR_ICS_REG) & VTD_ICS_IWC) {
        trace_vtd_inv_desc_wait_irq("One pending, skip current");
        return;
    }
    vtd_set_clear_mask_long(s, DMAR_ICS_REG, 0, VTD_ICS_IWC);
    vtd_set_clear_mask_long(s, DMAR_IECTL_REG, 0, VTD_IECTL_IP);
    if (vtd_get_long_raw(s, DMAR_IECTL_REG) & VTD_IECTL_IM) {
        trace_vtd_inv_desc_wait_irq("IM in IECTL_REG is set, "
                                    "new event not generated");
        return;
    } else {
        /* Generate the interrupt event */
        trace_vtd_inv_desc_wait_irq("Generating complete event");
        vtd_generate_interrupt(s, DMAR_IEADDR_REG, DMAR_IEDATA_REG);
        vtd_set_clear_mask_long(s, DMAR_IECTL_REG, VTD_IECTL_IP, 0);
    }
}

static inline bool vtd_root_entry_present(IntelIOMMUState *s,
                                          VTDRootEntry *re,
                                          uint8_t devfn)
{
    if (s->root_scalable && devfn > UINT8_MAX / 2) {
        return re->hi & VTD_ROOT_ENTRY_P;
    }

    return re->lo & VTD_ROOT_ENTRY_P;
}

static int vtd_get_root_entry(IntelIOMMUState *s, uint8_t index,
                              VTDRootEntry *re)
{
    dma_addr_t addr;

    addr = s->root + index * sizeof(*re);
    if (dma_memory_read(&address_space_memory, addr,
                        re, sizeof(*re), MEMTXATTRS_UNSPECIFIED)) {
        re->lo = 0;
        return -VTD_FR_ROOT_TABLE_INV;
    }
    re->lo = le64_to_cpu(re->lo);
    re->hi = le64_to_cpu(re->hi);
    return 0;
}

static inline bool vtd_ce_present(VTDContextEntry *context)
{
    return context->lo & VTD_CONTEXT_ENTRY_P;
}

static int vtd_get_context_entry_from_root(IntelIOMMUState *s,
                                           VTDRootEntry *re,
                                           uint8_t index,
                                           VTDContextEntry *ce)
{
    dma_addr_t addr, ce_size;

    /* we have checked that root entry is present */
    ce_size = s->root_scalable ? VTD_CTX_ENTRY_SCALABLE_SIZE :
              VTD_CTX_ENTRY_LEGACY_SIZE;

    if (s->root_scalable && index > UINT8_MAX / 2) {
        index = index & (~VTD_DEVFN_CHECK_MASK);
        addr = re->hi & VTD_ROOT_ENTRY_CTP;
    } else {
        addr = re->lo & VTD_ROOT_ENTRY_CTP;
    }

    addr = addr + index * ce_size;
    if (dma_memory_read(&address_space_memory, addr,
                        ce, ce_size, MEMTXATTRS_UNSPECIFIED)) {
        return -VTD_FR_CONTEXT_TABLE_INV;
    }

    ce->lo = le64_to_cpu(ce->lo);
    ce->hi = le64_to_cpu(ce->hi);
    if (ce_size == VTD_CTX_ENTRY_SCALABLE_SIZE) {
        ce->val[2] = le64_to_cpu(ce->val[2]);
        ce->val[3] = le64_to_cpu(ce->val[3]);
    }
    return 0;
}

static inline dma_addr_t vtd_ce_get_slpt_base(VTDContextEntry *ce)
{
    return ce->lo & VTD_CONTEXT_ENTRY_SLPTPTR;
}

static inline uint64_t vtd_get_slpte_addr(uint64_t slpte, uint8_t aw)
{
    return slpte & VTD_SL_PT_BASE_ADDR_MASK(aw);
}

/* Whether the pte indicates the address of the page frame */
static inline bool vtd_is_last_slpte(uint64_t slpte, uint32_t level)
{
    return level == VTD_SL_PT_LEVEL || (slpte & VTD_SL_PT_PAGE_SIZE_MASK);
}

/* Get the content of a spte located in @base_addr[@index] */
static uint64_t vtd_get_slpte(dma_addr_t base_addr, uint32_t index)
{
    uint64_t slpte;

    assert(index < VTD_SL_PT_ENTRY_NR);

    if (dma_memory_read(&address_space_memory,
                        base_addr + index * sizeof(slpte),
                        &slpte, sizeof(slpte), MEMTXATTRS_UNSPECIFIED)) {
        slpte = (uint64_t)-1;
        return slpte;
    }
    slpte = le64_to_cpu(slpte);
    return slpte;
}

/* Given an iova and the level of paging structure, return the offset
 * of current level.
 */
static inline uint32_t vtd_iova_level_offset(uint64_t iova, uint32_t level)
{
    return (iova >> vtd_slpt_level_shift(level)) &
            ((1ULL << VTD_SL_LEVEL_BITS) - 1);
}

/* Check Capability Register to see if the @level of page-table is supported */
static inline bool vtd_is_level_supported(IntelIOMMUState *s, uint32_t level)
{
    return VTD_CAP_SAGAW_MASK & s->cap &
           (1ULL << (level - 2 + VTD_CAP_SAGAW_SHIFT));
}

/* Return true if check passed, otherwise false */
static inline bool vtd_pe_type_check(X86IOMMUState *x86_iommu,
                                     VTDPASIDEntry *pe)
{
    switch (VTD_PE_GET_TYPE(pe)) {
    case VTD_SM_PASID_ENTRY_FLT:
    case VTD_SM_PASID_ENTRY_SLT:
    case VTD_SM_PASID_ENTRY_NESTED:
        break;
    case VTD_SM_PASID_ENTRY_PT:
        if (!x86_iommu->pt_supported) {
            return false;
        }
        break;
    default:
        /* Unknown type */
        return false;
    }
    return true;
}

static inline uint16_t vtd_pe_get_domain_id(VTDPASIDEntry *pe)
{
    return VTD_SM_PASID_ENTRY_DID((pe)->val[1]);
}

static inline uint32_t vtd_sm_ce_get_pdt_entry_num(VTDContextEntry *ce)
{
    return 1U << (VTD_SM_CONTEXT_ENTRY_PDTS(ce->val[0]) + 7);
}

static inline uint32_t vtd_pe_get_fl_aw(VTDPASIDEntry *pe)
{
    return 48 + ((pe->val[2] >> 2) & VTD_SM_PASID_ENTRY_FLPM) * 9;
}

static inline dma_addr_t vtd_pe_get_flpt_base(VTDPASIDEntry *pe)
{
    return pe->val[2] & VTD_SM_PASID_ENTRY_FLPTPTR;
}

static inline void pasid_cache_info_set_error(VTDPASIDCacheInfo *pc_info)
{
    if (pc_info->error_happened) {
        return;
    }
    pc_info->error_happened = true;
}

static inline bool vtd_pdire_present(VTDPASIDDirEntry *pdire)
{
    return pdire->val & 1;
}

/**
 * Caller of this function should check present bit if wants
 * to use pdir entry for further usage except for fpd bit check.
 */
static int vtd_get_pdire_from_pdir_table(dma_addr_t pasid_dir_base,
                                         uint32_t pasid,
                                         VTDPASIDDirEntry *pdire)
{
    uint32_t index;
    dma_addr_t addr, entry_size;

    index = VTD_PASID_DIR_INDEX(pasid);
    entry_size = VTD_PASID_DIR_ENTRY_SIZE;
    addr = pasid_dir_base + index * entry_size;
    if (dma_memory_read(&address_space_memory, addr,
                        pdire, entry_size, MEMTXATTRS_UNSPECIFIED)) {
        return -VTD_FR_PASID_DIR_ACCESS_ERR;
    }

    pdire->val = le64_to_cpu(pdire->val);

    return 0;
}

static inline bool vtd_pe_present(VTDPASIDEntry *pe)
{
    return pe->val[0] & VTD_PASID_ENTRY_P;
}

static inline uint32_t vtd_pe_get_flpt_level(VTDPASIDEntry *pe)
{
    return 4 + ((pe->val[2] >> 2) & VTD_SM_PASID_ENTRY_FLPM);
}

static int vtd_get_pe_in_pasid_leaf_table(IntelIOMMUState *s,
                                          uint32_t pasid,
                                          dma_addr_t addr,
                                          VTDPASIDEntry *pe)
{
    uint32_t index;
    dma_addr_t entry_size;
    X86IOMMUState *x86_iommu = X86_IOMMU_DEVICE(s);
    uint8_t pgtt;

    index = VTD_PASID_TABLE_INDEX(pasid);
    entry_size = VTD_PASID_ENTRY_SIZE;
    addr = addr + index * entry_size;
    if (dma_memory_read(&address_space_memory, addr,
                        pe, entry_size, MEMTXATTRS_UNSPECIFIED)) {
        return -VTD_FR_PASID_TABLE_ACCESS_ERR;
    }
    for (size_t i = 0; i < ARRAY_SIZE(pe->val); i++) {
        pe->val[i] = le64_to_cpu(pe->val[i]);
    }

    /* Do translation type check */
    if (!vtd_pe_type_check(x86_iommu, pe)) {
        return -VTD_FR_PASID_TABLE_ENTRY_INV;
    }

    pgtt = VTD_PE_GET_TYPE(pe);
    if (pgtt == VTD_SM_PASID_ENTRY_SLT &&
        !vtd_is_level_supported(s, VTD_PE_GET_LEVEL(pe)))
            return -VTD_FR_PASID_TABLE_ENTRY_INV;

    if (pgtt == VTD_SM_PASID_ENTRY_FLT &&
        vtd_pe_get_flpt_level(pe) != 4)
            return -VTD_FR_PASID_TABLE_ENTRY_INV;

    return 0;
}

/**
 * Caller of this function should check present bit if wants
 * to use pasid entry for further usage except for fpd bit check.
 */
static int vtd_get_pe_from_pdire(IntelIOMMUState *s,
                                 uint32_t pasid,
                                 VTDPASIDDirEntry *pdire,
                                 VTDPASIDEntry *pe)
{
    dma_addr_t addr = pdire->val & VTD_PASID_TABLE_BASE_ADDR_MASK;

    return vtd_get_pe_in_pasid_leaf_table(s, pasid, addr, pe);
}

/**
 * This function gets a pasid entry from a specified pasid
 * table (includes dir and leaf table) with a specified pasid.
 * Sanity check should be done to ensure return a present
 * pasid entry to caller.
 */
static int vtd_get_pe_from_pasid_table(IntelIOMMUState *s,
                                       dma_addr_t pasid_dir_base,
                                       uint32_t pasid,
                                       VTDPASIDEntry *pe)
{
    int ret;
    VTDPASIDDirEntry pdire;

    ret = vtd_get_pdire_from_pdir_table(pasid_dir_base,
                                        pasid, &pdire);
    if (ret) {
        return ret;
    }

    if (!vtd_pdire_present(&pdire)) {
        return -VTD_FR_PASID_DIR_ENTRY_P;
    }

    ret = vtd_get_pe_from_pdire(s, pasid, &pdire, pe);
    if (ret) {
        return ret;
    }

    if (!vtd_pe_present(pe)) {
        return -VTD_FR_PASID_ENTRY_P;
    }

    return 0;
}

static int vtd_ce_get_rid2pasid_entry(IntelIOMMUState *s,
                                      VTDContextEntry *ce,
                                      VTDPASIDEntry *pe,
                                      uint32_t pasid)
{
    dma_addr_t pasid_dir_base;
    int ret = 0;

    if (pasid == PCI_NO_PASID) {
        pasid = VTD_CE_GET_RID2PASID(ce);
    }
    pasid_dir_base = VTD_CE_GET_PASID_DIR_TABLE(ce);
    ret = vtd_get_pe_from_pasid_table(s, pasid_dir_base, pasid, pe);

    return ret;
}

static int vtd_ce_get_pasid_fpd(IntelIOMMUState *s,
                                VTDContextEntry *ce,
                                bool *pe_fpd_set,
                                uint32_t pasid)
{
    int ret;
    dma_addr_t pasid_dir_base;
    VTDPASIDDirEntry pdire;
    VTDPASIDEntry pe;

    if (pasid == PCI_NO_PASID) {
        pasid = VTD_CE_GET_RID2PASID(ce);
    }
    pasid_dir_base = VTD_CE_GET_PASID_DIR_TABLE(ce);

    /*
     * No present bit check since fpd is meaningful even
     * if the present bit is clear.
     */
    ret = vtd_get_pdire_from_pdir_table(pasid_dir_base, pasid, &pdire);
    if (ret) {
        return ret;
    }

    if (pdire.val & VTD_PASID_DIR_FPD) {
        *pe_fpd_set = true;
        return 0;
    }

    if (!vtd_pdire_present(&pdire)) {
        return -VTD_FR_PASID_DIR_ENTRY_P;
    }

    /*
     * No present bit check since fpd is meaningful even
     * if the present bit is clear.
     */
    ret = vtd_get_pe_from_pdire(s, pasid, &pdire, &pe);
    if (ret) {
        return ret;
    }

    if (pe.val[0] & VTD_PASID_ENTRY_FPD) {
        *pe_fpd_set = true;
    }

    return 0;
}

/* Get the page-table level that hardware should use for the second-level
 * page-table walk from the Address Width field of context-entry.
 */
static inline uint32_t vtd_ce_get_level(VTDContextEntry *ce)
{
    return 2 + (ce->hi & VTD_CONTEXT_ENTRY_AW);
}

static uint32_t vtd_get_iova_level(IntelIOMMUState *s,
                                   VTDContextEntry *ce,
                                   uint32_t pasid)
{
    VTDPASIDEntry pe;

    if (s->root_scalable) {
        vtd_ce_get_rid2pasid_entry(s, ce, &pe, pasid);
        return VTD_PE_GET_LEVEL(&pe);
    }

    return vtd_ce_get_level(ce);
}

static inline uint32_t vtd_ce_get_agaw(VTDContextEntry *ce)
{
    return 30 + (ce->hi & VTD_CONTEXT_ENTRY_AW) * 9;
}

static uint32_t vtd_get_iova_agaw(IntelIOMMUState *s,
                                  VTDContextEntry *ce,
                                  uint32_t pasid)
{
    VTDPASIDEntry pe;

    if (s->root_scalable) {
        vtd_ce_get_rid2pasid_entry(s, ce, &pe, pasid);
        return 30 + ((pe.val[0] >> 2) & VTD_SM_PASID_ENTRY_AW) * 9;
    }

    return vtd_ce_get_agaw(ce);
}

static inline uint32_t vtd_ce_get_type(VTDContextEntry *ce)
{
    return ce->lo & VTD_CONTEXT_ENTRY_TT;
}

/* Only for Legacy Mode. Return true if check passed, otherwise false */
static inline bool vtd_ce_type_check(X86IOMMUState *x86_iommu,
                                     VTDContextEntry *ce)
{
    switch (vtd_ce_get_type(ce)) {
    case VTD_CONTEXT_TT_MULTI_LEVEL:
        /* Always supported */
        break;
    case VTD_CONTEXT_TT_DEV_IOTLB:
        if (!x86_iommu->dt_supported) {
            error_report_once("%s: DT specified but not supported", __func__);
            return false;
        }
        break;
    case VTD_CONTEXT_TT_PASS_THROUGH:
        if (!x86_iommu->pt_supported) {
            error_report_once("%s: PT specified but not supported", __func__);
            return false;
        }
        break;
    default:
        /* Unknown type */
        error_report_once("%s: unknown ce type: %"PRIu32, __func__,
                          vtd_ce_get_type(ce));
        return false;
    }
    return true;
}

static inline uint64_t vtd_iova_limit(IntelIOMMUState *s,
                                      VTDContextEntry *ce, uint8_t aw,
                                      uint32_t pasid)
{
    uint32_t ce_agaw = vtd_get_iova_agaw(s, ce, pasid);
    return 1ULL << MIN(ce_agaw, aw);
}

/* Return true if IOVA passes range check, otherwise false. */
static inline bool vtd_iova_range_check(IntelIOMMUState *s,
                                        uint64_t iova, VTDContextEntry *ce,
                                        uint8_t aw, uint32_t pasid)
{
    /*
     * Check if @iova is above 2^X-1, where X is the minimum of MGAW
     * in CAP_REG and AW in context-entry.
     */
    return !(iova & ~(vtd_iova_limit(s, ce, aw, pasid) - 1));
}

static dma_addr_t vtd_get_iova_pgtbl_base(IntelIOMMUState *s,
                                          VTDContextEntry *ce,
                                          uint32_t pasid)
{
    VTDPASIDEntry pe;

    if (s->root_scalable) {
        vtd_ce_get_rid2pasid_entry(s, ce, &pe, pasid);
        return pe.val[0] & VTD_SM_PASID_ENTRY_SLPTPTR;
    }

    return vtd_ce_get_slpt_base(ce);
}

/*
 * Rsvd field masks for spte:
 *     vtd_spte_rsvd 4k pages
 *     vtd_spte_rsvd_large large pages
 *
 * We support only 3-level and 4-level page tables (see vtd_init() which
 * sets only VTD_CAP_SAGAW_39bit and maybe VTD_CAP_SAGAW_48bit bits in s->cap).
 */
#define VTD_SPTE_RSVD_LEN 5
static uint64_t vtd_spte_rsvd[VTD_SPTE_RSVD_LEN];
static uint64_t vtd_spte_rsvd_large[VTD_SPTE_RSVD_LEN];

static bool vtd_slpte_nonzero_rsvd(uint64_t slpte, uint32_t level)
{
    uint64_t rsvd_mask;

    /*
     * We should have caught a guest-mis-programmed level earlier,
     * via vtd_is_level_supported.
     */
    assert(level < VTD_SPTE_RSVD_LEN);
    /*
     * Zero level doesn't exist. The smallest level is VTD_SL_PT_LEVEL=1 and
     * checked by vtd_is_last_slpte().
     */
    assert(level);

    if ((level == VTD_SL_PD_LEVEL || level == VTD_SL_PDP_LEVEL) &&
        (slpte & VTD_SL_PT_PAGE_SIZE_MASK)) {
        /* large page */
        rsvd_mask = vtd_spte_rsvd_large[level];
    } else {
        rsvd_mask = vtd_spte_rsvd[level];
    }

    return slpte & rsvd_mask;
}

/* Given the @iova, get relevant @slptep. @slpte_level will be the last level
 * of the translation, can be used for deciding the size of large page.
 */
static int vtd_iova_to_slpte(IntelIOMMUState *s, VTDContextEntry *ce,
                             uint64_t iova, bool is_write,
                             uint64_t *slptep, uint32_t *slpte_level,
                             bool *reads, bool *writes, uint8_t aw_bits,
                             uint32_t pasid)
{
    dma_addr_t addr = vtd_get_iova_pgtbl_base(s, ce, pasid);
    uint32_t level = vtd_get_iova_level(s, ce, pasid);
    uint32_t offset;
    uint64_t slpte;
    uint64_t access_right_check;
    uint64_t xlat, size;

    if (!vtd_iova_range_check(s, iova, ce, aw_bits, pasid)) {
        error_report_once("%s: detected IOVA overflow (iova=0x%" PRIx64 ","
                          "pasid=0x%" PRIx32 ")", __func__, iova, pasid);
        return -VTD_FR_ADDR_BEYOND_MGAW;
    }

    /* FIXME: what is the Atomics request here? */
    access_right_check = is_write ? VTD_SL_W : VTD_SL_R;

    while (true) {
        offset = vtd_iova_level_offset(iova, level);
        slpte = vtd_get_slpte(addr, offset);

        if (slpte == (uint64_t)-1) {
            error_report_once("%s: detected read error on DMAR slpte "
                              "(iova=0x%" PRIx64 ", pasid=0x%" PRIx32 ")",
                              __func__, iova, pasid);
            if (level == vtd_get_iova_level(s, ce, pasid)) {
                /* Invalid programming of context-entry */
                return -VTD_FR_CONTEXT_ENTRY_INV;
            } else {
                return -VTD_FR_PAGING_ENTRY_INV;
            }
        }
        *reads = (*reads) && (slpte & VTD_SL_R);
        *writes = (*writes) && (slpte & VTD_SL_W);
        if (!(slpte & access_right_check)) {
            error_report_once("%s: detected slpte permission error "
                              "(iova=0x%" PRIx64 ", level=0x%" PRIx32 ", "
                              "slpte=0x%" PRIx64 ", write=%d, pasid=0x%"
                              PRIx32 ")", __func__, iova, level,
                              slpte, is_write, pasid);
            return is_write ? -VTD_FR_WRITE : -VTD_FR_READ;
        }
        if (vtd_slpte_nonzero_rsvd(slpte, level)) {
            error_report_once("%s: detected splte reserve non-zero "
                              "iova=0x%" PRIx64 ", level=0x%" PRIx32
                              "slpte=0x%" PRIx64 ", pasid=0x%" PRIX32 ")",
                              __func__, iova, level, slpte, pasid);
            return -VTD_FR_PAGING_ENTRY_RSVD;
        }

        if (vtd_is_last_slpte(slpte, level)) {
            *slptep = slpte;
            *slpte_level = level;
            break;
        }
        addr = vtd_get_slpte_addr(slpte, aw_bits);
        level--;
    }

    xlat = vtd_get_slpte_addr(*slptep, aw_bits);
    size = ~vtd_slpt_level_page_mask(level) + 1;

    /*
     * From VT-d spec 3.14: Untranslated requests and translation
     * requests that result in an address in the interrupt range will be
     * blocked with condition code LGN.4 or SGN.8.
     */
    if ((xlat > VTD_INTERRUPT_ADDR_LAST ||
         xlat + size - 1 < VTD_INTERRUPT_ADDR_FIRST)) {
        return 0;
    } else {
        error_report_once("%s: xlat address is in interrupt range "
                          "(iova=0x%" PRIx64 ", level=0x%" PRIx32 ", "
                          "slpte=0x%" PRIx64 ", write=%d, "
                          "xlat=0x%" PRIx64 ", size=0x%" PRIx64 ", "
                          "pasid=0x%" PRIx32 ")",
                          __func__, iova, level, slpte, is_write,
                          xlat, size, pasid);
        return s->scalable_mode ? -VTD_FR_SM_INTERRUPT_ADDR :
                                  -VTD_FR_INTERRUPT_ADDR;
    }
}

typedef int (*vtd_page_walk_hook)(IOMMUTLBEvent *event, void *private);

/**
 * Constant information used during page walking
 *
 * @hook_fn: hook func to be called when detected page
 * @private: private data to be passed into hook func
 * @notify_unmap: whether we should notify invalid entries
 * @as: VT-d address space of the device
 * @aw: maximum address width
 * @domain: domain ID of the page walk
 */
typedef struct {
    VTDAddressSpace *as;
    vtd_page_walk_hook hook_fn;
    void *private;
    bool notify_unmap;
    uint8_t aw;
    uint16_t domain_id;
} vtd_page_walk_info;

static int vtd_page_walk_one(IOMMUTLBEvent *event, vtd_page_walk_info *info)
{
    VTDAddressSpace *as = info->as;
    vtd_page_walk_hook hook_fn = info->hook_fn;
    void *private = info->private;
    IOMMUTLBEntry *entry = &event->entry;
    DMAMap target = {
        .iova = entry->iova,
        .size = entry->addr_mask,
        .translated_addr = entry->translated_addr,
        .perm = entry->perm,
    };
    const DMAMap *mapped = iova_tree_find(as->iova_tree, &target);

    if (event->type == IOMMU_NOTIFIER_UNMAP && !info->notify_unmap) {
        trace_vtd_page_walk_one_skip_unmap(entry->iova, entry->addr_mask);
        return 0;
    }

    assert(hook_fn);

    /* Update local IOVA mapped ranges */
    if (event->type == IOMMU_NOTIFIER_MAP) {
        if (mapped) {
            /* If it's exactly the same translation, skip */
            if (!memcmp(mapped, &target, sizeof(target))) {
                trace_vtd_page_walk_one_skip_map(entry->iova, entry->addr_mask,
                                                 entry->translated_addr);
                return 0;
            } else {
                /*
                 * Translation changed.  Normally this should not
                 * happen, but it can happen when with buggy guest
                 * OSes.  Note that there will be a small window that
                 * we don't have map at all.  But that's the best
                 * effort we can do.  The ideal way to emulate this is
                 * atomically modify the PTE to follow what has
                 * changed, but we can't.  One example is that vfio
                 * driver only has VFIO_IOMMU_[UN]MAP_DMA but no
                 * interface to modify a mapping (meanwhile it seems
                 * meaningless to even provide one).  Anyway, let's
                 * mark this as a TODO in case one day we'll have
                 * a better solution.
                 */
                IOMMUAccessFlags cache_perm = entry->perm;
                int ret;

                /* Emulate an UNMAP */
                event->type = IOMMU_NOTIFIER_UNMAP;
                entry->perm = IOMMU_NONE;
                trace_vtd_page_walk_one(info->domain_id,
                                        entry->iova,
                                        entry->translated_addr,
                                        entry->addr_mask,
                                        entry->perm);
                ret = hook_fn(event, private);
                if (ret) {
                    return ret;
                }
                /* Drop any existing mapping */
                iova_tree_remove(as->iova_tree, target);
                /* Recover the correct type */
                event->type = IOMMU_NOTIFIER_MAP;
                entry->perm = cache_perm;
            }
        }
        iova_tree_insert(as->iova_tree, &target);
    } else {
        if (!mapped) {
            /* Skip since we didn't map this range at all */
            trace_vtd_page_walk_one_skip_unmap(entry->iova, entry->addr_mask);
            return 0;
        }
        iova_tree_remove(as->iova_tree, target);
    }

    trace_vtd_page_walk_one(info->domain_id, entry->iova,
                            entry->translated_addr, entry->addr_mask,
                            entry->perm);
    return hook_fn(event, private);
}

/**
 * vtd_page_walk_level - walk over specific level for IOVA range
 *
 * @addr: base GPA addr to start the walk
 * @start: IOVA range start address
 * @end: IOVA range end address (start <= addr < end)
 * @read: whether parent level has read permission
 * @write: whether parent level has write permission
 * @info: constant information for the page walk
 */
static int vtd_page_walk_level(dma_addr_t addr, uint64_t start,
                               uint64_t end, uint32_t level, bool read,
                               bool write, vtd_page_walk_info *info)
{
    bool read_cur, write_cur, entry_valid;
    uint32_t offset;
    uint64_t slpte;
    uint64_t subpage_size, subpage_mask;
    IOMMUTLBEvent event;
    uint64_t iova = start;
    uint64_t iova_next;
    int ret = 0;

    trace_vtd_page_walk_level(addr, level, start, end);

    subpage_size = 1ULL << vtd_slpt_level_shift(level);
    subpage_mask = vtd_slpt_level_page_mask(level);

    while (iova < end) {
        iova_next = (iova & subpage_mask) + subpage_size;

        offset = vtd_iova_level_offset(iova, level);
        slpte = vtd_get_slpte(addr, offset);

        if (slpte == (uint64_t)-1) {
            trace_vtd_page_walk_skip_read(iova, iova_next);
            goto next;
        }

        if (vtd_slpte_nonzero_rsvd(slpte, level)) {
            trace_vtd_page_walk_skip_reserve(iova, iova_next);
            goto next;
        }

        /* Permissions are stacked with parents' */
        read_cur = read && (slpte & VTD_SL_R);
        write_cur = write && (slpte & VTD_SL_W);

        /*
         * As long as we have either read/write permission, this is a
         * valid entry. The rule works for both page entries and page
         * table entries.
         */
        entry_valid = read_cur | write_cur;

        if (!vtd_is_last_slpte(slpte, level) && entry_valid) {
            /*
             * This is a valid PDE (or even bigger than PDE).  We need
             * to walk one further level.
             */
            ret = vtd_page_walk_level(vtd_get_slpte_addr(slpte, info->aw),
                                      iova, MIN(iova_next, end), level - 1,
                                      read_cur, write_cur, info);
        } else {
            /*
             * This means we are either:
             *
             * (1) the real page entry (either 4K page, or huge page)
             * (2) the whole range is invalid
             *
             * In either case, we send an IOTLB notification down.
             */
            event.entry.target_as = &address_space_memory;
            event.entry.iova = iova & subpage_mask;
            event.entry.perm = IOMMU_ACCESS_FLAG(read_cur, write_cur);
            event.entry.addr_mask = ~subpage_mask;
            /* NOTE: this is only meaningful if entry_valid == true */
            event.entry.translated_addr = vtd_get_slpte_addr(slpte, info->aw);
            event.type = event.entry.perm ? IOMMU_NOTIFIER_MAP :
                                            IOMMU_NOTIFIER_UNMAP;
            ret = vtd_page_walk_one(&event, info);
        }

        if (ret < 0) {
            return ret;
        }

next:
        iova = iova_next;
    }

    return 0;
}

/**
 * vtd_page_walk - walk specific IOVA range, and call the hook
 *
 * @s: intel iommu state
 * @ce: context entry to walk upon
 * @start: IOVA address to start the walk
 * @end: IOVA range end address (start <= addr < end)
 * @info: page walking information struct
 */
static int vtd_page_walk(IntelIOMMUState *s, VTDContextEntry *ce,
                         uint64_t start, uint64_t end,
                         vtd_page_walk_info *info,
                         uint32_t pasid)
{
    dma_addr_t addr = vtd_get_iova_pgtbl_base(s, ce, pasid);
    uint32_t level = vtd_get_iova_level(s, ce, pasid);

    if (!vtd_iova_range_check(s, start, ce, info->aw, pasid)) {
        return -VTD_FR_ADDR_BEYOND_MGAW;
    }

    if (!vtd_iova_range_check(s, end, ce, info->aw, pasid)) {
        /* Fix end so that it reaches the maximum */
        end = vtd_iova_limit(s, ce, info->aw, pasid);
    }

    return vtd_page_walk_level(addr, start, end, level, true, true, info);
}

static int vtd_root_entry_rsvd_bits_check(IntelIOMMUState *s,
                                          VTDRootEntry *re)
{
    /* Legacy Mode reserved bits check */
    if (!s->root_scalable &&
        (re->hi || (re->lo & VTD_ROOT_ENTRY_RSVD(s->aw_bits))))
        goto rsvd_err;

    /* Scalable Mode reserved bits check */
    if (s->root_scalable &&
        ((re->lo & VTD_ROOT_ENTRY_RSVD(s->aw_bits)) ||
         (re->hi & VTD_ROOT_ENTRY_RSVD(s->aw_bits))))
        goto rsvd_err;

    return 0;

rsvd_err:
    error_report_once("%s: invalid root entry: hi=0x%"PRIx64
                      ", lo=0x%"PRIx64,
                      __func__, re->hi, re->lo);
    return -VTD_FR_ROOT_ENTRY_RSVD;
}

static inline int vtd_context_entry_rsvd_bits_check(IntelIOMMUState *s,
                                                    VTDContextEntry *ce)
{
    if (!s->root_scalable &&
        (ce->hi & VTD_CONTEXT_ENTRY_RSVD_HI ||
         ce->lo & VTD_CONTEXT_ENTRY_RSVD_LO(s->aw_bits))) {
        error_report_once("%s: invalid context entry: hi=%"PRIx64
                          ", lo=%"PRIx64" (reserved nonzero)",
                          __func__, ce->hi, ce->lo);
        return -VTD_FR_CONTEXT_ENTRY_RSVD;
    }

    if (s->root_scalable &&
        (ce->val[0] & VTD_SM_CONTEXT_ENTRY_RSVD_VAL0(s->aw_bits) ||
         ce->val[1] & VTD_SM_CONTEXT_ENTRY_RSVD_VAL1 ||
         ce->val[2] ||
         ce->val[3])) {
        error_report_once("%s: invalid context entry: val[3]=%"PRIx64
                          ", val[2]=%"PRIx64
                          ", val[1]=%"PRIx64
                          ", val[0]=%"PRIx64" (reserved nonzero)",
                          __func__, ce->val[3], ce->val[2],
                          ce->val[1], ce->val[0]);
        return -VTD_FR_CONTEXT_ENTRY_RSVD;
    }

    return 0;
}

static int vtd_ce_rid2pasid_check(IntelIOMMUState *s,
                                  VTDContextEntry *ce)
{
    VTDPASIDEntry pe;

    /*
     * Make sure in Scalable Mode, a present context entry
     * has valid rid2pasid setting, which includes valid
     * rid2pasid field and corresponding pasid entry setting
     */
    return vtd_ce_get_rid2pasid_entry(s, ce, &pe, PCI_NO_PASID);
}

/* Map a device to its corresponding domain (context-entry) */
static int vtd_dev_to_context_entry(IntelIOMMUState *s, uint8_t bus_num,
                                    uint8_t devfn, VTDContextEntry *ce)
{
    VTDRootEntry re;
    int ret_fr;
    X86IOMMUState *x86_iommu = X86_IOMMU_DEVICE(s);

    ret_fr = vtd_get_root_entry(s, bus_num, &re);
    if (ret_fr) {
        return ret_fr;
    }

    if (!vtd_root_entry_present(s, &re, devfn)) {
        /* Not error - it's okay we don't have root entry. */
        trace_vtd_re_not_present(bus_num);
        return -VTD_FR_ROOT_ENTRY_P;
    }

    ret_fr = vtd_root_entry_rsvd_bits_check(s, &re);
    if (ret_fr) {
        return ret_fr;
    }

    ret_fr = vtd_get_context_entry_from_root(s, &re, devfn, ce);
    if (ret_fr) {
        return ret_fr;
    }

    if (!vtd_ce_present(ce)) {
        /* Not error - it's okay we don't have context entry. */
        trace_vtd_ce_not_present(bus_num, devfn);
        return -VTD_FR_CONTEXT_ENTRY_P;
    }

    ret_fr = vtd_context_entry_rsvd_bits_check(s, ce);
    if (ret_fr) {
        return ret_fr;
    }

    /* Check if the programming of context-entry is valid */
    if (!s->root_scalable &&
        !vtd_is_level_supported(s, vtd_ce_get_level(ce))) {
        error_report_once("%s: invalid context entry: hi=%"PRIx64
                          ", lo=%"PRIx64" (level %d not supported)",
                          __func__, ce->hi, ce->lo,
                          vtd_ce_get_level(ce));
        return -VTD_FR_CONTEXT_ENTRY_INV;
    }

    if (!s->root_scalable) {
        /* Do translation type check */
        if (!vtd_ce_type_check(x86_iommu, ce)) {
            /* Errors dumped in vtd_ce_type_check() */
            return -VTD_FR_CONTEXT_ENTRY_INV;
        }
    } else {
        /*
         * Check if the programming of context-entry.rid2pasid
         * and corresponding pasid setting is valid, and thus
         * avoids to check pasid entry fetching result in future
         * helper function calling.
         */
        ret_fr = vtd_ce_rid2pasid_check(s, ce);
        if (ret_fr) {
            return ret_fr;
        }
    }

    return 0;
}

static int vtd_sync_shadow_page_hook(IOMMUTLBEvent *event,
                                     void *private)
{
    memory_region_notify_iommu(private, 0, *event);
    return 0;
}

static uint16_t vtd_get_domain_id(IntelIOMMUState *s,
                                  VTDContextEntry *ce,
                                  uint32_t pasid)
{
    VTDPASIDEntry pe;

    if (s->root_scalable) {
        vtd_ce_get_rid2pasid_entry(s, ce, &pe, pasid);
        return VTD_SM_PASID_ENTRY_DID(pe.val[1]);
    }

    return VTD_CONTEXT_ENTRY_DID(ce->hi);
}

static int vtd_sync_shadow_page_table_range(VTDAddressSpace *vtd_as,
                                            VTDContextEntry *ce,
                                            hwaddr addr, hwaddr size)
{
    IntelIOMMUState *s = vtd_as->iommu_state;
    vtd_page_walk_info info = {
        .hook_fn = vtd_sync_shadow_page_hook,
        .private = (void *)&vtd_as->iommu,
        .notify_unmap = true,
        .aw = s->aw_bits,
        .as = vtd_as,
        .domain_id = vtd_get_domain_id(s, ce, vtd_as->pasid),
    };

    return vtd_page_walk(s, ce, addr, addr + size, &info, vtd_as->pasid);
}

static int vtd_address_space_sync(VTDAddressSpace *vtd_as)
{
    int ret;
    VTDContextEntry ce;
    IOMMUNotifier *n;

    /* If no MAP notifier registered, we simply invalidate all the cache */
    if (!vtd_as_has_map_notifier(vtd_as)) {
        IOMMU_NOTIFIER_FOREACH(n, &vtd_as->iommu) {
            memory_region_unmap_iommu_notifier_range(n);
        }
        return 0;
    }

    ret = vtd_dev_to_context_entry(vtd_as->iommu_state,
                                   pci_bus_num(vtd_as->bus),
                                   vtd_as->devfn, &ce);
    if (ret) {
        if (ret == -VTD_FR_CONTEXT_ENTRY_P) {
            /*
             * It's a valid scenario to have a context entry that is
             * not present.  For example, when a device is removed
             * from an existing domain then the context entry will be
             * zeroed by the guest before it was put into another
             * domain.  When this happens, instead of synchronizing
             * the shadow pages we should invalidate all existing
             * mappings and notify the backends.
             */
            IOMMU_NOTIFIER_FOREACH(n, &vtd_as->iommu) {
                vtd_address_space_unmap(vtd_as, n);
            }
            ret = 0;
        }
        return ret;
    }

    return vtd_sync_shadow_page_table_range(vtd_as, &ce, 0, UINT64_MAX);
}

static bool vtd_pe_pgtt_is_pt(VTDPASIDEntry *pe)
{
    return (VTD_PE_GET_TYPE(pe) == VTD_SM_PASID_ENTRY_PT);
}

/* check if pgtt is first stage translation */
static bool vtd_pe_pgtt_is_flt(VTDPASIDEntry *pe)
{
    return (VTD_PE_GET_TYPE(pe) == VTD_SM_PASID_ENTRY_FLT);
}

/*
 * Check if specific device is configured to bypass address
 * translation for DMA requests. In Scalable Mode, bypass
 * 1st-level translation or 2nd-level translation, it depends
 * on PGTT setting.
 */
static bool vtd_dev_pt_enabled(IntelIOMMUState *s, VTDContextEntry *ce,
                               uint32_t pasid)
{
    VTDPASIDEntry pe;
    int ret;

    if (s->root_scalable) {
        ret = vtd_ce_get_rid2pasid_entry(s, ce, &pe, pasid);
        if (ret) {
            /*
             * This error is guest triggerable. We should assumt PT
             * not enabled for safety.
             */
            return false;
        }
        return vtd_pe_pgtt_is_pt(&pe);
    }

    return (vtd_ce_get_type(ce) == VTD_CONTEXT_TT_PASS_THROUGH);

}

static bool vtd_as_pt_enabled(VTDAddressSpace *as)
{
    IntelIOMMUState *s;
    VTDContextEntry ce;

    assert(as);

    s = as->iommu_state;
    if (vtd_dev_to_context_entry(s, pci_bus_num(as->bus), as->devfn,
                                 &ce)) {
        /*
         * Possibly failed to parse the context entry for some reason
         * (e.g., during init, or any guest configuration errors on
         * context entries). We should assume PT not enabled for
         * safety.
         */
        return false;
    }

    return vtd_dev_pt_enabled(s, &ce, as->pasid);
}

/* Return whether the device is using IOMMU translation. */
static bool vtd_switch_address_space(VTDAddressSpace *as)
{
    bool use_iommu, pt;
    /* Whether we need to take the BQL on our own */
    bool take_bql = !qemu_mutex_iothread_locked();

    assert(as);

    use_iommu = as->iommu_state->dmar_enabled && !vtd_as_pt_enabled(as);
    pt = as->iommu_state->dmar_enabled && vtd_as_pt_enabled(as);

    trace_vtd_switch_address_space(pci_bus_num(as->bus),
                                   VTD_PCI_SLOT(as->devfn),
                                   VTD_PCI_FUNC(as->devfn),
                                   use_iommu);

    /*
     * It's possible that we reach here without BQL, e.g., when called
     * from vtd_pt_enable_fast_path(). However the memory APIs need
     * it. We'd better make sure we have had it already, or, take it.
     */
    if (take_bql) {
        qemu_mutex_lock_iothread();
    }

    /* Turn off first then on the other */
    if (use_iommu) {
        memory_region_set_enabled(&as->nodmar, false);
        memory_region_set_enabled(MEMORY_REGION(&as->iommu), true);
        /*
         * vt-d spec v3.4 3.14:
         *
         * """
         * Requests-with-PASID with input address in range 0xFEEx_xxxx
         * are translated normally like any other request-with-PASID
         * through DMA-remapping hardware.
         * """
         *
         * Need to disable ir for as with PASID.
         */
        if (as->pasid != PCI_NO_PASID) {
            memory_region_set_enabled(&as->iommu_ir, false);
        } else {
            memory_region_set_enabled(&as->iommu_ir, true);
        }
    } else {
        memory_region_set_enabled(MEMORY_REGION(&as->iommu), false);
        memory_region_set_enabled(&as->nodmar, true);
    }

    /*
     * vtd-spec v3.4 3.14:
     *
     * """
     * Requests-with-PASID with input address in range 0xFEEx_xxxx are
     * translated normally like any other request-with-PASID through
     * DMA-remapping hardware. However, if such a request is processed
     * using pass-through translation, it will be blocked as described
     * in the paragraph below.
     *
     * Software must not program paging-structure entries to remap any
     * address to the interrupt address range. Untranslated requests
     * and translation requests that result in an address in the
     * interrupt range will be blocked with condition code LGN.4 or
     * SGN.8.
     * """
     *
     * We enable per as memory region (iommu_ir_fault) for catching
     * the translation for interrupt range through PASID + PT.
     */
    if (pt && as->pasid != PCI_NO_PASID) {
        memory_region_set_enabled(&as->iommu_ir_fault, true);
    } else {
        memory_region_set_enabled(&as->iommu_ir_fault, false);
    }

    if (take_bql) {
        qemu_mutex_unlock_iothread();
    }

    return use_iommu;
}

static void vtd_switch_address_space_all(IntelIOMMUState *s)
{
    VTDAddressSpace *vtd_as;
    GHashTableIter iter;

    g_hash_table_iter_init(&iter, s->vtd_address_spaces);
    while (g_hash_table_iter_next(&iter, NULL, (void **)&vtd_as)) {
        vtd_switch_address_space(vtd_as);
    }
}

static const bool vtd_qualified_faults[] = {
    [VTD_FR_RESERVED] = false,
    [VTD_FR_ROOT_ENTRY_P] = false,
    [VTD_FR_CONTEXT_ENTRY_P] = true,
    [VTD_FR_CONTEXT_ENTRY_INV] = true,
    [VTD_FR_ADDR_BEYOND_MGAW] = true,
    [VTD_FR_WRITE] = true,
    [VTD_FR_READ] = true,
    [VTD_FR_PAGING_ENTRY_INV] = true,
    [VTD_FR_ROOT_TABLE_INV] = false,
    [VTD_FR_CONTEXT_TABLE_INV] = false,
    [VTD_FR_INTERRUPT_ADDR] = true,
    [VTD_FR_ROOT_ENTRY_RSVD] = false,
    [VTD_FR_PAGING_ENTRY_RSVD] = true,
    [VTD_FR_CONTEXT_ENTRY_TT] = true,
    [VTD_FR_PASID_DIR_ACCESS_ERR] = false,
    [VTD_FR_PASID_DIR_ENTRY_P] = true,
    [VTD_FR_PASID_TABLE_ACCESS_ERR] = false,
    [VTD_FR_PASID_ENTRY_P] = true,
    [VTD_FR_PASID_TABLE_ENTRY_INV] = true,
    [VTD_FR_SM_INTERRUPT_ADDR] = true,
    [VTD_FR_MAX] = false,
};

/* To see if a fault condition is "qualified", which is reported to software
 * only if the FPD field in the context-entry used to process the faulting
 * request is 0.
 */
static inline bool vtd_is_qualified_fault(VTDFaultReason fault)
{
    return vtd_qualified_faults[fault];
}

static inline bool vtd_is_interrupt_addr(hwaddr addr)
{
    return VTD_INTERRUPT_ADDR_FIRST <= addr && addr <= VTD_INTERRUPT_ADDR_LAST;
}

static gboolean vtd_find_as_by_sid(gpointer key, gpointer value,
                                   gpointer user_data)
{
    struct vtd_as_key *as_key = (struct vtd_as_key *)key;
    uint16_t target_sid = *(uint16_t *)user_data;
    uint16_t sid = PCI_BUILD_BDF(pci_bus_num(as_key->bus), as_key->devfn);
    return sid == target_sid;
}

static VTDAddressSpace *vtd_get_as_by_sid(IntelIOMMUState *s, uint16_t sid)
{
    uint8_t bus_num = PCI_BUS_NUM(sid);
    VTDAddressSpace *vtd_as = s->vtd_as_cache[bus_num];

    if (vtd_as &&
        (sid == PCI_BUILD_BDF(pci_bus_num(vtd_as->bus), vtd_as->devfn))) {
        return vtd_as;
    }

    vtd_as = g_hash_table_find(s->vtd_address_spaces, vtd_find_as_by_sid, &sid);
    s->vtd_as_cache[bus_num] = vtd_as;

    return vtd_as;
}

static void vtd_pt_enable_fast_path(IntelIOMMUState *s, uint16_t source_id)
{
    VTDAddressSpace *vtd_as;
    bool success = false;

    vtd_as = vtd_get_as_by_sid(s, source_id);
    if (!vtd_as) {
        goto out;
    }

    if (vtd_switch_address_space(vtd_as) == false) {
        /* We switched off IOMMU region successfully. */
        success = true;
    }

out:
    trace_vtd_pt_enable_fast_path(source_id, success);
}

/* The shift of an addr for a certain level of paging structure */
static inline uint32_t vtd_flpt_level_shift(uint32_t level)
{
    assert(level != 0);
    return VTD_PAGE_SHIFT_4K + (level - 1) * VTD_FL_LEVEL_BITS;
}

static inline uint64_t vtd_flpt_level_page_mask(uint32_t level)
{
    return ~((1ULL << vtd_flpt_level_shift(level)) - 1);
}

/*
 * Given an iova and the level of paging structure, return the offset
 * of current level.
 */
static inline uint32_t vtd_iova_fl_level_offset(uint64_t iova, uint32_t level)
{
    return (iova >> vtd_flpt_level_shift(level)) &
            ((1ULL << VTD_FL_LEVEL_BITS) - 1);
}

/* Get the content of a flpte located in @base_addr[@index] */
static uint64_t vtd_get_flpte(dma_addr_t base_addr, uint32_t index)
{
    uint64_t flpte;

    assert(index < VTD_FL_PT_ENTRY_NR);

    if (dma_memory_read(&address_space_memory,
                        base_addr + index * sizeof(flpte), &flpte,
                        sizeof(flpte), MEMTXATTRS_UNSPECIFIED)) {
        flpte = (uint64_t)-1;
        return flpte;
    }
    flpte = le64_to_cpu(flpte);
    return flpte;
}

static inline bool vtd_flpte_present(uint64_t flpte)
{
    return !!(flpte & 0x1);
}

/* Whether the pte indicates the address of the page frame */
static inline bool vtd_is_last_flpte(uint64_t flpte, uint32_t level)
{
    return level == VTD_FL_PT_LEVEL || (flpte & VTD_FL_PT_PAGE_SIZE_MASK);
}

static inline uint64_t vtd_get_flpte_addr(uint64_t flpte, uint8_t aw)
{
    return flpte & VTD_FL_PT_BASE_ADDR_MASK(aw);
}

static int vtd_flt_page_walk_level(dma_addr_t addr,
                                   uint64_t start, uint64_t end,
                                   uint32_t level, vtd_page_walk_info *info)
{
    bool read, write;
    uint32_t offset;
    uint64_t flpte;
    uint64_t iova = start;
    uint64_t iova_next;
    uint64_t subpage_size, subpage_mask;
    int ret = 0;
    IOMMUTLBEvent event;
    IOMMUNotifier *n;
    IOMMUMemoryRegion *iommu = (IOMMUMemoryRegion *)info->private;

    IOMMU_NOTIFIER_FOREACH(n, iommu) {
        if (n->iommu_idx == 0) {
            break;
        }
    }

    subpage_size = 1ULL << vtd_flpt_level_shift(level);
    subpage_mask = vtd_flpt_level_page_mask(level);

    while (iova < end) {
        iova_next = (iova & subpage_mask) + subpage_size;

        offset = vtd_iova_fl_level_offset(iova, level);
        flpte = vtd_get_flpte(addr, offset);

        if (flpte == (uint64_t)-1) {
            goto next;
        }

        if (vtd_flpte_present(flpte)) {
            read = true;
            write = flpte & VTD_FL_RW_MASK;
        } else {
            read = false;
            write = false;
        }

        if (!vtd_is_last_flpte(flpte, level) && vtd_flpte_present(flpte)) {
            addr = vtd_get_flpte_addr(flpte, info->aw);
            level--;
            ret = vtd_flt_page_walk_level(addr, iova, MIN(iova_next, end),
                                          level, info);
        } else {
            event.entry.target_as = &address_space_memory;
            event.entry.iova = ((iova & subpage_mask) < n->start) ?
                               n->start : iova & subpage_mask;
            event.entry.perm = IOMMU_ACCESS_FLAG(read, write);
            event.entry.addr_mask =
                    ((event.entry.iova + event.entry.addr_mask) > n->end) ?
                    (n->end - event.entry.iova) : ~subpage_mask;
            event.entry.translated_addr = vtd_get_flpte_addr(flpte, info->aw);
            event.type = event.entry.perm ? IOMMU_NOTIFIER_MAP :
                                            IOMMU_NOTIFIER_UNMAP |
                                            IOMMU_NOTIFIER_DEVIOTLB_UNMAP;
            ret = vtd_page_walk_one(&event, info);
        }

        if (ret < 0) {
            return ret;
        }

next:
        iova = iova_next;
    }

    return 0;
}

static int vtd_flt_page_walk(IntelIOMMUState *s, VTDContextEntry *ce,
                             uint64_t start, uint64_t end,
                             vtd_page_walk_info *info, uint32_t pasid)
{
    VTDPASIDEntry pe;
    dma_addr_t addr;
    uint32_t level;
    int ret;

    ret = vtd_ce_get_rid2pasid_entry(s, ce, &pe, pasid);
    if (ret) {
        return ret;
    }

    addr = vtd_pe_get_flpt_base(&pe);
    level = vtd_pe_get_flpt_level(&pe);

    if (!vtd_iova_range_check(s, start, ce, info->aw, pasid)) {
        return -VTD_FR_ADDR_BEYOND_MGAW;
    }

    if (!vtd_iova_range_check(s, end, ce, info->aw, pasid)) {
        /* Fix end so that it reaches the maximum */
        end = vtd_iova_limit(s, ce, info->aw, pasid);
    }

    return vtd_flt_page_walk_level(addr, start, end, level, info);
}

static int vtd_sync_flt_range(VTDAddressSpace *vtd_as,
                              VTDContextEntry *ce,
                              hwaddr addr, hwaddr size)
{
    IntelIOMMUState *s = vtd_as->iommu_state;
    vtd_page_walk_info info = {
        .hook_fn = vtd_sync_shadow_page_hook,
        .private = (void *)&vtd_as->iommu,
        .notify_unmap = true,
        .aw = s->aw_bits,
        .as = vtd_as,
        .domain_id = vtd_get_domain_id(s, ce, vtd_as->pasid),
    };

    return vtd_flt_page_walk(s, ce, addr, addr + size, &info, vtd_as->pasid);
}

/*
 * Given the @iova, get relevant @flptep. @flpte_level will be the last level
 * of the translation, can be used for deciding the size of large page.
 */
static int vtd_iova_to_flpte(VTDPASIDEntry *pe, uint64_t iova, bool is_write,
                             uint64_t *flptep, uint32_t *flpte_level,
                             bool *reads, bool *writes, uint8_t aw_bits)
{
    dma_addr_t addr = vtd_pe_get_flpt_base(pe);
    uint32_t level = vtd_pe_get_flpt_level(pe);
    uint32_t offset;
    uint64_t flpte;

    while (true) {
        offset = vtd_iova_fl_level_offset(iova, level);
        flpte = vtd_get_flpte(addr, offset);
        if (flpte == (uint64_t)-1) {
            if (level == VTD_PE_GET_LEVEL(pe)) {
                /* Invalid programming of context-entry */
                return -VTD_FR_CONTEXT_ENTRY_INV;
            } else {
                return -VTD_FR_PAGING_ENTRY_INV;
            }
        }

        if (!vtd_flpte_present(flpte)) {
            *reads = false;
            *writes = false;
            return -VTD_FR_PAGING_ENTRY_INV;
        }

        *reads = true;
        *writes = (*writes) && (flpte & VTD_FL_RW_MASK);
        if (is_write && !(flpte & VTD_FL_RW_MASK)) {
            return -VTD_FR_WRITE;
        }

        if (vtd_is_last_flpte(flpte, level)) {
            *flptep = flpte;
            *flpte_level = level;
            return 0;
        }

        addr = vtd_get_flpte_addr(flpte, aw_bits);
        level--;
    }
}

static void vtd_report_fault(IntelIOMMUState *s,
                             int err, bool is_fpd_set,
                             uint16_t source_id,
                             hwaddr addr,
                             bool is_write,
                             bool is_pasid,
                             uint32_t pasid)
{
    if (is_fpd_set && vtd_is_qualified_fault(err)) {
        trace_vtd_fault_disabled();
    } else {
        vtd_report_dmar_fault(s, source_id, addr, err, is_write,
                              is_pasid, pasid);
    }
}

static uint64_t vtd_get_piotlb_gfn(hwaddr addr, uint32_t level)
{
    return (addr & vtd_flpt_level_page_mask(level)) >> VTD_PAGE_SHIFT_4K;
}

static int vtd_get_piotlb_key(char *key, int key_size, uint64_t gfn,
                              uint32_t pasid, uint32_t level,
                              uint16_t source_id)
{
    return snprintf(key, key_size,
                    "rsv%010dsid%06dpasid%010dgfn%017lldlevel%01d",
                    0, source_id, pasid, (unsigned long long int)gfn, level);
}

static VTDIOTLBEntry *vtd_lookup_piotlb(IntelIOMMUState *s, uint32_t pasid,
                                        hwaddr addr, uint16_t source_id)
{
    VTDIOTLBEntry *entry;
    char key[64];
    int level;

    for (level = VTD_SL_PT_LEVEL; level < VTD_SL_PML4_LEVEL; level++) {
        vtd_get_piotlb_key(&key[0], 64, vtd_get_piotlb_gfn(addr, level),
                           pasid, level, source_id);
        entry = g_hash_table_lookup(s->p_iotlb, &key[0]);
        if (entry) {
            goto out;
        }
    }

out:
    return entry;
}

static void vtd_update_piotlb(IntelIOMMUState *s, uint32_t pasid,
                              uint16_t domain_id, hwaddr addr, uint64_t flpte,
                              uint8_t access_flags, uint32_t level,
                              uint16_t source_id)
{
    VTDIOTLBEntry *entry = g_malloc(sizeof(*entry));
    char *key = g_malloc(64);
    uint64_t gfn = vtd_get_piotlb_gfn(addr, level);

    if (g_hash_table_size(s->p_iotlb) >= VTD_PASID_IOTLB_MAX_SIZE) {
        vtd_reset_piotlb(s);
    }

    entry->gfn = gfn;
    entry->domain_id = domain_id;
    entry->pte = flpte;
    entry->pasid = pasid;
    entry->access_flags = access_flags;
    entry->mask = vtd_flpt_level_page_mask(level);
    vtd_get_piotlb_key(key, 64, gfn, pasid, level, source_id);
    g_hash_table_replace(s->p_iotlb, key, entry);
}

/*
 * Map dev to pasid-entry then do a paging-structures walk to do a iommu
 * translation.
 *
 * Called from RCU critical section.
 *
 * @vtd_as: The untranslated address space
 * @bus_num: The bus number
 * @devfn: The devfn, which is the  combined of device and function number
 * @is_write: The access is a write operation
 * @entry: IOMMUTLBEntry that contain the addr to be translated and result
 *
 * Returns true if translation is successful, otherwise false.
 */
static bool vtd_do_iommu_fl_translate(VTDAddressSpace *vtd_as, PCIBus *bus,
                                      uint8_t devfn, hwaddr addr, bool is_write,
                                      IOMMUTLBEntry *entry)
{
    IntelIOMMUState *s = vtd_as->iommu_state;
    VTDContextEntry ce;
    VTDPASIDEntry pe;
    uint8_t bus_num = pci_bus_num(bus);
    uint64_t flpte, page_mask;
    uint32_t level;
    uint16_t source_id = PCI_BUILD_BDF(bus_num, devfn);
    int ret;
    bool is_fpd_set = false;
    bool reads = true;
    bool writes = true;
    uint8_t access_flags;
    uint32_t pasid;
    VTDIOTLBEntry *piotlb_entry;

    /*
     * We have standalone memory region for interrupt addresses, we
     * should never receive translation requests in this region.
     */
    assert(!vtd_is_interrupt_addr(addr));

    ret = vtd_dev_to_context_entry(s, pci_bus_num(bus), devfn, &ce);
    if (ret) {
        error_report_once("%s: detected translation failure 1 "
                          "(dev=%02x:%02x:%02x, iova=0x%" PRIx64 ")",
                          __func__, pci_bus_num(bus),
                          VTD_PCI_SLOT(devfn),
                          VTD_PCI_FUNC(devfn),
                          addr);
        return false;
    }

    /* For emulated device IOVA translation, use RID2PASID. */
    if (vtd_dev_get_rid2pasid(s, pci_bus_num(bus), devfn, &pasid)) {
        error_report_once("%s: detected translation failure 2 "
                          "(dev=%02x:%02x:%02x, iova=0x%" PRIx64 ")",
                          __func__, pci_bus_num(bus),
                          VTD_PCI_SLOT(devfn),
                          VTD_PCI_FUNC(devfn),
                          addr);
        return false;
    }

    /* Try to fetch flpte form IOTLB */
    piotlb_entry = vtd_lookup_piotlb(s, pasid, addr, source_id);
    if (piotlb_entry) {
        trace_vtd_piotlb_page_hit(source_id, pasid, addr, piotlb_entry->pte,
                                  piotlb_entry->domain_id);
        flpte = piotlb_entry->pte;
        access_flags = piotlb_entry->access_flags;
        page_mask = piotlb_entry->mask;
        goto out;
    }

    vtd_iommu_lock(s);

    ret = vtd_ce_get_rid2pasid_entry(s, &ce, &pe, PCI_NO_PASID);
    is_fpd_set = pe.val[0] & VTD_PASID_ENTRY_FPD;
    if (ret) {
        vtd_report_fault(s, -ret, is_fpd_set, source_id, addr, is_write,
                         false, PCI_NO_PASID);
        goto error;
    }

    /*
     * We don't need to translate for pass-through context entries.
     * Also, let's ignore IOTLB caching as well for PT devices.
     */
    if (VTD_PE_GET_TYPE(&pe) == VTD_SM_PASID_ENTRY_PT) {
        entry->iova = addr & VTD_PAGE_MASK_4K;
        entry->translated_addr = entry->iova;
        entry->addr_mask = ~VTD_PAGE_MASK_4K;
        entry->perm = IOMMU_RW;
        vtd_iommu_unlock(s);
        return true;
    }

    ret = vtd_iova_to_flpte(&pe, addr, is_write, &flpte, &level,
                            &reads, &writes, s->aw_bits);
    if (ret) {
        vtd_report_fault(s, -ret, is_fpd_set, source_id, addr, is_write,
                         false, PCI_NO_PASID);
        goto error;
    }

    page_mask = vtd_flpt_level_page_mask(level);
    access_flags = IOMMU_ACCESS_FLAG(reads, writes);

    vtd_update_piotlb(s, pasid, vtd_pe_get_domain_id(&pe), addr, flpte,
                      access_flags, level, source_id);
out:
    vtd_iommu_unlock(s);

    entry->iova = addr & page_mask;
    entry->translated_addr = vtd_get_flpte_addr(flpte, s->aw_bits) & page_mask;
    entry->addr_mask = ~page_mask;
    entry->perm = access_flags;
    return true;

error:
    vtd_iommu_unlock(s);
    entry->iova = 0;
    entry->translated_addr = 0;
    entry->addr_mask = 0;
    entry->perm = IOMMU_NONE;
    return false;
}

/* Map dev to context-entry then do a paging-structures walk to do a iommu
 * translation.
 *
 * Called from RCU critical section.
 *
 * @bus_num: The bus number
 * @devfn: The devfn, which is the  combined of device and function number
 * @is_write: The access is a write operation
 * @entry: IOMMUTLBEntry that contain the addr to be translated and result
 *
 * Returns true if translation is successful, otherwise false.
 */
static bool vtd_do_iommu_translate(VTDAddressSpace *vtd_as, PCIBus *bus,
                                   uint8_t devfn, hwaddr addr, bool is_write,
                                   IOMMUTLBEntry *entry)
{
    IntelIOMMUState *s = vtd_as->iommu_state;
    VTDContextEntry ce;
    uint8_t bus_num = pci_bus_num(bus);
    VTDContextCacheEntry *cc_entry;
    uint64_t slpte, page_mask;
    uint32_t level, pasid = vtd_as->pasid;
    uint16_t source_id = PCI_BUILD_BDF(bus_num, devfn);
    int ret_fr;
    bool is_fpd_set = false;
    bool reads = true;
    bool writes = true;
    uint8_t access_flags;
    bool rid2pasid = (pasid == PCI_NO_PASID) && s->root_scalable;
    VTDIOTLBEntry *iotlb_entry;

    /*
     * We have standalone memory region for interrupt addresses, we
     * should never receive translation requests in this region.
     */
    assert(!vtd_is_interrupt_addr(addr));

    vtd_iommu_lock(s);

    cc_entry = &vtd_as->context_cache_entry;

    /* Try to fetch slpte form IOTLB, we don't need RID2PASID logic */
    if (!rid2pasid) {
        iotlb_entry = vtd_lookup_iotlb(s, source_id, pasid, addr);
        if (iotlb_entry) {
            trace_vtd_iotlb_page_hit(source_id, addr, iotlb_entry->pte,
                                     iotlb_entry->domain_id);
            slpte = iotlb_entry->pte;
            access_flags = iotlb_entry->access_flags;
            page_mask = iotlb_entry->mask;
            goto out;
        }
    }

    /* Try to fetch context-entry from cache first */
    if (cc_entry->context_cache_gen == s->context_cache_gen) {
        trace_vtd_iotlb_cc_hit(bus_num, devfn, cc_entry->context_entry.hi,
                               cc_entry->context_entry.lo,
                               cc_entry->context_cache_gen);
        ce = cc_entry->context_entry;
        is_fpd_set = ce.lo & VTD_CONTEXT_ENTRY_FPD;
        if (!is_fpd_set && s->root_scalable) {
            ret_fr = vtd_ce_get_pasid_fpd(s, &ce, &is_fpd_set, pasid);
            if (ret_fr) {
                vtd_report_fault(s, -ret_fr, is_fpd_set,
                                 source_id, addr, is_write,
                                 false, 0);
                goto error;
            }
        }
    } else {
        ret_fr = vtd_dev_to_context_entry(s, bus_num, devfn, &ce);
        is_fpd_set = ce.lo & VTD_CONTEXT_ENTRY_FPD;
        if (!ret_fr && !is_fpd_set && s->root_scalable) {
            ret_fr = vtd_ce_get_pasid_fpd(s, &ce, &is_fpd_set, pasid);
        }
        if (ret_fr) {
            vtd_report_fault(s, -ret_fr, is_fpd_set,
                             source_id, addr, is_write,
                             false, 0);
            goto error;
        }
        /* Update context-cache */
        trace_vtd_iotlb_cc_update(bus_num, devfn, ce.hi, ce.lo,
                                  cc_entry->context_cache_gen,
                                  s->context_cache_gen);
        cc_entry->context_entry = ce;
        cc_entry->context_cache_gen = s->context_cache_gen;
    }

    if (rid2pasid) {
        pasid = VTD_CE_GET_RID2PASID(&ce);
    }

    /*
     * We don't need to translate for pass-through context entries.
     * Also, let's ignore IOTLB caching as well for PT devices.
     */
    if (vtd_dev_pt_enabled(s, &ce, pasid)) {
        entry->iova = addr & VTD_PAGE_MASK_4K;
        entry->translated_addr = entry->iova;
        entry->addr_mask = ~VTD_PAGE_MASK_4K;
        entry->perm = IOMMU_RW;
        trace_vtd_translate_pt(source_id, entry->iova);

        /*
         * When this happens, it means firstly caching-mode is not
         * enabled, and this is the first passthrough translation for
         * the device. Let's enable the fast path for passthrough.
         *
         * When passthrough is disabled again for the device, we can
         * capture it via the context entry invalidation, then the
         * IOMMU region can be swapped back.
         */
        vtd_pt_enable_fast_path(s, source_id);
        vtd_iommu_unlock(s);
        return true;
    }

    /* Try to fetch slpte form IOTLB for RID2PASID slow path */
    if (rid2pasid) {
        iotlb_entry = vtd_lookup_iotlb(s, source_id, pasid, addr);
        if (iotlb_entry) {
            trace_vtd_iotlb_page_hit(source_id, addr, iotlb_entry->pte,
                                     iotlb_entry->domain_id);
            slpte = iotlb_entry->pte;
            access_flags = iotlb_entry->access_flags;
            page_mask = iotlb_entry->mask;
            goto out;
        }
    }

    ret_fr = vtd_iova_to_slpte(s, &ce, addr, is_write, &slpte, &level,
                               &reads, &writes, s->aw_bits, pasid);
    if (ret_fr) {
        vtd_report_fault(s, -ret_fr, is_fpd_set, source_id,
                         addr, is_write, pasid != PCI_NO_PASID, pasid);
        goto error;
    }

    page_mask = vtd_slpt_level_page_mask(level);
    access_flags = IOMMU_ACCESS_FLAG(reads, writes);
    vtd_update_iotlb(s, source_id, vtd_get_domain_id(s, &ce, pasid),
                     addr, slpte, access_flags, level, pasid);
out:
    vtd_iommu_unlock(s);
    entry->iova = addr & page_mask;
    entry->translated_addr = vtd_get_slpte_addr(slpte, s->aw_bits) & page_mask;
    entry->addr_mask = ~page_mask;
    entry->perm = access_flags;
    return true;

error:
    vtd_iommu_unlock(s);
    entry->iova = 0;
    entry->translated_addr = 0;
    entry->addr_mask = 0;
    entry->perm = IOMMU_NONE;
    return false;
}

static void vtd_root_table_setup(IntelIOMMUState *s)
{
    s->root = vtd_get_quad_raw(s, DMAR_RTADDR_REG);
    s->root &= VTD_RTADDR_ADDR_MASK(s->aw_bits);

    vtd_update_scalable_state(s);

    trace_vtd_reg_dmar_root(s->root, s->root_scalable);
}

static void vtd_iec_notify_all(IntelIOMMUState *s, bool global,
                               uint32_t index, uint32_t mask)
{
    x86_iommu_iec_notify_all(X86_IOMMU_DEVICE(s), global, index, mask);
}

static void vtd_interrupt_remap_table_setup(IntelIOMMUState *s)
{
    uint64_t value = 0;
    value = vtd_get_quad_raw(s, DMAR_IRTA_REG);
    s->intr_size = 1UL << ((value & VTD_IRTA_SIZE_MASK) + 1);
    s->intr_root = value & VTD_IRTA_ADDR_MASK(s->aw_bits);
    s->intr_eime = value & VTD_IRTA_EIME;

    /* Notify global invalidation */
    vtd_iec_notify_all(s, true, 0, 0);

    trace_vtd_reg_ir_root(s->intr_root, s->intr_size);
}

static void vtd_iommu_replay_all(IntelIOMMUState *s)
{
    VTDAddressSpace *vtd_as;

    QLIST_FOREACH(vtd_as, &s->vtd_as_with_notifiers, next) {
        vtd_address_space_sync(vtd_as);
    }
}

static void vtd_context_global_invalidate(IntelIOMMUState *s)
{
    VTDPASIDCacheInfo pc_info = { .error_happened = false, };

    trace_vtd_inv_desc_cc_global();
    /* Protects context cache */
    vtd_iommu_lock(s);
    s->context_cache_gen++;
    if (s->context_cache_gen == VTD_CONTEXT_CACHE_GEN_MAX) {
        vtd_reset_context_cache_locked(s);
    }
    vtd_iommu_unlock(s);
    vtd_address_space_refresh_all(s);
    /*
     * From VT-d spec 6.5.2.1, a global context entry invalidation
     * should be followed by a IOTLB global invalidation, so we should
     * be safe even without this. Hoewever, let's replay the region as
     * well to be safer, and go back here when we need finer tunes for
     * VT-d emulation codes.
     */
    vtd_iommu_replay_all(s);

    pc_info.type = VTD_PASID_CACHE_GLOBAL_INV;
    vtd_pasid_cache_sync(s, &pc_info);
}

static bool iommufd_listener_skipped_section(VTDIOASContainer *container,
                                             MemoryRegionSection *section)
{
    return !memory_region_is_ram(section->mr) ||
           memory_region_is_protected(section->mr) ||
           /*
            * Sizing an enabled 64-bit BAR can cause spurious mappings to
            * addresses in the upper part of the 64-bit address space.  These
            * are never accessed by the CPU and beyond the address width of
            * some IOMMU hardware.  TODO: VFIO should tell us the IOMMU width.
            */
           section->offset_within_address_space & (1ULL << 63) ||
           (container->errata && section->readonly);
}

static void iommufd_listener_region_add_s2domain(MemoryListener *listener,
                                                 MemoryRegionSection *section)
{
    VTDIOASContainer *container = container_of(listener,
                                               VTDIOASContainer, listener);
    IOMMUFDBackend *iommufd = container->iommufd;
    uint32_t ioas_id = container->ioas_id;
    hwaddr iova;
    Int128 llend, llsize;
    void *vaddr;
    Error *err = NULL;
    int ret;

    if (iommufd_listener_skipped_section(container, section)) {
        return;
    }
    iova = REAL_HOST_PAGE_ALIGN(section->offset_within_address_space);
    llend = int128_make64(section->offset_within_address_space);
    llend = int128_add(llend, section->size);
    llend = int128_and(llend, int128_exts64(qemu_real_host_page_mask()));
    llsize = int128_sub(llend, int128_make64(iova));
    vaddr = memory_region_get_ram_ptr(section->mr) +
            section->offset_within_region +
            (iova - section->offset_within_address_space);

    memory_region_ref(section->mr);

    ret = iommufd_backend_map_dma(iommufd, ioas_id, iova, int128_get64(llsize),
                                  vaddr, section->readonly);
    if (!ret) {
        return;
    }

    error_setg(&err,
               "iommufd_listener_region_add_s2domain(%p, 0x%"HWADDR_PRIx", "
               "0x%"HWADDR_PRIx", %p) = %d (%s)",
               container, iova, int128_get64(llsize), vaddr, ret,
               strerror(-ret));

    if (memory_region_is_ram_device(section->mr)) {
        /* Allow unexpected mappings not to be fatal for RAM devices */
        error_report_err(err);
        return;
    }

    if (!container->error) {
        error_propagate_prepend(&container->error, err, "Region %s: ",
                                memory_region_name(section->mr));
    } else {
        error_free(err);
    }
}

static void iommufd_listener_region_del_s2domain(MemoryListener *listener,
                                                 MemoryRegionSection *section)
{
    VTDIOASContainer *container = container_of(listener,
                                               VTDIOASContainer, listener);
    IOMMUFDBackend *iommufd = container->iommufd;
    uint32_t ioas_id = container->ioas_id;
    hwaddr iova;
    Int128 llend, llsize;
    int ret;

    if (iommufd_listener_skipped_section(container, section)) {
        return;
    }
    iova = REAL_HOST_PAGE_ALIGN(section->offset_within_address_space);
    llend = int128_make64(section->offset_within_address_space);
    llend = int128_add(llend, section->size);
    llend = int128_and(llend, int128_exts64(qemu_real_host_page_mask()));
    llsize = int128_sub(llend, int128_make64(iova));

    ret = iommufd_backend_unmap_dma(iommufd, ioas_id,
                                    iova, int128_get64(llsize));
    if (ret) {
        error_report("iommufd_listener_region_del_s2domain(%p, "
                     "0x%"HWADDR_PRIx", 0x%"HWADDR_PRIx") = %d (%s)",
                     container, iova, int128_get64(llsize), ret,
                     strerror(-ret));
    }

    memory_region_unref(section->mr);
}

static const MemoryListener iommufd_s2domain_memory_listener = {
    .name = "iommufd_s2domain",
    .priority = 1000,
    .region_add = iommufd_listener_region_add_s2domain,
    .region_del = iommufd_listener_region_del_s2domain,
};

static void vtd_init_fl_hwpt_data(struct iommu_hwpt_vtd_s1 *vtd,
                                  VTDPASIDEntry *pe)
{
    memset(vtd, 0, sizeof(*vtd));

    vtd->flags =  (VTD_SM_PASID_ENTRY_SRE_BIT(pe->val[2]) ?
                                        IOMMU_VTD_S1_SRE : 0) |
                  (VTD_SM_PASID_ENTRY_WPE_BIT(pe->val[2]) ?
                                        IOMMU_VTD_S1_WPE : 0) |
                  (VTD_SM_PASID_ENTRY_EAFE_BIT(pe->val[2]) ?
                                        IOMMU_VTD_S1_EAFE : 0);
    vtd->addr_width = vtd_pe_get_fl_aw(pe);
    vtd->pgtbl_addr = (uint64_t)vtd_pe_get_flpt_base(pe);
}

static int vtd_init_fl_hwpt(IOMMUFDDevice *idev,
                            VTDS2Hwpt *s2_hwpt, VTDHwpt *hwpt,
                            VTDPASIDEntry *pe, Error **errp)
{
    struct iommu_hwpt_vtd_s1 vtd;
    uint32_t hwpt_id, s2_hwptid = s2_hwpt->hwpt_id;
    int ret;

    vtd_init_fl_hwpt_data(&vtd, pe);

    ret = iommufd_backend_alloc_hwpt(idev->iommufd, idev->dev_id,
                                     s2_hwptid, 0, IOMMU_HWPT_DATA_VTD_S1,
                                     sizeof(vtd), &vtd, &hwpt_id);
    if (ret) {
        error_setg(errp, "Failed to allocate stage-1 page table, dev_id %d",
                   idev->dev_id);
        return ret;
    }

    hwpt->hwpt_id = hwpt_id;

    return 0;
}

static void vtd_destroy_fl_hwpt(IOMMUFDDevice *idev, VTDHwpt *hwpt)
{
    iommufd_backend_free_id(idev->iommufd, hwpt->hwpt_id);
}

static VTDS2Hwpt *vtd_ioas_container_get_hwpt(VTDIOASContainer *container,
                                                uint32_t hwpt_id)
{
    VTDS2Hwpt *s2_hwpt;

    QLIST_FOREACH(s2_hwpt, &container->hwpt_list, next) {
        if (s2_hwpt->hwpt_id == hwpt_id) {
            return s2_hwpt;
        }
    }

    s2_hwpt = g_malloc0(sizeof(*s2_hwpt));

    s2_hwpt->hwpt_id = hwpt_id;
    s2_hwpt->container = container;
    QLIST_INSERT_HEAD(&container->hwpt_list, s2_hwpt, next);

    return s2_hwpt;
}

static void vtd_ioas_container_put_hwpt(VTDS2Hwpt *s2_hwpt)
{
    VTDIOASContainer *container = s2_hwpt->container;

    if (s2_hwpt->users) {
        return;
    }

    QLIST_REMOVE(s2_hwpt, next);
    iommufd_backend_free_id(container->iommufd, s2_hwpt->hwpt_id);
    g_free(s2_hwpt);
}

static void vtd_ioas_container_destroy(VTDIOASContainer *container)
{
    if (!QLIST_EMPTY(&container->hwpt_list)) {
        return;
    }

    QLIST_REMOVE(container, next);
    memory_listener_unregister(&container->listener);
    iommufd_backend_free_id(container->iommufd, container->ioas_id);
    g_free(container);
}

static int vtd_device_attach_hwpt(VTDIOMMUFDDevice *vtd_idev, uint32_t pasid,
                                  uint32_t rid_pasid, VTDPASIDEntry *pe,
                                  VTDS2Hwpt *s2_hwpt, VTDHwpt *hwpt,
                                  Error **errp)
{
    IOMMUFDDevice *idev = vtd_idev->idev;
    int ret;

    if (vtd_pe_pgtt_is_flt(pe)) {
        ret = vtd_init_fl_hwpt(vtd_idev->idev, s2_hwpt,
                               hwpt, pe, errp);
        if (ret) {
            return ret;
        }
    } else {
        hwpt->hwpt_id = s2_hwpt->hwpt_id;
    }

    if (pasid == rid_pasid) {
        ret = iommufd_device_attach_hwpt(idev, hwpt->hwpt_id);
    } else {
        ret = iommufd_device_pasid_attach_hwpt(idev, pasid, hwpt->hwpt_id);
    }
    trace_vtd_device_attach_hwpt(idev->dev_id, pasid, hwpt->hwpt_id, ret);
    if (ret) {
        if (vtd_pe_pgtt_is_flt(pe)) {
            vtd_destroy_fl_hwpt(idev, hwpt);
        }
        hwpt->hwpt_id = 0;
        error_setg(errp, "dev_id %d pasid %d failed to attach hwpt %d",
                   idev->dev_id, pasid, hwpt->hwpt_id);
        return ret;
    }

    s2_hwpt->users++;
    hwpt->s2_hwpt = s2_hwpt;

    return 0;
}

static void vtd_device_detach_hwpt(VTDIOMMUFDDevice *vtd_idev, uint32_t pasid,
                                   uint32_t rid_pasid, VTDPASIDEntry *pe,
                                   VTDHwpt *hwpt, Error **errp)
{
    IOMMUFDDevice *idev = vtd_idev->idev;
    int ret;

    if (pasid != rid_pasid) {
        ret = iommufd_device_pasid_detach_hwpt(idev, pasid);
        trace_vtd_device_detach_hwpt(idev->dev_id, pasid, ret);
    } else if (vtd_idev->iommu_state->dmar_enabled) {
        ret = iommufd_device_detach_hwpt(idev);
        trace_vtd_device_detach_hwpt(idev->dev_id, rid_pasid, ret);
    } else {
        ret = iommufd_device_attach_hwpt(idev, idev->def_hwpt_id);
        trace_vtd_device_reattach_def_hwpt(idev->dev_id, rid_pasid,
                                           idev->def_hwpt_id, ret);
    }

    if (ret) {
        error_setg(errp, "dev_id %d pasid %d failed to attach hwpt %d",
                   idev->dev_id, rid_pasid, hwpt->hwpt_id);
    }

    if (vtd_pe_pgtt_is_flt(pe)) {
        vtd_destroy_fl_hwpt(idev, hwpt);
    }

    hwpt->s2_hwpt->users--;
    hwpt->s2_hwpt = NULL;
    hwpt->hwpt_id = 0;
}

static int vtd_device_attach_container(VTDIOMMUFDDevice *vtd_idev,
                                       VTDIOASContainer *container,
                                       uint32_t pasid,
                                       uint32_t rid_pasid,
                                       VTDPASIDEntry *pe,
                                       VTDHwpt *hwpt,
                                       Error **errp)
{
    IOMMUFDDevice *idev = vtd_idev->idev;
    IOMMUFDBackend *iommufd = idev->iommufd;
    VTDS2Hwpt *s2_hwpt;
    uint32_t hwpt_id;
    Error *err = NULL;
    int ret;

    /* try to attach to an existing hwpt in this container */
    QLIST_FOREACH(s2_hwpt, &container->hwpt_list, next) {
        ret = vtd_device_attach_hwpt(vtd_idev, pasid, rid_pasid, pe,
                                     s2_hwpt, hwpt, &err);
        if (ret) {
            const char *msg = error_get_pretty(err);

            trace_vtd_device_fail_attach_existing_hwpt(msg);
            error_free(err);
            err = NULL;
        } else {
            goto found_hwpt;
        }
    }

    ret = iommufd_backend_alloc_hwpt(iommufd, idev->dev_id,
                                     container->ioas_id,
                                     IOMMU_HWPT_ALLOC_NEST_PARENT,
                                     IOMMU_HWPT_DATA_NONE,
                                     0, NULL, &hwpt_id);
    if (ret) {
        error_setg_errno(errp, errno, "error alloc parent hwpt");
        return ret;
    }

    s2_hwpt = vtd_ioas_container_get_hwpt(container, hwpt_id);

    /* Attach vtd device to a new allocated hwpt within iommufd */
    ret = vtd_device_attach_hwpt(vtd_idev, pasid, rid_pasid, pe,
                                 s2_hwpt, hwpt, &err);
    if (ret) {
        goto err_attach_hwpt;
    }

found_hwpt:
    trace_vtd_device_attach_container(iommufd->fd, idev->dev_id, pasid,
                                      container->ioas_id, hwpt->hwpt_id);
    return 0;

err_attach_hwpt:
    vtd_ioas_container_put_hwpt(s2_hwpt);
    return ret;
}

static void vtd_device_detach_container(VTDIOMMUFDDevice *vtd_idev,
                                        uint32_t pasid,
                                        uint32_t rid_pasid,
                                        VTDPASIDEntry *pe,
                                        VTDHwpt *hwpt,
                                        Error **errp)
{
    IOMMUFDDevice *idev = vtd_idev->idev;
    IOMMUFDBackend *iommufd = idev->iommufd;
    VTDS2Hwpt *s2_hwpt = hwpt->s2_hwpt;

    trace_vtd_device_detach_container(iommufd->fd, idev->dev_id, pasid);
    vtd_device_detach_hwpt(vtd_idev, pasid, rid_pasid, pe, hwpt, errp);
    vtd_ioas_container_put_hwpt(s2_hwpt);
}

static int vtd_device_attach_iommufd(VTDIOMMUFDDevice *vtd_idev,
                                     uint32_t pasid,
                                     uint32_t rid_pasid,
                                     VTDPASIDEntry *pe,
                                     VTDHwpt *hwpt,
                                     Error **errp)
{
    IntelIOMMUState *s = vtd_idev->iommu_state;
    VTDIOASContainer *container;
    IOMMUFDBackend *iommufd = vtd_idev->idev->iommufd;
    Error *err = NULL;
    uint32_t ioas_id;
    int ret;

    /* try to attach to an existing container in this space */
    QLIST_FOREACH(container, &s->containers, next) {
        if (container->iommufd != iommufd ||
            container->errata != vtd_idev->errata) {
            continue;
        }

        if (vtd_device_attach_container(vtd_idev, container, pasid,
                                        rid_pasid, pe, hwpt, &err)) {
            const char *msg = error_get_pretty(err);

            trace_vtd_device_fail_attach_existing_container(msg);
            error_free(err);
            err = NULL;
        } else {
            return 0;
        }
    }

    /* Need to allocate a new dedicated container */
    ret = iommufd_backend_alloc_ioas(iommufd, &ioas_id, errp);
    if (ret < 0) {
        return ret;
    }

    trace_vtd_device_alloc_ioas(iommufd->fd, ioas_id);

    container = g_malloc0(sizeof(*container));
    container->iommufd = iommufd;
    container->ioas_id = ioas_id;
    container->errata = vtd_idev->errata;
    QLIST_INIT(&container->hwpt_list);

    if (vtd_device_attach_container(vtd_idev, container, pasid,
                                    rid_pasid, pe, hwpt, errp)) {
        goto err_attach_container;
    }

    container->listener = iommufd_s2domain_memory_listener;
    memory_listener_register(&container->listener, &address_space_memory);

    if (container->error) {
        ret = -1;
        error_propagate_prepend(errp, container->error,
                                "memory listener initialization failed: ");
        goto err_listener_register;
    }

    QLIST_INSERT_HEAD(&s->containers, container, next);

    return 0;

err_listener_register:
    vtd_device_detach_container(vtd_idev, pasid, rid_pasid, pe, hwpt, errp);
err_attach_container:
    iommufd_backend_free_id(iommufd, container->ioas_id);
    g_free(container);
    return ret;
}

static void vtd_device_detach_iommufd(VTDIOMMUFDDevice *vtd_idev,
                                      uint32_t pasid,
                                      uint32_t rid_pasid,
                                      VTDPASIDEntry *pe,
                                      VTDHwpt *hwpt,
                                      Error **errp)
{
    VTDIOASContainer *container = hwpt->s2_hwpt->container;

    vtd_device_detach_container(vtd_idev, pasid, rid_pasid, pe, hwpt, errp);
    vtd_ioas_container_destroy(container);
}

static int vtd_device_attach_pgtbl(VTDIOMMUFDDevice *vtd_idev,
                                   VTDPASIDEntry *pe,
                                   VTDPASIDAddressSpace *vtd_pasid_as,
                                   uint32_t rid_pasid)
{
    /*
     * If pe->gptt != FLT, should be go ahead to do bind as host only
     * accepts guest FLT under nesting. If pe->pgtt==PT, should setup
     * the pasid with GPA page table. Otherwise should return failure.
     */
    if (!vtd_pe_pgtt_is_flt(pe) && !vtd_pe_pgtt_is_pt(pe)) {
        return -EINVAL;
    }

    /* Should fail if the FLPT base is 0 */
    if (vtd_pe_pgtt_is_flt(pe) && !vtd_pe_get_flpt_base(pe)) {
        return -EINVAL;
    }

    return vtd_device_attach_iommufd(vtd_idev, vtd_pasid_as->pasid, rid_pasid,
                                     pe, &vtd_pasid_as->hwpt, &error_abort);
}

static int vtd_device_detach_pgtbl(VTDIOMMUFDDevice *vtd_idev,
                                  VTDPASIDAddressSpace *vtd_pasid_as,
                                  uint32_t rid_pasid)
{
    VTDPASIDEntry *cached_pe = vtd_pasid_as->pasid_cache_entry.cache_filled ?
                       &vtd_pasid_as->pasid_cache_entry.pasid_entry : NULL;

    if (!cached_pe ||
        (!vtd_pe_pgtt_is_flt(cached_pe) && !vtd_pe_pgtt_is_pt(cached_pe))) {
        return 0;
    }

    vtd_device_detach_iommufd(vtd_idev, vtd_pasid_as->pasid, rid_pasid,
                              cached_pe, &vtd_pasid_as->hwpt, &error_abort);

    return 0;
}

static int vtd_dev_get_rid2pasid(IntelIOMMUState *s, uint8_t bus_num,
                                 uint8_t devfn, uint32_t *rid_pasid)
{
    VTDContextEntry ce;
    int ret;

    /*
     * Currently, ECAP.RPS bit is likely to be reported as "Clear".
     * And per VT-d 3.1 spec, it will use PASID #0 as RID2PASID when
     * RPS bit is reported as "Clear".
     */
    if (likely(!(s->ecap & VTD_ECAP_RPS))) {
        *rid_pasid = 0;
        return 0;
    }

    /*
     * In future, to improve performance, could try to fetch context
     * entry from cache firstly.
     */
    ret = vtd_dev_to_context_entry(s, bus_num, devfn, &ce);
    if (!ret) {
        *rid_pasid = VTD_CE_GET_RID2PASID(&ce);
    }

    return ret;
}

/**
 * Caller should hold iommu_lock.
 */
static int vtd_bind_guest_pasid(VTDPASIDAddressSpace *vtd_pasid_as,
                                VTDPASIDEntry *pe, VTDPASIDOp op)
{
    IntelIOMMUState *s = vtd_pasid_as->iommu_state;
    VTDIOMMUFDDevice *vtd_idev;
    uint32_t rid_pasid;
    int devfn = vtd_pasid_as->devfn;
    int ret = -EINVAL;
    struct vtd_as_key key = {
        .bus = vtd_pasid_as->bus,
        .devfn = devfn,
    };

    vtd_idev = g_hash_table_lookup(s->vtd_iommufd_dev, &key);
    if (!vtd_idev || !vtd_idev->idev) {
        /* means no need to go further, e.g. for emulated devices */
        return 0;
    }

    if (vtd_dev_get_rid2pasid(s, pci_bus_num(vtd_pasid_as->bus),
                              devfn, &rid_pasid)) {
        error_report("Unable to get rid_pasid for devfn: %d!", devfn);
        return ret;
    }

    switch (op) {
    case VTD_PASID_UPDATE:
    case VTD_PASID_BIND:
    {
        ret = vtd_device_attach_pgtbl(vtd_idev, pe, vtd_pasid_as, rid_pasid);
        break;
    }
    case VTD_PASID_UNBIND:
    {
        ret = vtd_device_detach_pgtbl(vtd_idev, vtd_pasid_as, rid_pasid);
        break;
    }
    default:
        error_report_once("Unknown VTDPASIDOp!!!\n");
        break;
    }

    return ret;
}

/* Do a context-cache device-selective invalidation.
 * @func_mask: FM field after shifting
 */
static void vtd_context_device_invalidate(IntelIOMMUState *s,
                                          uint16_t source_id,
                                          uint16_t func_mask)
{
    GHashTableIter as_it;
    uint16_t mask;
    VTDAddressSpace *vtd_as;
    uint8_t bus_n, devfn;

    trace_vtd_inv_desc_cc_devices(source_id, func_mask);

    switch (func_mask & 3) {
    case 0:
        mask = 0;   /* No bits in the SID field masked */
        break;
    case 1:
        mask = 4;   /* Mask bit 2 in the SID field */
        break;
    case 2:
        mask = 6;   /* Mask bit 2:1 in the SID field */
        break;
    case 3:
        mask = 7;   /* Mask bit 2:0 in the SID field */
        break;
    default:
        g_assert_not_reached();
    }
    mask = ~mask;

    bus_n = VTD_SID_TO_BUS(source_id);
    devfn = VTD_SID_TO_DEVFN(source_id);

    g_hash_table_iter_init(&as_it, s->vtd_address_spaces);
    while (g_hash_table_iter_next(&as_it, NULL, (void **)&vtd_as)) {
        if ((pci_bus_num(vtd_as->bus) == bus_n) &&
            (vtd_as->devfn & mask) == (devfn & mask)) {
            trace_vtd_inv_desc_cc_device(bus_n, VTD_PCI_SLOT(vtd_as->devfn),
                                         VTD_PCI_FUNC(vtd_as->devfn));
            vtd_iommu_lock(s);
            vtd_as->context_cache_entry.context_cache_gen = 0;
            vtd_iommu_unlock(s);
            /*
             * Do switch address space when needed, in case if the
             * device passthrough bit is switched.
             */
            vtd_switch_address_space(vtd_as);
            /*
             * So a device is moving out of (or moving into) a
             * domain, resync the shadow page table.
             * This won't bring bad even if we have no such
             * notifier registered - the IOMMU notification
             * framework will skip MAP notifications if that
             * happened.
             */
            vtd_address_space_sync(vtd_as);
            /*
             * Per spec, context flush should also followed with PASID
             * cache and iotlb flush. Regards to a device selective
             * context cache invalidation:
             * if (emaulted_device)
             *    invalidate pasid cahce and pasid-based iotlb
             * else if (assigned_device)
             *    check if the device has been bound to any pasid
             *    invoke pasid_unbind regards to each bound pasid
             * Here, we have vtd_pasid_cache_devsi() to invalidate pasid
             * caches, while for piotlb in QEMU, we don't have it yet, so
             * no handling. For assigned device, host iommu driver would
             * flush piotlb when a pasid unbind is pass down to it.
             */
             vtd_pasid_cache_devsi(s, vtd_as->bus, devfn);
        }
    }
}

/* Context-cache invalidation
 * Returns the Context Actual Invalidation Granularity.
 * @val: the content of the CCMD_REG
 */
static uint64_t vtd_context_cache_invalidate(IntelIOMMUState *s, uint64_t val)
{
    uint64_t caig;
    uint64_t type = val & VTD_CCMD_CIRG_MASK;

    switch (type) {
    case VTD_CCMD_DOMAIN_INVL:
        /* Fall through */
    case VTD_CCMD_GLOBAL_INVL:
        caig = VTD_CCMD_GLOBAL_INVL_A;
        vtd_context_global_invalidate(s);
        break;

    case VTD_CCMD_DEVICE_INVL:
        caig = VTD_CCMD_DEVICE_INVL_A;
        vtd_context_device_invalidate(s, VTD_CCMD_SID(val), VTD_CCMD_FM(val));
        break;

    default:
        error_report_once("%s: invalid context: 0x%" PRIx64,
                          __func__, val);
        caig = 0;
    }
    return caig;
}

static void vtd_iotlb_global_invalidate(IntelIOMMUState *s)
{
    trace_vtd_inv_desc_iotlb_global();
    vtd_reset_iotlb(s);
    vtd_iommu_replay_all(s);
}

static void vtd_iotlb_domain_invalidate(IntelIOMMUState *s, uint16_t domain_id)
{
    VTDContextEntry ce;
    VTDAddressSpace *vtd_as;

    trace_vtd_inv_desc_iotlb_domain(domain_id);

    vtd_iommu_lock(s);
    g_hash_table_foreach_remove(s->iotlb, vtd_hash_remove_by_domain,
                                &domain_id);
    vtd_iommu_unlock(s);

    QLIST_FOREACH(vtd_as, &s->vtd_as_with_notifiers, next) {
        if (!vtd_dev_to_context_entry(s, pci_bus_num(vtd_as->bus),
                                      vtd_as->devfn, &ce) &&
            domain_id == vtd_get_domain_id(s, &ce, vtd_as->pasid)) {
            vtd_address_space_sync(vtd_as);
        }
    }
}

static void vtd_iotlb_page_invalidate_notify(IntelIOMMUState *s,
                                           uint16_t domain_id, hwaddr addr,
                                             uint8_t am, uint32_t pasid)
{
    VTDAddressSpace *vtd_as;
    VTDContextEntry ce;
    int ret;
    hwaddr size = (1 << am) * VTD_PAGE_SIZE;

    QLIST_FOREACH(vtd_as, &(s->vtd_as_with_notifiers), next) {
        if (pasid != PCI_NO_PASID && pasid != vtd_as->pasid) {
            continue;
        }
        ret = vtd_dev_to_context_entry(s, pci_bus_num(vtd_as->bus),
                                       vtd_as->devfn, &ce);
        if (!ret && domain_id == vtd_get_domain_id(s, &ce, vtd_as->pasid)) {
            if (vtd_as_has_map_notifier(vtd_as)) {
                /*
                 * As long as we have MAP notifications registered in
                 * any of our IOMMU notifiers, we need to sync the
                 * shadow page table.
                 */
                vtd_sync_shadow_page_table_range(vtd_as, &ce, addr, size);
            } else {
                /*
                 * For UNMAP-only notifiers, we don't need to walk the
                 * page tables.  We just deliver the PSI down to
                 * invalidate caches.
                 */
                IOMMUTLBEvent event = {
                    .type = IOMMU_NOTIFIER_UNMAP,
                    .entry = {
                        .target_as = &address_space_memory,
                        .iova = addr,
                        .translated_addr = 0,
                        .addr_mask = size - 1,
                        .perm = IOMMU_NONE,
                    },
                };
                memory_region_notify_iommu(&vtd_as->iommu, 0, event);
            }
        }
    }
}

static void vtd_iotlb_page_invalidate(IntelIOMMUState *s, uint16_t domain_id,
                                      hwaddr addr, uint8_t am)
{
    VTDIOTLBPageInvInfo info;

    trace_vtd_inv_desc_iotlb_pages(domain_id, addr, am);

    assert(am <= VTD_MAMV);
    info.is_piotlb = false;
    info.domain_id = domain_id;
    info.addr = addr;
    info.mask = ~((1 << am) - 1);
    vtd_iommu_lock(s);
    g_hash_table_foreach_remove(s->iotlb, vtd_hash_remove_by_page, &info);
    vtd_iommu_unlock(s);
    vtd_iotlb_page_invalidate_notify(s, domain_id, addr, am, PCI_NO_PASID);
}

/* Flush IOTLB
 * Returns the IOTLB Actual Invalidation Granularity.
 * @val: the content of the IOTLB_REG
 */
static uint64_t vtd_iotlb_flush(IntelIOMMUState *s, uint64_t val)
{
    uint64_t iaig;
    uint64_t type = val & VTD_TLB_FLUSH_GRANU_MASK;
    uint16_t domain_id;
    hwaddr addr;
    uint8_t am;

    switch (type) {
    case VTD_TLB_GLOBAL_FLUSH:
        iaig = VTD_TLB_GLOBAL_FLUSH_A;
        vtd_iotlb_global_invalidate(s);
        break;

    case VTD_TLB_DSI_FLUSH:
        domain_id = VTD_TLB_DID(val);
        iaig = VTD_TLB_DSI_FLUSH_A;
        vtd_iotlb_domain_invalidate(s, domain_id);
        break;

    case VTD_TLB_PSI_FLUSH:
        domain_id = VTD_TLB_DID(val);
        addr = vtd_get_quad_raw(s, DMAR_IVA_REG);
        am = VTD_IVA_AM(addr);
        addr = VTD_IVA_ADDR(addr);
        if (am > VTD_MAMV) {
            error_report_once("%s: address mask overflow: 0x%" PRIx64,
                              __func__, vtd_get_quad_raw(s, DMAR_IVA_REG));
            iaig = 0;
            break;
        }
        iaig = VTD_TLB_PSI_FLUSH_A;
        vtd_iotlb_page_invalidate(s, domain_id, addr, am);
        break;

    default:
        error_report_once("%s: invalid granularity: 0x%" PRIx64,
                          __func__, val);
        iaig = 0;
    }
    return iaig;
}

static void vtd_fetch_inv_desc(IntelIOMMUState *s);

static inline bool vtd_queued_inv_disable_check(IntelIOMMUState *s)
{
    return s->qi_enabled && (s->iq_tail == s->iq_head) &&
           (s->iq_last_desc_type == VTD_INV_DESC_WAIT);
}

static void vtd_handle_gcmd_qie(IntelIOMMUState *s, bool en)
{
    uint64_t iqa_val = vtd_get_quad_raw(s, DMAR_IQA_REG);

    trace_vtd_inv_qi_enable(en);

    if (en) {
        s->iq = iqa_val & VTD_IQA_IQA_MASK(s->aw_bits);
        /* 2^(x+8) entries */
        s->iq_size = 1UL << ((iqa_val & VTD_IQA_QS) + 8 - (s->iq_dw ? 1 : 0));
        s->qi_enabled = true;
        trace_vtd_inv_qi_setup(s->iq, s->iq_size);
        /* Ok - report back to driver */
        vtd_set_clear_mask_long(s, DMAR_GSTS_REG, 0, VTD_GSTS_QIES);

        if (s->iq_tail != 0) {
            /*
             * This is a spec violation but Windows guests are known to set up
             * Queued Invalidation this way so we allow the write and process
             * Invalidation Descriptors right away.
             */
            trace_vtd_warn_invalid_qi_tail(s->iq_tail);
            if (!(vtd_get_long_raw(s, DMAR_FSTS_REG) & VTD_FSTS_IQE)) {
                vtd_fetch_inv_desc(s);
            }
        }
    } else {
        if (vtd_queued_inv_disable_check(s)) {
            /* disable Queued Invalidation */
            vtd_set_quad_raw(s, DMAR_IQH_REG, 0);
            s->iq_head = 0;
            s->qi_enabled = false;
            /* Ok - report back to driver */
            vtd_set_clear_mask_long(s, DMAR_GSTS_REG, VTD_GSTS_QIES, 0);
        } else {
            error_report_once("%s: detected improper state when disable QI "
                              "(head=0x%x, tail=0x%x, last_type=%d)",
                              __func__,
                              s->iq_head, s->iq_tail, s->iq_last_desc_type);
        }
    }
}

/* Set Root Table Pointer */
static void vtd_handle_gcmd_srtp(IntelIOMMUState *s)
{
    vtd_root_table_setup(s);
    /* Ok - report back to driver */
    vtd_set_clear_mask_long(s, DMAR_GSTS_REG, 0, VTD_GSTS_RTPS);
    vtd_reset_caches(s);
    vtd_address_space_refresh_all(s);
    vtd_refresh_pasid_bind(s);
}

/* Set Interrupt Remap Table Pointer */
static void vtd_handle_gcmd_sirtp(IntelIOMMUState *s)
{
    vtd_interrupt_remap_table_setup(s);
    /* Ok - report back to driver */
    vtd_set_clear_mask_long(s, DMAR_GSTS_REG, 0, VTD_GSTS_IRTPS);
}

/* Handle Translation Enable/Disable */
static void vtd_handle_gcmd_te(IntelIOMMUState *s, bool en)
{
    if (s->dmar_enabled == en) {
        return;
    }

    trace_vtd_dmar_enable(en);

    if (en) {
        s->dmar_enabled = true;
        /* Ok - report back to driver */
        vtd_set_clear_mask_long(s, DMAR_GSTS_REG, 0, VTD_GSTS_TES);
    } else {
        s->dmar_enabled = false;

        /* Clear the index of Fault Recording Register */
        s->next_frcd_reg = 0;
        /* Ok - report back to driver */
        vtd_set_clear_mask_long(s, DMAR_GSTS_REG, VTD_GSTS_TES, 0);
    }

    vtd_reset_caches(s);
    vtd_address_space_refresh_all(s);
    vtd_refresh_pasid_bind(s);
}

/* Handle Interrupt Remap Enable/Disable */
static void vtd_handle_gcmd_ire(IntelIOMMUState *s, bool en)
{
    trace_vtd_ir_enable(en);

    if (en) {
        s->intr_enabled = true;
        /* Ok - report back to driver */
        vtd_set_clear_mask_long(s, DMAR_GSTS_REG, 0, VTD_GSTS_IRES);
    } else {
        s->intr_enabled = false;
        /* Ok - report back to driver */
        vtd_set_clear_mask_long(s, DMAR_GSTS_REG, VTD_GSTS_IRES, 0);
    }
}

/* Handle write to Global Command Register */
static void vtd_handle_gcmd_write(IntelIOMMUState *s)
{
    X86IOMMUState *x86_iommu = X86_IOMMU_DEVICE(s);
    uint32_t status = vtd_get_long_raw(s, DMAR_GSTS_REG);
    uint32_t val = vtd_get_long_raw(s, DMAR_GCMD_REG);
    uint32_t changed = status ^ val;

    trace_vtd_reg_write_gcmd(status, val);
    if ((changed & VTD_GCMD_TE) && s->dma_translation) {
        /* Translation enable/disable */
        vtd_handle_gcmd_te(s, val & VTD_GCMD_TE);
    }
    if (val & VTD_GCMD_SRTP) {
        /* Set/update the root-table pointer */
        vtd_handle_gcmd_srtp(s);
    }
    if (changed & VTD_GCMD_QIE) {
        /* Queued Invalidation Enable */
        vtd_handle_gcmd_qie(s, val & VTD_GCMD_QIE);
    }
    if (val & VTD_GCMD_SIRTP) {
        /* Set/update the interrupt remapping root-table pointer */
        vtd_handle_gcmd_sirtp(s);
    }
    if ((changed & VTD_GCMD_IRE) &&
        x86_iommu_ir_supported(x86_iommu)) {
        /* Interrupt remap enable/disable */
        vtd_handle_gcmd_ire(s, val & VTD_GCMD_IRE);
    }
}

/* Handle write to Context Command Register */
static void vtd_handle_ccmd_write(IntelIOMMUState *s)
{
    uint64_t ret;
    uint64_t val = vtd_get_quad_raw(s, DMAR_CCMD_REG);

    /* Context-cache invalidation request */
    if (val & VTD_CCMD_ICC) {
        if (s->qi_enabled) {
            error_report_once("Queued Invalidation enabled, "
                              "should not use register-based invalidation");
            return;
        }
        ret = vtd_context_cache_invalidate(s, val);
        /* Invalidation completed. Change something to show */
        vtd_set_clear_mask_quad(s, DMAR_CCMD_REG, VTD_CCMD_ICC, 0ULL);
        ret = vtd_set_clear_mask_quad(s, DMAR_CCMD_REG, VTD_CCMD_CAIG_MASK,
                                      ret);
    }
}

/* Handle write to IOTLB Invalidation Register */
static void vtd_handle_iotlb_write(IntelIOMMUState *s)
{
    uint64_t ret;
    uint64_t val = vtd_get_quad_raw(s, DMAR_IOTLB_REG);

    /* IOTLB invalidation request */
    if (val & VTD_TLB_IVT) {
        if (s->qi_enabled) {
            error_report_once("Queued Invalidation enabled, "
                              "should not use register-based invalidation");
            return;
        }
        ret = vtd_iotlb_flush(s, val);
        /* Invalidation completed. Change something to show */
        vtd_set_clear_mask_quad(s, DMAR_IOTLB_REG, VTD_TLB_IVT, 0ULL);
        ret = vtd_set_clear_mask_quad(s, DMAR_IOTLB_REG,
                                      VTD_TLB_FLUSH_GRANU_MASK_A, ret);
    }
}

/* Fetch an Invalidation Descriptor from the Invalidation Queue */
static bool vtd_get_inv_desc(IntelIOMMUState *s,
                             VTDInvDesc *inv_desc)
{
    dma_addr_t base_addr = s->iq;
    uint32_t offset = s->iq_head;
    uint32_t dw = s->iq_dw ? 32 : 16;
    dma_addr_t addr = base_addr + offset * dw;

    if (dma_memory_read(&address_space_memory, addr,
                        inv_desc, dw, MEMTXATTRS_UNSPECIFIED)) {
        error_report_once("Read INV DESC failed.");
        return false;
    }
    inv_desc->lo = le64_to_cpu(inv_desc->lo);
    inv_desc->hi = le64_to_cpu(inv_desc->hi);
    if (dw == 32) {
        inv_desc->val[2] = le64_to_cpu(inv_desc->val[2]);
        inv_desc->val[3] = le64_to_cpu(inv_desc->val[3]);
    }
    return true;
}

static bool vtd_process_wait_desc(IntelIOMMUState *s, VTDInvDesc *inv_desc)
{
    if ((inv_desc->hi & VTD_INV_DESC_WAIT_RSVD_HI) ||
        (inv_desc->lo & VTD_INV_DESC_WAIT_RSVD_LO)) {
        error_report_once("%s: invalid wait desc: hi=%"PRIx64", lo=%"PRIx64
                          " (reserved nonzero)", __func__, inv_desc->hi,
                          inv_desc->lo);
        return false;
    }
    if (inv_desc->lo & VTD_INV_DESC_WAIT_SW) {
        /* Status Write */
        uint32_t status_data = (uint32_t)(inv_desc->lo >>
                               VTD_INV_DESC_WAIT_DATA_SHIFT);

        assert(!(inv_desc->lo & VTD_INV_DESC_WAIT_IF));

        /* FIXME: need to be masked with HAW? */
        dma_addr_t status_addr = inv_desc->hi;
        trace_vtd_inv_desc_wait_sw(status_addr, status_data);
        status_data = cpu_to_le32(status_data);
        if (dma_memory_write(&address_space_memory, status_addr,
                             &status_data, sizeof(status_data),
                             MEMTXATTRS_UNSPECIFIED)) {
            trace_vtd_inv_desc_wait_write_fail(inv_desc->hi, inv_desc->lo);
            return false;
        }
    } else if (inv_desc->lo & VTD_INV_DESC_WAIT_IF) {
        /* Interrupt flag */
        vtd_generate_completion_event(s);
    } else if (inv_desc->lo & VTD_INV_DESC_WAIT_FN) {
        /* Fence flag */
        trace_vtd_inv_desc_wait_fence(inv_desc->hi, inv_desc->lo);
        /*
         * per spec CH 7.10, such wait descriptor is to ensure all
         * requests submitted to invalidation queue is processed
         * before processing requests after this wait descriptor.
         * This is already guaranteed in current implemenation.
         */
    } else {
        error_report_once("%s: invalid wait desc: hi=%"PRIx64", lo=%"PRIx64
                          " (unknown type)", __func__, inv_desc->hi,
                          inv_desc->lo);
        return false;
    }
    return true;
}

static bool vtd_process_context_cache_desc(IntelIOMMUState *s,
                                           VTDInvDesc *inv_desc)
{
    uint16_t sid, fmask;

    if ((inv_desc->lo & VTD_INV_DESC_CC_RSVD) || inv_desc->hi) {
        error_report_once("%s: invalid cc inv desc: hi=%"PRIx64", lo=%"PRIx64
                          " (reserved nonzero)", __func__, inv_desc->hi,
                          inv_desc->lo);
        return false;
    }
    switch (inv_desc->lo & VTD_INV_DESC_CC_G) {
    case VTD_INV_DESC_CC_DOMAIN:
        trace_vtd_inv_desc_cc_domain(
            (uint16_t)VTD_INV_DESC_CC_DID(inv_desc->lo));
        /* Fall through */
    case VTD_INV_DESC_CC_GLOBAL:
        vtd_context_global_invalidate(s);
        break;

    case VTD_INV_DESC_CC_DEVICE:
        sid = VTD_INV_DESC_CC_SID(inv_desc->lo);
        fmask = VTD_INV_DESC_CC_FM(inv_desc->lo);
        vtd_context_device_invalidate(s, sid, fmask);
        break;

    default:
        error_report_once("%s: invalid cc inv desc: hi=%"PRIx64", lo=%"PRIx64
                          " (invalid type)", __func__, inv_desc->hi,
                          inv_desc->lo);
        return false;
    }
    return true;
}

static bool vtd_process_iotlb_desc(IntelIOMMUState *s, VTDInvDesc *inv_desc)
{
    uint16_t domain_id;
    uint8_t am;
    hwaddr addr;

    if ((inv_desc->lo & VTD_INV_DESC_IOTLB_RSVD_LO) ||
        (inv_desc->hi & VTD_INV_DESC_IOTLB_RSVD_HI)) {
        error_report_once("%s: invalid iotlb inv desc: hi=0x%"PRIx64
                          ", lo=0x%"PRIx64" (reserved bits unzero)",
                          __func__, inv_desc->hi, inv_desc->lo);
        return false;
    }

    switch (inv_desc->lo & VTD_INV_DESC_IOTLB_G) {
    case VTD_INV_DESC_IOTLB_GLOBAL:
        vtd_iotlb_global_invalidate(s);
        break;

    case VTD_INV_DESC_IOTLB_DOMAIN:
        domain_id = VTD_INV_DESC_IOTLB_DID(inv_desc->lo);
        vtd_iotlb_domain_invalidate(s, domain_id);
        break;

    case VTD_INV_DESC_IOTLB_PAGE:
        domain_id = VTD_INV_DESC_IOTLB_DID(inv_desc->lo);
        addr = VTD_INV_DESC_IOTLB_ADDR(inv_desc->hi);
        am = VTD_INV_DESC_IOTLB_AM(inv_desc->hi);
        if (am > VTD_MAMV) {
            error_report_once("%s: invalid iotlb inv desc: hi=0x%"PRIx64
                              ", lo=0x%"PRIx64" (am=%u > VTD_MAMV=%u)",
                              __func__, inv_desc->hi, inv_desc->lo,
                              am, (unsigned)VTD_MAMV);
            return false;
        }
        vtd_iotlb_page_invalidate(s, domain_id, addr, am);
        break;

    default:
        error_report_once("%s: invalid iotlb inv desc: hi=0x%"PRIx64
                          ", lo=0x%"PRIx64" (type mismatch: 0x%llx)",
                          __func__, inv_desc->hi, inv_desc->lo,
                          inv_desc->lo & VTD_INV_DESC_IOTLB_G);
        return false;
    }
    return true;
}

static inline void vtd_init_pasid_key(uint32_t pasid,
                                     uint16_t sid,
                                     struct pasid_key *key)
{
    key->pasid = pasid;
    key->sid = sid;
}

static guint vtd_pasid_as_key_hash(gconstpointer v)
{
    struct pasid_key *key = (struct pasid_key *)v;
    uint32_t a, b, c;

    /* Jenkins hash */
    a = b = c = JHASH_INITVAL + sizeof(*key);
    a += key->sid;
    b += extract32(key->pasid, 0, 16);
    c += extract32(key->pasid, 16, 16);

    __jhash_mix(a, b, c);
    __jhash_final(a, b, c);

    return c;
}

static gboolean vtd_pasid_as_key_equal(gconstpointer v1, gconstpointer v2)
{
    const struct pasid_key *k1 = v1;
    const struct pasid_key *k2 = v2;

    return (k1->pasid == k2->pasid) && (k1->sid == k2->sid);
}

static inline int vtd_dev_get_pe_from_pasid(IntelIOMMUState *s,
                                            uint8_t bus_num,
                                            uint8_t devfn,
                                            uint32_t pasid,
                                            VTDPASIDEntry *pe)
{
    VTDContextEntry ce;
    int ret;
    dma_addr_t pasid_dir_base;

    if (!s->root_scalable) {
        return -VTD_FR_RTADDR_INV_TTM;
    }

    ret = vtd_dev_to_context_entry(s, bus_num, devfn, &ce);
    if (ret) {
        return ret;
    }

    pasid_dir_base = VTD_CE_GET_PASID_DIR_TABLE(&ce);
    ret = vtd_get_pe_from_pasid_table(s,
                                  pasid_dir_base, pasid, pe);

    return ret;
}

static bool vtd_pasid_entry_compare(VTDPASIDEntry *p1, VTDPASIDEntry *p2)
{
    return !memcmp(p1, p2, sizeof(*p1));
}

/*
 * This function fills in the pasid entry in &vtd_pasid_as. Caller
 * of this function should hold iommu_lock.
 */
static int vtd_fill_pe_in_cache(IntelIOMMUState *s,
                                VTDPASIDAddressSpace *vtd_pasid_as,
                                VTDPASIDEntry *pe)
{
    VTDPASIDCacheEntry *pc_entry = &vtd_pasid_as->pasid_cache_entry;
    int ret;

    if (pc_entry->cache_filled) {
        if (vtd_pasid_entry_compare(pe, &pc_entry->pasid_entry)) {
            /* No need to go further as cached pasid entry is latest */
            return 0;
        }
        ret = vtd_bind_guest_pasid(vtd_pasid_as,
                                   pe, VTD_PASID_UPDATE);
    } else {
        ret = vtd_bind_guest_pasid(vtd_pasid_as,
                                   pe, VTD_PASID_BIND);
    }

    if (!ret) {
        pc_entry->pasid_entry = *pe;
        pc_entry->cache_filled = true;
    }
    return ret;
}

/*
 * This function is used to clear cached pasid entry in vtd_pasid_as
 * instances. Caller of this function should hold iommu_lock.
 */
static gboolean vtd_flush_pasid(gpointer key, gpointer value,
                                gpointer user_data)
{
    VTDPASIDCacheInfo *pc_info = user_data;
    VTDPASIDAddressSpace *vtd_pasid_as = value;
    IntelIOMMUState *s = vtd_pasid_as->iommu_state;
    VTDPASIDCacheEntry *pc_entry = &vtd_pasid_as->pasid_cache_entry;
    PCIBus *bus = vtd_pasid_as->bus;
    VTDPASIDEntry pe;
    VTDIOMMUFDDevice *vtd_idev;
    VTDIOTLBPageInvInfo info;
    uint16_t did;
    uint32_t pasid;
    uint16_t devfn;
    int ret;
    struct vtd_as_key as_key = {
        .bus = vtd_pasid_as->bus,
        .devfn = vtd_pasid_as->devfn,
    };

    did = vtd_pe_get_domain_id(&pc_entry->pasid_entry);
    pasid = vtd_pasid_as->pasid;
    devfn = vtd_pasid_as->devfn;
    vtd_idev = g_hash_table_lookup(s->vtd_iommufd_dev, &as_key);

    switch (pc_info->type) {
    case VTD_PASID_CACHE_FORCE_RESET:
        goto remove;
    case VTD_PASID_CACHE_PASIDSI:
        if (pc_info->pasid != pasid) {
            return false;
        }
        /* Fall through */
    case VTD_PASID_CACHE_DOMSI:
        if (pc_info->domain_id != did) {
            return false;
        }
        /* Fall through */
    case VTD_PASID_CACHE_GLOBAL_INV:
        break;
    case VTD_PASID_CACHE_DEVSI:
        if (pc_info->bus != bus ||
            pc_info->devfn != devfn) {
            return false;
        }
        break;
    default:
        error_report("invalid pc_info->type");
        abort();
    }

    info.domain_id = did;
    info.pasid = pasid;
    /* For passthrough device, we don't need invalidate QEMU piotlb */
    if (s->root_scalable && likely(s->dmar_enabled) && !vtd_idev)
        g_hash_table_foreach_remove(s->p_iotlb, vtd_hash_remove_by_pasid,
                                    &info);

    /*
     * pasid cache invalidation may indicate a present pasid
     * entry to present pasid entry modification. To cover such
     * case, vIOMMU emulator needs to fetch latest guest pasid
     * entry and check cached pasid entry, then update pasid
     * cache and send pasid bind/unbind to host properly.
     */
    ret = vtd_dev_get_pe_from_pasid(s, pci_bus_num(bus),
                                    devfn, pasid, &pe);
    if (ret) {
        /*
         * No valid pasid entry in guest memory. e.g. pasid entry
         * was modified to be either all-zero or non-present. Either
         * case means existing pasid cache should be removed.
         */
        goto remove;
    }

    if (vtd_fill_pe_in_cache(s, vtd_pasid_as, &pe)) {
        pasid_cache_info_set_error(pc_info);
        return true;
    }

    return false;

remove:
    if (vtd_bind_guest_pasid(vtd_pasid_as,
                             NULL, VTD_PASID_UNBIND)) {
        pasid_cache_info_set_error(pc_info);
    }

    return true;
}

/*
 * This function finds or adds a VTDPASIDAddressSpace for a device
 * when it is bound to a pasid. Caller of this function should hold
 * iommu_lock.
 */
static VTDPASIDAddressSpace *vtd_add_find_pasid_as(IntelIOMMUState *s,
                                                   PCIBus *bus,
                                                   int devfn,
                                                   uint32_t pasid)
{
    struct pasid_key key;
    struct pasid_key *new_key;
    VTDPASIDAddressSpace *vtd_pasid_as;
    uint16_t sid;

    sid = PCI_BUILD_BDF(pci_bus_num(bus), devfn);
    vtd_init_pasid_key(pasid, sid, &key);
    vtd_pasid_as = g_hash_table_lookup(s->vtd_pasid_as, &key);

    if (!vtd_pasid_as) {
        new_key = g_malloc0(sizeof(*new_key));
        vtd_init_pasid_key(pasid, sid, new_key);
        /*
         * Initiate the vtd_pasid_as structure.
         *
         * This structure here is used to track the guest pasid
         * binding and also serves as pasid-cache mangement entry.
         *
         * TODO: in future, if wants to support the SVA-aware DMA
         *       emulation, the vtd_pasid_as should have include
         *       AddressSpace to support DMA emulation.
         */
        vtd_pasid_as = g_malloc0(sizeof(VTDPASIDAddressSpace));
        vtd_pasid_as->iommu_state = s;
        vtd_pasid_as->bus = bus;
        vtd_pasid_as->devfn = devfn;
        vtd_pasid_as->pasid = pasid;
        g_hash_table_insert(s->vtd_pasid_as, new_key, vtd_pasid_as);
    }
    return vtd_pasid_as;
}

/* Caller of this function should hold iommu_lock. */
static void vtd_remove_pasid_as(VTDPASIDAddressSpace *vtd_pasid_as)
{
    IntelIOMMUState *s = vtd_pasid_as->iommu_state;
    PCIBus *bus = vtd_pasid_as->bus;
    struct pasid_key key;
    int devfn = vtd_pasid_as->devfn;
    uint32_t pasid = vtd_pasid_as->pasid;
    uint16_t sid;

    sid = PCI_BUILD_BDF(pci_bus_num(bus), devfn);
    vtd_init_pasid_key(pasid, sid, &key);

    g_hash_table_remove(s->vtd_pasid_as, &key);
}

/* Caller of this function should hold iommu_lock. */
static void vtd_sm_pasid_table_walk_one(IntelIOMMUState *s,
                                        dma_addr_t pt_base,
                                        int start,
                                        int end,
                                        VTDPASIDCacheInfo *info)
{
    VTDPASIDEntry pe;
    int pasid = start;
    int pasid_next;
    VTDPASIDAddressSpace *vtd_pasid_as;

    while (pasid < end) {
        pasid_next = pasid + 1;

        if (!vtd_get_pe_in_pasid_leaf_table(s, pasid, pt_base, &pe)
            && vtd_pe_present(&pe)) {
            vtd_pasid_as = vtd_add_find_pasid_as(s,
                                       info->bus, info->devfn, pasid);
            if ((info->type == VTD_PASID_CACHE_DOMSI ||
                 info->type == VTD_PASID_CACHE_PASIDSI) &&
                !(info->domain_id == vtd_pe_get_domain_id(&pe))) {
                /*
                 * VTD_PASID_CACHE_DOMSI and VTD_PASID_CACHE_PASIDSI
                 * requires domain ID check. If domain Id check fail,
                 * go to next pasid.
                 */
                pasid = pasid_next;
                continue;
            }
            if (vtd_fill_pe_in_cache(s, vtd_pasid_as, &pe)) {
                vtd_remove_pasid_as(vtd_pasid_as);
                pasid_cache_info_set_error(info);
            }
        }
        pasid = pasid_next;
    }
}

/*
 * Currently, VT-d scalable mode pasid table is a two level table,
 * this function aims to loop a range of PASIDs in a given pasid
 * table to identify the pasid config in guest.
 * Caller of this function should hold iommu_lock.
 */
static void vtd_sm_pasid_table_walk(IntelIOMMUState *s,
                                    dma_addr_t pdt_base,
                                    int start,
                                    int end,
                                    VTDPASIDCacheInfo *info)
{
    VTDPASIDDirEntry pdire;
    int pasid = start;
    int pasid_next;
    dma_addr_t pt_base;

    while (pasid < end) {
        pasid_next = ((end - pasid) > VTD_PASID_TBL_ENTRY_NUM) ?
                      (pasid + VTD_PASID_TBL_ENTRY_NUM) : end;
        if (!vtd_get_pdire_from_pdir_table(pdt_base, pasid, &pdire)
            && vtd_pdire_present(&pdire)) {
            pt_base = pdire.val & VTD_PASID_TABLE_BASE_ADDR_MASK;
            vtd_sm_pasid_table_walk_one(s, pt_base, pasid, pasid_next, info);
        }
        pasid = pasid_next;
    }
}

static void vtd_replay_pasid_bind_for_dev(IntelIOMMUState *s,
                                          int start, int end,
                                          VTDPASIDCacheInfo *info)
{
    VTDContextEntry ce;
    int bus_n, devfn;

    bus_n = pci_bus_num(info->bus);
    devfn = info->devfn;

    if (!vtd_dev_to_context_entry(s, bus_n, devfn, &ce)) {
        uint32_t max_pasid;

        max_pasid = vtd_sm_ce_get_pdt_entry_num(&ce) * VTD_PASID_TBL_ENTRY_NUM;
        if (end > max_pasid) {
            end = max_pasid;
        }
        vtd_sm_pasid_table_walk(s,
                                VTD_CE_GET_PASID_DIR_TABLE(&ce),
                                start,
                                end,
                                info);
    }
}

/*
 * This function replay the guest pasid bindings to hots by
 * walking the guest PASID table. This ensures host will have
 * latest guest pasid bindings. Caller should hold iommu_lock.
 */
static void vtd_replay_guest_pasid_bindings(IntelIOMMUState *s,
                                            VTDPASIDCacheInfo *pc_info)
{
    VTDIOMMUFDDevice *vtd_idev;
    int start = 0, end = 1 << (VTD_GET_PSS(s->ecap) + 1);
    VTDPASIDCacheInfo walk_info;

    switch (pc_info->type) {
    case VTD_PASID_CACHE_PASIDSI:
        start = pc_info->pasid;
        end = pc_info->pasid + 1;
        /*
         * PASID selective invalidation is within domain,
         * thus fall through.
         */
    case VTD_PASID_CACHE_DOMSI:
    case VTD_PASID_CACHE_GLOBAL_INV:
        /* loop all assigned devices */
        break;
    case VTD_PASID_CACHE_DEVSI:
        walk_info.bus = pc_info->bus;
        walk_info.devfn = pc_info->devfn;
        vtd_replay_pasid_bind_for_dev(s, start, end, &walk_info);
        return;
    case VTD_PASID_CACHE_FORCE_RESET:
        /* For force reset, no need to go further replay */
        return;
    default:
        error_report("invalid pc_info->type for replay");
        abort();
    }

    /*
     * In this replay, only needs to care about the devices which
     * are backed by host IOMMU. For such devices, their vtd_idev
     * instances are in the s->vtd_idev_list. For devices which
     * are not backed byhost IOMMU, it is not necessary to replay
     * the bindings since their cache could be re-created in the future
     * DMA address transaltion.
     */
    walk_info = *pc_info;
    QLIST_FOREACH(vtd_idev, &s->vtd_idev_list, next) {
        /* bus|devfn fields are not identical with pc_info */
        walk_info.bus = vtd_idev->bus;
        walk_info.devfn = vtd_idev->devfn;
        vtd_replay_pasid_bind_for_dev(s, start, end, &walk_info);
    }
    if (walk_info.error_happened) {
        pasid_cache_info_set_error(pc_info);
    }
}

static void vtd_refresh_pasid_bind(IntelIOMMUState *s)
{
    VTDPASIDCacheInfo pc_info = { .error_happened = false,
                                  .type = VTD_PASID_CACHE_GLOBAL_INV };

    /*
     * Only when dmar is enabled, should pasid bindings replayed,
     * otherwise no need to replay.
     */
    if (!s->dmar_enabled) {
        return;
    }

    if (!s->scalable_modern || !s->root_scalable) {
        return;
    }

    vtd_iommu_lock(s);
    vtd_replay_guest_pasid_bindings(s, &pc_info);
    vtd_iommu_unlock(s);
}

/*
 * This function syncs the pasid bindings between guest and host.
 * It includes updating the pasid cache in vIOMMU and updating the
 * pasid bindings per guest's latest pasid entry presence.
 */
static void vtd_pasid_cache_sync(IntelIOMMUState *s,
                                 VTDPASIDCacheInfo *pc_info)
{
    if (!s->scalable_modern || !s->root_scalable || !s->dmar_enabled) {
        return;
    }

    /*
     * Regards to a pasid cache invalidation, e.g. a PSI.
     * it could be either cases of below:
     * a) a present pasid entry moved to non-present
     * b) a present pasid entry to be a present entry
     * c) a non-present pasid entry moved to present
     *
     * Different invalidation granularity may affect different device
     * scope and pasid scope. But for each invalidation granularity,
     * it needs to do two steps to sync host and guest pasid binding.
     *
     * Here is the handling of a PSI:
     * 1) loop all the existing vtd_pasid_as instances to update them
     *    according to the latest guest pasid entry in pasid table.
     *    this will make sure affected existing vtd_pasid_as instances
     *    cached the latest pasid entries. Also, during the loop, the
     *    host should be notified if needed. e.g. pasid unbind or pasid
     *    update. Should be able to cover case a) and case b).
     *
     * 2) loop all devices to cover case c)
     *    - For devices which have IOMMUFDDevice instances,
     *      we loop them and check if guest pasid entry exists. If yes,
     *      it is case c), we update the pasid cache and also notify
     *      host.
     *    - For devices which have no IOMMUFDDevice, it is not
     *      necessary to create pasid cache at this phase since it
     *      could be created when vIOMMU does DMA address translation.
     *      This is not yet implemented since there is no emulated
     *      pasid-capable devices today. If we have such devices in
     *      future, the pasid cache shall be created there.
     * Other granularity follow the same steps, just with different scope
     *
     */

    vtd_iommu_lock(s);
    /* Step 1: loop all the exisitng vtd_pasid_as instances */
    g_hash_table_foreach_remove(s->vtd_pasid_as,
                                vtd_flush_pasid, pc_info);

    /*
     * Step 2: loop all the exisitng vtd_idev instances.
     * Ideally, needs to loop all devices to find if there is any new
     * PASID binding regards to the PASID cache invalidation request.
     * But it is enough to loop the devices which are backed by host
     * IOMMU. For devices backed by vIOMMU (a.k.a emulated devices),
     * if new PASID happened on them, their vtd_pasid_as instance could
     * be created during future vIOMMU DMA translation.
     */
    vtd_replay_guest_pasid_bindings(s, pc_info);
    vtd_iommu_unlock(s);
}

static void vtd_pasid_cache_devsi(IntelIOMMUState *s,
                                  PCIBus *bus, uint16_t devfn)
{
    VTDPASIDCacheInfo pc_info = { .error_happened = false, };

    trace_vtd_pasid_cache_devsi(devfn);

    pc_info.type = VTD_PASID_CACHE_DEVSI;
    pc_info.bus = bus;
    pc_info.devfn = devfn;

    vtd_pasid_cache_sync(s, &pc_info);
}

/* Caller of this function should hold iommu_lock */
static void vtd_pasid_cache_reset(IntelIOMMUState *s)
{
    VTDPASIDCacheInfo pc_info = { .error_happened = false, };

    trace_vtd_pasid_cache_reset();

    pc_info.type = VTD_PASID_CACHE_FORCE_RESET;

    /*
     * Reset pasid cache is a big hammer, so use
     * g_hash_table_foreach_remove which will free
     * the vtd_pasid_as instances. Also, as a big
     * hammer, use VTD_PASID_CACHE_FORCE_RESET to
     * ensure all the vtd_pasid_as instances are
     * dropped, meanwhile the change will be pass
     * to host if IOMMUFDDevice is available.
     */
    g_hash_table_foreach_remove(s->vtd_pasid_as,
                                vtd_flush_pasid, &pc_info);
}

static bool vtd_process_pasid_desc(IntelIOMMUState *s,
                                   VTDInvDesc *inv_desc)
{
    VTDPASIDCacheInfo pc_info = { .error_happened = false, };
    uint16_t domain_id;
    uint32_t pasid;

    if ((inv_desc->val[0] & VTD_INV_DESC_PASIDC_RSVD_VAL0) ||
        (inv_desc->val[1] & VTD_INV_DESC_PASIDC_RSVD_VAL1) ||
        (inv_desc->val[2] & VTD_INV_DESC_PASIDC_RSVD_VAL2) ||
        (inv_desc->val[3] & VTD_INV_DESC_PASIDC_RSVD_VAL3)) {
        error_report_once("non-zero-field-in-pc_inv_desc hi: 0x%" PRIx64
                  " lo: 0x%" PRIx64, inv_desc->val[1], inv_desc->val[0]);
        return false;
    }

    domain_id = VTD_INV_DESC_PASIDC_DID(inv_desc->val[0]);
    pasid = VTD_INV_DESC_PASIDC_PASID(inv_desc->val[0]);

    switch (inv_desc->val[0] & VTD_INV_DESC_PASIDC_G) {
    case VTD_INV_DESC_PASIDC_DSI:
        trace_vtd_pasid_cache_dsi(domain_id);
        pc_info.type = VTD_PASID_CACHE_DOMSI;
        pc_info.domain_id = domain_id;
        break;

    case VTD_INV_DESC_PASIDC_PASID_SI:
        /* PASID selective implies a DID selective */
        trace_vtd_pasid_cache_psi(domain_id, pasid);
        pc_info.type = VTD_PASID_CACHE_PASIDSI;
        pc_info.domain_id = domain_id;
        pc_info.pasid = pasid;
        break;

    case VTD_INV_DESC_PASIDC_GLOBAL:
        trace_vtd_pasid_cache_gsi();
        pc_info.type = VTD_PASID_CACHE_GLOBAL_INV;
        break;

    default:
        error_report_once("invalid-inv-granu-in-pc_inv_desc hi: 0x%" PRIx64
                  " lo: 0x%" PRIx64, inv_desc->val[1], inv_desc->val[0]);
        return false;
    }

    vtd_pasid_cache_sync(s, &pc_info);
    return !pc_info.error_happened ? true : false;
}

/**
 * Caller of this function should hold iommu_lock.
 */
static void vtd_invalidate_piotlb(VTDPASIDAddressSpace *vtd_pasid_as,
                                  struct iommu_hwpt_vtd_s1_invalidate *cache)
{
    VTDIOMMUFDDevice *vtd_idev;
    VTDHwpt *hwpt = &vtd_pasid_as->hwpt;
    int devfn = vtd_pasid_as->devfn;
    struct vtd_as_key key = {
        .bus = vtd_pasid_as->bus,
        .devfn = devfn,
    };
    IntelIOMMUState *s = vtd_pasid_as->iommu_state;
    uint32_t req_num = 1; /* Only implement one request for simplicity */

    if (!hwpt) {
        return;
    }

    vtd_idev = g_hash_table_lookup(s->vtd_iommufd_dev, &key);
    if (!vtd_idev || !vtd_idev->idev) {
        return;
    }
    if (iommufd_backend_invalidate_cache(vtd_idev->idev->iommufd, hwpt->hwpt_id,
                                         IOMMU_HWPT_DATA_VTD_S1,
                                         sizeof(*cache), &req_num, cache)) {
        error_report("Cache flush failed");
    }
    g_assert(req_num == 1);
}

/**
 * This function is a loop function for the s->vtd_pasid_as
 * list with VTDPIOTLBInvInfo as execution filter. It propagates
 * the piotlb invalidation to host. Caller of this function
 * should hold iommu_lock.
 */
static void vtd_flush_pasid_iotlb(gpointer key, gpointer value,
                                  gpointer user_data)
{
    VTDPIOTLBInvInfo *piotlb_info = user_data;
    VTDPASIDAddressSpace *vtd_pasid_as = value;
    VTDPASIDCacheEntry *pc_entry = &vtd_pasid_as->pasid_cache_entry;
    uint16_t did;

    if (!vtd_pe_pgtt_is_flt(&pc_entry->pasid_entry)) {
        return;
    }

    did = vtd_pe_get_domain_id(&pc_entry->pasid_entry);

    if ((piotlb_info->domain_id == did) &&
        (piotlb_info->pasid == vtd_pasid_as->pasid)) {
        vtd_invalidate_piotlb(vtd_pasid_as,
                              piotlb_info->inv_data);
    }
}

static gboolean vtd_hash_remove_by_pasid(gpointer key, gpointer value,
                                         gpointer user_data)
{
    VTDIOTLBEntry *entry = (VTDIOTLBEntry *)value;
    VTDIOTLBPageInvInfo *info = (VTDIOTLBPageInvInfo *)user_data;

    return ((entry->domain_id == info->domain_id) &&
            (entry->pasid == info->pasid));
}

static void vtd_piotlb_pasid_invalidate(IntelIOMMUState *s,
                                        uint16_t domain_id,
                                        uint32_t pasid)
{
    struct iommu_hwpt_vtd_s1_invalidate cache_info = { 0 };
    VTDPIOTLBInvInfo piotlb_info;
    VTDIOTLBPageInvInfo info;
    VTDAddressSpace *vtd_as;
    VTDContextEntry ce;
    VTDPASIDEntry pe;
    int ret;

    cache_info.addr = 0;
    cache_info.npages = (uint64_t)-1;

    piotlb_info.domain_id = domain_id;
    piotlb_info.pasid = pasid;
    piotlb_info.inv_data = &cache_info;

    info.domain_id = domain_id;
    info.pasid = pasid;

    vtd_iommu_lock(s);
    /*
     * Here loops all the vtd_pasid_as instances in s->vtd_pasid_as
     * to find out the affected devices since piotlb invalidation
     * should check pasid cache per architecture point of view.
     */
    g_hash_table_foreach(s->vtd_pasid_as,
                         vtd_flush_pasid_iotlb, &piotlb_info);
    g_hash_table_foreach_remove(s->p_iotlb, vtd_hash_remove_by_pasid,
                                &info);
    vtd_iommu_unlock(s);

    QLIST_FOREACH(vtd_as, &(s->vtd_as_with_notifiers), next) {
        uint32_t rid2pasid = 0;
        vtd_dev_get_rid2pasid(s, pci_bus_num(vtd_as->bus), vtd_as->devfn,
                              &rid2pasid);
        ret = vtd_dev_to_context_entry(s, pci_bus_num(vtd_as->bus),
                                       vtd_as->devfn, &ce);
        if (s->root_scalable && likely(s->dmar_enabled) &&
            domain_id == vtd_get_domain_id(s, &ce, pasid) &&
            !ret && pasid == rid2pasid) {
            ret = vtd_ce_get_rid2pasid_entry(s, &ce, &pe, pasid);
            if (!ret && VTD_PE_GET_TYPE(&pe) == VTD_SM_PASID_ENTRY_FLT) {
                ret = vtd_sync_flt_range(vtd_as, &ce, 0, UINT64_MAX);
            } else {
                ret = vtd_sync_shadow_page_table_range(vtd_as, &ce,
                                                       0, UINT64_MAX);
            }
        }
    }
}

static void vtd_piotlb_page_invalidate(IntelIOMMUState *s, uint16_t domain_id,
                                       uint32_t pasid, hwaddr addr, uint8_t am,
                                       bool ih)
{
    struct iommu_hwpt_vtd_s1_invalidate cache_info = { 0 };
    VTDPIOTLBInvInfo piotlb_info;
    VTDIOTLBPageInvInfo info;
    VTDAddressSpace *vtd_as;
    VTDContextEntry ce;
    hwaddr size = (1 << am) * VTD_PAGE_SIZE;
    int ret;

    cache_info.addr = addr;
    cache_info.npages = 1 << am;
    cache_info.flags = ih ? IOMMU_VTD_INV_FLAGS_LEAF : 0;

    piotlb_info.domain_id = domain_id;
    piotlb_info.pasid = pasid;
    piotlb_info.inv_data = &cache_info;

    info.is_piotlb = true;
    info.domain_id = domain_id;
    info.pasid = pasid;
    info.addr = addr;
    info.mask = ~((1 << am) - 1);

    vtd_iommu_lock(s);
    /*
     * Here loops all the vtd_pasid_as instances in s->vtd_pasid_as
     * to find out the affected devices since piotlb invalidation
     * should check pasid cache per architecture point of view.
     */
    g_hash_table_foreach(s->vtd_pasid_as,
                         vtd_flush_pasid_iotlb, &piotlb_info);
    g_hash_table_foreach_remove(s->p_iotlb,
                                vtd_hash_remove_by_page, &info);
    vtd_iommu_unlock(s);

    QLIST_FOREACH(vtd_as, &(s->vtd_as_with_notifiers), next) {
        ret = vtd_dev_to_context_entry(s, pci_bus_num(vtd_as->bus),
                                       vtd_as->devfn, &ce);
        if (!ret && domain_id == vtd_get_domain_id(s, &ce, vtd_as->pasid)) {
            if (vtd_as_has_map_notifier(vtd_as)) {
                error_report_once("%s: FLT does not do map, should not come"
                                  " here.\n", __func__);
            } else {
                IOMMUTLBEvent event;
                IOMMUTLBEntry entry = {
                    .target_as = &address_space_memory,
                    .iova = addr,
                    .translated_addr = 0,
                    .addr_mask = size - 1,
                    .perm = IOMMU_NONE,
                };

                event.type = IOMMU_NOTIFIER_UNMAP |
                             IOMMU_NOTIFIER_DEVIOTLB_UNMAP;
                event.entry = entry;
                memory_region_notify_iommu(&vtd_as->iommu, 0, event);
            }
        }
    }
}

static bool vtd_process_piotlb_desc(IntelIOMMUState *s,
                                    VTDInvDesc *inv_desc)
{
    uint16_t domain_id;
    uint32_t pasid;
    uint8_t am;
    hwaddr addr;

    if ((inv_desc->val[0] & VTD_INV_DESC_PIOTLB_RSVD_VAL0) ||
        (inv_desc->val[1] & VTD_INV_DESC_PIOTLB_RSVD_VAL1)) {
        error_report_once("non-zero-field-in-piotlb_inv_desc hi: 0x%" PRIx64
                  " lo: 0x%" PRIx64, inv_desc->val[1], inv_desc->val[0]);
        return false;
    }

    domain_id = VTD_INV_DESC_PIOTLB_DID(inv_desc->val[0]);
    pasid = VTD_INV_DESC_PIOTLB_PASID(inv_desc->val[0]);
    switch (inv_desc->val[0] & VTD_INV_DESC_IOTLB_G) {
    case VTD_INV_DESC_PIOTLB_ALL_IN_PASID:
        vtd_piotlb_pasid_invalidate(s, domain_id, pasid);
        break;

    case VTD_INV_DESC_PIOTLB_PSI_IN_PASID:
        am = VTD_INV_DESC_PIOTLB_AM(inv_desc->val[1]);
        addr = (hwaddr) VTD_INV_DESC_PIOTLB_ADDR(inv_desc->val[1]);
        vtd_piotlb_page_invalidate(s, domain_id, pasid, addr, am,
                                   VTD_INV_DESC_PIOTLB_IH(inv_desc->val[1]));
        break;

    default:
        error_report_once("Invalid granularity in P-IOTLB desc hi: 0x%" PRIx64
                  " lo: 0x%" PRIx64, inv_desc->val[1], inv_desc->val[0]);
        return false;
    }
    return true;
}

static bool vtd_process_inv_iec_desc(IntelIOMMUState *s,
                                     VTDInvDesc *inv_desc)
{
    trace_vtd_inv_desc_iec(inv_desc->iec.granularity,
                           inv_desc->iec.index,
                           inv_desc->iec.index_mask);

    vtd_iec_notify_all(s, !inv_desc->iec.granularity,
                       inv_desc->iec.index,
                       inv_desc->iec.index_mask);
    return true;
}

static bool vtd_process_device_piotlb_desc(IntelIOMMUState *s,
                                           VTDInvDesc *inv_desc)
{
    /*
     * no need to handle it for passthru device, for emulated
     * devices with device tlb, it may be required, but for now,
     * return is enough
     */
    return true;
}

static bool vtd_process_device_iotlb_desc(IntelIOMMUState *s,
                                          VTDInvDesc *inv_desc)
{
    VTDAddressSpace *vtd_dev_as;
    IOMMUTLBEvent event;
    hwaddr addr;
    uint64_t sz;
    uint16_t sid;
    bool size;

    addr = VTD_INV_DESC_DEVICE_IOTLB_ADDR(inv_desc->hi);
    sid = VTD_INV_DESC_DEVICE_IOTLB_SID(inv_desc->lo);
    size = VTD_INV_DESC_DEVICE_IOTLB_SIZE(inv_desc->hi);

    if ((inv_desc->lo & VTD_INV_DESC_DEVICE_IOTLB_RSVD_LO) ||
        (inv_desc->hi & VTD_INV_DESC_DEVICE_IOTLB_RSVD_HI)) {
        error_report_once("%s: invalid dev-iotlb inv desc: hi=%"PRIx64
                          ", lo=%"PRIx64" (reserved nonzero)", __func__,
                          inv_desc->hi, inv_desc->lo);
        return false;
    }

    /*
     * Using sid is OK since the guest should have finished the
     * initialization of both the bus and device.
     */
    vtd_dev_as = vtd_get_as_by_sid(s, sid);
    if (!vtd_dev_as) {
        goto done;
    }

    /* According to ATS spec table 2.4:
     * S = 0, bits 15:12 = xxxx     range size: 4K
     * S = 1, bits 15:12 = xxx0     range size: 8K
     * S = 1, bits 15:12 = xx01     range size: 16K
     * S = 1, bits 15:12 = x011     range size: 32K
     * S = 1, bits 15:12 = 0111     range size: 64K
     * ...
     */
    if (size) {
        sz = (VTD_PAGE_SIZE * 2) << cto64(addr >> VTD_PAGE_SHIFT);
        addr &= ~(sz - 1);
    } else {
        sz = VTD_PAGE_SIZE;
    }

    event.type = IOMMU_NOTIFIER_DEVIOTLB_UNMAP;
    event.entry.target_as = &vtd_dev_as->as;
    event.entry.addr_mask = sz - 1;
    event.entry.iova = addr;
    event.entry.perm = IOMMU_NONE;
    event.entry.translated_addr = 0;
    memory_region_notify_iommu(&vtd_dev_as->iommu, 0, event);

done:
    return true;
}

static bool vtd_process_inv_desc(IntelIOMMUState *s)
{
    VTDInvDesc inv_desc;
    uint8_t desc_type;

    trace_vtd_inv_qi_head(s->iq_head);
    if (!vtd_get_inv_desc(s, &inv_desc)) {
        s->iq_last_desc_type = VTD_INV_DESC_NONE;
        return false;
    }

    desc_type = inv_desc.lo & VTD_INV_DESC_TYPE;
    /* FIXME: should update at first or at last? */
    s->iq_last_desc_type = desc_type;

    switch (desc_type) {
    case VTD_INV_DESC_CC:
        trace_vtd_inv_desc("context-cache", inv_desc.hi, inv_desc.lo);
        if (!vtd_process_context_cache_desc(s, &inv_desc)) {
            return false;
        }
        break;

    case VTD_INV_DESC_IOTLB:
        trace_vtd_inv_desc("iotlb", inv_desc.hi, inv_desc.lo);
        if (!vtd_process_iotlb_desc(s, &inv_desc)) {
            return false;
        }
        break;

    case VTD_INV_DESC_PC:
        trace_vtd_inv_desc("pasid-cache", inv_desc.val[1], inv_desc.val[0]);
        if (!vtd_process_pasid_desc(s, &inv_desc)) {
            return false;
        }
        break;

    case VTD_INV_DESC_PIOTLB:
        trace_vtd_inv_desc("p-iotlb", inv_desc.val[1], inv_desc.val[0]);
        if (!vtd_process_piotlb_desc(s, &inv_desc)) {
            return false;
        }
        break;

    case VTD_INV_DESC_WAIT:
        trace_vtd_inv_desc("wait", inv_desc.hi, inv_desc.lo);
        if (!vtd_process_wait_desc(s, &inv_desc)) {
            return false;
        }
        break;

    case VTD_INV_DESC_IEC:
        trace_vtd_inv_desc("iec", inv_desc.hi, inv_desc.lo);
        if (!vtd_process_inv_iec_desc(s, &inv_desc)) {
            return false;
        }
        break;

    case VTD_INV_DESC_DEV_PIOTLB:
        trace_vtd_inv_desc("device-piotlb", inv_desc.hi, inv_desc.lo);
        if (!vtd_process_device_piotlb_desc(s, &inv_desc)) {
            return false;
        }
        break;

    case VTD_INV_DESC_DEVICE:
        trace_vtd_inv_desc("device", inv_desc.hi, inv_desc.lo);
        if (!vtd_process_device_iotlb_desc(s, &inv_desc)) {
            return false;
        }
        break;

    default:
        error_report_once("%s: invalid inv desc: hi=%"PRIx64", lo=%"PRIx64
                          " (unknown type)", __func__, inv_desc.hi,
                          inv_desc.lo);
        return false;
    }
    s->iq_head++;
    if (s->iq_head == s->iq_size) {
        s->iq_head = 0;
    }
    return true;
}

/* Try to fetch and process more Invalidation Descriptors */
static void vtd_fetch_inv_desc(IntelIOMMUState *s)
{
    int qi_shift;

    /* Refer to 10.4.23 of VT-d spec 3.0 */
    qi_shift = s->iq_dw ? VTD_IQH_QH_SHIFT_5 : VTD_IQH_QH_SHIFT_4;

    trace_vtd_inv_qi_fetch();

    if (s->iq_tail >= s->iq_size) {
        /* Detects an invalid Tail pointer */
        error_report_once("%s: detected invalid QI tail "
                          "(tail=0x%x, size=0x%x)",
                          __func__, s->iq_tail, s->iq_size);
        vtd_handle_inv_queue_error(s);
        return;
    }
    while (s->iq_head != s->iq_tail) {
        if (!vtd_process_inv_desc(s)) {
            /* Invalidation Queue Errors */
            vtd_handle_inv_queue_error(s);
            break;
        }
        /* Must update the IQH_REG in time */
        vtd_set_quad_raw(s, DMAR_IQH_REG,
                         (((uint64_t)(s->iq_head)) << qi_shift) &
                         VTD_IQH_QH_MASK);
    }
}

/* Handle write to Invalidation Queue Tail Register */
static void vtd_handle_iqt_write(IntelIOMMUState *s)
{
    uint64_t val = vtd_get_quad_raw(s, DMAR_IQT_REG);

    if (s->iq_dw && (val & VTD_IQT_QT_256_RSV_BIT)) {
        error_report_once("%s: RSV bit is set: val=0x%"PRIx64,
                          __func__, val);
        return;
    }
    s->iq_tail = VTD_IQT_QT(s->iq_dw, val);
    trace_vtd_inv_qi_tail(s->iq_tail);

    if (s->qi_enabled && !(vtd_get_long_raw(s, DMAR_FSTS_REG) & VTD_FSTS_IQE)) {
        /* Process Invalidation Queue here */
        vtd_fetch_inv_desc(s);
    }
}

static void vtd_handle_fsts_write(IntelIOMMUState *s)
{
    uint32_t fsts_reg = vtd_get_long_raw(s, DMAR_FSTS_REG);
    uint32_t fectl_reg = vtd_get_long_raw(s, DMAR_FECTL_REG);
    uint32_t status_fields = VTD_FSTS_PFO | VTD_FSTS_PPF | VTD_FSTS_IQE;

    if ((fectl_reg & VTD_FECTL_IP) && !(fsts_reg & status_fields)) {
        vtd_set_clear_mask_long(s, DMAR_FECTL_REG, VTD_FECTL_IP, 0);
        trace_vtd_fsts_clear_ip();
    }
    /* FIXME: when IQE is Clear, should we try to fetch some Invalidation
     * Descriptors if there are any when Queued Invalidation is enabled?
     */
}

static void vtd_handle_fectl_write(IntelIOMMUState *s)
{
    uint32_t fectl_reg;
    /* FIXME: when software clears the IM field, check the IP field. But do we
     * need to compare the old value and the new value to conclude that
     * software clears the IM field? Or just check if the IM field is zero?
     */
    fectl_reg = vtd_get_long_raw(s, DMAR_FECTL_REG);

    trace_vtd_reg_write_fectl(fectl_reg);

    if ((fectl_reg & VTD_FECTL_IP) && !(fectl_reg & VTD_FECTL_IM)) {
        vtd_generate_interrupt(s, DMAR_FEADDR_REG, DMAR_FEDATA_REG);
        vtd_set_clear_mask_long(s, DMAR_FECTL_REG, VTD_FECTL_IP, 0);
    }
}

static void vtd_handle_ics_write(IntelIOMMUState *s)
{
    uint32_t ics_reg = vtd_get_long_raw(s, DMAR_ICS_REG);
    uint32_t iectl_reg = vtd_get_long_raw(s, DMAR_IECTL_REG);

    if ((iectl_reg & VTD_IECTL_IP) && !(ics_reg & VTD_ICS_IWC)) {
        trace_vtd_reg_ics_clear_ip();
        vtd_set_clear_mask_long(s, DMAR_IECTL_REG, VTD_IECTL_IP, 0);
    }
}

static void vtd_handle_iectl_write(IntelIOMMUState *s)
{
    uint32_t iectl_reg;
    /* FIXME: when software clears the IM field, check the IP field. But do we
     * need to compare the old value and the new value to conclude that
     * software clears the IM field? Or just check if the IM field is zero?
     */
    iectl_reg = vtd_get_long_raw(s, DMAR_IECTL_REG);

    trace_vtd_reg_write_iectl(iectl_reg);

    if ((iectl_reg & VTD_IECTL_IP) && !(iectl_reg & VTD_IECTL_IM)) {
        vtd_generate_interrupt(s, DMAR_IEADDR_REG, DMAR_IEDATA_REG);
        vtd_set_clear_mask_long(s, DMAR_IECTL_REG, VTD_IECTL_IP, 0);
    }
}

static uint64_t vtd_mem_read(void *opaque, hwaddr addr, unsigned size)
{
    IntelIOMMUState *s = opaque;
    uint64_t val;

    trace_vtd_reg_read(addr, size);

    if (addr + size > DMAR_REG_SIZE) {
        error_report_once("%s: MMIO over range: addr=0x%" PRIx64
                          " size=0x%x", __func__, addr, size);
        return (uint64_t)-1;
    }

    switch (addr) {
    /* Root Table Address Register, 64-bit */
    case DMAR_RTADDR_REG:
        val = vtd_get_quad_raw(s, DMAR_RTADDR_REG);
        if (size == 4) {
            val = val & ((1ULL << 32) - 1);
        }
        break;

    case DMAR_RTADDR_REG_HI:
        assert(size == 4);
        val = vtd_get_quad_raw(s, DMAR_RTADDR_REG) >> 32;
        break;

    /* Invalidation Queue Address Register, 64-bit */
    case DMAR_IQA_REG:
        val = s->iq | (vtd_get_quad(s, DMAR_IQA_REG) & (VTD_IQA_QS
                    | VTD_IQA_DW_MASK));
        if (size == 4) {
            val = val & ((1ULL << 32) - 1);
        }
        break;

    case DMAR_IQA_REG_HI:
        assert(size == 4);
        val = s->iq >> 32;
        break;

    default:
        if (size == 4) {
            val = vtd_get_long(s, addr);
        } else {
            val = vtd_get_quad(s, addr);
        }
    }

    return val;
}

static void vtd_mem_write(void *opaque, hwaddr addr,
                          uint64_t val, unsigned size)
{
    IntelIOMMUState *s = opaque;

    trace_vtd_reg_write(addr, size, val);

    if (addr + size > DMAR_REG_SIZE) {
        error_report_once("%s: MMIO over range: addr=0x%" PRIx64
                          " size=0x%x", __func__, addr, size);
        return;
    }

    switch (addr) {
    /* Global Command Register, 32-bit */
    case DMAR_GCMD_REG:
        vtd_set_long(s, addr, val);
        vtd_handle_gcmd_write(s);
        break;

    /* Context Command Register, 64-bit */
    case DMAR_CCMD_REG:
        if (size == 4) {
            vtd_set_long(s, addr, val);
        } else {
            vtd_set_quad(s, addr, val);
            vtd_handle_ccmd_write(s);
        }
        break;

    case DMAR_CCMD_REG_HI:
        assert(size == 4);
        vtd_set_long(s, addr, val);
        vtd_handle_ccmd_write(s);
        break;

    /* IOTLB Invalidation Register, 64-bit */
    case DMAR_IOTLB_REG:
        if (size == 4) {
            vtd_set_long(s, addr, val);
        } else {
            vtd_set_quad(s, addr, val);
            vtd_handle_iotlb_write(s);
        }
        break;

    case DMAR_IOTLB_REG_HI:
        assert(size == 4);
        vtd_set_long(s, addr, val);
        vtd_handle_iotlb_write(s);
        break;

    /* Invalidate Address Register, 64-bit */
    case DMAR_IVA_REG:
        if (size == 4) {
            vtd_set_long(s, addr, val);
        } else {
            vtd_set_quad(s, addr, val);
        }
        break;

    case DMAR_IVA_REG_HI:
        assert(size == 4);
        vtd_set_long(s, addr, val);
        break;

    /* Fault Status Register, 32-bit */
    case DMAR_FSTS_REG:
        assert(size == 4);
        vtd_set_long(s, addr, val);
        vtd_handle_fsts_write(s);
        break;

    /* Fault Event Control Register, 32-bit */
    case DMAR_FECTL_REG:
        assert(size == 4);
        vtd_set_long(s, addr, val);
        vtd_handle_fectl_write(s);
        break;

    /* Fault Event Data Register, 32-bit */
    case DMAR_FEDATA_REG:
        assert(size == 4);
        vtd_set_long(s, addr, val);
        break;

    /* Fault Event Address Register, 32-bit */
    case DMAR_FEADDR_REG:
        if (size == 4) {
            vtd_set_long(s, addr, val);
        } else {
            /*
             * While the register is 32-bit only, some guests (Xen...) write to
             * it with 64-bit.
             */
            vtd_set_quad(s, addr, val);
        }
        break;

    /* Fault Event Upper Address Register, 32-bit */
    case DMAR_FEUADDR_REG:
        assert(size == 4);
        vtd_set_long(s, addr, val);
        break;

    /* Protected Memory Enable Register, 32-bit */
    case DMAR_PMEN_REG:
        assert(size == 4);
        vtd_set_long(s, addr, val);
        break;

    /* Root Table Address Register, 64-bit */
    case DMAR_RTADDR_REG:
        if (size == 4) {
            vtd_set_long(s, addr, val);
        } else {
            vtd_set_quad(s, addr, val);
        }
        break;

    case DMAR_RTADDR_REG_HI:
        assert(size == 4);
        vtd_set_long(s, addr, val);
        break;

    /* Invalidation Queue Tail Register, 64-bit */
    case DMAR_IQT_REG:
        if (size == 4) {
            vtd_set_long(s, addr, val);
        } else {
            vtd_set_quad(s, addr, val);
        }
        vtd_handle_iqt_write(s);
        break;

    case DMAR_IQT_REG_HI:
        assert(size == 4);
        vtd_set_long(s, addr, val);
        /* 19:63 of IQT_REG is RsvdZ, do nothing here */
        break;

    /* Invalidation Queue Address Register, 64-bit */
    case DMAR_IQA_REG:
        if (size == 4) {
            vtd_set_long(s, addr, val);
        } else {
            vtd_set_quad(s, addr, val);
        }
        vtd_update_iq_dw(s);
        break;

    case DMAR_IQA_REG_HI:
        assert(size == 4);
        vtd_set_long(s, addr, val);
        break;

    /* Invalidation Completion Status Register, 32-bit */
    case DMAR_ICS_REG:
        assert(size == 4);
        vtd_set_long(s, addr, val);
        vtd_handle_ics_write(s);
        break;

    /* Invalidation Event Control Register, 32-bit */
    case DMAR_IECTL_REG:
        assert(size == 4);
        vtd_set_long(s, addr, val);
        vtd_handle_iectl_write(s);
        break;

    /* Invalidation Event Data Register, 32-bit */
    case DMAR_IEDATA_REG:
        assert(size == 4);
        vtd_set_long(s, addr, val);
        break;

    /* Invalidation Event Address Register, 32-bit */
    case DMAR_IEADDR_REG:
        assert(size == 4);
        vtd_set_long(s, addr, val);
        break;

    /* Invalidation Event Upper Address Register, 32-bit */
    case DMAR_IEUADDR_REG:
        assert(size == 4);
        vtd_set_long(s, addr, val);
        break;

    /* Fault Recording Registers, 128-bit */
    case DMAR_FRCD_REG_0_0:
        if (size == 4) {
            vtd_set_long(s, addr, val);
        } else {
            vtd_set_quad(s, addr, val);
        }
        break;

    case DMAR_FRCD_REG_0_1:
        assert(size == 4);
        vtd_set_long(s, addr, val);
        break;

    case DMAR_FRCD_REG_0_2:
        if (size == 4) {
            vtd_set_long(s, addr, val);
        } else {
            vtd_set_quad(s, addr, val);
            /* May clear bit 127 (Fault), update PPF */
            vtd_update_fsts_ppf(s);
        }
        break;

    case DMAR_FRCD_REG_0_3:
        assert(size == 4);
        vtd_set_long(s, addr, val);
        /* May clear bit 127 (Fault), update PPF */
        vtd_update_fsts_ppf(s);
        break;

    case DMAR_IRTA_REG:
        if (size == 4) {
            vtd_set_long(s, addr, val);
        } else {
            vtd_set_quad(s, addr, val);
        }
        break;

    case DMAR_IRTA_REG_HI:
        assert(size == 4);
        vtd_set_long(s, addr, val);
        break;

    default:
        if (size == 4) {
            vtd_set_long(s, addr, val);
        } else {
            vtd_set_quad(s, addr, val);
        }
    }
}

static IOMMUTLBEntry vtd_iommu_translate(IOMMUMemoryRegion *iommu, hwaddr addr,
                                         IOMMUAccessFlags flag, int iommu_idx)
{
    VTDAddressSpace *vtd_as = container_of(iommu, VTDAddressSpace, iommu);
    IntelIOMMUState *s = vtd_as->iommu_state;
    IOMMUTLBEntry iotlb = {
        /* We'll fill in the rest later. */
        .target_as = &address_space_memory,
    };
    bool success;
    VTDContextEntry ce;
    VTDPASIDEntry pe;
    int ret = 0;

    if (likely(s->dmar_enabled)) {
        if (s->root_scalable) {
            ret = vtd_dev_to_context_entry(s, pci_bus_num(vtd_as->bus),
                                           vtd_as->devfn, &ce);
            ret = vtd_ce_get_rid2pasid_entry(s, &ce, &pe, PCI_NO_PASID);
            if (ret) {
                error_report_once("%s: detected translation failure 1 "
                                  "(dev=%02x:%02x:%02x, iova=0x%" PRIx64 ")",
                                  __func__, pci_bus_num(vtd_as->bus),
                                  VTD_PCI_SLOT(vtd_as->devfn),
                                  VTD_PCI_FUNC(vtd_as->devfn),
                                  addr);
                return iotlb;
            }
            if (VTD_PE_GET_TYPE(&pe) == VTD_SM_PASID_ENTRY_FLT) {
                success = vtd_do_iommu_fl_translate(vtd_as, vtd_as->bus,
                                                    vtd_as->devfn, addr,
                                                    flag & IOMMU_WO, &iotlb);
            } else {
                success = vtd_do_iommu_translate(vtd_as, vtd_as->bus,
                                                 vtd_as->devfn, addr,
                                                 flag & IOMMU_WO, &iotlb);
            }
        } else {
            success = vtd_do_iommu_translate(vtd_as, vtd_as->bus, vtd_as->devfn,
                                             addr, flag & IOMMU_WO, &iotlb);
        }
    } else {
        /* DMAR disabled, passthrough, use 4k-page*/
        iotlb.iova = addr & VTD_PAGE_MASK_4K;
        iotlb.translated_addr = addr & VTD_PAGE_MASK_4K;
        iotlb.addr_mask = ~VTD_PAGE_MASK_4K;
        iotlb.perm = IOMMU_RW;
        success = true;
    }

    if (likely(success)) {
        trace_vtd_dmar_translate(pci_bus_num(vtd_as->bus),
                                 VTD_PCI_SLOT(vtd_as->devfn),
                                 VTD_PCI_FUNC(vtd_as->devfn),
                                 iotlb.iova, iotlb.translated_addr,
                                 iotlb.addr_mask);
    } else {
        error_report_once("%s: detected translation failure "
                          "(dev=%02x:%02x:%02x, iova=0x%" PRIx64 ")",
                          __func__, pci_bus_num(vtd_as->bus),
                          VTD_PCI_SLOT(vtd_as->devfn),
                          VTD_PCI_FUNC(vtd_as->devfn),
                          addr);
    }

    return iotlb;
}

static int vtd_iommu_notify_flag_changed(IOMMUMemoryRegion *iommu,
                                         IOMMUNotifierFlag old,
                                         IOMMUNotifierFlag new,
                                         Error **errp)
{
    VTDAddressSpace *vtd_as = container_of(iommu, VTDAddressSpace, iommu);
    IntelIOMMUState *s = vtd_as->iommu_state;
    X86IOMMUState *x86_iommu = X86_IOMMU_DEVICE(s);

    /* TODO: add support for VFIO and vhost users */
    if (s->snoop_control) {
        error_setg_errno(errp, ENOTSUP,
                         "Snoop Control with vhost or VFIO is not supported");
        return -ENOTSUP;
    }
    if (!s->caching_mode && (new & IOMMU_NOTIFIER_MAP)) {
        error_setg_errno(errp, ENOTSUP,
                         "device %02x.%02x.%x requires caching mode",
                         pci_bus_num(vtd_as->bus), PCI_SLOT(vtd_as->devfn),
                         PCI_FUNC(vtd_as->devfn));
        return -ENOTSUP;
    }
    if (!x86_iommu->dt_supported && (new & IOMMU_NOTIFIER_DEVIOTLB_UNMAP)) {
        error_setg_errno(errp, ENOTSUP,
                         "device %02x.%02x.%x requires device IOTLB mode",
                         pci_bus_num(vtd_as->bus), PCI_SLOT(vtd_as->devfn),
                         PCI_FUNC(vtd_as->devfn));
        return -ENOTSUP;
    }

    /* Update per-address-space notifier flags */
    vtd_as->notifier_flags = new;

    if (old == IOMMU_NOTIFIER_NONE) {
        QLIST_INSERT_HEAD(&s->vtd_as_with_notifiers, vtd_as, next);
    } else if (new == IOMMU_NOTIFIER_NONE) {
        QLIST_REMOVE(vtd_as, next);
    }
    return 0;
}

static int vtd_post_load(void *opaque, int version_id)
{
    IntelIOMMUState *iommu = opaque;

    /*
     * We don't need to migrate the root_scalable because we can
     * simply do the calculation after the loading is complete.  We
     * can actually do similar things with root, dmar_enabled, etc.
     * however since we've had them already so we'd better keep them
     * for compatibility of migration.
     */
    vtd_update_scalable_state(iommu);

    vtd_update_iq_dw(iommu);

    /*
     * Memory regions are dynamically turned on/off depending on
     * context entry configurations from the guest. After migration,
     * we need to make sure the memory regions are still correct.
     */
    vtd_switch_address_space_all(iommu);

    return 0;
}

static const VMStateDescription vtd_vmstate = {
    .name = "iommu-intel",
    .version_id = 1,
    .minimum_version_id = 1,
    .priority = MIG_PRI_IOMMU,
    .post_load = vtd_post_load,
    .fields = (VMStateField[]) {
        VMSTATE_UINT64(root, IntelIOMMUState),
        VMSTATE_UINT64(intr_root, IntelIOMMUState),
        VMSTATE_UINT64(iq, IntelIOMMUState),
        VMSTATE_UINT32(intr_size, IntelIOMMUState),
        VMSTATE_UINT16(iq_head, IntelIOMMUState),
        VMSTATE_UINT16(iq_tail, IntelIOMMUState),
        VMSTATE_UINT16(iq_size, IntelIOMMUState),
        VMSTATE_UINT16(next_frcd_reg, IntelIOMMUState),
        VMSTATE_UINT8_ARRAY(csr, IntelIOMMUState, DMAR_REG_SIZE),
        VMSTATE_UINT8(iq_last_desc_type, IntelIOMMUState),
        VMSTATE_UNUSED(1),      /* bool root_extended is obsolete by VT-d */
        VMSTATE_BOOL(dmar_enabled, IntelIOMMUState),
        VMSTATE_BOOL(qi_enabled, IntelIOMMUState),
        VMSTATE_BOOL(intr_enabled, IntelIOMMUState),
        VMSTATE_BOOL(intr_eime, IntelIOMMUState),
        VMSTATE_END_OF_LIST()
    }
};

static const MemoryRegionOps vtd_mem_ops = {
    .read = vtd_mem_read,
    .write = vtd_mem_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 4,
        .max_access_size = 8,
    },
    .valid = {
        .min_access_size = 4,
        .max_access_size = 8,
    },
};

static Property vtd_properties[] = {
    DEFINE_PROP_UINT32("version", IntelIOMMUState, version, 0),
    DEFINE_PROP_ON_OFF_AUTO("eim", IntelIOMMUState, intr_eim,
                            ON_OFF_AUTO_AUTO),
    DEFINE_PROP_BOOL("x-buggy-eim", IntelIOMMUState, buggy_eim, false),
    DEFINE_PROP_UINT8("aw-bits", IntelIOMMUState, aw_bits,
                      VTD_HOST_ADDRESS_WIDTH),
    DEFINE_PROP_BOOL("caching-mode", IntelIOMMUState, caching_mode, FALSE),
    DEFINE_PROP_STRING("x-scalable-mode", IntelIOMMUState, scalable_mode_str),
    DEFINE_PROP_BOOL("snoop-control", IntelIOMMUState, snoop_control, false),
    DEFINE_PROP_BOOL("x-pasid-mode", IntelIOMMUState, pasid, false),
    DEFINE_PROP_BOOL("dma-drain", IntelIOMMUState, dma_drain, true),
    DEFINE_PROP_BOOL("dma-translation", IntelIOMMUState, dma_translation, true),
    DEFINE_PROP_END_OF_LIST(),
};

/* Read IRTE entry with specific index */
static bool vtd_irte_get(IntelIOMMUState *iommu, uint16_t index,
                         VTD_IR_TableEntry *entry, uint16_t sid,
                         bool do_fault)
{
    static const uint16_t vtd_svt_mask[VTD_SQ_MAX] = \
        {0xffff, 0xfffb, 0xfff9, 0xfff8};
    dma_addr_t addr = 0x00;
    uint16_t mask, source_id;
    uint8_t bus, bus_max, bus_min;

    if (index >= iommu->intr_size) {
        error_report_once("%s: index too large: ind=0x%x",
                          __func__, index);
        if (do_fault) {
            vtd_report_ir_fault(iommu, sid, VTD_FR_IR_INDEX_OVER, index);
        }
        return false;
    }

    addr = iommu->intr_root + index * sizeof(*entry);
    if (dma_memory_read(&address_space_memory, addr,
                        entry, sizeof(*entry), MEMTXATTRS_UNSPECIFIED)) {
        error_report_once("%s: read failed: ind=0x%x addr=0x%" PRIx64,
                          __func__, index, addr);
        if (do_fault) {
            vtd_report_ir_fault(iommu, sid, VTD_FR_IR_ROOT_INVAL, index);
        }
        return false;
    }

    entry->data[0] = le64_to_cpu(entry->data[0]);
    entry->data[1] = le64_to_cpu(entry->data[1]);

    trace_vtd_ir_irte_get(index, entry->data[1], entry->data[0]);

    /*
     * The remaining potential fault conditions are "qualified" by the
     * Fault Processing Disable bit in the IRTE. Even "not present".
     * So just clear the do_fault flag if PFD is set, which will
     * prevent faults being raised.
     */
    if (entry->irte.fault_disable) {
        do_fault = false;
    }

    if (!entry->irte.present) {
        error_report_once("%s: detected non-present IRTE "
                          "(index=%u, high=0x%" PRIx64 ", low=0x%" PRIx64 ")",
                          __func__, index, entry->data[1], entry->data[0]);
        if (do_fault) {
            vtd_report_ir_fault(iommu, sid, VTD_FR_IR_ENTRY_P, index);
        }
        return false;
    }

    if (entry->irte.__reserved_0 || entry->irte.__reserved_1 ||
        entry->irte.__reserved_2) {
        error_report_once("%s: detected non-zero reserved IRTE "
                          "(index=%u, high=0x%" PRIx64 ", low=0x%" PRIx64 ")",
                          __func__, index, entry->data[1], entry->data[0]);
        if (do_fault) {
            vtd_report_ir_fault(iommu, sid, VTD_FR_IR_IRTE_RSVD, index);
        }
        return false;
    }

    if (sid != X86_IOMMU_SID_INVALID) {
        /* Validate IRTE SID */
        source_id = entry->irte.source_id;
        switch (entry->irte.sid_vtype) {
        case VTD_SVT_NONE:
            break;

        case VTD_SVT_ALL:
            mask = vtd_svt_mask[entry->irte.sid_q];
            if ((source_id & mask) != (sid & mask)) {
                error_report_once("%s: invalid IRTE SID "
                                  "(index=%u, sid=%u, source_id=%u)",
                                  __func__, index, sid, source_id);
                if (do_fault) {
                    vtd_report_ir_fault(iommu, sid, VTD_FR_IR_SID_ERR, index);
                }
                return false;
            }
            break;

        case VTD_SVT_BUS:
            bus_max = source_id >> 8;
            bus_min = source_id & 0xff;
            bus = sid >> 8;
            if (bus > bus_max || bus < bus_min) {
                error_report_once("%s: invalid SVT_BUS "
                                  "(index=%u, bus=%u, min=%u, max=%u)",
                                  __func__, index, bus, bus_min, bus_max);
                if (do_fault) {
                    vtd_report_ir_fault(iommu, sid, VTD_FR_IR_SID_ERR, index);
                }
                return false;
            }
            break;

        default:
            error_report_once("%s: detected invalid IRTE SVT "
                              "(index=%u, type=%d)", __func__,
                              index, entry->irte.sid_vtype);
            /* Take this as verification failure. */
            if (do_fault) {
                vtd_report_ir_fault(iommu, sid, VTD_FR_IR_SID_ERR, index);
            }
            return false;
        }
    }

    return true;
}

/* Fetch IRQ information of specific IR index */
static bool vtd_remap_irq_get(IntelIOMMUState *iommu, uint16_t index,
                              X86IOMMUIrq *irq, uint16_t sid, bool do_fault)
{
    VTD_IR_TableEntry irte = {};

    if (!vtd_irte_get(iommu, index, &irte, sid, do_fault)) {
        return false;
    }

    irq->trigger_mode = irte.irte.trigger_mode;
    irq->vector = irte.irte.vector;
    irq->delivery_mode = irte.irte.delivery_mode;
    irq->dest = irte.irte.dest_id;
    if (!iommu->intr_eime) {
#define  VTD_IR_APIC_DEST_MASK         (0xff00ULL)
#define  VTD_IR_APIC_DEST_SHIFT        (8)
        irq->dest = (irq->dest & VTD_IR_APIC_DEST_MASK) >>
            VTD_IR_APIC_DEST_SHIFT;
    }
    irq->dest_mode = irte.irte.dest_mode;
    irq->redir_hint = irte.irte.redir_hint;

    trace_vtd_ir_remap(index, irq->trigger_mode, irq->vector,
                       irq->delivery_mode, irq->dest, irq->dest_mode);

    return true;
}

/* Interrupt remapping for MSI/MSI-X entry */
static int vtd_interrupt_remap_msi(IntelIOMMUState *iommu,
                                   MSIMessage *origin,
                                   MSIMessage *translated,
                                   uint16_t sid, bool do_fault)
{
    VTD_IR_MSIAddress addr;
    uint16_t index;
    X86IOMMUIrq irq = {};

    assert(origin && translated);

    trace_vtd_ir_remap_msi_req(origin->address, origin->data);

    if (!iommu || !iommu->intr_enabled) {
        memcpy(translated, origin, sizeof(*origin));
        goto out;
    }

    if (origin->address & VTD_MSI_ADDR_HI_MASK) {
        error_report_once("%s: MSI address high 32 bits non-zero detected: "
                          "address=0x%" PRIx64, __func__, origin->address);
        if (do_fault) {
            vtd_report_ir_fault(iommu, sid, VTD_FR_IR_REQ_RSVD, 0);
        }
        return -EINVAL;
    }

    addr.data = origin->address & VTD_MSI_ADDR_LO_MASK;
    if (addr.addr.__head != 0xfee) {
        error_report_once("%s: MSI address low 32 bit invalid: 0x%" PRIx32,
                          __func__, addr.data);
        if (do_fault) {
            vtd_report_ir_fault(iommu, sid, VTD_FR_IR_REQ_RSVD, 0);
        }
        return -EINVAL;
    }

    /* This is compatible mode. */
    if (addr.addr.int_mode != VTD_IR_INT_FORMAT_REMAP) {
        memcpy(translated, origin, sizeof(*origin));
        goto out;
    }

    index = addr.addr.index_h << 15 | addr.addr.index_l;

#define  VTD_IR_MSI_DATA_SUBHANDLE       (0x0000ffff)
#define  VTD_IR_MSI_DATA_RESERVED        (0xffff0000)

    if (addr.addr.sub_valid) {
        /* See VT-d spec 5.1.2.2 and 5.1.3 on subhandle */
        index += origin->data & VTD_IR_MSI_DATA_SUBHANDLE;
    }

    if (!vtd_remap_irq_get(iommu, index, &irq, sid, do_fault)) {
        return -EINVAL;
    }

    if (addr.addr.sub_valid) {
        trace_vtd_ir_remap_type("MSI");
        if (origin->data & VTD_IR_MSI_DATA_RESERVED) {
            error_report_once("%s: invalid IR MSI "
                              "(sid=%u, address=0x%" PRIx64
                              ", data=0x%" PRIx32 ")",
                              __func__, sid, origin->address, origin->data);
            if (do_fault) {
                vtd_report_ir_fault(iommu, sid, VTD_FR_IR_REQ_RSVD, 0);
            }
            return -EINVAL;
        }
    } else {
        uint8_t vector = origin->data & 0xff;
        uint8_t trigger_mode = (origin->data >> MSI_DATA_TRIGGER_SHIFT) & 0x1;

        trace_vtd_ir_remap_type("IOAPIC");
        /* IOAPIC entry vector should be aligned with IRTE vector
         * (see vt-d spec 5.1.5.1). */
        if (vector != irq.vector) {
            trace_vtd_warn_ir_vector(sid, index, vector, irq.vector);
        }

        /* The Trigger Mode field must match the Trigger Mode in the IRTE.
         * (see vt-d spec 5.1.5.1). */
        if (trigger_mode != irq.trigger_mode) {
            trace_vtd_warn_ir_trigger(sid, index, trigger_mode,
                                      irq.trigger_mode);
        }
    }

    /*
     * We'd better keep the last two bits, assuming that guest OS
     * might modify it. Keep it does not hurt after all.
     */
    irq.msi_addr_last_bits = addr.addr.__not_care;

    /* Translate X86IOMMUIrq to MSI message */
    x86_iommu_irq_to_msi_message(&irq, translated);

out:
    trace_vtd_ir_remap_msi(origin->address, origin->data,
                           translated->address, translated->data);
    return 0;
}

static int vtd_int_remap(X86IOMMUState *iommu, MSIMessage *src,
                         MSIMessage *dst, uint16_t sid)
{
    return vtd_interrupt_remap_msi(INTEL_IOMMU_DEVICE(iommu),
                                   src, dst, sid, false);
}

static MemTxResult vtd_mem_ir_read(void *opaque, hwaddr addr,
                                   uint64_t *data, unsigned size,
                                   MemTxAttrs attrs)
{
    return MEMTX_OK;
}

static MemTxResult vtd_mem_ir_write(void *opaque, hwaddr addr,
                                    uint64_t value, unsigned size,
                                    MemTxAttrs attrs)
{
    int ret = 0;
    MSIMessage from = {}, to = {};
    uint16_t sid = X86_IOMMU_SID_INVALID;

    from.address = (uint64_t) addr + VTD_INTERRUPT_ADDR_FIRST;
    from.data = (uint32_t) value;

    if (!attrs.unspecified) {
        /* We have explicit Source ID */
        sid = attrs.requester_id;
    }

    ret = vtd_interrupt_remap_msi(opaque, &from, &to, sid, true);
    if (ret) {
        /* Drop this interrupt */
        return MEMTX_ERROR;
    }

    apic_get_class(NULL)->send_msi(&to);

    return MEMTX_OK;
}

static const MemoryRegionOps vtd_mem_ir_ops = {
    .read_with_attrs = vtd_mem_ir_read,
    .write_with_attrs = vtd_mem_ir_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void vtd_report_ir_illegal_access(VTDAddressSpace *vtd_as,
                                         hwaddr addr, bool is_write)
{
    IntelIOMMUState *s = vtd_as->iommu_state;
    uint8_t bus_n = pci_bus_num(vtd_as->bus);
    uint16_t sid = PCI_BUILD_BDF(bus_n, vtd_as->devfn);
    bool is_fpd_set = false;
    VTDContextEntry ce;

    assert(vtd_as->pasid != PCI_NO_PASID);

    /* Try out best to fetch FPD, we can't do anything more */
    if (vtd_dev_to_context_entry(s, bus_n, vtd_as->devfn, &ce) == 0) {
        is_fpd_set = ce.lo & VTD_CONTEXT_ENTRY_FPD;
        if (!is_fpd_set && s->root_scalable) {
            vtd_ce_get_pasid_fpd(s, &ce, &is_fpd_set, vtd_as->pasid);
        }
    }

    vtd_report_fault(s, VTD_FR_SM_INTERRUPT_ADDR,
                     is_fpd_set, sid, addr, is_write,
                     true, vtd_as->pasid);
}

static MemTxResult vtd_mem_ir_fault_read(void *opaque, hwaddr addr,
                                         uint64_t *data, unsigned size,
                                         MemTxAttrs attrs)
{
    vtd_report_ir_illegal_access(opaque, addr, false);

    return MEMTX_ERROR;
}

static MemTxResult vtd_mem_ir_fault_write(void *opaque, hwaddr addr,
                                          uint64_t value, unsigned size,
                                          MemTxAttrs attrs)
{
    vtd_report_ir_illegal_access(opaque, addr, true);

    return MEMTX_ERROR;
}

static const MemoryRegionOps vtd_mem_ir_fault_ops = {
    .read_with_attrs = vtd_mem_ir_fault_read,
    .write_with_attrs = vtd_mem_ir_fault_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
};

VTDAddressSpace *vtd_find_add_as(IntelIOMMUState *s, PCIBus *bus,
                                 int devfn, unsigned int pasid)
{
    /*
     * We can't simply use sid here since the bus number might not be
     * initialized by the guest.
     */
    struct vtd_as_key key = {
        .bus = bus,
        .devfn = devfn,
        .pasid = pasid,
    };
    VTDAddressSpace *vtd_dev_as;
    char name[128];

    vtd_dev_as = g_hash_table_lookup(s->vtd_address_spaces, &key);
    if (!vtd_dev_as) {
        struct vtd_as_key *new_key = g_malloc(sizeof(*new_key));

        new_key->bus = bus;
        new_key->devfn = devfn;
        new_key->pasid = pasid;

        if (pasid == PCI_NO_PASID) {
            snprintf(name, sizeof(name), "vtd-%02x.%x", PCI_SLOT(devfn),
                     PCI_FUNC(devfn));
        } else {
            snprintf(name, sizeof(name), "vtd-%02x.%x-pasid-%x", PCI_SLOT(devfn),
                     PCI_FUNC(devfn), pasid);
        }

        vtd_dev_as = g_new0(VTDAddressSpace, 1);

        vtd_dev_as->bus = bus;
        vtd_dev_as->devfn = (uint8_t)devfn;
        vtd_dev_as->pasid = pasid;
        vtd_dev_as->iommu_state = s;
        vtd_dev_as->context_cache_entry.context_cache_gen = 0;
        vtd_dev_as->iova_tree = iova_tree_new();

        memory_region_init(&vtd_dev_as->root, OBJECT(s), name, UINT64_MAX);
        address_space_init(&vtd_dev_as->as, &vtd_dev_as->root, "vtd-root");

        /*
         * Build the DMAR-disabled container with aliases to the
         * shared MRs.  Note that aliasing to a shared memory region
         * could help the memory API to detect same FlatViews so we
         * can have devices to share the same FlatView when DMAR is
         * disabled (either by not providing "intel_iommu=on" or with
         * "iommu=pt").  It will greatly reduce the total number of
         * FlatViews of the system hence VM runs faster.
         */
        memory_region_init_alias(&vtd_dev_as->nodmar, OBJECT(s),
                                 "vtd-nodmar", &s->mr_nodmar, 0,
                                 memory_region_size(&s->mr_nodmar));

        /*
         * Build the per-device DMAR-enabled container.
         *
         * TODO: currently we have per-device IOMMU memory region only
         * because we have per-device IOMMU notifiers for devices.  If
         * one day we can abstract the IOMMU notifiers out of the
         * memory regions then we can also share the same memory
         * region here just like what we've done above with the nodmar
         * region.
         */
        strcat(name, "-dmar");
        memory_region_init_iommu(&vtd_dev_as->iommu, sizeof(vtd_dev_as->iommu),
                                 TYPE_INTEL_IOMMU_MEMORY_REGION, OBJECT(s),
                                 name, UINT64_MAX);
        memory_region_init_alias(&vtd_dev_as->iommu_ir, OBJECT(s), "vtd-ir",
                                 &s->mr_ir, 0, memory_region_size(&s->mr_ir));
        memory_region_add_subregion_overlap(MEMORY_REGION(&vtd_dev_as->iommu),
                                            VTD_INTERRUPT_ADDR_FIRST,
                                            &vtd_dev_as->iommu_ir, 1);

        /*
         * This region is used for catching fault to access interrupt
         * range via passthrough + PASID. See also
         * vtd_switch_address_space(). We can't use alias since we
         * need to know the sid which is valid for MSI who uses
         * bus_master_as (see msi_send_message()).
         */
        memory_region_init_io(&vtd_dev_as->iommu_ir_fault, OBJECT(s),
                              &vtd_mem_ir_fault_ops, vtd_dev_as, "vtd-no-ir",
                              VTD_INTERRUPT_ADDR_SIZE);
        /*
         * Hook to root since when PT is enabled vtd_dev_as->iommu
         * will be disabled.
         */
        memory_region_add_subregion_overlap(MEMORY_REGION(&vtd_dev_as->root),
                                            VTD_INTERRUPT_ADDR_FIRST,
                                            &vtd_dev_as->iommu_ir_fault, 2);

        /*
         * Hook both the containers under the root container, we
         * switch between DMAR & noDMAR by enable/disable
         * corresponding sub-containers
         */
        memory_region_add_subregion_overlap(&vtd_dev_as->root, 0,
                                            MEMORY_REGION(&vtd_dev_as->iommu),
                                            0);
        memory_region_add_subregion_overlap(&vtd_dev_as->root, 0,
                                            &vtd_dev_as->nodmar, 0);

        vtd_switch_address_space(vtd_dev_as);

        g_hash_table_insert(s->vtd_address_spaces, new_key, vtd_dev_as);
    }
    return vtd_dev_as;
}

static bool vtd_check_hw_info(IntelIOMMUState *s, struct iommu_hw_info_vtd *vtd,
                              Error **errp)
{
    if (s->aw_bits > VTD_HOST_AW_48BIT && !(vtd->cap_reg & VTD_CAP_FL5LP)) {
        error_setg(errp, "User aw-bits: %u > host address width: %u",
                   s->aw_bits, VTD_HOST_AW_48BIT);
        return false;
    }

    if (!(vtd->ecap_reg & VTD_ECAP_NEST)) {
        error_setg(errp, "Need nested translation on host in modern mode");
        return false;
    }

    return true;
}

/* cap/ecap are readonly after vIOMMU finalized */
static bool vtd_check_hw_info_finalized(IntelIOMMUState *s,
                                        struct iommu_hw_info_vtd *vtd,
                                        Error **errp)
{
    if (s->cap & ~vtd->cap_reg & VTD_CAP_MASK) {
        error_setg(errp, "vIOMMU cap %lx isn't compatible with host %llx",
                   s->cap, vtd->cap_reg);
        return false;
    }

    if (s->ecap & ~vtd->ecap_reg & VTD_ECAP_MASK) {
        error_setg(errp, "vIOMMU ecap %lx isn't compatible with host %llx",
                   s->ecap, vtd->ecap_reg);
        return false;
    }

    if (s->ecap & vtd->ecap_reg & VTD_ECAP_PASID &&
        VTD_GET_PSS(s->ecap) > VTD_GET_PSS(vtd->ecap_reg)) {
        error_setg(errp, "vIOMMU pasid bits %lu > host pasid bits %llu",
                   VTD_GET_PSS(s->ecap), VTD_GET_PSS(vtd->ecap_reg));
        return false;
    }

    return true;
}

/* Caller should hold iommu lock. */
static bool vtd_sync_hw_info(IntelIOMMUState *s, struct iommu_hw_info_vtd *vtd,
                             Error **errp)
{
    uint64_t cap, ecap, addr_width, pasid_bits;

    if (!s->scalable_modern) {
        addr_width = (vtd->cap_reg >> 16) & 0x3fULL;
        if (s->aw_bits > addr_width) {
            error_setg(errp, "User aw-bits: %u > host address width: %lu",
                       s->aw_bits, addr_width);
            return false;
        }
        return true;
    }

    if (!vtd_check_hw_info(s, vtd, errp)) {
        return false;
    }

    if (s->cap_finalized) {
        return vtd_check_hw_info_finalized(s, vtd, errp);
    }

    /* sync host cap/ecap to vIOMMU */

    cap = s->host_cap & vtd->cap_reg & VTD_CAP_MASK;
    s->host_cap &= ~VTD_CAP_MASK;
    s->host_cap |= cap;
    ecap = s->host_ecap & vtd->ecap_reg & VTD_ECAP_MASK;
    s->host_ecap &= ~VTD_ECAP_MASK;
    s->host_ecap |= ecap;

    pasid_bits = VTD_GET_PSS(vtd->ecap_reg);
    if (s->host_ecap & VTD_ECAP_PASID &&
        VTD_GET_PSS(s->host_ecap) > pasid_bits) {
        s->host_ecap &= ~VTD_ECAP_PSS_MASK;
        s->host_ecap |= VTD_ECAP_PSS(pasid_bits);
    }

    return true;
}

/*
 * virtual VT-d which wants nested needs to check the host IOMMU
 * nesting cap info behind the assigned devices. Thus that vIOMMU
 * could bind guest page table to host.
 */
static bool vtd_check_idev(IntelIOMMUState *s, IOMMUFDDevice *idev,
                           uint32_t *flags, Error **errp)
{
    struct iommu_hw_info_vtd vtd;
    enum iommu_hw_info_type type = IOMMU_HW_INFO_TYPE_INTEL_VTD;
    bool passed;

    if (iommufd_device_get_info(idev, &type, sizeof(vtd), &vtd)) {
        error_setg(errp, "Failed to get IOMMU capability!!!");
        return false;
    }

    if (type != IOMMU_HW_INFO_TYPE_INTEL_VTD) {
        error_setg(errp, "IOMMU hardware is not compatible!!!");
        return false;
    }

    passed = vtd_sync_hw_info(s, &vtd, errp);
    if (passed) {
        *flags = vtd.flags;
    }
    return passed;
}

static int vtd_dev_set_iommu_device(PCIBus *bus, void *opaque, int32_t devfn,
                                    IOMMUFDDevice *idev, Error **errp)
{
    IntelIOMMUState *s = opaque;
    VTDIOMMUFDDevice *vtd_idev;
    struct vtd_as_key key = {
        .bus = bus,
        .devfn = devfn,
    };
    struct vtd_as_key *new_key;
    uint32_t flags;

    assert(0 <= devfn && devfn < PCI_DEVFN_MAX);

    if (!s->scalable_modern && !idev) {
        /* Legacy vIOMMU and legacy VFIO backend */
        return 0;
    } else if (!idev) {
        /* Modern vIOMMU and legacy VFIO backend */
        error_setg(errp, "Need IOMMUFD backend to setup nested page table");
        return -EINVAL;
    }

    vtd_iommu_lock(s);

    if (!vtd_check_idev(s, idev, &flags, errp)) {
        vtd_iommu_unlock(s);
        return -ENOENT;
    }

    vtd_idev = g_hash_table_lookup(s->vtd_iommufd_dev, &key);

    assert(!vtd_idev);

    new_key = g_malloc(sizeof(*new_key));
    new_key->bus = bus;
    new_key->devfn = devfn;

    vtd_idev = g_malloc0(sizeof(VTDIOMMUFDDevice));
    vtd_idev->bus = bus;
    vtd_idev->devfn = (uint8_t)devfn;
    vtd_idev->iommu_state = s;
    vtd_idev->idev = idev;
    vtd_idev->errata = flags & IOMMU_HW_INFO_VTD_ERRATA_772415_SPR17;
    QLIST_INSERT_HEAD(&s->vtd_idev_list, vtd_idev, next);

    g_hash_table_insert(s->vtd_iommufd_dev, new_key, vtd_idev);

    vtd_iommu_unlock(s);

    return 0;
}

static void vtd_dev_unset_iommu_device(PCIBus *bus, void *opaque, int32_t devfn)
{
    IntelIOMMUState *s = opaque;
    VTDIOMMUFDDevice *vtd_idev;
    struct vtd_as_key key = {
        .bus = bus,
        .devfn = devfn,
    };

    assert(0 <= devfn && devfn < PCI_DEVFN_MAX);

    vtd_iommu_lock(s);

    vtd_idev = g_hash_table_lookup(s->vtd_iommufd_dev, &key);
    if (!vtd_idev) {
        vtd_iommu_unlock(s);
        return;
    }

    QLIST_REMOVE(vtd_idev, next);
    g_hash_table_remove(s->vtd_iommufd_dev, &key);

    vtd_iommu_unlock(s);
}

/* Unmap the whole range in the notifier's scope. */
static void vtd_address_space_unmap(VTDAddressSpace *as, IOMMUNotifier *n)
{
    hwaddr total, remain;
    hwaddr start = n->start;
    hwaddr end = n->end;
    IntelIOMMUState *s = as->iommu_state;
    DMAMap map;

    /*
     * Note: all the codes in this function has a assumption that IOVA
     * bits are no more than VTD_MGAW bits (which is restricted by
     * VT-d spec), otherwise we need to consider overflow of 64 bits.
     */

    if (end > VTD_ADDRESS_SIZE(s->aw_bits) - 1) {
        /*
         * Don't need to unmap regions that is bigger than the whole
         * VT-d supported address space size
         */
        end = VTD_ADDRESS_SIZE(s->aw_bits) - 1;
    }

    assert(start <= end);
    total = remain = end - start + 1;

    while (remain >= VTD_PAGE_SIZE) {
        IOMMUTLBEvent event;
        uint64_t mask = dma_aligned_pow2_mask(start, end, s->aw_bits);
        uint64_t size = mask + 1;

        assert(size);

        event.type = IOMMU_NOTIFIER_UNMAP;
        event.entry.iova = start;
        event.entry.addr_mask = mask;
        event.entry.target_as = &address_space_memory;
        event.entry.perm = IOMMU_NONE;
        /* This field is meaningless for unmap */
        event.entry.translated_addr = 0;

        memory_region_notify_iommu_one(n, &event);

        start += size;
        remain -= size;
    }

    assert(!remain);

    trace_vtd_as_unmap_whole(pci_bus_num(as->bus),
                             VTD_PCI_SLOT(as->devfn),
                             VTD_PCI_FUNC(as->devfn),
                             n->start, total);

    map.iova = n->start;
    map.size = total - 1; /* Inclusive */
    iova_tree_remove(as->iova_tree, map);
}

static void vtd_address_space_unmap_all(IntelIOMMUState *s)
{
    VTDAddressSpace *vtd_as;
    IOMMUNotifier *n;

    QLIST_FOREACH(vtd_as, &s->vtd_as_with_notifiers, next) {
        IOMMU_NOTIFIER_FOREACH(n, &vtd_as->iommu) {
            vtd_address_space_unmap(vtd_as, n);
        }
    }
}

static void vtd_address_space_refresh_all(IntelIOMMUState *s)
{
    vtd_address_space_unmap_all(s);
    vtd_switch_address_space_all(s);
}

static int vtd_replay_hook(IOMMUTLBEvent *event, void *private)
{
    memory_region_notify_iommu_one(private, event);
    return 0;
}

static void vtd_iommu_replay(IOMMUMemoryRegion *iommu_mr, IOMMUNotifier *n)
{
    VTDAddressSpace *vtd_as = container_of(iommu_mr, VTDAddressSpace, iommu);
    IntelIOMMUState *s = vtd_as->iommu_state;
    uint8_t bus_n = pci_bus_num(vtd_as->bus);
    VTDContextEntry ce;
    DMAMap map = { .iova = 0, .size = HWADDR_MAX };

    /* replay is protected by BQL, page walk will re-setup it safely */
    iova_tree_remove(vtd_as->iova_tree, map);

    if (vtd_dev_to_context_entry(s, bus_n, vtd_as->devfn, &ce) == 0) {
        trace_vtd_replay_ce_valid(s->root_scalable ? "scalable mode" :
                                  "legacy mode",
                                  bus_n, PCI_SLOT(vtd_as->devfn),
                                  PCI_FUNC(vtd_as->devfn),
                                  vtd_get_domain_id(s, &ce, vtd_as->pasid),
                                  ce.hi, ce.lo);
        if (n->notifier_flags & IOMMU_NOTIFIER_MAP) {
            /* This is required only for MAP typed notifiers */
            vtd_page_walk_info info = {
                .hook_fn = vtd_replay_hook,
                .private = (void *)n,
                .notify_unmap = false,
                .aw = s->aw_bits,
                .as = vtd_as,
                .domain_id = vtd_get_domain_id(s, &ce, vtd_as->pasid),
            };

            vtd_page_walk(s, &ce, 0, ~0ULL, &info, vtd_as->pasid);
        }
    } else {
        trace_vtd_replay_ce_invalid(bus_n, PCI_SLOT(vtd_as->devfn),
                                    PCI_FUNC(vtd_as->devfn));
    }

    return;
}

static void vtd_cap_init(IntelIOMMUState *s)
{
    X86IOMMUState *x86_iommu = X86_IOMMU_DEVICE(s);

    s->cap = VTD_CAP_FRO | VTD_CAP_NFR | VTD_CAP_ND |
             VTD_CAP_MAMV | VTD_CAP_PSI | VTD_CAP_SLLPS |
             VTD_CAP_MGAW(s->aw_bits);
    if (s->dma_drain) {
        s->cap |= VTD_CAP_DRAIN;
    }
    if (s->dma_translation) {
            if (s->aw_bits >= VTD_HOST_AW_39BIT) {
                    s->cap |= VTD_CAP_SAGAW_39bit;
            }
            if (s->aw_bits >= VTD_HOST_AW_48BIT) {
                    s->cap |= VTD_CAP_SAGAW_48bit;
            }
    }
    s->ecap = VTD_ECAP_QI | VTD_ECAP_IRO;

    if (x86_iommu_ir_supported(x86_iommu)) {
        s->ecap |= VTD_ECAP_IR | VTD_ECAP_MHMV;
        if (s->intr_eim == ON_OFF_AUTO_ON) {
            s->ecap |= VTD_ECAP_EIM;
        }
        assert(s->intr_eim != ON_OFF_AUTO_AUTO);
    }

    if (x86_iommu->dt_supported) {
        s->ecap |= VTD_ECAP_DT;
        if (s->scalable_modern) {
            s->ecap |= VTD_ECAP_PRS;
        }
    }

    if (x86_iommu->pt_supported) {
        s->ecap |= VTD_ECAP_PT;
    }

    if (s->caching_mode) {
        s->cap |= VTD_CAP_CM;
    }

    /* TODO: read cap/ecap from host to decide which cap to be exposed. */
    if (s->scalable_mode && !s->scalable_modern) {
        s->ecap |= VTD_ECAP_SMTS | VTD_ECAP_SRS | VTD_ECAP_SLTS;
    } else if (s->scalable_mode && s->scalable_modern) {
        s->ecap |= VTD_ECAP_SMTS | VTD_ECAP_SRS;
        if (s->aw_bits == VTD_HOST_AW_48BIT) {
            s->ecap |= VTD_ECAP_FLTS;
            s->cap |= VTD_CAP_FL1GP;
        }
    }

    if (s->snoop_control) {
        s->ecap |= VTD_ECAP_SC;
    }

    if (s->pasid) {
        s->ecap |= VTD_ECAP_PASID | VTD_ECAP_PSS(VTD_ECAP_PSS_MAX);
    }
}

/*
 * Do the initialization. It will also be called when reset, so pay
 * attention when adding new initialization stuff.
 */
static void vtd_init(IntelIOMMUState *s)
{
    X86IOMMUState *x86_iommu = X86_IOMMU_DEVICE(s);

    /* CAP/ECAP are initialized in machine create done stage */
    memset(s->csr + DMAR_GCMD_REG, 0, DMAR_REG_SIZE - DMAR_GCMD_REG);
    memset(s->wmask + DMAR_GCMD_REG, 0, DMAR_REG_SIZE - DMAR_GCMD_REG);
    memset(s->w1cmask + DMAR_GCMD_REG, 0, DMAR_REG_SIZE - DMAR_GCMD_REG);
    memset(s->womask + DMAR_GCMD_REG, 0, DMAR_REG_SIZE - DMAR_GCMD_REG);

    s->root = 0;
    s->root_scalable = false;
    s->dmar_enabled = false;
    s->intr_enabled = false;
    s->iq_head = 0;
    s->iq_tail = 0;
    s->iq = 0;
    s->iq_size = 0;
    s->qi_enabled = false;
    s->iq_last_desc_type = VTD_INV_DESC_NONE;
    s->iq_dw = false;
    s->next_frcd_reg = 0;

    /*
     * Rsvd field masks for spte
     */
    vtd_spte_rsvd[0] = ~0ULL;
    vtd_spte_rsvd[1] = VTD_SPTE_PAGE_L1_RSVD_MASK(s->aw_bits,
                                                  x86_iommu->dt_supported);
    vtd_spte_rsvd[2] = VTD_SPTE_PAGE_L2_RSVD_MASK(s->aw_bits);
    vtd_spte_rsvd[3] = VTD_SPTE_PAGE_L3_RSVD_MASK(s->aw_bits);
    vtd_spte_rsvd[4] = VTD_SPTE_PAGE_L4_RSVD_MASK(s->aw_bits);

    vtd_spte_rsvd_large[2] = VTD_SPTE_LPAGE_L2_RSVD_MASK(s->aw_bits,
                                                    x86_iommu->dt_supported);
    vtd_spte_rsvd_large[3] = VTD_SPTE_LPAGE_L3_RSVD_MASK(s->aw_bits,
                                                    x86_iommu->dt_supported);

    if (s->scalable_mode || s->snoop_control) {
        vtd_spte_rsvd[1] &= ~VTD_SPTE_SNP;
        vtd_spte_rsvd_large[2] &= ~VTD_SPTE_SNP;
        vtd_spte_rsvd_large[3] &= ~VTD_SPTE_SNP;
    }

    if (!s->cap_finalized) {
        vtd_cap_init(s);
        s->host_cap = s->cap;
        s->host_ecap = s->ecap;
    }

    vtd_reset_caches(s);

    /* Define registers with default values and bit semantics */
    vtd_define_long(s, DMAR_VER_REG, 0x10UL, 0, 0);
    vtd_define_long(s, DMAR_GCMD_REG, 0, 0xff800000UL, 0);
    vtd_define_long_wo(s, DMAR_GCMD_REG, 0xff800000UL);
    vtd_define_long(s, DMAR_GSTS_REG, 0, 0, 0);
    vtd_define_quad(s, DMAR_RTADDR_REG, 0, 0xfffffffffffffc00ULL, 0);
    vtd_define_quad(s, DMAR_CCMD_REG, 0, 0xe0000003ffffffffULL, 0);
    vtd_define_quad_wo(s, DMAR_CCMD_REG, 0x3ffff0000ULL);

    /* Advanced Fault Logging not supported */
    vtd_define_long(s, DMAR_FSTS_REG, 0, 0, 0x11UL);
    vtd_define_long(s, DMAR_FECTL_REG, 0x80000000UL, 0x80000000UL, 0);
    vtd_define_long(s, DMAR_FEDATA_REG, 0, 0x0000ffffUL, 0);
    vtd_define_long(s, DMAR_FEADDR_REG, 0, 0xfffffffcUL, 0);

    /* Treated as RsvdZ when EIM in ECAP_REG is not supported
     * vtd_define_long(s, DMAR_FEUADDR_REG, 0, 0xffffffffUL, 0);
     */
    vtd_define_long(s, DMAR_FEUADDR_REG, 0, 0, 0);

    /* Treated as RO for implementations that PLMR and PHMR fields reported
     * as Clear in the CAP_REG.
     * vtd_define_long(s, DMAR_PMEN_REG, 0, 0x80000000UL, 0);
     */
    vtd_define_long(s, DMAR_PMEN_REG, 0, 0, 0);

    vtd_define_quad(s, DMAR_IQH_REG, 0, 0, 0);
    vtd_define_quad(s, DMAR_IQT_REG, 0, 0x7fff0ULL, 0);
    vtd_define_quad(s, DMAR_IQA_REG, 0, 0xfffffffffffff807ULL, 0);
    vtd_define_long(s, DMAR_ICS_REG, 0, 0, 0x1UL);
    vtd_define_long(s, DMAR_IECTL_REG, 0x80000000UL, 0x80000000UL, 0);
    vtd_define_long(s, DMAR_IEDATA_REG, 0, 0xffffffffUL, 0);
    vtd_define_long(s, DMAR_IEADDR_REG, 0, 0xfffffffcUL, 0);
    /* Treadted as RsvdZ when EIM in ECAP_REG is not supported */
    vtd_define_long(s, DMAR_IEUADDR_REG, 0, 0, 0);

    /* IOTLB registers */
    vtd_define_quad(s, DMAR_IOTLB_REG, 0, 0Xb003ffff00000000ULL, 0);
    vtd_define_quad(s, DMAR_IVA_REG, 0, 0xfffffffffffff07fULL, 0);
    vtd_define_quad_wo(s, DMAR_IVA_REG, 0xfffffffffffff07fULL);

    /* Fault Recording Registers, 128-bit */
    vtd_define_quad(s, DMAR_FRCD_REG_0_0, 0, 0, 0);
    vtd_define_quad(s, DMAR_FRCD_REG_0_2, 0, 0, 0x8000000000000000ULL);

    /*
     * Interrupt remapping registers.
     */
    vtd_define_quad(s, DMAR_IRTA_REG, 0, 0xfffffffffffff80fULL, 0);
}

/* Should not reset address_spaces when reset because devices will still use
 * the address space they got at first (won't ask the bus again).
 */
static void vtd_reset(DeviceState *dev)
{
    IntelIOMMUState *s = INTEL_IOMMU_DEVICE(dev);

    vtd_init(s);
    vtd_address_space_refresh_all(s);
    vtd_refresh_pasid_bind(s);
}

static AddressSpace *vtd_host_dma_iommu(PCIBus *bus, void *opaque, int devfn)
{
    IntelIOMMUState *s = opaque;
    VTDAddressSpace *vtd_as;

    assert(0 <= devfn && devfn < PCI_DEVFN_MAX);

    vtd_as = vtd_find_add_as(s, bus, devfn, PCI_NO_PASID);
    return &vtd_as->as;
}

static PCIIOMMUOps vtd_iommu_ops = {
    .get_address_space = vtd_host_dma_iommu,
    .set_iommu_device = vtd_dev_set_iommu_device,
    .unset_iommu_device = vtd_dev_unset_iommu_device,
};

static bool vtd_decide_config(IntelIOMMUState *s, Error **errp)
{
    X86IOMMUState *x86_iommu = X86_IOMMU_DEVICE(s);

    if (s->intr_eim == ON_OFF_AUTO_ON && !x86_iommu_ir_supported(x86_iommu)) {
        error_setg(errp, "eim=on cannot be selected without intremap=on");
        return false;
    }

    if (s->intr_eim == ON_OFF_AUTO_AUTO) {
        s->intr_eim = (kvm_irqchip_in_kernel() || s->buggy_eim)
                      && x86_iommu_ir_supported(x86_iommu) ?
                                              ON_OFF_AUTO_ON : ON_OFF_AUTO_OFF;
    }
    if (s->intr_eim == ON_OFF_AUTO_ON && !s->buggy_eim) {
        if (!kvm_irqchip_is_split()) {
            error_setg(errp, "eim=on requires accel=kvm,kernel-irqchip=split");
            return false;
        }
        if (kvm_enabled() && !kvm_enable_x2apic()) {
            error_setg(errp, "eim=on requires support on the KVM side"
                             "(X2APIC_API, first shipped in v4.7)");
            return false;
        }
    }

    if (s->scalable_mode && !s->dma_drain) {
        error_setg(errp, "Need to set dma_drain for scalable mode");
        return false;
    }

    if (s->scalable_mode_str &&
        (strcmp(s->scalable_mode_str, "off") &&
         strcmp(s->scalable_mode_str, "modern") &&
         strcmp(s->scalable_mode_str, "legacy"))) {
        error_setg(errp, "Invalid x-scalable-mode config,"
                         "Please use \"modern\", \"legacy\" or \"off\"");
        return false;
    }

    if (s->scalable_mode_str &&
        !strcmp(s->scalable_mode_str, "legacy")) {
        s->scalable_mode = true;
        s->scalable_modern = false;
    } else if (s->scalable_mode_str &&
        !strcmp(s->scalable_mode_str, "modern")) {
        s->scalable_mode = true;
        s->scalable_modern = true;
    } else {
        s->scalable_mode = false;
        s->scalable_modern = false;
    }

    if ((s->aw_bits != VTD_HOST_AW_48BIT) &&
        (s->aw_bits != VTD_HOST_AW_39BIT) &&
        !s->scalable_modern) {
        error_setg(errp, "Supported values for aw-bits are: %d, %d",
                   VTD_HOST_AW_48BIT, VTD_HOST_AW_39BIT);
        return false;
    }

    if ((s->aw_bits != VTD_HOST_AW_48BIT) && s->scalable_modern) {
        error_setg(errp, "Supported values for aw-bits are: %d",
                   VTD_HOST_AW_48BIT);
        return false;
    }

    if (s->pasid && !s->scalable_mode) {
        error_setg(errp, "Need to set scalable mode for PASID");
        return false;
    }

    return true;
}

static void vtd_setup_capability_reg(IntelIOMMUState *s)
{
    vtd_define_quad(s, DMAR_CAP_REG, s->cap, 0, 0);
    vtd_define_quad(s, DMAR_ECAP_REG, s->ecap, 0, 0);
}

static int vtd_machine_done_notify_one(Object *child, void *unused)
{
    IntelIOMMUState *iommu = INTEL_IOMMU_DEVICE(x86_iommu_get_default());

    /*
     * We hard-coded here because vfio-pci is the only special case
     * here.  Let's be more elegant in the future when we can, but so
     * far there seems to be no better way.
     */
    if (object_dynamic_cast(child, "vfio-pci") && !iommu->caching_mode) {
        vtd_panic_require_caching_mode();
    }

    return 0;
}

static void vtd_machine_done_hook(Notifier *notifier, void *unused)
{
    IntelIOMMUState *iommu = INTEL_IOMMU_DEVICE(x86_iommu_get_default());

    vtd_iommu_lock(iommu);
    iommu->cap = iommu->host_cap;
    iommu->ecap = iommu->host_ecap;
    iommu->cap_finalized = true;

    vtd_setup_capability_reg(iommu);
    vtd_iommu_unlock(iommu);

    object_child_foreach_recursive(object_get_root(),
                                   vtd_machine_done_notify_one, NULL);
}

static Notifier vtd_machine_done_notify = {
    .notify = vtd_machine_done_hook,
};

static void vtd_realize(DeviceState *dev, Error **errp)
{
    MachineState *ms = MACHINE(qdev_get_machine());
    PCMachineState *pcms = PC_MACHINE(ms);
    X86MachineState *x86ms = X86_MACHINE(ms);
    PCIBus *bus = pcms->bus;
    IntelIOMMUState *s = INTEL_IOMMU_DEVICE(dev);

    if (!vtd_decide_config(s, errp)) {
        return;
    }

    QLIST_INIT(&s->vtd_as_with_notifiers);
    QLIST_INIT(&s->vtd_idev_list);
    qemu_mutex_init(&s->iommu_lock);
    s->cap_finalized = false;
    memory_region_init_io(&s->csrmem, OBJECT(s), &vtd_mem_ops, s,
                          "intel_iommu", DMAR_REG_SIZE);
    memory_region_add_subregion(get_system_memory(),
                                Q35_HOST_BRIDGE_IOMMU_ADDR, &s->csrmem);

    /* Create the shared memory regions by all devices */
    memory_region_init(&s->mr_nodmar, OBJECT(s), "vtd-nodmar",
                       UINT64_MAX);
    memory_region_init_io(&s->mr_ir, OBJECT(s), &vtd_mem_ir_ops,
                          s, "vtd-ir", VTD_INTERRUPT_ADDR_SIZE);
    memory_region_init_alias(&s->mr_sys_alias, OBJECT(s),
                             "vtd-sys-alias", get_system_memory(), 0,
                             memory_region_size(get_system_memory()));
    memory_region_add_subregion_overlap(&s->mr_nodmar, 0,
                                        &s->mr_sys_alias, 0);
    memory_region_add_subregion_overlap(&s->mr_nodmar,
                                        VTD_INTERRUPT_ADDR_FIRST,
                                        &s->mr_ir, 1);
    /* No corresponding destroy */
    s->iotlb = g_hash_table_new_full(vtd_iotlb_hash, vtd_iotlb_equal,
                                     g_free, g_free);
    s->p_iotlb = g_hash_table_new_full(&g_str_hash, &g_str_equal,
                                       g_free, g_free);
    s->vtd_address_spaces = g_hash_table_new_full(vtd_as_hash, vtd_as_equal,
                                      g_free, g_free);
    s->vtd_iommufd_dev = g_hash_table_new_full(vtd_as_hash, vtd_as_idev_equal,
                                      g_free, g_free);
    s->vtd_pasid_as = g_hash_table_new_full(vtd_pasid_as_key_hash,
                                            vtd_pasid_as_key_equal,
                                            g_free, g_free);
    vtd_init(s);
    pci_setup_iommu(bus, &vtd_iommu_ops, dev);
    /* Pseudo address space under root PCI bus. */
    x86ms->ioapic_as = vtd_host_dma_iommu(bus, s, Q35_PSEUDO_DEVFN_IOAPIC);
    qemu_add_machine_init_done_notifier(&vtd_machine_done_notify);
}

static void vtd_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    X86IOMMUClass *x86_class = X86_IOMMU_DEVICE_CLASS(klass);

    dc->reset = vtd_reset;
    dc->vmsd = &vtd_vmstate;
    device_class_set_props(dc, vtd_properties);
    dc->hotpluggable = false;
    x86_class->realize = vtd_realize;
    x86_class->int_remap = vtd_int_remap;
    /* Supported by the pc-q35-* machine types */
    dc->user_creatable = true;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    dc->desc = "Intel IOMMU (VT-d) DMA Remapping device";
}

static const TypeInfo vtd_info = {
    .name          = TYPE_INTEL_IOMMU_DEVICE,
    .parent        = TYPE_X86_IOMMU_DEVICE,
    .instance_size = sizeof(IntelIOMMUState),
    .class_init    = vtd_class_init,
};

static void vtd_iommu_memory_region_class_init(ObjectClass *klass,
                                                     void *data)
{
    IOMMUMemoryRegionClass *imrc = IOMMU_MEMORY_REGION_CLASS(klass);

    imrc->translate = vtd_iommu_translate;
    imrc->notify_flag_changed = vtd_iommu_notify_flag_changed;
    imrc->replay = vtd_iommu_replay;
}

static const TypeInfo vtd_iommu_memory_region_info = {
    .parent = TYPE_IOMMU_MEMORY_REGION,
    .name = TYPE_INTEL_IOMMU_MEMORY_REGION,
    .class_init = vtd_iommu_memory_region_class_init,
};

static void vtd_register_types(void)
{
    type_register_static(&vtd_info);
    type_register_static(&vtd_iommu_memory_region_info);
}

type_init(vtd_register_types)
