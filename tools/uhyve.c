/* Copyright (c) 2015, IBM
 * Author(s): Dan Williams <djwillia@us.ibm.com>
 *            Ricardo Koller <kollerr@us.ibm.com>
 * Copyright (c) 2017, RWTH Aachen University
 * Author(s): Stefan Lankes <slankes@eonerc.rwth-aachen.de>
 *
 * Permission to use, copy, modify, and/or distribute this software
 * for any purpose with or without fee is hereby granted, provided
 * that the above copyright notice and this permission notice appear
 * in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS
 * OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
 * NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/* We used several existing projects as guides
 * kvmtest.c: http://lwn.net/Articles/658512/
 * lkvm: http://github.com/clearlinux/kvmtool
 */

/*
 * 15.1.2017: extend original version (https://github.com/Solo5/solo5)
 *            for HermitCore
 * 25.2.2017: add SMP support to enable more than one core
 */

#define _GNU_SOURCE

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <limits.h>
#include <assert.h>
#include <pthread.h>
#include <elf.h>
#include <err.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <linux/const.h>
#include <linux/kvm.h>
#include <asm/msr-index.h>
#include <asm/mman.h>

#include "uhyve-cpu.h"
#include "uhyve-syscalls.h"
#include "proxy.h"

#define MAX_FNAME	256
#define MAX_MSR_ENTRIES	50

#define GUEST_OFFSET		0x0
#define CPUID_FUNC_PERFMON	0x0A
#define GUEST_PAGE_SIZE		0x200000   /* 2 MB pages in guest */

#define BOOT_GDT	0x1000
#define BOOT_INFO	0x2000
#define BOOT_PML4	0x10000
#define BOOT_PDPTE	0x11000
#define BOOT_PDE	0x12000

#define BOOT_GDT_NULL	0
#define BOOT_GDT_CODE	1
#define BOOT_GDT_DATA	2
#define BOOT_GDT_MAX	3

#define KVM_32BIT_MAX_MEM_SIZE	(1ULL << 32)
#define KVM_32BIT_GAP_SIZE	(768 << 20)
#define KVM_32BIT_GAP_START	(KVM_32BIT_MAX_MEM_SIZE - KVM_32BIT_GAP_SIZE)

/// Page offset bits
#define PAGE_BITS			12
#define PAGE_2M_BITS	21
#define PAGE_SIZE			(1L << PAGE_BITS)
/// Mask the page address without page map flags and XD flag
#if 0
#define PAGE_MASK		((~0L) << PAGE_BITS)
#define PAGE_2M_MASK		(~0L) << PAGE_2M_BITS)
#else
#define PAGE_MASK			(((~0L) << PAGE_BITS) & ~PG_XD)
#define PAGE_2M_MASK	(((~0L) << PAGE_2M_BITS) & ~PG_XD)
#endif

// Page is present
#define PG_PRESENT		(1 << 0)
// Page is read- and writable
#define PG_RW			(1 << 1)
// Page is addressable from userspace
#define PG_USER			(1 << 2)
// Page write through is activated
#define PG_PWT			(1 << 3)
// Page cache is disabled
#define PG_PCD			(1 << 4)
// Page was recently accessed (set by CPU)
#define PG_ACCESSED		(1 << 5)
// Page is dirty due to recent write-access (set by CPU)
#define PG_DIRTY		(1 << 6)
// Huge page: 4MB (or 2MB, 1GB)
#define PG_PSE			(1 << 7)
// Page attribute table
#define PG_PAT			PG_PSE
#if 1
/* @brief Global TLB entry (Pentium Pro and later)
 *
 * HermitCore is a single-address space operating system
 * => CR3 never changed => The flag isn't required for HermitCore
 */
#define PG_GLOBAL		0
#else
#define PG_GLOBAL		(1 << 8)
#endif
// This table is a self-reference and should skipped by page_map_copy()
#define PG_SELF			(1 << 9)

/// Disable execution for this page
#define PG_XD			(1L << 63)

#define BITS					64
#define PHYS_BITS			52
#define VIRT_BITS			48
#define PAGE_MAP_BITS	9
#define PAGE_LEVELS		4

#define kvm_ioctl(fd, cmd, arg) ({ \
	const int ret = ioctl(fd, cmd, arg); \
	if(ret == -1) \
		err(1, "KVM: ioctl " #cmd " failed"); \
	ret; \
	})

static uint32_t restart = 0;
static uint32_t ncores = 1;
static uint8_t* guest_mem = NULL;
static uint8_t* klog = NULL;
static uint8_t* mboot = NULL;
static size_t guest_size = 0x20000000ULL;
static uint64_t elf_entry;
static pthread_t* vcpu_threads = NULL;
static int kvm = -1, vmfd = -1;
static uint32_t no_checkpoint = 0;
static pthread_barrier_t barrier;
static __thread struct kvm_run *run = NULL;
static __thread int vcpufd = 1;
static __thread uint32_t cpuid = 0;

static uint64_t memparse(const char *ptr)
{
	// local pointer to end of parsed string
	char *endptr;

	// parse number
	uint64_t size = strtoull(ptr, &endptr, 0);

	// parse size extension, intentional fall-through
	switch (*endptr) {
	case 'E':
	case 'e':
		size <<= 10;
	case 'P':
	case 'p':
		size <<= 10;
	case 'T':
	case 't':
		size <<= 10;
	case 'G':
	case 'g':
		size <<= 10;
	case 'M':
	case 'm':
		size <<= 10;
	case 'K':
	case 'k':
		size <<= 10;
		endptr++;
	default:
		break;
	}

	return size;
}

/// Just close file descriptor if not already done
static inline void close_fd(int* fd)
{
	if(*fd != -1) {
		close(*fd);
		*fd = -1;
	}
}

static void sig_func(int sig)
{
	(void) sig;

	close_fd(&vcpufd);
	pthread_exit(NULL);
}

static void uhyve_exit(void)
{
	// only the main thread will execute this
	if (vcpu_threads) {
		for(uint32_t i = 1; i < ncores; i++) {
			pthread_kill(vcpu_threads[i], SIGTERM);
			pthread_join(vcpu_threads[i], NULL);
		}

		free(vcpu_threads);
	}

	char* verbose = getenv("HERMIT_VERBOSE");
	if (klog && verbose && (strcmp(verbose, "0") != 0))
	{
		puts("\nDump kernel log:");
		puts("================\n");
		printf("%s\n", klog);
	}

	// clean up and close KVM
	close_fd(&vcpufd);
	close_fd(&vmfd);
	close_fd(&kvm);
}

static uint32_t get_cpufreq(void)
{
	char line[128];
	uint32_t freq = 0;
	char* match;

	FILE* fp = fopen("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq", "r");
	if (fp != NULL) {
		if (fgets(line, sizeof(line), fp) != NULL) {
			// cpuinfo_max_freq is in kHz
			freq = (uint32_t) atoi(line) / 1000;
		}

		fclose(fp);
	} else if( (fp = fopen("/proc/cpuinfo", "r")) ) {
		// Resorting to /proc/cpuinfo, however on most systems this will only
		// return the current frequency that might change over time.
		// Currently only needed when running inside a VM

		// read until we find the line indicating cpu frequency
		while(fgets(line, sizeof(line), fp) != NULL) {
			match = strstr(line, "cpu MHz");

			if(match != NULL) {
				// advance pointer to beginning of number
				while( ((*match < '0') || (*match > '9')) && (*match != '\0') )
					match++;

				freq = (uint32_t) atoi(match);
				break;
			}
		}

		fclose(fp);
	}

	return freq;
}

static ssize_t pread_in_full(int fd, void *buf, size_t count, off_t offset)
{
	ssize_t total = 0;
	char *p = buf;

	if (count > SSIZE_MAX) {
		errno = E2BIG;
		return -1;
	}

	while (count > 0) {
		ssize_t nr;

		nr = pread(fd, p, count, offset);
		if (nr == 0)
			return total;
		else if (nr == -1 && errno == EINTR)
			continue;
		else if (nr == -1)
			return -1;

		count -= nr;
		total += nr;
		p += nr;
		offset += nr;
	}

	return total;
}

static int load_checkpoint(uint8_t* mem)
{
	char fname[MAX_FNAME];
	size_t location;
	size_t paddr = elf_entry;
	int ret;

	if (!klog)
		klog = mem+paddr+0x5000-GUEST_OFFSET;
	if (!mboot)
		mboot = mem+paddr-GUEST_OFFSET;

	for(uint32_t i=0; i<=no_checkpoint; i++)
	{
		snprintf(fname, MAX_FNAME, "checkpoint/chk%u_mem.dat", i);

		FILE* f = fopen(fname, "r");
		if (f == NULL)
			return -1;

		struct kvm_irqchip irqchip;
		if (fread(&irqchip, sizeof(irqchip), 1, f) != 1)
			err(1, "fread failed");
		kvm_ioctl(vmfd, KVM_SET_IRQCHIP, &irqchip);

		while (fread(&location, sizeof(location), 1, f) == 1) {
			if (location & PG_PSE)
				ret = fread((size_t*) (mem + (location & PAGE_2M_MASK)), (1UL << PAGE_2M_BITS), 1, f);
			else
				ret = fread((size_t*) (mem + (location & PAGE_MASK)), (1UL << PAGE_BITS), 1, f);

			if (ret != 1)
				err(1, "fread failed");
		}

		fclose(f);
	}

	return 0;
}

static int load_kernel(uint8_t* mem, char* path)
{
	Elf64_Ehdr hdr;
	Elf64_Phdr *phdr = NULL;
	size_t buflen;
	int fd, ret;
	int first_load = 1;

	fd = open(path, O_RDONLY);
	if (fd == -1)
	{
		perror("Unable to open file");
		return -1;
	}

	ret = pread_in_full(fd, &hdr, sizeof(hdr), 0);
	if (ret < 0)
		goto out;

	//  check if the program is a HermitCore file
	if (hdr.e_ident[EI_MAG0] != ELFMAG0
	    || hdr.e_ident[EI_MAG1] != ELFMAG1
	    || hdr.e_ident[EI_MAG2] != ELFMAG2
	    || hdr.e_ident[EI_MAG3] != ELFMAG3
	    || hdr.e_ident[EI_CLASS] != ELFCLASS64
	    || hdr.e_ident[EI_OSABI] != HERMIT_ELFOSABI
	    || hdr.e_type != ET_EXEC || hdr.e_machine != EM_X86_64) {
		fprintf(stderr, "Inavlide HermitCore file!\n");
		goto out;
	}

	elf_entry = hdr.e_entry;

	buflen = hdr.e_phentsize * hdr.e_phnum;
	phdr = malloc(buflen);
	if (!phdr) {
		fprintf(stderr, "Not enough memory\n");
		goto out;
	}

	ret = pread_in_full(fd, phdr, buflen, hdr.e_phoff);
	if (ret < 0)
		goto out;

	/*
	 * Load all segments with type "LOAD" from the file at offset
	 * p_offset, and copy that into in memory.
	 */
	for (Elf64_Half ph_i = 0; ph_i < hdr.e_phnum; ph_i++)
	{
		uint64_t paddr = phdr[ph_i].p_paddr;
		size_t offset = phdr[ph_i].p_offset;
		size_t filesz = phdr[ph_i].p_filesz;
		size_t memsz = phdr[ph_i].p_memsz;

		if (phdr[ph_i].p_type != PT_LOAD)
			continue;

		//printf("Kernel location 0x%zx, file size 0x%zx, memory size 0x%zx\n", paddr, filesz, memsz);

		ret = pread_in_full(fd, mem+paddr-GUEST_OFFSET, filesz, offset);
		if (ret < 0)
			goto out;
		if (!klog)
			klog = mem+paddr+0x5000-GUEST_OFFSET;
		if (!mboot)
			mboot = mem+paddr-GUEST_OFFSET;

		if (first_load) {
			first_load = 0;

			// initialize kernel
			*((uint64_t*) (mem+paddr-GUEST_OFFSET + 0x08)) = paddr; // physical start address
			*((uint64_t*) (mem+paddr-GUEST_OFFSET + 0x10)) = guest_size;   // physical limit
			*((uint32_t*) (mem+paddr-GUEST_OFFSET + 0x18)) = get_cpufreq();
			*((uint32_t*) (mem+paddr-GUEST_OFFSET + 0x24)) = 1; // number of used cpus
			*((uint32_t*) (mem+paddr-GUEST_OFFSET + 0x30)) = 0; // apicid
			*((uint32_t*) (mem+paddr-GUEST_OFFSET + 0x60)) = 1; // numa nodes
			*((uint32_t*) (mem+paddr-GUEST_OFFSET + 0x94)) = 1; // announce uhyve
		}
		*((uint64_t*) (mem+paddr-GUEST_OFFSET + 0x38)) += memsz; // total kernel size
	}

out:
	if (phdr)
		free(phdr);

	close(fd);

	return 0;
}

/// Filter CPUID functions that are not supported by the hypervisor and enable
/// features according to our needs.
static void filter_cpuid(struct kvm_cpuid2 *kvm_cpuid)
{
	for (uint32_t i = 0; i < kvm_cpuid->nent; i++) {
		struct kvm_cpuid_entry2 *entry = &kvm_cpuid->entries[i];

		switch (entry->function) {
		case 1:
			// CPUID to define basic cpu features
			entry->ecx |= (1U << 31); // propagate that we are running on a hypervisor
			entry->edx |= (1U <<  5); // enable msr support
			break;

		case CPUID_FUNC_PERFMON:
			// disable it
			entry->eax	= 0x00;
			break;

		default:
			// Keep the CPUID function as-is
			break;
		};
	}
}

static void setup_system_64bit(struct kvm_sregs *sregs)
{
	sregs->cr0 |= X86_CR0_PE;
	sregs->efer |= EFER_LME;
}

static void setup_system_page_tables(struct kvm_sregs *sregs, uint8_t *mem)
{
	uint64_t *pml4 = (uint64_t *) (mem + BOOT_PML4);
	uint64_t *pdpte = (uint64_t *) (mem + BOOT_PDPTE);
	uint64_t *pde = (uint64_t *) (mem + BOOT_PDE);
	uint64_t paddr;

	/*
	 * For simplicity we currently use 2MB pages and only a single
	 * PML4/PDPTE/PDE.  Sanity check that the guest size is a multiple of the
	 * page size and will fit in a single PDE (512 entries).
	 */
	assert((guest_size & (GUEST_PAGE_SIZE - 1)) == 0);
	assert(guest_size <= (GUEST_PAGE_SIZE * 512));

	memset(pml4, 0x00, 4096);
	memset(pdpte, 0x00, 4096);
	memset(pde, 0x00, 4096);

	*pml4 = BOOT_PDPTE | (X86_PDPT_P | X86_PDPT_RW);
	*pdpte = BOOT_PDE | (X86_PDPT_P | X86_PDPT_RW);
	for (paddr = 0; paddr < guest_size; paddr += GUEST_PAGE_SIZE, pde++)
		*pde = paddr | (X86_PDPT_P | X86_PDPT_RW | X86_PDPT_PS);

	sregs->cr3 = BOOT_PML4;
	sregs->cr4 |= X86_CR4_PAE;
	sregs->cr0 |= X86_CR0_PG;
}

static void setup_system_gdt(struct kvm_sregs *sregs,
                             uint8_t *mem,
                             uint64_t off)
{
	uint64_t *gdt = (uint64_t *) (mem + off);
	struct kvm_segment data_seg, code_seg;

	/* flags, base, limit */
	gdt[BOOT_GDT_NULL] = GDT_ENTRY(0, 0, 0);
	gdt[BOOT_GDT_CODE] = GDT_ENTRY(0xA09B, 0, 0xFFFFF);
	gdt[BOOT_GDT_DATA] = GDT_ENTRY(0xC093, 0, 0xFFFFF);

	sregs->gdt.base = off;
	sregs->gdt.limit = (sizeof(uint64_t) * BOOT_GDT_MAX) - 1;

	GDT_TO_KVM_SEGMENT(code_seg, gdt, BOOT_GDT_CODE);
	GDT_TO_KVM_SEGMENT(data_seg, gdt, BOOT_GDT_DATA);

	sregs->cs = code_seg;
	sregs->ds = data_seg;
	sregs->es = data_seg;
	sregs->fs = data_seg;
	sregs->gs = data_seg;
	sregs->ss = data_seg;
}

static void setup_system(int vcpufd, uint8_t *mem, uint32_t id)
{
	static struct kvm_sregs sregs;

	// all cores use the same startup code
	// => all cores use the same sregs
	// => only the boot processor has to initialize sregs
	if (id == 0) {
		kvm_ioctl(vcpufd, KVM_GET_SREGS, &sregs);

		/* Set all cpu/mem system structures */
		setup_system_gdt(&sregs, mem, BOOT_GDT);
		setup_system_page_tables(&sregs, mem);
		setup_system_64bit(&sregs);
	}

	kvm_ioctl(vcpufd, KVM_SET_SREGS, &sregs);
}

static void setup_cpuid(int kvm, int vcpufd)
{
	struct kvm_cpuid2 *kvm_cpuid;
	unsigned int max_entries = 100;

	// allocate space for cpuid we get from KVM
	kvm_cpuid = calloc(1, sizeof(*kvm_cpuid) + (max_entries * sizeof(kvm_cpuid->entries[0])));
	kvm_cpuid->nent = max_entries;

	kvm_ioctl(kvm, KVM_GET_SUPPORTED_CPUID, kvm_cpuid);

	// set features
	filter_cpuid(kvm_cpuid);
	kvm_ioctl(vcpufd, KVM_SET_CPUID2, kvm_cpuid);

	free(kvm_cpuid);
}

static int vcpu_loop(void)
{
	int ret;

	while (1) {
		ret = ioctl(vcpufd, KVM_RUN, NULL);

		if(ret == -1) {
			switch(errno) {
			case EINTR:
				continue;

			case EFAULT: {
				struct kvm_regs regs;
				kvm_ioctl(vcpufd, KVM_GET_REGS, &regs);
				err(1, "KVM: host/guest translation fault: rip=0x%llx", regs.rip);
			}

			default:
				err(1, "KVM: ioctl KVM_RUN in vcpu_loop failed");
				break;
			}
		}

		/* handle requests */
		switch (run->exit_reason) {
		case KVM_EXIT_HLT:
			fprintf(stderr, "Guest has halted the CPU, this is considered as a normal exit.\n");
			return 0;

		case KVM_EXIT_MMIO:
			err(1, "KVM: unhandled KVM_EXIT_MMIO at 0x%llx\n", run->mmio.phys_addr);
			break;

		case KVM_EXIT_IO:
			//printf("port 0x%x\n", run->io.port);
			switch (run->io.port) {
			case UHYVE_PORT_WRITE: {
					unsigned data = *((unsigned*)((size_t)run+run->io.data_offset));
					uhyve_write_t* uhyve_write = (uhyve_write_t*) (guest_mem+data);

					uhyve_write->len = write(uhyve_write->fd, guest_mem+(size_t)uhyve_write->buf, uhyve_write->len);
					break;
				}

			case UHYVE_PORT_READ: {
					unsigned data = *((unsigned*)((size_t)run+run->io.data_offset));
					uhyve_read_t* uhyve_read = (uhyve_read_t*) (guest_mem+data);

					uhyve_read->ret = read(uhyve_read->fd, guest_mem+(size_t)uhyve_read->buf, uhyve_read->len);
					break;
				}

			case UHYVE_PORT_EXIT: {
					unsigned data = *((unsigned*)((size_t)run+run->io.data_offset));

					//printf("%s\n", klog);
					exit(*(int*)(guest_mem+data));
					break;
				}

			case UHYVE_PORT_OPEN: {
					unsigned data = *((unsigned*)((size_t)run+run->io.data_offset));
					uhyve_open_t* uhyve_open = (uhyve_open_t*) (guest_mem+data);

					uhyve_open->ret = open((const char*)guest_mem+(size_t)uhyve_open->name, uhyve_open->flags, uhyve_open->mode);
					break;
				}

			case UHYVE_PORT_CLOSE: {
					unsigned data = *((unsigned*)((size_t)run+run->io.data_offset));
					uhyve_close_t* uhyve_close = (uhyve_close_t*) (guest_mem+data);

					if (uhyve_close->ret > 2)
						uhyve_close->ret = close(uhyve_close->fd);
					break;
				}

			case UHYVE_PORT_LSEEK: {
					unsigned data = *((unsigned*)((size_t)run+run->io.data_offset));
					uhyve_lseek_t* uhyve_lseek = (uhyve_lseek_t*) (guest_mem+data);

					uhyve_lseek->offset = lseek(uhyve_lseek->fd, uhyve_lseek->offset, uhyve_lseek->whence);
					break;
				}
			default:
				err(1, "KVM: unhandled KVM_EXIT_IO at port 0x%x, direction %d\n", run->io.port, run->io.direction);
				break;
			}
			break;

		case KVM_EXIT_FAIL_ENTRY:
			err(1, "KVM: entry failure: hw_entry_failure_reason=0x%llx\n",
				run->fail_entry.hardware_entry_failure_reason);
			break;

		case KVM_EXIT_INTERNAL_ERROR:
			err(1, "KVM: internal error exit: suberror = 0x%x\n", run->internal.suberror);
			break;

		case KVM_EXIT_SHUTDOWN:
			err(1, "KVM: receive shutdown command\n");
			break;

		default:
			fprintf(stderr, "KVM: unhandled exit: exit_reason = 0x%x\n", run->exit_reason);
			exit(EXIT_FAILURE);
		}
	}

	close(vcpufd);
	vcpufd = -1;

	return 0;
}

static int vcpu_init(void)
{
	struct kvm_mp_state state = { KVM_MP_STATE_RUNNABLE };
	struct kvm_regs regs = {
		.rip = elf_entry,	// entry point to HermitCore
		.rflags = 0x2,		// POR value required by x86 architecture
	};

	vcpufd = kvm_ioctl(vmfd, KVM_CREATE_VCPU, cpuid);

	/* Map the shared kvm_run structure and following data. */
	size_t mmap_size = (size_t) kvm_ioctl(kvm, KVM_GET_VCPU_MMAP_SIZE, NULL);

	if (mmap_size < sizeof(*run))
		err(1, "KVM: invalid VCPU_MMAP_SIZE: %zd", mmap_size);

	run = mmap(NULL, mmap_size, PROT_READ | PROT_WRITE, MAP_SHARED, vcpufd, 0);
	if (run == MAP_FAILED)
		err(1, "KVM: VCPU mmap failed");

	setup_cpuid(kvm, vcpufd);

	// be sure that the multiprocessor is runable
	kvm_ioctl(vcpufd, KVM_SET_MP_STATE, &state);

	if (restart) {
		char fname[MAX_FNAME];
		struct kvm_sregs sregs;
		struct kvm_fpu fpu;
		struct {
			struct kvm_msrs info;
			struct kvm_msr_entry entries[MAX_MSR_ENTRIES];
		} msr_data;
		struct kvm_lapic_state lapic;
		struct kvm_xsave xsave;
		struct kvm_xcrs xcrs;
		struct kvm_vcpu_events events;

		snprintf(fname, MAX_FNAME, "checkpoint/chk%u_core%u.dat", no_checkpoint, cpuid);

		FILE* f = fopen(fname, "r");
		if (f == NULL)
			err(1, "fopen: unable to open file");

		if (fread(&sregs, sizeof(sregs), 1, f) != 1)
			err(1, "fread failed");
		if (fread(&regs, sizeof(regs), 1, f) != 1)
			err(1, "fread failed");
		if (fread(&fpu, sizeof(fpu), 1, f) != 1)
			err(1, "fread failed");
		if (fread(&msr_data, sizeof(msr_data), 1, f) != 1)
			err(1, "fread failed");
		if (fread(&lapic, sizeof(lapic), 1, f) != 1)
			err(1, "fread failed");
		if (fread(&xsave, sizeof(xsave), 1, f) != 1)
			err(1, "fread failed");
		if (fread(&xcrs, sizeof(xcrs), 1, f) != 1)
			err(1, "fread failed");
		if (fread(&events, sizeof(events), 1, f) != 1)
			err(1, "fread failed");

		fclose(f);

		kvm_ioctl(vcpufd, KVM_SET_SREGS, &sregs);
		kvm_ioctl(vcpufd, KVM_SET_REGS, &regs);
		kvm_ioctl(vcpufd, KVM_SET_MSRS, &msr_data);
		kvm_ioctl(vcpufd, KVM_SET_XCRS, &xcrs);
		kvm_ioctl(vcpufd, KVM_SET_LAPIC, &lapic);
		kvm_ioctl(vcpufd, KVM_SET_FPU, &fpu);
		kvm_ioctl(vcpufd, KVM_SET_XSAVE, &xsave);
		kvm_ioctl(vcpufd, KVM_SET_VCPU_EVENTS, &events);

		if (cpuid > 0)
			pthread_barrier_wait(&barrier);
	} else {
		/* Setup registers and memory. */
		setup_system(vcpufd, guest_mem, cpuid);
		kvm_ioctl(vcpufd, KVM_SET_REGS, &regs);

		// only one core is able to enter startup code
		// => the wait for the predecessor core
		while (*((volatile uint32_t*) (mboot + 0x20)) < cpuid)
			pthread_yield();
		*((volatile uint32_t*) (mboot + 0x30)) = cpuid;
	}

	return 0;
}

static void save_cpu_state(void)
{
	struct {
		struct kvm_msrs info;
		struct kvm_msr_entry entries[MAX_MSR_ENTRIES];
	} msr_data;
	struct kvm_msr_entry *msrs = msr_data.entries;
	struct kvm_regs regs;
	struct kvm_sregs sregs;
	struct kvm_fpu fpu;
	struct kvm_lapic_state lapic;
	struct kvm_xsave xsave;
	struct kvm_xcrs xcrs;
	struct kvm_vcpu_events events;
	char fname[MAX_FNAME];
	int n = 0;

	/* define the list of required MSRs */
	msrs[n++].index = MSR_IA32_APICBASE;
	msrs[n++].index = MSR_IA32_SYSENTER_CS;
	msrs[n++].index = MSR_IA32_SYSENTER_ESP;
	msrs[n++].index = MSR_IA32_SYSENTER_EIP;
	msrs[n++].index = MSR_IA32_CR_PAT;
	msrs[n++].index = MSR_IA32_PLATFORM_ID;
	msrs[n++].index = MSR_IA32_MISC_ENABLE;
	msrs[n++].index = MSR_CSTAR;
	msrs[n++].index = MSR_STAR;
	msrs[n++].index = MSR_EFER;
	msrs[n++].index = MSR_LSTAR;
	msrs[n++].index = MSR_GS_BASE;
	msrs[n++].index = MSR_FS_BASE;
	msrs[n++].index = MSR_KERNEL_GS_BASE;

	kvm_ioctl(vcpufd, KVM_GET_SREGS, &sregs);
	kvm_ioctl(vcpufd, KVM_GET_REGS, &regs);
	msr_data.info.nmsrs = n;
	kvm_ioctl(vcpufd, KVM_GET_MSRS, &msr_data);
	kvm_ioctl(vcpufd, KVM_GET_XCRS, &xcrs);
	kvm_ioctl(vcpufd, KVM_GET_LAPIC, &lapic);
	kvm_ioctl(vcpufd, KVM_GET_FPU, &fpu);
	kvm_ioctl(vcpufd, KVM_GET_XSAVE, &xsave);
	kvm_ioctl(vcpufd, KVM_GET_VCPU_EVENTS, &events);

	snprintf(fname, MAX_FNAME, "checkpoint/chk%u_core%u.dat", no_checkpoint, cpuid);

	FILE* f = fopen(fname, "w");
	if (f == NULL) {
		err(1, "fopen: unable to open file");
	}

	if (fwrite(&sregs, sizeof(sregs), 1, f) != 1)
		err(1, "fwrite failed");
	if (fwrite(&regs, sizeof(regs), 1, f) != 1)
		err(1, "fwrite failed");
	if (fwrite(&fpu, sizeof(fpu), 1, f) != 1)
		err(1, "fwrite failed");
	if (fwrite(&msr_data, sizeof(msr_data), 1, f) != 1)
		err(1, "fwrite failed");
	if (fwrite(&lapic, sizeof(lapic), 1, f) != 1)
		err(1, "fwrite failed");
	if (fwrite(&xsave, sizeof(xsave), 1, f) != 1)
		err(1, "fwrite failed");
	if (fwrite(&xcrs, sizeof(xcrs), 1, f) != 1)
		err(1, "fwrite failed");
	if (fwrite(&events, sizeof(events), 1, f) != 1)
		err(1, "fwrite failed");

	fclose(f);
}

static void sigusr_handler(int signum)
{
	pthread_barrier_wait(&barrier);

	save_cpu_state();

	pthread_barrier_wait(&barrier);
}

static void* uhyve_thread(void* arg)
{
	struct sigaction sa;

	cpuid = (size_t) arg;

	/* Install timer_handler as the signal handler for SIGVTALRM. */
	memset(&sa, 0x00, sizeof(sa));
	sa.sa_handler = &sigusr_handler;
	sigaction(SIGUSR1, &sa, NULL);

	// create new cpu
	vcpu_init();

	// run cpu loop until thread gets killed
	const size_t ret = vcpu_loop();

	return (void*) ret;
}

int uhyve_init(char *path)
{
	// register signal handler before going multithread
	signal(SIGTERM, sig_func);

	// register routine to close the VM
	atexit(uhyve_exit);

	FILE* f = fopen("checkpoint/chk_config.txt", "r");
	if (f != NULL) {
		restart = 1;

		fscanf(f, "number of cores: %u\n", &ncores);
		fscanf(f, "memory size: 0x%zx\n", &guest_size);
		fscanf(f, "checkpoint number: %u\n", &no_checkpoint);
		fscanf(f, "entry point: 0x%zx", &elf_entry);

		printf("Restart from checkpoint %u (ncores, %d, mem size 0x%zx)\n", no_checkpoint, ncores, guest_size);

		fclose(f);
	} else {
		const char* hermit_memory = getenv("HERMIT_MEM");
		if (hermit_memory)
			guest_size = memparse(hermit_memory);
	}

	kvm = open("/dev/kvm", O_RDWR | O_CLOEXEC);
	if (kvm < 0)
		err(1, "Could not open: /dev/kvm");

	/* Make sure we have the stable version of the API */
	int kvm_api_version = kvm_ioctl(kvm, KVM_GET_API_VERSION, NULL);
	if (kvm_api_version != 12)
		err(1, "KVM: API version is %d, uhyve requires version 12", kvm_api_version);

	/* Create the virtual machine */
	vmfd = kvm_ioctl(kvm, KVM_CREATE_VM, 0);

	// TODO: we have to create a gap  for PCI
	assert(guest_size < KVM_32BIT_GAP_SIZE);

	/* Allocate page-aligned guest memory. */
	guest_mem = mmap(NULL, guest_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS /*| MAP_HUGETLB | MAP_HUGE_2MB*/, -1, 0);
	if (guest_mem == MAP_FAILED)
		err(1, "mmap failed");

	/* Map it to the second page frame (to avoid the real-mode IDT at 0). */
	struct kvm_userspace_memory_region kvm_region = {
		.slot = 0,
		.guest_phys_addr = GUEST_OFFSET,
		.memory_size = guest_size,
		.userspace_addr = (uint64_t) guest_mem,
	};

	kvm_ioctl(vmfd, KVM_SET_USER_MEMORY_REGION, &kvm_region);
	kvm_ioctl(vmfd, KVM_CREATE_IRQCHIP, NULL);

	if (restart) {
		if (load_checkpoint(guest_mem) != 0)
			exit(EXIT_FAILURE);
	} else {
		if (load_kernel(guest_mem, path) != 0)
			exit(EXIT_FAILURE);
	}

	pthread_barrier_init(&barrier, NULL, ncores);
	cpuid = 0;

	// create first CPU, it will be the boot processor by default
	return vcpu_init();
}

static void timer_handler(int signum)
{
	struct stat st = {0};
	const size_t flag = no_checkpoint > 0 ? PG_DIRTY : PG_ACCESSED;
	char fname[MAX_FNAME];
	struct timeval begin, end;

	gettimeofday(&begin, NULL);

	if (stat("checkpoint", &st) == -1)
		mkdir("checkpoint", 0700);

	for(size_t i = 0; i < ncores; i++)
		if (vcpu_threads[i] != pthread_self())
			pthread_kill(vcpu_threads[i], SIGUSR1);

	pthread_barrier_wait(&barrier);

	save_cpu_state();

	snprintf(fname, MAX_FNAME, "checkpoint/chk%u_mem.dat", no_checkpoint);

	FILE* f = fopen(fname, "w");
	if (f == NULL) {
		err(1, "fopen: unable to open file");
	}

	struct kvm_irqchip irqchip;
	kvm_ioctl(vmfd, KVM_GET_IRQCHIP, &irqchip);
	if (fwrite(&irqchip, sizeof(irqchip), 1, f) != 1)
		err(1, "fwrite failed");

	size_t* pml4 = (size_t*) (guest_mem+elf_entry+PAGE_SIZE);
	for(size_t i=0; i<(1 << PAGE_MAP_BITS); i++) {
		if (!(pml4[i] & PG_PRESENT))
			continue;
		//printf("pml[%zd] 0x%zx\n", i, pml4[i]);
		size_t* pdpt = (size_t*) (guest_mem+(pml4[i] & PAGE_MASK));
		for(size_t j=0; j<(1 << PAGE_MAP_BITS); j++) {
			if (!(pdpt[j] & PG_PRESENT))
				continue;
			//printf("\tpdpt[%zd] 0x%zx\n", j, pdpt[j]);
			size_t* pgd = (size_t*) (guest_mem+(pdpt[i] & PAGE_MASK));
			for(size_t k=0; k<(1 << PAGE_MAP_BITS); k++) {
				if (!(pgd[k] & PG_PRESENT))
					continue;
				if (!(pgd[k] & PG_PSE)) {
					size_t* pgt = (size_t*) (guest_mem+(pgd[k] & PAGE_MASK));
					for(size_t l=0; l<(1 << PAGE_MAP_BITS); l++) {
						if ((pgt[l] & (PG_PRESENT|flag)) == (PG_PRESENT|flag)) {
							//printf("\t\t\t*pgt[%zd] 0x%zx\n", l, pgt[l] & ~PG_XD);
							//pgt[l] = pgt[l] & ~(PG_DIRTY|PG_ACCESSED);
							if (fwrite(pgt+l, sizeof(size_t), 1, f) != 1)
								err(1, "fwrite failed");
							if (fwrite((size_t*) (guest_mem + (pgt[l] & PAGE_MASK)), PAGE_SIZE, 1, f) != 1)
								err(1, "fwrite failed");
						}
					}
				} else if (pgd[k] & flag) {
					//printf("\t\t*pgd[%zd] 0x%zx\n", k, pgd[k] & ~PG_XD);
					//pgd[k] = pgd[k] & ~(PG_DIRTY|PG_ACCESSED);
					if (fwrite(pgd+k, sizeof(size_t), 1, f) != 1)
						err(1, "fwrite failed");
					if (fwrite((size_t*) (guest_mem + (pgd[k] & PAGE_2M_MASK)), (1UL << PAGE_2M_BITS), 1, f) != 1)
						err(1, "fwrite failed");
				}
			}
		}
	}

	fclose(f);

	pthread_barrier_wait(&barrier);

	// update configuration file
	f = fopen("checkpoint/chk_config.txt", "w");
	if (f == NULL) {
		err(1, "fopen: unable to open file");
	}

	fprintf(f, "number of cores: %u\n", ncores);
	fprintf(f, "memory size: 0x%zx\n", guest_size);
	fprintf(f, "checkpoint number: %u\n", no_checkpoint);
	fprintf(f, "entry point: 0x%zx", elf_entry);

	fclose(f);

	gettimeofday(&end, NULL);
	size_t msec = (end.tv_sec - begin.tv_sec) * 1000;
	msec += (end.tv_usec - begin.tv_usec) / 1000;
	printf("Create checkpoint %u in %zd ms\n", no_checkpoint, msec);

	no_checkpoint++;
}

int uhyve_loop(void)
{
	const char* hermit_cpus = getenv("HERMIT_CPUS");
	const char* hermit_check = getenv("HERMIT_CHECKPOINT");
	int ts = 0;

	if (!restart && hermit_cpus)
		ncores = (uint32_t) atoi(hermit_cpus);
	if (hermit_check)
		ts = atoi(hermit_check);

	*((uint32_t*) (mboot+0x24)) = ncores;

	vcpu_threads = (pthread_t*) calloc(ncores, sizeof(pthread_t));
	if (!vcpu_threads)
		err(1, "Not enough memory");

	// First CPU is special because it will boot the system. Other CPUs will
	// be booted linearily after the first one.
	vcpu_threads[0] = pthread_self();

	// start threads to create VCPUs
	for(size_t i = 1; i < ncores; i++)
		pthread_create(&vcpu_threads[i], NULL, uhyve_thread, (void*) i);

	if (ts > 0)
	{
		struct sigaction sa;
		struct itimerval timer;

		/* Install timer_handler as the signal handler for SIGVTALRM. */
		memset(&sa, 0x00, sizeof(sa));
		sa.sa_handler = &timer_handler;
		sigaction(SIGVTALRM, &sa, NULL);

		/* Configure the timer to expire after "ts" sec... */
		timer.it_value.tv_sec = ts;
		timer.it_value.tv_usec = 0;
		/* ... and every "ts" sec after that. */
		timer.it_interval.tv_sec = ts;
		timer.it_interval.tv_usec = 0;
		/* Start a virtual timer. It counts down whenever this process is executing. */
		setitimer(ITIMER_VIRTUAL, &timer, NULL);
	}

	if (restart) {
		pthread_barrier_wait(&barrier);
		no_checkpoint++;
	}

	// Run first CPU
	return vcpu_loop();
}
