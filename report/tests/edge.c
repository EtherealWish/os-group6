#include <defs.h>
#include <elf.h>
#include <error.h>
#include <stdio.h>
#include <ulib.h>

static volatile int shared_value = 7;

static const struct {
    struct elfhdr header;
    struct proghdr segment;
    uint32_t instructions[3];
} program = {
    .header = {
        .e_magic = ELF_MAGIC, .e_elf = {2, 1, 1},
        .e_type = 2, .e_machine = 243, .e_version = 1,
        .e_entry = 0x900000, .e_phoff = sizeof(struct elfhdr),
        .e_ehsize = sizeof(struct elfhdr),
        .e_phentsize = sizeof(struct proghdr), .e_phnum = 1,
    },
    .segment = {
        .p_type = ELF_PT_LOAD, .p_flags = ELF_PF_R | ELF_PF_X,
        .p_offset = sizeof(struct elfhdr) + sizeof(struct proghdr),
        .p_va = 0x900000, .p_filesz = 12, .p_memsz = 16,
        .p_align = 1,
    },
    .instructions = {
        0x04d00593, // addi a1, zero, 77
        0x00100513, // addi a0, zero, SYS_exit
        0x00000073, // ecall
    },
};

int main(void)
{
    int pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        assert(shared_value == 7);
        shared_value = 19;
        assert(exec("truncated", 9, &program, 16) == -E_INVAL_ELF);
        assert(exec("badptr", 6, (void *)0x7fff0000, 16) == -E_INVAL);
        assert(shared_value == 19);
        exec("replacement", 11, &program, sizeof(program));
        panic("successful exec must enter the replacement image");
    }
    int status = 0;
    assert(waitpid(pid, (void *)0x7fff0000) == -E_INVAL);
    assert(waitpid(pid, (void *)0x7fffbffe) == -E_INVAL);
    assert(waitpid(pid, (void *)&main) == -E_INVAL);
    assert(waitpid(pid, &status) == 0 && status == 77);
    assert(shared_value == 7);
    assert(waitpid(pid, NULL) == -E_BAD_PROC);
    cprintf("EDGE: fork isolation, unmapped and cross-page pointers, failed and successful exec passed.\n");
    return 0;
}
