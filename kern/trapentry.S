/* See COPYRIGHT for copyright information. */

#include <inc/mmu.h>
#include <inc/memlayout.h>
#include <inc/trap.h>



###################################################################
# exceptions/interrupts
###################################################################

/* TRAPHANDLER defines a globally-visible function for handling a trap.
 * It pushes a trap number onto the stack, then jumps to _alltraps.
 * Use TRAPHANDLER for traps where the CPU automatically pushes an error code.
 *
 * You shouldn't call a TRAPHANDLER function from C, but you may
 * need to _declare_ one in C (for instance, to get a function pointer
 * during IDT setup).  You can declare the function with
 *   void NAME();
 * where NAME is the argument passed to TRAPHANDLER.
 */
#define TRAPHANDLER(name, num)						\
	.globl name;		/* define global symbol for 'name' */	\
	.type name, @function;	/* symbol type is function */		\
	.align 2;		/* align function definition */		\
	name:			/* function starts here */		\
	pushl $(num);							\
	jmp _alltraps

/* Use TRAPHANDLER_NOEC for traps where the CPU doesn't push an error code.
 * It pushes a 0 in place of the error code, so the trap frame has the same
 * format in either case.
 */
#define TRAPHANDLER_NOEC(name, num)					\
	.globl name;							\
	.type name, @function;						\
	.align 2;							\
	name:								\
	pushl $0;							\
	pushl $(num);							\
	jmp _alltraps

.text

/*
 * Lab 3: Your code here for generating entry points for the different traps.
 */

// HINT 1 : TRAPHANDLER_NOEC(t_divide, T_DIVIDE);
//          Do something like this if there is no error code for the trap
// HINT 2 : TRAPHANDLER(t_dblflt, T_DBLFLT);
//          Do something like this if the trap includes an error code..
// HINT 3 : READ Intel's manual to check if the trap includes an error code
//          or not...

TRAPHANDLER_NOEC(t_divide,  T_DIVIDE)
TRAPHANDLER_NOEC(t_debug,   T_DEBUG)
TRAPHANDLER_NOEC(t_nmi,     T_NMI)
TRAPHANDLER_NOEC(t_brkpt,   T_BRKPT)
TRAPHANDLER_NOEC(t_oflow,   T_OFLOW)
TRAPHANDLER_NOEC(t_bound,   T_BOUND)
TRAPHANDLER_NOEC(t_illop,   T_ILLOP)
TRAPHANDLER_NOEC(t_device,  T_DEVICE)
/* Double Fault DOES push an error code, but it is always 0 */
TRAPHANDLER(t_dblflt,       T_DBLFLT)
/* T_COPROC(9) is unused in JOS. */
TRAPHANDLER(t_tss,          T_TSS)
TRAPHANDLER(t_segnp,        T_SEGNP)
TRAPHANDLER(t_stack,        T_STACK)
TRAPHANDLER(t_gpflt,        T_GPFLT)
TRAPHANDLER(t_pgflt,        T_PGFLT)
/* T_RES(15) is reserved */
TRAPHANDLER_NOEC(t_fperr,   T_FPERR)
/* #17 alignment check DOES push an error code. */
TRAPHANDLER(t_align,        T_ALIGN)
/* #18 machine check has no error code. */
TRAPHANDLER_NOEC(t_mchk,    T_MCHK)
TRAPHANDLER_NOEC(t_simderr, T_SIMDERR)
TRAPHANDLER_NOEC(t_syscall, T_SYSCALL)

/*
 * Lab 3: Your code here for _alltraps
 */
.globl _alltraps
_alltraps:
    # pushal saves (EAX,ECX,EDX,EBX,ESP,EBP,ESI,EDI) in that order
    pushal

    # push %ds, then %es
    pushl %ds
    pushl %es

    # Load kernel data segment into DS and ES
    movw $GD_KD, %ax
    movw %ax, %ds
    movw %ax, %es

    # Now pushl %esp as argument to trap()
    pushl %esp

    call trap

    # Remove the argument from the stack
    addl $4, %esp

    # pop es, ds
    popl %es
    popl %ds

    # popal to restore EDI, ESI, EBP, OESP, EBX, EDX, ECX, EAX
    popal

    # Now remove the trapno and errcode that macros put on stack
    addl $8, %esp

    # Return from interrupt
    iret

