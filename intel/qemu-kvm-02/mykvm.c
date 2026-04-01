// SPDX-License-Identifier: GPL
/*
 * mykvm.c - educational VT-x scaffold
 *
 * This version is intentionally focused on:
 *   - compiling cleanly on modern x86 kernels
 *   - creating VM/vCPU objects
 *   - allocating 1 GiB guest RAM
 *   - building a simple EPT hierarchy
 *   - enabling VMX on one CPU
 *   - preparing a VMCS
 *
 * It does NOT yet implement a correct VM-entry/VM-exit execution path.
 * MYKVM_RUN currently returns MYKVM_EXIT_NOT_IMPLEMENTED.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/smp.h>
#include <linux/mm.h>
#include <linux/highmem.h>
#include <linux/types.h>
#include <linux/errno.h>

#include <asm/msr.h>
#include <asm/desc.h>
#include <asm/processor.h>
#include <asm/io.h>
#include <asm/page.h>
#include <asm/special_insns.h>
#include <asm/special_insns.h>

#include "mykvm_uapi.h"

#define DRV_NAME "mykvm"

#define IA32_FEATURE_CONTROL          0x3a
#define IA32_VMX_BASIC                0x480
#define IA32_VMX_PINBASED_CTLS        0x481
#define IA32_VMX_PROCBASED_CTLS       0x482
#define IA32_VMX_EXIT_CTLS            0x483
#define IA32_VMX_ENTRY_CTLS           0x484
#define IA32_VMX_PROCBASED_CTLS2      0x48b

#define CR4_VMXE                      (1UL << 13)

/* VMCS fields */
#define VMCS_VPID                     0x0000
#define VMCS_CTRL_PIN_BASED           0x4000
#define VMCS_CTRL_CPU_BASED           0x4002
#define VMCS_CTRL_EXCEPTION_BITMAP    0x4004
#define VMCS_CTRL_VMEXIT_CONTROLS     0x400c
#define VMCS_CTRL_VMENTRY_CONTROLS    0x4012
#define VMCS_CTRL_SECONDARY_CPU_BASED 0x401e
#define VMCS_CTRL_EPT_POINTER         0x201a
#define VMCS_CTRL_CR0_MASK            0x6000
#define VMCS_CTRL_CR4_MASK            0x6002
#define VMCS_CTRL_CR0_SHADOW          0x6004
#define VMCS_CTRL_CR4_SHADOW          0x6006

#define VMCS_GUEST_CR0                0x6800
#define VMCS_GUEST_CR3                0x6802
#define VMCS_GUEST_CR4                0x6804
#define VMCS_GUEST_ES_SELECTOR        0x0800
#define VMCS_GUEST_CS_SELECTOR        0x0802
#define VMCS_GUEST_SS_SELECTOR        0x0804
#define VMCS_GUEST_DS_SELECTOR        0x0806
#define VMCS_GUEST_FS_SELECTOR        0x0808
#define VMCS_GUEST_GS_SELECTOR        0x080a
#define VMCS_GUEST_LDTR_SELECTOR      0x080c
#define VMCS_GUEST_TR_SELECTOR        0x080e

#define VMCS_GUEST_ES_LIMIT           0x4800
#define VMCS_GUEST_CS_LIMIT           0x4802
#define VMCS_GUEST_SS_LIMIT           0x4804
#define VMCS_GUEST_DS_LIMIT           0x4806
#define VMCS_GUEST_FS_LIMIT           0x4808
#define VMCS_GUEST_GS_LIMIT           0x480a
#define VMCS_GUEST_LDTR_LIMIT         0x480c
#define VMCS_GUEST_TR_LIMIT           0x480e
#define VMCS_GUEST_GDTR_LIMIT         0x4810
#define VMCS_GUEST_IDTR_LIMIT         0x4812
#define VMCS_GUEST_ES_AR_BYTES        0x4814
#define VMCS_GUEST_CS_AR_BYTES        0x4816
#define VMCS_GUEST_SS_AR_BYTES        0x4818
#define VMCS_GUEST_DS_AR_BYTES        0x481a
#define VMCS_GUEST_FS_AR_BYTES        0x481c
#define VMCS_GUEST_GS_AR_BYTES        0x481e
#define VMCS_GUEST_LDTR_AR_BYTES      0x4820
#define VMCS_GUEST_TR_AR_BYTES        0x4822
#define VMCS_GUEST_INTERRUPTIBILITY   0x4824
#define VMCS_GUEST_ACTIVITY_STATE     0x4826
#define VMCS_GUEST_SYSENTER_CS        0x482a

#define VMCS_GUEST_ES_BASE            0x6806
#define VMCS_GUEST_CS_BASE            0x6808
#define VMCS_GUEST_SS_BASE            0x680a
#define VMCS_GUEST_DS_BASE            0x680c
#define VMCS_GUEST_FS_BASE            0x680e
#define VMCS_GUEST_GS_BASE            0x6810
#define VMCS_GUEST_LDTR_BASE          0x6812
#define VMCS_GUEST_TR_BASE            0x6814
#define VMCS_GUEST_GDTR_BASE          0x6816
#define VMCS_GUEST_IDTR_BASE          0x6818
#define VMCS_GUEST_DR7                0x681a
#define VMCS_GUEST_RSP                0x681c
#define VMCS_GUEST_RIP                0x681e
#define VMCS_GUEST_RFLAGS             0x6820
#define VMCS_GUEST_PENDING_DBG_EXC    0x6822
#define VMCS_GUEST_SYSENTER_ESP       0x6824
#define VMCS_GUEST_SYSENTER_EIP       0x6826

#define VMCS_HOST_CR0                 0x6c00
#define VMCS_HOST_CR3                 0x6c02
#define VMCS_HOST_CR4                 0x6c04
#define VMCS_HOST_FS_BASE             0x6c06
#define VMCS_HOST_GS_BASE             0x6c08
#define VMCS_HOST_TR_BASE             0x6c0a
#define VMCS_HOST_GDTR_BASE           0x6c0c
#define VMCS_HOST_IDTR_BASE           0x6c0e
#define VMCS_HOST_SYSENTER_ESP        0x6c10
#define VMCS_HOST_SYSENTER_EIP        0x6c12
#define VMCS_HOST_RSP                 0x6c14
#define VMCS_HOST_RIP                 0x6c16
#define VMCS_HOST_ES_SELECTOR         0x0c00
#define VMCS_HOST_CS_SELECTOR         0x0c02
#define VMCS_HOST_SS_SELECTOR         0x0c04
#define VMCS_HOST_DS_SELECTOR         0x0c06
#define VMCS_HOST_FS_SELECTOR         0x0c08
#define VMCS_HOST_GS_SELECTOR         0x0c0a
#define VMCS_HOST_TR_SELECTOR         0x0c0c
#define VMCS_HOST_SYSENTER_CS         0x4c00

#define VMCS_RO_VM_INSTRUCTION_ERROR  0x4400

/* controls */
#define CPU_BASED_HLT_EXITING            (1u << 7)
#define CPU_BASED_ACTIVATE_SECONDARY     (1u << 31)

#define SECONDARY_EXEC_ENABLE_EPT        (1u << 1)
#define SECONDARY_EXEC_MONITOR_TRAP_FLAG (1u << 27)

#define VM_EXIT_HOST_ADDR_SPACE_SIZE     (1u << 9)
#define VM_ENTRY_IA32E_MODE              (1u << 9)

/* EPT */
#define EPT_R                            (1ULL << 0)
#define EPT_W                            (1ULL << 1)
#define EPT_X                            (1ULL << 2)
#define EPT_MEMTYPE_WB                   (6ULL << 3)
#define EPT_PS                           (1ULL << 7)

#define MYKVM_TEST_GPA 0x1000

struct mykvm_vm {
    bool created;

    void *guest_ram_hva;
    u64 guest_ram_size;

    u64 *ept_pml4;
    phys_addr_t ept_pml4_pa;
    u64 *ept_pdpt;
    phys_addr_t ept_pdpt_pa;
    u64 *ept_pd;
    phys_addr_t ept_pd_pa;
};

struct mykvm_vcpu {
    bool created;
    bool launched;
    bool single_step;
    u32 id;
    int cpu;

    struct mykvm_regs regs;

    void *vmxon_region;
    phys_addr_t vmxon_pa;

    void *vmcs_region;
    phys_addr_t vmcs_pa;
};

struct mykvm_smp_call {
    struct mykvm_vcpu *vcpu;
    int ret;
};

struct mykvm_state {
    struct mutex lock;
    struct mykvm_vm vm;
    struct mykvm_vcpu vcpu;
};

static struct mykvm_state g;

static inline u64 my_rdmsr64(u32 msr)
{
    u32 lo, hi;
    rdmsr(msr, lo, hi);
    return ((u64)hi << 32) | lo;
}

static inline int my_cpu_has_vmx(void)
{
    u32 eax = 1, ebx = 0, ecx = 0, edx = 0;
    native_cpuid(&eax, &ebx, &ecx, &edx);
    return !!(ecx & BIT(5));
}

static inline int my_vmxon(u64 pa)
{
    u8 ret;
    asm volatile("vmxon %[pa]; setna %[ret]"
                 : [ret] "=rm"(ret)
                 : [pa] "m"(pa)
                 : "cc", "memory");
    return ret ? -EIO : 0;
}

static inline int my_vmxoff(void)
{
    u8 ret;
    asm volatile("vmxoff; setna %0"
                 : "=rm"(ret)
                 :
                 : "cc", "memory");
    return ret ? -EIO : 0;
}

static inline int my_vmptrld(u64 pa)
{
    u8 ret;
    asm volatile("vmptrld %[pa]; setna %[ret]"
                 : [ret] "=rm"(ret)
                 : [pa] "m"(pa)
                 : "cc", "memory");
    return ret ? -EIO : 0;
}

static inline int my_vmclear(u64 pa)
{
    u8 ret;
    asm volatile("vmclear %[pa]; setna %[ret]"
                 : [ret] "=rm"(ret)
                 : [pa] "m"(pa)
                 : "cc", "memory");
    return ret ? -EIO : 0;
}

static inline int my_vmread(u64 field, u64 *value)
{
    u8 ret;
    asm volatile("vmread %[fld], %[val]; setna %[ret]"
                 : [ret] "=rm"(ret), [val] "=rm"(*value)
                 : [fld] "r"(field)
                 : "cc", "memory");
    return ret ? -EIO : 0;
}
static inline int my_vmwrite(u64 field, u64 value)
{
    u8 ret;
    asm volatile("vmwrite %[val], %[fld]; setna %[ret]"
                 : [ret] "=rm"(ret)
                 : [fld] "r"(field), [val] "r"(value)
                 : "cc", "memory");
    return ret ? -EIO : 0;
}

static u32 adjust_vmx_controls(u32 ctl, u32 msr)
{
    u64 v = my_rdmsr64(msr);
    u32 allowed0 = (u32)v;
    u32 allowed1 = (u32)(v >> 32);

    ctl |= allowed0;
    ctl &= allowed1;
    return ctl;
}

static int mykvm_alloc_vm_memory(struct mykvm_vm *vm)
{
    vm->guest_ram_size = MYKVM_GUEST_RAM_SIZE;
    vm->guest_ram_hva = vzalloc(vm->guest_ram_size);
    if (!vm->guest_ram_hva)
        return -ENOMEM;
    return 0;
}

static void mykvm_free_vm_memory(struct mykvm_vm *vm)
{
    if (vm->guest_ram_hva)
        vfree(vm->guest_ram_hva);
    vm->guest_ram_hva = NULL;
}

static int mykvm_build_ept_1g(struct mykvm_vm *vm)
{
    int i;

    vm->ept_pml4 = (u64 *)get_zeroed_page(GFP_KERNEL);
    if (!vm->ept_pml4)
        return -ENOMEM;
    vm->ept_pml4_pa = virt_to_phys(vm->ept_pml4);

    vm->ept_pdpt = (u64 *)get_zeroed_page(GFP_KERNEL);
    if (!vm->ept_pdpt)
        return -ENOMEM;
    vm->ept_pdpt_pa = virt_to_phys(vm->ept_pdpt);

    vm->ept_pd = (u64 *)get_zeroed_page(GFP_KERNEL);
    if (!vm->ept_pd)
        return -ENOMEM;
    vm->ept_pd_pa = virt_to_phys(vm->ept_pd);

    vm->ept_pml4[0] = vm->ept_pdpt_pa | EPT_R | EPT_W | EPT_X;
    vm->ept_pdpt[0] = vm->ept_pd_pa   | EPT_R | EPT_W | EPT_X;

    for (i = 0; i < 512; i++) {
        phys_addr_t hpa = virt_to_phys((char *)vm->guest_ram_hva + (i * 0x200000ULL));
        vm->ept_pd[i] = hpa | EPT_R | EPT_W | EPT_X | EPT_MEMTYPE_WB | EPT_PS;
    }

    return 0;
}

static void mykvm_free_ept(struct mykvm_vm *vm)
{
    if (vm->ept_pd)
        free_page((unsigned long)vm->ept_pd);
    if (vm->ept_pdpt)
        free_page((unsigned long)vm->ept_pdpt);
    if (vm->ept_pml4)
        free_page((unsigned long)vm->ept_pml4);

    vm->ept_pd = NULL;
    vm->ept_pdpt = NULL;
    vm->ept_pml4 = NULL;
}

static int mykvm_alloc_vcpu_resources(struct mykvm_vcpu *vcpu)
{
    u32 rev = (u32)my_rdmsr64(IA32_VMX_BASIC);

    vcpu->vmxon_region = (void *)get_zeroed_page(GFP_KERNEL);
    if (!vcpu->vmxon_region)
        return -ENOMEM;
    *(u32 *)vcpu->vmxon_region = rev;
    vcpu->vmxon_pa = virt_to_phys(vcpu->vmxon_region);

    vcpu->vmcs_region = (void *)get_zeroed_page(GFP_KERNEL);
    if (!vcpu->vmcs_region)
        return -ENOMEM;
    *(u32 *)vcpu->vmcs_region = rev;
    vcpu->vmcs_pa = virt_to_phys(vcpu->vmcs_region);

    return 0;
}

static void mykvm_free_vcpu_resources(struct mykvm_vcpu *vcpu)
{
    if (vcpu->vmcs_region)
        free_page((unsigned long)vcpu->vmcs_region);
    if (vcpu->vmxon_region)
        free_page((unsigned long)vcpu->vmxon_region);

    vcpu->vmcs_region = NULL;
    vcpu->vmxon_region = NULL;
}

static void read_host_seg(u16 *sel, unsigned long *base, int which)
{
    struct desc_ptr gdtr;

    native_store_gdt(&gdtr);

    switch (which) {
    case 0: asm volatile("mov %%cs, %0" : "=rm"(*sel)); break;
    case 1: asm volatile("mov %%ss, %0" : "=rm"(*sel)); break;
    case 2: asm volatile("mov %%ds, %0" : "=rm"(*sel)); break;
    case 3: asm volatile("mov %%es, %0" : "=rm"(*sel)); break;
    case 4: asm volatile("mov %%fs, %0" : "=rm"(*sel)); break;
    case 5: asm volatile("mov %%gs, %0" : "=rm"(*sel)); break;
    case 6: asm volatile("str %0" : "=rm"(*sel)); break;
    default: *sel = 0; break;
    }

    *sel &= ~0x7;

    if (*sel == 0) {
        *base = 0;
        return;
    }

    {
        struct desc_struct *gdt = (struct desc_struct *)gdtr.address;
        struct desc_struct *d = &gdt[*sel >> 3];
        *base = get_desc_base(d);
    }
}

static int mykvm_setup_vmcs(struct mykvm_vm *vm, struct mykvm_vcpu *vcpu)
{
    u16 sel;
    unsigned long base;
    struct desc_ptr gdtr, idtr;
    unsigned long cr0 = read_cr0();
    unsigned long cr3 = __read_cr3();
    unsigned long cr4 = __read_cr4();
    u32 pinbased, procbased, secondary, exitctl, entryctl;
    u64 eptp;

    if (my_vmclear(vcpu->vmcs_pa))
        return -EIO;
    if (my_vmptrld(vcpu->vmcs_pa))
        return -EIO;

    native_store_gdt(&gdtr);
    store_idt(&idtr);

    pinbased  = adjust_vmx_controls(0, IA32_VMX_PINBASED_CTLS);
    procbased = adjust_vmx_controls(CPU_BASED_HLT_EXITING |
                                    CPU_BASED_ACTIVATE_SECONDARY,
                                    IA32_VMX_PROCBASED_CTLS);
    secondary = adjust_vmx_controls(SECONDARY_EXEC_ENABLE_EPT |
                                    SECONDARY_EXEC_MONITOR_TRAP_FLAG,
                                    IA32_VMX_PROCBASED_CTLS2);
    exitctl   = adjust_vmx_controls(VM_EXIT_HOST_ADDR_SPACE_SIZE, IA32_VMX_EXIT_CTLS);
    entryctl  = adjust_vmx_controls(VM_ENTRY_IA32E_MODE, IA32_VMX_ENTRY_CTLS);

    read_host_seg(&sel, &base, 0); my_vmwrite(VMCS_HOST_CS_SELECTOR, sel);
    read_host_seg(&sel, &base, 1); my_vmwrite(VMCS_HOST_SS_SELECTOR, sel);
    read_host_seg(&sel, &base, 2); my_vmwrite(VMCS_HOST_DS_SELECTOR, sel);
    read_host_seg(&sel, &base, 3); my_vmwrite(VMCS_HOST_ES_SELECTOR, sel);
    read_host_seg(&sel, &base, 4); my_vmwrite(VMCS_HOST_FS_SELECTOR, sel); my_vmwrite(VMCS_HOST_FS_BASE, base);
    read_host_seg(&sel, &base, 5); my_vmwrite(VMCS_HOST_GS_SELECTOR, sel); my_vmwrite(VMCS_HOST_GS_BASE, base);
    read_host_seg(&sel, &base, 6); my_vmwrite(VMCS_HOST_TR_SELECTOR, sel); my_vmwrite(VMCS_HOST_TR_BASE, base);

    my_vmwrite(VMCS_HOST_CR0, cr0);
    my_vmwrite(VMCS_HOST_CR3, cr3);
    my_vmwrite(VMCS_HOST_CR4, cr4);
    my_vmwrite(VMCS_HOST_GDTR_BASE, gdtr.address);
    my_vmwrite(VMCS_HOST_IDTR_BASE, idtr.address);

    my_vmwrite(VMCS_HOST_RSP, 0);
    my_vmwrite(VMCS_HOST_RIP, 0);

    my_vmwrite(VMCS_HOST_SYSENTER_CS, 0);
    my_vmwrite(VMCS_HOST_SYSENTER_ESP, 0);
    my_vmwrite(VMCS_HOST_SYSENTER_EIP, 0);

    my_vmwrite(VMCS_GUEST_CR0, cr0);
    my_vmwrite(VMCS_GUEST_CR3, 0);
    my_vmwrite(VMCS_GUEST_CR4, cr4);

    my_vmwrite(VMCS_GUEST_ES_SELECTOR, 0x10);
    my_vmwrite(VMCS_GUEST_CS_SELECTOR, 0x08);
    my_vmwrite(VMCS_GUEST_SS_SELECTOR, 0x10);
    my_vmwrite(VMCS_GUEST_DS_SELECTOR, 0x10);
    my_vmwrite(VMCS_GUEST_FS_SELECTOR, 0x10);
    my_vmwrite(VMCS_GUEST_GS_SELECTOR, 0x10);
    my_vmwrite(VMCS_GUEST_LDTR_SELECTOR, 0);
    my_vmwrite(VMCS_GUEST_TR_SELECTOR, 0x18);

    my_vmwrite(VMCS_GUEST_ES_LIMIT, 0xffffffff);
    my_vmwrite(VMCS_GUEST_CS_LIMIT, 0xffffffff);
    my_vmwrite(VMCS_GUEST_SS_LIMIT, 0xffffffff);
    my_vmwrite(VMCS_GUEST_DS_LIMIT, 0xffffffff);
    my_vmwrite(VMCS_GUEST_FS_LIMIT, 0xffffffff);
    my_vmwrite(VMCS_GUEST_GS_LIMIT, 0xffffffff);
    my_vmwrite(VMCS_GUEST_LDTR_LIMIT, 0);
    my_vmwrite(VMCS_GUEST_TR_LIMIT, 0xffffffff);
    my_vmwrite(VMCS_GUEST_GDTR_LIMIT, 0);
    my_vmwrite(VMCS_GUEST_IDTR_LIMIT, 0);

    my_vmwrite(VMCS_GUEST_ES_AR_BYTES,   0xc093);
    my_vmwrite(VMCS_GUEST_CS_AR_BYTES,   0xa09b);
    my_vmwrite(VMCS_GUEST_SS_AR_BYTES,   0xc093);
    my_vmwrite(VMCS_GUEST_DS_AR_BYTES,   0xc093);
    my_vmwrite(VMCS_GUEST_FS_AR_BYTES,   0xc093);
    my_vmwrite(VMCS_GUEST_GS_AR_BYTES,   0xc093);
    my_vmwrite(VMCS_GUEST_LDTR_AR_BYTES, 0x10000);
    my_vmwrite(VMCS_GUEST_TR_AR_BYTES,   0x008b);

    my_vmwrite(VMCS_GUEST_ES_BASE, 0);
    my_vmwrite(VMCS_GUEST_CS_BASE, 0);
    my_vmwrite(VMCS_GUEST_SS_BASE, 0);
    my_vmwrite(VMCS_GUEST_DS_BASE, 0);
    my_vmwrite(VMCS_GUEST_FS_BASE, 0);
    my_vmwrite(VMCS_GUEST_GS_BASE, 0);
    my_vmwrite(VMCS_GUEST_LDTR_BASE, 0);
    my_vmwrite(VMCS_GUEST_TR_BASE, 0);
    my_vmwrite(VMCS_GUEST_GDTR_BASE, 0);
    my_vmwrite(VMCS_GUEST_IDTR_BASE, 0);

    my_vmwrite(VMCS_GUEST_DR7, 0x400);
    my_vmwrite(VMCS_GUEST_RSP, vcpu->regs.rsp);
    my_vmwrite(VMCS_GUEST_RIP, vcpu->regs.rip);
    my_vmwrite(VMCS_GUEST_RFLAGS, vcpu->regs.rflags ? vcpu->regs.rflags : 0x2);
    my_vmwrite(VMCS_GUEST_PENDING_DBG_EXC, 0);
    my_vmwrite(VMCS_GUEST_INTERRUPTIBILITY, 0);
    my_vmwrite(VMCS_GUEST_ACTIVITY_STATE, 0);
    my_vmwrite(VMCS_GUEST_SYSENTER_CS, 0);
    my_vmwrite(VMCS_GUEST_SYSENTER_ESP, 0);
    my_vmwrite(VMCS_GUEST_SYSENTER_EIP, 0);

    my_vmwrite(VMCS_CTRL_PIN_BASED, pinbased);
    my_vmwrite(VMCS_CTRL_CPU_BASED, procbased);
    my_vmwrite(VMCS_CTRL_SECONDARY_CPU_BASED, secondary);
    my_vmwrite(VMCS_CTRL_VMEXIT_CONTROLS, exitctl);
    my_vmwrite(VMCS_CTRL_VMENTRY_CONTROLS, entryctl);
    my_vmwrite(VMCS_CTRL_EXCEPTION_BITMAP, 0);
    my_vmwrite(VMCS_CTRL_CR0_MASK, 0);
    my_vmwrite(VMCS_CTRL_CR4_MASK, 0);
    my_vmwrite(VMCS_CTRL_CR0_SHADOW, cr0);
    my_vmwrite(VMCS_CTRL_CR4_SHADOW, cr4);
    my_vmwrite(VMCS_VPID, 1);

    eptp = vm->ept_pml4_pa | 6ULL | (3ULL << 3);
    my_vmwrite(VMCS_CTRL_EPT_POINTER, eptp);

    return 0;
}

static int mykvm_enter_vmx_worker(struct mykvm_vcpu *vcpu)
{
    u64 feature;
    unsigned long cr4;
    int ret;

    if (!my_cpu_has_vmx())
        return -EOPNOTSUPP;

    feature = my_rdmsr64(IA32_FEATURE_CONTROL);
    if (!(feature & 0x1) || !(feature & 0x4))
        return -EOPNOTSUPP;

    cr4 = __read_cr4();
    native_write_cr4(cr4 | CR4_VMXE);

    ret = my_vmxon(vcpu->vmxon_pa);
    if (ret)
        return ret;

    ret = mykvm_setup_vmcs(&g.vm, vcpu);
    if (ret) {
        my_vmxoff();
        return ret;
    }

    return 0;
}

static int mykvm_leave_vmx_worker(struct mykvm_vcpu *vcpu)
{
    (void)vcpu;
    return my_vmxoff();
}

static void mykvm_enter_vmx_on_cpu(void *arg)
{
    struct mykvm_smp_call *call = arg;
    call->ret = mykvm_enter_vmx_worker(call->vcpu);
}

static void mykvm_leave_vmx_on_cpu(void *arg)
{
    struct mykvm_smp_call *call = arg;
    call->ret = mykvm_leave_vmx_worker(call->vcpu);
}

static int mykvm_prepare_vcpu(struct mykvm_vcpu *vcpu)
{
    struct mykvm_smp_call call = {
        .vcpu = vcpu,
        .ret = 0,
    };
    int ret;

    vcpu->cpu = get_cpu();
    ret = smp_call_function_single(vcpu->cpu, mykvm_enter_vmx_on_cpu, &call, 1);
    put_cpu();

    if (ret)
        return ret;

    return call.ret;
}

static void mykvm_teardown_vcpu(struct mykvm_vcpu *vcpu)
{
    struct mykvm_smp_call call = {
        .vcpu = vcpu,
        .ret = 0,
    };

    if (vcpu->cpu >= 0)
        smp_call_function_single(vcpu->cpu, mykvm_leave_vmx_on_cpu, &call, 1);
}

static int mykvm_install_test_instruction(struct mykvm_vm *vm,
                                          const struct mykvm_test_insn *t)
{
    u8 *p;

    if (t->len == 0 || t->len > MYKVM_TEST_MAX_LEN)
        return -EINVAL;
    if (!vm->guest_ram_hva)
        return -EINVAL;
    if (MYKVM_TEST_GPA + 16 >= vm->guest_ram_size)
        return -EINVAL;

    p = (u8 *)vm->guest_ram_hva + MYKVM_TEST_GPA;
    memset(p, 0x90, 16);
    memcpy(p, t->bytes, t->len);
    p[t->len] = 0x90;

    return 0;
}

static int mykvm_run_vcpu(struct mykvm_vcpu *vcpu, struct mykvm_run *run)
{
    u64 err = 0;

    memset(run, 0, sizeof(*run));

    if (my_vmptrld(vcpu->vmcs_pa)) {
        run->exit_reason = MYKVM_EXIT_INTERNAL_ERROR;
        run->internal.guest_rip = vcpu->regs.rip;
        return 0;
    }

    my_vmwrite(VMCS_GUEST_RIP, vcpu->regs.rip);
    my_vmwrite(VMCS_GUEST_RSP, vcpu->regs.rsp);
    my_vmwrite(VMCS_GUEST_RFLAGS, vcpu->regs.rflags ? vcpu->regs.rflags : 0x2);

    /*
     * Placeholder:
     * A correct VMLAUNCH/VMRESUME path needs a proper host RIP/RSP target and
     * a real VM-exit protocol. The previous raw asm stub approach was rejected
     * by objtool/rethunk and was not architecturally sound enough to keep.
     */
    my_vmread(VMCS_RO_VM_INSTRUCTION_ERROR, &err);

    run->exit_reason = MYKVM_EXIT_NOT_IMPLEMENTED;
    run->internal.guest_rip = vcpu->regs.rip;
    run->internal.raw_exit_reason = 0;
    run->internal.qualification = err;

    return 0;
}

static long mykvm_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
    long ret = 0;

    mutex_lock(&g.lock);

    switch (cmd) {
    case MYKVM_VM_CREATE:
        if (g.vm.created) {
            ret = -EEXIST;
            break;
        }

        memset(&g.vm, 0, sizeof(g.vm));
        memset(&g.vcpu, 0, sizeof(g.vcpu));

        g.vcpu.cpu = -1;
        g.vcpu.single_step = true;

        ret = mykvm_alloc_vm_memory(&g.vm);
        if (ret)
            break;

        ret = mykvm_build_ept_1g(&g.vm);
        if (ret)
            break;

        g.vm.created = true;
        break;

    case MYKVM_VCPU_CREATE: {
        struct mykvm_vcpu_create c;

        if (!g.vm.created) {
            ret = -EINVAL;
            break;
        }
        if (g.vcpu.created) {
            ret = -EEXIST;
            break;
        }
        if (copy_from_user(&c, (void __user *)arg, sizeof(c))) {
            ret = -EFAULT;
            break;
        }

        memset(&g.vcpu, 0, sizeof(g.vcpu));
        g.vcpu.id = c.vcpu_id;
        g.vcpu.cpu = -1;
        g.vcpu.single_step = true;
        g.vcpu.regs.rip = MYKVM_TEST_GPA;
        g.vcpu.regs.rsp = 0x8000;
        g.vcpu.regs.rflags = 0x2;

        ret = mykvm_alloc_vcpu_resources(&g.vcpu);
        if (ret)
            break;

        ret = mykvm_prepare_vcpu(&g.vcpu);
        if (ret) {
            mykvm_free_vcpu_resources(&g.vcpu);
            break;
        }

        g.vcpu.created = true;
        break;
    }

    case MYKVM_SET_REGS:
        if (!g.vcpu.created) {
            ret = -EINVAL;
            break;
        }
        if (copy_from_user(&g.vcpu.regs, (void __user *)arg, sizeof(g.vcpu.regs))) {
            ret = -EFAULT;
            break;
        }
        break;

    case MYKVM_GET_REGS:
        if (!g.vcpu.created) {
            ret = -EINVAL;
            break;
        }
        if (copy_to_user((void __user *)arg, &g.vcpu.regs, sizeof(g.vcpu.regs))) {
            ret = -EFAULT;
            break;
        }
        break;

    case MYKVM_SET_TEST: {
        struct mykvm_test_insn t;

        if (!g.vm.created) {
            ret = -EINVAL;
            break;
        }
        if (copy_from_user(&t, (void __user *)arg, sizeof(t))) {
            ret = -EFAULT;
            break;
        }
        ret = mykvm_install_test_instruction(&g.vm, &t);
        if (ret)
            break;

        g.vcpu.regs.rip = MYKVM_TEST_GPA;
        g.vcpu.launched = false;
        break;
    }

    case MYKVM_RUN: {
        struct mykvm_run run;

        if (!g.vm.created || !g.vcpu.created) {
            ret = -EINVAL;
            break;
        }

        memset(&run, 0, sizeof(run));
        ret = mykvm_run_vcpu(&g.vcpu, &run);
        if (ret)
            break;

        if (copy_to_user((void __user *)arg, &run, sizeof(run))) {
            ret = -EFAULT;
            break;
        }
        break;
    }

    default:
        ret = -ENOTTY;
        break;
    }

    mutex_unlock(&g.lock);
    return ret;
}

static const struct file_operations mykvm_fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = mykvm_ioctl,
#ifdef CONFIG_COMPAT
    .compat_ioctl = mykvm_ioctl,
#endif
};

static struct miscdevice mykvm_misc = {
    .minor = MISC_DYNAMIC_MINOR,
    .name  = DRV_NAME,
    .fops  = &mykvm_fops,
    .mode  = 0600,
};

static int __init mykvm_init(void)
{
    mutex_init(&g.lock);
    return misc_register(&mykvm_misc);
}

static void __exit mykvm_exit(void)
{
    mykvm_teardown_vcpu(&g.vcpu);
    mykvm_free_vcpu_resources(&g.vcpu);
    mykvm_free_ept(&g.vm);
    mykvm_free_vm_memory(&g.vm);
    misc_deregister(&mykvm_misc);
}

module_init(mykvm_init);
module_exit(mykvm_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("OpenAI");
MODULE_DESCRIPTION("Educational VT-x scaffold");
