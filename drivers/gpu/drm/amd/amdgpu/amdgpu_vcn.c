/*
 * Copyright 2016-2024 Advanced Micro Devices, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDERS, AUTHORS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 */

#include <linux/firmware.h>
#include <linux/module.h>
#include <linux/dmi.h>
#include <linux/pci.h>
#include <linux/debugfs.h>
#include <drm/drm_drv.h>

#include "amdgpu.h"
#include "amdgpu_pm.h"
#include "amdgpu_vcn.h"
#include "soc15d.h"

/* Firmware Names */
#define FIRMWARE_RAVEN			"amdgpu/raven_vcn.bin"
#define FIRMWARE_PICASSO		"amdgpu/picasso_vcn.bin"
#define FIRMWARE_RAVEN2			"amdgpu/raven2_vcn.bin"
#define FIRMWARE_ARCTURUS		"amdgpu/arcturus_vcn.bin"
#define FIRMWARE_RENOIR			"amdgpu/renoir_vcn.bin"
#define FIRMWARE_GREEN_SARDINE		"amdgpu/green_sardine_vcn.bin"
#define FIRMWARE_NAVI10			"amdgpu/navi10_vcn.bin"
#define FIRMWARE_NAVI14			"amdgpu/navi14_vcn.bin"
#define FIRMWARE_NAVI12			"amdgpu/navi12_vcn.bin"
#define FIRMWARE_SIENNA_CICHLID		"amdgpu/sienna_cichlid_vcn.bin"
#define FIRMWARE_NAVY_FLOUNDER		"amdgpu/navy_flounder_vcn.bin"
#define FIRMWARE_VANGOGH		"amdgpu/vangogh_vcn.bin"
#define FIRMWARE_DIMGREY_CAVEFISH	"amdgpu/dimgrey_cavefish_vcn.bin"
#define FIRMWARE_ALDEBARAN		"amdgpu/aldebaran_vcn.bin"
#define FIRMWARE_BEIGE_GOBY		"amdgpu/beige_goby_vcn.bin"
#define FIRMWARE_YELLOW_CARP		"amdgpu/yellow_carp_vcn.bin"
#define FIRMWARE_VCN_3_1_2		"amdgpu/vcn_3_1_2.bin"
#define FIRMWARE_VCN4_0_0		"amdgpu/vcn_4_0_0.bin"
#define FIRMWARE_VCN4_0_2		"amdgpu/vcn_4_0_2.bin"
#define FIRMWARE_VCN4_0_3		"amdgpu/vcn_4_0_3.bin"
#define FIRMWARE_VCN4_0_4		"amdgpu/vcn_4_0_4.bin"
#define FIRMWARE_VCN4_0_5		"amdgpu/vcn_4_0_5.bin"
#define FIRMWARE_VCN4_0_6		"amdgpu/vcn_4_0_6.bin"
#define FIRMWARE_VCN4_0_6_1		"amdgpu/vcn_4_0_6_1.bin"
#define FIRMWARE_VCN5_0_0		"amdgpu/vcn_5_0_0.bin"
#define FIRMWARE_VCN5_0_1		"amdgpu/vcn_5_0_1.bin"

MODULE_FIRMWARE(FIRMWARE_RAVEN);
MODULE_FIRMWARE(FIRMWARE_PICASSO);
MODULE_FIRMWARE(FIRMWARE_RAVEN2);
MODULE_FIRMWARE(FIRMWARE_ARCTURUS);
MODULE_FIRMWARE(FIRMWARE_RENOIR);
MODULE_FIRMWARE(FIRMWARE_GREEN_SARDINE);
MODULE_FIRMWARE(FIRMWARE_ALDEBARAN);
MODULE_FIRMWARE(FIRMWARE_NAVI10);
MODULE_FIRMWARE(FIRMWARE_NAVI14);
MODULE_FIRMWARE(FIRMWARE_NAVI12);
MODULE_FIRMWARE(FIRMWARE_SIENNA_CICHLID);
MODULE_FIRMWARE(FIRMWARE_NAVY_FLOUNDER);
MODULE_FIRMWARE(FIRMWARE_VANGOGH);
MODULE_FIRMWARE(FIRMWARE_DIMGREY_CAVEFISH);
MODULE_FIRMWARE(FIRMWARE_BEIGE_GOBY);
MODULE_FIRMWARE(FIRMWARE_YELLOW_CARP);
MODULE_FIRMWARE(FIRMWARE_VCN_3_1_2);
MODULE_FIRMWARE(FIRMWARE_VCN4_0_0);
MODULE_FIRMWARE(FIRMWARE_VCN4_0_2);
MODULE_FIRMWARE(FIRMWARE_VCN4_0_3);
MODULE_FIRMWARE(FIRMWARE_VCN4_0_4);
MODULE_FIRMWARE(FIRMWARE_VCN4_0_5);
MODULE_FIRMWARE(FIRMWARE_VCN4_0_6);
MODULE_FIRMWARE(FIRMWARE_VCN4_0_6_1);
MODULE_FIRMWARE(FIRMWARE_VCN5_0_0);
MODULE_FIRMWARE(FIRMWARE_VCN5_0_1);

static void amdgpu_vcn_idle_work_handler(struct work_struct *work);

int amdgpu_vcn_early_init(struct amdgpu_device *adev, int i)
{
	char ucode_prefix[25];
	int r;

	adev->vcn.inst[i].adev = adev;
	adev->vcn.inst[i].inst = i;
	amdgpu_ucode_ip_version_decode(adev, UVD_HWIP, ucode_prefix, sizeof(ucode_prefix));

	if (i != 0 && adev->vcn.per_inst_fw) {
		r = amdgpu_ucode_request(adev, &adev->vcn.inst[i].fw,
					 AMDGPU_UCODE_REQUIRED,
					 "amdgpu/%s_%d.bin", ucode_prefix, i);
		if (r)
			amdgpu_ucode_release(&adev->vcn.inst[i].fw);
	} else {
		if (!adev->vcn.inst[0].fw) {
			r = amdgpu_ucode_request(adev, &adev->vcn.inst[0].fw,
						 AMDGPU_UCODE_REQUIRED,
						 "amdgpu/%s.bin", ucode_prefix);
			if (r)
				amdgpu_ucode_release(&adev->vcn.inst[0].fw);
		} else {
			r = 0;
		}
		adev->vcn.inst[i].fw = adev->vcn.inst[0].fw;
	}

	return r;
}

int amdgpu_vcn_sw_init(struct amdgpu_device *adev, int i)
{
	unsigned long bo_size;
	const struct common_firmware_header *hdr;
	unsigned char fw_check;
	unsigned int fw_shared_size, log_offset;
	int r;

	mutex_init(&adev->vcn.inst[i].vcn1_jpeg1_workaround);
	mutex_init(&adev->vcn.inst[i].vcn_pg_lock);
	mutex_init(&adev->vcn.inst[i].engine_reset_mutex);
	atomic_set(&adev->vcn.inst[i].total_submission_cnt, 0);
	INIT_DELAYED_WORK(&adev->vcn.inst[i].idle_work, amdgpu_vcn_idle_work_handler);
	atomic_set(&adev->vcn.inst[i].dpg_enc_submission_cnt, 0);
	if ((adev->firmware.load_type == AMDGPU_FW_LOAD_PSP) &&
	    (adev->pg_flags & AMD_PG_SUPPORT_VCN_DPG))
		adev->vcn.inst[i].indirect_sram = true;

	/*
	 * Some Steam Deck's BIOS versions are incompatible with the
	 * indirect SRAM mode, leading to amdgpu being unable to get
	 * properly probed (and even potentially crashing the kernel).
	 * Hence, check for these versions here - notice this is
	 * restricted to Vangogh (Deck's APU).
	 */
	if (amdgpu_ip_version(adev, UVD_HWIP, 0) == IP_VERSION(3, 0, 2)) {
		const char *bios_ver = dmi_get_system_info(DMI_BIOS_VERSION);

		if (bios_ver && (!strncmp("F7A0113", bios_ver, 7) ||
				 !strncmp("F7A0114", bios_ver, 7))) {
			adev->vcn.inst[i].indirect_sram = false;
			dev_info(adev->dev,
				 "Steam Deck quirk: indirect SRAM disabled on BIOS %s\n", bios_ver);
		}
	}

	/* from vcn4 and above, only unified queue is used */
	adev->vcn.inst[i].using_unified_queue =
		amdgpu_ip_version(adev, UVD_HWIP, 0) >= IP_VERSION(4, 0, 0);

	hdr = (const struct common_firmware_header *)adev->vcn.inst[i].fw->data;
	adev->vcn.inst[i].fw_version = le32_to_cpu(hdr->ucode_version);
	adev->vcn.fw_version = le32_to_cpu(hdr->ucode_version);

	/* Bit 20-23, it is encode major and non-zero for new naming convention.
	 * This field is part of version minor and DRM_DISABLED_FLAG in old naming
	 * convention. Since the l:wq!atest version minor is 0x5B and DRM_DISABLED_FLAG
	 * is zero in old naming convention, this field is always zero so far.
	 * These four bits are used to tell which naming convention is present.
	 */
	fw_check = (le32_to_cpu(hdr->ucode_version) >> 20) & 0xf;
	if (fw_check) {
		unsigned int dec_ver, enc_major, enc_minor, vep, fw_rev;

		fw_rev = le32_to_cpu(hdr->ucode_version) & 0xfff;
		enc_minor = (le32_to_cpu(hdr->ucode_version) >> 12) & 0xff;
		enc_major = fw_check;
		dec_ver = (le32_to_cpu(hdr->ucode_version) >> 24) & 0xf;
		vep = (le32_to_cpu(hdr->ucode_version) >> 28) & 0xf;
		dev_info(adev->dev,
			 "Found VCN firmware Version ENC: %u.%u DEC: %u VEP: %u Revision: %u\n",
			 enc_major, enc_minor, dec_ver, vep, fw_rev);
	} else {
		unsigned int version_major, version_minor, family_id;

		family_id = le32_to_cpu(hdr->ucode_version) & 0xff;
		version_major = (le32_to_cpu(hdr->ucode_version) >> 24) & 0xff;
		version_minor = (le32_to_cpu(hdr->ucode_version) >> 8) & 0xff;
		dev_info(adev->dev, "Found VCN firmware Version: %u.%u Family ID: %u\n",
			 version_major, version_minor, family_id);
	}

	bo_size = AMDGPU_VCN_STACK_SIZE + AMDGPU_VCN_CONTEXT_SIZE;
	if (adev->firmware.load_type != AMDGPU_FW_LOAD_PSP)
		bo_size += AMDGPU_GPU_PAGE_ALIGN(le32_to_cpu(hdr->ucode_size_bytes) + 8);

	if (amdgpu_ip_version(adev, UVD_HWIP, 0) >= IP_VERSION(5, 0, 0)) {
		fw_shared_size = AMDGPU_GPU_PAGE_ALIGN(sizeof(struct amdgpu_vcn5_fw_shared));
		log_offset = offsetof(struct amdgpu_vcn5_fw_shared, fw_log);
	} else if (amdgpu_ip_version(adev, UVD_HWIP, 0) >= IP_VERSION(4, 0, 0)) {
		fw_shared_size = AMDGPU_GPU_PAGE_ALIGN(sizeof(struct amdgpu_vcn4_fw_shared));
		log_offset = offsetof(struct amdgpu_vcn4_fw_shared, fw_log);
	} else {
		fw_shared_size = AMDGPU_GPU_PAGE_ALIGN(sizeof(struct amdgpu_fw_shared));
		log_offset = offsetof(struct amdgpu_fw_shared, fw_log);
	}

	bo_size += fw_shared_size;

	if (amdgpu_vcnfw_log)
		bo_size += AMDGPU_VCNFW_LOG_SIZE;

	r = amdgpu_bo_create_kernel(adev, bo_size, PAGE_SIZE,
				    AMDGPU_GEM_DOMAIN_VRAM |
				    AMDGPU_GEM_DOMAIN_GTT,
				    &adev->vcn.inst[i].vcpu_bo,
				    &adev->vcn.inst[i].gpu_addr,
				    &adev->vcn.inst[i].cpu_addr);
	if (r) {
		dev_err(adev->dev, "(%d) failed to allocate vcn bo\n", r);
		return r;
	}

	adev->vcn.inst[i].fw_shared.cpu_addr = adev->vcn.inst[i].cpu_addr +
		bo_size - fw_shared_size;
	adev->vcn.inst[i].fw_shared.gpu_addr = adev->vcn.inst[i].gpu_addr +
		bo_size - fw_shared_size;

	adev->vcn.inst[i].fw_shared.mem_size = fw_shared_size;

	if (amdgpu_vcnfw_log) {
		adev->vcn.inst[i].fw_shared.cpu_addr -= AMDGPU_VCNFW_LOG_SIZE;
		adev->vcn.inst[i].fw_shared.gpu_addr -= AMDGPU_VCNFW_LOG_SIZE;
		adev->vcn.inst[i].fw_shared.log_offset = log_offset;
	}

	if (adev->vcn.inst[i].indirect_sram) {
		r = amdgpu_bo_create_kernel(adev, 64 * 2 * 4, PAGE_SIZE,
					    AMDGPU_GEM_DOMAIN_VRAM |
					    AMDGPU_GEM_DOMAIN_GTT,
					    &adev->vcn.inst[i].dpg_sram_bo,
					    &adev->vcn.inst[i].dpg_sram_gpu_addr,
					    &adev->vcn.inst[i].dpg_sram_cpu_addr);
		if (r) {
			dev_err(adev->dev, "VCN %d (%d) failed to allocate DPG bo\n", i, r);
			return r;
		}
	}

	return 0;
}

int amdgpu_vcn_sw_fini(struct amdgpu_device *adev, int i)
{
	int j;

	if (adev->vcn.harvest_config & (1 << i))
		return 0;

	amdgpu_bo_free_kernel(
		&adev->vcn.inst[i].dpg_sram_bo,
		&adev->vcn.inst[i].dpg_sram_gpu_addr,
		(void **)&adev->vcn.inst[i].dpg_sram_cpu_addr);

	kvfree(adev->vcn.inst[i].saved_bo);

	amdgpu_bo_free_kernel(&adev->vcn.inst[i].vcpu_bo,
			      &adev->vcn.inst[i].gpu_addr,
			      (void **)&adev->vcn.inst[i].cpu_addr);

	amdgpu_ring_fini(&adev->vcn.inst[i].ring_dec);

	for (j = 0; j < adev->vcn.inst[i].num_enc_rings; ++j)
		amdgpu_ring_fini(&adev->vcn.inst[i].ring_enc[j]);

	if (adev->vcn.per_inst_fw) {
		amdgpu_ucode_release(&adev->vcn.inst[i].fw);
	} else {
		amdgpu_ucode_release(&adev->vcn.inst[0].fw);
		adev->vcn.inst[i].fw = NULL;
	}
	mutex_destroy(&adev->vcn.inst[i].vcn_pg_lock);
	mutex_destroy(&adev->vcn.inst[i].vcn1_jpeg1_workaround);

	return 0;
}

bool amdgpu_vcn_is_disabled_vcn(struct amdgpu_device *adev, enum vcn_ring_type type, uint32_t vcn_instance)
{
	bool ret = false;
	int vcn_config = adev->vcn.inst[vcn_instance].vcn_config;

	if ((type == VCN_ENCODE_RING) && (vcn_config & VCN_BLOCK_ENCODE_DISABLE_MASK))
		ret = true;
	else if ((type == VCN_DECODE_RING) && (vcn_config & VCN_BLOCK_DECODE_DISABLE_MASK))
		ret = true;
	else if ((type == VCN_UNIFIED_RING) && (vcn_config & VCN_BLOCK_QUEUE_DISABLE_MASK))
		ret = true;

	return ret;
}

static int amdgpu_vcn_save_vcpu_bo_inst(struct amdgpu_device *adev, int i)
{
	unsigned int size;
	void *ptr;
	int idx;

	if (adev->vcn.harvest_config & (1 << i))
		return 0;
	if (adev->vcn.inst[i].vcpu_bo == NULL)
		return 0;

	size = amdgpu_bo_size(adev->vcn.inst[i].vcpu_bo);
	ptr = adev->vcn.inst[i].cpu_addr;

	adev->vcn.inst[i].saved_bo = kvmalloc(size, GFP_KERNEL);
	if (!adev->vcn.inst[i].saved_bo)
		return -ENOMEM;

	if (drm_dev_enter(adev_to_drm(adev), &idx)) {
		memcpy_fromio(adev->vcn.inst[i].saved_bo, ptr, size);
		drm_dev_exit(idx);
	}

	return 0;
}

int amdgpu_vcn_save_vcpu_bo(struct amdgpu_device *adev)
{
	int ret, i;

	for (i = 0; i < adev->vcn.num_vcn_inst; ++i) {
		ret = amdgpu_vcn_save_vcpu_bo_inst(adev, i);
		if (ret)
			return ret;
	}

	return 0;
}

int amdgpu_vcn_suspend(struct amdgpu_device *adev, int i)
{
	bool in_ras_intr = amdgpu_ras_intr_triggered();

	if (adev->vcn.harvest_config & (1 << i))
		return 0;

	cancel_delayed_work_sync(&adev->vcn.inst[i].idle_work);

	/* err_event_athub and dpc recovery will corrupt VCPU buffer, so we need to
	 * restore fw data and clear buffer in amdgpu_vcn_resume() */
	if (in_ras_intr || adev->pcie_reset_ctx.in_link_reset)
		return 0;

	return amdgpu_vcn_save_vcpu_bo_inst(adev, i);
}

int amdgpu_vcn_resume(struct amdgpu_device *adev, int i)
{
	unsigned int size;
	void *ptr;
	int idx;

	if (adev->vcn.harvest_config & (1 << i))
		return 0;
	if (adev->vcn.inst[i].vcpu_bo == NULL)
		return -EINVAL;

	size = amdgpu_bo_size(adev->vcn.inst[i].vcpu_bo);
	ptr = adev->vcn.inst[i].cpu_addr;

	if (adev->vcn.inst[i].saved_bo != NULL) {
		if (drm_dev_enter(adev_to_drm(adev), &idx)) {
			memcpy_toio(ptr, adev->vcn.inst[i].saved_bo, size);
			drm_dev_exit(idx);
		}
		kvfree(adev->vcn.inst[i].saved_bo);
		adev->vcn.inst[i].saved_bo = NULL;
	} else {
		const struct common_firmware_header *hdr;
		unsigned int offset;

		hdr = (const struct common_firmware_header *)adev->vcn.inst[i].fw->data;
		if (adev->firmware.load_type != AMDGPU_FW_LOAD_PSP) {
			offset = le32_to_cpu(hdr->ucode_array_offset_bytes);
			if (drm_dev_enter(adev_to_drm(adev), &idx)) {
				memcpy_toio(adev->vcn.inst[i].cpu_addr,
					    adev->vcn.inst[i].fw->data + offset,
					    le32_to_cpu(hdr->ucode_size_bytes));
				drm_dev_exit(idx);
			}
			size -= le32_to_cpu(hdr->ucode_size_bytes);
			ptr += le32_to_cpu(hdr->ucode_size_bytes);
		}
		memset_io(ptr, 0, size);
	}

	return 0;
}

static void amdgpu_vcn_idle_work_handler(struct work_struct *work)
{
	struct amdgpu_vcn_inst *vcn_inst =
		container_of(work, struct amdgpu_vcn_inst, idle_work.work);
	struct amdgpu_device *adev = vcn_inst->adev;
	unsigned int fences = 0, fence[AMDGPU_MAX_VCN_INSTANCES] = {0};
	unsigned int i = vcn_inst->inst, j;
	int r = 0;

	if (adev->vcn.harvest_config & (1 << i))
		return;

	for (j = 0; j < adev->vcn.inst[i].num_enc_rings; ++j)
		fence[i] += amdgpu_fence_count_emitted(&vcn_inst->ring_enc[j]);

	/* Only set DPG pause for VCN3 or below, VCN4 and above will be handled by FW */
	if (adev->pg_flags & AMD_PG_SUPPORT_VCN_DPG &&
	    !adev->vcn.inst[i].using_unified_queue) {
		struct dpg_pause_state new_state;

		if (fence[i] ||
		    unlikely(atomic_read(&vcn_inst->dpg_enc_submission_cnt)))
			new_state.fw_based = VCN_DPG_STATE__PAUSE;
		else
			new_state.fw_based = VCN_DPG_STATE__UNPAUSE;

		adev->vcn.inst[i].pause_dpg_mode(vcn_inst, &new_state);
	}

	fence[i] += amdgpu_fence_count_emitted(&vcn_inst->ring_dec);
	fences += fence[i];

	if (!fences && !atomic_read(&vcn_inst->total_submission_cnt)) {
		vcn_inst->set_pg_state(vcn_inst, AMD_PG_STATE_GATE);
		mutex_lock(&adev->vcn.workload_profile_mutex);
		if (adev->vcn.workload_profile_active) {
			r = amdgpu_dpm_switch_power_profile(adev, PP_SMC_POWER_PROFILE_VIDEO,
							    false);
			if (r)
				dev_warn(adev->dev, "(%d) failed to disable video power profile mode\n", r);
			adev->vcn.workload_profile_active = false;
		}
		mutex_unlock(&adev->vcn.workload_profile_mutex);
	} else {
		schedule_delayed_work(&vcn_inst->idle_work, VCN_IDLE_TIMEOUT);
	}
}

void amdgpu_vcn_ring_begin_use(struct amdgpu_ring *ring)
{
	struct amdgpu_device *adev = ring->adev;
	struct amdgpu_vcn_inst *vcn_inst = &adev->vcn.inst[ring->me];
	int r = 0;

	atomic_inc(&vcn_inst->total_submission_cnt);

	cancel_delayed_work_sync(&vcn_inst->idle_work);

	/* We can safely return early here because we've cancelled the
	 * the delayed work so there is no one else to set it to false
	 * and we don't care if someone else sets it to true.
	 */
	if (adev->vcn.workload_profile_active)
		goto pg_lock;

	mutex_lock(&adev->vcn.workload_profile_mutex);
	if (!adev->vcn.workload_profile_active) {
		r = amdgpu_dpm_switch_power_profile(adev, PP_SMC_POWER_PROFILE_VIDEO,
						    true);
		if (r)
			dev_warn(adev->dev, "(%d) failed to switch to video power profile mode\n", r);
		adev->vcn.workload_profile_active = true;
	}
	mutex_unlock(&adev->vcn.workload_profile_mutex);

pg_lock:
	mutex_lock(&vcn_inst->vcn_pg_lock);
	vcn_inst->set_pg_state(vcn_inst, AMD_PG_STATE_UNGATE);

	/* Only set DPG pause for VCN3 or below, VCN4 and above will be handled by FW */
	if (adev->pg_flags & AMD_PG_SUPPORT_VCN_DPG &&
	    !vcn_inst->using_unified_queue) {
		struct dpg_pause_state new_state;

		if (ring->funcs->type == AMDGPU_RING_TYPE_VCN_ENC) {
			atomic_inc(&vcn_inst->dpg_enc_submission_cnt);
			new_state.fw_based = VCN_DPG_STATE__PAUSE;
		} else {
			unsigned int fences = 0;
			unsigned int i;

			for (i = 0; i < vcn_inst->num_enc_rings; ++i)
				fences += amdgpu_fence_count_emitted(&vcn_inst->ring_enc[i]);

			if (fences || atomic_read(&vcn_inst->dpg_enc_submission_cnt))
				new_state.fw_based = VCN_DPG_STATE__PAUSE;
			else
				new_state.fw_based = VCN_DPG_STATE__UNPAUSE;
		}

		vcn_inst->pause_dpg_mode(vcn_inst, &new_state);
	}
	mutex_unlock(&vcn_inst->vcn_pg_lock);
}

void amdgpu_vcn_ring_end_use(struct amdgpu_ring *ring)
{
	struct amdgpu_device *adev = ring->adev;

	/* Only set DPG pause for VCN3 or below, VCN4 and above will be handled by FW */
	if (ring->adev->pg_flags & AMD_PG_SUPPORT_VCN_DPG &&
	    ring->funcs->type == AMDGPU_RING_TYPE_VCN_ENC &&
	    !adev->vcn.inst[ring->me].using_unified_queue)
		atomic_dec(&ring->adev->vcn.inst[ring->me].dpg_enc_submission_cnt);

	atomic_dec(&ring->adev->vcn.inst[ring->me].total_submission_cnt);

	schedule_delayed_work(&ring->adev->vcn.inst[ring->me].idle_work,
			      VCN_IDLE_TIMEOUT);
}

int amdgpu_vcn_dec_ring_test_ring(struct amdgpu_ring *ring)
{
	struct amdgpu_device *adev = ring->adev;
	uint32_t tmp = 0;
	unsigned int i;
	int r;

	/* VCN in SRIOV does not support direct register read/write */
	if (amdgpu_sriov_vf(adev))
		return 0;

	WREG32(adev->vcn.inst[ring->me].external.scratch9, 0xCAFEDEAD);
	r = amdgpu_ring_alloc(ring, 3);
	if (r)
		return r;
	amdgpu_ring_write(ring, PACKET0(adev->vcn.inst[ring->me].internal.scratch9, 0));
	amdgpu_ring_write(ring, 0xDEADBEEF);
	amdgpu_ring_commit(ring);
	for (i = 0; i < adev->usec_timeout; i++) {
		tmp = RREG32(adev->vcn.inst[ring->me].external.scratch9);
		if (tmp == 0xDEADBEEF)
			break;
		udelay(1);
	}

	if (i >= adev->usec_timeout)
		r = -ETIMEDOUT;

	return r;
}

int amdgpu_vcn_dec_sw_ring_test_ring(struct amdgpu_ring *ring)
{
	struct amdgpu_device *adev = ring->adev;
	uint32_t rptr;
	unsigned int i;
	int r;

	if (amdgpu_sriov_vf(adev))
		return 0;

	r = amdgpu_ring_alloc(ring, 16);
	if (r)
		return r;

	rptr = amdgpu_ring_get_rptr(ring);

	amdgpu_ring_write(ring, VCN_DEC_SW_CMD_END);
	amdgpu_ring_commit(ring);

	for (i = 0; i < adev->usec_timeout; i++) {
		if (amdgpu_ring_get_rptr(ring) != rptr)
			break;
		udelay(1);
	}

	if (i >= adev->usec_timeout)
		r = -ETIMEDOUT;

	return r;
}

static int amdgpu_vcn_dec_send_msg(struct amdgpu_ring *ring,
				   struct amdgpu_ib *ib_msg,
				   struct dma_fence **fence)
{
	u64 addr = AMDGPU_GPU_PAGE_ALIGN(ib_msg->gpu_addr);
	struct amdgpu_device *adev = ring->adev;
	struct dma_fence *f = NULL;
	struct amdgpu_job *job;
	struct amdgpu_ib *ib;
	int i, r;

	r = amdgpu_job_alloc_with_ib(ring->adev, NULL, NULL,
				     64, AMDGPU_IB_POOL_DIRECT,
				     &job);
	if (r)
		goto err;

	ib = &job->ibs[0];
	ib->ptr[0] = PACKET0(adev->vcn.inst[ring->me].internal.data0, 0);
	ib->ptr[1] = addr;
	ib->ptr[2] = PACKET0(adev->vcn.inst[ring->me].internal.data1, 0);
	ib->ptr[3] = addr >> 32;
	ib->ptr[4] = PACKET0(adev->vcn.inst[ring->me].internal.cmd, 0);
	ib->ptr[5] = 0;
	for (i = 6; i < 16; i += 2) {
		ib->ptr[i] = PACKET0(adev->vcn.inst[ring->me].internal.nop, 0);
		ib->ptr[i+1] = 0;
	}
	ib->length_dw = 16;

	r = amdgpu_job_submit_direct(job, ring, &f);
	if (r)
		goto err_free;

	amdgpu_ib_free(ib_msg, f);

	if (fence)
		*fence = dma_fence_get(f);
	dma_fence_put(f);

	return 0;

err_free:
	amdgpu_job_free(job);
err:
	amdgpu_ib_free(ib_msg, f);
	return r;
}

static int amdgpu_vcn_dec_get_create_msg(struct amdgpu_ring *ring, uint32_t handle,
		struct amdgpu_ib *ib)
{
	struct amdgpu_device *adev = ring->adev;
	uint32_t *msg;
	int r, i;

	memset(ib, 0, sizeof(*ib));
	r = amdgpu_ib_get(adev, NULL, AMDGPU_GPU_PAGE_SIZE * 2,
			AMDGPU_IB_POOL_DIRECT,
			ib);
	if (r)
		return r;

	msg = (uint32_t *)AMDGPU_GPU_PAGE_ALIGN((unsigned long)ib->ptr);
	msg[0] = cpu_to_le32(0x00000028);
	msg[1] = cpu_to_le32(0x00000038);
	msg[2] = cpu_to_le32(0x00000001);
	msg[3] = cpu_to_le32(0x00000000);
	msg[4] = cpu_to_le32(handle);
	msg[5] = cpu_to_le32(0x00000000);
	msg[6] = cpu_to_le32(0x00000001);
	msg[7] = cpu_to_le32(0x00000028);
	msg[8] = cpu_to_le32(0x00000010);
	msg[9] = cpu_to_le32(0x00000000);
	msg[10] = cpu_to_le32(0x00000007);
	msg[11] = cpu_to_le32(0x00000000);
	msg[12] = cpu_to_le32(0x00000780);
	msg[13] = cpu_to_le32(0x00000440);
	for (i = 14; i < 1024; ++i)
		msg[i] = cpu_to_le32(0x0);

	return 0;
}

static int amdgpu_vcn_dec_get_destroy_msg(struct amdgpu_ring *ring, uint32_t handle,
					  struct amdgpu_ib *ib)
{
	struct amdgpu_device *adev = ring->adev;
	uint32_t *msg;
	int r, i;

	memset(ib, 0, sizeof(*ib));
	r = amdgpu_ib_get(adev, NULL, AMDGPU_GPU_PAGE_SIZE * 2,
			AMDGPU_IB_POOL_DIRECT,
			ib);
	if (r)
		return r;

	msg = (uint32_t *)AMDGPU_GPU_PAGE_ALIGN((unsigned long)ib->ptr);
	msg[0] = cpu_to_le32(0x00000028);
	msg[1] = cpu_to_le32(0x00000018);
	msg[2] = cpu_to_le32(0x00000000);
	msg[3] = cpu_to_le32(0x00000002);
	msg[4] = cpu_to_le32(handle);
	msg[5] = cpu_to_le32(0x00000000);
	for (i = 6; i < 1024; ++i)
		msg[i] = cpu_to_le32(0x0);

	return 0;
}

int amdgpu_vcn_dec_ring_test_ib(struct amdgpu_ring *ring, long timeout)
{
	struct dma_fence *fence = NULL;
	struct amdgpu_ib ib;
	long r;

	r = amdgpu_vcn_dec_get_create_msg(ring, 1, &ib);
	if (r)
		goto error;

	r = amdgpu_vcn_dec_send_msg(ring, &ib, NULL);
	if (r)
		goto error;
	r = amdgpu_vcn_dec_get_destroy_msg(ring, 1, &ib);
	if (r)
		goto error;

	r = amdgpu_vcn_dec_send_msg(ring, &ib, &fence);
	if (r)
		goto error;

	r = dma_fence_wait_timeout(fence, false, timeout);
	if (r == 0)
		r = -ETIMEDOUT;
	else if (r > 0)
		r = 0;

	dma_fence_put(fence);
error:
	return r;
}

static uint32_t *amdgpu_vcn_unified_ring_ib_header(struct amdgpu_ib *ib,
						uint32_t ib_pack_in_dw, bool enc)
{
	uint32_t *ib_checksum;

	ib->ptr[ib->length_dw++] = 0x00000010; /* single queue checksum */
	ib->ptr[ib->length_dw++] = 0x30000002;
	ib_checksum = &ib->ptr[ib->length_dw++];
	ib->ptr[ib->length_dw++] = ib_pack_in_dw;

	ib->ptr[ib->length_dw++] = 0x00000010; /* engine info */
	ib->ptr[ib->length_dw++] = 0x30000001;
	ib->ptr[ib->length_dw++] = enc ? 0x2 : 0x3;
	ib->ptr[ib->length_dw++] = ib_pack_in_dw * sizeof(uint32_t);

	return ib_checksum;
}

static void amdgpu_vcn_unified_ring_ib_checksum(uint32_t **ib_checksum,
						uint32_t ib_pack_in_dw)
{
	uint32_t i;
	uint32_t checksum = 0;

	for (i = 0; i < ib_pack_in_dw; i++)
		checksum += *(*ib_checksum + 2 + i);

	**ib_checksum = checksum;
}

static int amdgpu_vcn_dec_sw_send_msg(struct amdgpu_ring *ring,
				      struct amdgpu_ib *ib_msg,
				      struct dma_fence **fence)
{
	struct amdgpu_vcn_decode_buffer *decode_buffer = NULL;
	unsigned int ib_size_dw = 64;
	struct amdgpu_device *adev = ring->adev;
	struct dma_fence *f = NULL;
	struct amdgpu_job *job;
	struct amdgpu_ib *ib;
	uint64_t addr = AMDGPU_GPU_PAGE_ALIGN(ib_msg->gpu_addr);
	uint32_t *ib_checksum;
	uint32_t ib_pack_in_dw;
	int i, r;

	if (adev->vcn.inst[ring->me].using_unified_queue)
		ib_size_dw += 8;

	r = amdgpu_job_alloc_with_ib(ring->adev, NULL, NULL,
				     ib_size_dw * 4, AMDGPU_IB_POOL_DIRECT,
				     &job);
	if (r)
		goto err;

	ib = &job->ibs[0];
	ib->length_dw = 0;

	/* single queue headers */
	if (adev->vcn.inst[ring->me].using_unified_queue) {
		ib_pack_in_dw = sizeof(struct amdgpu_vcn_decode_buffer) / sizeof(uint32_t)
						+ 4 + 2; /* engine info + decoding ib in dw */
		ib_checksum = amdgpu_vcn_unified_ring_ib_header(ib, ib_pack_in_dw, false);
	}

	ib->ptr[ib->length_dw++] = sizeof(struct amdgpu_vcn_decode_buffer) + 8;
	ib->ptr[ib->length_dw++] = cpu_to_le32(AMDGPU_VCN_IB_FLAG_DECODE_BUFFER);
	decode_buffer = (struct amdgpu_vcn_decode_buffer *)&(ib->ptr[ib->length_dw]);
	ib->length_dw += sizeof(struct amdgpu_vcn_decode_buffer) / 4;
	memset(decode_buffer, 0, sizeof(struct amdgpu_vcn_decode_buffer));

	decode_buffer->valid_buf_flag |= cpu_to_le32(AMDGPU_VCN_CMD_FLAG_MSG_BUFFER);
	decode_buffer->msg_buffer_address_hi = cpu_to_le32(addr >> 32);
	decode_buffer->msg_buffer_address_lo = cpu_to_le32(addr);

	for (i = ib->length_dw; i < ib_size_dw; ++i)
		ib->ptr[i] = 0x0;

	if (adev->vcn.inst[ring->me].using_unified_queue)
		amdgpu_vcn_unified_ring_ib_checksum(&ib_checksum, ib_pack_in_dw);

	r = amdgpu_job_submit_direct(job, ring, &f);
	if (r)
		goto err_free;

	amdgpu_ib_free(ib_msg, f);

	if (fence)
		*fence = dma_fence_get(f);
	dma_fence_put(f);

	return 0;

err_free:
	amdgpu_job_free(job);
err:
	amdgpu_ib_free(ib_msg, f);
	return r;
}

int amdgpu_vcn_dec_sw_ring_test_ib(struct amdgpu_ring *ring, long timeout)
{
	struct dma_fence *fence = NULL;
	struct amdgpu_ib ib;
	long r;

	r = amdgpu_vcn_dec_get_create_msg(ring, 1, &ib);
	if (r)
		goto error;

	r = amdgpu_vcn_dec_sw_send_msg(ring, &ib, NULL);
	if (r)
		goto error;
	r = amdgpu_vcn_dec_get_destroy_msg(ring, 1, &ib);
	if (r)
		goto error;

	r = amdgpu_vcn_dec_sw_send_msg(ring, &ib, &fence);
	if (r)
		goto error;

	r = dma_fence_wait_timeout(fence, false, timeout);
	if (r == 0)
		r = -ETIMEDOUT;
	else if (r > 0)
		r = 0;

	dma_fence_put(fence);
error:
	return r;
}

int amdgpu_vcn_enc_ring_test_ring(struct amdgpu_ring *ring)
{
	struct amdgpu_device *adev = ring->adev;
	uint32_t rptr;
	unsigned int i;
	int r;

	if (amdgpu_sriov_vf(adev))
		return 0;

	r = amdgpu_ring_alloc(ring, 16);
	if (r)
		return r;

	rptr = amdgpu_ring_get_rptr(ring);

	amdgpu_ring_write(ring, VCN_ENC_CMD_END);
	amdgpu_ring_commit(ring);

	for (i = 0; i < adev->usec_timeout; i++) {
		if (amdgpu_ring_get_rptr(ring) != rptr)
			break;
		udelay(1);
	}

	if (i >= adev->usec_timeout)
		r = -ETIMEDOUT;

	return r;
}

static int amdgpu_vcn_enc_get_create_msg(struct amdgpu_ring *ring, uint32_t handle,
					 struct amdgpu_ib *ib_msg,
					 struct dma_fence **fence)
{
	unsigned int ib_size_dw = 16;
	struct amdgpu_device *adev = ring->adev;
	struct amdgpu_job *job;
	struct amdgpu_ib *ib;
	struct dma_fence *f = NULL;
	uint32_t *ib_checksum = NULL;
	uint64_t addr;
	int i, r;

	if (adev->vcn.inst[ring->me].using_unified_queue)
		ib_size_dw += 8;

	r = amdgpu_job_alloc_with_ib(ring->adev, NULL, NULL,
				     ib_size_dw * 4, AMDGPU_IB_POOL_DIRECT,
				     &job);
	if (r)
		return r;

	ib = &job->ibs[0];
	addr = AMDGPU_GPU_PAGE_ALIGN(ib_msg->gpu_addr);

	ib->length_dw = 0;

	if (adev->vcn.inst[ring->me].using_unified_queue)
		ib_checksum = amdgpu_vcn_unified_ring_ib_header(ib, 0x11, true);

	ib->ptr[ib->length_dw++] = 0x00000018;
	ib->ptr[ib->length_dw++] = 0x00000001; /* session info */
	ib->ptr[ib->length_dw++] = handle;
	ib->ptr[ib->length_dw++] = upper_32_bits(addr);
	ib->ptr[ib->length_dw++] = addr;
	ib->ptr[ib->length_dw++] = 0x00000000;

	ib->ptr[ib->length_dw++] = 0x00000014;
	ib->ptr[ib->length_dw++] = 0x00000002; /* task info */
	ib->ptr[ib->length_dw++] = 0x0000001c;
	ib->ptr[ib->length_dw++] = 0x00000000;
	ib->ptr[ib->length_dw++] = 0x00000000;

	ib->ptr[ib->length_dw++] = 0x00000008;
	ib->ptr[ib->length_dw++] = 0x08000001; /* op initialize */

	for (i = ib->length_dw; i < ib_size_dw; ++i)
		ib->ptr[i] = 0x0;

	if (adev->vcn.inst[ring->me].using_unified_queue)
		amdgpu_vcn_unified_ring_ib_checksum(&ib_checksum, 0x11);

	r = amdgpu_job_submit_direct(job, ring, &f);
	if (r)
		goto err;

	if (fence)
		*fence = dma_fence_get(f);
	dma_fence_put(f);

	return 0;

err:
	amdgpu_job_free(job);
	return r;
}

static int amdgpu_vcn_enc_get_destroy_msg(struct amdgpu_ring *ring, uint32_t handle,
					  struct amdgpu_ib *ib_msg,
					  struct dma_fence **fence)
{
	unsigned int ib_size_dw = 16;
	struct amdgpu_device *adev = ring->adev;
	struct amdgpu_job *job;
	struct amdgpu_ib *ib;
	struct dma_fence *f = NULL;
	uint32_t *ib_checksum = NULL;
	uint64_t addr;
	int i, r;

	if (adev->vcn.inst[ring->me].using_unified_queue)
		ib_size_dw += 8;

	r = amdgpu_job_alloc_with_ib(ring->adev, NULL, NULL,
				     ib_size_dw * 4, AMDGPU_IB_POOL_DIRECT,
				     &job);
	if (r)
		return r;

	ib = &job->ibs[0];
	addr = AMDGPU_GPU_PAGE_ALIGN(ib_msg->gpu_addr);

	ib->length_dw = 0;

	if (adev->vcn.inst[ring->me].using_unified_queue)
		ib_checksum = amdgpu_vcn_unified_ring_ib_header(ib, 0x11, true);

	ib->ptr[ib->length_dw++] = 0x00000018;
	ib->ptr[ib->length_dw++] = 0x00000001;
	ib->ptr[ib->length_dw++] = handle;
	ib->ptr[ib->length_dw++] = upper_32_bits(addr);
	ib->ptr[ib->length_dw++] = addr;
	ib->ptr[ib->length_dw++] = 0x00000000;

	ib->ptr[ib->length_dw++] = 0x00000014;
	ib->ptr[ib->length_dw++] = 0x00000002;
	ib->ptr[ib->length_dw++] = 0x0000001c;
	ib->ptr[ib->length_dw++] = 0x00000000;
	ib->ptr[ib->length_dw++] = 0x00000000;

	ib->ptr[ib->length_dw++] = 0x00000008;
	ib->ptr[ib->length_dw++] = 0x08000002; /* op close session */

	for (i = ib->length_dw; i < ib_size_dw; ++i)
		ib->ptr[i] = 0x0;

	if (adev->vcn.inst[ring->me].using_unified_queue)
		amdgpu_vcn_unified_ring_ib_checksum(&ib_checksum, 0x11);

	r = amdgpu_job_submit_direct(job, ring, &f);
	if (r)
		goto err;

	if (fence)
		*fence = dma_fence_get(f);
	dma_fence_put(f);

	return 0;

err:
	amdgpu_job_free(job);
	return r;
}

int amdgpu_vcn_enc_ring_test_ib(struct amdgpu_ring *ring, long timeout)
{
	struct amdgpu_device *adev = ring->adev;
	struct dma_fence *fence = NULL;
	struct amdgpu_ib ib;
	long r;

	memset(&ib, 0, sizeof(ib));
	r = amdgpu_ib_get(adev, NULL, (128 << 10) + AMDGPU_GPU_PAGE_SIZE,
			AMDGPU_IB_POOL_DIRECT,
			&ib);
	if (r)
		return r;

	r = amdgpu_vcn_enc_get_create_msg(ring, 1, &ib, NULL);
	if (r)
		goto error;

	r = amdgpu_vcn_enc_get_destroy_msg(ring, 1, &ib, &fence);
	if (r)
		goto error;

	r = dma_fence_wait_timeout(fence, false, timeout);
	if (r == 0)
		r = -ETIMEDOUT;
	else if (r > 0)
		r = 0;

error:
	amdgpu_ib_free(&ib, fence);
	dma_fence_put(fence);

	return r;
}

int amdgpu_vcn_unified_ring_test_ib(struct amdgpu_ring *ring, long timeout)
{
	struct amdgpu_device *adev = ring->adev;
	long r;

	if ((amdgpu_ip_version(adev, UVD_HWIP, 0) != IP_VERSION(4, 0, 3)) &&
	    (amdgpu_ip_version(adev, UVD_HWIP, 0) != IP_VERSION(5, 0, 1))) {
		r = amdgpu_vcn_enc_ring_test_ib(ring, timeout);
		if (r)
			goto error;
	}

	r =  amdgpu_vcn_dec_sw_ring_test_ib(ring, timeout);

error:
	return r;
}

enum amdgpu_ring_priority_level amdgpu_vcn_get_enc_ring_prio(int ring)
{
	switch (ring) {
	case 0:
		return AMDGPU_RING_PRIO_0;
	case 1:
		return AMDGPU_RING_PRIO_1;
	case 2:
		return AMDGPU_RING_PRIO_2;
	default:
		return AMDGPU_RING_PRIO_0;
	}
}

void amdgpu_vcn_setup_ucode(struct amdgpu_device *adev, int i)
{
	unsigned int idx;

	if (adev->firmware.load_type == AMDGPU_FW_LOAD_PSP) {
		const struct common_firmware_header *hdr;

		if (adev->vcn.harvest_config & (1 << i))
			return;

		if ((amdgpu_ip_version(adev, UVD_HWIP, 0) == IP_VERSION(4, 0, 3) ||
		     amdgpu_ip_version(adev, UVD_HWIP, 0) == IP_VERSION(5, 0, 1))
		    && (i > 0))
			return;

		hdr = (const struct common_firmware_header *)adev->vcn.inst[i].fw->data;
		/* currently only support 2 FW instances */
		if (i >= 2) {
			dev_info(adev->dev, "More then 2 VCN FW instances!\n");
			return;
		}
		idx = AMDGPU_UCODE_ID_VCN + i;
		adev->firmware.ucode[idx].ucode_id = idx;
		adev->firmware.ucode[idx].fw = adev->vcn.inst[i].fw;
		adev->firmware.fw_size +=
			ALIGN(le32_to_cpu(hdr->ucode_size_bytes), PAGE_SIZE);
	}
}

/*
 * debugfs for mapping vcn firmware log buffer.
 */
#if defined(CONFIG_DEBUG_FS)
static ssize_t amdgpu_debugfs_vcn_fwlog_read(struct file *f, char __user *buf,
					     size_t size, loff_t *pos)
{
	struct amdgpu_vcn_inst *vcn;
	void *log_buf;
	volatile struct amdgpu_vcn_fwlog *plog;
	unsigned int read_pos, write_pos, available, i, read_bytes = 0;
	unsigned int read_num[2] = {0};

	vcn = file_inode(f)->i_private;
	if (!vcn)
		return -ENODEV;

	if (!vcn->fw_shared.cpu_addr || !amdgpu_vcnfw_log)
		return -EFAULT;

	log_buf = vcn->fw_shared.cpu_addr + vcn->fw_shared.mem_size;

	plog = (volatile struct amdgpu_vcn_fwlog *)log_buf;
	read_pos = plog->rptr;
	write_pos = plog->wptr;

	if (read_pos > AMDGPU_VCNFW_LOG_SIZE || write_pos > AMDGPU_VCNFW_LOG_SIZE)
		return -EFAULT;

	if (!size || (read_pos == write_pos))
		return 0;

	if (write_pos > read_pos) {
		available = write_pos - read_pos;
		read_num[0] = min_t(size_t, size, available);
	} else {
		read_num[0] = AMDGPU_VCNFW_LOG_SIZE - read_pos;
		available = read_num[0] + write_pos - plog->header_size;
		if (size > available)
			read_num[1] = write_pos - plog->header_size;
		else if (size > read_num[0])
			read_num[1] = size - read_num[0];
		else
			read_num[0] = size;
	}

	for (i = 0; i < 2; i++) {
		if (read_num[i]) {
			if (read_pos == AMDGPU_VCNFW_LOG_SIZE)
				read_pos = plog->header_size;
			if (read_num[i] == copy_to_user((buf + read_bytes),
							(log_buf + read_pos), read_num[i]))
				return -EFAULT;

			read_bytes += read_num[i];
			read_pos += read_num[i];
		}
	}

	plog->rptr = read_pos;
	*pos += read_bytes;
	return read_bytes;
}

static const struct file_operations amdgpu_debugfs_vcnfwlog_fops = {
	.owner = THIS_MODULE,
	.read = amdgpu_debugfs_vcn_fwlog_read,
	.llseek = default_llseek
};
#endif

void amdgpu_debugfs_vcn_fwlog_init(struct amdgpu_device *adev, uint8_t i,
				   struct amdgpu_vcn_inst *vcn)
{
#if defined(CONFIG_DEBUG_FS)
	struct drm_minor *minor = adev_to_drm(adev)->primary;
	struct dentry *root = minor->debugfs_root;
	char name[32];

	sprintf(name, "amdgpu_vcn_%d_fwlog", i);
	debugfs_create_file_size(name, S_IFREG | 0444, root, vcn,
				 &amdgpu_debugfs_vcnfwlog_fops,
				 AMDGPU_VCNFW_LOG_SIZE);
#endif
}

void amdgpu_vcn_fwlog_init(struct amdgpu_vcn_inst *vcn)
{
#if defined(CONFIG_DEBUG_FS)
	volatile uint32_t *flag = vcn->fw_shared.cpu_addr;
	void *fw_log_cpu_addr = vcn->fw_shared.cpu_addr + vcn->fw_shared.mem_size;
	uint64_t fw_log_gpu_addr = vcn->fw_shared.gpu_addr + vcn->fw_shared.mem_size;
	volatile struct amdgpu_vcn_fwlog *log_buf = fw_log_cpu_addr;
	volatile struct amdgpu_fw_shared_fw_logging *fw_log = vcn->fw_shared.cpu_addr
							 + vcn->fw_shared.log_offset;
	*flag |= cpu_to_le32(AMDGPU_VCN_FW_LOGGING_FLAG);
	fw_log->is_enabled = 1;
	fw_log->addr_lo = cpu_to_le32(fw_log_gpu_addr & 0xFFFFFFFF);
	fw_log->addr_hi = cpu_to_le32(fw_log_gpu_addr >> 32);
	fw_log->size = cpu_to_le32(AMDGPU_VCNFW_LOG_SIZE);

	log_buf->header_size = sizeof(struct amdgpu_vcn_fwlog);
	log_buf->buffer_size = AMDGPU_VCNFW_LOG_SIZE;
	log_buf->rptr = log_buf->header_size;
	log_buf->wptr = log_buf->header_size;
	log_buf->wrapped = 0;
#endif
}

int amdgpu_vcn_process_poison_irq(struct amdgpu_device *adev,
				struct amdgpu_irq_src *source,
				struct amdgpu_iv_entry *entry)
{
	struct ras_common_if *ras_if = adev->vcn.ras_if;
	struct ras_dispatch_if ih_data = {
		.entry = entry,
	};

	if (!ras_if)
		return 0;

	if (!amdgpu_sriov_vf(adev)) {
		ih_data.head = *ras_if;
		amdgpu_ras_interrupt_dispatch(adev, &ih_data);
	} else {
		if (adev->virt.ops && adev->virt.ops->ras_poison_handler)
			adev->virt.ops->ras_poison_handler(adev, ras_if->block);
		else
			dev_warn(adev->dev,
				"No ras_poison_handler interface in SRIOV for VCN!\n");
	}

	return 0;
}

int amdgpu_vcn_ras_late_init(struct amdgpu_device *adev, struct ras_common_if *ras_block)
{
	int r, i;

	r = amdgpu_ras_block_late_init(adev, ras_block);
	if (r)
		return r;

	if (amdgpu_ras_is_supported(adev, ras_block->block)) {
		for (i = 0; i < adev->vcn.num_vcn_inst; i++) {
			if (adev->vcn.harvest_config & (1 << i) ||
			    !adev->vcn.inst[i].ras_poison_irq.funcs)
				continue;

			r = amdgpu_irq_get(adev, &adev->vcn.inst[i].ras_poison_irq, 0);
			if (r)
				goto late_fini;
		}
	}
	return 0;

late_fini:
	amdgpu_ras_block_late_fini(adev, ras_block);
	return r;
}

int amdgpu_vcn_ras_sw_init(struct amdgpu_device *adev)
{
	int err;
	struct amdgpu_vcn_ras *ras;

	if (!adev->vcn.ras)
		return 0;

	ras = adev->vcn.ras;
	err = amdgpu_ras_register_ras_block(adev, &ras->ras_block);
	if (err) {
		dev_err(adev->dev, "Failed to register vcn ras block!\n");
		return err;
	}

	strcpy(ras->ras_block.ras_comm.name, "vcn");
	ras->ras_block.ras_comm.block = AMDGPU_RAS_BLOCK__VCN;
	ras->ras_block.ras_comm.type = AMDGPU_RAS_ERROR__POISON;
	adev->vcn.ras_if = &ras->ras_block.ras_comm;

	if (!ras->ras_block.ras_late_init)
		ras->ras_block.ras_late_init = amdgpu_vcn_ras_late_init;

	return 0;
}

int amdgpu_vcn_psp_update_sram(struct amdgpu_device *adev, int inst_idx,
			       enum AMDGPU_UCODE_ID ucode_id)
{
	struct amdgpu_firmware_info ucode = {
		.ucode_id = (ucode_id ? ucode_id :
			    (inst_idx ? AMDGPU_UCODE_ID_VCN1_RAM :
					AMDGPU_UCODE_ID_VCN0_RAM)),
		.mc_addr = adev->vcn.inst[inst_idx].dpg_sram_gpu_addr,
		.ucode_size = ((uintptr_t)adev->vcn.inst[inst_idx].dpg_sram_curr_addr -
			      (uintptr_t)adev->vcn.inst[inst_idx].dpg_sram_cpu_addr),
	};

	return psp_execute_ip_fw_load(&adev->psp, &ucode);
}

static ssize_t amdgpu_get_vcn_reset_mask(struct device *dev,
						struct device_attribute *attr,
						char *buf)
{
	struct drm_device *ddev = dev_get_drvdata(dev);
	struct amdgpu_device *adev = drm_to_adev(ddev);

	if (!adev)
		return -ENODEV;

	return amdgpu_show_reset_mask(buf, adev->vcn.supported_reset);
}

static DEVICE_ATTR(vcn_reset_mask, 0444,
		   amdgpu_get_vcn_reset_mask, NULL);

int amdgpu_vcn_sysfs_reset_mask_init(struct amdgpu_device *adev)
{
	int r = 0;

	if (adev->vcn.num_vcn_inst) {
		r = device_create_file(adev->dev, &dev_attr_vcn_reset_mask);
		if (r)
			return r;
	}

	return r;
}

void amdgpu_vcn_sysfs_reset_mask_fini(struct amdgpu_device *adev)
{
	if (adev->dev->kobj.sd) {
		if (adev->vcn.num_vcn_inst)
			device_remove_file(adev->dev, &dev_attr_vcn_reset_mask);
	}
}

/*
 * debugfs to enable/disable vcn job submission to specific core or
 * instance. It is created only if the queue type is unified.
 */
#if defined(CONFIG_DEBUG_FS)
static int amdgpu_debugfs_vcn_sched_mask_set(void *data, u64 val)
{
	struct amdgpu_device *adev = (struct amdgpu_device *)data;
	u32 i;
	u64 mask;
	struct amdgpu_ring *ring;

	if (!adev)
		return -ENODEV;

	mask = (1ULL << adev->vcn.num_vcn_inst) - 1;
	if ((val & mask) == 0)
		return -EINVAL;
	for (i = 0; i < adev->vcn.num_vcn_inst; ++i) {
		ring = &adev->vcn.inst[i].ring_enc[0];
		if (val & (1ULL << i))
			ring->sched.ready = true;
		else
			ring->sched.ready = false;
	}
	/* publish sched.ready flag update effective immediately across smp */
	smp_rmb();
	return 0;
}

static int amdgpu_debugfs_vcn_sched_mask_get(void *data, u64 *val)
{
	struct amdgpu_device *adev = (struct amdgpu_device *)data;
	u32 i;
	u64 mask = 0;
	struct amdgpu_ring *ring;

	if (!adev)
		return -ENODEV;
	for (i = 0; i < adev->vcn.num_vcn_inst; ++i) {
		ring = &adev->vcn.inst[i].ring_enc[0];
		if (ring->sched.ready)
			mask |= 1ULL << i;
		}
	*val = mask;
	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(amdgpu_debugfs_vcn_sched_mask_fops,
			 amdgpu_debugfs_vcn_sched_mask_get,
			 amdgpu_debugfs_vcn_sched_mask_set, "%llx\n");
#endif

void amdgpu_debugfs_vcn_sched_mask_init(struct amdgpu_device *adev)
{
#if defined(CONFIG_DEBUG_FS)
	struct drm_minor *minor = adev_to_drm(adev)->primary;
	struct dentry *root = minor->debugfs_root;
	char name[32];

	if (adev->vcn.num_vcn_inst <= 1 || !adev->vcn.inst[0].using_unified_queue)
		return;
	sprintf(name, "amdgpu_vcn_sched_mask");
	debugfs_create_file(name, 0600, root, adev,
			    &amdgpu_debugfs_vcn_sched_mask_fops);
#endif
}

/**
 * vcn_set_powergating_state - set VCN block powergating state
 *
 * @ip_block: amdgpu_ip_block pointer
 * @state: power gating state
 *
 * Set VCN block powergating state
 */
int vcn_set_powergating_state(struct amdgpu_ip_block *ip_block,
			      enum amd_powergating_state state)
{
	struct amdgpu_device *adev = ip_block->adev;
	int ret = 0, i;

	for (i = 0; i < adev->vcn.num_vcn_inst; ++i) {
		struct amdgpu_vcn_inst *vinst = &adev->vcn.inst[i];

		ret |= vinst->set_pg_state(vinst, state);
	}

	return ret;
}

/**
 * amdgpu_vcn_reset_engine - Reset a specific VCN engine
 * @adev: Pointer to the AMDGPU device
 * @instance_id: VCN engine instance to reset
 *
 * Returns: 0 on success, or a negative error code on failure.
 */
static int amdgpu_vcn_reset_engine(struct amdgpu_device *adev,
				   uint32_t instance_id)
{
	struct amdgpu_vcn_inst *vinst = &adev->vcn.inst[instance_id];
	int r, i;

	mutex_lock(&vinst->engine_reset_mutex);
	/* Stop the scheduler's work queue for the dec and enc rings if they are running.
	 * This ensures that no new tasks are submitted to the queues while
	 * the reset is in progress.
	 */
	drm_sched_wqueue_stop(&vinst->ring_dec.sched);
	for (i = 0; i < vinst->num_enc_rings; i++)
		drm_sched_wqueue_stop(&vinst->ring_enc[i].sched);

	/* Perform the VCN reset for the specified instance */
	r = vinst->reset(vinst);
	if (r)
		goto unlock;
	r = amdgpu_ring_test_ring(&vinst->ring_dec);
	if (r)
		goto unlock;
	for (i = 0; i < vinst->num_enc_rings; i++) {
		r = amdgpu_ring_test_ring(&vinst->ring_enc[i]);
		if (r)
			goto unlock;
	}
	amdgpu_fence_driver_force_completion(&vinst->ring_dec);
	for (i = 0; i < vinst->num_enc_rings; i++)
		amdgpu_fence_driver_force_completion(&vinst->ring_enc[i]);

	/* Restart the scheduler's work queue for the dec and enc rings
	 * if they were stopped by this function. This allows new tasks
	 * to be submitted to the queues after the reset is complete.
	 */
	drm_sched_wqueue_start(&vinst->ring_dec.sched);
	for (i = 0; i < vinst->num_enc_rings; i++)
		drm_sched_wqueue_start(&vinst->ring_enc[i].sched);

unlock:
	mutex_unlock(&vinst->engine_reset_mutex);

	return r;
}

/**
 * amdgpu_vcn_ring_reset - Reset a VCN ring
 * @ring: ring to reset
 * @vmid: vmid of guilty job
 * @timedout_fence: fence of timed out job
 *
 * This helper is for VCN blocks without unified queues because
 * resetting the engine resets all queues in that case.  With
 * unified queues we have one queue per engine.
 * Returns: 0 on success, or a negative error code on failure.
 */
int amdgpu_vcn_ring_reset(struct amdgpu_ring *ring,
			  unsigned int vmid,
			  struct amdgpu_fence *timedout_fence)
{
	struct amdgpu_device *adev = ring->adev;

	if (adev->vcn.inst[ring->me].using_unified_queue)
		return -EINVAL;

	return amdgpu_vcn_reset_engine(adev, ring->me);
}
