#include <asm/e820/types.h>
#include <asm/processor.h>
#include "pgtable.h"
#include "../string.h"

/*
 * __force_order is used by special_insns.h asm code to force instruction
 * serialization.
 *
 * It is not referenced from the code, but GCC < 5 with -fPIE would fail
 * due to an undefined symbol. Define it to make these ancient GCCs work.
 */
unsigned long __force_order;

#define BIOS_START_MIN		0x20000U	/* 128K, less than this is insane */
#define BIOS_START_MAX		0x9f000U	/* 640K, absolute maximum */

struct paging_config {
	unsigned long trampoline_start;
	unsigned long l5_required;
};

/* Buffer to preserve trampoline memory */
static char trampoline_save[TRAMPOLINE_32BIT_SIZE];

/*
 * Trampoline address will be printed by extract_kernel() for debugging
 * purposes.
 *
 * Avoid putting the pointer into .bss as it will be cleared between
 * paging_prepare() and extract_kernel().
 */
unsigned long *trampoline_32bit __section(.data);

extern struct boot_params *boot_params;
int cmdline_find_option_bool(const char *option);

static unsigned long find_trampoline_placement(void)
{
	unsigned long bios_start, ebda_start;
	unsigned long trampoline_start;
	struct boot_e820_entry *entry;
	int i;

	/*
	 * Find a suitable spot for the trampoline.
	 * This code is based on reserve_bios_regions().
	 */

	ebda_start = *(unsigned short *)0x40e << 4;
	bios_start = *(unsigned short *)0x413 << 10;

	if (bios_start < BIOS_START_MIN || bios_start > BIOS_START_MAX)
		bios_start = BIOS_START_MAX;

	if (ebda_start > BIOS_START_MIN && ebda_start < bios_start)
		bios_start = ebda_start;

	bios_start = round_down(bios_start, PAGE_SIZE);

	/* Find the first usable memory region under bios_start. */
	for (i = boot_params->e820_entries - 1; i >= 0; i--) {
		entry = &boot_params->e820_table[i];

		/* Skip all entries above bios_start. */
        /* 如果要是 <= continue */
		if (bios_start <= entry->addr)
			continue;

		/* Skip non-RAM entries. */
        /* 如果不是RAM */
		if (entry->type != E820_TYPE_RAM)
			continue;

		/* Adjust bios_start to the end of the entry if needed. */
		if (bios_start > entry->addr + entry->size)
			bios_start = entry->addr + entry->size;

		/* Keep bios_start page-aligned. */
        //PAGE_SIZE对齐
		bios_start = round_down(bios_start, PAGE_SIZE);

		/* Skip the entry if it's too small. */
        /* 2 PAGE_SIZE */
        /*  查看这个entry是否可以满足 */
		if (bios_start - TRAMPOLINE_32BIT_SIZE < entry->addr)
			continue;

		break;
	}

	/* Place the trampoline just below the end of low memory */
	return bios_start - TRAMPOLINE_32BIT_SIZE;
}

struct paging_config paging_prepare(void *rmode)
{
	struct paging_config paging_config = {};

	/* Initialize boot_params. Required for cmdline_find_option_bool(). */
	boot_params = rmode;

	/*
	 * Check if LA57 is desired and supported.
	 *
	 * There are several parts to the check:
	 *   - if the kernel supports 5-level paging: CONFIG_X86_5LEVEL=y
	 *   - if user asked to disable 5-level paging: no5lvl in cmdline
	 *   - if the machine supports 5-level paging:
	 *     + CPUID leaf 7 is supported
	 *     + the leaf has the feature bit set
	 *
	 * That's substitute for boot_cpu_has() in early boot code.
	 */
    /*
     * 如果要去看intel芯片手册，建议下载最新的芯片手册去查看cpuid(7), 
     * 旧版的手册(2019)没有X86_FEATURE_LA57 
     */
	if (IS_ENABLED(CONFIG_X86_5LEVEL) &&
			!cmdline_find_option_bool("no5lvl") &&
			native_cpuid_eax(0) >= 7 &&
			(native_cpuid_ecx(7) & (1 << (X86_FEATURE_LA57 & 31)))) {
		paging_config.l5_required = 1;
	}

	paging_config.trampoline_start = find_trampoline_placement();

	trampoline_32bit = (unsigned long *)paging_config.trampoline_start;

	/* Preserve trampoline memory */
    /* 先将原数据备份 */
	memcpy(trampoline_save, trampoline_32bit, TRAMPOLINE_32BIT_SIZE);

	/* Clear trampoline memory first */
    /* 将该buffer cleanup */
	memset(trampoline_32bit, 0, TRAMPOLINE_32BIT_SIZE);

	/* Copy trampoline code in place */
    /* 这是个code32 */
	memcpy(trampoline_32bit + TRAMPOLINE_32BIT_CODE_OFFSET / sizeof(unsigned long),
			&trampoline_32bit_src, TRAMPOLINE_32BIT_CODE_SIZE);

	/*
	 * The code below prepares page table in trampoline memory.
	 *
	 * The new page table will be used by trampoline code for switching
	 * from 4- to 5-level paging or vice versa.
	 *
	 * If switching is not required, the page table is unused: trampoline
	 * code wouldn't touch CR3.
	 */

    /* 
     * X86_CR4_LA57 bit control 5-level page
     *
     * When set in IA-32e mode, the processor uses 5-level paging
     * to translate 57-bit linear addresses.
     */

	/*
	 * We are not going to use the page table in trampoline memory if we
	 * are already in the desired paging mode.
	 */
    /*
     * 1. 如果需要5-level mode , 并且该位已经置上了，说明CPU已经进入了
     * 5-level mode，不需要再使用trampoline memory
     * 2. 如果不需要5-level mode, 恰好CR4该位没有置该位，也不需要再做其他操作
     */
	if (paging_config.l5_required == !!(native_read_cr4() & X86_CR4_LA57))
		goto out;

	if (paging_config.l5_required) {
		/*
		 * For 4- to 5-level paging transition, set up current CR3 as
		 * the first and the only entry in a new top-level page table.
		 */
        /* PML5 */
		trampoline_32bit[TRAMPOLINE_32BIT_PGTABLE_OFFSET] = __native_read_cr3() | _PAGE_TABLE_NOENC;
	} else {
        /* 这种情况就是 已经进入了5-level， 但是kenrel没有配置5-level */
		unsigned long src;

		/*
		 * For 5- to 4-level paging transition, copy page table pointed
		 * by first entry in the current top-level page table as our
		 * new top-level page table.
		 *
		 * We cannot just point to the page table from trampoline as it
		 * may be above 4G.
		 */
        /* 获取PAGE_MASK, PAGE_MASK获取的值，实际上是PML4 table的首地址 */
		src = *(unsigned long *)__native_read_cr3() & PAGE_MASK;
        /* copy whole PML4 table */
		memcpy(trampoline_32bit + TRAMPOLINE_32BIT_PGTABLE_OFFSET / sizeof(unsigned long),
		       (void *)src, PAGE_SIZE);
	}

out:
	return paging_config;
}

void cleanup_trampoline(void *pgtable)
{
	void *trampoline_pgtable;
    /* 找到 top pgtable 的首地址 */
	trampoline_pgtable = trampoline_32bit + TRAMPOLINE_32BIT_PGTABLE_OFFSET / sizeof(unsigned long);

	/*
	 * Move the top level page table out of trampoline memory,
	 * if it's there.
	 */
    /*
     * 如果trampoline_pgtable的地址 == cr3中保存的地址, 
     * 说明cr3已经切换了，那么trampoline_pgtable这个临时的存储
     * 区域其中的内容需要拷贝到pgtable中
     */
	if ((void *)__native_read_cr3() == trampoline_pgtable) {
		memcpy(pgtable, trampoline_pgtable, PAGE_SIZE);
		native_write_cr3((unsigned long)pgtable);
	}

	/* Restore trampoline memory */
    /* 将trampoline_32bit其中的内存恢复 */
	memcpy(trampoline_32bit, trampoline_save, TRAMPOLINE_32BIT_SIZE);
}
