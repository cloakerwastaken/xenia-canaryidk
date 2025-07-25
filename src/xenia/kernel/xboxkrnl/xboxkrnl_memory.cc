/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xboxkrnl/xboxkrnl_memory.h"
#include "xenia/base/logging.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"
#include "xenia/xbox.h"

DEFINE_bool(
    ignore_offset_for_ranged_allocations, false,
    "Allows to ignore 4k offset for physical allocations with provided range. "
    "Certain titles check if result matches provided lower range.",
    "Memory");

namespace xe {
namespace kernel {
namespace xboxkrnl {

uint32_t ToXdkProtectFlags(uint32_t protect) {
  uint32_t result = 0;
  if (!(protect & kMemoryProtectRead) && !(protect & kMemoryProtectWrite)) {
    result = X_PAGE_NOACCESS;
  } else if ((protect & kMemoryProtectRead) &&
             !(protect & kMemoryProtectWrite)) {
    result = X_PAGE_READONLY;
  } else {
    result = X_PAGE_READWRITE;
  }
  if (protect & kMemoryProtectNoCache) {
    result |= X_PAGE_NOCACHE;
  }
  if (protect & kMemoryProtectWriteCombine) {
    result |= X_PAGE_WRITECOMBINE;
  }
  return result;
}

uint32_t FromXdkProtectFlags(uint32_t protect) {
  uint32_t result = 0;
  if ((protect & X_PAGE_READONLY) | (protect & X_PAGE_EXECUTE_READ)) {
    result = kMemoryProtectRead;
  } else if ((protect & X_PAGE_READWRITE) |
             (protect & X_PAGE_EXECUTE_READWRITE)) {
    result = kMemoryProtectRead | kMemoryProtectWrite;
  }
  if (protect & X_PAGE_NOCACHE) {
    result |= kMemoryProtectNoCache;
  }
  if (protect & X_PAGE_WRITECOMBINE) {
    result |= kMemoryProtectWriteCombine;
  }
  return result;
}

dword_result_t NtAllocateVirtualMemory_entry(lpdword_t base_addr_ptr,
                                             lpdword_t region_size_ptr,
                                             dword_t alloc_type,
                                             dword_t protect_bits,
                                             dword_t debug_memory) {
  // NTSTATUS
  // _Inout_  PVOID *BaseAddress,
  // _Inout_  PSIZE_T RegionSize,
  // _In_     ULONG AllocationType,
  // _In_     ULONG Protect
  // _In_     BOOLEAN DebugMemory

  assert_not_null(base_addr_ptr);
  assert_not_null(region_size_ptr);

  // Set to TRUE when allocation is from devkit memory area.
  assert_true(debug_memory == 0);

  // This allocates memory from the kernel heap, which is initialized on startup
  // and shared by both the kernel implementation and user code.
  // The xe_memory_ref object is used to actually get the memory, and although
  // it's simple today we could extend it to do better things in the future.

  // Must request a size.
  if (!base_addr_ptr || !region_size_ptr || !*region_size_ptr) {
    return X_STATUS_INVALID_PARAMETER;
  }
  // Check allocation type.
  if (!(alloc_type & (X_MEM_COMMIT | X_MEM_RESET | X_MEM_RESERVE))) {
    return X_STATUS_INVALID_PARAMETER;
  }
  // If MEM_RESET is set only MEM_RESET can be set.
  if (alloc_type & X_MEM_RESET && (alloc_type & ~X_MEM_RESET)) {
    return X_STATUS_INVALID_PARAMETER;
  }
  // Don't allow games to set execute bits.
  if (protect_bits & (X_PAGE_EXECUTE | X_PAGE_EXECUTE_READ |
                      X_PAGE_EXECUTE_READWRITE | X_PAGE_EXECUTE_WRITECOPY)) {
    XELOGW("Game setting EXECUTE bit on allocation");
  }

  uint32_t page_size;
  if (*base_addr_ptr != 0) {
    // ignore specified page size when base address is specified.
    auto heap = kernel_memory()->LookupHeap(*base_addr_ptr);
    // Edge case when title can check for XPS/MMIO range and will receive
    // nullptr.
    if (!heap) {
      // Code returned in this case is unknown but probably this one.
      return X_STATUS_INVALID_PARAMETER;
    }

    if (heap->heap_type() != HeapType::kGuestVirtual) {
      return X_STATUS_INVALID_PARAMETER;
    }
    page_size = heap->page_size();
  } else {
    // Adjust size.
    page_size = 4 * 1024;
    if (alloc_type & X_MEM_LARGE_PAGES) {
      page_size = 64 * 1024;
    }
  }

  // Round the base address down to the nearest page boundary.
  uint32_t adjusted_base = *base_addr_ptr - (*base_addr_ptr % page_size);
  // For some reason, some games pass in negative sizes.
  uint32_t adjusted_size = int32_t(*region_size_ptr) < 0
                               ? -int32_t(region_size_ptr.value())
                               : region_size_ptr.value();

  adjusted_size =
      xe::round_up(adjusted_size, adjusted_base ? page_size : 64 * 1024);

  // Allocate.
  uint32_t allocation_type = 0;
  if (alloc_type & X_MEM_RESERVE) {
    allocation_type |= kMemoryAllocationReserve;
  }
  if (alloc_type & X_MEM_COMMIT) {
    allocation_type |= kMemoryAllocationCommit;
  }
  if (alloc_type & X_MEM_RESET) {
    XELOGE("X_MEM_RESET not implemented");
    assert_always();
  }
  uint32_t protect = FromXdkProtectFlags(protect_bits);
  uint32_t address = 0;
  BaseHeap* heap;
  HeapAllocationInfo prev_alloc_info = {};
  bool was_commited = false;

  if (adjusted_base != 0) {
    heap = kernel_memory()->LookupHeap(adjusted_base);
    if (heap->page_size() != page_size) {
      // Specified the wrong page size for the wrong heap.
      return X_STATUS_ACCESS_DENIED;
    }
    was_commited = heap->QueryRegionInfo(adjusted_base, &prev_alloc_info) &&
                   (prev_alloc_info.state & kMemoryAllocationCommit) != 0;

    if (heap->AllocFixed(adjusted_base, adjusted_size, page_size,
                         allocation_type, protect)) {
      address = adjusted_base;
    }
  } else {
    bool top_down = !!(alloc_type & X_MEM_TOP_DOWN);
    heap = kernel_memory()->LookupHeapByType(false, page_size);
    heap->Alloc(adjusted_size, page_size, allocation_type, protect, top_down,
                &address);
  }
  if (!address) {
    // Failed - assume no memory available.
    return X_STATUS_NO_MEMORY;
  }

  // Zero memory, if needed.
  if (address && !(alloc_type & X_MEM_NOZERO)) {
    if (alloc_type & X_MEM_COMMIT) {
      if (!(protect & kMemoryProtectWrite)) {
        heap->Protect(address, adjusted_size,
                      kMemoryProtectRead | kMemoryProtectWrite);
      }
      if (!was_commited) {
        kernel_memory()->Zero(address, adjusted_size);
      }
      if (!(protect & kMemoryProtectWrite)) {
        heap->Protect(address, adjusted_size, protect);
      }
    }
  }

  XELOGD("NtAllocateVirtualMemory = {:08X}", address);

  // Stash back.
  // Maybe set X_STATUS_ALREADY_COMMITTED if MEM_COMMIT?
  *base_addr_ptr = address;
  *region_size_ptr = adjusted_size;
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(NtAllocateVirtualMemory, kMemory, kImplemented);

dword_result_t NtProtectVirtualMemory_entry(lpdword_t base_addr_ptr,
                                            lpdword_t region_size_ptr,
                                            dword_t protect_bits,
                                            lpdword_t old_protect,
                                            dword_t debug_memory) {
  // Set to TRUE when this memory refers to devkit memory area.
  assert_true(debug_memory == 0);

  // Must request a size.
  if (!base_addr_ptr || !region_size_ptr || !*region_size_ptr) {
    return X_STATUS_INVALID_PARAMETER;
  }

  // Don't allow games to set execute bits.
  if (protect_bits & (X_PAGE_EXECUTE | X_PAGE_EXECUTE_READ |
                      X_PAGE_EXECUTE_READWRITE | X_PAGE_EXECUTE_WRITECOPY)) {
    XELOGW("Game setting EXECUTE bit on protect");
    return X_STATUS_INVALID_PAGE_PROTECTION;
  }

  auto heap = kernel_memory()->LookupHeap(*base_addr_ptr);
  if (heap->heap_type() != HeapType::kGuestVirtual) {
    return X_STATUS_INVALID_PARAMETER;
  }
  // Adjust the base downwards to the nearest page boundary.
  uint32_t adjusted_base =
      *base_addr_ptr - (*base_addr_ptr % heap->page_size());
  uint32_t adjusted_size = xe::round_up(*region_size_ptr, heap->page_size());
  uint32_t protect = FromXdkProtectFlags(protect_bits);

  uint32_t tmp_old_protect = 0;

  // FIXME: I think it's valid for NtProtectVirtualMemory to span regions, but
  // as of now our implementation will fail in this case. Need to verify.
  if (!heap->Protect(adjusted_base, adjusted_size, protect, &tmp_old_protect)) {
    return X_STATUS_ACCESS_DENIED;
  }

  // Write back output variables.
  *base_addr_ptr = adjusted_base;
  *region_size_ptr = adjusted_size;

  if (old_protect) {
    *old_protect = tmp_old_protect;
  }

  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(NtProtectVirtualMemory, kMemory, kImplemented);

dword_result_t NtFreeVirtualMemory_entry(lpdword_t base_addr_ptr,
                                         lpdword_t region_size_ptr,
                                         dword_t free_type,
                                         dword_t debug_memory) {
  uint32_t base_addr_value = *base_addr_ptr;
  uint32_t region_size_value = *region_size_ptr;
  // X_MEM_DECOMMIT | X_MEM_RELEASE

  // NTSTATUS
  // _Inout_  PVOID *BaseAddress,
  // _Inout_  PSIZE_T RegionSize,
  // _In_     ULONG FreeType
  // _In_     BOOLEAN DebugMemory

  // Set to TRUE when freeing external devkit memory.
  assert_true(debug_memory == 0);

  if (!base_addr_value) {
    return X_STATUS_MEMORY_NOT_ALLOCATED;
  }

  auto heap = kernel_state()->memory()->LookupHeap(base_addr_value);
  if (heap->heap_type() != HeapType::kGuestVirtual) {
    return X_STATUS_INVALID_PARAMETER;
  }
  bool result = false;
  if (free_type == X_MEM_DECOMMIT) {
    // If zero, we may need to query size (free whole region).
    assert_not_zero(region_size_value);

    region_size_value = xe::round_up(region_size_value, heap->page_size());
    result = heap->Decommit(base_addr_value, region_size_value);
  } else {
    result = heap->Release(base_addr_value, &region_size_value);
  }
  if (!result) {
    return X_STATUS_UNSUCCESSFUL;
  }

  *base_addr_ptr = base_addr_value;
  *region_size_ptr = region_size_value;
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(NtFreeVirtualMemory, kMemory, kImplemented);

struct X_MEMORY_BASIC_INFORMATION {
  be<uint32_t> base_address;
  be<uint32_t> allocation_base;
  be<uint32_t> allocation_protect;
  be<uint32_t> region_size;
  be<uint32_t> state;
  be<uint32_t> protect;
  be<uint32_t> type;
};
// chrispy: added region_type ? guessed name, havent seen any except 0 used
dword_result_t NtQueryVirtualMemory_entry(
    dword_t base_address,
    pointer_t<X_MEMORY_BASIC_INFORMATION> memory_basic_information_ptr,
    dword_t region_type) {
  switch (region_type) {
    case 0:
    case 1:
    case 2:
      break;
    default:
      return X_STATUS_INVALID_PARAMETER;
  }
  auto heap = kernel_state()->memory()->LookupHeap(base_address);
  HeapAllocationInfo alloc_info;
  if (heap == nullptr || !heap->QueryRegionInfo(base_address, &alloc_info)) {
    return X_STATUS_INVALID_PARAMETER;
  }

  memory_basic_information_ptr->base_address = alloc_info.base_address;
  memory_basic_information_ptr->allocation_base = alloc_info.allocation_base;
  memory_basic_information_ptr->allocation_protect =
      ToXdkProtectFlags(alloc_info.allocation_protect);
  memory_basic_information_ptr->region_size = alloc_info.region_size;
  // https://docs.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-memory_basic_information
  // State: ... This member can be one of the following values: MEM_COMMIT,
  // MEM_FREE, MEM_RESERVE.
  // State queried by Beautiful Katamari before displaying the loading screen.
  uint32_t x_state;
  if (alloc_info.state & kMemoryAllocationCommit) {
    assert_not_zero(alloc_info.state & kMemoryAllocationReserve);
    x_state = X_MEM_COMMIT;
  } else if (alloc_info.state & kMemoryAllocationReserve) {
    x_state = X_MEM_RESERVE;
  } else {
    x_state = X_MEM_FREE;
  }
  memory_basic_information_ptr->state = x_state;
  memory_basic_information_ptr->protect = ToXdkProtectFlags(alloc_info.protect);
  memory_basic_information_ptr->type = X_MEM_PRIVATE;

  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(NtQueryVirtualMemory, kMemory, kImplemented);

dword_result_t NtAllocateEncryptedMemory_entry(dword_t unk, dword_t region_size,
                                               lpdword_t base_addr_ptr) {
  if (!region_size) {
    return X_STATUS_INVALID_PARAMETER;
  }

  const uint32_t region_size_adjusted =
      xe::round_up(region_size, 64 * 1024, true);

  if (region_size_adjusted > 16 * 1024 * 1024) {
    return X_STATUS_INVALID_PARAMETER;
  }

  uint32_t out_address = 0;
  auto heap = kernel_memory()->LookupHeap(0x8C000000);
  const bool result =
      heap->AllocRange(0x8C000000, 0x8FFFFFFF, region_size_adjusted, 64 * 1024,
                       MemoryAllocationFlag::kMemoryAllocationCommit,
                       MemoryProtectFlag::kMemoryProtectRead |
                           MemoryProtectFlag::kMemoryProtectWrite,
                       false, &out_address);

  if (!result) {
    return X_STATUS_UNSUCCESSFUL;
  }

  XELOGD("NtAllocateEncryptedMemory = {:08X}", out_address);
  *base_addr_ptr = out_address;
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(NtAllocateEncryptedMemory, kMemory, kImplemented);

dword_result_t NtFreeEncryptedMemory_entry(dword_t region_type,
                                           lpdword_t base_address_ptr) {
  if (!base_address_ptr) {
    return X_STATUS_INVALID_PARAMETER;
  }

  auto heap = kernel_state()->memory()->LookupHeap(0x80000000);
  const uint32_t encrypt_address =
      heap->heap_base() + heap->page_size() * (*base_address_ptr);

  auto encrypt_heap = kernel_state()->memory()->LookupHeap(encrypt_address);

  if (encrypt_heap->heap_type() != HeapType::kGuestXex) {
    return X_STATUS_INVALID_PARAMETER;
  }

  kernel_state()->memory()->SystemHeapFree(encrypt_address);

  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(NtFreeEncryptedMemory, kMemory, kImplemented);

uint32_t xeMmAllocatePhysicalMemoryEx(uint32_t flags, uint32_t region_size,
                                      uint32_t protect_bits,
                                      uint32_t min_addr_range,
                                      uint32_t max_addr_range,
                                      uint32_t alignment) {
  // Type will usually be 0 (user request?), where 1 and 2 are sometimes made
  // by D3D/etc.

  // Check protection bits.
  if (!(protect_bits & (X_PAGE_READONLY | X_PAGE_READWRITE))) {
    XELOGE("MmAllocatePhysicalMemoryEx: bad protection bits");
    return 0;
  }

  // Either may be OR'ed into protect_bits:
  // X_PAGE_NOCACHE
  // X_PAGE_WRITECOMBINE
  // We could use this to detect what's likely GPU-synchronized memory
  // and let the GPU know we're messing with it (or even allocate from
  // the GPU). At least the D3D command buffer is X_PAGE_WRITECOMBINE.

  // Calculate page size.
  // Default            = 4KB
  // X_MEM_LARGE_PAGES  = 64KB
  // X_MEM_16MB_PAGES   = 16MB
  uint32_t page_size = 4 * 1024;
  if (protect_bits & X_MEM_LARGE_PAGES) {
    page_size = 64 * 1024;
  } else if (protect_bits & X_MEM_16MB_PAGES) {
    page_size = 16 * 1024 * 1024;
  }

  // Round up the region size and alignment to the next page.
  uint32_t adjusted_size = xe::round_up(region_size, page_size);
  uint32_t adjusted_alignment = xe::round_up(alignment, page_size);

  uint32_t allocation_type = kMemoryAllocationReserve | kMemoryAllocationCommit;
  uint32_t protect = FromXdkProtectFlags(protect_bits);
  bool top_down = true;
  auto heap = static_cast<PhysicalHeap*>(
      kernel_memory()->LookupHeapByType(true, page_size));
  // min_addr_range/max_addr_range are bounds in physical memory, not virtual.
  uint32_t heap_base = heap->heap_base();
  uint32_t heap_physical_address_offset = heap->GetPhysicalAddress(heap_base);
  // TODO(Gliniak): Games like 545108B4 compares min_addr_range with value
  // returned. 0x1000 offset causes it to go below that minimal range and goes
  // haywire
  if (min_addr_range && max_addr_range &&
      cvars::ignore_offset_for_ranged_allocations) {
    heap_physical_address_offset = 0;
  }

  uint32_t heap_min_addr =
      xe::sat_sub(min_addr_range, heap_physical_address_offset);
  uint32_t heap_max_addr =
      xe::sat_sub(max_addr_range, heap_physical_address_offset);
  uint32_t heap_size = heap->heap_size();
  heap_min_addr = heap_base + std::min(heap_min_addr, heap_size - 1);
  heap_max_addr = heap_base + std::min(heap_max_addr, heap_size - 1);
  uint32_t base_address;
  if (!heap->AllocRange(heap_min_addr, heap_max_addr, adjusted_size,
                        adjusted_alignment, allocation_type, protect, top_down,
                        &base_address)) {
    // Failed - assume no memory available.
    XELOGW("MmAllocatePhysicalMemoryEx: Allocation failed: {:08X} Size: {:08X}",
           base_address, adjusted_size);
    return 0;
  }
  XELOGD("MmAllocatePhysicalMemoryEx = {:08X} Size: {:08X}", base_address,
         adjusted_size);

  return base_address;
}

dword_result_t MmAllocatePhysicalMemoryEx_entry(
    dword_t flags, dword_t region_size, dword_t protect_bits,
    dword_t min_addr_range, dword_t max_addr_range, dword_t alignment) {
  return xeMmAllocatePhysicalMemoryEx(flags, region_size, protect_bits,
                                      min_addr_range, max_addr_range,
                                      alignment);
}
DECLARE_XBOXKRNL_EXPORT1(MmAllocatePhysicalMemoryEx, kMemory, kImplemented);

dword_result_t MmAllocatePhysicalMemory_entry(dword_t flags,
                                              dword_t region_size,
                                              dword_t protect_bits) {
  return xeMmAllocatePhysicalMemoryEx(flags, region_size, protect_bits, 0,
                                      0xFFFFFFFFu, 0);
}
DECLARE_XBOXKRNL_EXPORT1(MmAllocatePhysicalMemory, kMemory, kImplemented);

void MmFreePhysicalMemory_entry(dword_t type, dword_t base_address) {
  // base_address = result of MmAllocatePhysicalMemory.

  assert_true((base_address & 0x1F) == 0);

  auto heap = kernel_state()->memory()->LookupHeap(base_address);
  heap->Release(base_address);
}
DECLARE_XBOXKRNL_EXPORT1(MmFreePhysicalMemory, kMemory, kImplemented);

dword_result_t MmQueryAddressProtect_entry(dword_t base_address) {
  auto heap = kernel_state()->memory()->LookupHeap(base_address);
  uint32_t access;
  if (!heap->QueryProtect(base_address, &access)) {
    access = 0;
  }
  access = !access ? 0 : ToXdkProtectFlags(access);

  return access;
}
DECLARE_XBOXKRNL_EXPORT2(MmQueryAddressProtect, kMemory, kImplemented,
                         kHighFrequency);

void MmSetAddressProtect_entry(lpvoid_t base_address, dword_t region_size,
                               dword_t protect_bits) {
  constexpr uint32_t required_protect_bits =
      X_PAGE_NOACCESS | X_PAGE_READONLY | X_PAGE_READWRITE |
      X_PAGE_EXECUTE_READ | X_PAGE_EXECUTE_READWRITE;

  if (xe::bit_count(protect_bits & required_protect_bits) != 1) {
    // Many titles use invalid combination with zero valid bits set.
    // We're skipping assertion for these cases to prevent unnecessary spam.
    assert_false(xe::bit_count(protect_bits & required_protect_bits) > 1);
    return;
  }

  uint32_t protect = FromXdkProtectFlags(protect_bits);
  auto heap = kernel_memory()->LookupHeap(base_address);

  // More research required: 544307D1 uses it with base_address in xex range,
  // which causes write exception in long term. Probably console disables
  // modification of xex range page protection for security reasons.
  if (heap->heap_type() == HeapType::kGuestXex) {
    return;
  }

  heap->Protect(base_address.guest_address(), region_size, protect);
}
DECLARE_XBOXKRNL_EXPORT1(MmSetAddressProtect, kMemory, kImplemented);

dword_result_t MmQueryAllocationSize_entry(lpvoid_t base_address) {
  auto heap = kernel_state()->memory()->LookupHeap(base_address);
  uint32_t size;
  if (!heap->QuerySize(base_address, &size)) {
    size = 0;
  }

  return size;
}
DECLARE_XBOXKRNL_EXPORT1(MmQueryAllocationSize, kMemory, kImplemented);

// https://code.google.com/p/vdash/source/browse/trunk/vdash/include/kernel.h
struct X_MM_QUERY_STATISTICS_SECTION {
  xe::be<uint32_t> available_pages;
  xe::be<uint32_t> total_virtual_memory_bytes;
  xe::be<uint32_t> reserved_virtual_memory_bytes;
  xe::be<uint32_t> physical_pages;
  xe::be<uint32_t> pool_pages;
  xe::be<uint32_t> stack_pages;
  xe::be<uint32_t> image_pages;
  xe::be<uint32_t> heap_pages;
  xe::be<uint32_t> virtual_pages;
  xe::be<uint32_t> page_table_pages;
  xe::be<uint32_t> cache_pages;
};

struct X_MM_QUERY_STATISTICS_RESULT {
  xe::be<uint32_t> size;
  xe::be<uint32_t> total_physical_pages;
  xe::be<uint32_t> kernel_pages;
  X_MM_QUERY_STATISTICS_SECTION title;
  X_MM_QUERY_STATISTICS_SECTION system;
  xe::be<uint32_t> highest_physical_page;
};
static_assert_size(X_MM_QUERY_STATISTICS_RESULT, 104);

dword_result_t MmQueryStatistics_entry(
    pointer_t<X_MM_QUERY_STATISTICS_RESULT> stats_ptr) {
  if (!stats_ptr) {
    return X_STATUS_INVALID_PARAMETER;
  }

  const uint32_t size = sizeof(X_MM_QUERY_STATISTICS_RESULT);

  if (stats_ptr->size != size) {
    return X_STATUS_BUFFER_TOO_SMALL;
  }

  // Zero out the struct.
  stats_ptr.Zero();

  // Set the constants the game is likely asking for.
  // These numbers are mostly guessed. If the game is just checking for
  // memory, this should satisfy it. If it's actually verifying things
  // this won't work :/
  stats_ptr->size = size;

  stats_ptr->total_physical_pages = 0x00020000;  // 512mb / 4kb pages
  stats_ptr->kernel_pages = 0x00000100;          // Previous value 0x300

  uint32_t reserved_pages = 0;
  uint32_t unreserved_pages = 0;
  uint32_t used_pages = 0;
  uint32_t reserved_pages_bytes = 0;
  const BaseHeap* physical_heaps[3] = {
      kernel_memory()->LookupHeapByType(true, 0x1000),
      kernel_memory()->LookupHeapByType(true, 0x10000),
      kernel_memory()->LookupHeapByType(true, 0x1000000)};

  kernel_memory()->GetHeapsPageStatsSummary(
      physical_heaps, std::size(physical_heaps), reserved_pages,
      unreserved_pages, used_pages, reserved_pages_bytes);

  assert_true(used_pages < stats_ptr->total_physical_pages);
  stats_ptr->title.available_pages =
      stats_ptr->total_physical_pages - stats_ptr->kernel_pages - used_pages;
  stats_ptr->title.total_virtual_memory_bytes = 0x2FFE0000;
  stats_ptr->title.reserved_virtual_memory_bytes = reserved_pages_bytes;
  stats_ptr->title.physical_pages = 0x00001000;  // TODO(gibbed): FIXME
  stats_ptr->title.pool_pages = 0x00000010;
  stats_ptr->title.stack_pages = 0x00000100;
  stats_ptr->title.image_pages = 0x00000100;
  stats_ptr->title.heap_pages = 0x00000100;
  stats_ptr->title.virtual_pages = 0x00000100;
  stats_ptr->title.page_table_pages = 0x00000100;
  stats_ptr->title.cache_pages = 0x00000100;

  stats_ptr->system.available_pages = 0x00000000;
  stats_ptr->system.total_virtual_memory_bytes = 0x00000000;
  stats_ptr->system.reserved_virtual_memory_bytes = 0x00000000;
  stats_ptr->system.physical_pages = 0x00000000;
  stats_ptr->system.pool_pages = 0x00000000;
  stats_ptr->system.stack_pages = 0x00000000;
  stats_ptr->system.image_pages = 0x00000000;
  stats_ptr->system.heap_pages = 0x00000000;
  stats_ptr->system.virtual_pages = 0x00000000;
  stats_ptr->system.page_table_pages = 0x00000000;
  stats_ptr->system.cache_pages = 0x00000000;

  stats_ptr->highest_physical_page = 0x0001FFFF;

  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT2(MmQueryStatistics, kMemory, kImplemented,
                         kHighFrequency);

// https://msdn.microsoft.com/en-us/library/windows/hardware/ff554547(v=vs.85).aspx
dword_result_t MmGetPhysicalAddress_entry(dword_t base_address) {
  // PHYSICAL_ADDRESS MmGetPhysicalAddress(
  //   _In_  PVOID BaseAddress
  // );
  // base_address = result of MmAllocatePhysicalMemory.
  uint32_t physical_address = kernel_memory()->GetPhysicalAddress(base_address);
  assert_true(physical_address != UINT32_MAX);
  if (physical_address == UINT32_MAX) {
    physical_address = 0;
  }
  return physical_address;
}
DECLARE_XBOXKRNL_EXPORT1(MmGetPhysicalAddress, kMemory, kImplemented);

dword_result_t MmMapIoSpace_entry(dword_t unk0, lpvoid_t src_address,
                                  dword_t size, dword_t flags) {
  // I've only seen this used to map XMA audio contexts.
  // The code seems fine with taking the src address, so this just returns that.
  // If others start using it there could be problems.
  assert_true(unk0 == 2);
  assert_true(size == 0x40);
  assert_true(flags == 0x404);

  return src_address.guest_address();
}
DECLARE_XBOXKRNL_EXPORT1(MmMapIoSpace, kMemory, kImplemented);

struct X_POOL_ALLOC_HEADER {
  uint8_t unk_0;
  uint8_t unk_1;
  uint8_t unk_2;  // set this to 170
  uint8_t unk_3;
  xe::be<uint32_t> tag;
};

uint32_t xeAllocatePoolTypeWithTag(PPCContext* context, uint32_t size,
                                   uint32_t tag, uint32_t zero) {
  if (size <= 0xFD8) {
    uint32_t adjusted_size = size + sizeof(X_POOL_ALLOC_HEADER);

    uint32_t addr =
        kernel_state()->memory()->SystemHeapAlloc(adjusted_size, 64);

    auto result_ptr = context->TranslateVirtual<X_POOL_ALLOC_HEADER*>(addr);
    result_ptr->unk_2 = 170;
    result_ptr->tag = tag;

    return addr + sizeof(X_POOL_ALLOC_HEADER);
  } else {
    return kernel_state()->memory()->SystemHeapAlloc(size, 4096);
  }
}

dword_result_t ExAllocatePoolTypeWithTag_entry(dword_t size, dword_t tag,
                                               dword_t zero,
                                               const ppc_context_t& context) {
  return xeAllocatePoolTypeWithTag(context, size, tag, zero);
}
DECLARE_XBOXKRNL_EXPORT1(ExAllocatePoolTypeWithTag, kMemory, kImplemented);

dword_result_t ExAllocatePoolWithTag_entry(dword_t numbytes, dword_t tag,
                                           const ppc_context_t& context) {
  return xeAllocatePoolTypeWithTag(context, numbytes, tag, 0);
}
DECLARE_XBOXKRNL_EXPORT1(ExAllocatePoolWithTag, kMemory, kImplemented);

dword_result_t ExAllocatePool_entry(dword_t size,
                                    const ppc_context_t& context) {
  constexpr uint32_t none = 0x656E6F4E;  // 'None'
  return xeAllocatePoolTypeWithTag(context, size, none, 0);
}
DECLARE_XBOXKRNL_EXPORT1(ExAllocatePool, kMemory, kImplemented);

void xeFreePool(PPCContext* context, uint32_t base_address) {
  auto memory = context->kernel_state->memory();
  // if 4kb aligned, there is no pool header!
  if ((base_address & (4096 - 1)) == 0) {
    memory->SystemHeapFree(base_address);
  } else {
    memory->SystemHeapFree(base_address - sizeof(X_POOL_ALLOC_HEADER));
  }
}

void ExFreePool_entry(lpvoid_t base_address, const ppc_context_t& context) {
  xeFreePool(context, base_address.guest_address());
}
DECLARE_XBOXKRNL_EXPORT1(ExFreePool, kMemory, kImplemented);

// hv syscall 15, jumps into (bootloader function table??) alternative table ptr
// offset 224
// this is not a correct implementation. i just wanted to get it to return a
// value thats in the same range as the hv's values that kind of reflects the
// pages index and heap
dword_result_t KeGetImagePageTableEntry_entry(dword_t address,
                                              const ppc_context_t& ctx) {
  auto kernel_state = ctx->kernel_state;
  xe::BaseHeap* image_heap = kernel_state->memory()->LookupHeap(address);
  if (image_heap->heap_type() != HeapType::kGuestXex) {
    return 0;
  }
  uint32_t returned_value = address - image_heap->heap_base();

  // todo: its always a power of two, should shift
  returned_value /= image_heap->page_size();

  if (image_heap->page_size() < 65536) {
    returned_value |= 0x40000000;
  }

  return returned_value & 0x400FFFFF;  // this is actually the mask it applies
                                       // to the final
  // result before returning it
}
DECLARE_XBOXKRNL_EXPORT1(KeGetImagePageTableEntry, kMemory, kStub);

dword_result_t KeLockL2_entry() {
  // TODO
  return 0;
}
DECLARE_XBOXKRNL_EXPORT1(KeLockL2, kMemory, kStub);

void KeUnlockL2_entry() {}
DECLARE_XBOXKRNL_EXPORT1(KeUnlockL2, kMemory, kStub);

uint32_t xeMmCreateKernelStack(uint32_t stack_size, uint32_t r4) {
  auto stack_size_aligned = (stack_size + 0xFFF) & 0xFFFFF000;
  uint32_t stack_alignment = (stack_size & 0xF000) ? 0x1000 : 0x10000;

  uint32_t stack_address;
  kernel_memory()
      ->LookupHeap(0x70000000)
      ->AllocRange(0x70000000, 0x7F000000, stack_size_aligned, stack_alignment,
                   kMemoryAllocationReserve | kMemoryAllocationCommit,
                   kMemoryProtectRead | kMemoryProtectWrite, false,
                   &stack_address);
  return stack_address + stack_size;
}
dword_result_t MmCreateKernelStack_entry(dword_t stack_size, dword_t r4) {
  return xeMmCreateKernelStack(stack_size, r4);
}
DECLARE_XBOXKRNL_EXPORT1(MmCreateKernelStack, kMemory, kImplemented);

dword_result_t MmDeleteKernelStack_entry(lpvoid_t stack_base,
                                         lpvoid_t stack_end) {
  // Release the stack (where stack_end is the low address)
  if (kernel_memory()->LookupHeap(0x70000000)->Release(stack_end)) {
    return X_STATUS_SUCCESS;
  }

  return X_STATUS_UNSUCCESSFUL;
}
DECLARE_XBOXKRNL_EXPORT1(MmDeleteKernelStack, kMemory, kImplemented);

dword_result_t MmIsAddressValid_entry(dword_t address,
                                      const ppc_context_t& ctx) {
  auto kernel = ctx->kernel_state;
  auto memory = kernel->memory();
  auto heap = memory->LookupHeap(address);
  if (!heap) {
    return 0;
  }

  return heap->QueryRangeAccess(address, address) !=
         memory::PageAccess::kNoAccess;
}

DECLARE_XBOXKRNL_EXPORT1(MmIsAddressValid, kMemory, kImplemented);

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

DECLARE_XBOXKRNL_EMPTY_REGISTER_EXPORTS(Memory);
