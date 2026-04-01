#ifndef MYKVM_UAPI_H
#define MYKVM_UAPI_H

#include <linux/types.h>
#include <linux/ioctl.h>

#define MYKVM_IOCTL_BASE      0xB7

#define MYKVM_VM_CREATE       _IO(MYKVM_IOCTL_BASE, 0x01)
#define MYKVM_VCPU_CREATE     _IOW(MYKVM_IOCTL_BASE, 0x02, struct mykvm_vcpu_create)
#define MYKVM_SET_REGS        _IOW(MYKVM_IOCTL_BASE, 0x03, struct mykvm_regs)
#define MYKVM_GET_REGS        _IOR(MYKVM_IOCTL_BASE, 0x04, struct mykvm_regs)
#define MYKVM_SET_TEST        _IOW(MYKVM_IOCTL_BASE, 0x05, struct mykvm_test_insn)
#define MYKVM_RUN             _IOWR(MYKVM_IOCTL_BASE, 0x06, struct mykvm_run)

#define MYKVM_GUEST_RAM_SIZE  (1ULL << 30)   /* 1 GiB */
#define MYKVM_TEST_MAX_LEN    15

enum mykvm_exit_reason {
    MYKVM_EXIT_NONE = 0,
    MYKVM_EXIT_SINGLE_STEP,
    MYKVM_EXIT_IO,
    MYKVM_EXIT_CPUID,
    MYKVM_EXIT_HLT,
    MYKVM_EXIT_NPF,
    MYKVM_EXIT_FAIL_ENTRY,
    MYKVM_EXIT_INTERNAL_ERROR,
    MYKVM_EXIT_NOT_IMPLEMENTED,
};

struct mykvm_vcpu_create {
    __u32 vcpu_id;
};

struct mykvm_regs {
    __u64 rax, rbx, rcx, rdx;
    __u64 rsi, rdi, rsp, rbp;
    __u64 r8, r9, r10, r11, r12, r13, r14, r15;
    __u64 rip;
    __u64 rflags;
};

struct mykvm_test_insn {
    __u8  bytes[MYKVM_TEST_MAX_LEN];
    __u32 len;
};

struct mykvm_run {
    __u32 exit_reason;
    __u32 reserved;

    union {
        struct {
            __u64 guest_rip;
        } single_step;

        struct {
            __u16 port;
            __u8  size;
            __u8  direction; /* 0=out, 1=in */
            __u32 count;
            __u64 data;
            __u64 guest_rip;
        } io;

        struct {
            __u32 eax, ebx, ecx, edx;
            __u64 guest_rip;
        } cpuid;

        struct {
            __u64 guest_rip;
        } hlt;

        struct {
            __u64 exitinfo1;
            __u64 exitinfo2;
            __u64 guest_rip;
        } npf;

        struct {
            __u64 guest_rip;
            __u32 instruction_error;
            __u32 raw_exit_reason;
            __u64 qualification;
        } fail_entry;

        struct {
            __u64 guest_rip;
            __u64 raw_exit_reason;
            __u64 qualification;
        } internal;
    };
};

#endif
