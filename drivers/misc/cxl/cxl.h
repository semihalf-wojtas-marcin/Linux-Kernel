/*
 * Copyright 2014 IBM Corp.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#ifndef _CXL_H_
#define _CXL_H_

#include <linux/interrupt.h>
#include <linux/semaphore.h>
#include <linux/device.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/pid.h>
#include <linux/io.h>
#include <linux/pci.h>
#include <linux/fs.h>
#include <asm/cputable.h>
#include <asm/mmu.h>
#include <asm/reg.h>
#include <misc/cxl-base.h>

#include <uapi/misc/cxl.h>

extern uint cxl_verbose;

#define CXL_TIMEOUT 5

/*
 * Bump version each time a user API change is made, whether it is
 * backwards compatible ot not.
 */
#define CXL_API_VERSION 2
#define CXL_API_VERSION_COMPATIBLE 1

/*
 * Opaque types to avoid accidentally passing registers for the wrong MMIO
 *
 * At the end of the day, I'm not married to using typedef here, but it might
 * (and has!) help avoid bugs like mixing up CXL_PSL_CtxTime and
 * CXL_PSL_CtxTime_An, or calling cxl_p1n_write instead of cxl_p1_write.
 *
 * I'm quite happy if these are changed back to #defines before upstreaming, it
 * should be little more than a regexp search+replace operation in this file.
 */
typedef struct {
	const int x;
} cxl_p1_reg_t;
typedef struct {
	const int x;
} cxl_p1n_reg_t;
typedef struct {
	const int x;
} cxl_p2n_reg_t;
#define cxl_reg_off(reg) \
	(reg.x)

/* Memory maps. Ref CXL Appendix A */

/* PSL Privilege 1 Memory Map */
/* Configuration and Control area */
static const cxl_p1_reg_t CXL_PSL_CtxTime = {0x0000};
static const cxl_p1_reg_t CXL_PSL_ErrIVTE = {0x0008};
static const cxl_p1_reg_t CXL_PSL_KEY1    = {0x0010};
static const cxl_p1_reg_t CXL_PSL_KEY2    = {0x0018};
static const cxl_p1_reg_t CXL_PSL_Control = {0x0020};
/* Downloading */
static const cxl_p1_reg_t CXL_PSL_DLCNTL  = {0x0060};
static const cxl_p1_reg_t CXL_PSL_DLADDR  = {0x0068};

/* PSL Lookaside Buffer Management Area */
static const cxl_p1_reg_t CXL_PSL_LBISEL  = {0x0080};
static const cxl_p1_reg_t CXL_PSL_SLBIE   = {0x0088};
static const cxl_p1_reg_t CXL_PSL_SLBIA   = {0x0090};
static const cxl_p1_reg_t CXL_PSL_TLBIE   = {0x00A0};
static const cxl_p1_reg_t CXL_PSL_TLBIA   = {0x00A8};
static const cxl_p1_reg_t CXL_PSL_AFUSEL  = {0x00B0};

/* 0x00C0:7EFF Implementation dependent area */
static const cxl_p1_reg_t CXL_PSL_FIR1      = {0x0100};
static const cxl_p1_reg_t CXL_PSL_FIR2      = {0x0108};
static const cxl_p1_reg_t CXL_PSL_Timebase  = {0x0110};
static const cxl_p1_reg_t CXL_PSL_VERSION   = {0x0118};
static const cxl_p1_reg_t CXL_PSL_RESLCKTO  = {0x0128};
static const cxl_p1_reg_t CXL_PSL_TB_CTLSTAT = {0x0140};
static const cxl_p1_reg_t CXL_PSL_FIR_CNTL  = {0x0148};
static const cxl_p1_reg_t CXL_PSL_DSNDCTL   = {0x0150};
static const cxl_p1_reg_t CXL_PSL_SNWRALLOC = {0x0158};
static const cxl_p1_reg_t CXL_PSL_TRACE     = {0x0170};
/* 0x7F00:7FFF Reserved PCIe MSI-X Pending Bit Array area */
/* 0x8000:FFFF Reserved PCIe MSI-X Table Area */

/* PSL Slice Privilege 1 Memory Map */
/* Configuration Area */
static const cxl_p1n_reg_t CXL_PSL_SR_An          = {0x00};
static const cxl_p1n_reg_t CXL_PSL_LPID_An        = {0x08};
static const cxl_p1n_reg_t CXL_PSL_AMBAR_An       = {0x10};
static const cxl_p1n_reg_t CXL_PSL_SPOffset_An    = {0x18};
static const cxl_p1n_reg_t CXL_PSL_ID_An          = {0x20};
static const cxl_p1n_reg_t CXL_PSL_SERR_An        = {0x28};
/* Memory Management and Lookaside Buffer Management */
static const cxl_p1n_reg_t CXL_PSL_SDR_An         = {0x30};
static const cxl_p1n_reg_t CXL_PSL_AMOR_An        = {0x38};
/* Pointer Area */
static const cxl_p1n_reg_t CXL_HAURP_An           = {0x80};
static const cxl_p1n_reg_t CXL_PSL_SPAP_An        = {0x88};
static const cxl_p1n_reg_t CXL_PSL_LLCMD_An       = {0x90};
/* Control Area */
static const cxl_p1n_reg_t CXL_PSL_SCNTL_An       = {0xA0};
static const cxl_p1n_reg_t CXL_PSL_CtxTime_An     = {0xA8};
static const cxl_p1n_reg_t CXL_PSL_IVTE_Offset_An = {0xB0};
static const cxl_p1n_reg_t CXL_PSL_IVTE_Limit_An  = {0xB8};
/* 0xC0:FF Implementation Dependent Area */
static const cxl_p1n_reg_t CXL_PSL_FIR_SLICE_An   = {0xC0};
static const cxl_p1n_reg_t CXL_AFU_DEBUG_An       = {0xC8};
static const cxl_p1n_reg_t CXL_PSL_APCALLOC_A     = {0xD0};
static const cxl_p1n_reg_t CXL_PSL_COALLOC_A      = {0xD8};
static const cxl_p1n_reg_t CXL_PSL_RXCTL_A        = {0xE0};
static const cxl_p1n_reg_t CXL_PSL_SLICE_TRACE    = {0xE8};

/* PSL Slice Privilege 2 Memory Map */
/* Configuration and Control Area */
static const cxl_p2n_reg_t CXL_PSL_PID_TID_An = {0x000};
static const cxl_p2n_reg_t CXL_CSRP_An        = {0x008};
static const cxl_p2n_reg_t CXL_AURP0_An       = {0x010};
static const cxl_p2n_reg_t CXL_AURP1_An       = {0x018};
static const cxl_p2n_reg_t CXL_SSTP0_An       = {0x020};
static const cxl_p2n_reg_t CXL_SSTP1_An       = {0x028};
static const cxl_p2n_reg_t CXL_PSL_AMR_An     = {0x030};
/* Segment Lookaside Buffer Management */
static const cxl_p2n_reg_t CXL_SLBIE_An       = {0x040};
static const cxl_p2n_reg_t CXL_SLBIA_An       = {0x048};
static const cxl_p2n_reg_t CXL_SLBI_Select_An = {0x050};
/* Interrupt Registers */
static const cxl_p2n_reg_t CXL_PSL_DSISR_An   = {0x060};
static const cxl_p2n_reg_t CXL_PSL_DAR_An     = {0x068};
static const cxl_p2n_reg_t CXL_PSL_DSR_An     = {0x070};
static const cxl_p2n_reg_t CXL_PSL_TFC_An     = {0x078};
static const cxl_p2n_reg_t CXL_PSL_PEHandle_An = {0x080};
static const cxl_p2n_reg_t CXL_PSL_ErrStat_An = {0x088};
/* AFU Registers */
static const cxl_p2n_reg_t CXL_AFU_Cntl_An    = {0x090};
static const cxl_p2n_reg_t CXL_AFU_ERR_An     = {0x098};
/* Work Element Descriptor */
static const cxl_p2n_reg_t CXL_PSL_WED_An     = {0x0A0};
/* 0x0C0:FFF Implementation Dependent Area */

#define CXL_PSL_SPAP_Addr 0x0ffffffffffff000ULL
#define CXL_PSL_SPAP_Size 0x0000000000000ff0ULL
#define CXL_PSL_SPAP_Size_Shift 4
#define CXL_PSL_SPAP_V    0x0000000000000001ULL

/****** CXL_PSL_Control ****************************************************/
#define CXL_PSL_Control_tb 0x0000000000000001ULL

/****** CXL_PSL_DLCNTL *****************************************************/
#define CXL_PSL_DLCNTL_D (0x1ull << (63-28))
#define CXL_PSL_DLCNTL_C (0x1ull << (63-29))
#define CXL_PSL_DLCNTL_E (0x1ull << (63-30))
#define CXL_PSL_DLCNTL_S (0x1ull << (63-31))
#define CXL_PSL_DLCNTL_CE (CXL_PSL_DLCNTL_C | CXL_PSL_DLCNTL_E)
#define CXL_PSL_DLCNTL_DCES (CXL_PSL_DLCNTL_D | CXL_PSL_DLCNTL_CE | CXL_PSL_DLCNTL_S)

/****** CXL_PSL_SR_An ******************************************************/
#define CXL_PSL_SR_An_SF  MSR_SF            /* 64bit */
#define CXL_PSL_SR_An_TA  (1ull << (63-1))  /* Tags active,   GA1: 0 */
#define CXL_PSL_SR_An_HV  MSR_HV            /* Hypervisor,    GA1: 0 */
#define CXL_PSL_SR_An_PR  MSR_PR            /* Problem state, GA1: 1 */
#define CXL_PSL_SR_An_ISL (1ull << (63-53)) /* Ignore Segment Large Page */
#define CXL_PSL_SR_An_TC  (1ull << (63-54)) /* Page Table secondary hash */
#define CXL_PSL_SR_An_US  (1ull << (63-56)) /* User state,    GA1: X */
#define CXL_PSL_SR_An_SC  (1ull << (63-58)) /* Segment Table secondary hash */
#define CXL_PSL_SR_An_R   MSR_DR            /* Relocate,      GA1: 1 */
#define CXL_PSL_SR_An_MP  (1ull << (63-62)) /* Master Process */
#define CXL_PSL_SR_An_LE  (1ull << (63-63)) /* Little Endian */

/****** CXL_PSL_LLCMD_An ****************************************************/
#define CXL_LLCMD_TERMINATE   0x0001000000000000ULL
#define CXL_LLCMD_REMOVE      0x0002000000000000ULL
#define CXL_LLCMD_SUSPEND     0x0003000000000000ULL
#define CXL_LLCMD_RESUME      0x0004000000000000ULL
#define CXL_LLCMD_ADD         0x0005000000000000ULL
#define CXL_LLCMD_UPDATE      0x0006000000000000ULL
#define CXL_LLCMD_HANDLE_MASK 0x000000000000ffffULL

/****** CXL_PSL_ID_An ****************************************************/
#define CXL_PSL_ID_An_F	(1ull << (63-31))
#define CXL_PSL_ID_An_L	(1ull << (63-30))

/****** CXL_PSL_SCNTL_An ****************************************************/
#define CXL_PSL_SCNTL_An_CR          (0x1ull << (63-15))
/* Programming Modes: */
#define CXL_PSL_SCNTL_An_PM_MASK     (0xffffull << (63-31))
#define CXL_PSL_SCNTL_An_PM_Shared   (0x0000ull << (63-31))
#define CXL_PSL_SCNTL_An_PM_OS       (0x0001ull << (63-31))
#define CXL_PSL_SCNTL_An_PM_Process  (0x0002ull << (63-31))
#define CXL_PSL_SCNTL_An_PM_AFU      (0x0004ull << (63-31))
#define CXL_PSL_SCNTL_An_PM_AFU_PBT  (0x0104ull << (63-31))
/* Purge Status (ro) */
#define CXL_PSL_SCNTL_An_Ps_MASK     (0x3ull << (63-39))
#define CXL_PSL_SCNTL_An_Ps_Pending  (0x1ull << (63-39))
#define CXL_PSL_SCNTL_An_Ps_Complete (0x3ull << (63-39))
/* Purge */
#define CXL_PSL_SCNTL_An_Pc          (0x1ull << (63-48))
/* Suspend Status (ro) */
#define CXL_PSL_SCNTL_An_Ss_MASK     (0x3ull << (63-55))
#define CXL_PSL_SCNTL_An_Ss_Pending  (0x1ull << (63-55))
#define CXL_PSL_SCNTL_An_Ss_Complete (0x3ull << (63-55))
/* Suspend Control */
#define CXL_PSL_SCNTL_An_Sc          (0x1ull << (63-63))

/* AFU Slice Enable Status (ro) */
#define CXL_AFU_Cntl_An_ES_MASK     (0x7ull << (63-2))
#define CXL_AFU_Cntl_An_ES_Disabled (0x0ull << (63-2))
#define CXL_AFU_Cntl_An_ES_Enabled  (0x4ull << (63-2))
/* AFU Slice Enable */
#define CXL_AFU_Cntl_An_E           (0x1ull << (63-3))
/* AFU Slice Reset status (ro) */
#define CXL_AFU_Cntl_An_RS_MASK     (0x3ull << (63-5))
#define CXL_AFU_Cntl_An_RS_Pending  (0x1ull << (63-5))
#define CXL_AFU_Cntl_An_RS_Complete (0x2ull << (63-5))
/* AFU Slice Reset */
#define CXL_AFU_Cntl_An_RA          (0x1ull << (63-7))

/****** CXL_SSTP0/1_An ******************************************************/
/* These top bits are for the segment that CONTAINS the segment table */
#define CXL_SSTP0_An_B_SHIFT    SLB_VSID_SSIZE_SHIFT
#define CXL_SSTP0_An_KS             (1ull << (63-2))
#define CXL_SSTP0_An_KP             (1ull << (63-3))
#define CXL_SSTP0_An_N              (1ull << (63-4))
#define CXL_SSTP0_An_L              (1ull << (63-5))
#define CXL_SSTP0_An_C              (1ull << (63-6))
#define CXL_SSTP0_An_TA             (1ull << (63-7))
#define CXL_SSTP0_An_LP_SHIFT                (63-9)  /* 2 Bits */
/* And finally, the virtual address & size of the segment table: */
#define CXL_SSTP0_An_SegTableSize_SHIFT      (63-31) /* 12 Bits */
#define CXL_SSTP0_An_SegTableSize_MASK \
	(((1ull << 12) - 1) << CXL_SSTP0_An_SegTableSize_SHIFT)
#define CXL_SSTP0_An_STVA_U_MASK   ((1ull << (63-49))-1)
#define CXL_SSTP1_An_STVA_L_MASK (~((1ull << (63-55))-1))
#define CXL_SSTP1_An_V              (1ull << (63-63))

/****** CXL_PSL_SLBIE_[An] **************************************************/
/* write: */
#define CXL_SLBIE_C        PPC_BIT(36)         /* Class */
#define CXL_SLBIE_SS       PPC_BITMASK(37, 38) /* Segment Size */
#define CXL_SLBIE_SS_SHIFT PPC_BITLSHIFT(38)
#define CXL_SLBIE_TA       PPC_BIT(38)         /* Tags Active */
/* read: */
#define CXL_SLBIE_MAX      PPC_BITMASK(24, 31)
#define CXL_SLBIE_PENDING  PPC_BITMASK(56, 63)

/****** Common to all CXL_TLBIA/SLBIA_[An] **********************************/
#define CXL_TLB_SLB_P          (1ull) /* Pending (read) */

/****** Common to all CXL_TLB/SLB_IA/IE_[An] registers **********************/
#define CXL_TLB_SLB_IQ_ALL     (0ull) /* Inv qualifier */
#define CXL_TLB_SLB_IQ_LPID    (1ull) /* Inv qualifier */
#define CXL_TLB_SLB_IQ_LPIDPID (3ull) /* Inv qualifier */

/****** CXL_PSL_AFUSEL ******************************************************/
#define CXL_PSL_AFUSEL_A (1ull << (63-55)) /* Adapter wide invalidates affect all AFUs */

/****** CXL_PSL_DSISR_An ****************************************************/
#define CXL_PSL_DSISR_An_DS (1ull << (63-0))  /* Segment not found */
#define CXL_PSL_DSISR_An_DM (1ull << (63-1))  /* PTE not found (See also: M) or protection fault */
#define CXL_PSL_DSISR_An_ST (1ull << (63-2))  /* Segment Table PTE not found */
#define CXL_PSL_DSISR_An_UR (1ull << (63-3))  /* AURP PTE not found */
#define CXL_PSL_DSISR_TRANS (CXL_PSL_DSISR_An_DS | CXL_PSL_DSISR_An_DM | CXL_PSL_DSISR_An_ST | CXL_PSL_DSISR_An_UR)
#define CXL_PSL_DSISR_An_PE (1ull << (63-4))  /* PSL Error (implementation specific) */
#define CXL_PSL_DSISR_An_AE (1ull << (63-5))  /* AFU Error */
#define CXL_PSL_DSISR_An_OC (1ull << (63-6))  /* OS Context Warning */
#define CXL_PSL_DSISR_PENDING (CXL_PSL_DSISR_TRANS | CXL_PSL_DSISR_An_PE | CXL_PSL_DSISR_An_AE | CXL_PSL_DSISR_An_OC)
/* NOTE: Bits 32:63 are undefined if DSISR[DS] = 1 */
#define CXL_PSL_DSISR_An_M  DSISR_NOHPTE      /* PTE not found */
#define CXL_PSL_DSISR_An_P  DSISR_PROTFAULT   /* Storage protection violation */
#define CXL_PSL_DSISR_An_A  (1ull << (63-37)) /* AFU lock access to write through or cache inhibited storage */
#define CXL_PSL_DSISR_An_S  DSISR_ISSTORE     /* Access was afu_wr or afu_zero */
#define CXL_PSL_DSISR_An_K  DSISR_KEYFAULT    /* Access not permitted by virtual page class key protection */

/****** CXL_PSL_TFC_An ******************************************************/
#define CXL_PSL_TFC_An_A  (1ull << (63-28)) /* Acknowledge non-translation fault */
#define CXL_PSL_TFC_An_C  (1ull << (63-29)) /* Continue (abort transaction) */
#define CXL_PSL_TFC_An_AE (1ull << (63-30)) /* Restart PSL with address error */
#define CXL_PSL_TFC_An_R  (1ull << (63-31)) /* Restart PSL transaction */

/* cxl_process_element->software_status */
#define CXL_PE_SOFTWARE_STATE_V (1ul << (31 -  0)) /* Valid */
#define CXL_PE_SOFTWARE_STATE_C (1ul << (31 - 29)) /* Complete */
#define CXL_PE_SOFTWARE_STATE_S (1ul << (31 - 30)) /* Suspend */
#define CXL_PE_SOFTWARE_STATE_T (1ul << (31 - 31)) /* Terminate */

/****** CXL_PSL_RXCTL_An (Implementation Specific) **************************
 * Controls AFU Hang Pulse, which sets the timeout for the AFU to respond to
 * the PSL for any response (except MMIO). Timeouts will occur between 1x to 2x
 * of the hang pulse frequency.
 */
#define CXL_PSL_RXCTL_AFUHP_4S      0x7000000000000000ULL

/* SPA->sw_command_status */
#define CXL_SPA_SW_CMD_MASK         0xffff000000000000ULL
#define CXL_SPA_SW_CMD_TERMINATE    0x0001000000000000ULL
#define CXL_SPA_SW_CMD_REMOVE       0x0002000000000000ULL
#define CXL_SPA_SW_CMD_SUSPEND      0x0003000000000000ULL
#define CXL_SPA_SW_CMD_RESUME       0x0004000000000000ULL
#define CXL_SPA_SW_CMD_ADD          0x0005000000000000ULL
#define CXL_SPA_SW_CMD_UPDATE       0x0006000000000000ULL
#define CXL_SPA_SW_STATE_MASK       0x0000ffff00000000ULL
#define CXL_SPA_SW_STATE_TERMINATED 0x0000000100000000ULL
#define CXL_SPA_SW_STATE_REMOVED    0x0000000200000000ULL
#define CXL_SPA_SW_STATE_SUSPENDED  0x0000000300000000ULL
#define CXL_SPA_SW_STATE_RESUMED    0x0000000400000000ULL
#define CXL_SPA_SW_STATE_ADDED      0x0000000500000000ULL
#define CXL_SPA_SW_STATE_UPDATED    0x0000000600000000ULL
#define CXL_SPA_SW_PSL_ID_MASK      0x00000000ffff0000ULL
#define CXL_SPA_SW_LINK_MASK        0x000000000000ffffULL

#define CXL_MAX_SLICES 4
#define MAX_AFU_MMIO_REGS 3

#define CXL_MODE_TIME_SLICED 0x4
#define CXL_SUPPORTED_MODES (CXL_MODE_DEDICATED | CXL_MODE_DIRECTED)

#define CXL_DEV_MINORS 13   /* 1 control + 4 AFUs * 3 (dedicated/master/shared) */
#define CXL_CARD_MINOR(adapter) (adapter->adapter_num * CXL_DEV_MINORS)
#define CXL_DEVT_ADAPTER(dev) (MINOR(dev) / CXL_DEV_MINORS)

enum cxl_context_status {
	CLOSED,
	OPENED,
	STARTED
};

enum prefault_modes {
	CXL_PREFAULT_NONE,
	CXL_PREFAULT_WED,
	CXL_PREFAULT_ALL,
};

enum cxl_attrs {
	CXL_ADAPTER_ATTRS,
	CXL_AFU_MASTER_ATTRS,
	CXL_AFU_ATTRS,
};

struct cxl_sste {
	__be64 esid_data;
	__be64 vsid_data;
};

#define to_cxl_adapter(d) container_of(d, struct cxl, dev)
#define to_cxl_afu(d) container_of(d, struct cxl_afu, dev)

struct cxl_afu_native {
	void __iomem *p1n_mmio;
	void __iomem *afu_desc_mmio;
	irq_hw_number_t psl_hwirq;
	unsigned int psl_virq;
	struct mutex spa_mutex;
	/*
	 * Only the first part of the SPA is used for the process element
	 * linked list. The only other part that software needs to worry about
	 * is sw_command_status, which we store a separate pointer to.
	 * Everything else in the SPA is only used by hardware
	 */
	struct cxl_process_element *spa;
	__be64 *sw_command_status;
	unsigned int spa_size;
	int spa_order;
	int spa_max_procs;
	u64 pp_offset;
};

struct cxl_afu_guest {
	struct cxl_afu *parent;
	u64 handle;
	phys_addr_t p2n_phys;
	u64 p2n_size;
	int max_ints;
	bool handle_err;
	struct delayed_work work_err;
	int previous_state;
};

struct cxl_afu {
	struct cxl_afu_native *native;
	struct cxl_afu_guest *guest;
	irq_hw_number_t serr_hwirq;
	unsigned int serr_virq;
	char *psl_irq_name;
	char *err_irq_name;
	void __iomem *p2n_mmio;
	phys_addr_t psn_phys;
	u64 pp_size;

	struct cxl *adapter;
	struct device dev;
	struct cdev afu_cdev_s, afu_cdev_m, afu_cdev_d;
	struct device *chardev_s, *chardev_m, *chardev_d;
	struct idr contexts_idr;
	struct dentry *debugfs;
	struct mutex contexts_lock;
	spinlock_t afu_cntl_lock;

	/* AFU error buffer fields and bin attribute for sysfs */
	u64 eb_len, eb_offset;
	struct bin_attribute attr_eb;

	/* pointer to the vphb */
	struct pci_controller *phb;

	int pp_irqs;
	int irqs_max;
	int num_procs;
	int max_procs_virtualised;
	int slice;
	int modes_supported;
	int current_mode;
	int crs_num;
	u64 crs_len;
	u64 crs_offset;
	struct list_head crs;
	enum prefault_modes prefault_mode;
	bool psa;
	bool pp_psa;
	bool enabled;
};

/* AFU refcount management */
static inline struct cxl_afu *cxl_afu_get(struct cxl_afu *afu)
{

	return (get_device(&afu->dev) == NULL) ? NULL : afu;
}

static inline void  cxl_afu_put(struct cxl_afu *afu)
{
	put_device(&afu->dev);
}


struct cxl_irq_name {
	struct list_head list;
	char *name;
};

struct irq_avail {
	irq_hw_number_t offset;
	irq_hw_number_t range;
	unsigned long   *bitmap;
};

/*
 * This is a cxl context.  If the PSL is in dedicated mode, there will be one
 * of these per AFU.  If in AFU directed there can be lots of these.
 */
struct cxl_context {
	struct cxl_afu *afu;

	/* Problem state MMIO */
	phys_addr_t psn_phys;
	u64 psn_size;

	/* Used to unmap any mmaps when force detaching */
	struct address_space *mapping;
	struct mutex mapping_lock;
	struct page *ff_page;
	bool mmio_err_ff;
	bool kernelapi;

	spinlock_t sste_lock; /* Protects segment table entries */
	struct cxl_sste *sstp;
	u64 sstp0, sstp1;
	unsigned int sst_size, sst_lru;

	wait_queue_head_t wq;
	/* pid of the group leader associated with the pid */
	struct pid *glpid;
	/* use mm context associated with this pid for ds faults */
	struct pid *pid;
	spinlock_t lock; /* Protects pending_irq_mask, pending_fault and fault_addr */
	/* Only used in PR mode */
	u64 process_token;

	unsigned long *irq_bitmap; /* Accessed from IRQ context */
	struct cxl_irq_ranges irqs;
	struct list_head irq_names;
	u64 fault_addr;
	u64 fault_dsisr;
	u64 afu_err;

	/*
	 * This status and it's lock pretects start and detach context
	 * from racing.  It also prevents detach from racing with
	 * itself
	 */
	enum cxl_context_status status;
	struct mutex status_mutex;


	/* XXX: Is it possible to need multiple work items at once? */
	struct work_struct fault_work;
	u64 dsisr;
	u64 dar;

	struct cxl_process_element *elem;

	/*
	 * pe is the process element handle, assigned by this driver when the
	 * context is initialized.
	 *
	 * external_pe is the PE shown outside of cxl.
	 * On bare-metal, pe=external_pe, because we decide what the handle is.
	 * In a guest, we only find out about the pe used by pHyp when the
	 * context is attached, and that's the value we want to report outside
	 * of cxl.
	 */
	int pe;
	int external_pe;

	u32 irq_count;
	bool pe_inserted;
	bool master;
	bool kernel;
	bool pending_irq;
	bool pending_fault;
	bool pending_afu_err;

	struct rcu_head rcu;
};

struct cxl_native {
	u64 afu_desc_off;
	u64 afu_desc_size;
	void __iomem *p1_mmio;
	void __iomem *p2_mmio;
	irq_hw_number_t err_hwirq;
	unsigned int err_virq;
	u64 ps_off;
};

struct cxl_guest {
	struct platform_device *pdev;
	int irq_nranges;
	struct cdev cdev;
	irq_hw_number_t irq_base_offset;
	struct irq_avail *irq_avail;
	spinlock_t irq_alloc_lock;
	u64 handle;
	char *status;
	u16 vendor;
	u16 device;
	u16 subsystem_vendor;
	u16 subsystem;
};

struct cxl {
	struct cxl_native *native;
	struct cxl_guest *guest;
	spinlock_t afu_list_lock;
	struct cxl_afu *afu[CXL_MAX_SLICES];
	struct device dev;
	struct dentry *trace;
	struct dentry *psl_err_chk;
	struct dentry *debugfs;
	char *irq_name;
	struct bin_attribute cxl_attr;
	int adapter_num;
	int user_irqs;
	u64 ps_size;
	u16 psl_rev;
	u16 base_image;
	u8 vsec_status;
	u8 caia_major;
	u8 caia_minor;
	u8 slices;
	bool user_image_loaded;
	bool perst_loads_image;
	bool perst_select_user;
	bool perst_same_image;
	bool psl_timebase_synced;
};

int cxl_pci_alloc_one_irq(struct cxl *adapter);
void cxl_pci_release_one_irq(struct cxl *adapter, int hwirq);
int cxl_pci_alloc_irq_ranges(struct cxl_irq_ranges *irqs, struct cxl *adapter, unsigned int num);
void cxl_pci_release_irq_ranges(struct cxl_irq_ranges *irqs, struct cxl *adapter);
int cxl_pci_setup_irq(struct cxl *adapter, unsigned int hwirq, unsigned int virq);
int cxl_update_image_control(struct cxl *adapter);
int cxl_pci_reset(struct cxl *adapter);
void cxl_pci_release_afu(struct device *dev);
ssize_t cxl_pci_read_adapter_vpd(struct cxl *adapter, void *buf, size_t len);

/* common == phyp + powernv */
struct cxl_process_element_common {
	__be32 tid;
	__be32 pid;
	__be64 csrp;
	__be64 aurp0;
	__be64 aurp1;
	__be64 sstp0;
	__be64 sstp1;
	__be64 amr;
	u8     reserved3[4];
	__be64 wed;
} __packed;

/* just powernv */
struct cxl_process_element {
	__be64 sr;
	__be64 SPOffset;
	__be64 sdr;
	__be64 haurp;
	__be32 ctxtime;
	__be16 ivte_offsets[4];
	__be16 ivte_ranges[4];
	__be32 lpid;
	struct cxl_process_element_common common;
	__be32 software_state;
} __packed;

static inline bool cxl_adapter_link_ok(struct cxl *cxl, struct cxl_afu *afu)
{
	struct pci_dev *pdev;

	if (cpu_has_feature(CPU_FTR_HVMODE)) {
		pdev = to_pci_dev(cxl->dev.parent);
		return !pci_channel_offline(pdev);
	}
	return true;
}

static inline void __iomem *_cxl_p1_addr(struct cxl *cxl, cxl_p1_reg_t reg)
{
	WARN_ON(!cpu_has_feature(CPU_FTR_HVMODE));
	return cxl->native->p1_mmio + cxl_reg_off(reg);
}

static inline void cxl_p1_write(struct cxl *cxl, cxl_p1_reg_t reg, u64 val)
{
	if (likely(cxl_adapter_link_ok(cxl, NULL)))
		out_be64(_cxl_p1_addr(cxl, reg), val);
}

static inline u64 cxl_p1_read(struct cxl *cxl, cxl_p1_reg_t reg)
{
	if (likely(cxl_adapter_link_ok(cxl, NULL)))
		return in_be64(_cxl_p1_addr(cxl, reg));
	else
		return ~0ULL;
}

static inline void __iomem *_cxl_p1n_addr(struct cxl_afu *afu, cxl_p1n_reg_t reg)
{
	WARN_ON(!cpu_has_feature(CPU_FTR_HVMODE));
	return afu->native->p1n_mmio + cxl_reg_off(reg);
}

static inline void cxl_p1n_write(struct cxl_afu *afu, cxl_p1n_reg_t reg, u64 val)
{
	if (likely(cxl_adapter_link_ok(afu->adapter, afu)))
		out_be64(_cxl_p1n_addr(afu, reg), val);
}

static inline u64 cxl_p1n_read(struct cxl_afu *afu, cxl_p1n_reg_t reg)
{
	if (likely(cxl_adapter_link_ok(afu->adapter, afu)))
		return in_be64(_cxl_p1n_addr(afu, reg));
	else
		return ~0ULL;
}

static inline void __iomem *_cxl_p2n_addr(struct cxl_afu *afu, cxl_p2n_reg_t reg)
{
	return afu->p2n_mmio + cxl_reg_off(reg);
}

static inline void cxl_p2n_write(struct cxl_afu *afu, cxl_p2n_reg_t reg, u64 val)
{
	if (likely(cxl_adapter_link_ok(afu->adapter, afu)))
		out_be64(_cxl_p2n_addr(afu, reg), val);
}

static inline u64 cxl_p2n_read(struct cxl_afu *afu, cxl_p2n_reg_t reg)
{
	if (likely(cxl_adapter_link_ok(afu->adapter, afu)))
		return in_be64(_cxl_p2n_addr(afu, reg));
	else
		return ~0ULL;
}

ssize_t cxl_pci_afu_read_err_buffer(struct cxl_afu *afu, char *buf,
				loff_t off, size_t count);


struct cxl_calls {
	void (*cxl_slbia)(struct mm_struct *mm);
	struct module *owner;
};
int register_cxl_calls(struct cxl_calls *calls);
void unregister_cxl_calls(struct cxl_calls *calls);
int cxl_update_properties(struct device_node *dn, struct property *new_prop);

void cxl_remove_adapter_nr(struct cxl *adapter);

int cxl_alloc_spa(struct cxl_afu *afu);
void cxl_release_spa(struct cxl_afu *afu);

dev_t cxl_get_dev(void);
int cxl_file_init(void);
void cxl_file_exit(void);
int cxl_register_adapter(struct cxl *adapter);
int cxl_register_afu(struct cxl_afu *afu);
int cxl_chardev_d_afu_add(struct cxl_afu *afu);
int cxl_chardev_m_afu_add(struct cxl_afu *afu);
int cxl_chardev_s_afu_add(struct cxl_afu *afu);
void cxl_chardev_afu_remove(struct cxl_afu *afu);

void cxl_context_detach_all(struct cxl_afu *afu);
void cxl_context_free(struct cxl_context *ctx);
void cxl_context_detach(struct cxl_context *ctx);

int cxl_sysfs_adapter_add(struct cxl *adapter);
void cxl_sysfs_adapter_remove(struct cxl *adapter);
int cxl_sysfs_afu_add(struct cxl_afu *afu);
void cxl_sysfs_afu_remove(struct cxl_afu *afu);
int cxl_sysfs_afu_m_add(struct cxl_afu *afu);
void cxl_sysfs_afu_m_remove(struct cxl_afu *afu);

struct cxl *cxl_alloc_adapter(void);
struct cxl_afu *cxl_alloc_afu(struct cxl *adapter, int slice);
int cxl_afu_select_best_mode(struct cxl_afu *afu);

int cxl_native_register_psl_irq(struct cxl_afu *afu);
void cxl_native_release_psl_irq(struct cxl_afu *afu);
int cxl_native_register_psl_err_irq(struct cxl *adapter);
void cxl_native_release_psl_err_irq(struct cxl *adapter);
int cxl_native_register_serr_irq(struct cxl_afu *afu);
void cxl_native_release_serr_irq(struct cxl_afu *afu);
int afu_register_irqs(struct cxl_context *ctx, u32 count);
void afu_release_irqs(struct cxl_context *ctx, void *cookie);
void afu_irq_name_free(struct cxl_context *ctx);

int cxl_debugfs_init(void);
void cxl_debugfs_exit(void);
int cxl_debugfs_adapter_add(struct cxl *adapter);
void cxl_debugfs_adapter_remove(struct cxl *adapter);
int cxl_debugfs_afu_add(struct cxl_afu *afu);
void cxl_debugfs_afu_remove(struct cxl_afu *afu);

void cxl_handle_fault(struct work_struct *work);
void cxl_prefault(struct cxl_context *ctx, u64 wed);

struct cxl *get_cxl_adapter(int num);
int cxl_alloc_sst(struct cxl_context *ctx);
void cxl_dump_debug_buffer(void *addr, size_t size);

void init_cxl_native(void);

struct cxl_context *cxl_context_alloc(void);
int cxl_context_init(struct cxl_context *ctx, struct cxl_afu *afu, bool master);
void cxl_context_set_mapping(struct cxl_context *ctx,
			struct address_space *mapping);
void cxl_context_free(struct cxl_context *ctx);
int cxl_context_iomap(struct cxl_context *ctx, struct vm_area_struct *vma);
unsigned int cxl_map_irq(struct cxl *adapter, irq_hw_number_t hwirq,
			 irq_handler_t handler, void *cookie, const char *name);
void cxl_unmap_irq(unsigned int virq, void *cookie);
int __detach_context(struct cxl_context *ctx);

/*
 * This must match the layout of the H_COLLECT_CA_INT_INFO retbuf defined
 * in PAPR.
 * A word about endianness: a pointer to this structure is passed when
 * calling the hcall. However, it is not a block of memory filled up by
 * the hypervisor. The return values are found in registers, and copied
 * one by one when returning from the hcall. See the end of the call to
 * plpar_hcall9() in hvCall.S
 * As a consequence:
 * - we don't need to do any endianness conversion
 * - the pid and tid are an exception. They are 32-bit values returned in
 *   the same 64-bit register. So we do need to worry about byte ordering.
 */
struct cxl_irq_info {
	u64 dsisr;
	u64 dar;
	u64 dsr;
#ifndef CONFIG_CPU_LITTLE_ENDIAN
	u32 pid;
	u32 tid;
#else
	u32 tid;
	u32 pid;
#endif
	u64 afu_err;
	u64 errstat;
	u64 proc_handle;
	u64 padding[2]; /* to match the expected retbuf size for plpar_hcall9 */
};

void cxl_assign_psn_space(struct cxl_context *ctx);
irqreturn_t cxl_irq(int irq, struct cxl_context *ctx, struct cxl_irq_info *irq_info);
int cxl_register_one_irq(struct cxl *adapter, irq_handler_t handler,
			void *cookie, irq_hw_number_t *dest_hwirq,
			unsigned int *dest_virq, const char *name);

int cxl_check_error(struct cxl_afu *afu);
int cxl_afu_slbia(struct cxl_afu *afu);
int cxl_tlb_slb_invalidate(struct cxl *adapter);
int cxl_afu_disable(struct cxl_afu *afu);
int cxl_psl_purge(struct cxl_afu *afu);

void cxl_stop_trace(struct cxl *cxl);
int cxl_pci_vphb_add(struct cxl_afu *afu);
void cxl_pci_vphb_remove(struct cxl_afu *afu);
void cxl_release_mapping(struct cxl_context *ctx);

extern struct pci_driver cxl_pci_driver;
extern struct platform_driver cxl_of_driver;
int afu_allocate_irqs(struct cxl_context *ctx, u32 count);

int afu_open(struct inode *inode, struct file *file);
int afu_release(struct inode *inode, struct file *file);
long afu_ioctl(struct file *file, unsigned int cmd, unsigned long arg);
int afu_mmap(struct file *file, struct vm_area_struct *vm);
unsigned int afu_poll(struct file *file, struct poll_table_struct *poll);
ssize_t afu_read(struct file *file, char __user *buf, size_t count, loff_t *off);
extern const struct file_operations afu_fops;

struct cxl *cxl_guest_init_adapter(struct device_node *np, struct platform_device *dev);
void cxl_guest_remove_adapter(struct cxl *adapter);
int cxl_of_read_adapter_handle(struct cxl *adapter, struct device_node *np);
int cxl_of_read_adapter_properties(struct cxl *adapter, struct device_node *np);
ssize_t cxl_guest_read_adapter_vpd(struct cxl *adapter, void *buf, size_t len);
ssize_t cxl_guest_read_afu_vpd(struct cxl_afu *afu, void *buf, size_t len);
int cxl_guest_init_afu(struct cxl *adapter, int slice, struct device_node *afu_np);
void cxl_guest_remove_afu(struct cxl_afu *afu);
int cxl_of_read_afu_handle(struct cxl_afu *afu, struct device_node *afu_np);
int cxl_of_read_afu_properties(struct cxl_afu *afu, struct device_node *afu_np);
int cxl_guest_add_chardev(struct cxl *adapter);
void cxl_guest_remove_chardev(struct cxl *adapter);
void cxl_guest_reload_module(struct cxl *adapter);
int cxl_of_probe(struct platform_device *pdev);

struct cxl_backend_ops {
	struct module *module;
	int (*adapter_reset)(struct cxl *adapter);
	int (*alloc_one_irq)(struct cxl *adapter);
	void (*release_one_irq)(struct cxl *adapter, int hwirq);
	int (*alloc_irq_ranges)(struct cxl_irq_ranges *irqs,
				struct cxl *adapter, unsigned int num);
	void (*release_irq_ranges)(struct cxl_irq_ranges *irqs,
				struct cxl *adapter);
	int (*setup_irq)(struct cxl *adapter, unsigned int hwirq,
			unsigned int virq);
	irqreturn_t (*handle_psl_slice_error)(struct cxl_context *ctx,
					u64 dsisr, u64 errstat);
	irqreturn_t (*psl_interrupt)(int irq, void *data);
	int (*ack_irq)(struct cxl_context *ctx, u64 tfc, u64 psl_reset_mask);
	void (*irq_wait)(struct cxl_context *ctx);
	int (*attach_process)(struct cxl_context *ctx, bool kernel,
			u64 wed, u64 amr);
	int (*detach_process)(struct cxl_context *ctx);
	bool (*support_attributes)(const char *attr_name, enum cxl_attrs type);
	bool (*link_ok)(struct cxl *cxl, struct cxl_afu *afu);
	void (*release_afu)(struct device *dev);
	ssize_t (*afu_read_err_buffer)(struct cxl_afu *afu, char *buf,
				loff_t off, size_t count);
	int (*afu_check_and_enable)(struct cxl_afu *afu);
	int (*afu_activate_mode)(struct cxl_afu *afu, int mode);
	int (*afu_deactivate_mode)(struct cxl_afu *afu, int mode);
	int (*afu_reset)(struct cxl_afu *afu);
	int (*afu_cr_read8)(struct cxl_afu *afu, int cr_idx, u64 offset, u8 *val);
	int (*afu_cr_read16)(struct cxl_afu *afu, int cr_idx, u64 offset, u16 *val);
	int (*afu_cr_read32)(struct cxl_afu *afu, int cr_idx, u64 offset, u32 *val);
	int (*afu_cr_read64)(struct cxl_afu *afu, int cr_idx, u64 offset, u64 *val);
	int (*afu_cr_write8)(struct cxl_afu *afu, int cr_idx, u64 offset, u8 val);
	int (*afu_cr_write16)(struct cxl_afu *afu, int cr_idx, u64 offset, u16 val);
	int (*afu_cr_write32)(struct cxl_afu *afu, int cr_idx, u64 offset, u32 val);
	ssize_t (*read_adapter_vpd)(struct cxl *adapter, void *buf, size_t count);
};
extern const struct cxl_backend_ops cxl_native_ops;
extern const struct cxl_backend_ops cxl_guest_ops;
extern const struct cxl_backend_ops *cxl_ops;

/* check if the given pci_dev is on the the cxl vphb bus */
bool cxl_pci_is_vphb_device(struct pci_dev *dev);
#endif
