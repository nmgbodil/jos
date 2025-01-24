// Simple command-line kernel monitor useful for
// controlling the kernel and exploring the system interactively.

#include <inc/stdio.h>
#include <inc/string.h>
#include <inc/memlayout.h>
#include <inc/assert.h>
#include <inc/x86.h>

#include <kern/console.h>
#include <kern/monitor.h>
#include <kern/kdebug.h>

#include <kern/hidden.h>

#define CMDBUF_SIZE	80	// enough for one VGA text line


struct Command {
	const char *name;
	const char *desc;
	// return -1 to force monitor to exit
	int (*func)(int argc, char** argv, struct Trapframe* tf);
};

// LAB 1: add your command to here...
static struct Command commands[] = {
	{ "help", "Display this list of commands", mon_help },
	{ "kerninfo", "Display information about the kernel", mon_kerninfo },
	{ "hidden", "Run hidden test cases", exec_hidden_cases},
	{ "backtrace", "Display a stack backtrace", mon_backtrace},
	{ "show", "Display ASCII art", show}
};

/***** Implementations of basic kernel monitor commands *****/

int
mon_help(int argc, char **argv, struct Trapframe *tf)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(commands); i++)
		cprintf("%s - %s\n", commands[i].name, commands[i].desc);
	return 0;
}

int
mon_kerninfo(int argc, char **argv, struct Trapframe *tf)
{
	extern char _start[], entry[], etext[], edata[], end[];

	cprintf("Special kernel symbols:\n");
	cprintf("  _start                  %08x (phys)\n", _start);
	cprintf("  entry  %08x (virt)  %08x (phys)\n", entry, entry - KERNBASE);
	cprintf("  etext  %08x (virt)  %08x (phys)\n", etext, etext - KERNBASE);
	cprintf("  edata  %08x (virt)  %08x (phys)\n", edata, edata - KERNBASE);
	cprintf("  end    %08x (virt)  %08x (phys)\n", end, end - KERNBASE);
	cprintf("Kernel executable memory footprint: %dKB\n",
	ROUNDUP(end - entry, 1024) / 1024);
	return 0;
}

int
mon_backtrace(int argc, char **argv, struct Trapframe *tf)
{
	// LAB 1: Your code here.
    // HINT 1: use read_ebp().
    // HINT 2: print the current ebp on the first line (not current_ebp[0])

	uint32_t *ebp = (uint32_t*)read_ebp();
	struct Eipdebuginfo info;
	cprintf("Stack backtrace:\n");

	// Walk the stack frames
	while (ebp != 0) {
		uint32_t saved_ebp = ebp[0];
		uint32_t return_eip = ebp[1];

		uint32_t arg0 = ebp[2];
		uint32_t arg1 = ebp[3];
		uint32_t arg2 = ebp[4];
		uint32_t arg3 = ebp[5];
		uint32_t arg4 = ebp[6];

		cprintf(
			" ebp %08x eip %08x args %08x &08x %08x %08x %08x\n",
			(uint32_t)ebp,
			return_eip,
			arg0,
			arg1,
			arg2,
			arg3,
			arg4
		);

		//LLMPrompt: print offset
		if (debuginfo_eip(return_eip, &info) == 0) {
			cprintf(" %s:%d: %.*s+%d\n",
			info.eip_file,
			info.eip_line,
			info.eip_fn_namelen,
			info.eip_fn_name,
			return_eip - info.eip_fn_addr);
		}
		//Move to the previous stack frame
		ebp = (uint32_t*)saved_ebp;
	}
	return 0;
}

int exec_hidden_cases(int argc, char **argv, struct Trapframe *tf) {
	hidden_test_cases();
	return 0;
}

/***** Kernel monitor command interpreter *****/

#define WHITESPACE "\t\r\n "
#define MAXARGS 16

static int
runcmd(char *buf, struct Trapframe *tf)
{
	int argc;
	char *argv[MAXARGS];
	int i;

	// Parse the command buffer into whitespace-separated arguments
	argc = 0;
	argv[argc] = 0;
	while (1) {
		// gobble whitespace
		while (*buf && strchr(WHITESPACE, *buf))
			*buf++ = 0;
		if (*buf == 0)
			break;

		// save and scan past next arg
		if (argc == MAXARGS-1) {
			cprintf("Too many arguments (max %d)\n", MAXARGS);
			return 0;
		}
		argv[argc++] = buf;
		while (*buf && !strchr(WHITESPACE, *buf))
			buf++;
	}
	argv[argc] = 0;

	// Lookup and invoke the command
	if (argc == 0)
		return 0;
	for (i = 0; i < ARRAY_SIZE(commands); i++) {
		if (strcmp(argv[0], commands[i].name) == 0)
			return commands[i].func(argc, argv, tf);
	}
	cprintf("Unknown command '%s'\n", argv[0]);
	return 0;
}

void
monitor(struct Trapframe *tf)
{
	char *buf;

	cprintf("Welcome to the JOS kernel monitor!\n");
	cprintf("Type 'help' for a list of commands.\n");


	while (1) {
		buf = readline("K> ");
		if (buf != NULL)
			if (runcmd(buf, tf) < 0)
				break;
	}
}

// ANSI escape codes for colors
#define RED "\033[31m"
#define MAGENTA "\033[35m"
#define YELLOW "\033[1;93m"
#define DARK_BLUE "\033[38;5;17m"  // Navy Blue
#define LIGHT_BLUE "\033[38;5;195m" // Very Light Blue
#define WHITE "\033[97m"
#define RESET "\033[0m"

int
show(int argc, char **argv, struct Trapframe* tf) {
    const char *ascii_art[] = {
        YELLOW "@@@@@@@@@@@@@@@@@@@@@@@@%" RESET "       " RED ".*******************." RESET "       " YELLOW "@@@@@@@@@@@@@@@@@@@@@@@@" RESET,
        YELLOW "@@@@@@@@@@@@@@@@@@@@@@&" RESET "      " RED ",*/******************////*//," RESET "       " YELLOW ".@@@@@@@@@@@@@@@@@@" RESET,
        YELLOW "@@@@@@@@@@@@@@@@@@@@@." RESET "      " RED "**********************************" RESET "     " YELLOW ".@@@@@@@@@@@@@@@@@" RESET,
        YELLOW "@@@@@@@@@@@@@@@@@@@@" RESET "      " RED "*********/*********/*/*///***//*****, " RESET "      " YELLOW "@@@@@@@@@@@@" RESET,
        YELLOW "@@@@@@@@@@@@@@@@@@@" RESET "      " RED "***************.                     " RESET "           " YELLOW "@@@@@@@@@@" RESET,
        YELLOW "@@@@@@@@@@@@@@@@@@/" RESET "      " RED "************                           " RESET "            " YELLOW ",@@@@@@@@" RESET,
        YELLOW "@@@@@@@@@@@@@@@@@@" RESET "      " RED "***********        " RESET LIGHT_BLUE ".*#%%%%%%%%%%%%%%%%%%%%#/ " RESET "        " YELLOW ".@@@@@@@" RESET,
        YELLOW "@@@@@@@@@@@@@@@@@," RESET "      " RED "***********      " RESET DARK_BLUE ".((" RESET LIGHT_BLUE "%%%%%%%%%%%" RESET WHITE "&@@@@@@@@@@@&" RESET LIGHT_BLUE "%%%%% " RESET "      " YELLOW "@@@@" RESET,
        YELLOW "@@@@@@@@@@@@@@@@@" RESET "      " RED "*********/*/     " RESET DARK_BLUE ".(((" RESET LIGHT_BLUE "#%%%%%%%%%%%" RESET WHITE "&&@@@@@@@@@@&" RESET LIGHT_BLUE "%%%%%%* " RESET "     " YELLOW "@@@" RESET,
        YELLOW "@@@@@@@&,       " RESET "       " RED "*********/**     " RESET DARK_BLUE "/(((((" RESET LIGHT_BLUE "%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%/ " RESET "    " YELLOW ".@@@" RESET,
        YELLOW "@@@/" RESET "                   " RED "************     " RESET DARK_BLUE "/((((((" RESET LIGHT_BLUE "#%%%%%%%%%%%%%%%%%%%%%%%%%%%#( " RESET "    " YELLOW "@" RESET,
        YELLOW "@&" RESET RED "       ,*//*. " RESET "       " RED "************.    " RESET DARK_BLUE ",((((((((((" RESET LIGHT_BLUE "#%%%%%%%%%%%%%#" RESET DARK_BLUE "((((((((((/ " RESET "    " YELLOW "@" RESET,
        YELLOW "@" RESET RED "      *//***** " RESET "       " RED "**************      " RESET DARK_BLUE "(((((((((((((((((((((((((((((((((* " RESET "    " YELLOW "@" RESET,
        YELLOW "@" RESET RED "     ********, " RESET "       " RED "**************,       " RESET DARK_BLUE "/(((((((((((((((((((((((((((/   " RESET "    " YELLOW "@" RESET,
        YELLOW "@" RESET RED "     ********  " RESET "       " RED "**************/*         " RESET DARK_BLUE ".((((((((((((((((((/,       " RESET "   " YELLOW "%@" RESET,
        YELLOW "@" RESET RED "    .********  " RESET "       " RED "*****************,                                     " RESET " " YELLOW "@@@@" RESET,
        YELLOW "&" RESET RED "    ,********  " RESET "       " RED "********************,                         .**/. " RESET "    " YELLOW "&@@@@" RESET,
        YELLOW "#" RESET RED "    *********  " RESET "       " RED "***************************,,.....,,**************. " RESET "    " YELLOW "#@@@@" RESET,
        YELLOW "/" RESET RED "    *********  " RESET "       " RED "*************************/*********/**************, " RESET "    " YELLOW "@@@@" RESET,
        YELLOW "," RESET RED "    *********  " RESET "       " RED "*************************************************** " RESET "    " YELLOW "%@@@" RESET,
        YELLOW "" RESET RED "     *********  " RESET "       " RED "*************************************************** " RESET "    " YELLOW "*@@@" RESET,
        YELLOW "" RESET RED "    .*********. " RESET "       " RED "*************************************************** " RESET "    " YELLOW "*@@@" RESET,
        YELLOW "" RESET RED "    .*********. " RESET "       " RED "*************************************************** " RESET "    " YELLOW "#@@@" RESET,
        YELLOW "," RESET RED "   .*********. " RESET "       " RED "*************************************************** " RESET "    " YELLOW "@@@" RESET,
        YELLOW "/" RESET RED "    *********. " RESET "       " RED "**************************************************, " RESET "    " YELLOW "@@@" RESET,
        YELLOW "%" RESET RED "    *********. " RESET "       " RED "**************************************************. " RESET "    " YELLOW "#@@@" RESET,
        YELLOW "@" RESET RED "    ,********, " RESET "       " RED "************************************************* " RESET "    " YELLOW "@@@@@" RESET,
        YELLOW "@" RESET RED "     ********* " RESET "       " RED "************************************************, " RESET "    " YELLOW ",@@@@" RESET,
        YELLOW "@&" RESET RED "     .******, " RESET "       " RED "************************************************ " RESET "    " YELLOW "#@@@@" RESET,
        YELLOW "@@@," RESET RED "                " RESET "   " RED ".*******************.    .,,********************," RESET "    " YELLOW "@@@@@" RESET
    };

    for (size_t i = 0; i < sizeof(ascii_art) / sizeof(ascii_art[0]); i++) {
        cprintf("%s\n", ascii_art[i]);
    }

    return 0;
}