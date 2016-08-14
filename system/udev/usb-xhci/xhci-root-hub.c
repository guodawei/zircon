// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stdio.h>

#include "xhci.h"

//#define TRACE 1
#include "xhci-debug.h"

static void xhci_reset_port(xhci_t* xhci, int port) {
    volatile uint32_t* portsc = &xhci->op_regs->port_regs[port - 1].portsc;
    uint32_t temp = XHCI_READ32(portsc);
    temp = (temp & PORTSC_CONTROL_BITS) | PORTSC_PR;
    XHCI_WRITE32(portsc, temp);
}

static void xhci_handle_port_connected(xhci_t* xhci, int port) {
    xprintf("xhci_handle_port_connected %d\n", port);
    xhci_reset_port(xhci, port);
}

static void xhci_handle_port_disconnected(xhci_t* xhci, int port) {
    xprintf("xhci_handle_port_disconnected %d\n", port);
    xhci_device_disconnected(xhci, 0, port);
}

static void xhci_handle_port_enabled(xhci_t* xhci, int port, int speed) {
    xprintf("xhci_handle_port_enabled %d speed: %d\n", port, speed);

    xhci_enumerate_device(xhci, 0, port, speed);
}

void xhci_handle_port_changed_event(xhci_t* xhci, xhci_trb_t* trb) {
    volatile xhci_port_regs_t* port_regs = xhci->op_regs->port_regs;
    uint32_t port = XHCI_GET_BITS32(&trb->ptr_low, EVT_TRB_PORT_ID_START, EVT_TRB_PORT_ID_BITS);
    uint32_t portsc = XHCI_READ32(&port_regs[port - 1].portsc);
    uint32_t speed = (portsc & XHCI_MASK(PORTSC_SPEED_START, PORTSC_SPEED_BITS)) >> PORTSC_SPEED_START;

    xprintf("xhci_handle_port_changed_event port: %d speed: %d\n", port, speed);

    uint32_t status_bits = portsc & PORTSC_STATUS_BITS;
    if (status_bits) {
        bool connected = !!(portsc & PORTSC_CCS);
        bool enabled = !!(portsc & PORTSC_PED);

        // set change bits to acknowledge
        XHCI_WRITE32(&port_regs[port - 1].portsc, (portsc & PORTSC_CONTROL_BITS) | status_bits);

        if (portsc & PORTSC_CSC) {
            // connect status change
            if (connected) {
                xhci_handle_port_connected(xhci, port);
            } else {
                xhci_handle_port_disconnected(xhci, port);
            }
        }
        if (portsc & PORTSC_PRC) {
            // port reset change
            if (enabled) {
                xhci_handle_port_enabled(xhci, port, speed);
            }
        }
    }
}
