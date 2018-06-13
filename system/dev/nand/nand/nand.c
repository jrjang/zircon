// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stdint.h>
#include <time.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/param.h>

#include <bits/limits.h>
#include <ddk/binding.h>
#include <ddk/debug.h>
#include <ddk/device.h>
#include <ddk/protocol/platform-bus.h>
#include <ddk/protocol/platform-defs.h>
#include <ddk/protocol/platform-device.h>
#include <ddk/protocol/rawnand.h>
#include <ddk/io-buffer.h>
#include <hw/reg.h>

#include <zircon/assert.h>
#include <zircon/threads.h>
#include <zircon/types.h>
#include <zircon/status.h>
#include <sync/completion.h>

#include <string.h>

#include "nand.h"

/*
 * TODO: This code does vx_vmar_map/unmap and copies data in/out of the
 * mapped virtual address. unmapping is expensive, but required (a close
 * of the vmo (DOES NOT) unmap, so not unmapping will quickly lead to memory
 * exhaustion. Check to see if we can do something different - is vmo_read/write
 * cheaper than mapping and unmapping (which will cause TLB flushes) ?
 */

#define NAND_TXN_RECEIVED      ZX_EVENT_SIGNALED
#define NAND_SHUTDOWN          ZX_USER_SIGNAL_0

#define NAND_READ_RETRIES       3

static void nand_io_complete(nand_op_t *nand_op, zx_status_t status)
{
    if (nand_op->completion_cb) {
        nand_op->completion_cb(nand_op, status);
    } else {
        zxlogf(TRACE, "nand: nand op %p completion_cb unset!\n", nand_op);
    }
}

/*
 * Main Read entry point into the NAND. Calls controller specific
 * read function.
 * data, oob: pointers to user oob/data buffers.
 * nand_page : NAND page address to read.
 * ecc_correct : Number of ecc corrected bitflips (< 0 indicates
 * ecc could not correct all bitflips - caller needs to check that).
 * retries : I see code to retry in the controller uboot code, so keeping
 * the retry logic here. This may not be needed.
 */
zx_status_t nand_read_page(nand_device_t *dev,
                           void *data,
                           void *oob,
                           uint32_t nand_page,
                           int *corrected_bits,
                           int retries)
{
    zx_status_t status;

    do {
        status = raw_nand_read_page_hwecc(&dev->host, data, oob, nand_page,
                                         corrected_bits);
        if (status != ZX_OK)
            zxlogf(ERROR, "%s: Retrying Read@%u\n",
                   __func__, nand_page);
    } while (status != ZX_OK && --retries >= 0);
    if (status != ZX_OK)
        zxlogf(ERROR, "%s: Read error %d, exhausted all retries\n",
               __func__, status);
    return status;
}

/*
 * Main Write entry point into the NAND. Calls controller specific
 * write function.
 * data, oob: pointers to user oob/data buffers.
 * nand_page : NAND page address to read.
 */
zx_status_t nand_write_page(nand_device_t *dev,
                            void *data,
                            void *oob,
                            uint32_t nand_page)
{
    return raw_nand_write_page_hwecc(&dev->host, data, oob, nand_page);
}

/*
 * Main Erase entry point into NAND. Calls controller specific
 * erase function.
 * nandblock : NAND erase block address.
 */
zx_status_t nand_erase_block(nand_device_t *dev,
                             uint32_t nand_page)
{
    return raw_nand_erase_block(&dev->host, nand_page);
}

zx_status_t nand_erase_op(nand_device_t *dev, nand_op_t *nand_op)
{
    uint32_t nand_page;

    for (uint32_t i = 0; i < nand_op->erase.num_blocks; i++){
        nand_page = (nand_op->erase.first_block + i) *
            dev->nand_info.pages_per_block;
        zx_status_t status;
        status = nand_erase_block(dev, nand_page);
        if (status != ZX_OK) {
            zxlogf(ERROR, "nand: Erase of block %u failed\n",
                   nand_op->erase.first_block + i);
            return status;
        }
    }
    return ZX_OK;
}

static zx_status_t nand_read_page_data_oob_op(nand_device_t *dev,
                                              nand_op_t *nand_op)
{
    uint8_t *vaddr_data = NULL;
    uint8_t *vaddr_oob = NULL;
    int ecc_correct;
    zx_status_t status;

    /* Map data */
    if (nand_op->rw_data_oob.data.length > 0) {
        status = zx_vmar_map(zx_vmar_root_self(),
                             0,
                             nand_op->rw_data_oob.data.vmo,
                             nand_op->rw_data_oob.data.offset_vmo * dev->nand_info.page_size,
                             dev->nand_info.page_size,
                             ZX_VM_FLAG_PERM_READ | ZX_VM_FLAG_PERM_WRITE,
                             (uintptr_t*)&vaddr_data);
        if (status != ZX_OK) {
            zxlogf(ERROR, "nand read page: Cannot map data vmo\n");
            return status;
        }
    }

    /* Map oob */
    if (nand_op->rw_data_oob.oob.length > 0) {
        status = zx_vmar_map(zx_vmar_root_self(),
                             0,
                             nand_op->rw_data_oob.oob.vmo,
                             nand_op->rw_data_oob.oob.offset_vmo,
                             nand_op->rw_data_oob.oob.length,
                             ZX_VM_FLAG_PERM_READ | ZX_VM_FLAG_PERM_WRITE,
                             (uintptr_t*)&vaddr_oob);
        if (status != ZX_OK) {
            zxlogf(ERROR, "nand read page: Cannot map oob vmo\n");
            if (vaddr_data != NULL)
                status = zx_vmar_unmap(zx_vmar_root_self(),
                                       (uintptr_t)vaddr_data, dev->nand_info.page_size);
            return status;
        }
    }

    ecc_correct = 0;
    status = nand_read_page(dev,
                            vaddr_data,
                            vaddr_oob,
                            nand_op->rw_data_oob.page_num,
                            &ecc_correct,
                            NAND_READ_RETRIES);
    if (status != ZX_OK) {
        zxlogf(ERROR, "nand: Read data+oob error %d at page offset %u\n",
               status, nand_op->rw_data_oob.page_num);
    } else {
        nand_op->rw_data_oob.corrected_bit_flips = ecc_correct;
    }
    if (vaddr_data != NULL) {
        status = zx_vmar_unmap(zx_vmar_root_self(),
                               (uintptr_t)vaddr_data, dev->nand_info.page_size);
        if (status != ZX_OK)
            zxlogf(ERROR, "nand: Read Cannot unmap data %d\n",
                   status);
    }
    if (vaddr_oob != NULL) {
        status = zx_vmar_unmap(zx_vmar_root_self(),
                               (uintptr_t)vaddr_oob, nand_op->rw_data_oob.oob.length);
        if (status != ZX_OK)
            zxlogf(ERROR, "nand: Read Cannot unmap oob %d\n",
                   status);
    }
    return status;
}

static zx_status_t nand_write_page_data_oob_op(nand_device_t *dev,
                                               nand_op_t *nand_op)
{
    uint8_t *vaddr_data = NULL;
    uint8_t *vaddr_oob = NULL;
    zx_status_t status;

    /* Map data */
    if (nand_op->rw_data_oob.data.length > 0) {
        status = zx_vmar_map(zx_vmar_root_self(),
                             0,
                             nand_op->rw_data_oob.data.vmo,
                             nand_op->rw_data_oob.data.offset_vmo * dev->nand_info.page_size,
                             dev->nand_info.page_size,
                             ZX_VM_FLAG_PERM_READ | ZX_VM_FLAG_PERM_WRITE,
                             (uintptr_t*)&vaddr_data);
        if (status != ZX_OK) {
            zxlogf(ERROR, "nand write page: Cannot map data vmo\n");
            return status;
        }
    }

    /* Map oob */
    if (nand_op->rw_data_oob.oob.length > 0) {
        status = zx_vmar_map(zx_vmar_root_self(),
                             0,
                             nand_op->rw_data_oob.oob.vmo,
                             nand_op->rw_data_oob.oob.offset_vmo,
                             nand_op->rw_data_oob.oob.length,
                             ZX_VM_FLAG_PERM_READ | ZX_VM_FLAG_PERM_WRITE,
                             (uintptr_t*)&vaddr_oob);
        if (status != ZX_OK) {
            zxlogf(ERROR, "nand write page: Cannot map oob vmo\n");
            if (vaddr_data != NULL)
                status = zx_vmar_unmap(zx_vmar_root_self(),
                                       (uintptr_t)vaddr_data, dev->nand_info.page_size);
            return status;
        }
    }

    status = nand_write_page(dev,
                             vaddr_data,
                             vaddr_oob,
                             nand_op->rw_data_oob.page_num);
    if (status != ZX_OK) {
        zxlogf(ERROR, "nand: Write data+oob error %d at page offset %u\n",
               status, nand_op->rw_data_oob.page_num);
    }
    if (vaddr_data != NULL) {
        status = zx_vmar_unmap(zx_vmar_root_self(),
                               (uintptr_t)vaddr_data, dev->nand_info.page_size);
        if (status != ZX_OK)
            zxlogf(ERROR, "nand: Write Cannot unmap data %d\n",
                   status);
    }
    if (vaddr_oob != NULL) {
        status = zx_vmar_unmap(zx_vmar_root_self(),
                               (uintptr_t)vaddr_oob, nand_op->rw_data_oob.oob.length);
        if (status != ZX_OK)
            zxlogf(ERROR, "nand: Write Cannot unmap oob %d\n",
                   status);
    }
    return status;
}

static void nand_do_io(nand_device_t *dev, nand_io_t *io)
{
    zx_status_t status = ZX_OK;
    nand_op_t *nand_op = &io->nand_op;

    ZX_DEBUG_ASSERT(dev != NULL);
    ZX_DEBUG_ASSERT(io != NULL);
    switch (nand_op->command) {
    case NAND_OP_READ_PAGE_DATA_OOB:
        status = nand_read_page_data_oob_op(dev, nand_op);
        break;
    case NAND_OP_WRITE_PAGE_DATA_OOB:
        status = nand_write_page_data_oob_op(dev, nand_op);
        break;
    case NAND_OP_ERASE:
        status = nand_erase_op(dev, nand_op);
        break;
    default:
        ZX_DEBUG_ASSERT(false);  // Unexpected.
    }
    nand_io_complete(nand_op, status);
}

/*
 * Initialization is complete by the time the thread
 * starts.
 */
static zx_status_t nand_worker_thread(void* arg)
{
    nand_device_t *dev = (nand_device_t *)arg;
    zx_status_t status;

    for (;;) {
        /*
         * Don't loop until io_list is empty to check for NAND_SHUTDOWN
         * between each io.
         */
        mtx_lock(&dev->lock);
        nand_io_t *io = list_remove_head_type(&dev->io_list, nand_io_t, node);

        if (io) {
            // Unlock if we execute the transaction
            mtx_unlock(&dev->lock);
            nand_do_io(dev, io);
        } else {
            /* clear the "RECEIVED" flag under the lock */
            zx_object_signal(dev->worker_event, NAND_TXN_RECEIVED, 0);
            mtx_unlock(&dev->lock);
        }

        uint32_t pending;
        status = zx_object_wait_one(dev->worker_event,
                                    NAND_TXN_RECEIVED | NAND_SHUTDOWN,
                                    ZX_TIME_INFINITE, &pending);
        if (status != ZX_OK) {
            zxlogf(ERROR, "nand: worker thread wait failed, retcode = %d\n", status);
            break;
        }
        if (pending & NAND_SHUTDOWN)
            break;
    }

    zxlogf(TRACE, "nand: worker thread terminated\n");
    return ZX_OK;
}

static void nand_query(void *ctx, nand_info_t *info_out,
                       size_t *nand_op_size_out)
{
    nand_device_t *dev = (nand_device_t *)ctx;

    memcpy(info_out, &dev->nand_info, sizeof(*info_out));
    *nand_op_size_out = sizeof(nand_io_t);
}

static void nand_queue(void *ctx, nand_op_t *op)
{
    nand_device_t *dev = (nand_device_t *)ctx;
    nand_io_t *io = containerof(op, nand_io_t, nand_op);

    switch (op->command) {
    case NAND_OP_WRITE_OOB:
    case NAND_OP_READ_OOB:
    case NAND_OP_WRITE_DATA:
    case NAND_OP_READ_DATA: {
        op->completion_cb(op, ZX_ERR_NOT_SUPPORTED);
        return;
    }
    case NAND_OP_ERASE:
        if (!op->erase.num_blocks ||
            op->erase.first_block >= dev->nand_info.num_blocks ||
            ((uint64_t)(op->erase.first_block + op->erase.num_blocks) >
             (uint64_t)dev->nand_info.num_blocks)) {
            op->completion_cb(op, ZX_ERR_OUT_OF_RANGE);
            return;
        }
        break;
    case NAND_OP_READ_PAGE_DATA_OOB:
    case NAND_OP_WRITE_PAGE_DATA_OOB:
        if ((op->rw_data_oob.data.length == 0 &&
             op->rw_data_oob.oob.length == 0) ||
            op->rw_data_oob.data.length > 1 ||
            op->rw_data_oob.oob.length > dev->nand_info.oob_size ||
            op->rw_data_oob.page_num >= dev->num_nand_pages ||
            (op->rw_data_oob.data.length + op->rw_data_oob.page_num) > dev->num_nand_pages) {
            op->completion_cb(op, ZX_ERR_OUT_OF_RANGE);
            return;
        }
        /*
         * If oob length is > 0, it must equal oobsize
         * because we could copy out upto oob_size bytes
         */
        if (op->rw_data_oob.oob.length > 0 &&
            op->rw_data_oob.oob.length != dev->nand_info.oob_size) {
            op->completion_cb(op, ZX_ERR_OUT_OF_RANGE);
            return;
        }
        break;

    default:
        op->completion_cb(op, ZX_ERR_NOT_SUPPORTED);
        return;
    }

    mtx_lock(&dev->lock);
    /* TODO: UPDATE STATS HERE */
    list_add_tail(&dev->io_list, &io->node);
    /*
     * Wake up the worker thread (while locked, so they don't accidentally
     * clear the event).
     */
    zx_object_signal(dev->worker_event, 0, NAND_TXN_RECEIVED);
    mtx_unlock(&dev->lock);
}

/* XXX Not needed so not supported right now */
static void nand_get_bad_block_list(void *ctx, uint32_t *bad_blocks,
                                    uint32_t bad_block_len,
                                    uint32_t *num_bad_blocks)
{
    *num_bad_blocks = 0;
}

// Nand protocol
static nand_protocol_ops_t nand_proto = {
    .query = nand_query,
    .queue = nand_queue,
    .get_bad_block_list = nand_get_bad_block_list,
};

static void nand_unbind(void* ctx)
{
    nand_device_t *dev = ctx;

    device_remove(dev->zxdev);
}

static void nand_release(void* ctx)
{
    nand_device_t *dev = ctx;

    // signal the worker thread and wait for it to terminate
    zx_object_signal(dev->worker_event, 0, NAND_SHUTDOWN);
    thrd_join(dev->worker_thread, NULL);
    mtx_lock(&dev->lock);
    // error out all pending requests
    nand_io_t* io = NULL;
    list_for_every_entry(&dev->io_list, io, nand_io_t, node) {
        mtx_unlock(&dev->lock);
        nand_io_complete(&io->nand_op, ZX_ERR_BAD_STATE);
        mtx_lock(&dev->lock);
    }
    mtx_unlock(&dev->lock);
    if (dev->worker_event != ZX_HANDLE_INVALID) {
        zx_handle_close(dev->worker_event);
    }
    free(dev);
}

extern zx_status_t nand_ioctl(void* ctx, uint32_t op, const void* cmd,
                              size_t cmdlen, void* reply, size_t max,
                              size_t* out_actual);

// Device protocol.
static zx_protocol_device_t nand_device_proto = {
    .version = DEVICE_OPS_VERSION,
    .ioctl = nand_ioctl,
    .unbind = nand_unbind,
    .release = nand_release,
};

static zx_status_t nand_bind(void* ctx, zx_device_t* parent)
{
    zxlogf(ERROR, "nand_bind: Starting...!\n");

    nand_device_t* dev = calloc(1, sizeof(*dev));
    if (!dev) {
        zxlogf(ERROR, "nand: no memory to allocate nand device!\n");
        return ZX_ERR_NO_MEMORY;
    }

    dev->nand_proto.ops = &nand_proto;
    dev->nand_proto.ctx = dev;

    zx_status_t st = device_get_protocol(parent, ZX_PROTOCOL_RAW_NAND, &dev->host);
    if (st != ZX_OK) {
        zxlogf(ERROR, "nand: failed to get raw_nand protocol %d\n", st);
        st = ZX_ERR_NOT_SUPPORTED;
        goto fail;
    }

    mtx_init(&dev->lock, mtx_plain);
    list_initialize(&dev->io_list);

    st = zx_event_create(0, &dev->worker_event);
    if (st != ZX_OK) {
        zxlogf(ERROR, "nand: failed to create event, retcode = %d\n", st);
        goto fail;
    }

    zx_device_prop_t props[] = {
        { BIND_PROTOCOL, 0, ZX_PROTOCOL_NAND },
        { BIND_NAND_CLASS, 0, NAND_CLASS_PARTMAP },
    };

    device_add_args_t args = {
        .version = DEVICE_ADD_ARGS_VERSION,
        .name = "nand",
        .ctx = dev,
        .ops = &nand_device_proto,
        .proto_id = ZX_PROTOCOL_NAND,
        .proto_ops = &nand_proto,
        .props = props,
        .prop_count = countof(props),
    };

    if (dev->host.ops->get_nand_info == NULL) {
        st = ZX_ERR_NOT_SUPPORTED;
        zxlogf(ERROR, "nand: failed to get nand info, function does not exist\n");
        goto fail;
    }
    st = raw_nand_get_info(&dev->host, &dev->nand_info);
    if (st != ZX_OK) {
        zxlogf(ERROR, "nand: get_nand_info returned error %d\n", st);
        goto fail;
    }
    dev->num_nand_pages = dev->nand_info.num_blocks *
        dev->nand_info.pages_per_block;

    int rc = thrd_create_with_name(&dev->worker_thread, nand_worker_thread, dev,
                                   "nand-worker");
    if (rc != thrd_success) {
        st = thrd_status_to_zx_status(rc);
        goto fail_remove;
    }

    st = device_add(parent, &args, &dev->zxdev);
    if (st != ZX_OK) {
        goto fail;
    }

    return ZX_OK;

fail_remove:
    device_remove(dev->zxdev);
fail:
    free(dev);
    return st;
}

static zx_driver_ops_t nand_driver_ops = {
    .version = DRIVER_OPS_VERSION,
    .bind = nand_bind,
};

// The formatter does not play nice with these macros.
// clang-format off
ZIRCON_DRIVER_BEGIN(nand, nand_driver_ops, "zircon", "0.1", 1)
    BI_MATCH_IF(EQ, BIND_PROTOCOL, ZX_PROTOCOL_RAW_NAND),
ZIRCON_DRIVER_END(nand)
// clang-format on

