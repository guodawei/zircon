// Copyright 2016 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <err.h>
#include <platform.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <trace.h>

#include <dev/interrupt.h>
#include <dev/iommu.h>
#include <dev/udisplay.h>
#include <vm/vm.h>
#include <vm/vm_object_paged.h>
#include <vm/vm_object_physical.h>
#include <lib/user_copy/user_ptr.h>
#include <object/bus_transaction_initiator_dispatcher.h>
#include <object/handle_owner.h>
#include <object/handles.h>
#include <object/interrupt_dispatcher.h>
#include <object/interrupt_event_dispatcher.h>
#include <object/iommu_dispatcher.h>
#include <object/process_dispatcher.h>
#include <object/resources.h>
#include <object/vm_object_dispatcher.h>
#include <zxcpp/new.h>

#if ARCH_X86
#include <platform/pc/bootloader.h>
#endif

#include <zircon/syscalls/iommu.h>
#include <zircon/syscalls/pci.h>
#include <fbl/auto_call.h>
#include <fbl/inline_array.h>

#include "syscalls_priv.h"

#define LOCAL_TRACE 0

static_assert(ZX_CACHE_POLICY_CACHED == ARCH_MMU_FLAG_CACHED,
              "Cache policy constant mismatch - CACHED");
static_assert(ZX_CACHE_POLICY_UNCACHED == ARCH_MMU_FLAG_UNCACHED,
              "Cache policy constant mismatch - UNCACHED");
static_assert(ZX_CACHE_POLICY_UNCACHED_DEVICE == ARCH_MMU_FLAG_UNCACHED_DEVICE,
              "Cache policy constant mismatch - UNCACHED_DEVICE");
static_assert(ZX_CACHE_POLICY_WRITE_COMBINING == ARCH_MMU_FLAG_WRITE_COMBINING,
              "Cache policy constant mismatch - WRITE_COMBINING");

zx_status_t sys_interrupt_create(zx_handle_t hrsrc, uint32_t vector, uint32_t options,
                                 user_out_ptr<zx_handle_t> out_handle) {
    LTRACEF("vector %u options 0x%x\n", vector, options);

    zx_status_t status;
    if ((status = validate_resource_irq(hrsrc, vector)) < 0) {
        return status;
    }

    fbl::RefPtr<Dispatcher> dispatcher;
    zx_rights_t rights;
    zx_status_t result = InterruptEventDispatcher::Create(vector, options, &dispatcher, &rights);
    if (result != ZX_OK)
        return result;

    HandleOwner handle(MakeHandle(fbl::move(dispatcher), rights));

    auto up = ProcessDispatcher::GetCurrent();
    zx_handle_t hv = up->MapHandleToValue(handle);

    if (out_handle.copy_to_user(hv) != ZX_OK) {
        return ZX_ERR_INVALID_ARGS;
    }

    up->AddHandle(fbl::move(handle));
    return ZX_OK;
}

zx_status_t sys_interrupt_complete(zx_handle_t handle_value) {
    LTRACEF("handle %x\n", handle_value);

    auto up = ProcessDispatcher::GetCurrent();
    fbl::RefPtr<InterruptDispatcher> interrupt;
    zx_status_t status = up->GetDispatcher(handle_value, &interrupt);
    if (status != ZX_OK)
        return status;

    return interrupt->InterruptComplete();
}

zx_status_t sys_interrupt_wait(zx_handle_t handle_value) {
    LTRACEF("handle %x\n", handle_value);

    auto up = ProcessDispatcher::GetCurrent();
    fbl::RefPtr<InterruptDispatcher> interrupt;
    zx_status_t status = up->GetDispatcher(handle_value, &interrupt);
    if (status != ZX_OK)
        return status;

    return interrupt->WaitForInterrupt();
}

zx_status_t sys_interrupt_signal(zx_handle_t handle_value) {
    LTRACEF("handle %x\n", handle_value);

    auto up = ProcessDispatcher::GetCurrent();
    fbl::RefPtr<InterruptDispatcher> interrupt;
    zx_status_t status = up->GetDispatcher(handle_value, &interrupt);
    if (status != ZX_OK)
        return status;

    return interrupt->UserSignal();
}

zx_status_t sys_vmo_create_contiguous(zx_handle_t hrsrc, size_t size,
                                      uint32_t alignment_log2,
                                      user_out_ptr<zx_handle_t> _out) {
    LTRACEF("size 0x%zu\n", size);

    if (size == 0) return ZX_ERR_INVALID_ARGS;
    if (alignment_log2 == 0)
        alignment_log2 = PAGE_SIZE_SHIFT;
    // catch obviously wrong values
    if (alignment_log2 < PAGE_SIZE_SHIFT ||
            alignment_log2 >= (8 * sizeof(uint64_t)))
        return ZX_ERR_INVALID_ARGS;

    // TODO(ZX-971): finer grained validation
    zx_status_t status;
    if ((status = validate_resource(hrsrc, ZX_RSRC_KIND_ROOT)) < 0) {
        return status;
    }

    size = ROUNDUP_PAGE_SIZE(size);
    // create a vm object
    fbl::RefPtr<VmObject> vmo;
    status = VmObjectPaged::Create(PMM_ALLOC_FLAG_ANY, size, &vmo);
    if (status != ZX_OK)
        return status;

    // always immediately commit memory to the object
    uint64_t committed;
    // CommitRangeContiguous takes a uint8_t for the alignment
    auto align_log2_arg = static_cast<uint8_t>(alignment_log2);
    status = vmo->CommitRangeContiguous(0, size, &committed, align_log2_arg);
    if (status < 0 || (size_t)committed < size) {
        LTRACEF("failed to allocate enough pages (asked for %zu, got %zu)\n", size / PAGE_SIZE,
                (size_t)committed / PAGE_SIZE);
        return ZX_ERR_NO_MEMORY;
    }

    // create a Vm Object dispatcher
    fbl::RefPtr<Dispatcher> dispatcher;
    zx_rights_t rights;
    zx_status_t result = VmObjectDispatcher::Create(fbl::move(vmo), &dispatcher, &rights);
    if (result != ZX_OK)
        return result;

    // create a handle and attach the dispatcher to it
    HandleOwner handle(MakeHandle(fbl::move(dispatcher), rights));
    if (!handle)
        return ZX_ERR_NO_MEMORY;

    auto up = ProcessDispatcher::GetCurrent();

    if (_out.copy_to_user(up->MapHandleToValue(handle)) != ZX_OK)
        return ZX_ERR_INVALID_ARGS;

    up->AddHandle(fbl::move(handle));
    return ZX_OK;
}

zx_status_t sys_vmo_create_physical(zx_handle_t hrsrc, uintptr_t paddr, size_t size,
                                    user_out_ptr<zx_handle_t> _out) {
    LTRACEF("size 0x%zu\n", size);

    // TODO: attempting to create a physical VMO that points to memory should be an error

    zx_status_t status;
    if ((status = validate_resource_mmio(hrsrc, paddr, size)) < 0) {
        return status;
    }

    size = ROUNDUP_PAGE_SIZE(size);

    // create a vm object
    fbl::RefPtr<VmObject> vmo;
    zx_status_t result = VmObjectPhysical::Create(paddr, size, &vmo);
    if (result != ZX_OK) {
        return result;
    }

    // create a Vm Object dispatcher
    fbl::RefPtr<Dispatcher> dispatcher;
    zx_rights_t rights;
    result = VmObjectDispatcher::Create(fbl::move(vmo), &dispatcher, &rights);
    if (result != ZX_OK)
        return result;

    // create a handle and attach the dispatcher to it
    HandleOwner handle(MakeHandle(fbl::move(dispatcher), rights));
    if (!handle)
        return ZX_ERR_NO_MEMORY;

    auto up = ProcessDispatcher::GetCurrent();

    if (_out.copy_to_user(up->MapHandleToValue(handle)) != ZX_OK)
        return ZX_ERR_INVALID_ARGS;

    up->AddHandle(fbl::move(handle));
    return ZX_OK;
}

zx_status_t sys_bootloader_fb_get_info(user_out_ptr<uint32_t> format, user_out_ptr<uint32_t> width,
                                       user_out_ptr<uint32_t> height, user_out_ptr<uint32_t> stride) {
#if ARCH_X86
    if (!bootloader.fb.base ||
            format.copy_to_user(bootloader.fb.format) ||
            width.copy_to_user(bootloader.fb.width) ||
            height.copy_to_user(bootloader.fb.height) ||
            stride.copy_to_user(bootloader.fb.stride)) {
        return ZX_ERR_INVALID_ARGS;
    } else {
        return ZX_OK;
    }
#else
    return ZX_ERR_NOT_SUPPORTED;
#endif
}

zx_status_t sys_set_framebuffer(zx_handle_t hrsrc, user_inout_ptr<void> vaddr, uint32_t len, uint32_t format, uint32_t width, uint32_t height, uint32_t stride) {
    // TODO(ZX-971): finer grained validation
    zx_status_t status;
    if ((status = validate_resource(hrsrc, ZX_RSRC_KIND_ROOT)) < 0) {
        return status;
    }

    intptr_t paddr = vaddr_to_paddr(vaddr.get());
    udisplay_set_framebuffer(paddr, len);

    struct display_info di;
    memset(&di, 0, sizeof(struct display_info));
    di.format = format;
    di.width = width;
    di.height = height;
    di.stride = stride;
    di.flags = DISPLAY_FLAG_HW_FRAMEBUFFER;
    udisplay_set_display_info(&di);

    return ZX_OK;
}

zx_status_t sys_set_framebuffer_vmo(zx_handle_t hrsrc, zx_handle_t vmo_handle, uint32_t len, uint32_t format, uint32_t width, uint32_t height, uint32_t stride) {
    zx_status_t status;
    if ((status = validate_resource(hrsrc, ZX_RSRC_KIND_ROOT)) < 0)
        return status;

    auto up = ProcessDispatcher::GetCurrent();

    // lookup the dispatcher from handle
    fbl::RefPtr<VmObjectDispatcher> vmo;
    status = up->GetDispatcher(vmo_handle, &vmo);
    if (status != ZX_OK)
        return status;

    status = udisplay_set_framebuffer_vmo(vmo->vmo());
    if (status != ZX_OK)
        return status;

    struct display_info di;
    memset(&di, 0, sizeof(struct display_info));
    di.format = format;
    di.width = width;
    di.height = height;
    di.stride = stride;
    di.flags = DISPLAY_FLAG_HW_FRAMEBUFFER;
    udisplay_set_display_info(&di);

    return ZX_OK;
}

zx_status_t sys_iommu_create(zx_handle_t rsrc_handle, uint32_t type, user_in_ptr<const void> desc,
                             uint32_t desc_len, user_out_ptr<zx_handle_t> out) {
    // TODO: finer grained validation
    zx_status_t status;
    if ((status = validate_resource(rsrc_handle, ZX_RSRC_KIND_ROOT)) < 0) {
        return status;
    }

    if (desc_len > ZX_IOMMU_MAX_DESC_LEN) {
        return ZX_ERR_INVALID_ARGS;
    }

    fbl::RefPtr<Dispatcher> dispatcher;
    zx_rights_t rights;

    {
        // Copy the descriptor into the kernel and try to create the dispatcher
        // using it.
        fbl::AllocChecker ac;
        fbl::unique_ptr<uint8_t[]> copied_desc(new (&ac) uint8_t[desc_len]);
        if (!ac.check()) {
            return ZX_ERR_NO_MEMORY;
        }
        if ((status = desc.copy_array_from_user(copied_desc.get(), desc_len)) != ZX_OK) {
            return status;
        }
        status = IommuDispatcher::Create(type,
                                         fbl::unique_ptr<const uint8_t[]>(copied_desc.release()),
                                         desc_len, &dispatcher, &rights);
        if (status != ZX_OK) {
            return status;
        }
    }

    HandleOwner handle(MakeHandle(fbl::move(dispatcher), rights));

    auto up = ProcessDispatcher::GetCurrent();
    if (out.copy_to_user(up->MapHandleToValue(handle)) != ZX_OK)
        return ZX_ERR_INVALID_ARGS;

    up->AddHandle(fbl::move(handle));
    return ZX_OK;
}

#if ARCH_X86
#include <arch/x86/descriptor.h>
#include <arch/x86/ioport.h>

zx_status_t sys_mmap_device_io(zx_handle_t hrsrc, uint32_t io_addr, uint32_t len) {
    // TODO(ZX-971): finer grained validation
    zx_status_t status;
    if ((status = validate_resource(hrsrc, ZX_RSRC_KIND_ROOT)) < 0) {
        return status;
    }

    LTRACEF("addr 0x%x len 0x%x\n", io_addr, len);

    return IoBitmap::GetCurrent().SetIoBitmap(io_addr, len, 1);
}
#else
zx_status_t sys_mmap_device_io(zx_handle_t hrsrc, uint32_t io_addr, uint32_t len) {
    // doesn't make sense on non-x86
    return ZX_ERR_NOT_SUPPORTED;
}
#endif

uint64_t sys_acpi_uefi_rsdp(zx_handle_t hrsrc) {
    // TODO(ZX-971): finer grained validation
    zx_status_t status;
    if ((status = validate_resource(hrsrc, ZX_RSRC_KIND_ROOT)) < 0) {
        return status;
    }
#if ARCH_X86
    return bootloader.acpi_rsdp;
#endif
    return 0;
}

zx_status_t sys_bti_create(zx_handle_t iommu, uint64_t bti_id, user_out_ptr<zx_handle_t> out) {
    auto up = ProcessDispatcher::GetCurrent();

    fbl::RefPtr<IommuDispatcher> iommu_dispatcher;
    // TODO(teisenbe): This should probably have a right on it.
    zx_status_t status = up->GetDispatcherWithRights(iommu, ZX_RIGHT_NONE, &iommu_dispatcher);
    if (status != ZX_OK) {
        return status;
    }

    fbl::RefPtr<Dispatcher> dispatcher;
    zx_rights_t rights;
    // TODO(teisenbe): Migrate BusTransactionInitiatorDispatcher::Create to
    // taking the iommu_dispatcher
    status = BusTransactionInitiatorDispatcher::Create(iommu_dispatcher->iommu(), bti_id,
                                                       &dispatcher, &rights);
    if (status != ZX_OK) {
        return status;
    }
    HandleOwner handle(MakeHandle(fbl::move(dispatcher), rights));

    zx_handle_t hv = up->MapHandleToValue(handle);
    if ((status = out.copy_to_user(hv)) != ZX_OK) {
        return status;
    }

    up->AddHandle(fbl::move(handle));
    return ZX_OK;
}

zx_status_t sys_bti_pin(zx_handle_t bti, zx_handle_t vmo, uint64_t offset, uint64_t size,
                        uint32_t perms, user_out_ptr<uint64_t> extents, uint32_t extents_len,
                        user_out_ptr<uint32_t> actual_extents_len) {
    auto up = ProcessDispatcher::GetCurrent();

    if (!IS_PAGE_ALIGNED(offset)) {
        return ZX_ERR_INVALID_ARGS;
    }

    fbl::RefPtr<BusTransactionInitiatorDispatcher> bti_dispatcher;
    zx_status_t status = up->GetDispatcherWithRights(bti, ZX_RIGHT_MAP, &bti_dispatcher);
    if (status != ZX_OK) {
        return status;
    }

    fbl::RefPtr<VmObjectDispatcher> vmo_dispatcher;
    zx_rights_t vmo_rights;
    status = up->GetDispatcherAndRights(vmo, &vmo_dispatcher, &vmo_rights);
    if (status != ZX_OK) {
        return status;
    }
    if (!(vmo_rights & ZX_RIGHT_MAP)) {
        return ZX_ERR_ACCESS_DENIED;
    }

    // Convert requested permissions and check against VMO rights
    uint32_t iommu_perms = 0;
    if (perms & ZX_VM_FLAG_PERM_READ) {
        if (!(vmo_rights & ZX_RIGHT_READ)) {
            return ZX_ERR_ACCESS_DENIED;
        }
        iommu_perms |= IOMMU_FLAG_PERM_READ;
        perms &= ~ZX_VM_FLAG_PERM_READ;
    }
    if (perms & ZX_VM_FLAG_PERM_WRITE) {
        if (!(vmo_rights & ZX_RIGHT_WRITE)) {
            return ZX_ERR_ACCESS_DENIED;
        }
        iommu_perms |= IOMMU_FLAG_PERM_WRITE;
        perms &= ~ZX_VM_FLAG_PERM_WRITE;
    }
    if (perms & ZX_VM_FLAG_PERM_EXECUTE) {
        if (!(vmo_rights & ZX_RIGHT_EXECUTE)) {
            return ZX_ERR_ACCESS_DENIED;
        }
        iommu_perms |= IOMMU_FLAG_PERM_EXECUTE;
        perms &= ~ZX_VM_FLAG_PERM_EXECUTE;
    }
    if (perms) {
        return ZX_ERR_INVALID_ARGS;
    }

    fbl::AllocChecker ac;
    fbl::InlineArray<dev_vaddr_t, 4u> mapped_extents(&ac, extents_len);
    if (!ac.check()) {
        return ZX_ERR_NO_MEMORY;
    }

    size_t actual_len;
    status = bti_dispatcher->Pin(vmo_dispatcher->vmo(), offset, size, iommu_perms,
                                 mapped_extents.get(), extents_len, &actual_len);
    if (status != ZX_OK) {
        return status;
    }

    auto pin_cleanup = fbl::MakeAutoCall([&bti_dispatcher, &mapped_extents, actual_len]() {
        bti_dispatcher->Unpin(mapped_extents.get(), actual_len);
    });

    static_assert(sizeof(dev_vaddr_t) == sizeof(uint64_t), "mismatched types");
    if ((status = extents.copy_array_to_user(mapped_extents.get(), actual_len)) != ZX_OK) {
        return status;
    }
    if ((status = actual_extents_len.copy_to_user(static_cast<uint32_t>(actual_len))) != ZX_OK) {
        return status;
    }

    pin_cleanup.cancel();
    return ZX_OK;
}

zx_status_t sys_bti_unpin(zx_handle_t bti, user_in_ptr<const uint64_t> extents,
                          uint32_t extents_len) {
    auto up = ProcessDispatcher::GetCurrent();

    fbl::RefPtr<BusTransactionInitiatorDispatcher> bti_dispatcher;
    zx_status_t status = up->GetDispatcherWithRights(bti, ZX_RIGHT_MAP, &bti_dispatcher);
    if (status != ZX_OK) {
        return status;
    }

    fbl::AllocChecker ac;
    fbl::InlineArray<dev_vaddr_t, 4u> mapped_extents(&ac, extents_len);
    if (!ac.check()) {
        return ZX_ERR_NO_MEMORY;
    }
    static_assert(sizeof(dev_vaddr_t) == sizeof(uint64_t), "mismatched types");
    if ((status = extents.copy_array_from_user(mapped_extents.get(), extents_len)) != ZX_OK) {
        return status;
    }

    return bti_dispatcher->Unpin(mapped_extents.get(), extents_len);

}
