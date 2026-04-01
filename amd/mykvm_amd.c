// SPDX-License-Identifier: GPL
/*
 * mykvm_amd.c - educational AMD SVM scaffold
 *
 * This version is intentionally focused on:
 *   - compiling cleanly on modern x86 kernels
 *   - creating VM/vCPU objects
 *   - allocating 1 GiB guest RAM
 *   - building a simple NPT root placeholder
 *   - enabling SVM on one CPU
 *   - preparing a VMCB
 *
 * It does NOT yet implement a correct VMRUN entry/exit execution path.
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
#include <asm/processor.h>
#include <asm/io.h>
#include <asm/page.h>
#include <asm/special_insns.h>
#include <asm/msr-index.h>

#include "mykvm_uapi.h"

#define DRV_NAME "mykvm"

/* AMD SVM MSRs */
#define MSR_VM_CR           0xc0010114
#define MSR_VM_HSAVE_PA     0xc0010117
#define MSR_EFER_AMD        0xc0000080

#define EFER_SVME           (1ULL << 12)

/* CPUID leaves */
#ifndef X86_FEATURE_SVM
#define X86_FEATURE_SVM     (3*32+2)
#endif

#define MYKVM_TEST_GPA      0x1000

/* VMCB intercept bits - minimal symbolic placeholders */
#define SVM_EXIT_READ_CR0   0x000
#define SVM_EXIT_WRITE_CR0  0x010
#define SVM_EXIT_CPUID      0x072
#define SVM_EXIT_HLT        0x078
#define SVM_EXIT_IOIO       0x07b
#define SVM_EXIT_MSR        0x07c
#define SVM_EXIT_NPF        0x400

/* NPT bits */
#define NPT_PRESENT         (1ULL << 0)
#define NPT_RW              (1ULL << 1)
#define NPT_USER            (1ULL << 2)
#define NPT_PSE             (1ULL << 7)

struct vmcb_control_area {
    u32 intercept_cr_read;
    u32 intercept_cr_write;
    u32 intercept_dr_read;
    u32 intercept_dr_write;
    u32 intercept_exceptions;
    u32 intercept_misc1;
    u32 intercept_misc2;
    u8  reserved_1[40];

    u16 pause_filter_thresh;
    u16 pause_filter_count;

    u64 iopm_base_pa;
    u64 msrpm_base_pa;
    u64 tsc_offset;
    u32 asid;
    u8  tlb_control;
    u8  reserved_2[3];

    u64 int_ctl;
    u64 int_vector;
    u64 int_state;
    u64 exit_code;
    u64 exit_code_hi;
    u64 exit_info_1;
    u64 exit_info_2;
    u64 exit_int_info;
    u64 np_enable;
    u64 avic_apic_bar;
    u64 ghcb_pa;
    u64 event_inj;
    u64 n_cr3;
    u64 lbr_virtualization_enable;
    u64 vmcb_clean;
    u64 nrip;
    u8  reserved_3[88];
} __packed;

struct vmcb_save_area {
    u16 es_selector;
    u16 es_attrib;
    u32 es_limit;
    u64 es_base;

    u16 cs_selector;
    u16 cs_attrib;
    u32 cs_limit;
    u64 cs_base;

    u16 ss_selector;
    u16 ss_attrib;
    u32 ss_limit;
    u64 ss_base;

    u16 ds_selector;
    u16 ds_attrib;
    u32 ds_limit;
    u64 ds_base;

    u16 fs_selector;
    u16 fs_attrib;
    u32 fs_limit;
    u64 fs_base;

    u16 gs_selector;
    u16 gs_attrib;
    u32 gs_limit;
    u64 gs_base;

    u16 gdtr_selector;
    u16 gdtr_attrib;
    u32 gdtr_limit;
    u64 gdtr_base;

    u16 ldtr_selector;
    u16 ldtr_attrib;
    u32 ldtr_limit;
    u64 ldtr_base;

    u16 idtr_selector;
    u16 idtr_attrib;
    u32 idtr_limit;
    u64 idtr_base;

    u16 tr_selector;
    u16 tr_attrib;
    u32 tr_limit;
    u64 tr_base;

    u8  reserved_1[43];
    u8  cpl;
    u32 efer;
    u8  reserved_2[112];

    u64 cr4;
    u64 cr3;
    u64 cr0;
    u64 dr7;
    u64 dr6;
    u64 rflags;
    u64 rip;
    u8  reserved_3[88];
    u64 rsp;
    u8  reserved_4[24];
    u64 rax;
    u64 star;
    u64 lstar;
    u64 cstar;
    u64 sfmask;
    u64 kernel_gs_base;
    u64 sysenter_cs;
    u64 sysenter_esp;
    u64 sysenter_eip;
    u64 cr2;
    u8  reserved_5[32];
    u64 g_pat;
    u64 dbgctl;
    u64 br_from;
    u64 br_to;
    u64 last_excp_from;
    u64 last_excp_to;
} __packed;

struct vmcb {
    struct vmcb_control_area control;
    struct vmcb_save_area save;
} __packed;

struct mykvm_vm {
    bool created;

    void *guest_ram_hva;
    u64 guest_ram_size;

    u64 *npt_pml4;
    phys_addr_t npt_pml4_pa;
    u64 *npt_pdpt;
    phys_addr_t npt_pdpt_pa;
    u64 *npt_pd;
    phys_addr_t npt_pd_pa;

    void *iopm;
    phys_addr_t iopm_pa;
    void *msrpm;
    phys_addr_t msrpm_pa;
};

struct mykvm_vcpu {
    bool created;
    bool launched;
    bool single_step;
    u32 id;
    int cpu;

    struct mykvm_regs regs;

    struct vmcb *vmcb;
    phys_addr_t vmcb_pa;

    void *hsave;
    phys_addr_t hsave_pa;
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

static inline void my_wrmsr64(u32 msr, u64 val)
{
    wrmsrl(msr, val);
}

static inline int my_cpu_has_svm(void)
{
    u32 eax = 0x80000001, ebx = 0, ecx = 0, edx = 0;
    native_cpuid(&eax, &ebx, &ecx, &edx);
    return !!(ecx & BIT(2));
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

static int mykvm_build_npt_1g(struct mykvm_vm *vm)
{
    int i;

    vm->npt_pml4 = (u64 *)get_zeroed_page(GFP_KERNEL);
    if (!vm->npt_pml4)
        return -ENOMEM;
    vm->npt_pml4_pa = virt_to_phys(vm->npt_pml4);

    vm->npt_pdpt = (u64 *)get_zeroed_page(GFP_KERNEL);
    if (!vm->npt_pdpt)
        return -ENOMEM;
    vm->npt_pdpt_pa = virt_to_phys(vm->npt_pdpt);

    vm->npt_pd = (u64 *)get_zeroed_page(GFP_KERNEL);
    if (!vm->npt_pd)
        return -ENOMEM;
    vm->npt_pd_pa = virt_to_phys(vm->npt_pd);

    vm->npt_pml4[0] = vm->npt_pdpt_pa | NPT_PRESENT | NPT_RW | NPT_USER;
    vm->npt_pdpt[0] = vm->npt_pd_pa   | NPT_PRESENT | NPT_RW | NPT_USER;

    for (i = 0; i < 512; i++) {
        phys_addr_t hpa = virt_to_phys((char *)vm->guest_ram_hva + (i * 0x200000ULL));
        vm->npt_pd[i] = hpa | NPT_PRESENT | NPT_RW | NPT_USER | NPT_PSE;
    }

    return 0;
}

static void mykvm_free_npt(struct mykvm_vm *vm)
{
    if (vm->npt_pd)
        free_page((unsigned long)vm->npt_pd);
    if (vm->npt_pdpt)
        free_page((unsigned long)vm->npt_pdpt);
    if (vm->npt_pml4)
        free_page((unsigned long)vm->npt_pml4);

    vm->npt_pd = NULL;
    vm->npt_pdpt = NULL;
    vm->npt_pml4 = NULL;
}

static int mykvm_alloc_iopm_msrpm(struct mykvm_vm *vm)
{
    vm->iopm = (void *)__get_free_pages(GFP_KERNEL | __GFP_ZERO, 3);   /* 3 pages = 12KB */
    if (!vm->iopm)
        return -ENOMEM;
    vm->iopm_pa = virt_to_phys(vm->iopm);

    vm->msrpm = (void *)__get_free_pages(GFP_KERNEL | __GFP_ZERO, 1);  /* placeholder */
    if (!vm->msrpm)
        return -ENOMEM;
    vm->msrpm_pa = virt_to_phys(vm->msrpm);

    return 0;
}

static void mykvm_free_iopm_msrpm(struct mykvm_vm *vm)
{
    if (vm->msrpm)
        free_pages((unsigned long)vm->msrpm, 1);
    if (vm->iopm)
        free_pages((unsigned long)vm->iopm, 3);

    vm->msrpm = NULL;
    vm->iopm = NULL;
}

static int mykvm_alloc_vcpu_resources(struct mykvm_vcpu *vcpu)
{
    vcpu->vmcb = (struct vmcb *)get_zeroed_page(GFP_KERNEL);
    if (!vcpu->vmcb)
        return -ENOMEM;
    vcpu->vmcb_pa = virt_to_phys(vcpu->vmcb);

    vcpu->hsave = (void *)get_zeroed_page(GFP_KERNEL);
    if (!vcpu->hsave)
        return -ENOMEM;
    vcpu->hsave_pa = virt_to_phys(vcpu->hsave);

    return 0;
}

static void mykvm_free_vcpu_resources(struct mykvm_vcpu *vcpu)
{
    if (vcpu->hsave)
        free_page((unsigned long)vcpu->hsave);
    if (vcpu->vmcb)
        free_page((unsigned long)vcpu->vmcb);

    vcpu->hsave = NULL;
    vcpu->vmcb = NULL;
}

static void mykvm_setup_vmcb(struct mykvm_vm *vm, struct mykvm_vcpu *vcpu)
{
    struct vmcb *vmcb = vcpu->vmcb;
    unsigned long cr0 = read_cr0();
    unsigned long cr4 = __read_cr4();
    u64 efer = my_rdmsr64(MSR_EFER_AMD);

    memset(vmcb, 0, sizeof(*vmcb));

    vmcb->control.iopm_base_pa = vm->iopm_pa;
    vmcb->control.msrpm_base_pa = vm->msrpm_pa;
    vmcb->control.asid = 1;
    vmcb->control.np_enable = 1;
    vmcb->control.n_cr3 = vm->npt_pml4_pa;

    /*
     * Minimal intercepts for experiments.
     * Real SVM code usually manipulates bitmaps more carefully.
     */
    vmcb->control.intercept_misc1 = 0;
    vmcb->control.intercept_misc2 = 0;

    /* Save area - simplified flat long-mode-ish setup */
    vmcb->save.es_selector = 0x10;
    vmcb->save.cs_selector = 0x08;
    vmcb->save.ss_selector = 0x10;
    vmcb->save.ds_selector = 0x10;
    vmcb->save.fs_selector = 0x10;
    vmcb->save.gs_selector = 0x10;
    vmcb->save.tr_selector = 0x18;

    vmcb->save.es_attrib = 0xc093;
    vmcb->save.cs_attrib = 0xa09b;
    vmcb->save.ss_attrib = 0xc093;
    vmcb->save.ds_attrib = 0xc093;
    vmcb->save.fs_attrib = 0xc093;
    vmcb->save.gs_attrib = 0xc093;
    vmcb->save.tr_attrib = 0x008b;

    vmcb->save.es_limit = 0xffffffff;
    vmcb->save.cs_limit = 0xffffffff;
    vmcb->save.ss_limit = 0xffffffff;
    vmcb->save.ds_limit = 0xffffffff;
    vmcb->save.fs_limit = 0xffffffff;
    vmcb->save.gs_limit = 0xffffffff;
    vmcb->save.tr_limit = 0xffffffff;

    vmcb->save.es_base = 0;
    vmcb->save.cs_base = 0;
    vmcb->save.ss_base = 0;
    vmcb->save.ds_base = 0;
    vmcb->save.fs_base = 0;
    vmcb->save.gs_base = 0;
    vmcb->save.tr_base = 0;

    vmcb->save.gdtr_base = 0;
    vmcb->save.idtr_base = 0;
    vmcb->save.gdtr_limit = 0;
    vmcb->save.idtr_limit = 0;

    vmcb->save.cr0 = cr0;
    vmcb->save.cr3 = 0;
    vmcb->save.cr4 = cr4;
    vmcb->save.dr7 = 0x400;
    vmcb->save.rflags = vcpu->regs.rflags ? vcpu->regs.rflags : 0x2;
    vmcb->save.rip = vcpu->regs.rip;
    vmcb->save.rsp = vcpu->regs.rsp;
    vmcb->save.rax = vcpu->regs.rax;
    vmcb->save.efer = (u32)efer;
}

static int mykvm_enter_svm_worker(struct mykvm_vcpu *vcpu)
{
    u64 vm_cr, efer;

    if (!my_cpu_has_svm())
        return -EOPNOTSUPP;

    vm_cr = my_rdmsr64(MSR_VM_CR);
    if (vm_cr & BIT(4)) /* SVMDIS */
        return -EOPNOTSUPP;

    efer = my_rdmsr64(MSR_EFER_AMD);
    my_wrmsr64(MSR_EFER_AMD, efer | EFER_SVME);

    my_wrmsr64(MSR_VM_HSAVE_PA, vcpu->hsave_pa);

    mykvm_setup_vmcb(&g.vm, vcpu);
    return 0;
}

static int mykvm_leave_svm_worker(struct mykvm_vcpu *vcpu)
{
    u64 efer = my_rdmsr64(MSR_EFER_AMD);
    my_wrmsr64(MSR_EFER_AMD, efer & ~EFER_SVME);
    (void)vcpu;
    return 0;
}

static void mykvm_enter_svm_on_cpu(void *arg)
{
    struct mykvm_smp_call *call = arg;
    call->ret = mykvm_enter_svm_worker(call->vcpu);
}

static void mykvm_leave_svm_on_cpu(void *arg)
{
    struct mykvm_smp_call *call = arg;
    call->ret = mykvm_leave_svm_worker(call->vcpu);
}

static int mykvm_prepare_vcpu(struct mykvm_vcpu *vcpu)
{
    struct mykvm_smp_call call = {
        .vcpu = vcpu,
        .ret = 0,
    };
    int ret;

    vcpu->cpu = get_cpu();
    ret = smp_call_function_single(vcpu->cpu, mykvm_enter_svm_on_cpu, &call, 1);
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
        smp_call_function_single(vcpu->cpu, mykvm_leave_svm_on_cpu, &call, 1);
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
    memset(run, 0, sizeof(*run));

    /*
     * Placeholder:
     * A correct AMD SVM VMRUN/exit path needs a proper assembly world-switch
     * implementation and save/restore protocol.
     */
    run->exit_reason = MYKVM_EXIT_NOT_IMPLEMENTED;
    run->internal.guest_rip = vcpu->regs.rip;
    run->internal.raw_exit_reason = 0;
    run->internal.qualification = 0;

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

        ret = mykvm_build_npt_1g(&g.vm);
        if (ret)
            break;

        ret = mykvm_alloc_iopm_msrpm(&g.vm);
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
        if (g.vcpu.vmcb) {
            g.vcpu.vmcb->save.rip = g.vcpu.regs.rip;
            g.vcpu.vmcb->save.rsp = g.vcpu.regs.rsp;
            g.vcpu.vmcb->save.rflags = g.vcpu.regs.rflags ? g.vcpu.regs.rflags : 0x2;
            g.vcpu.vmcb->save.rax = g.vcpu.regs.rax;
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
        if (g.vcpu.vmcb)
            g.vcpu.vmcb->save.rip = MYKVM_TEST_GPA;
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
    mykvm_free_iopm_msrpm(&g.vm);
    mykvm_free_npt(&g.vm);
    mykvm_free_vm_memory(&g.vm);
    misc_deregister(&mykvm_misc);
}

module_init(mykvm_init);
module_exit(mykvm_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("OpenAI");
MODULE_DESCRIPTION("Educational AMD SVM scaffold");
