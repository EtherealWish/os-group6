#include <defs.h>
#include <error.h>
#include <stdio.h>
#include <ulib.h>

static volatile int value = 7;
static volatile int untouched = 11;
static const char readonly[] = "read-only";

int main(void)
{
    int pid = fork();
    assert(pid >= 0);
    if (pid == 0)
    {
        assert(value == 7);
        value = 29;
        assert(value == 29 && untouched == 11);
        exit(42);
    }

    value = 19;
    int status = 0;
    assert(waitpid(pid, &status) == 0 && status == 42);
    assert(value == 19 && untouched == 11);

    pid = fork();
    assert(pid >= 0);
    if (pid == 0)
    {
        // Issue one ordinary byte store to verify the hardware permission check.
        asm volatile("sb zero, 0(%0)" :: "r"(readonly) : "memory");
        panic("read-only mapping accepted a store");
    }
    assert(waitpid(pid, &status) == 0 && status == -E_KILLED);
    assert(*(const volatile char *)readonly == 'r');
    cprintf("COW: parent and child stores isolated; user text remained read-only.\n");
    return 0;
}
