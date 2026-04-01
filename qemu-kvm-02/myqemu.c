#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include "mykvm_uapi.h"

static void die(const char *msg)
{
    perror(msg);
    exit(1);
}

struct testcase {
    const char *name;
    uint8_t bytes[MYKVM_TEST_MAX_LEN];
    uint32_t len;
};

static void print_regs(const struct mykvm_regs *r)
{
    printf("RIP=0x%llx RAX=0x%llx RBX=0x%llx RCX=0x%llx RDX=0x%llx RSP=0x%llx\n",
           (unsigned long long)r->rip,
           (unsigned long long)r->rax,
           (unsigned long long)r->rbx,
           (unsigned long long)r->rcx,
           (unsigned long long)r->rdx,
           (unsigned long long)r->rsp);
}

int main(void)
{
    int fd = open("/dev/mykvm", O_RDWR);
    if (fd < 0)
        die("open /dev/mykvm");

    if (ioctl(fd, MYKVM_VM_CREATE, 0) < 0)
        die("MYKVM_VM_CREATE");

    struct mykvm_vcpu_create vcpu = { .vcpu_id = 0 };
    if (ioctl(fd, MYKVM_VCPU_CREATE, &vcpu) < 0)
        die("MYKVM_VCPU_CREATE");

    struct mykvm_regs regs;
    memset(&regs, 0, sizeof(regs));
    regs.rip = 0x1000;
    regs.rsp = 0x8000;
    regs.rflags = 0x2;
    if (ioctl(fd, MYKVM_SET_REGS, &regs) < 0)
        die("MYKVM_SET_REGS");

    struct testcase tests[] = {
        { "nop",            { 0x90 }, 1 },
        { "cpuid",          { 0x0f, 0xa2 }, 2 },
        { "hlt",            { 0xf4 }, 1 },
        { "out 0xe9, al",   { 0xe6, 0xe9 }, 2 },
        { "in al, 0xe9",    { 0xe4, 0xe9 }, 2 },
        { "mov eax, imm32", { 0xb8, 0x78, 0x56, 0x34, 0x12 }, 5 },
    };

    size_t nr_tests = sizeof(tests) / sizeof(tests[0]);

    for (size_t i = 0; i < nr_tests; i++) {
        struct mykvm_test_insn t;
        struct mykvm_run run;

        memset(&t, 0, sizeof(t));
        memcpy(t.bytes, tests[i].bytes, tests[i].len);
        t.len = tests[i].len;

        regs.rip = 0x1000;
        regs.rsp = 0x8000;
        regs.rflags = 0x2;
        regs.rax = 0x41;
        regs.rbx = regs.rcx = regs.rdx = 0;

        if (ioctl(fd, MYKVM_SET_REGS, &regs) < 0)
            die("MYKVM_SET_REGS(reset)");

        if (ioctl(fd, MYKVM_SET_TEST, &t) < 0)
            die("MYKVM_SET_TEST");

        memset(&run, 0, sizeof(run));
        if (ioctl(fd, MYKVM_RUN, &run) < 0)
            die("MYKVM_RUN");

        if (ioctl(fd, MYKVM_GET_REGS, &regs) < 0)
            die("MYKVM_GET_REGS");

        printf("\n=== test: %s ===\n", tests[i].name);

        switch (run.exit_reason) {
        case MYKVM_EXIT_NOT_IMPLEMENTED:
            printf("result: VT-x guest entry/exit path not implemented yet in this build\n");
            break;
        case MYKVM_EXIT_FAIL_ENTRY:
            printf("result: VM-entry failure\n");
            printf("rip=0x%llx instr_error=%u raw_exit_reason=0x%x qual=0x%llx\n",
                   (unsigned long long)run.fail_entry.guest_rip,
                   run.fail_entry.instruction_error,
                   run.fail_entry.raw_exit_reason,
                   (unsigned long long)run.fail_entry.qualification);
            break;
        case MYKVM_EXIT_INTERNAL_ERROR:
            printf("result: internal error\n");
            printf("rip=0x%llx raw_exit_reason=0x%llx qual=0x%llx\n",
                   (unsigned long long)run.internal.guest_rip,
                   (unsigned long long)run.internal.raw_exit_reason,
                   (unsigned long long)run.internal.qualification);
            break;
        default:
            printf("result: exit reason %u\n", run.exit_reason);
            break;
        }

        print_regs(&regs);
    }

    close(fd);
    return 0;
}
