/*
 * Copyright (c) 2016 QLogic Corporation.
 * All rights reserved.
 * www.qlogic.com
 *
 * See LICENSE.qede_pmd for copyright and licensing details.
 */

#include "bcm_osal.h"
#include "ecore_hsi_common.h"
#include "ecore_status.h"
#include "ecore.h"
#include "ecore_hw.h"
#include "reg_addr.h"
#include "ecore_utils.h"
#include "ecore_iov_api.h"

#ifndef ASIC_ONLY
#define ECORE_EMUL_FACTOR 2000
#define ECORE_FPGA_FACTOR 200
#endif

#define ECORE_BAR_ACQUIRE_TIMEOUT 1000

/* Invalid values */
#define ECORE_BAR_INVALID_OFFSET	(OSAL_CPU_TO_LE32(-1))

struct ecore_ptt {
	osal_list_entry_t list_entry;
	unsigned int idx;
	struct pxp_ptt_entry pxp;
	u8 hwfn_id;
};

struct ecore_ptt_pool {
	osal_list_t free_list;
	osal_spinlock_t lock; /* ptt synchronized access */
	struct ecore_ptt ptts[PXP_EXTERNAL_BAR_PF_WINDOW_NUM];
};

enum _ecore_status_t ecore_ptt_pool_alloc(struct ecore_hwfn *p_hwfn)
{
	struct ecore_ptt_pool *p_pool = OSAL_ALLOC(p_hwfn->p_dev,
						   GFP_KERNEL,
						   sizeof(*p_pool));
	int i;

	if (!p_pool)
		return ECORE_NOMEM;

	OSAL_LIST_INIT(&p_pool->free_list);
	for (i = 0; i < PXP_EXTERNAL_BAR_PF_WINDOW_NUM; i++) {
		p_pool->ptts[i].idx = i;
		p_pool->ptts[i].pxp.offset = ECORE_BAR_INVALID_OFFSET;
		p_pool->ptts[i].pxp.pretend.control = 0;
		p_pool->ptts[i].hwfn_id = p_hwfn->my_id;

		/* There are special PTT entries that are taken only by design.
		 * The rest are added ot the list for general usage.
		 */
		if (i >= RESERVED_PTT_MAX)
			OSAL_LIST_PUSH_HEAD(&p_pool->ptts[i].list_entry,
					    &p_pool->free_list);
	}

	p_hwfn->p_ptt_pool = p_pool;
	OSAL_SPIN_LOCK_ALLOC(p_hwfn, &p_pool->lock);
	OSAL_SPIN_LOCK_INIT(&p_pool->lock);

	return ECORE_SUCCESS;
}

void ecore_ptt_invalidate(struct ecore_hwfn *p_hwfn)
{
	struct ecore_ptt *p_ptt;
	int i;

	for (i = 0; i < PXP_EXTERNAL_BAR_PF_WINDOW_NUM; i++) {
		p_ptt = &p_hwfn->p_ptt_pool->ptts[i];
		p_ptt->pxp.offset = ECORE_BAR_INVALID_OFFSET;
	}
}

void ecore_ptt_pool_free(struct ecore_hwfn *p_hwfn)
{
	if (p_hwfn->p_ptt_pool)
		OSAL_SPIN_LOCK_DEALLOC(&p_hwfn->p_ptt_pool->lock);
	OSAL_FREE(p_hwfn->p_dev, p_hwfn->p_ptt_pool);
	p_hwfn->p_ptt_pool = OSAL_NULL;
}

struct ecore_ptt *ecore_ptt_acquire(struct ecore_hwfn *p_hwfn)
{
	struct ecore_ptt *p_ptt;
	unsigned int i;

	/* Take the free PTT from the list */
	for (i = 0; i < ECORE_BAR_ACQUIRE_TIMEOUT; i++) {
		OSAL_SPIN_LOCK(&p_hwfn->p_ptt_pool->lock);
		if (!OSAL_LIST_IS_EMPTY(&p_hwfn->p_ptt_pool->free_list)) {
			p_ptt = OSAL_LIST_FIRST_ENTRY(
						&p_hwfn->p_ptt_pool->free_list,
						struct ecore_ptt, list_entry);
			OSAL_LIST_REMOVE_ENTRY(&p_ptt->list_entry,
					       &p_hwfn->p_ptt_pool->free_list);

			OSAL_SPIN_UNLOCK(&p_hwfn->p_ptt_pool->lock);

			DP_VERBOSE(p_hwfn, ECORE_MSG_HW,
				   "allocated ptt %d\n", p_ptt->idx);

			return p_ptt;
		}

		OSAL_SPIN_UNLOCK(&p_hwfn->p_ptt_pool->lock);
		OSAL_MSLEEP(1);
	}

	DP_NOTICE(p_hwfn, true,
		  "PTT acquire timeout - failed to allocate PTT\n");
	return OSAL_NULL;
}

void ecore_ptt_release(struct ecore_hwfn *p_hwfn, struct ecore_ptt *p_ptt)
{
	/* This PTT should not be set to pretend if it is being released */
	/* TODO - add some pretend sanity checks, to make sure pretend
	 * isn't set on this ptt
	 */

	OSAL_SPIN_LOCK(&p_hwfn->p_ptt_pool->lock);
	OSAL_LIST_PUSH_HEAD(&p_ptt->list_entry, &p_hwfn->p_ptt_pool->free_list);
	OSAL_SPIN_UNLOCK(&p_hwfn->p_ptt_pool->lock);
}

static u32 ecore_ptt_get_hw_addr(struct ecore_hwfn *p_hwfn, struct ecore_ptt *p_ptt)
{
	/* The HW is using DWORDS and we need to translate it to Bytes */
	return OSAL_LE32_TO_CPU(p_ptt->pxp.offset) << 2;
}

static u32 ecore_ptt_config_addr(struct ecore_ptt *p_ptt)
{
	return PXP_PF_WINDOW_ADMIN_PER_PF_START +
	    p_ptt->idx * sizeof(struct pxp_ptt_entry);
}

u32 ecore_ptt_get_bar_addr(struct ecore_ptt *p_ptt)
{
	return PXP_EXTERNAL_BAR_PF_WINDOW_START +
	    p_ptt->idx * PXP_EXTERNAL_BAR_PF_WINDOW_SINGLE_SIZE;
}

void ecore_ptt_set_win(struct ecore_hwfn *p_hwfn,
		       struct ecore_ptt *p_ptt, u32 new_hw_addr)
{
	u32 prev_hw_addr;

	prev_hw_addr = ecore_ptt_get_hw_addr(p_hwfn, p_ptt);

	if (new_hw_addr == prev_hw_addr)
		return;

	/* Update PTT entery in admin window */
	DP_VERBOSE(p_hwfn, ECORE_MSG_HW,
		   "Updating PTT entry %d to offset 0x%x\n",
		   p_ptt->idx, new_hw_addr);

	/* The HW is using DWORDS and the address is in Bytes */
	p_ptt->pxp.offset = OSAL_CPU_TO_LE32(new_hw_addr >> 2);

	REG_WR(p_hwfn,
	       ecore_ptt_config_addr(p_ptt) +
	       OFFSETOF(struct pxp_ptt_entry, offset),
	       OSAL_LE32_TO_CPU(p_ptt->pxp.offset));
}

static u32 ecore_set_ptt(struct ecore_hwfn *p_hwfn,
			 struct ecore_ptt *p_ptt, u32 hw_addr)
{
	u32 win_hw_addr = ecore_ptt_get_hw_addr(p_hwfn, p_ptt);
	u32 offset;

	offset = hw_addr - win_hw_addr;

	if (p_ptt->hwfn_id != p_hwfn->my_id)
		DP_NOTICE(p_hwfn, true,
			  "ptt[%d] of hwfn[%02x] is used by hwfn[%02x]!\n",
			  p_ptt->idx, p_ptt->hwfn_id, p_hwfn->my_id);

	/* Verify the address is within the window */
	if (hw_addr < win_hw_addr ||
	    offset >= PXP_EXTERNAL_BAR_PF_WINDOW_SINGLE_SIZE) {
		ecore_ptt_set_win(p_hwfn, p_ptt, hw_addr);
		offset = 0;
	}

	return ecore_ptt_get_bar_addr(p_ptt) + offset;
}

struct ecore_ptt *ecore_get_reserved_ptt(struct ecore_hwfn *p_hwfn,
					 enum reserved_ptts ptt_idx)
{
	if (ptt_idx >= RESERVED_PTT_MAX) {
		DP_NOTICE(p_hwfn, true,
			  "Requested PTT %d is out of range\n", ptt_idx);
		return OSAL_NULL;
	}

	return &p_hwfn->p_ptt_pool->ptts[ptt_idx];
}

static bool ecore_is_reg_fifo_empty(struct ecore_hwfn *p_hwfn,
				    struct ecore_ptt *p_ptt)
{
	bool is_empty = true;
	u32 bar_addr;

	if (!p_hwfn->p_dev->chk_reg_fifo)
		goto out;

	/* ecore_rd() cannot be used here since it calls this function */
	bar_addr = ecore_set_ptt(p_hwfn, p_ptt, GRC_REG_TRACE_FIFO_VALID_DATA);
	is_empty = REG_RD(p_hwfn, bar_addr) == 0;

#ifndef ASIC_ONLY
	if (CHIP_REV_IS_SLOW(p_hwfn->p_dev))
		OSAL_UDELAY(100);
#endif

out:
	return is_empty;
}

void ecore_wr(struct ecore_hwfn *p_hwfn,
	      struct ecore_ptt *p_ptt, u32 hw_addr, u32 val)
{
	bool prev_fifo_err;
	u32 bar_addr;

	prev_fifo_err = !ecore_is_reg_fifo_empty(p_hwfn, p_ptt);

	bar_addr = ecore_set_ptt(p_hwfn, p_ptt, hw_addr);
	REG_WR(p_hwfn, bar_addr, val);
	DP_VERBOSE(p_hwfn, ECORE_MSG_HW,
		   "bar_addr 0x%x, hw_addr 0x%x, val 0x%x\n",
		   bar_addr, hw_addr, val);

#ifndef ASIC_ONLY
	if (CHIP_REV_IS_SLOW(p_hwfn->p_dev))
		OSAL_UDELAY(100);
#endif

	OSAL_WARN(!prev_fifo_err && !ecore_is_reg_fifo_empty(p_hwfn, p_ptt),
		  "reg_fifo err was caused by a call to ecore_wr(0x%x, 0x%x)\n",
		  hw_addr, val);
}

u32 ecore_rd(struct ecore_hwfn *p_hwfn, struct ecore_ptt *p_ptt, u32 hw_addr)
{
	bool prev_fifo_err;
	u32 bar_addr, val;

	prev_fifo_err = !ecore_is_reg_fifo_empty(p_hwfn, p_ptt);

	bar_addr = ecore_set_ptt(p_hwfn, p_ptt, hw_addr);
	val = REG_RD(p_hwfn, bar_addr);

	DP_VERBOSE(p_hwfn, ECORE_MSG_HW,
		   "bar_addr 0x%x, hw_addr 0x%x, val 0x%x\n",
		   bar_addr, hw_addr, val);

#ifndef ASIC_ONLY
	if (CHIP_REV_IS_SLOW(p_hwfn->p_dev))
		OSAL_UDELAY(100);
#endif

	OSAL_WARN(!prev_fifo_err && !ecore_is_reg_fifo_empty(p_hwfn, p_ptt),
		  "reg_fifo error was caused by a call to ecore_rd(0x%x)\n",
		  hw_addr);

	return val;
}

static void ecore_memcpy_hw(struct ecore_hwfn *p_hwfn,
			    struct ecore_ptt *p_ptt,
			    void *addr,
			    u32 hw_addr, osal_size_t n, bool to_device)
{
	u32 dw_count, *host_addr, hw_offset;
	osal_size_t quota, done = 0;
	u32 OSAL_IOMEM *reg_addr;

	while (done < n) {
		quota = OSAL_MIN_T(osal_size_t, n - done,
				   PXP_EXTERNAL_BAR_PF_WINDOW_SINGLE_SIZE);

		if (IS_PF(p_hwfn->p_dev)) {
			ecore_ptt_set_win(p_hwfn, p_ptt, hw_addr + done);
			hw_offset = ecore_ptt_get_bar_addr(p_ptt);
		} else {
			hw_offset = hw_addr + done;
		}

		dw_count = quota / 4;
		host_addr = (u32 *)((u8 *)addr + done);
		reg_addr = (u32 OSAL_IOMEM *)OSAL_REG_ADDR(p_hwfn, hw_offset);

		if (to_device)
			while (dw_count--)
				DIRECT_REG_WR(p_hwfn, reg_addr++, *host_addr++);
		else
			while (dw_count--)
				*host_addr++ = DIRECT_REG_RD(p_hwfn,
							     reg_addr++);

		done += quota;
	}
}

void ecore_memcpy_from(struct ecore_hwfn *p_hwfn,
		       struct ecore_ptt *p_ptt,
		       void *dest, u32 hw_addr, osal_size_t n)
{
	DP_VERBOSE(p_hwfn, ECORE_MSG_HW,
		   "hw_addr 0x%x, dest %p hw_addr 0x%x, size %lu\n",
		   hw_addr, dest, hw_addr, (unsigned long)n);

	ecore_memcpy_hw(p_hwfn, p_ptt, dest, hw_addr, n, false);
}

void ecore_memcpy_to(struct ecore_hwfn *p_hwfn,
		     struct ecore_ptt *p_ptt,
		     u32 hw_addr, void *src, osal_size_t n)
{
	DP_VERBOSE(p_hwfn, ECORE_MSG_HW,
		   "hw_addr 0x%x, hw_addr 0x%x, src %p size %lu\n",
		   hw_addr, hw_addr, src, (unsigned long)n);

	ecore_memcpy_hw(p_hwfn, p_ptt, src, hw_addr, n, true);
}

void ecore_fid_pretend(struct ecore_hwfn *p_hwfn,
		       struct ecore_ptt *p_ptt, u16 fid)
{
	u16 control = 0;

	SET_FIELD(control, PXP_PRETEND_CMD_IS_CONCRETE, 1);
	SET_FIELD(control, PXP_PRETEND_CMD_PRETEND_FUNCTION, 1);

/* Every pretend undos prev pretends, including previous port pretend */

	SET_FIELD(control, PXP_PRETEND_CMD_PORT, 0);
	SET_FIELD(control, PXP_PRETEND_CMD_USE_PORT, 0);
	SET_FIELD(control, PXP_PRETEND_CMD_PRETEND_PORT, 1);

	if (!GET_FIELD(fid, PXP_CONCRETE_FID_VFVALID))
		fid = GET_FIELD(fid, PXP_CONCRETE_FID_PFID);

	p_ptt->pxp.pretend.control = OSAL_CPU_TO_LE16(control);
	p_ptt->pxp.pretend.fid.concrete_fid.fid = OSAL_CPU_TO_LE16(fid);

	REG_WR(p_hwfn,
	       ecore_ptt_config_addr(p_ptt) +
	       OFFSETOF(struct pxp_ptt_entry, pretend),
			*(u32 *)&p_ptt->pxp.pretend);
}

void ecore_port_pretend(struct ecore_hwfn *p_hwfn,
			struct ecore_ptt *p_ptt, u8 port_id)
{
	u16 control = 0;

	SET_FIELD(control, PXP_PRETEND_CMD_PORT, port_id);
	SET_FIELD(control, PXP_PRETEND_CMD_USE_PORT, 1);
	SET_FIELD(control, PXP_PRETEND_CMD_PRETEND_PORT, 1);
	p_ptt->pxp.pretend.control = OSAL_CPU_TO_LE16(control);

	REG_WR(p_hwfn,
	       ecore_ptt_config_addr(p_ptt) +
	       OFFSETOF(struct pxp_ptt_entry, pretend),
			*(u32 *)&p_ptt->pxp.pretend);
}

void ecore_port_unpretend(struct ecore_hwfn *p_hwfn, struct ecore_ptt *p_ptt)
{
	u16 control = 0;

	SET_FIELD(control, PXP_PRETEND_CMD_PORT, 0);
	SET_FIELD(control, PXP_PRETEND_CMD_USE_PORT, 0);
	SET_FIELD(control, PXP_PRETEND_CMD_PRETEND_PORT, 1);

	p_ptt->pxp.pretend.control = OSAL_CPU_TO_LE16(control);

	REG_WR(p_hwfn,
	       ecore_ptt_config_addr(p_ptt) +
	       OFFSETOF(struct pxp_ptt_entry, pretend),
			*(u32 *)&p_ptt->pxp.pretend);
}

u32 ecore_vfid_to_concrete(struct ecore_hwfn *p_hwfn, u8 vfid)
{
	u32 concrete_fid = 0;

	SET_FIELD(concrete_fid, PXP_CONCRETE_FID_PFID, p_hwfn->rel_pf_id);
	SET_FIELD(concrete_fid, PXP_CONCRETE_FID_VFID, vfid);
	SET_FIELD(concrete_fid, PXP_CONCRETE_FID_VFVALID, 1);

	return concrete_fid;
}

/* Not in use @DPDK
 * Ecore HW lock
 * =============
 * Although the implementation is ready, today we don't have any flow that
 * utliizes said locks - and we want to keep it this way.
 * If this changes, this needs to be revisted.
 */

/* Ecore DMAE
 * =============
 */
static void ecore_dmae_opcode(struct ecore_hwfn *p_hwfn,
			      const u8 is_src_type_grc,
			      const u8 is_dst_type_grc,
			      struct ecore_dmae_params *p_params)
{
	u16 opcode_b = 0;
	u32 opcode = 0;

	/* Whether the source is the PCIe or the GRC.
	 * 0- The source is the PCIe
	 * 1- The source is the GRC.
	 */
	opcode |= (is_src_type_grc ? DMAE_CMD_SRC_MASK_GRC
		   : DMAE_CMD_SRC_MASK_PCIE) << DMAE_CMD_SRC_SHIFT;
	opcode |= (p_hwfn->rel_pf_id & DMAE_CMD_SRC_PF_ID_MASK) <<
	    DMAE_CMD_SRC_PF_ID_SHIFT;

	/* The destination of the DMA can be: 0-None 1-PCIe 2-GRC 3-None */
	opcode |= (is_dst_type_grc ? DMAE_CMD_DST_MASK_GRC
		   : DMAE_CMD_DST_MASK_PCIE) << DMAE_CMD_DST_SHIFT;
	opcode |= (p_hwfn->rel_pf_id & DMAE_CMD_DST_PF_ID_MASK) <<
	    DMAE_CMD_DST_PF_ID_SHIFT;

	/* DMAE_E4_TODO need to check which value to specifiy here. */
	/* opcode |= (!b_complete_to_host)<< DMAE_CMD_C_DST_SHIFT; */

	/* Whether to write a completion word to the completion destination:
	 * 0-Do not write a completion word
	 * 1-Write the completion word
	 */
	opcode |= DMAE_CMD_COMP_WORD_EN_MASK << DMAE_CMD_COMP_WORD_EN_SHIFT;
	opcode |= DMAE_CMD_SRC_ADDR_RESET_MASK << DMAE_CMD_SRC_ADDR_RESET_SHIFT;

	if (p_params->flags & ECORE_DMAE_FLAG_COMPLETION_DST)
		opcode |= 1 << DMAE_CMD_COMP_FUNC_SHIFT;

	/* swapping mode 3 - big endian there should be a define ifdefed in
	 * the HSI somewhere. Since it is currently
	 */
	opcode |= DMAE_CMD_ENDIANITY << DMAE_CMD_ENDIANITY_MODE_SHIFT;

	opcode |= p_hwfn->port_id << DMAE_CMD_PORT_ID_SHIFT;

	/* reset source address in next go */
	opcode |= DMAE_CMD_SRC_ADDR_RESET_MASK << DMAE_CMD_SRC_ADDR_RESET_SHIFT;

	/* reset dest address in next go */
	opcode |= DMAE_CMD_DST_ADDR_RESET_MASK << DMAE_CMD_DST_ADDR_RESET_SHIFT;

	/* SRC/DST VFID: all 1's - pf, otherwise VF id */
	if (p_params->flags & ECORE_DMAE_FLAG_VF_SRC) {
		opcode |= (1 << DMAE_CMD_SRC_VF_ID_VALID_SHIFT);
		opcode_b |= (p_params->src_vfid << DMAE_CMD_SRC_VF_ID_SHIFT);
	} else {
		opcode_b |= (DMAE_CMD_SRC_VF_ID_MASK <<
			     DMAE_CMD_SRC_VF_ID_SHIFT);
	}
	if (p_params->flags & ECORE_DMAE_FLAG_VF_DST) {
		opcode |= 1 << DMAE_CMD_DST_VF_ID_VALID_SHIFT;
		opcode_b |= p_params->dst_vfid << DMAE_CMD_DST_VF_ID_SHIFT;
	} else {
		opcode_b |= DMAE_CMD_DST_VF_ID_MASK << DMAE_CMD_DST_VF_ID_SHIFT;
	}

	p_hwfn->dmae_info.p_dmae_cmd->opcode = OSAL_CPU_TO_LE32(opcode);
	p_hwfn->dmae_info.p_dmae_cmd->opcode_b = OSAL_CPU_TO_LE16(opcode_b);
}

static u32 ecore_dmae_idx_to_go_cmd(u8 idx)
{
	OSAL_BUILD_BUG_ON((DMAE_REG_GO_C31 - DMAE_REG_GO_C0) != 31 * 4);

	/* All the DMAE 'go' registers form an array in internal memory */
	return DMAE_REG_GO_C0 + (idx << 2);
}

static enum _ecore_status_t
ecore_dmae_post_command(struct ecore_hwfn *p_hwfn, struct ecore_ptt *p_ptt)
{
	struct dmae_cmd *p_command = p_hwfn->dmae_info.p_dmae_cmd;
	u8 idx_cmd = p_hwfn->dmae_info.channel, i;
	enum _ecore_status_t ecore_status = ECORE_SUCCESS;

	/* verify address is not OSAL_NULL */
	if ((((!p_command->dst_addr_lo) && (!p_command->dst_addr_hi)) ||
	     ((!p_command->src_addr_lo) && (!p_command->src_addr_hi)))) {
		DP_NOTICE(p_hwfn, true,
			  "source or destination address 0 idx_cmd=%d\n"
			  "opcode = [0x%08x,0x%04x] len=0x%x"
			  " src=0x%x:%x dst=0x%x:%x\n",
			  idx_cmd,
			  OSAL_LE32_TO_CPU(p_command->opcode),
			  OSAL_LE16_TO_CPU(p_command->opcode_b),
			  OSAL_LE16_TO_CPU(p_command->length_dw),
			  OSAL_LE32_TO_CPU(p_command->src_addr_hi),
			  OSAL_LE32_TO_CPU(p_command->src_addr_lo),
			  OSAL_LE32_TO_CPU(p_command->dst_addr_hi),
			  OSAL_LE32_TO_CPU(p_command->dst_addr_lo));

		return ECORE_INVAL;
	}

	DP_VERBOSE(p_hwfn, ECORE_MSG_HW,
		   "Posting DMAE command [idx %d]: opcode = [0x%08x,0x%04x]"
		   "len=0x%x src=0x%x:%x dst=0x%x:%x\n",
		   idx_cmd,
		   OSAL_LE32_TO_CPU(p_command->opcode),
		   OSAL_LE16_TO_CPU(p_command->opcode_b),
		   OSAL_LE16_TO_CPU(p_command->length_dw),
		   OSAL_LE32_TO_CPU(p_command->src_addr_hi),
		   OSAL_LE32_TO_CPU(p_command->src_addr_lo),
		   OSAL_LE32_TO_CPU(p_command->dst_addr_hi),
		   OSAL_LE32_TO_CPU(p_command->dst_addr_lo));

	/* Copy the command to DMAE - need to do it before every call
	 * for source/dest address no reset.
	 * The number of commands have been increased to 16 (previous was 14)
	 * The first 9 DWs are the command registers, the 10 DW is the
	 * GO register, and
	 * the rest are result registers (which are read only by the client).
	 */
	for (i = 0; i < DMAE_CMD_SIZE; i++) {
		u32 data = (i < DMAE_CMD_SIZE_TO_FILL) ?
		    *(((u32 *)p_command) + i) : 0;

		ecore_wr(p_hwfn, p_ptt,
			 DMAE_REG_CMD_MEM +
			 (idx_cmd * DMAE_CMD_SIZE * sizeof(u32)) +
			 (i * sizeof(u32)), data);
	}

	ecore_wr(p_hwfn, p_ptt,
		 ecore_dmae_idx_to_go_cmd(idx_cmd), DMAE_GO_VALUE);

	return ecore_status;
}

enum _ecore_status_t ecore_dmae_info_alloc(struct ecore_hwfn *p_hwfn)
{
	dma_addr_t *p_addr = &p_hwfn->dmae_info.completion_word_phys_addr;
	struct dmae_cmd **p_cmd = &p_hwfn->dmae_info.p_dmae_cmd;
	u32 **p_buff = &p_hwfn->dmae_info.p_intermediate_buffer;
	u32 **p_comp = &p_hwfn->dmae_info.p_completion_word;

	*p_comp = OSAL_DMA_ALLOC_COHERENT(p_hwfn->p_dev, p_addr, sizeof(u32));
	if (*p_comp == OSAL_NULL) {
		DP_NOTICE(p_hwfn, true,
			  "Failed to allocate `p_completion_word'\n");
		goto err;
	}

	p_addr = &p_hwfn->dmae_info.dmae_cmd_phys_addr;
	*p_cmd = OSAL_DMA_ALLOC_COHERENT(p_hwfn->p_dev, p_addr,
					 sizeof(struct dmae_cmd));
	if (*p_cmd == OSAL_NULL) {
		DP_NOTICE(p_hwfn, true,
			  "Failed to allocate `struct dmae_cmd'\n");
		goto err;
	}

	p_addr = &p_hwfn->dmae_info.intermediate_buffer_phys_addr;
	*p_buff = OSAL_DMA_ALLOC_COHERENT(p_hwfn->p_dev, p_addr,
					  sizeof(u32) * DMAE_MAX_RW_SIZE);
	if (*p_buff == OSAL_NULL) {
		DP_NOTICE(p_hwfn, true,
			  "Failed to allocate `intermediate_buffer'\n");
		goto err;
	}

	p_hwfn->dmae_info.channel = p_hwfn->rel_pf_id;

	return ECORE_SUCCESS;
err:
	ecore_dmae_info_free(p_hwfn);
	return ECORE_NOMEM;
}

void ecore_dmae_info_free(struct ecore_hwfn *p_hwfn)
{
	dma_addr_t p_phys;

	/* Just make sure no one is in the middle */
	OSAL_MUTEX_ACQUIRE(&p_hwfn->dmae_info.mutex);

	if (p_hwfn->dmae_info.p_completion_word != OSAL_NULL) {
		p_phys = p_hwfn->dmae_info.completion_word_phys_addr;
		OSAL_DMA_FREE_COHERENT(p_hwfn->p_dev,
				       p_hwfn->dmae_info.p_completion_word,
				       p_phys, sizeof(u32));
		p_hwfn->dmae_info.p_completion_word = OSAL_NULL;
	}

	if (p_hwfn->dmae_info.p_dmae_cmd != OSAL_NULL) {
		p_phys = p_hwfn->dmae_info.dmae_cmd_phys_addr;
		OSAL_DMA_FREE_COHERENT(p_hwfn->p_dev,
				       p_hwfn->dmae_info.p_dmae_cmd,
				       p_phys, sizeof(struct dmae_cmd));
		p_hwfn->dmae_info.p_dmae_cmd = OSAL_NULL;
	}

	if (p_hwfn->dmae_info.p_intermediate_buffer != OSAL_NULL) {
		p_phys = p_hwfn->dmae_info.intermediate_buffer_phys_addr;
		OSAL_DMA_FREE_COHERENT(p_hwfn->p_dev,
				       p_hwfn->dmae_info.p_intermediate_buffer,
				       p_phys, sizeof(u32) * DMAE_MAX_RW_SIZE);
		p_hwfn->dmae_info.p_intermediate_buffer = OSAL_NULL;
	}

	OSAL_MUTEX_RELEASE(&p_hwfn->dmae_info.mutex);
}

static enum _ecore_status_t ecore_dmae_operation_wait(struct ecore_hwfn *p_hwfn)
{
	u32 wait_cnt_limit = 10000, wait_cnt = 0;
	enum _ecore_status_t ecore_status = ECORE_SUCCESS;

#ifndef ASIC_ONLY
	u32 factor = (CHIP_REV_IS_EMUL(p_hwfn->p_dev) ?
		      ECORE_EMUL_FACTOR :
		      (CHIP_REV_IS_FPGA(p_hwfn->p_dev) ?
		       ECORE_FPGA_FACTOR : 1));

	wait_cnt_limit *= factor;
#endif

	/* DMAE_E4_TODO : TODO check if we have to call any other function
	 * other than BARRIER to sync the completion_word since we are not
	 * using the volatile keyword for this
	 */
	OSAL_BARRIER(p_hwfn->p_dev);
	while (*p_hwfn->dmae_info.p_completion_word != DMAE_COMPLETION_VAL) {
		OSAL_UDELAY(DMAE_MIN_WAIT_TIME);
		if (++wait_cnt > wait_cnt_limit) {
			DP_NOTICE(p_hwfn->p_dev, ECORE_MSG_HW,
				  "Timed-out waiting for operation to"
				  " complete. Completion word is 0x%08x"
				  " expected 0x%08x.\n",
				  *p_hwfn->dmae_info.p_completion_word,
				  DMAE_COMPLETION_VAL);
			ecore_status = ECORE_TIMEOUT;
			break;
		}
		/* to sync the completion_word since we are not
		 * using the volatile keyword for p_completion_word
		 */
		OSAL_BARRIER(p_hwfn->p_dev);
	}

	if (ecore_status == ECORE_SUCCESS)
		*p_hwfn->dmae_info.p_completion_word = 0;

	return ecore_status;
}

static enum _ecore_status_t
ecore_dmae_execute_sub_operation(struct ecore_hwfn *p_hwfn,
				 struct ecore_ptt *p_ptt,
				 u64 src_addr,
				 u64 dst_addr,
				 u8 src_type, u8 dst_type, u32 length_dw)
{
	dma_addr_t phys = p_hwfn->dmae_info.intermediate_buffer_phys_addr;
	struct dmae_cmd *cmd = p_hwfn->dmae_info.p_dmae_cmd;
	enum _ecore_status_t ecore_status = ECORE_SUCCESS;

	switch (src_type) {
	case ECORE_DMAE_ADDRESS_GRC:
	case ECORE_DMAE_ADDRESS_HOST_PHYS:
		cmd->src_addr_hi = OSAL_CPU_TO_LE32(DMA_HI(src_addr));
		cmd->src_addr_lo = OSAL_CPU_TO_LE32(DMA_LO(src_addr));
		break;
		/* for virt source addresses we use the intermediate buffer. */
	case ECORE_DMAE_ADDRESS_HOST_VIRT:
		cmd->src_addr_hi = OSAL_CPU_TO_LE32(DMA_HI(phys));
		cmd->src_addr_lo = OSAL_CPU_TO_LE32(DMA_LO(phys));
		OSAL_MEMCPY(&p_hwfn->dmae_info.p_intermediate_buffer[0],
			    (void *)(osal_uintptr_t)src_addr,
			    length_dw * sizeof(u32));
		break;
	default:
		return ECORE_INVAL;
	}

	switch (dst_type) {
	case ECORE_DMAE_ADDRESS_GRC:
	case ECORE_DMAE_ADDRESS_HOST_PHYS:
		cmd->dst_addr_hi = OSAL_CPU_TO_LE32(DMA_HI(dst_addr));
		cmd->dst_addr_lo = OSAL_CPU_TO_LE32(DMA_LO(dst_addr));
		break;
		/* for virt destination address we use the intermediate buff. */
	case ECORE_DMAE_ADDRESS_HOST_VIRT:
		cmd->dst_addr_hi = OSAL_CPU_TO_LE32(DMA_HI(phys));
		cmd->dst_addr_lo = OSAL_CPU_TO_LE32(DMA_LO(phys));
		break;
	default:
		return ECORE_INVAL;
	}

	cmd->length_dw = OSAL_CPU_TO_LE16((u16)length_dw);

	if (src_type == ECORE_DMAE_ADDRESS_HOST_VIRT ||
	    src_type == ECORE_DMAE_ADDRESS_HOST_PHYS)
		OSAL_DMA_SYNC(p_hwfn->p_dev,
			      (void *)HILO_U64(cmd->src_addr_hi,
					       cmd->src_addr_lo),
			      length_dw * sizeof(u32), false);

	ecore_dmae_post_command(p_hwfn, p_ptt);

	ecore_status = ecore_dmae_operation_wait(p_hwfn);

	/* TODO - is it true ? */
	if (src_type == ECORE_DMAE_ADDRESS_HOST_VIRT ||
	    src_type == ECORE_DMAE_ADDRESS_HOST_PHYS)
		OSAL_DMA_SYNC(p_hwfn->p_dev,
			      (void *)HILO_U64(cmd->src_addr_hi,
					       cmd->src_addr_lo),
			      length_dw * sizeof(u32), true);

	if (ecore_status != ECORE_SUCCESS) {
		DP_NOTICE(p_hwfn, ECORE_MSG_HW,
			  "ecore_dmae_host2grc: Wait Failed. source_addr"
			  " 0x%lx, grc_addr 0x%lx, size_in_dwords 0x%x\n",
			  (unsigned long)src_addr, (unsigned long)dst_addr,
			  length_dw);
		return ecore_status;
	}

	if (dst_type == ECORE_DMAE_ADDRESS_HOST_VIRT)
		OSAL_MEMCPY((void *)(osal_uintptr_t)(dst_addr),
			    &p_hwfn->dmae_info.p_intermediate_buffer[0],
			    length_dw * sizeof(u32));

	return ECORE_SUCCESS;
}

static enum _ecore_status_t
ecore_dmae_execute_command(struct ecore_hwfn *p_hwfn,
			   struct ecore_ptt *p_ptt,
			   u64 src_addr,
			   u64 dst_addr,
			   u8 src_type,
			   u8 dst_type,
			   u32 size_in_dwords,
			   struct ecore_dmae_params *p_params)
{
	dma_addr_t phys = p_hwfn->dmae_info.completion_word_phys_addr;
	u16 length_cur = 0, i = 0, cnt_split = 0, length_mod = 0;
	struct dmae_cmd *cmd = p_hwfn->dmae_info.p_dmae_cmd;
	u64 src_addr_split = 0, dst_addr_split = 0;
	u16 length_limit = DMAE_MAX_RW_SIZE;
	enum _ecore_status_t ecore_status = ECORE_SUCCESS;
	u32 offset = 0;

	ecore_dmae_opcode(p_hwfn,
			  (src_type == ECORE_DMAE_ADDRESS_GRC),
			  (dst_type == ECORE_DMAE_ADDRESS_GRC), p_params);

	cmd->comp_addr_lo = OSAL_CPU_TO_LE32(DMA_LO(phys));
	cmd->comp_addr_hi = OSAL_CPU_TO_LE32(DMA_HI(phys));
	cmd->comp_val = OSAL_CPU_TO_LE32(DMAE_COMPLETION_VAL);

	/* Check if the grc_addr is valid like < MAX_GRC_OFFSET */
	cnt_split = size_in_dwords / length_limit;
	length_mod = size_in_dwords % length_limit;

	src_addr_split = src_addr;
	dst_addr_split = dst_addr;

	for (i = 0; i <= cnt_split; i++) {
		offset = length_limit * i;

		if (!(p_params->flags & ECORE_DMAE_FLAG_RW_REPL_SRC)) {
			if (src_type == ECORE_DMAE_ADDRESS_GRC)
				src_addr_split = src_addr + offset;
			else
				src_addr_split = src_addr + (offset * 4);
		}

		if (dst_type == ECORE_DMAE_ADDRESS_GRC)
			dst_addr_split = dst_addr + offset;
		else
			dst_addr_split = dst_addr + (offset * 4);

		length_cur = (cnt_split == i) ? length_mod : length_limit;

		/* might be zero on last iteration */
		if (!length_cur)
			continue;

		ecore_status = ecore_dmae_execute_sub_operation(p_hwfn,
								p_ptt,
								src_addr_split,
								dst_addr_split,
								src_type,
								dst_type,
								length_cur);
		if (ecore_status != ECORE_SUCCESS) {
			DP_NOTICE(p_hwfn, false,
				  "ecore_dmae_execute_sub_operation Failed"
				  " with error 0x%x. source_addr 0x%lx,"
				  " dest addr 0x%lx, size_in_dwords 0x%x\n",
				  ecore_status, (unsigned long)src_addr,
				  (unsigned long)dst_addr, length_cur);

			ecore_hw_err_notify(p_hwfn, ECORE_HW_ERR_DMAE_FAIL);
			break;
		}
	}

	return ecore_status;
}

enum _ecore_status_t
ecore_dmae_host2grc(struct ecore_hwfn *p_hwfn,
		    struct ecore_ptt *p_ptt,
		    u64 source_addr,
		    u32 grc_addr, u32 size_in_dwords, u32 flags)
{
	u32 grc_addr_in_dw = grc_addr / sizeof(u32);
	struct ecore_dmae_params params;
	enum _ecore_status_t rc;

	OSAL_MEMSET(&params, 0, sizeof(struct ecore_dmae_params));
	params.flags = flags;

	OSAL_MUTEX_ACQUIRE(&p_hwfn->dmae_info.mutex);

	rc = ecore_dmae_execute_command(p_hwfn, p_ptt, source_addr,
					grc_addr_in_dw,
					ECORE_DMAE_ADDRESS_HOST_VIRT,
					ECORE_DMAE_ADDRESS_GRC,
					size_in_dwords, &params);

	OSAL_MUTEX_RELEASE(&p_hwfn->dmae_info.mutex);

	return rc;
}

enum _ecore_status_t
ecore_dmae_grc2host(struct ecore_hwfn *p_hwfn,
		    struct ecore_ptt *p_ptt,
		    u32 grc_addr,
		    dma_addr_t dest_addr, u32 size_in_dwords, u32 flags)
{
	u32 grc_addr_in_dw = grc_addr / sizeof(u32);
	struct ecore_dmae_params params;
	enum _ecore_status_t rc;

	OSAL_MEMSET(&params, 0, sizeof(struct ecore_dmae_params));
	params.flags = flags;

	OSAL_MUTEX_ACQUIRE(&p_hwfn->dmae_info.mutex);

	rc = ecore_dmae_execute_command(p_hwfn, p_ptt, grc_addr_in_dw,
					dest_addr, ECORE_DMAE_ADDRESS_GRC,
					ECORE_DMAE_ADDRESS_HOST_VIRT,
					size_in_dwords, &params);

	OSAL_MUTEX_RELEASE(&p_hwfn->dmae_info.mutex);

	return rc;
}

enum _ecore_status_t
ecore_dmae_host2host(struct ecore_hwfn *p_hwfn,
		     struct ecore_ptt *p_ptt,
		     dma_addr_t source_addr,
		     dma_addr_t dest_addr,
		     u32 size_in_dwords, struct ecore_dmae_params *p_params)
{
	enum _ecore_status_t rc;

	OSAL_MUTEX_ACQUIRE(&p_hwfn->dmae_info.mutex);

	rc = ecore_dmae_execute_command(p_hwfn, p_ptt, source_addr,
					dest_addr,
					ECORE_DMAE_ADDRESS_HOST_PHYS,
					ECORE_DMAE_ADDRESS_HOST_PHYS,
					size_in_dwords, p_params);

	OSAL_MUTEX_RELEASE(&p_hwfn->dmae_info.mutex);

	return rc;
}

u16 ecore_get_qm_pq(struct ecore_hwfn *p_hwfn,
		    enum protocol_type proto,
		    union ecore_qm_pq_params *p_params)
{
	u16 pq_id = 0;

	if ((proto == PROTOCOLID_CORE ||
	     proto == PROTOCOLID_ETH) && !p_params) {
		DP_NOTICE(p_hwfn, true,
			  "Protocol %d received NULL PQ params\n", proto);
		return 0;
	}

	switch (proto) {
	case PROTOCOLID_CORE:
		if (p_params->core.tc == LB_TC)
			pq_id = p_hwfn->qm_info.pure_lb_pq;
		else if (p_params->core.tc == OOO_LB_TC)
			pq_id = p_hwfn->qm_info.ooo_pq;
		else
			pq_id = p_hwfn->qm_info.offload_pq;
		break;
	case PROTOCOLID_ETH:
		pq_id = p_params->eth.tc;
		/* TODO - multi-CoS for VFs? */
		if (p_params->eth.is_vf)
			pq_id += p_hwfn->qm_info.vf_queues_offset +
			    p_params->eth.vf_id;
		break;
	default:
		pq_id = 0;
	}

	pq_id = CM_TX_PQ_BASE + pq_id + RESC_START(p_hwfn, ECORE_PQ);

	return pq_id;
}

void ecore_hw_err_notify(struct ecore_hwfn *p_hwfn,
			 enum ecore_hw_err_type err_type)
{
	/* Fan failure cannot be masked by handling of another HW error */
	if (p_hwfn->p_dev->recov_in_prog && err_type != ECORE_HW_ERR_FAN_FAIL) {
		DP_VERBOSE(p_hwfn, ECORE_MSG_DRV,
			   "Recovery is in progress."
			   "Avoid notifying about HW error %d.\n",
			   err_type);
		return;
	}

	OSAL_HW_ERROR_OCCURRED(p_hwfn, err_type);
}
