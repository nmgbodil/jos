#include <inc/x86.h>
#include <inc/mmu.h>
#include <inc/error.h>
#include <inc/string.h>
#include <inc/assert.h>

#include <kern/pmap.h>
#include <kern/kclock.h>
#include <kern/env.h>

// These variables are set by i386_detect_memory()
size_t npages;                 // Amount of physical memory (in pages)
static size_t npages_basemem;  // Amount of base memory (in pages)

// These variables are set in mem_init()
pde_t *kern_pgdir;      // Kernel's initial page directory
struct PageInfo *pages; // Physical page state array
static struct PageInfo *page_free_list; // Free list of physical pages

// --------------------------------------------------------------
// Detect machine's physical memory setup.
// --------------------------------------------------------------

static int
nvram_read(int r)
{
    return mc146818_read(r) | (mc146818_read(r + 1) << 8);
}

static void
i386_detect_memory(void)
{
    size_t basemem, extmem, ext16mem, totalmem;

    // Use CMOS calls to measure available base & extended memory.
    // (CMOS calls return results in kilobytes.)
    basemem   = nvram_read(NVRAM_BASELO);
    extmem    = nvram_read(NVRAM_EXTLO);
    ext16mem  = nvram_read(NVRAM_EXT16LO) * 64;

    // Calculate the number of physical pages available in both base
    // and extended memory.
    if (ext16mem)
        totalmem = 16 * 1024 + ext16mem;    // 16MB + ext16
    else if (extmem)
        totalmem = 1 * 1024 + extmem;       // 1MB + ext
    else
        totalmem = basemem;

    npages = totalmem / (PGSIZE / 1024);
    npages_basemem = basemem / (PGSIZE / 1024);

    cprintf("Physical memory: %uK available, base = %uK, extended = %uK\n",
            totalmem, basemem, totalmem - basemem);
}

// --------------------------------------------------------------
// boot_alloc
//   Simple allocator used during early boot to reserve or return
//   next available kernel virtual address range.
// --------------------------------------------------------------

static void *
boot_alloc(uint32_t n)
{
    static char *nextfree; // virtual address of next byte of free mem
    char *result;

    // Initialize nextfree if first time. 'end' is a linker symbol.
    if (!nextfree) {
        extern char end[];
        // Round up end to page boundary
        nextfree = ROUNDUP((char *)end, PGSIZE);
    }

    //LLM Prompt: Round up page boundary after moving nextfree by n bytes
    // If n > 0, allocate and advance nextfree;
    // if n == 0, just return the current nextfree without allocating.
    if (n > 0) {
        result = nextfree;
        // Bump nextfree by 'n' bytes, round up to page boundary
        nextfree = ROUNDUP(nextfree + n, PGSIZE);
        return result;
    } else {
        // n == 0, return the current free pointer
        return nextfree;
    }
}

// --------------------------------------------------------------
// Set up a two-level page table:
//   kern_pgdir is its linear (virtual) address of the root
//
// This function only sets up the kernel part of the address space,
// i.e. addresses >= UTOP.  The user part is set up later.
// --------------------------------------------------------------

static void boot_map_region(pde_t *pgdir, uintptr_t va, size_t size,
                            physaddr_t pa, int perm);
static void check_page_free_list(bool only_low_memory);
static void check_page_alloc(void);
static void check_kern_pgdir(void);
static physaddr_t check_va2pa(pde_t *pgdir, uintptr_t va);
static void check_page(void);
static void check_page_installed_pgdir(void);

void
mem_init(void)
{
    uint32_t cr0;

    // Find out how much memory we have (npages, npages_basemem).
    i386_detect_memory();

    // Create initial page directory (one page)
    kern_pgdir = (pde_t *) boot_alloc(PGSIZE);
    memset(kern_pgdir, 0, PGSIZE);

    // Recursively insert PD in itself as a page table, to form
    // a virtual page table at virtual address UVPT.
    // Permissions: kernel R, user R
    kern_pgdir[PDX(UVPT)] = PADDR(kern_pgdir) | PTE_U | PTE_P;

    //LLM Prompt: Allocate npages array, PageInfo
    // Allocate an array of npages 'struct PageInfo' and store it in 'pages'.
    // This array lets the kernel track all physical pages.
    pages = (struct PageInfo *)boot_alloc(npages * sizeof(struct PageInfo));
    memset(pages, 0, npages * sizeof(struct PageInfo));

    //////////////////////////////////////////////////////////////////////
    // Make 'envs' point to an array of size 'NENV' of 'struct Env'.
    // LAB 3: Your code here.
    envs = (struct Env *)boot_alloc(NENV * sizeof(struct Env));
    memset(envs, 0, NENV * sizeof(struct Env));

    // Now set up the free page list (page_init).
    page_init();

    // Check that the free list seems sane.
    check_page_free_list(true);
    check_page_alloc();
    check_page();

    //--------------------------------------------------------------
    // Now we set up the kernel portions of the virtual address space
    //--------------------------------------------------------------

    //
    // Map 'pages' read-only by the user at linear address UPAGES
    //    - the new mapping at UPAGES: kernel R, user R (PTE_U | PTE_P)
    //    - the actual 'pages' array: kernel RW, user NONE
    //
    size_t pages_size = ROUNDUP(npages * sizeof(struct PageInfo), PGSIZE);
    boot_map_region(kern_pgdir, UPAGES, pages_size, PADDR(pages), PTE_U);

    // Also map the same physical pages[] at its own kernel virtual address
    // with PTE_W so the kernel can write to it. (The user sees it at UPAGES.)
    boot_map_region(kern_pgdir, (uintptr_t) pages,
                    pages_size,
                    PADDR(pages),
                    PTE_W);

    //////////////////////////////////////////////////////////////////////
    // Map the 'envs' array read-only by the user at linear address UENVS
    // (ie. perm = PTE_U | PTE_P).
    // Permissions:
    //    - the new image at UENVS  -- kernel R, user R
    //    - envs itself -- kernel RW, user NONE
    // LAB 3: Your code here.
    size_t envs_size = ROUNDUP(NENV * sizeof(struct Env), PGSIZE);
    boot_map_region(kern_pgdir, UENVS, envs_size, PADDR(envs), PTE_U);

    //
    // Map the physical memory that 'bootstack' refers to as the kernel stack.
    // The kernel stack grows down from KSTACKTOP.
    //   [KSTACKTOP-KSTKSIZE, KSTACKTOP) -- backed by physical memory
    //   [KSTACKTOP-PTSIZE,   KSTACKTOP-KSTKSIZE) -- not backed
    // Permissions: kernel RW, user NONE
    //
    boot_map_region(kern_pgdir,
                    KSTACKTOP - KSTKSIZE,
                    KSTKSIZE,
                    PADDR(bootstack),
                    PTE_W);

    //
    // Map all of physical memory at KERNBASE.
    // i.e. the VA range [KERNBASE, 2^32) -> PA range [0, 2^32 - KERNBASE).
    // Permissions: kernel RW, user NONE
    //
    boot_map_region(kern_pgdir,
                    KERNBASE,
                    (0x100000000 - KERNBASE),
                    0,
                    PTE_W);

    // Check that the initial page directory seems correct.
    check_kern_pgdir();

    // Switch from entry_pgdir to the full kern_pgdir we just created.
    lcr3(PADDR(kern_pgdir));

    // Check the free list again (after lcr3)
    check_page_free_list(false);

    // set the rest of the flags in cr0
    cr0 = rcr0();
    cr0 |=  CR0_PE | CR0_PG | CR0_AM | CR0_WP | CR0_NE | CR0_MP;
    cr0 &= ~(CR0_TS | CR0_EM);
    lcr0(cr0);

    // Final checks, only possible now that kern_pgdir is installed.
    check_page_installed_pgdir();

    // Hidden test cases
    //hidden_test_cases();
}


// --------------------------------------------------------------
// Tracking of physical pages.
// The 'pages' array has one 'struct PageInfo' per physical page.
// Pages are reference counted, and free pages are on a linked list.
// --------------------------------------------------------------

//
// Initialize page structure and memory free list.
// After this, NEVER use boot_alloc again. ONLY use page_* functions.
//
void
page_init(void)
{
    //
    //  1) Mark physical page 0 as in use (pp_ref=1). This keeps BIOS/real-mode
    //     data structures from being overwritten.
    //  2) Free pages in [1, npages_basemem).
    //  3) IO hole [IOPHYSMEM, EXTPHYSMEM) must never be allocated.
    //  4) Then extended memory [EXTPHYSMEM, ...]. Part of that region
    //     is occupied by kernel code/data, so exclude that as well.
    //
    size_t i;
    extern char end[];
    uintptr_t final_boot_alloc = (uintptr_t)boot_alloc(0);
    size_t first_free_pgnum = PGNUM(PADDR((void *) final_boot_alloc));

    for (i = 0; i < npages; i++) {
        // Page 0 is in use
        if (i == 0) {
            pages[i].pp_ref = 1;
            pages[i].pp_link = NULL;
            continue;
        }
        // IO hole: [IOPHYSMEM, EXTPHYSMEM)
        if (i >= PGNUM(IOPHYSMEM) && i < PGNUM(EXTPHYSMEM)) {
            pages[i].pp_ref = 1;  
            pages[i].pp_link = NULL;
            continue;
        }
        // Any pages that the kernel is currently using 
        // (from EXTPHYSMEM up to first_free_pgnum) must be in use
        if (i >= PGNUM(EXTPHYSMEM) && i < first_free_pgnum) {
            pages[i].pp_ref = 1;
            pages[i].pp_link = NULL;
            continue;
        }

        // Everything else is free
        pages[i].pp_ref = 0;
        pages[i].pp_link = page_free_list;
        page_free_list = &pages[i];
    }
}

//
// Allocates a physical page. If (alloc_flags & ALLOC_ZERO), fill the entire
// returned physical page with '\0'. Does NOT increment pp_ref.
// Returns NULL if out of free memory.
//
struct PageInfo *
page_alloc(int alloc_flags)
{
    if (!page_free_list) {
        // No free pages
        return NULL;
    }
    struct PageInfo *pp = page_free_list;
    page_free_list = pp->pp_link;
    pp->pp_link = NULL;

    if (alloc_flags & ALLOC_ZERO) {
        memset(page2kva(pp), 0, PGSIZE);
    }
    return pp;
}

//
// Return 'pp' to the free list.
// (Only call this when pp->pp_ref == 0.)
//
void
page_free(struct PageInfo *pp)
{
    if (pp->pp_ref != 0) {
        panic("page_free: pp->pp_ref != 0");
    }
    if (pp->pp_link != NULL) {
        panic("page_free: pp->pp_link != NULL");
    }
    pp->pp_link = page_free_list;
    page_free_list = pp;
}

//
// Decrement the reference count on page 'pp', freeing it if there are no more refs.
//
void
page_decref(struct PageInfo* pp)
{
    if (--pp->pp_ref == 0) {
        page_free(pp);
    }
}

// --------------------------------------------------------------
// Page table management routines
// --------------------------------------------------------------

//
// pgdir_walk returns pointer to the PTE in the page table corresponding to 'va'.
// If the relevant page table does not exist yet and create==false, return NULL.
// If create==true, allocate a new page table. If that fails, return NULL.
// Otherwise, clear the new page table, increment its pp_ref, and return pte.
//
pte_t *
pgdir_walk(pde_t *pgdir, const void *va, int create)
{
    pde_t *pde = &pgdir[PDX(va)];
    if (*pde & PTE_P) {
        // Already present
        pte_t *table = (pte_t *) KADDR(PTE_ADDR(*pde));
        return &table[PTX(va)];
    } else {
        if (!create) {
            return NULL;
        }
        // Allocate new page table
        struct PageInfo *ptpage = page_alloc(ALLOC_ZERO);
        if (!ptpage)
            return NULL;
        ptpage->pp_ref++;
        // PDE
        *pde = page2pa(ptpage) | PTE_P | PTE_W | PTE_U;
        pte_t *table = (pte_t *) page2kva(ptpage);
        return &table[PTX(va)];
    }
}

//
// Map [va, va+size) of virtual address space to [pa, pa+size) of physical space.
// size is multiple of PGSIZE, va and pa are page aligned. Use permission bits
// perm|PTE_P for all entries. This is used only during early boot (boot-time).
//
static void
boot_map_region(pde_t *pgdir, uintptr_t va, size_t size, physaddr_t pa, int perm)
{
    //LLM Prompt:Map continous range of virtual addresses using PTEs
    size_t i;
    for (i = 0; i < size; i += PGSIZE) {
        pte_t *pte = pgdir_walk(pgdir, (void*)(va + i), 1);
        if (!pte) {
            panic("boot_map_region: out of memory for page table");
        }
        *pte = (pa + i) | perm | PTE_P;
    }
}

//
// Map the physical page 'pp' at virtual address 'va' with permissions (perm|PTE_P).
// If a page is already mapped at 'va', page_remove() it first. If needed, allocate
// a new page table. Increment pp->pp_ref if insertion succeeds. TLB must be
// invalidated if an old mapping is removed.
//
// Returns 0 on success, -E_NO_MEM if page table allocation fails.
//
int
page_insert(pde_t *pgdir, struct PageInfo *pp, void *va, int perm)
{
    pte_t *pte = pgdir_walk(pgdir, va, 1);
    if (!pte)
        return -E_NO_MEM;

    // Increase ref count for this page
    pp->pp_ref++;

    if (*pte & PTE_P) {
        // There's already a page; remove it
        page_remove(pgdir, va);
    }
    // Now map it
    *pte = page2pa(pp) | perm | PTE_P;
    return 0;
}

//
// Return the page mapped at virtual address 'va'. If pte_store != 0,
// store the address of the PTE. Return NULL if no page is present.
//
struct PageInfo *
page_lookup(pde_t *pgdir, void *va, pte_t **pte_store)
{
    pte_t *pte = pgdir_walk(pgdir, va, 0);
    if (!pte || !(*pte & PTE_P)) {
        return NULL;
    }
    if (pte_store) {
        *pte_store = pte;
    }
    return pa2page(PTE_ADDR(*pte));
}

//
// Unmap the page at 'va'. If no page is present, do nothing.
// Decrement the ref count, free if it hits 0. Invalidate TLB.
//
void
page_remove(pde_t *pgdir, void *va)
{
    pte_t *pte;
    struct PageInfo *rem = page_lookup(pgdir, va, &pte);
    if (!rem) {
        return;
    }
    *pte = 0;
    tlb_invalidate(pgdir, va);
    page_decref(rem);
}

//
// Invalidate a TLB entry, but only if the page tables being edited
// are the ones currently in use.
//
void
tlb_invalidate(pde_t *pgdir, void *va)
{
    // Since JOS uses only one address space, we always invalidate.
    invlpg(va);
}

static uintptr_t user_mem_check_addr;

//
// Check that an environment is allowed to access the range of memory
// [va, va+len) with permissions 'perm | PTE_P'.
// Normally 'perm' will contain PTE_U at least, but this is not required.
// 'va' and 'len' need not be page-aligned; you must test every page that
// contains any of that range.  You will test either 'len/PGSIZE',
// 'len/PGSIZE + 1', or 'len/PGSIZE + 2' pages.
//
// A user program can access a virtual address if (1) the address is below
// ULIM, and (2) the page table gives it permission.  These are exactly
// the tests you should implement here.
//
// If there is an error, set the 'user_mem_check_addr' variable to the first
// erroneous virtual address.
//
// Returns 0 if the user program can access this range of addresses,
// and -E_FAULT otherwise.
//
int
user_mem_check(struct Env *env, const void *va, size_t len, int perm)
{
	// LAB 3: Your code here.
    uintptr_t start_addr = ROUNDDOWN((uintptr_t)va, PGSIZE);
    uintptr_t end_addr   = (uintptr_t)va + len;
    uintptr_t limit      = ROUNDUP(end_addr, PGSIZE);

    // We always need at least PTE_P in addition to 'perm'
    perm |= PTE_P;

    for (uintptr_t curr = start_addr; curr < limit; curr += PGSIZE) {
        if (curr >= ULIM) {
            // Out of user range
            if (curr < (uintptr_t)va)
                user_mem_check_addr = (uintptr_t)va;
            else
                user_mem_check_addr = curr;
            return -E_FAULT;
        }
        pte_t *my_pte = pgdir_walk(env->env_pgdir, (void*) curr, 0);
        if (!my_pte || ((*my_pte & perm) != perm)) {
            if (curr < (uintptr_t)va)
                user_mem_check_addr = (uintptr_t)va;
            else
                user_mem_check_addr = curr;
            return -E_FAULT;
        }
    }
    return 0;
}

//
// Checks that environment 'env' is allowed to access the range
// of memory [va, va+len) with permissions 'perm | PTE_U | PTE_P'.
// If it can, then the function simply returns.
// If it cannot, 'env' is destroyed and, if env is the current
// environment, this function will not return.
//
void
user_mem_assert(struct Env *env, const void *va, size_t len, int perm)
{
	if (user_mem_check(env, va, len, perm | PTE_U) < 0) {
		cprintf("[%08x] user_mem_check assertion failure for "
			"va %08x\n", env->env_id, user_mem_check_addr);
		env_destroy(env);	// may not return
	}
}


// --------------------------------------------------------------
// Checking functions (do not modify)
// --------------------------------------------------------------

static void
check_page_free_list(bool only_low_memory)
{
    //LLM Prompt: Ensure list is correctly populated
    struct PageInfo *pp;
    unsigned pdx_limit = only_low_memory ? 1 : NPDENTRIES;
    int nfree_basemem = 0, nfree_extmem = 0;
    char *first_free_page;

    if (!page_free_list)
        panic("'page_free_list' is a null pointer!");

    if (only_low_memory) {
        // Move pages with lower addresses first in the free list
        struct PageInfo *pp1, *pp2;
        struct PageInfo **tp[2] = { &pp1, &pp2 };
        for (pp = page_free_list; pp; pp = pp->pp_link) {
            int pagetype = PDX(page2pa(pp)) >= pdx_limit;
            *tp[pagetype] = pp;
            tp[pagetype] = &pp->pp_link;
        }
        *tp[1] = 0;
        *tp[0] = pp2;
        page_free_list = pp1;
    }

    // if there's a page that shouldn't be on the free list,
    // try to make sure it eventually causes trouble.
    for (pp = page_free_list; pp; pp = pp->pp_link)
        if (PDX(page2pa(pp)) < pdx_limit)
            memset(page2kva(pp), 0x97, 128);

    first_free_page = (char *) boot_alloc(0);
    for (pp = page_free_list; pp; pp = pp->pp_link) {
        // check that we didn't corrupt the free list itself
        assert(pp >= pages);
        assert(pp < pages + npages);
        assert(((char *) pp - (char *) pages) % sizeof(*pp) == 0);

        // check a few pages that shouldn't be on the free list
        assert(page2pa(pp) != 0);
        assert(page2pa(pp) != IOPHYSMEM);
        assert(page2pa(pp) != EXTPHYSMEM - PGSIZE);
        assert(page2pa(pp) != EXTPHYSMEM);
        assert(page2pa(pp) < EXTPHYSMEM ||
               (char *) page2kva(pp) >= first_free_page);

        if (page2pa(pp) < EXTPHYSMEM)
            ++nfree_basemem;
        else
            ++nfree_extmem;
    }

    assert(nfree_basemem > 0);
    assert(nfree_extmem > 0);

    cprintf("check_page_free_list() succeeded!\n");
}

static void
check_page_alloc(void)
{
    //LLM Prompt: Test pagealloc for allocation and when out of memory
    struct PageInfo *pp, *pp0, *pp1, *pp2;
    int nfree;
    struct PageInfo *fl;
    char *c;
    int i;

    if (!pages)
        panic("'pages' is a null pointer!");

    // count number of free pages
    for (pp = page_free_list, nfree = 0; pp; pp = pp->pp_link)
        ++nfree;

    // should be able to allocate three pages
    pp0 = pp1 = pp2 = 0;
    assert((pp0 = page_alloc(0)));
    assert((pp1 = page_alloc(0)));
    assert((pp2 = page_alloc(0)));

    assert(pp0);
    assert(pp1 && pp1 != pp0);
    assert(pp2 && pp2 != pp1 && pp2 != pp0);

    // temporarily steal the rest of the free pages
    fl = page_free_list;
    page_free_list = 0;

    // should be no free memory
    assert(!page_alloc(0));

    // free and re-allocate?
    page_free(pp0);
    page_free(pp1);
    page_free(pp2);
    pp0 = pp1 = pp2 = 0;
    assert((pp0 = page_alloc(0)));
    assert((pp1 = page_alloc(0)));
    assert((pp2 = page_alloc(0)));
    assert(pp0);
    assert(pp1 && pp1 != pp0);
    assert(pp2 && pp2 != pp1 && pp2 != pp0);
    assert(!page_alloc(0));

    // test ALLOC_ZERO
    memset(page2kva(pp0), 1, PGSIZE);
    page_free(pp0);
    assert((pp = page_alloc(ALLOC_ZERO)));
    assert(pp && pp0 == pp);
    c = page2kva(pp);
    for (i = 0; i < PGSIZE; i++)
        assert(c[i] == 0);

    // restore free list
    page_free_list = fl;

    // free the pages we took
    page_free(pp0);
    page_free(pp1);
    page_free(pp2);

    // number of free pages should be the same
    for (pp = page_free_list; pp; pp = pp->pp_link)
        --nfree;
    assert(nfree == 0);

    cprintf("check_page_alloc() succeeded!\n");
}

static void
check_kern_pgdir(void)
{
    uint32_t i, n;
    pde_t *pgdir;

    pgdir = kern_pgdir;

    // check pages array
    n = ROUNDUP(npages*sizeof(struct PageInfo), PGSIZE);
    for (i = 0; i < n; i += PGSIZE)
        assert(check_va2pa(pgdir, UPAGES + i) == PADDR(pages) + i);

    // check envs array (new test for lab 3)
    n = ROUNDUP(NENV*sizeof(struct Env), PGSIZE);
    for (i = 0; i < n; i += PGSIZE)
        assert(check_va2pa(pgdir, UENVS + i) == PADDR(envs) + i);

    // check phys mem
    for (i = 0; i < npages * PGSIZE; i += PGSIZE) {
        assert(check_va2pa(pgdir, KERNBASE + i) == i);
    }

    // check kernel stack
    for (i = 0; i < KSTKSIZE; i += PGSIZE) {
        assert(check_va2pa(pgdir, KSTACKTOP - KSTKSIZE + i) ==
               PADDR(bootstack) + i);
    }
    assert(check_va2pa(pgdir, KSTACKTOP - PTSIZE) == ~0);

    // check PDE permissions
    for (i = 0; i < NPDENTRIES; i++) {
        switch (i) {
        case PDX(UVPT):
        case PDX(KSTACKTOP-1):
        case PDX(UPAGES):
        case PDX(UENVS):
            assert(pgdir[i] & PTE_P);
            break;
        default:
            if (i >= PDX(KERNBASE)) {
                assert(pgdir[i] & PTE_P);
                assert(pgdir[i] & PTE_W);
            } else
                assert(pgdir[i] == 0);
            break;
        }
    }
    cprintf("check_kern_pgdir() succeeded!\n");
}

static physaddr_t
check_va2pa(pde_t *pgdir, uintptr_t va)
{
    pte_t *p;

    pgdir = &pgdir[PDX(va)];
    if (!(*pgdir & PTE_P))
        return ~0;
    p = (pte_t*) KADDR(PTE_ADDR(*pgdir));
    if (!(p[PTX(va)] & PTE_P))
        return ~0;
    return PTE_ADDR(p[PTX(va)]);
}

static void
check_page(void)
{
    struct PageInfo *pp, *pp0, *pp1, *pp2;
    struct PageInfo *fl;
    pte_t *ptep, *ptep1;
    void *va;
    int i;
    extern pde_t entry_pgdir[];

    // should be able to allocate three pages
    pp0 = pp1 = pp2 = 0;
    assert((pp0 = page_alloc(0)));
    assert((pp1 = page_alloc(0)));
    assert((pp2 = page_alloc(0)));

    // steal the rest
    fl = page_free_list;
    page_free_list = 0;

    // no free memory now
    assert(!page_alloc(0));

    // no page allocated at address 0
    assert(page_lookup(kern_pgdir, (void *)0x0, &ptep) == NULL);

    // can't allocate page table
    assert(page_insert(kern_pgdir, pp1, 0x0, PTE_W) < 0);

    // free pp0 and try again
    page_free(pp0);
    assert(page_insert(kern_pgdir, pp1, 0x0, PTE_W) == 0);
    assert(PTE_ADDR(kern_pgdir[0]) == page2pa(pp0));
    assert(check_va2pa(kern_pgdir, 0x0) == page2pa(pp1));
    assert(pp1->pp_ref == 1);
    assert(pp0->pp_ref == 1);

    // map pp2 at PGSIZE
    assert(page_insert(kern_pgdir, pp2, (void*)PGSIZE, PTE_W) == 0);
    assert(check_va2pa(kern_pgdir, PGSIZE) == page2pa(pp2));
    assert(pp2->pp_ref == 1);

    // remap pp2 at PGSIZE with different perms
    assert(page_insert(kern_pgdir, pp2, (void*)PGSIZE, PTE_W|PTE_U) == 0);
    assert(check_va2pa(kern_pgdir, PGSIZE) == page2pa(pp2));
    assert(pp2->pp_ref == 1);

    // should be able to remap with fewer permissions
    assert(page_insert(kern_pgdir, pp2, (void*) PGSIZE, PTE_W) == 0);

    // should not be able to map at PTSIZE
    assert(page_insert(kern_pgdir, pp0, (void*)PTSIZE, PTE_W) < 0);

    // insert pp1 at PGSIZE, replacing pp2
    assert(page_insert(kern_pgdir, pp1, (void*) PGSIZE, PTE_W) == 0);
    assert(check_va2pa(kern_pgdir, PGSIZE) == page2pa(pp1));
    assert(pp1->pp_ref == 2);
    assert(pp2->pp_ref == 0);

    // pp2 should be returned by page_alloc
    assert((pp = page_alloc(0)) && pp == pp2);

    // unmap pp1 at 0
    page_remove(kern_pgdir, 0x0);
    assert(check_va2pa(kern_pgdir, 0x0) == ~0);
    assert(check_va2pa(kern_pgdir, PGSIZE) == page2pa(pp1));
    assert(pp1->pp_ref == 1);
    assert(pp2->pp_ref == 0);

    // re-insert pp1 at PGSIZE
    assert(page_insert(kern_pgdir, pp1, (void*) PGSIZE, 0) == 0);
    assert(pp1->pp_ref == 1);

    // unmap pp1 at PGSIZE
    page_remove(kern_pgdir, (void*) PGSIZE);
    assert(check_va2pa(kern_pgdir, 0x0) == ~0);
    assert(check_va2pa(kern_pgdir, PGSIZE) == ~0);
    assert(pp1->pp_ref == 0);

    // so pp1 should be returned by page_alloc
    assert((pp = page_alloc(0)) && pp == pp1);

    // forcibly take pp0 back
    assert(PTE_ADDR(kern_pgdir[0]) == page2pa(pp0));
    kern_pgdir[0] = 0;
    assert(pp0->pp_ref == 1);
    pp0->pp_ref = 0;

    // check pointer arithmetic in pgdir_walk
    page_free(pp0);
    va = (void*)(PGSIZE * NPDENTRIES + PGSIZE);
    ptep = pgdir_walk(kern_pgdir, va, 1);
    ptep1 = (pte_t *)KADDR(PTE_ADDR(kern_pgdir[PDX(va)]));
    assert(ptep == ptep1 + PTX(va));
    kern_pgdir[PDX(va)] = 0;
    pp0->pp_ref = 0;

    // check that new page tables are cleared
    memset(page2kva(pp0), 0xFF, PGSIZE);
    page_free(pp0);
    pgdir_walk(kern_pgdir, 0x0, 1);
    ptep = (pte_t*) page2kva(pp0);
    for (i = 0; i < NPTENTRIES; i++) {
        assert((ptep[i] & PTE_P) == 0);
    }
    kern_pgdir[0] = 0;
    pp0->pp_ref = 0;

    // restore free list
    page_free_list = fl;

    // free the pages we took
    page_free(pp0);
    page_free(pp1);
    page_free(pp2);

    cprintf("check_page() succeeded!\n");
}

static void
check_page_installed_pgdir(void)
{
    struct PageInfo *pp, *pp0, *pp1, *pp2;
    struct PageInfo *fl;
    pte_t *ptep, *ptep1;
    uint32_t va;
    int i;

    // check we can read and write installed pages
    pp1 = pp2 = 0;
    assert((pp0 = page_alloc(0)));
    assert((pp1 = page_alloc(0)));
    assert((pp2 = page_alloc(0)));
    page_free(pp0);
    memset(page2kva(pp1), 1, PGSIZE);
    memset(page2kva(pp2), 2, PGSIZE);
    page_insert(kern_pgdir, pp1, (void*)PGSIZE, PTE_W);
    assert(pp1->pp_ref == 1);
    assert(*(uint32_t*)PGSIZE == 0x01010101U);
    page_insert(kern_pgdir, pp2, (void*)PGSIZE, PTE_W);
    assert(*(uint32_t*)PGSIZE == 0x02020202U);
    assert(pp2->pp_ref == 1);
    assert(pp1->pp_ref == 0);
    *(uint32_t*)PGSIZE = 0x03030303U;
    assert(*(uint32_t*)page2kva(pp2) == 0x03030303U);
    page_remove(kern_pgdir, (void*)PGSIZE);
    assert(pp2->pp_ref == 0);

    // forcibly take pp0 back
    assert(PTE_ADDR(kern_pgdir[0]) == page2pa(pp0));
    kern_pgdir[0] = 0;
    assert(pp0->pp_ref == 1);
    pp0->pp_ref = 0;

    // free the pages we took
    page_free(pp0);

    cprintf("check_page_installed_pgdir() succeeded!\n");
}
