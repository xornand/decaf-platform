/*
 Copyright (C) <2012> <Syracuse System Security (Sycure) Lab>

 DECAF is based on QEMU, a whole-system emulator. You can redistribute
 and modify it under the terms of the GNU GPL, version 3 or later,
 but it is made available WITHOUT ANY WARRANTY. See the top-level
 README file for more details.

 For more information about DECAF and other softwares, see our
 web site at:
 http://sycurelab.ecs.syr.edu/

 If you have any questions about DECAF,please post it on
 http://code.google.com/p/decaf-platform/
 */
/*
 * windows_vmi.c
 *
 *  Created on: Jun 8, 2012
 *      Author: haoru zhao, Heng Yin
 */

#include <inttypes.h>
#include <string>
#include <list>
#include <assert.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <tr1/unordered_map>
#include <tr1/unordered_set>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <queue>
#include <sys/time.h>
#include <math.h>
#include <glib.h>
#include <mcheck.h>
#include <stdio.h>
#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */
#include "cpu.h"
#include "config.h"
#include "hw/hw.h" // AWH
#include "qemu-timer.h"
#include "DECAF_main.h"
#include "DECAF_target.h"
//#include "sysemu.h"

#ifdef __cplusplus
}
;
#endif /* __cplusplus */

#include "windows_vmi.h"
#include "hookapi.h"
#include "function_map.h"
#include "shared/vmi.h"
#include "DECAF_main.h"
#include "shared/utils/SimpleCallback.h"

#if defined(TARGET_I386)

off_set xp_offset = { 0x18, 0x20, 0x2c, 0x88, 0x84, 0x174, 0x14c, 0x1a0, 0xc4,
		0x3c, 0x190, 0x1ec, 0x22c, 0x134, 0x78 };
off_set w7_offset = { 0x18, 0x20, 0x24, 0xb8, 0xb4, 0x16c, 0x140, 0x198, 0xf4,
		0x30, 0x188, 0x22c, 0x268, 0x128, 0xa8 };

static os_handle handle_funds[] = {
		{ WINXP_SP2, &xp_offset, 0, NULL, 0, 0, 0, },
		{ WINXP_SP3, &xp_offset, 0, NULL, 0, 0, 0, },
		{ WIN7_SP0, &w7_offset, 0, NULL, 0, 0, 0, },
		{ WIN7_SP1, &w7_offset, 0, NULL, 0, 0, 0, }, };

process *kernel_proc = NULL;
int rtflag = 0;
uint32_t gkpcr;
uint32_t GuestOS_index = 11;
uintptr_t block_handle = 0;
BYTE *recon_file_data_raw = 0;
#define MAX_MODULE_COUNT 500

/* Timer to check for proc exits */
static QEMUTimer *recon_timer = NULL;


unordered_map < uint32_t, pair<module *, uint32_t> > phys_module_map;


static inline int is_page_resolved(process *proc, uint32_t page_num)
{
	return (proc->resolved_pages.find(page_num>>12) != proc->resolved_pages.end());
}

static inline int unresolved_attempt(process *proc, uint32_t addr)
{
	unordered_map <uint32_t, int>::iterator iter = proc->unresolved_pages.find(addr>>12);
	if(iter == proc->unresolved_pages.end()) {
		proc->unresolved_pages[addr>>12] = 1;
		return 1;
	}
	iter->second++;

	return iter->second;
}


static process * find_new_process(CPUState *env, uint32_t cr3) {
	uint32_t kdvb, psAPH, curr_proc, next_proc;
	process *pe;

	if (gkpcr == 0)
		return 0;

	DECAF_read_mem(env, gkpcr + KDVB_OFFSET, 4, &kdvb);
	DECAF_read_mem(env, kdvb + PSAPH_OFFSET, 4, &psAPH);
	DECAF_read_mem(env, psAPH, 4, &curr_proc);

	while (curr_proc != 0 && curr_proc != psAPH) {
		uint32_t pid, proc_cr3;
		uint32_t curr_proc_base = curr_proc
				- handle_funds[GuestOS_index].offset->PSAPL_OFFSET;

		DECAF_read_mem(env,
				curr_proc_base
						+ handle_funds[GuestOS_index].offset->PSAPID_OFFSET, 4,
				&pid);
		if (VMI_find_process_by_pid(pid) != NULL) //we have seen this process
			goto next;

		DECAF_read_mem(env, curr_proc_base + 0x18, 4, &proc_cr3);
		if(cr3 != proc_cr3) //This is a new process, but not the current one. Skip it!
			goto next;

		//This is the one we are looking for
		pe = new process();
		pe->EPROC_base_addr = curr_proc_base;
		pe->pid = pid;
		pe->cr3 = proc_cr3;
		DECAF_read_mem(env, curr_proc_base + handle_funds[GuestOS_index].offset->PSAPNAME_OFFSET,
					NAMESIZE, pe->name);
		DECAF_read_mem(env,
					curr_proc_base
							+ handle_funds[GuestOS_index].offset->PSAPPID_OFFSET,
					4, &pe->parent_pid);
		VMI_create_process(pe);
		return pe;

next:
		DECAF_read_mem(env, curr_proc, 4, &next_proc);
		if (curr_proc == next_proc) { //why do we need this check?
			break;
		}
		curr_proc = next_proc;
	}

	return NULL;

}


static inline int get_IMAGE_NT_HEADERS(uint32_t cr3, uint32_t base, IMAGE_NT_HEADERS *nth)
{
	IMAGE_DOS_HEADER DosHeader;
	DECAF_read_mem(cpu_single_env, base, sizeof(IMAGE_DOS_HEADER), &DosHeader);

	if (DosHeader.e_magic != (0x5a4d)) {
		return -1;
	}

	if(DECAF_read_mem(cpu_single_env, base + DosHeader.e_lfanew,
			sizeof(IMAGE_NT_HEADERS), nth) < 0) {
		//monitor_printf(default_mon, "Reading NTHeader failed. :'(\n");
		//monitor_printf(default_mon, "%s, 0x%08x, %d\n", fullname, base, DosHeader->e_lfanew);
		return -1;
	}
    return 0;
}



//FIXME: this function may potentially overflow "buf" --Heng
static inline int readustr_with_cr3(uint32_t addr, uint32_t cr3, void *buf,
		CPUState *env) {
	uint32_t unicode_data[2];
	int i, j, unicode_len = 0;
	uint8_t unicode_str[MAX_UNICODE_LENGTH] = { '\0' };
	char *store = (char *) buf;

	if (DECAF_read_mem(env, addr, sizeof(unicode_data), unicode_data) < 0) {
		store[0] = '\0';
		goto done;
	}

	unicode_len = (int) (unicode_data[0] & 0xFFFF);
	if (unicode_len > MAX_UNICODE_LENGTH)
		unicode_len = MAX_UNICODE_LENGTH;

	if (DECAF_read_mem(env, unicode_data[1], unicode_len, (void *) unicode_str) < 0) {
		store[0] = '\0';
		goto done;
	}

	for (i = 0, j = 0; i < unicode_len; i += 2, j++) {
		store[j] = tolower(unicode_str[i]);
	}
	store[j] = '\0';

done:
	return j;
}


/* this function convert a full DLL name to a base name. */
static char * get_basename(char *fullname)
{
	int i = 0, last_slash = -1;
	for(; fullname[i] != 0; i++)
		if (fullname[i] == '/' || (fullname[i] == '\\'))
			last_slash = i;

	return fullname + last_slash + 1;
}

static void update_kernel_modules(CPUState *env, target_ulong vaddr) {
	uint32_t kdvb, psLM, curr_mod, next_mod;
	uint32_t holder;
	module *curr_entry = NULL;

	if (gkpcr == 0)
		return;

	//If this page has been resolved, return immediately
	//if (is_page_resolved(kernel_proc, vaddr)) return;

	DECAF_read_mem(env, gkpcr + KDVB_OFFSET, 4, &kdvb);
	DECAF_read_mem(env, kdvb + PSLM_OFFSET, 4, &psLM);
	DECAF_read_mem(env, psLM, 4, &curr_mod);

	while (curr_mod != 0 && curr_mod != psLM) {
		IMAGE_NT_HEADERS nth;
		uint32_t base = 0;
		DECAF_read_mem(env,
				curr_mod + handle_funds[GuestOS_index].offset->DLLBASE_OFFSET,
				4, &base);

		//if (is_page_resolved(kernel_proc, base))
			//goto next;


		char name[512];
		char key[512];
		char *base_name;

		readustr_with_cr3(curr_mod + handle_funds[GuestOS_index].offset->DLLNAME_OFFSET,
				0, name, env);
		base_name = get_basename(name);

		//We get checksum and use base module name along with checksum as the key to
		//uniquely identify a module.
		//We do not use full module name, because the same module can be referenced through
		//different full paths: e.g., c://windows/system32 and /systemroot/windows/system32.
		if(get_IMAGE_NT_HEADERS(env->cr[3], base, &nth) < 0)
			goto next;

		snprintf(key, sizeof(key)-1, "%s:%08x", base_name, nth.OptionalHeader.CheckSum);
		//See if we have extracted detailed info about this module
		curr_entry = VMI_find_module_by_key(key);
		if (!curr_entry) {
			curr_entry = new module();
			DECAF_read_mem(env,
					curr_mod + handle_funds[GuestOS_index].offset->SIZE_OFFSET,
					4, &curr_entry->size); // dllsize  SIZE_OFFSET

			strncpy(curr_entry->name, base_name, sizeof(curr_entry->name)-1);
			readustr_with_cr3(curr_mod + 0x24, 0, curr_entry->fullname, env);
			VMI_add_module(curr_entry, key);
		}

		VMI_insert_module(kernel_proc->pid, base, curr_entry);

next:
		DECAF_read_mem(env, curr_mod, 4, &next_mod);
		DECAF_read_mem(env, next_mod + 4, 4, &holder);
		if (holder != curr_mod) {
			//monitor_printf(default_mon,
			//		"Something is wrong. Next->prev != curr. curr_mod = 0x%08x\n",
			//		curr_mod);
			break;
		}
		curr_mod = next_mod;
	}

}

static void update_loaded_user_mods_with_peb(CPUState* env, process *proc,
		uint32_t peb, target_ulong vaddr)
{
	uint32_t cr3 = proc->cr3;
	uint32_t ldr, memlist, first_dll=0, curr_dll, count=0;
	module *curr_entry = NULL;

	if (peb == 0x00) return;

	DECAF_read_mem(env, peb + 0xc, 4, &ldr);
	memlist = ldr + 0xc;
	DECAF_read_mem(env, memlist, 4, &first_dll);

	if (first_dll == 0)	return;

	curr_dll = first_dll;
	do {
		IMAGE_NT_HEADERS nth;
		count++;
		uint32_t base = 0; //, size = 0;
		if (DECAF_read_mem(env, curr_dll + 0x18, 4, &base) < 0)
			break;

		if (!is_page_resolved(proc, base)) {
			char name[512];
			char key[512];

			readustr_with_cr3(curr_dll + 0x2c, 0, name, env);

			//We get checksum and use base module name along with checksum as the key to
			//uniquely identify a module.
			//We do not use full module name, because the same module can be referenced through
			//different full paths: e.g., c://windows/system32 and /systemroot/windows/system32.
			if(get_IMAGE_NT_HEADERS(cr3, base, &nth) < 0)
				goto next;

			snprintf(key, sizeof(key)-1, "%s:%08x", name, nth.OptionalHeader.CheckSum);
			//See if we have extracted detailed info about this module
			curr_entry = VMI_find_module_by_key(key);

			if(!curr_entry) { //We haven't seen this module before, even in other process memory spaces
				curr_entry = new module();
				readustr_with_cr3(curr_dll + 0x24, 0, curr_entry->fullname, env);
				DECAF_read_mem(env, curr_dll + 0x20, 4, &curr_entry->size);
				strncpy(curr_entry->name, name, sizeof(curr_entry->name)-1);
				VMI_add_module(curr_entry, key);
			}

			VMI_insert_module(proc->pid, base, curr_entry);
			//message_m(proc->pid, cr3, base, curr_entry);

		}

next:
		//read the next DLL
		DECAF_read_mem(env, curr_dll, 4, &curr_dll);
	} while (curr_dll != 0 && curr_dll != first_dll && count < MAX_MODULE_COUNT);

}

static void extract_export_table(IMAGE_NT_HEADERS *nth, uint32_t cr3, uint32_t base, module *mod)
{
	IMAGE_EXPORT_DIRECTORY ied;
	DWORD edt_va, edt_size;
	DWORD *func_addrs=NULL, *name_addrs=NULL;
	WORD *ordinals=NULL;
	char name[64];
	DWORD i;
	CPUState *env = cpu_single_env;
	edt_va = nth->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
	edt_size = nth->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].Size;

	if(DECAF_read_mem(env, base + edt_va, sizeof(ied), &ied) < 0) {
		//monitor_printf(default_mon, "Unable to read exp dir from image: mod=%s:%d base=%08x, va=%08x.\n", mod->name, ver, base, edt_va);
		//DECAF_stop_vm();
		goto done;
	}

	if(ied.NumberOfFunctions == 0 || ied.NumberOfNames == 0) {
		mod->symbols_extracted = true;
		goto done;
	}

	func_addrs = (DWORD *) malloc (sizeof(DWORD) * ied.NumberOfFunctions);
	name_addrs = (DWORD *) malloc (sizeof(DWORD) * ied.NumberOfNames);
	ordinals = (WORD *) malloc (sizeof(WORD) * ied.NumberOfNames);
	if(!func_addrs || !name_addrs || !ordinals)
		goto done;

	if(DECAF_read_mem(env, base + ied.AddressOfFunctions,
			sizeof(DWORD) * ied.NumberOfFunctions, func_addrs) < 0) {
		goto done;
	}

	if(DECAF_read_mem(env, base + ied.AddressOfNames,
			sizeof(DWORD) * ied.NumberOfNames, name_addrs) < 0) {
		goto done;
	}

	if(DECAF_read_mem(env, base + ied.AddressOfNameOrdinals,
			sizeof(WORD) * ied.NumberOfNames, ordinals) < 0) {
		goto done;
	}

	for(i = 0; i < ied.NumberOfNames; i++) {
		WORD index = ordinals[i];
		if(index >= ied.NumberOfFunctions)
			continue;

		DECAF_read_mem(env, base + name_addrs[i], sizeof(name)-1, name);
		name[127] = 0;
		funcmap_insert_function(mod->name, name, func_addrs[index]);
	}
	//monitor_printf(default_mon, "%s: Total exports = %d, %d\n", mod->fullname, ied.NumberOfFunctions, ied.NumberOfNames);

	mod->symbols_extracted = true;

done:
	if(func_addrs)
		free(func_addrs);

	if(name_addrs)
		free(name_addrs);

	if(ordinals)
		free(ordinals);
}

//Extract info in PE header and export table from a PE module
//If everything is done successfully, mod->symbols_extracted will be set true
static void extract_PE_info(uint32_t cr3, uint32_t base, module *mod)
{
	IMAGE_NT_HEADERS nth;

	if (get_IMAGE_NT_HEADERS(cr3, base, &nth) < 0)
		return;

	mod->checksum = nth.OptionalHeader.CheckSum;
	mod->codesize = nth.OptionalHeader.SizeOfCode;
	mod->major = nth.OptionalHeader.MajorImageVersion;
	mod->minor = nth.OptionalHeader.MinorImageVersion;
	extract_export_table(&nth, cr3, base, mod);
}


static void retrieve_missing_symbols(process *proc)
{
	unordered_map < uint32_t,module * >::iterator iter = proc->module_list.begin();

	for(; iter!=proc->module_list.end(); iter++) {
		module *cur_mod = iter->second;
		if(!cur_mod->symbols_extracted)
			extract_PE_info(proc->cr3, iter->first, cur_mod);
	}
}

static inline void get_new_modules(CPUState* env, process * proc, target_ulong vaddr)
{
	uint32_t base = 0, self = 0, pid = 0;
	if (proc == kernel_proc) {
		update_kernel_modules(env, vaddr);
	} else {
		base = env->segs[R_FS].base;
		DECAF_read_mem(env, base + 0x18, 4, &self);

		if (base != 0 && base == self) {
			//Why don't you use the offset table instead of these hard-coded offsets?
			uint32_t pid_addr = base + 0x20;
			DECAF_read_mem(env, pid_addr, 4, &pid);
			uint32_t peb_addr = base + 0x30;
			uint32_t peb;
			DECAF_read_mem(env, peb_addr, 4, &peb);
			update_loaded_user_mods_with_peb(env, proc, peb, vaddr);
		}
	}
}

static void tlb_call_back(DECAF_Callback_Params *temp)
{
	CPUState *env = temp->tx.env;
	target_ulong vaddr = temp->tx.vaddr;
	uint32_t cr3 = env->cr[3];
	process *proc;

	if(DECAF_is_in_kernel()) {
		proc = kernel_proc;
		kernel_proc->cr3 = cr3;
	} else {
		proc = VMI_find_process_by_pgd(cr3);
		if (proc == NULL)
			proc = find_new_process(env, cr3);
	}

	if (proc ) {
		if (!is_page_resolved(proc, vaddr)) {
			get_new_modules(env, proc, vaddr);

			if (!is_page_resolved(proc, vaddr)) {
				int attempts = unresolved_attempt(proc, vaddr);
				if (attempts > 3)
					proc->resolved_pages.insert(vaddr>>12);
			}

		}
		retrieve_missing_symbols(proc);
	}
}

static uint32_t get_kpcr(void)
{
	uint32_t kpcr = 0, selfpcr = 0;
	CPUState *env;

	for (env = first_cpu; env != NULL; env = env->next_cpu) {
		if (env->cpu_index == 0) {
			break;
		}
	}

	DECAF_read_mem(env, env->segs[R_FS].base + 0x1c, 4, &selfpcr);

	if (selfpcr == env->segs[R_FS].base) {
		kpcr = selfpcr;
	}

	return kpcr;
}

static void get_os_version(CPUState *env)
{
	//TODO: We need to check how volatility determines the OS version.

	if (gkpcr == 0xffdff000) {
		//cpu_memory_rw_debug(env, gkpcr + 0x34, (uint8_t *) &kdvb, 4, 0);
		//cpu_memory_rw_debug(env, kdvb + 0x290, (uint8_t *) &CmNtCSDVersion, 4, 0); //CmNt version info
		//cpu_memory_rw_debug(env, CmNtCSDVersion, (uint8_t *) &num_package, 4, 0);
		//uint32_t num = num_package >> 8;
		//if (num == 0x02) {
		GuestOS_index = 0; //winxpsp2
		//} else if (num == 0x03) {
		//	GuestOS_index = 1; //winxpsp3
		//}
	} else {
		GuestOS_index = 2; //win7
	}
}


static uint32_t get_ntoskrnl_internal(uint32_t curr_page, CPUState *env) {
	IMAGE_DOS_HEADER *DosHeader = NULL;

	uint8_t page_data[4 * 1024] = { 0 }; //page_size
	uint16_t DOS_HDR = 0x5a4d;

	for (; curr_page > 0x80000000; curr_page -= 4096) {
		if (DECAF_read_mem(env, curr_page, 4096, page_data) < 0)
			//this page is not present in ram, just continue
			continue;

		if (memcmp(&page_data, &DOS_HDR, 2) != 0)
			continue;

		DosHeader = (IMAGE_DOS_HEADER *) &(page_data);
		if ((DosHeader->e_magic == 0x5a4d)
				&&
				 (*((uint32_t *) (&page_data[*((uint32_t *) &page_data[0x3c])]))
										== IMAGE_NT_SIGNATURE))
			return curr_page;
	}

	return 0;
}

uint32_t get_ntoskrnl(CPUState *env) {
	uint32_t ntoskrnl_base = 0;
	ntoskrnl_base = get_ntoskrnl_internal(env->sysenter_eip & 0xfffff000, env);
	if (ntoskrnl_base)
		goto found;

	ntoskrnl_base = get_ntoskrnl_internal(env->eip & 0xfffff000, env);
	if (ntoskrnl_base)
		goto found;
	return 0;

found:
	return ntoskrnl_base;
}

/*FIXME: Supports only 32bit guest. */
static uint32_t probe_windows(CPUState *env)
{
	uint32_t base;

	if (env->eip > 0x80000000 && env->segs[R_FS].base > 0x80000000) {
		gkpcr = get_kpcr();
		if (gkpcr != 0) {
			//DECAF_unregister_callback(DECAF_INSN_END_CB, insn_handle);
			rtflag = 1;

			get_os_version(env);
			base = get_ntoskrnl(env);
			monitor_printf(default_mon,
					"The base address of ntoskrnl.exe is %08x\n", base);
			if (!base) {
				monitor_printf(default_mon,
						"Unable to locate kernel base. Stopping VM...\n");
				//vm_stop(RUN_STATE_DEBUG);
				return 0;
			} else {
				VMI_guest_kernel_base = 0x80000000;
				return base;
			}
		}
	}

	return 0;
}

int find_win7sp0(CPUState *env, uintptr_t insn_handle) {
	probe_windows(env);
	if (GuestOS_index == 2 && rtflag == 1)
		return 1;
	else
		return 0;
}
int find_win7sp1(CPUState *env, uintptr_t insn_handle) {
	probe_windows(env);
	if (GuestOS_index == 2 && rtflag == 1)
		return 1;
	else
		return 0;
}
int find_winxpsp2(CPUState *env, uintptr_t insn_handle) {

	probe_windows(env);
	if (GuestOS_index == 0 && rtflag == 1)
		return 1;
	else
		return 0;
}
int find_winxpsp3(CPUState *env, uintptr_t insn_handle) {
	probe_windows(env);
	if (GuestOS_index == 1 && rtflag == 1)
		return 1;
	else
		return 0;
}



void check_procexit(void *)
{
	CPUState *env = cpu_single_env ? cpu_single_env : first_cpu;
	qemu_mod_timer(recon_timer,
			qemu_get_clock_ns(vm_clock) + get_ticks_per_sec() * 10);
	//monitor_printf(default_mon, "Checking for proc exits...\n");

	uint32_t end_time[2];
	vector<target_ulong> pid_list;

	unordered_map < uint32_t, process * >::iterator iter = process_map.begin();
	for (; iter!=process_map.end(); iter++) {
		process *proc = iter->second;
		if (proc->parent_pid == 0)
			continue;

		//0x78 for xp, 0x88 for win7
		DECAF_read_mem(env, proc->EPROC_base_addr
						+ handle_funds[GuestOS_index].offset->PEXIT_TIME,
						8,  end_time);
		if (end_time[0] | end_time[1]) {
			pid_list.push_back(proc->pid);
		}
	}

	for (size_t i=0; i<pid_list.size(); i++) {
		VMI_remove_process(pid_list[i]);
	}
}

void win_vmi_init()
{
	DECAF_register_callback(DECAF_TLB_EXEC_CB, tlb_call_back, NULL);
	kernel_proc = new process();
	kernel_proc->cr3 = 0;
	strcpy(kernel_proc->name, "<kernel>");
	kernel_proc->pid = 0;
	VMI_create_process(kernel_proc);

	recon_timer = qemu_new_timer_ns(vm_clock, check_procexit, 0);
	qemu_mod_timer(recon_timer,
			qemu_get_clock_ns(vm_clock) + get_ticks_per_sec() * 30);

}

#endif