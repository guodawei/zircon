// Copyright 2017 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#pragma once

#include <arch/arm64/el2_state.h>
#include <fbl/ref_ptr.h>
#include <fbl/unique_ptr.h>
#include <hypervisor/trap_map.h>
#include <kernel/event.h>
#include <zircon/types.h>

class GuestPhysicalAddressSpace;
class PortDispatcher;
class VmObject;

static const uint32_t kGichLrPending = 0b01 << 28;
static const uint32_t kGichHcrEn = 1u << 0;
static const uint32_t kGichVtrListRegs = 0b111111;

typedef struct zx_port_packet zx_port_packet_t;

class Guest {
public:
    static zx_status_t Create(fbl::RefPtr<VmObject> physmem, fbl::unique_ptr<Guest>* out);
    ~Guest();
    DISALLOW_COPY_ASSIGN_AND_MOVE(Guest);

    zx_status_t SetTrap(uint32_t kind, zx_vaddr_t addr, size_t len,
                        fbl::RefPtr<PortDispatcher> port, uint64_t key);

    GuestPhysicalAddressSpace* AddressSpace() const { return gpas_.get(); }
    TrapMap* Traps() { return &traps_; }
    uint8_t Vmid() const { return vmid_; }

private:
    fbl::unique_ptr<GuestPhysicalAddressSpace> gpas_;
    TrapMap traps_;
    const uint8_t vmid_;

    explicit Guest(uint8_t vmid);
};

struct GicH {
    volatile uint32_t hcr;
    volatile uint32_t vtr;
    volatile uint32_t vmcr;
    volatile uint32_t reserved0;
    volatile uint32_t misr;
    volatile uint32_t reserved1[3];
    volatile uint64_t eisr;
    volatile uint32_t reserved2[2];
    volatile uint64_t elsr;
    volatile uint32_t reserved3[46];
    volatile uint32_t apr;
    volatile uint32_t reserved4[3];
    volatile uint32_t lr[64];
} __PACKED;

static_assert(__offsetof(GicH, hcr) == 0x00, "");
static_assert(__offsetof(GicH, vtr) == 0x04, "");
static_assert(__offsetof(GicH, vmcr) == 0x08, "");
static_assert(__offsetof(GicH, misr) == 0x10, "");
static_assert(__offsetof(GicH, eisr) == 0x20, "");
static_assert(__offsetof(GicH, elsr) == 0x30, "");
static_assert(__offsetof(GicH, apr) == 0xf0, "");
static_assert(__offsetof(GicH, lr) == 0x100, "");

struct GicState {
    // Gic hypervisor control registers.
    GicH* gich;
    // Event for handling block on WFI.
    event_t event;
};

class Vcpu {
public:
    static zx_status_t Create(zx_vaddr_t ip, uint8_t vmid, GuestPhysicalAddressSpace* gpas,
                              TrapMap* traps, fbl::unique_ptr<Vcpu>* out);
    ~Vcpu();
    DISALLOW_COPY_ASSIGN_AND_MOVE(Vcpu);

    zx_status_t Resume(zx_port_packet_t* packet);
    zx_status_t Interrupt(uint32_t interrupt);
    zx_status_t ReadState(uint32_t kind, void* buffer, uint32_t len) const;
    zx_status_t WriteState(uint32_t kind, const void* buffer, uint32_t len);

private:
    const uint8_t vmid_;
    const uint8_t vpid_;
    const thread_t* thread_;
    GicState gic_state_;
    GuestPhysicalAddressSpace* gpas_;
    TrapMap* traps_;
    El2State el2_state_;
    fbl::atomic<uint64_t> hcr_;

    Vcpu(uint8_t vmid, uint8_t vpid, const thread_t* thread, GuestPhysicalAddressSpace* gpas,
         TrapMap* traps);
};

/* Create a guest. */
zx_status_t arch_guest_create(fbl::RefPtr<VmObject> physmem, fbl::unique_ptr<Guest>* guest);

/* Set a trap within a guest. */
zx_status_t arch_guest_set_trap(Guest* guest, uint32_t kind, zx_vaddr_t addr, size_t len,
                                fbl::RefPtr<PortDispatcher> port, uint64_t key);

/* Create a VCPU. */
zx_status_t arm_vcpu_create(zx_vaddr_t ip, uint8_t vmid, GuestPhysicalAddressSpace* gpas,
                            TrapMap* traps, fbl::unique_ptr<Vcpu>* out);

/* Resume execution of a VCPU. */
zx_status_t arch_vcpu_resume(Vcpu* vcpu, zx_port_packet_t* packet);

/* Issue an interrupt on a VCPU. */
zx_status_t arch_vcpu_interrupt(Vcpu* vcpu, uint32_t interrupt);

/* Read the register state of a VCPU. */
zx_status_t arch_vcpu_read_state(const Vcpu* vcpu, uint32_t kind, void* buffer, uint32_t len);

/* Write the register state of a VCPU. */
zx_status_t arch_vcpu_write_state(Vcpu* vcpu, uint32_t kind, const void* buffer, uint32_t len);
