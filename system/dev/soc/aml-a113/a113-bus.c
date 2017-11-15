// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <unistd.h>

#include <ddk/binding.h>
#include <ddk/debug.h>
#include <ddk/device.h>
#include <ddk/driver.h>
#include <ddk/protocol/platform-defs.h>

#include <zircon/assert.h>
#include <zircon/process.h>
#include <zircon/syscalls.h>

#include "a113-bus.h"
#include "a113-hw.h"
#include "aml-i2c.h"
#include "aml-tdm.h"
#include "gauss-hw.h"
#include <hw/reg.h>

static zx_status_t a113_get_initial_mode(void* ctx, usb_mode_t* out_mode) {
    *out_mode = USB_MODE_HOST;
    return ZX_OK;
}

static zx_status_t a113_set_mode(void* ctx, usb_mode_t mode) {
    a113_bus_t* bus = ctx;
    return a113_usb_set_mode(bus, mode);
}

usb_mode_switch_protocol_ops_t usb_mode_switch_ops = {
    .get_initial_mode = a113_get_initial_mode,
    .set_mode = a113_set_mode,
};

static zx_status_t a113_bus_get_protocol(void* ctx, uint32_t proto_id, void* out) {
    a113_bus_t* bus = ctx;

    switch (proto_id) {
    case ZX_PROTOCOL_USB_MODE_SWITCH:
        memcpy(out, &bus->usb_mode_switch, sizeof(bus->usb_mode_switch));
        return ZX_OK;
    case ZX_PROTOCOL_GPIO:
        memcpy(out, &bus->gpio, sizeof(bus->gpio));
        return ZX_OK;
    case ZX_PROTOCOL_I2C:
        memcpy(out, &bus->i2c, sizeof(bus->i2c));
        return ZX_OK;
    default:
        return ZX_ERR_NOT_SUPPORTED;
    }
}

static pbus_interface_ops_t a113_bus_bus_ops = {
    .get_protocol = a113_bus_get_protocol,
};

static void a113_bus_release(void* ctx) {
    a113_bus_t* bus = ctx;

    a113_gpio_release(bus);

    free(bus);
}

static zx_protocol_device_t a113_bus_device_protocol = {
    .version = DEVICE_OPS_VERSION,
    .release = a113_bus_release,
};

static zx_status_t a113_bus_bind(void* ctx, zx_device_t* parent) {
    zx_status_t status;

    a113_bus_t* bus = calloc(1, sizeof(a113_bus_t));
    if (!bus) {
        return ZX_ERR_NO_MEMORY;
    }

    if (device_get_protocol(parent, ZX_PROTOCOL_PLATFORM_BUS, &bus->pbus) != ZX_OK) {
        free(bus);
        return ZX_ERR_NOT_SUPPORTED;
    }


    // Note: We need to do this before adding this device because this method
    //       sets up the GPIO protocol as well.

    if ((status = a113_gpio_init(bus)) != ZX_OK) {
        zxlogf(ERROR, "a113_gpio_init failed: %d\n", status);
    }

    // pinmux for Gauss i2c
    a113_pinmux_config(bus, I2C_SCK_A, 1);
    a113_pinmux_config(bus, I2C_SDA_A, 1);
    a113_pinmux_config(bus, I2C_SCK_B, 1);
    a113_pinmux_config(bus, I2C_SDA_B, 1);

    // Config pinmux for gauss PDM pins
    a113_pinmux_config(bus, A113_GPIOA(14), 1);
    a113_pinmux_config(bus, A113_GPIOA(15), 1);
    a113_pinmux_config(bus, A113_GPIOA(16), 1);
    a113_pinmux_config(bus, A113_GPIOA(17), 1);
    a113_pinmux_config(bus, A113_GPIOA(18), 1);

    if ((status = a113_i2c_init(bus)) != ZX_OK) {
        zxlogf(ERROR, "a113_i2c_init failed: %d\n", status);
    }



    a113_pinmux_config(bus, TDM_BCLK_C, 1);
    a113_pinmux_config(bus, TDM_FSYNC_C, 1);
    a113_pinmux_config(bus, TDM_MOSI_C, 1);
    a113_pinmux_config(bus, TDM_MISO_C, 2);

    a113_pinmux_config(bus, SPK_MUTEn, 0);
    bus->gpio.ops->config(NULL, SPK_MUTEn, GPIO_DIR_OUT);
    bus->gpio.ops->write(NULL, SPK_MUTEn, 1);




    bus->usb_mode_switch.ops = &usb_mode_switch_ops;
    bus->usb_mode_switch.ctx = bus;

    device_add_args_t args = {
        .version = DEVICE_ADD_ARGS_VERSION,
        .name = "a113-bus",
        .ctx = bus,
        .ops = &a113_bus_device_protocol,
        .flags = DEVICE_ADD_NON_BINDABLE,
    };

    status = device_add(parent, &args, NULL);
    if (status != ZX_OK) {
        goto fail;
    }

    pbus_interface_t intf;
    intf.ops = &a113_bus_bus_ops;
    intf.ctx = bus;
    pbus_set_interface(&bus->pbus, &intf);

    if ((status = a113_usb_init(bus)) != ZX_OK) {
        zxlogf(ERROR, "a113_bus_bind failed: %d\n", status);
    }
    if ((status = a113_audio_init(bus)) != ZX_OK) {
        zxlogf(ERROR, "a113_audio_init failed: %d\n", status);
    }

    return ZX_OK;

fail:
    printf("a113_bus_bind failed %d\n", status);
    a113_bus_release(bus);
    return status;
}

static zx_driver_ops_t a113_bus_driver_ops = {
    .version = DRIVER_OPS_VERSION,
    .bind = a113_bus_bind,
};

ZIRCON_DRIVER_BEGIN(a113_bus, a113_bus_driver_ops, "zircon", "0.1", 3)
    BI_ABORT_IF(NE, BIND_PROTOCOL, ZX_PROTOCOL_PLATFORM_BUS),
    BI_ABORT_IF(NE, BIND_PLATFORM_DEV_VID, PDEV_VID_GOOGLE),
    BI_MATCH_IF(EQ, BIND_PLATFORM_DEV_PID, PDEV_PID_GAUSS),
ZIRCON_DRIVER_END(a113_bus)
