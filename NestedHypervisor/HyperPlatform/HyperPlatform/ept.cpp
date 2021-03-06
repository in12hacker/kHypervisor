// Copyright (c) 2015-2016, tandasat. All rights reserved.
// Use of this source code is governed by a MIT-style license that can be
// found in the LICENSE file.

/// @file
/// Implements EPT functions.

#include "ept.h"
#include "asm.h"
#include "common.h"
#include "log.h"
#include "util.h"
#ifndef HYPERPLATFORM_PERFORMANCE_ENABLE_PERFCOUNTER
#define HYPERPLATFORM_PERFORMANCE_ENABLE_PERFCOUNTER 1
#endif  // HYPERPLATFORM_PERFORMANCE_ENABLE_PERFCOUNTER
#include "performance.h"
#include "../../DdiMon/shadow_hook.h"

extern "C" {
////////////////////////////////////////////////////////////////////////////////
//
// macro utilities
//

////////////////////////////////////////////////////////////////////////////////
//
// constants and macros
//

// Followings are how 64bits of a pysical address is used to locate EPT entries:
//
// EPT Page map level 4 selector           9 bits
// EPT Page directory pointer selector     9 bits
// EPT Page directory selector             9 bits
// EPT Page table selector                 9 bits
// EPT Byte within page                   12 bits

// Get the highest 25 bits
static const auto kVmxpPxiShift = 39ull;

// Get the highest 34 bits
static const auto kVmxpPpiShift = 30ull;

// Get the highest 43 bits
static const auto kVmxpPdiShift = 21ull;

// Get the highest 52 bits
static const auto kVmxpPtiShift = 12ull;

// Use 9 bits; 0b0000_0000_0000_0000_0000_0000_0001_1111_1111
static const auto kVmxpPtxMask = 0x1ffull;

// How many EPT entries are preallocated. When the number exceeds it, the
// hypervisor issues a bugcheck.
static const auto kVmxpNumberOfPreallocatedEntries = 50;

////////////////////////////////////////////////////////////////////////////////
//
// types
//

// EPT related data stored in ProcessorSharedData
struct EptData {
  EptPointer *ept_pointer;
  EptCommonEntry *ept_pml4;

  EptCommonEntry **preallocated_entries;  // An array of pre-allocated entries
  volatile long preallocated_entries_count;  // # of used pre-allocated entries
};

////////////////////////////////////////////////////////////////////////////////
//
// prototypes
//

_When_(ept_data == nullptr,
       _IRQL_requires_max_(DISPATCH_LEVEL)) static EptCommonEntry
    *EptpConstructTables(_In_ EptCommonEntry *table, _In_ ULONG table_level,
                         _In_ ULONG64 physical_address,
                         _In_opt_ EptData *ept_data);

static void EptpDestructTables(_In_ EptCommonEntry *table,
                               _In_ ULONG table_level);

_Must_inspect_result_ __drv_allocatesMem(Mem)
    _When_(ept_data == nullptr,
           _IRQL_requires_max_(DISPATCH_LEVEL)) static EptCommonEntry
        *EptpAllocateEptEntry(_In_opt_ EptData *ept_data);

static EptCommonEntry *EptpAllocateEptEntryFromPreAllocated(
    _In_ EptData *ept_data);

_Must_inspect_result_ __drv_allocatesMem(Mem) _IRQL_requires_max_(
    DISPATCH_LEVEL) static EptCommonEntry *EptpAllocateEptEntryFromPool();


static void EptpInitTableEntry(_In_ EptCommonEntry *Entry,
                               _In_ ULONG table_level,
                               _In_ ULONG64 physical_address);


//?取Pxe索引
static ULONG64 EptpAddressToPxeIndex(_In_ ULONG64 physical_address);

//?取Ppe索引
static ULONG64 EptpAddressToPpeIndex(_In_ ULONG64 physical_address);

//?取Pde索引
static ULONG64 EptpAddressToPdeIndex(_In_ ULONG64 physical_address);

//?取Pte索引
static ULONG64 EptpAddressToPteIndex(_In_ ULONG64 physical_address);

static bool EptpIsDeviceMemory(_In_ ULONG64 physical_address);

static EptCommonEntry *EptpGetEptPtEntry(_In_ EptCommonEntry *table,
                                         _In_ ULONG table_level,
                                         _In_ ULONG64 physical_address);

static bool EptpIsCopiedKiInterruptTemplate(_In_ void *virtual_address);

_IRQL_requires_min_(DISPATCH_LEVEL) static void EptpAddDisabledEntry(
    _In_ EptData *ept_data, _In_ EptCommonEntry *ept_entry);

_IRQL_requires_min_(DISPATCH_LEVEL) static void EptpResetDisabledEntriesUnsafe(
    _In_ EptData *ept_data);

static void EptpFreeUnusedPreAllocatedEntries(
    _Pre_notnull_ __drv_freesMem(Mem) EptCommonEntry **preallocated_entries,
    _In_ long used_count);

#if defined(ALLOC_PRAGMA)
#pragma alloc_text(INIT, EptIsEptAvailable)
#pragma alloc_text(INIT, EptGetEptPointer)
#pragma alloc_text(INIT, EptInitialization)
#endif

////////////////////////////////////////////////////////////////////////////////
//
// variables
//

////////////////////////////////////////////////////////////////////////////////
//
// implementations
//

// Checks if the system supports EPT technology sufficient enough

// 透?查看cpuid 檢查系統是否支持ept
_Use_decl_annotations_ bool EptIsEptAvailable() 
{
  PAGED_CODE();

  int regs[4] = {};
  __cpuidex(regs, 0x80000008, 0);
  Cpuid80000008Eax cpuidEax = {static_cast<ULONG32>(regs[0])};
  HYPERPLATFORM_LOG_DEBUG("Physical Address Range = %d bits",
                          cpuidEax.fields.physical_address_bits);

  // No processors supporting the Intel 64 architecture support more than 48
  // physical-address bits
  if (cpuidEax.fields.physical_address_bits > 48) {
    return false;
  }

  // page walk length is 4 steps
  // extended page tables can be laid out in write-back memory
  // INVEPT instruction with all possible types is supported

  //?取EPT的VPID和EPT的能力報告
  Ia32VmxEptVpidCapMsr vpid = {UtilReadMsr64(Msr::kIa32VmxEptVpidCap)};

  if (!vpid.fields.support_page_walk_length4 ||		//是否支持4級PAGE_WALK
      !vpid.fields.support_execute_only_pages ||	//是否支持可執行?
      !vpid.fields.support_write_back_memory_type ||//是否支持回寫內存?型
      !vpid.fields.support_invept ||				//是否INVEPT指令
      !vpid.fields.support_single_context_invept || //是否支持INVEPT type = 1 把指定在EPT描述符中的EPTP中的所有?面解除映射?係
      !vpid.fields.support_all_context_invept) {	//是否支持INVEPT tpye = 2 把EPTP所有?面解除映射?係
    return false;
  }
  return true;
}

// Returns an EPT pointer from ept_data
//
//?取EPT指?
_Use_decl_annotations_ ULONG64 EptGetEptPointer(EptData *ept_data) {
  return ept_data->ept_pointer->all;
}

// Builds EPT, allocates pre-allocated enties, initializes and returns EptData, 

// 建立EPT頁表 , 分配的表項 , 初始化及返回
_Use_decl_annotations_ EptData *EptInitialization() {
  PAGED_CODE();

  static const auto kEptPageWalkLevel = 4ul;

  // Allocate ept_data
  // 非分?內存(WIN8後支持的?型) 存放EPT數據結構(?似EPT對象)
  const auto ept_data = reinterpret_cast<EptData *>(ExAllocatePoolWithTag(
      NonPagedPoolNx, sizeof(EptData), kHyperPlatformCommonPoolTag));
  if (!ept_data) {
    return nullptr;
  }
  RtlZeroMemory(ept_data, sizeof(EptData));

  // Allocate EptPointer
  // 分配EPT指?(EPTP), 他指向了PML4(?似CR3指向?目??), 也是在非分?內存
  const auto ept_poiner = reinterpret_cast<EptPointer *>(ExAllocatePoolWithTag(
      NonPagedPoolNx, PAGE_SIZE, kHyperPlatformCommonPoolTag));
  if (!ept_poiner) {
    ExFreePoolWithTag(ept_data, kHyperPlatformCommonPoolTag);
    return nullptr;
  }
  RtlZeroMemory(ept_poiner, PAGE_SIZE);

  // Allocate EPT_PML4 and initialize EptPointer
  // 分配EPT_PML4 頁表 (類似頁目錄項(PDE)的數組), 也是在非分頁內存
  const auto ept_pml4 =
      reinterpret_cast<EptCommonEntry *>(ExAllocatePoolWithTag(
          NonPagedPoolNx, PAGE_SIZE, kHyperPlatformCommonPoolTag));

  if (!ept_pml4) {
    ExFreePoolWithTag(ept_poiner, kHyperPlatformCommonPoolTag);
    ExFreePoolWithTag(ept_data, kHyperPlatformCommonPoolTag);
    return nullptr;
  }
  RtlZeroMemory(ept_pml4, PAGE_SIZE);

  //?置EPTP(指向PML4的指?)的屬性
 
  //內存?型 ?置為回寫
  ept_poiner->fields.memory_type = static_cast<ULONG64>(memory_type::kWriteBack);
  //EPT?表有多少級別
  ept_poiner->fields.page_walk_length = kEptPageWalkLevel - 1;
  //指向PML4的物理地址
  ept_poiner->fields.pml4_address = UtilPfnFromPa(UtilPaFromVa(ept_pml4));

  // Initialize all EPT entries for all physical memory pages

  //描述符布局如下:
  /*   
   *  已經初始化了pm_block 內存塊數組:
   *  總括一下內容:
      pm_block->number_of_runs		; 內存塊數量
	  pm_block->number_of_page		; 內存塊的?面總大小
	  pm_block->run[1]				; 數組包含以下結構 
					  -> base_page	; 對應內存塊的基址
					  -> page_count ; 對應內存塊的?面數量
   */
  
  //?取物理內存描述符(自定義的UTIL.h)
  const auto pm_ranges = UtilGetPhysicalMemoryRanges();
  
 
  //其實以下在於遍歷每一塊可用的內存塊中的每一塊 也COPY一份到EPT中

  //遍歷所有物理內存塊
  for (auto run_index = 0ul; run_index < pm_ranges->number_of_runs; ++run_index) 
  {
	//?取物理內存塊地址
    const auto run = &pm_ranges->run[run_index];			//物理內存描述符->物理?面描述符(名為PhysicalMemoryRun)
    const auto base_addr = run->base_page * PAGE_SIZE;  //透?描述符?算?面基址 = ?面?*?面大小
	
	//遍歷??物理內存塊佔有的所有?面 
    for (auto page_index = 0ull; page_index < run->page_count; ++page_index) {
	  //第一?基址 遍歷每一?
      const auto indexed_addr = base_addr + page_index * PAGE_SIZE;
	  //傳入EPT_PML4的地址, 級別為4, 物理內存塊地址
	  //以結立EPTP?表
      const auto ept_pt_entry = EptpConstructTables(ept_pml4, 4, indexed_addr, nullptr);
      if (!ept_pt_entry) {
        EptpDestructTables(ept_pml4, 4);
        ExFreePoolWithTag(ept_poiner, kHyperPlatformCommonPoolTag);
        ExFreePoolWithTag(ept_data, kHyperPlatformCommonPoolTag);
        return nullptr;
      }
    }
  }

  // Initialize an EPT entry for APIC_BASE. It is required to allocated it now
  // for some reasons, or else, system hangs.

  //CPU 的 LAPIC基址
  const Ia32ApicBaseMsr apic_msr = {UtilReadMsr64(Msr::kIa32ApicBase)};

  //為??基址最行??映射到EPT?表
  if (!EptpConstructTables(ept_pml4, 4, apic_msr.fields.apic_base * PAGE_SIZE, nullptr)) {
    EptpDestructTables(ept_pml4, 4);
    ExFreePoolWithTag(ept_poiner, kHyperPlatformCommonPoolTag);
    ExFreePoolWithTag(ept_data, kHyperPlatformCommonPoolTag);
    return nullptr;
  }

  // Allocate preallocated_entries

  //除以上兩?, ?先分配的表?
  
  //?算要分配多少?表?
  const auto preallocated_entries_size = sizeof(EptCommonEntry *) * kVmxpNumberOfPreallocatedEntries;
  //分配在非分?內存
  const auto preallocated_entries = reinterpret_cast<EptCommonEntry **>(
      ExAllocatePoolWithTag(NonPagedPoolNx, preallocated_entries_size,
                            kHyperPlatformCommonPoolTag));
  if (!preallocated_entries) {
    EptpDestructTables(ept_pml4, 4);
    ExFreePoolWithTag(ept_poiner, kHyperPlatformCommonPoolTag);
    ExFreePoolWithTag(ept_data, kHyperPlatformCommonPoolTag);
    return nullptr;
  }
  RtlZeroMemory(preallocated_entries, preallocated_entries_size);

  // And fill preallocated_entries with newly created entries
  // 填充?先分配的表?
  for (auto i = 0ul; i < kVmxpNumberOfPreallocatedEntries; ++i) {
	//分配512?表?及?取其地址
    const auto ept_entry = EptpAllocateEptEntry(nullptr);
    if (!ept_entry) {
      EptpFreeUnusedPreAllocatedEntries(preallocated_entries, 0);
      EptpDestructTables(ept_pml4, 4);
      ExFreePoolWithTag(ept_poiner, kHyperPlatformCommonPoolTag);
      ExFreePoolWithTag(ept_data, kHyperPlatformCommonPoolTag);
      return nullptr;
    }
	//加插到數組中
    preallocated_entries[i] = ept_entry;
  }

  // Initialization completed
  // 完成初始化
  // 指向EPTP(包含一?結構?,保存PML4物理地址,內存屬性等信息)
  ept_data->ept_pointer = ept_poiner;
  // 指向PML4指??擬地址
  ept_data->ept_pml4 = ept_pml4;
  // ?先分配的表?數組
  ept_data->preallocated_entries = preallocated_entries;
  // ??為0, ?有被加插到表中
  ept_data->preallocated_entries_count = 0;
  
  /* 總結: 
   * 
   * EPTP結構如下:
		union EptPointer {
		  ULONG64 all;
		  struct {
			ULONG64 memory_type : 3;                      ///< [0:2]	回寫?型
			ULONG64 page_walk_length : 3;                 ///< [3:5]    4-1
			ULONG64 enable_accessed_and_dirty_flags : 1;  ///< [6]
			ULONG64 reserved1 : 5;                        ///< [7:11]
			ULONG64 pml4_address : 36;                    ///< [12:48-1] PML4 物理地址
			ULONG64 reserved2 : 16;                       ///< [48:63]
		  } fields;
		};
   * EPT初始化後得到EPT_DATA 
   * EPT_DATA結構:
		struct EptData {
		  EptPointer *ept_pointer;  //EPTP結構?(包含PML4物理地址)
		  EptCommonEntry *ept_pml4;	//PML4?擬地址

		  EptCommonEntry **preallocated_entries;    //?先分配的目??指?數組
		  volatile long preallocated_entries_count; //已插入的
		}; 
   
    ??EPT?表??分配的表?包括:
    那些?續的內存塊全部每一?的物理內存基址被分配並插入到EPT中, APIC基址
	每次分配都會分配級別以下的所有級別?表, 每512?為一?表
	保存數據到EPT_DATA和EPTP
   */
  return ept_data;
}

// Allocate and initialize all EPT entries associated with the physical_address

//建立對應物理地址 的EPT 4級?表, 並初始化對應的索引?, ?用路徑由高級?表向下?用 直到?置了PT中的物理地址(PT?似32位的PFN)
_Use_decl_annotations_ static EptCommonEntry *EptpConstructTables(
    EptCommonEntry *table, 
	ULONG table_level, 
	ULONG64 physical_address,
    EptData *ept_data) 
{
  switch (table_level) {
    case 4: {
      // table == PML4 (512 GB)

	  // ?取物理地址的Pxe索引
      const auto pxe_index = EptpAddressToPxeIndex(physical_address);

	  // 使用??索引?取EPT中PML4表?
      const auto ept_pml4_entry = &table[pxe_index];

	  // 如果表??有使用?
      if (!ept_pml4_entry->all) {
		
		 //分配下一級的?表空? 並返回基址
        const auto ept_pdpt = EptpAllocateEptEntry(ept_data);
        if (!ept_pdpt) {
          return nullptr;
        }
		//初始化表? , ?置表?屬性為read , write , execute, 地址為下一級?表(pdpt)物理地址
        EptpInitTableEntry(ept_pml4_entry, table_level, UtilPaFromVa(ept_pdpt));
      }
      return EptpConstructTables(
          reinterpret_cast<EptCommonEntry *>(
              UtilVaFromPfn(ept_pml4_entry->fields.physial_address)),
          table_level - 1, physical_address, ept_data);
    }
    case 3: {
      // table == PDPT (1 GB)
	  // ?取物理地址的PPE索引
	  const auto ppe_index = EptpAddressToPpeIndex(physical_address);
	  //使用??索引?取EPT中PDPT表?
      const auto ept_pdpt_entry = &table[ppe_index];
	  //表??有被初始化
      if (!ept_pdpt_entry->all) {
		 //分配512?pdt表?, 並返回基址
        const auto ept_pdt = EptpAllocateEptEntry(ept_data);
        if (!ept_pdt) {
          return nullptr;
        }
		//初始化表? , ?置表?屬性為read , write , execute, 地址為下一級?表(pdt)物理地址
        EptpInitTableEntry(ept_pdpt_entry, table_level, UtilPaFromVa(ept_pdt));
      }
	  //往下一級?發...
      return EptpConstructTables(
          reinterpret_cast<EptCommonEntry *>(
              UtilVaFromPfn(ept_pdpt_entry->fields.physial_address)),
          table_level - 1, physical_address, ept_data);
    }
    case 2: {
      // table == PDT (2 MB)
	  //?取物理地址的PDE索引
      const auto pde_index = EptpAddressToPdeIndex(physical_address);
	  //根據索引?取表?
      const auto ept_pdt_entry = &table[pde_index];

      if (!ept_pdt_entry->all) {
		  //分配下一級的?表空? 並返回基址
        const auto ept_pt = EptpAllocateEptEntry(ept_data);
        if (!ept_pt) {
          return nullptr;
		}
		//初始化表? , ?置表?屬性為read , write , execute, 地址為下一級?表(pdt)物理地址
		EptpInitTableEntry(ept_pdt_entry, table_level, UtilPaFromVa(ept_pt));
      }
	
      return EptpConstructTables(
          reinterpret_cast<EptCommonEntry *>(
              UtilVaFromPfn(ept_pdt_entry->fields.physial_address)),
          table_level - 1, physical_address, ept_data);
    }
    case 1: {
      // table == PT (4 KB)
	  //?取物理地址的PT表
      const auto pte_index = EptpAddressToPteIndex(physical_address);
      const auto ept_pt_entry = &table[pte_index];
      NT_ASSERT(!ept_pt_entry->all);
	  //直接?置為物理地址
      EptpInitTableEntry(ept_pt_entry, table_level, physical_address);
      return ept_pt_entry;
    }
    default:
      HYPERPLATFORM_COMMON_DBG_BREAK();
      return nullptr;
  }
}

// Return a new EPT entry either by creating new one or from pre-allocated ones
_Use_decl_annotations_ static EptCommonEntry *EptpAllocateEptEntry(
    EptData *ept_data) {
  if (ept_data) {
    return EptpAllocateEptEntryFromPreAllocated(ept_data);
  } else {
    return EptpAllocateEptEntryFromPool();
  }
}

// Return a new EPT entry from pre-allocated ones.
_Use_decl_annotations_ static EptCommonEntry *
EptpAllocateEptEntryFromPreAllocated(EptData *ept_data) {
  const auto count =
      InterlockedIncrement(&ept_data->preallocated_entries_count);
  if (count > kVmxpNumberOfPreallocatedEntries) {
    HYPERPLATFORM_COMMON_BUG_CHECK(
        HyperPlatformBugCheck::kExhaustedPreallocatedEntries, count,
        reinterpret_cast<ULONG_PTR>(ept_data), 0);
  }
  return ept_data->preallocated_entries[count - 1];
}

// Return a new EPT entry either by creating new one
_Use_decl_annotations_ static EptCommonEntry *EptpAllocateEptEntryFromPool() {
  static const auto kAllocSize = 512 * sizeof(EptCommonEntry);
  static_assert(kAllocSize == PAGE_SIZE, "Size check");

  const auto entry = reinterpret_cast<EptCommonEntry *>(ExAllocatePoolWithTag(
      NonPagedPoolNx, kAllocSize, kHyperPlatformCommonPoolTag));
  if (!entry) {
    return nullptr;
  }
  RtlZeroMemory(entry, kAllocSize);
  return entry;
}

// Initialize an EPT entry with a "pass through" attribute
_Use_decl_annotations_ static void EptpInitTableEntry(
    EptCommonEntry *entry, ULONG table_level, ULONG64 physical_address) {
  entry->fields.read_access = true;
  entry->fields.write_access = true;
  entry->fields.execute_access = true;
  entry->fields.physial_address = UtilPfnFromPa(physical_address);
  if (table_level == 1) {
    entry->fields.memory_type = static_cast<ULONG64>(memory_type::kWriteBack);
  }
}

// Return an address of PXE
_Use_decl_annotations_ static ULONG64 EptpAddressToPxeIndex(
    ULONG64 physical_address) {
  const auto index = (physical_address >> kVmxpPxiShift) & kVmxpPtxMask;
  return index;
}

// Return an address of PPE
_Use_decl_annotations_ static ULONG64 EptpAddressToPpeIndex(
    ULONG64 physical_address) {
  const auto index = (physical_address >> kVmxpPpiShift) & kVmxpPtxMask;
  return index;
}

// Return an address of PDE
_Use_decl_annotations_ static ULONG64 EptpAddressToPdeIndex(
    ULONG64 physical_address) {
  const auto index = (physical_address >> kVmxpPdiShift) & kVmxpPtxMask;
  return index;
}

// Return an address of PTE
_Use_decl_annotations_ static ULONG64 EptpAddressToPteIndex(
    ULONG64 physical_address) {
  const auto index = (physical_address >> kVmxpPtiShift) & kVmxpPtxMask;
  return index;
}

// Deal with EPT violation VM-exit.
_Use_decl_annotations_ void EptHandleEptViolation(
    EptData *ept_data, ShadowHookData *sh_data,
    SharedShadowHookData *shared_sh_data) {
  const EptViolationQualification exit_qualification = {
      UtilVmRead(VmcsField::kExitQualification)};

 // const auto guest_cr3 = UtilVmRead64(VmcsField::kGuestCr3);

  const auto fault_pa = UtilVmRead64(VmcsField::kGuestPhysicalAddress);
  const auto fault_va =
      exit_qualification.fields.valid_guest_linear_address
          ? reinterpret_cast<void *>(UtilVmRead(VmcsField::kGuestLinearAddress))
          : nullptr;

  if (!exit_qualification.fields.ept_readable &&
      !exit_qualification.fields.ept_writeable &&
      !exit_qualification.fields.ept_executable ) {
    // EPT entry miss. It should be device memory.
    HYPERPLATFORM_PERFORMANCE_MEASURE_THIS_SCOPE();

    if (!IsReleaseBuild()) {
      NT_VERIFY(EptpIsDeviceMemory(fault_pa));
    }
//	DbgPrint("CR3 without EPT : %x  %s\r\n", guest_cr3, ((ULONG)PsGetCurrentProcess()+0x2D8));
	EptpConstructTables(ept_data->ept_pml4, 4, fault_pa, ept_data);

    UtilInveptAll();
  } 
  else if (exit_qualification.fields.caused_by_translation) {
    // Tell EPT violation when it is caused due to read or write violation.
    const auto read_failure = exit_qualification.fields.read_access &&
                              !exit_qualification.fields.ept_readable;
    const auto write_failure = exit_qualification.fields.write_access &&
                               !exit_qualification.fields.ept_writeable;
	const auto execute_failure = exit_qualification.fields.execute_access &&
								!exit_qualification.fields.ept_executable;
	bool readOrWrite;
	if (read_failure)
		readOrWrite = true;
	else if (write_failure)
		readOrWrite = false;
    
	if (read_failure || write_failure || execute_failure) {
		//if (!K_HandleEptViolation(sh_data, shared_sh_data, ept_data, fault_va, execute_failure))
		//{
			ShHandleEptViolation(sh_data, shared_sh_data, ept_data, fault_va);
		//}
    } else {
      DbgPrint("[IGNR] OTH VA = %p, PA = %016llx", fault_va,
                                   fault_pa);
    }
  } else {
    DbgPrint("[IGNR] OTH VA = %p, PA = %016llx", fault_va,
                                 fault_pa);
  }
}

// Returns if the physical_address is device memory (which could not have a
// corresponding PFN entry)
_Use_decl_annotations_ static bool EptpIsDeviceMemory(
    ULONG64 physical_address) {
  const auto pm_ranges = UtilGetPhysicalMemoryRanges();
  for (auto i = 0ul; i < pm_ranges->number_of_runs; ++i) {
    const auto current_run = &pm_ranges->run[i];
    const auto base_addr =
        static_cast<ULONG64>(current_run->base_page) * PAGE_SIZE;
    const auto endAddr = base_addr + current_run->page_count * PAGE_SIZE - 1;
    if (UtilIsInBounds(physical_address, base_addr, endAddr)) {
      return false;
    }
  }
  return true;
}

// Returns an EPT entry corresponds to the physical_address
_Use_decl_annotations_ EptCommonEntry *EptGetEptPtEntry(
    EptData *ept_data, ULONG64 physical_address) {
  return EptpGetEptPtEntry(ept_data->ept_pml4, 4, physical_address);
}

// Returns an EPT entry corresponds to the physical_address
_Use_decl_annotations_ static EptCommonEntry *EptpGetEptPtEntry(
    EptCommonEntry *table, ULONG table_level, ULONG64 physical_address) {
  switch (table_level) {
    case 4: {
      // table == PML4
      const auto pxe_index = EptpAddressToPxeIndex(physical_address);
      const auto ept_pml4_entry = &table[pxe_index];
      return EptpGetEptPtEntry(reinterpret_cast<EptCommonEntry *>(UtilVaFromPfn(
                                   ept_pml4_entry->fields.physial_address)),
                               table_level - 1, physical_address);
    }
    case 3: {
      // table == PDPT
      const auto ppe_index = EptpAddressToPpeIndex(physical_address);
      const auto ept_pdpt_entry = &table[ppe_index];
      return EptpGetEptPtEntry(reinterpret_cast<EptCommonEntry *>(UtilVaFromPfn(
                                   ept_pdpt_entry->fields.physial_address)),
                               table_level - 1, physical_address);
    }
    case 2: {
      // table == PDT
      const auto pde_index = EptpAddressToPdeIndex(physical_address);
      const auto ept_pdt_entry = &table[pde_index];
      return EptpGetEptPtEntry(reinterpret_cast<EptCommonEntry *>(UtilVaFromPfn(
                                   ept_pdt_entry->fields.physial_address)),
                               table_level - 1, physical_address);
    }
    case 1: {
      // table == PT
      const auto pte_index = EptpAddressToPteIndex(physical_address);
      const auto ept_pt_entry = &table[pte_index];
      return ept_pt_entry;
    }
    default:
      HYPERPLATFORM_COMMON_DBG_BREAK();
      return nullptr;
  }
}

// Frees all EPT stuff
_Use_decl_annotations_ void EptTermination(EptData *ept_data) {
  HYPERPLATFORM_LOG_DEBUG("Used pre-allocated entries  = %2d / %2d",
                          ept_data->preallocated_entries_count,
                          kVmxpNumberOfPreallocatedEntries);

  EptpFreeUnusedPreAllocatedEntries(ept_data->preallocated_entries,
                                    ept_data->preallocated_entries_count);
  EptpDestructTables(ept_data->ept_pml4, 4);
  ExFreePoolWithTag(ept_data->ept_pointer, kHyperPlatformCommonPoolTag);
  ExFreePoolWithTag(ept_data, kHyperPlatformCommonPoolTag);
}

// Frees all unused pre-allocated EPT entries. Other used entries should be
// freed with EptpDestructTables().
_Use_decl_annotations_ static void EptpFreeUnusedPreAllocatedEntries(
    EptCommonEntry **preallocated_entries, long used_count) {
  for (auto i = used_count; i < kVmxpNumberOfPreallocatedEntries; ++i) {
    if (!preallocated_entries[i]) {
      break;
    }
#pragma warning(push)
#pragma warning(disable : 6001)
    ExFreePoolWithTag(preallocated_entries[i], kHyperPlatformCommonPoolTag);
#pragma warning(pop)
  }
  ExFreePoolWithTag(preallocated_entries, kHyperPlatformCommonPoolTag);
}

// Frees all used EPT entries by walking through whole EPT
_Use_decl_annotations_ static void EptpDestructTables(EptCommonEntry *table,
                                                      ULONG table_level) {
  for (auto i = 0ul; i < 512; ++i) {
    const auto entry = table[i];
    if (entry.fields.physial_address) {
      const auto sub_table = reinterpret_cast<EptCommonEntry *>(
          UtilVaFromPfn(entry.fields.physial_address));

      switch (table_level) {
        case 4:  // table == PML4, sub_table == PDPT
        case 3:  // table == PDPT, sub_table == PDT
          EptpDestructTables(sub_table, table_level - 1);
          break;
        case 2:  // table == PDT, sub_table == PT
          ExFreePoolWithTag(sub_table, kHyperPlatformCommonPoolTag);
          break;
        default:
          HYPERPLATFORM_COMMON_DBG_BREAK();
          break;
      }
    }
  }
  ExFreePoolWithTag(table, kHyperPlatformCommonPoolTag);
}

}  // extern "C"
