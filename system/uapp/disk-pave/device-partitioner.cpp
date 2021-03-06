// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <dirent.h>
#include <fcntl.h>

#include <chromeos-disk-setup/chromeos-disk-setup.h>
#include <fbl/auto_call.h>
#include <gpt/cros.h>
#include <zircon/device/device.h>
#include <zxcrypt/volume.h>

#include "device-partitioner.h"
#include "fvm/fvm.h"
#include "pave-logging.h"

namespace paver {

namespace {

bool KernelFilterCallback(const gpt_partition_t& part, fbl::StringPiece partition_name) {
    const uint8_t kern_type[GPT_GUID_LEN] = GUID_CROS_KERNEL_VALUE;
    char cstring_name[GPT_NAME_LEN];
    utf16_to_cstring(cstring_name, reinterpret_cast<const uint16_t*>(part.name), GPT_NAME_LEN);
    return memcmp(part.type, kern_type, GPT_GUID_LEN) == 0 &&
           strncmp(cstring_name, partition_name.data(), partition_name.length()) == 0;
}

bool FvmFilterCallback(const gpt_partition_t& part) {
    const uint8_t partition_type[GPT_GUID_LEN] = GUID_FVM_VALUE;
    return memcmp(part.type, partition_type, GPT_GUID_LEN) == 0;
}

constexpr size_t ReservedHeaderBlocks(size_t blk_size) {
    constexpr size_t kReservedEntryBlocks = (16 * 1024);
    return (kReservedEntryBlocks + 2 * blk_size) / blk_size;
};

constexpr char kFvmPartitionName[] = "fvm";

// Helper function to auto-deduce type.
template <typename T>
fbl::unique_ptr<T> WrapUnique(T* ptr) {
    return fbl::unique_ptr<T>(ptr);
}

} // namespace

fbl::unique_ptr<DevicePartitioner> DevicePartitioner::Create() {
    fbl::unique_ptr<DevicePartitioner> device_partitioner;
#if defined(__x86_64__)
    if ((CrosDevicePartitioner::Initialize(&device_partitioner) == ZX_OK) ||
        (EfiDevicePartitioner::Initialize(&device_partitioner) == ZX_OK)) {
        return fbl::move(device_partitioner);
    }
#elif defined(__aarch64__)
    if (FixedDevicePartitioner::Initialize(&device_partitioner) == ZX_OK) {
        return fbl::move(device_partitioner);
    }
#endif
    return nullptr;
}

/*====================================================*
 *                  GPT Common                        *
 *====================================================*/

bool GptDevicePartitioner::FindTargetGptPath(fbl::String* out) {
    constexpr char kBlockDevPath[] = "/dev/class/block";
    DIR* d = opendir(kBlockDevPath);
    if (d == nullptr) {
        ERROR("Cannot inspect block devices\n");
        return false;
    }
    const auto closer = fbl::MakeAutoCall([&]() { closedir(d); });

    struct dirent* de;
    while ((de = readdir(d)) != nullptr) {
        fbl::unique_fd fd(openat(dirfd(d), de->d_name, O_RDWR));
        if (!fd) {
            continue;
        }
        out->Set(PATH_MAX, '\0');
        ssize_t r = ioctl_device_get_topo_path(fd.get(), const_cast<char*>(out->data()), PATH_MAX);
        if (r < 0) {
            continue;
        }

        block_info_t info;
        if ((r = ioctl_block_get_info(fd.get(), &info) < 0)) {
            continue;
        }

        // TODO(ZX-1344): This is a hack, but practically, will work for our
        // usage.
        //
        // The GPT which will contain an FVM should be the first non-removable
        // block device that isn't a partition itself.
        if (!(info.flags & BLOCK_FLAG_REMOVABLE) && strstr(out->c_str(), "part-") == nullptr) {
            return true;
        }
    }

    ERROR("No candidate GPT found\n");
    return false;
}

zx_status_t GptDevicePartitioner::InitializeGpt(fbl::unique_ptr<GptDevicePartitioner>* gpt_out) {
    fbl::String gpt_path;
    if (!FindTargetGptPath(&gpt_path)) {
        ERROR("Failed to find GPT\n");
        return ZX_ERR_NOT_FOUND;
    }
    fbl::unique_fd fd(open(gpt_path.c_str(), O_RDWR));
    if (!fd) {
        ERROR("Failed to open GPT\n");
        return ZX_ERR_NOT_FOUND;
    }
    block_info_t block_info;
    ssize_t rc = ioctl_block_get_info(fd.get(), &block_info);
    if (rc < 0) {
        ERROR("Couldn't get GPT block info\n");
        return ZX_ERR_NOT_FOUND;
    }

    gpt_device_t* gpt;
    if (gpt_device_init(fd.get(), block_info.block_size, block_info.block_count, &gpt)) {
        ERROR("Failed to get GPT info\n");
        return ZX_ERR_BAD_STATE;
    }

    auto releaser = fbl::MakeAutoCall([&]() { gpt_device_release(gpt); });
    if (!gpt->valid) {
        ERROR("Located GPT is invalid; Attempting to initialize\n");
        if (gpt_partition_remove_all(gpt)) {
            ERROR("Failed to create empty GPT\n");
            return ZX_ERR_BAD_STATE;
        } else if (gpt_device_sync(gpt)) {
            ERROR("Failed to sync empty GPT\n");
            return ZX_ERR_BAD_STATE;
        } else if ((rc = ioctl_block_rr_part(fd.get())) != ZX_OK) {
            ERROR("Failed to re-read GPT\n");
            return ZX_ERR_BAD_STATE;
        }
    }

    releaser.cancel();
    *gpt_out = fbl::move(WrapUnique(new GptDevicePartitioner(fbl::move(fd), gpt, block_info)));
    return ZX_OK;
}

struct PartitionPosition {
    size_t start;  // Block, inclusive
    size_t length; // In Blocks
};

zx_status_t GptDevicePartitioner::FindFirstFit(size_t bytes_requested, size_t* start_out,
                                               size_t* length_out) const {
    LOG("Looking for space\n");
    // Gather GPT-related information.
    size_t blocks_requested =
        (bytes_requested + block_info_.block_size - 1) / block_info_.block_size;

    // Sort all partitions by starting block.
    // For simplicity, include the 'start' and 'end' reserved spots as
    // partitions.
    size_t partition_count = 0;
    PartitionPosition partitions[PARTITIONS_COUNT + 2];
    const size_t reserved_blocks = ReservedHeaderBlocks(block_info_.block_size);
    partitions[partition_count].start = 0;
    partitions[partition_count++].length = reserved_blocks;
    partitions[partition_count].start = block_info_.block_count - reserved_blocks;
    partitions[partition_count++].length = reserved_blocks;

    for (size_t i = 0; i < PARTITIONS_COUNT; i++) {
        const gpt_partition_t* p = gpt_->partitions[i];
        if (!p) {
            continue;
        }
        partitions[partition_count].start = p->first;
        partitions[partition_count].length = p->last - p->first + 1;
        LOG("Partition seen with start %zu, end %zu (length %zu)\n", p->first, p->last,
            partitions[partition_count].length);
        partition_count++;
    }
    LOG("Sorting\n");
    qsort(partitions, partition_count, sizeof(PartitionPosition),
          [](const void* p1, const void* p2) {
              ssize_t s1 = static_cast<ssize_t>(static_cast<const PartitionPosition*>(p1)->start);
              ssize_t s2 = static_cast<ssize_t>(static_cast<const PartitionPosition*>(p2)->start);
              return static_cast<int>(s1 - s2);
          });

    // Look for space between the partitions. Since the reserved spots of the
    // GPT were included in |partitions|, all available space will be located
    // "between" partitions.
    for (size_t i = 0; i < partition_count - 1; i++) {
        const size_t next = partitions[i].start + partitions[i].length;
        LOG("Partition[%zu] From Block [%zu, %zu) ... (next partition starts at block %zu)\n",
            i, partitions[i].start, next, partitions[i + 1].start);

        if (next > partitions[i + 1].start) {
            ERROR("Corrupted GPT\n");
            return ZX_ERR_IO;
        }
        const size_t free_blocks = partitions[i + 1].start - next;
        LOG("    There are %zu free blocks (%zu requested)\n", free_blocks, blocks_requested);
        if (free_blocks >= blocks_requested) {
            *start_out = next;
            *length_out = free_blocks;
            return ZX_OK;
        }
    }
    ERROR("No GPT space found\n");
    return ZX_ERR_NO_RESOURCES;
}

zx_status_t GptDevicePartitioner::CreateGptPartition(const char* name, uint8_t* type,
                                                     uint64_t offset, uint64_t blocks,
                                                     uint8_t* out_guid) {
    zx_status_t status;
    if ((status = zx_cprng_draw_new(out_guid, GPT_GUID_LEN)) != ZX_OK) {
        ERROR("Failed to get random GUID\n");
        return status;
    }
    if ((status = gpt_partition_add(gpt_, name, type, out_guid, offset, blocks, 0))) {
        ERROR("Failed to add partition\n");
        return ZX_ERR_IO;
    }
    if ((status = gpt_device_sync(gpt_))) {
        ERROR("Failed to sync GPT\n");
        return ZX_ERR_IO;
    }
    if ((status = gpt_partition_clear(gpt_, offset, 1))) {
        ERROR("Failed to clear first block of new partition\n");
        return ZX_ERR_IO;
    }
    if ((status = static_cast<zx_status_t>(ioctl_block_rr_part(fd_.get()))) < 0) {
        ERROR("Failed to rebind GPT\n");
        return status;
    }

    return ZX_OK;
}

zx_status_t GptDevicePartitioner::AddPartition(
    const char* name, uint8_t* type, size_t minimum_size_bytes,
    size_t optional_reserve_bytes, fbl::unique_fd* out_fd) {

    uint64_t start, length;
    zx_status_t status;
    if ((status = FindFirstFit(minimum_size_bytes, &start, &length)) != ZX_OK) {
        ERROR("Couldn't find fit\n");
        return status;
    }
    LOG("Found space in GPT - OK %zu @ %zu\n", length, start);

    if (optional_reserve_bytes) {
        // If we can fulfill the requested size, and we still have space for the
        // optional reserve section, then we should shorten the amount of blocks
        // we're asking for.
        //
        // This isn't necessary, but it allows growing the GPT later, if necessary.
        const size_t optional_reserve_blocks = optional_reserve_bytes / block_info_.block_size;
        if (length - optional_reserve_bytes > (minimum_size_bytes / block_info_.block_size)) {
            LOG("Space for reserve - OK\n");
            length -= optional_reserve_blocks;
        }
    } else {
        length = fbl::round_up(minimum_size_bytes, block_info_.block_size) / block_info_.block_size;
    }
    LOG("Final space in GPT - OK %zu @ %zu\n", length, start);

    uint8_t guid[GPT_GUID_LEN];
    if ((status = CreateGptPartition(name, type, start, length, guid)) != ZX_OK) {
        return status;
    }
    LOG("Added partition, waiting for bind\n");

    out_fd->reset(open_partition(guid, type, ZX_SEC(5), nullptr));
    if (!*out_fd) {
        ERROR("Added partition, waiting for bind - NOT FOUND\n");
        return ZX_ERR_IO;
    }
    LOG("Added partition, waiting for bind - OK\n");
    return ZX_OK;
}

zx_status_t GptDevicePartitioner::FindPartition(FilterCallback filter, gpt_partition_t** out,
                                                fbl::unique_fd* out_fd) {
    for (size_t i = 0; i < PARTITIONS_COUNT; i++) {
        gpt_partition_t* p = gpt_->partitions[i];
        if (!p) {
            continue;
        }

        if (filter(*p)) {
            LOG("Found partition in GPT, partition %zu\n", i);
            if (out) {
                *out = p;
            }
            if (out_fd) {
                out_fd->reset(open_partition(p->guid, p->type, ZX_SEC(5), nullptr));
                if (!*out_fd) {
                    ERROR("Couldn't open partition\n");
                    return ZX_ERR_IO;
                }
            }
            return ZX_OK;
        }
    }
    return ZX_ERR_NOT_FOUND;
}

zx_status_t GptDevicePartitioner::FindPartition(FilterCallback filter,
                                                fbl::unique_fd* out_fd) const {
    for (size_t i = 0; i < PARTITIONS_COUNT; i++) {
        const gpt_partition_t* p = gpt_->partitions[i];
        if (!p) {
            continue;
        }

        if (filter(*p)) {
            LOG("Found partition in GPT, partition %zu\n", i);
            if (out_fd) {
                out_fd->reset(open_partition(p->guid, p->type, ZX_SEC(5), nullptr));
                if (!*out_fd) {
                    ERROR("Couldn't open partition\n");
                    return ZX_ERR_IO;
                }
            }
            return ZX_OK;
        }
    }
    return ZX_ERR_NOT_FOUND;
}

zx_status_t GptDevicePartitioner::WipePartitions(FilterCallback filter) {
    bool modify = false;
    for (size_t i = 0; i < PARTITIONS_COUNT; i++) {
        const gpt_partition_t* p = gpt_->partitions[i];
        if (!p) {
            continue;
        }
        if (!filter(*p)) {
            continue;
        }

        modify = true;

        // Overwrite the first 8k to (hackily) ensure the destroyed partition
        // doesn't "reappear" in place.
        char buf[8192];
        memset(buf, 0, sizeof(buf));
        fbl::unique_fd pfd(open_partition(p->guid, p->type, ZX_SEC(2), nullptr));
        if (!pfd) {
            ERROR("Warning: Could not open partition to overwrite first 8KB\n");
        } else {
            write(pfd.get(), buf, sizeof(buf));
        }

        if (gpt_partition_remove(gpt_, p->guid)) {
            ERROR("Warning: Could not remove partition\n");
        } else {
            // If we successfully clear the partition, then all subsequent
            // partitions get shifted down. If we just deleted partition 'i',
            // we now need to look at partition 'i' again, since it's now
            // occupied by what was in 'i+1'.
            i--;
        }
    }
    if (modify) {
        gpt_device_sync(gpt_);
        LOG("GPT updated, reboot strongly recommended immediately\n");
    }
    ioctl_block_rr_part(fd_.get());
    return ZX_OK;
}

/*====================================================*
 *                 EFI SPECIFIC                       *
 *====================================================*/

zx_status_t EfiDevicePartitioner::Initialize(fbl::unique_ptr<DevicePartitioner>* partitioner) {
    fbl::unique_ptr<GptDevicePartitioner> gpt;
    zx_status_t status;
    if ((status = GptDevicePartitioner::InitializeGpt(&gpt)) != ZX_OK) {
        return status;
    }
    if (is_cros(gpt->GetGpt())) {
        ERROR("Use CrOS Device Partitioner.");
        return ZX_ERR_NOT_SUPPORTED;
    }

    LOG("Successfully intitialized EFI Device Partitioner\n");
    *partitioner = fbl::move(WrapUnique(new EfiDevicePartitioner(fbl::move(gpt))));
    return ZX_OK;
}

// Name used by previous Fuchsia Installer.
constexpr char kOldEfiName[] = "EFI";

// Name used for EFI partitions added by paver.
constexpr char kEfiName[] = "EFI Gigaboot";

zx_status_t EfiDevicePartitioner::AddPartition(Partition partition_type, fbl::unique_fd* out_fd) {
    const char* name;
    uint8_t type[GPT_GUID_LEN];
    size_t minimum_size_bytes = 0;
    size_t optional_reserve_bytes = 0;

    switch (partition_type) {
    case Partition::kEfi: {
        const uint8_t efi_type[GPT_GUID_LEN] = GUID_EFI_VALUE;
        memcpy(type, efi_type, GPT_GUID_LEN);
        minimum_size_bytes = 1LU * (1 << 30);
        name = kEfiName;
        break;
    }
    case Partition::kFuchsiaVolumeManager: {
        const uint8_t fvm_type[GPT_GUID_LEN] = GUID_FVM_VALUE;
        memcpy(type, fvm_type, GPT_GUID_LEN);
        minimum_size_bytes = 8LU * (1 << 30);
        name = kFvmPartitionName;
        break;
    }
    default:
        ERROR("EFI partitioner cannot add unknown partition type\n");
        return ZX_ERR_NOT_SUPPORTED;
    }

    return gpt_->AddPartition(name, type, minimum_size_bytes,
                                              optional_reserve_bytes, out_fd);
}

bool EfiDevicePartitioner::FilterZirconPartition(const block_info_t& info,
                                                 const gpt_partition_t& part) {
    const uint8_t efi_type[GPT_GUID_LEN] = GUID_EFI_VALUE;
    char cstring_name[GPT_NAME_LEN];
    utf16_to_cstring(cstring_name, reinterpret_cast<const uint16_t*>(part.name), GPT_NAME_LEN);
    // Old EFI: Installed by the legacy Fuchsia installer, identified by
    // large size and "EFI" label.
    constexpr unsigned int k512MB = (1LU << 29);
    const bool old_efi = strncmp(cstring_name, kOldEfiName, strlen(kOldEfiName)) == 0 &&
                         ((part.last - part.first + 1) * info.block_size) > k512MB;
    // Disk-paved EFI: Identified by "EFI Gigaboot" label.
    const bool new_efi = strncmp(cstring_name, kEfiName, strlen(kEfiName)) == 0;
    return memcmp(part.type, efi_type, GPT_GUID_LEN) == 0 && (old_efi || new_efi);
}

zx_status_t EfiDevicePartitioner::FindPartition(Partition partition_type,
                                                fbl::unique_fd* out_fd) const {
    block_info_t info;
    zx_status_t status;
    if ((status = gpt_->GetBlockInfo(&info)) != ZX_OK) {
        ERROR("Unable to get block info\n");
        return ZX_ERR_IO;
    }

    switch (partition_type) {
    case Partition::kEfi: {
        const auto filter = [&info](const gpt_partition_t& part) {
            return FilterZirconPartition(info, part);
        };
        return gpt_->FindPartition(filter, out_fd);
    }
    case Partition::kFuchsiaVolumeManager:
        return gpt_->FindPartition(FvmFilterCallback, out_fd);

    default:
        ERROR("EFI partitioner cannot find unknown partition type\n");
        return ZX_ERR_NOT_SUPPORTED;
    }
}

zx_status_t EfiDevicePartitioner::WipePartitions(const fbl::Vector<Partition>& partitions) {
    const uint8_t fvm_type[GPT_GUID_LEN] = GUID_FVM_VALUE;
    const uint8_t install_type[GPT_GUID_LEN] = GUID_INSTALL_VALUE;
    const uint8_t system_type[GPT_GUID_LEN] = GUID_SYSTEM_VALUE;
    const uint8_t blob_type[GPT_GUID_LEN] = GUID_BLOB_VALUE;
    const uint8_t data_type[GPT_GUID_LEN] = GUID_DATA_VALUE;

    block_info_t info;
    zx_status_t status;
    if ((status = gpt_->GetBlockInfo(&info)) != ZX_OK) {
        ERROR("Unable to get block info\n");
        return ZX_ERR_IO;
    }

    fbl::Vector<const uint8_t*> partition_list;
    bool efi = false;
    for (const Partition& partition_type : partitions) {
        switch (partition_type) {
        case Partition::kEfi: {
            // Special case.
            efi = true;
            break;
        }
        case Partition::kKernelC:
            break;
        case Partition::kFuchsiaVolumeManager:
            partition_list.push_back(fvm_type);
            break;
        case Partition::kInstallType:
            partition_list.push_back(install_type);
            break;
        case Partition::kSystem:
            partition_list.push_back(system_type);
            break;
        case Partition::kBlob:
            partition_list.push_back(blob_type);
            break;
        case Partition::kData:
            partition_list.push_back(data_type);
            break;
        default:
            return ZX_ERR_NOT_SUPPORTED;
        }
    }

    // Early return if nothing to wipe.
    if (partition_list.is_empty() && !efi) {
        return ZX_OK;
    }

    const auto filter = [&info, &partition_list, efi](const gpt_partition_t& part) {
        for (const auto& type : partition_list) {
            if (memcmp(part.type, type, GPT_GUID_LEN) == 0)
                return true;
        }
        if (efi) {
            return FilterZirconPartition(info, part);
        }
        return false;
    };
    return gpt_->WipePartitions(filter);
}

/*====================================================*
 *                CROS SPECIFIC                       *
 *====================================================*/

zx_status_t CrosDevicePartitioner::Initialize(fbl::unique_ptr<DevicePartitioner>* partitioner) {
    fbl::unique_ptr<GptDevicePartitioner> gpt_partitioner;
    zx_status_t status;
    if ((status = GptDevicePartitioner::InitializeGpt(&gpt_partitioner)) != ZX_OK) {
        return status;
    }

    gpt_device_t *gpt = gpt_partitioner->GetGpt();
    if (!is_cros(gpt)) {
        return ZX_ERR_NOT_FOUND;
    }

    block_info_t info;
    gpt_partitioner->GetBlockInfo(&info);

    if (!is_ready_to_pave(gpt, &info, SZ_ZX_PART, SZ_ROOT_PART, true)) {
        if ((status = config_cros_for_fuchsia(gpt, &info, SZ_ZX_PART, SZ_ROOT_PART,
                                              true)) != ZX_OK) {
            ERROR("Failed to configure CrOS for Fuchsia.\n");
            return status;
        }
        gpt_device_sync(gpt);
        ioctl_block_rr_part(gpt_partitioner->GetFd());
    }

    LOG("Successfully initialized CrOS Device Partitioner\n");
    *partitioner = fbl::move(WrapUnique(new CrosDevicePartitioner(fbl::move(gpt_partitioner))));
    return ZX_OK;
}

constexpr char kKernaName[] = "KERN-A";
constexpr char kKernbName[] = "KERN-B";
constexpr char kKerncName[] = "KERN-C";

zx_status_t CrosDevicePartitioner::AddPartition(Partition partition_type,
                                                fbl::unique_fd* out_fd) {
    const char* name;
    uint8_t type[GPT_GUID_LEN];
    size_t minimum_size_bytes = 0;
    size_t optional_reserve_bytes = 0;

    switch (partition_type) {
    case Partition::kKernelC: {
        const uint8_t kernc_type[GPT_GUID_LEN] = GUID_CROS_KERNEL_VALUE;
        memcpy(type, kernc_type, GPT_GUID_LEN);
        minimum_size_bytes = 64LU * (1 << 20);
        name = kKerncName;
        break;
    }
    case Partition::kFuchsiaVolumeManager: {
        const uint8_t fvm_type[GPT_GUID_LEN] = GUID_FVM_VALUE;
        memcpy(type, fvm_type, GPT_GUID_LEN);
        minimum_size_bytes = 8LU * (1 << 30);
        name = kFvmPartitionName;
        break;
    }
    default:
        ERROR("Cros partitioner cannot add unknown partition type\n");
        return ZX_ERR_NOT_SUPPORTED;
    }
    return gpt_->AddPartition(name, type, minimum_size_bytes,
                                              optional_reserve_bytes, out_fd);
}

zx_status_t CrosDevicePartitioner::FindPartition(Partition partition_type,
                                                 fbl::unique_fd* out_fd) const {
    switch (partition_type) {
    case Partition::kKernelC: {
        const auto filter = [](const gpt_partition_t& part) {
            return KernelFilterCallback(part, kKerncName);
        };
        return gpt_->FindPartition(filter, out_fd);
    }
    case Partition::kFuchsiaVolumeManager:
        return gpt_->FindPartition(FvmFilterCallback, out_fd);

    default:
        ERROR("Cros partitioner cannot find unknown partition type\n");
        return ZX_ERR_NOT_SUPPORTED;
    }
}

zx_status_t CrosDevicePartitioner::FinalizePartition(Partition partition_type) {
    // Special partition finalization is only necessary for Zircon partition.
    if (partition_type != Partition::kKernelC) {
        return ZX_OK;
    }

    // First, find the priority of the KERN-A and KERN-B partitions.
    gpt_partition_t* partition;
    zx_status_t status;
    const auto filter_kerna = [](const gpt_partition_t& part) {
        return KernelFilterCallback(part, kKernaName);
    };
    if ((status = gpt_->FindPartition(filter_kerna, &partition, nullptr)) != ZX_OK) {
        ERROR("Cannot find %s partition\n", kKernaName);
        return status;
    }
    const uint8_t priority_a = gpt_cros_attr_get_priority(partition->flags);

    const auto filter_kernb = [](const gpt_partition_t& part) {
        return KernelFilterCallback(part, kKernbName);
    };
    if ((status = gpt_->FindPartition(filter_kernb, &partition, nullptr)) != ZX_OK) {
        ERROR("Cannot find %s partition\n", kKernbName);
        return status;
    }
    const uint8_t priority_b = gpt_cros_attr_get_priority(partition->flags);

    const auto filter_kernc = [](const gpt_partition_t& part) {
        return KernelFilterCallback(part, kKerncName);
    };
    if ((status = gpt_->FindPartition(filter_kernc, &partition, nullptr)) != ZX_OK) {
        ERROR("Cannot find %s partition\n", kKerncName);
        return status;
    }

    // Priority for Kern C set to higher priority than Kern A and Kern B.
    uint8_t priority_c = fbl::max(priority_a, priority_b);
    if (priority_c + 1 <= priority_c) {
        ERROR("Cannot set CrOS partition priority higher than A and B\n");
        return ZX_ERR_OUT_OF_RANGE;
    }
    priority_c++;
    if (priority_c <= gpt_cros_attr_get_priority(partition->flags)) {
        // No modification required; the priority is already high enough.
        return ZX_OK;
    }

    if (gpt_cros_attr_set_priority(&partition->flags, priority_c) != 0) {
        ERROR("Cannot set CrOS partition priority for KERN-C\n");
        return ZX_ERR_OUT_OF_RANGE;
    }
    // Successful set to 'true' to encourage the bootloader to
    // use this partition.
    gpt_cros_attr_set_successful(&partition->flags, true);
    // Maximize the number of attempts to boot this partition before
    // we fall back to a different kernel.
    if (gpt_cros_attr_set_tries(&partition->flags, 15) != 0) {
        ERROR("Cannot set CrOS partition 'tries' for KERN-C\n");
        return ZX_ERR_OUT_OF_RANGE;
    }
    gpt_device_sync(gpt_->GetGpt());
    return ZX_OK;
}

zx_status_t CrosDevicePartitioner::WipePartitions(const fbl::Vector<Partition>& partitions) {
    const uint8_t fvm_type[GPT_GUID_LEN] = GUID_FVM_VALUE;
    const uint8_t install_type[GPT_GUID_LEN] = GUID_INSTALL_VALUE;
    const uint8_t system_type[GPT_GUID_LEN] = GUID_SYSTEM_VALUE;
    const uint8_t blob_type[GPT_GUID_LEN] = GUID_BLOB_VALUE;
    const uint8_t data_type[GPT_GUID_LEN] = GUID_DATA_VALUE;

    fbl::Vector<const uint8_t*> partition_list;
    for (const auto& partition_type : partitions) {
        switch (partition_type) {
        case Partition::kEfi:
            continue;
        case Partition::kFuchsiaVolumeManager:
            partition_list.push_back(fvm_type);
            break;
        case Partition::kInstallType:
            partition_list.push_back(install_type);
            break;
        case Partition::kSystem:
            partition_list.push_back(system_type);
            break;
        case Partition::kBlob:
            partition_list.push_back(blob_type);
            break;
        case Partition::kData:
            partition_list.push_back(data_type);
            break;
        default:
            return ZX_ERR_NOT_SUPPORTED;
        }
    }

    auto filter = [&](const gpt_partition_t& part) {
        for (const auto& type : partition_list) {
            if (memcmp(part.type, type, GPT_GUID_LEN) == 0) {
                return true;
            }
        }
        return false;
    };
    return gpt_->WipePartitions(filter);
}

/*====================================================*
 *                    NON-GPT                         *
 *====================================================*/

zx_status_t FixedDevicePartitioner::Initialize(fbl::unique_ptr<DevicePartitioner>* partitioner) {
    LOG("Successfully intitialized FixedDevicePartitioner Device Partitioner\n");
    *partitioner = fbl::move(WrapUnique(new FixedDevicePartitioner));
    return ZX_OK;
}

zx_status_t FixedDevicePartitioner::FindPartition(Partition partition_type,
                                                    fbl::unique_fd* out_fd) const {
    uint8_t type[GPT_GUID_LEN];

    switch (partition_type) {
    case Partition::kZirconA: {
        const uint8_t zircon_a_type[GPT_GUID_LEN] = GUID_ZIRCON_A_VALUE;
        memcpy(type, zircon_a_type, GPT_GUID_LEN);
        break;
    }
    case Partition::kZirconB: {
        const uint8_t zircon_b_type[GPT_GUID_LEN] = GUID_ZIRCON_B_VALUE;
        memcpy(type, zircon_b_type, GPT_GUID_LEN);
        break;
    }
    case Partition::kZirconR: {
        const uint8_t zircon_r_type[GPT_GUID_LEN] = GUID_ZIRCON_R_VALUE;
        memcpy(type, zircon_r_type, GPT_GUID_LEN);
        break;
    }
    case Partition::kFuchsiaVolumeManager: {
        const uint8_t fvm_type[GPT_GUID_LEN] = GUID_FVM_VALUE;
        memcpy(type, fvm_type, GPT_GUID_LEN);
        break;
    }
    default:
        ERROR("partition_type is invalid!\n");
        return ZX_ERR_NOT_SUPPORTED;
    }

    out_fd->reset(open_partition(nullptr, type, ZX_SEC(5), nullptr));
    if (!out_fd) {
        return ZX_ERR_NOT_FOUND;
    }
    return ZX_OK;
}

zx_status_t FixedDevicePartitioner::GetBlockInfo(const fbl::unique_fd& block_fd,
                                                   block_info_t* block_info) const {
    ssize_t r;
    if ((r = ioctl_block_get_info(block_fd.get(), block_info) < 0)) {
        return ZX_ERR_IO;
    }
    return ZX_OK;
}

} // namespace paver
