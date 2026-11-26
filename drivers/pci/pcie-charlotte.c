// SPDX-License-Identifier: GPL-2.0
/*
 * PCIe host controller glue for HiSilicon Kirin 9000C ("charlotte").
 *
 * The SoC has two DesignWare-based PCIe root complexes hidden behind a
 * Huawei "kport" wrapper (APB control block + xw/kphy PHY + CRG gates).
 * Mainline has no driver for this block, so this file reproduces the
 * power-on/PHY-up sequence of the vendor pcie-kport/pcie-jun stack and
 * then hands the controller to the generic DesignWare host core.
 *
 * Register offsets and sequences are taken from the vendor source
 * (platform_source/basicplatform/drivers/pci/host/pcie-jun.c and
 * pcie-kport-common.c) and the clock tables in
 * platform_source/basicplatform/drivers/clk/extreme/platform/clk-char-cs.c.
 */

#include <linux/arm-smccc.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/printk.h>

#include "pcie-designware.h"

/* APB wrapper registers */
#define APB_CTRL0		0x000
#define APB_CTRL1		0x004
#define APB_CTRL7		0x01c
#define APB_CTRL12		0x030
#define APB_CTRL20		0x050
#define APB_CTRL21		0x054
#define APB_CTRL22		0x058
#define APB_CTRL23		0x05c
#define APB_CTRL25		0x064
#define APB_CTRL30		0x078
#define APB_STATE4		0x410

#define PCIE_LTSSM_ENABLE_BIT	BIT(11)

#define PCIE_TYPE_MASK		0xfu
#define PCIE_TYPE_SHIFT		28
#define PCIE_TYPE_RC		4

#define PERST_IN_N_CTRL_BIT2	BIT(2)
#define PERST_IN_N_CTRL_BIT3	BIT(3)
#define PERST_OE_EN		BIT(1)
#define PERST_OUT_N		BIT(0)

#define CTRL7_AUX_PWR_DET	BIT(10)
#define CTRL22_CLKREQ_OUT	BIT(0)
#define CTRL25_TIMEOUT_MASK	(0x3f << 1)
#define CTRL25_TIMEOUT_VAL	(0x36 << 1)
#define CTRL1_ELBI_DBI_EN	BIT(21)
#define CTRL30_DBI_EP_SPLT	BIT(0)
#define CTRL20_TP_MEM_TRA	BIT(27)
#define CTRL23_SP_MEM_TRA	BIT(16)

/* APB PHY registers (apb_phy window = phy_base + 0x2000) */
#define APB_PHY_OFFSET		0x2000
#define PHY_CTRL0		0x000
#define PHY_CTRL1		0x004
#define PHY_CTRL39		0x09c
#define PHY_STATE0		0x400
#define PHY_PCS_POWER_STATUS	0x1400

#define PHY_REF_USE_CIO_PAD	BIT(14)
#define PHY_POR_N_RESET		BIT(1)
#define PHY_RESET		BIT(17)
#define PHY_PIPE_LINE0_RESET	BIT(19)
#define PHY_RX_TERMINATION	BIT(10)
#define PHY_PIPE_CLK_STABLE	BIT(19)
#define PHY_POWERON_STATUS	BIT(7)

/* HSDT CRG (0xee262000) */
#define CRG_PEREN1		0x010
#define CRG_PERDIS0		0x004
#define CRG_PERDIS1		0x014
#define CRG_PERRSTEN0		0x060
#define CRG_PERRSTDIS0		0x064
#define CRG_PCIECTRL0		0x300
#define CRG_PCIECTRL1		0x304

/* HSDT CRG clock gates for PCIe0 (bank 1) and PCIe1 (bank 0) */
#define CLK_PCIE0_PHY		BIT(3)
#define CLK_PCIE0_SYS		BIT(4)
#define CLK_PCIE0_ACLK		BIT(5)
#define CLK_PCIE0_PHYREF	BIT(6)
#define CLK_PCIE0_TCU		BIT(10)
#define CLK_PCIE0_TBU		BIT(11)
#define CLK_PCIE0_HP		BIT(1)
#define CLK_PCIE0_DEBOUNCE	BIT(2)

#define CLK_PCIE1_PHY		BIT(8)
#define CLK_PCIE1_SYS		BIT(9)
#define CLK_PCIE1_ACLK		BIT(10)
#define CLK_PCIE1_PHYREF	BIT(4)
#define CLK_PCIE1_TCU		BIT(10)	/* shared TCU gate lives in bank 1 */
#define CLK_PCIE1_TBU		BIT(13)
#define CLK_PCIE1_HP		BIT(6)
#define CLK_PCIE1_DEBOUNCE	BIT(7)

#define RST_PCIE0_POR_N		BIT(7)
#define RST_PCIE0_ASYNCBRG	BIT(4)
#define RST_PCIE0_TCU		BIT(3)
#define RST_PCIE0_TBU		BIT(5)
#define RST_PCIE1_POR_N		BIT(21)
#define RST_PCIE1_ASYNCBRG	BIT(1)
#define RST_PCIE1_TCU		BIT(3)
#define RST_PCIE1_TBU		BIT(25)

/* HSDT FNPLL (0xee263000) */
#define PLL_CTRL0		0x190
#define PLL_FCW		0x194
#define PLL_STATE		0x204
#define PLL_EN			BIT(0)
#define PLL_LOCK		BIT(4)
#define PLL_FCW_100M		0x2ee00000

/* SCTRL (0xfb21b000): PCIe aux gate */
#define SCTRL_PCIE_AUX		0x170
#define SCTRL_PCIE0_AUX_EN	BIT(19)
#define SCTRL_PCIE1_AUX_EN	BIT(20)
#define SCTRL_PCIE_ISO_DIS	0x004
#define SCTRL_PCIE0_ISO		BIT(3)
#define SCTRL_PCIE1_ISO		BIT(4)

/* ATF SMC IDs (vendor bl31_smc.h, group 0x27) */
#define FID_PCIE_SET_TBU_BYPASS	0xc3002700
#define FID_PCIE_GET_VREF_SEL	0xc3002703
#define FID_PCIE_SET_MEM_UNSEC	0xc3002702

/* PMCTRL (0xffb81000): NoC power idle */
#define PMC_NOC_IDLEREQ		0x380
#define PMC_NOC_IDLE		0x384
#define NOC_PCIE0_POWER		BIT(9)
#define NOC_PCIE0_POWER_MASK	BIT(25)
#define NOC_PCIE1_POWER		BIT(10)
#define NOC_PCIE1_POWER_MASK	BIT(26)

struct clt_pcie {
	struct dw_pcie pci;
	void __iomem *apb;
	void __iomem *phy;
	void __iomem *crg;
	void __iomem *pll;
	void __iomem *sctrl;
	void __iomem *pmctrl;
	void __iomem *gpio;
	u32 rc_id;
	int perst_gpio;
	u16 ffa_src_id;
	u16 ffa_dst_id;
};

static inline u32 clt_apb_read(struct clt_pcie *c, u32 reg)
{
	return readl(c->apb + reg);
}

static inline void clt_apb_write(struct clt_pcie *c, u32 reg, u32 val)
{
	writel(val, c->apb + reg);
}

static inline u32 clt_phy_read(struct clt_pcie *c, u32 reg)
{
	return readl(c->phy + APB_PHY_OFFSET + reg);
}

static inline void clt_phy_write(struct clt_pcie *c, u32 reg, u32 val)
{
	writel(val, c->phy + APB_PHY_OFFSET + reg);
}

static inline void clt_phy_native_write(struct clt_pcie *c, u32 reg, u32 val)
{
	writel(val, c->phy + reg);
}

static inline u32 clt_phy_native_read(struct clt_pcie *c, u32 reg)
{
	return readl(c->phy + reg);
}

static void clt_clk_pcie1_on(struct clt_pcie *c, u32 bits)
{
	writel(bits, c->crg + 0x000);
}

static void clt_clk_pcie0_on(struct clt_pcie *c, u32 bits)
{
	writel(bits, c->crg + CRG_PEREN1);
}

/*
 * PERST is not wired to the controller's internal perst logic on this
 * board: it comes from a hiv500 GPIO (pin 14 of gpio@ff806000 for PCIe0,
 * pin 9 of gpio@ee267000 for PCIe1).  Data register 0x04 drives pins
 * 0..15 with the upper 16 bits acting as a write mask; the per-pin
 * control register at 0x08 + 4*n selects output mode.
 */
static void clt_perst_set(struct clt_pcie *c, int level)
{
	u32 bit = c->perst_gpio;
	u32 val = BIT(bit + 16);

	if (level)
		val |= BIT(bit);
	writel(val, c->gpio + 0x04);
	writel(0x00100010, c->gpio + 0x08 + 4 * bit);
}

/* Gen3 TX de-emphasis / local FS-LF, from kphy_phy_ffe_cfg() */
static void clt_phy_ffe_cfg(struct clt_pcie *c)
{
	u32 rtermk;

	udelay(350);
	rtermk = (clt_phy_native_read(c, 0x0a78) >> 8) & 1;
	if (rtermk) {
		clt_phy_native_write(c, 0x1550, 0x10002f00);
		clt_phy_native_write(c, 0x1554, 0x0b003400);
		clt_phy_native_write(c, 0x1558, 0x3f00);
		clt_phy_native_write(c, 0x1408, 0xf3f);
	} else {
		clt_phy_native_write(c, 0x1408, 0xc2f);
	}
}

static int clt_wait_phy_ready(struct clt_pcie *c)
{
	u32 val;
	int i;

	for (i = 0; i < 500; i++) {
		val = clt_phy_native_read(c, PHY_PCS_POWER_STATUS);
		if ((val >> 7) & 1)
			return 0;
		udelay(5);
	}
	dev_err(c->pci.dev, "PHY power-on timeout, status=%#x\n", val);
	return -ETIMEDOUT;
}

static void clt_write_pll(struct clt_pcie *c, u32 reg, u32 val)
{
	writel(val, c->pll + reg);
}

/* FNPLL setup, straight from pcie-jun.c:pcie_fnpll_cfg_init_set() */
static void clt_fnpll_init(struct clt_pcie *c)
{
	clt_write_pll(c, 0x0008, 0x0000008a);
	clt_write_pll(c, 0x000c, 0x0000000c);
	clt_write_pll(c, 0x0010, 0x000002be);
	clt_write_pll(c, 0x0014, 0x00000000);
	clt_write_pll(c, 0x0018, 0x00300000);
	clt_write_pll(c, 0x001c, 0x026020a0);
	clt_write_pll(c, 0x0024, 0x00012abc);
	clt_write_pll(c, 0x0028, 0x03666e1f);
	clt_write_pll(c, 0x002c, 0x6420030a);
	clt_write_pll(c, 0x0030, 0x5620153f);
	clt_write_pll(c, 0x0034, 0x2005b08e);
	clt_write_pll(c, 0x0038, 0x00003543);
	clt_write_pll(c, 0x003c, 0x00000000);
	clt_write_pll(c, 0x0040, 0x00010000);
	clt_write_pll(c, 0x0044, 0x00000003);
	clt_write_pll(c, 0x0048, 0x000120ff);
	clt_write_pll(c, 0x004c, 0x0080103f);
	clt_write_pll(c, 0x0050, 0x80800a0a);
	clt_write_pll(c, 0x0054, 0x8848d00f);
	clt_write_pll(c, 0x005c, 0x00000000);
	clt_write_pll(c, 0x0060, 0x00100010);
	clt_write_pll(c, 0x0064, 0x0020a420);
	clt_write_pll(c, 0x0068, 0x01fe1fff);
	clt_write_pll(c, 0x006c, 0x000ff102);
	clt_write_pll(c, 0x0070, 0x00000000);
}

static int clt_fnpll_enable(struct clt_pcie *c)
{
	u32 val;

	clt_fnpll_init(c);
	udelay(1);
	/* EN = 0, BYPASS = 1, then FCW for 100MHz refclk */
	writel(0x2, c->crg + PLL_CTRL0);
	writel(PLL_FCW_100M, c->crg + PLL_FCW);
	udelay(5);
	writel(PLL_EN, c->crg + PLL_CTRL0);

	return readl_poll_timeout(c->crg + PLL_STATE, val, val & PLL_LOCK,
				  10, 100000);
}

/*
 * kphy PHY init table.  The DT phy node has version = <1>, which maps to
 * PCIE_PHY_VERSION_2 in the vendor driver (PCIE_PHY_VERSION_1 == 0), so
 * this is pcie_phy_init_tbl_v2[] from pcie-kport-phy-kphy.c.
 */
static const u32 clt_phy_init_tbl[][2] = {
	{0x10000, 0x0ac8},
	{0x800400ff, 0x0b0c},
	{0x0, 0x0b78},
	{0x1000, 0x1540},
	{0x22201ff, 0x1a1c},
	{0x4440000, 0x1a18},
	{0x20180010, 0x1534},
	{0x410018, 0x1a14},
	{0x40410439, 0x1580},
	{0x557, 0x1588},
	{0x26100030, 0x1530},
	{0x25, 0x1a10},
	{0x30a354, 0x1a04},
	{0xee001b7f, 0x152c},
	{0x11295, 0x1a20},
	{0x25a09104, 0x1564},
	{0x10053, 0x1548},
	{0x2010, 0x1c04},
	{0x4e150, 0x15a0},
	{0xe66177, 0x0a70},
	{0x136e1, 0x0a84},
	{0x26200780, 0x0a40},
	{0xbb, 0x0a9c},
	{0x6, 0x1b10},
	{0xf, 0x1b18},
	{0x0, 0x1b54},
	{0x600, 0x1b5c},
};

static void clt_phy_init(struct clt_pcie *c)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(clt_phy_init_tbl); i++)
		clt_phy_native_write(c, clt_phy_init_tbl[i][1],
				     clt_phy_init_tbl[i][0]);
}

static int clt_noc_exit_idle(struct clt_pcie *c)
{
	u32 mask = c->rc_id ? NOC_PCIE1_POWER_MASK : NOC_PCIE0_POWER_MASK;
	u32 bits = c->rc_id ? NOC_PCIE1_POWER : NOC_PCIE0_POWER;
	u32 val;
	int i;

	writel(mask, c->pmctrl + PMC_NOC_IDLEREQ);
	for (i = 0; i < 1000; i++) {
		val = readl(c->pmctrl + PMC_NOC_IDLE);
		if (!(val & bits))
			return 0;
		udelay(1);
	}
	dev_err(c->pci.dev, "NoC idle exit timeout, state=%#x\n", val);
	return -ETIMEDOUT;
}

/*
 * The vendor stack reaches the PCIe security services through the "platdrv"
 * secure partition over Arm FF-A, not through a raw platform SMC (the same
 * FID sent as a plain SMC is echoed back unhandled).  The mainline FF-A
 * transport refuses to initialise unless SMCCC 1.2 is advertised, while this
 * firmware is an FF-A 1.1 SPMC behind SMCCC 1.1, so drive FF-A directly.
 */
/* platform SMC: address of the ATF-reserved FF-A RX/TX shared buffer */
#define CLT_FID_FFA_SHMEM_GET		0xc3002d01

#define FFA_FID_VERSION			0x84000063
#define FFA_FID_ID_GET			0x84000069
#define FFA_FID_FEATURES		0x84000064
#define FFA_FID_RX_RELEASE		0x84000065
#define FFA_FID_RXTX_MAP		0xc4000066
#define FFA_FID_RXTX_UNMAP		0x84000067
#define FFA_FID_PARTITION_INFO_GET	0x84000068
#define FFA_FID_MSG_SEND_DIRECT_REQ	0x8400006f
#define FFA_FID_MSG_SEND_DIRECT_RESP	0x84000070
#define FFA_FID_RUN			0x8400006d
#define FFA_FID_INTERRUPT		0x84000062
#define FFA_FID_YIELD			0x8400006c
#define FFA_FID_ERROR			0x84000060
#define FFA_FID_SUCCESS			0x84000061

/* platdrv secure partition (UUID f9e75f33-a1ca4f82-98e6c802-0f20ecb4) */
#define CLT_PLATDRV_ID		0x8004

/*
 * platdrv is the FF-A secure partition with id 0x8004 on this SoC; the SPMC
 * does not fill in the UUID in its partition info, so the id is fixed here.
 * Direct requests do not need the RX/TX buffers, which keeps this path free
 * of the ATF shared-memory handshake.
 */
static void clt_ffa_smc(struct arm_smccc_1_2_regs *a,
			struct arm_smccc_1_2_regs *r)
{
	memset(r, 0, sizeof(*r));
	arm_smccc_1_2_smc(a, r);
}

static int clt_ffa_init(struct clt_pcie *c)
{
	struct arm_smccc_1_2_regs a = { }, r;

	a.a0 = FFA_FID_VERSION;
	a.a1 = 0x10001;			/* FF-A 1.1 */
	clt_ffa_smc(&a, &r);
	dev_info(c->pci.dev, "FF-A version %#lx\n", r.a0);
	if ((s32)r.a0 < 0)
		return -EOPNOTSUPP;

	memset(&a, 0, sizeof(a));
	a.a0 = FFA_FID_ID_GET;
	clt_ffa_smc(&a, &r);
	if (r.a0 != FFA_FID_SUCCESS) {
		dev_warn(c->pci.dev, "FF-A id get failed: %#lx\n", r.a0);
		return -EIO;
	}
	c->ffa_src_id = r.a2 & 0xffff;
	c->ffa_dst_id = CLT_PLATDRV_ID;
	dev_info(c->pci.dev, "FF-A src=%#x dst=%#x\n",
		 c->ffa_src_id, c->ffa_dst_id);
	return 0;
}

static int clt_ffa_call(struct clt_pcie *c, u32 fid)
{
	struct arm_smccc_1_2_regs a = { }, r;
	u32 ids = (c->ffa_src_id << 16) | c->ffa_dst_id;
	unsigned int i;

	if (!c->ffa_dst_id)
		return -ENODEV;

	a.a0 = FFA_FID_MSG_SEND_DIRECT_REQ;
	a.a1 = ids;
	a.a2 = 0;
	a.a3 = fid;
	a.a4 = c->rc_id;
	dev_info(c->pci.dev, "FFA %#x(rc%u) sending\n", fid, c->rc_id);
	clt_ffa_smc(&a, &r);
	for (i = 0; i < 100; i++) {
		if (r.a0 != FFA_FID_INTERRUPT && r.a0 != FFA_FID_YIELD)
			break;
		memset(&a, 0, sizeof(a));
		a.a0 = FFA_FID_RUN;
		a.a1 = r.a1;
		clt_ffa_smc(&a, &r);
	}

	if (r.a0 == FFA_FID_ERROR) {
		dev_err(c->pci.dev, "FFA %#x failed: %ld\n", fid, (long)r.a2);
		return -EIO;
	}
	if (r.a0 != FFA_FID_MSG_SEND_DIRECT_RESP) {
		dev_warn(c->pci.dev, "FFA %#x unexpected %#lx\n", fid, r.a0);
		return -EINVAL;
	}

	dev_info(c->pci.dev, "FFA %#x(rc%u) ok: %#lx %#lx %#lx\n",
		 fid, c->rc_id, r.a3, r.a4, r.a5);
	return 0;
}

static int clt_power_on(struct clt_pcie *c)
{
	u32 val, assert = c->rc_id ? 0x00c00000 : 0x00000300;
	u32 por_reset = c->rc_id ? RST_PCIE1_POR_N : RST_PCIE0_POR_N;

	/* 1. APB clocks for PHY and controller */
	if (c->rc_id)
		clt_clk_pcie1_on(c, CLK_PCIE1_PHY | CLK_PCIE1_SYS);
	else
		clt_clk_pcie0_on(c, CLK_PCIE0_PHY | CLK_PCIE0_SYS);

	/* 2. open the isolation cell between the controller and the NoC */
	writel(c->rc_id ? SCTRL_PCIE1_ISO : SCTRL_PCIE0_ISO,
	       c->sctrl + SCTRL_PCIE_ISO_DIS);
	udelay(10);

	/* 3. deassert controller reset + async bridge */
	writel(assert, c->crg + CRG_PERRSTDIS0);
	writel(c->rc_id ? RST_PCIE1_ASYNCBRG : RST_PCIE0_ASYNCBRG,
	       c->crg + CRG_PERRSTDIS0);

	/* 3. controller timeouts, PHY POR */
	val = clt_apb_read(c, APB_CTRL25);
	val &= ~CTRL25_TIMEOUT_MASK;
	val |= CTRL25_TIMEOUT_VAL;
	clt_apb_write(c, APB_CTRL25, val);

	val = clt_phy_read(c, PHY_CTRL1);
	val |= PHY_POR_N_RESET;
	clt_phy_write(c, PHY_CTRL1, val);

	/* 4. RC mode, PERST output, refclk mux */
	val = clt_apb_read(c, APB_CTRL22);
	val &= ~CTRL22_CLKREQ_OUT;
	clt_apb_write(c, APB_CTRL22, val);

	val = clt_apb_read(c, APB_CTRL7);
	val |= CTRL7_AUX_PWR_DET;
	clt_apb_write(c, APB_CTRL7, val);

	val = clt_apb_read(c, APB_CTRL0);
	val &= ~(PCIE_TYPE_MASK << PCIE_TYPE_SHIFT);
	val |= PCIE_TYPE_RC << PCIE_TYPE_SHIFT;
	clt_apb_write(c, APB_CTRL0, val);

	val = clt_apb_read(c, APB_CTRL12);
	val &= ~PERST_IN_N_CTRL_BIT3;
	val |= PERST_IN_N_CTRL_BIT2;
	val |= PERST_OE_EN;
	val &= ~PERST_OUT_N;
	clt_apb_write(c, APB_CTRL12, val);

	val = clt_phy_read(c, PHY_CTRL1);
	val |= PHY_RESET;
	val &= ~PHY_PIPE_LINE0_RESET;
	clt_phy_write(c, PHY_CTRL1, val);

	/* 5. FNPLL (100MHz PHY refclk) and refclk path */
	if (clt_fnpll_enable(c))
		dev_warn(c->pci.dev, "FNPLL did not lock\n");

	val = clt_phy_read(c, PHY_CTRL0);
	val &= ~PHY_REF_USE_CIO_PAD;
	clt_phy_write(c, PHY_CTRL0, val);

	/* HP/debounce gates + let HW drive phyref/io clocks */
	if (c->rc_id) {
		clt_clk_pcie1_on(c, CLK_PCIE1_HP | CLK_PCIE1_DEBOUNCE);
		writel(CLK_PCIE1_PHYREF | BIT(5), c->crg + CRG_PERDIS0);
		val = readl(c->crg + CRG_PCIECTRL1);
		val &= ~(BIT(0) | BIT(1));
		writel(val, c->crg + CRG_PCIECTRL1);
		val &= ~BIT(6);
		val &= ~BIT(9);
		val &= ~BIT(11);
		val &= ~BIT(28);
		val &= ~BIT(29);
		val |= BIT(27);
		writel(val, c->crg + CRG_PCIECTRL1);
	} else {
		clt_clk_pcie0_on(c, CLK_PCIE0_HP | CLK_PCIE0_DEBOUNCE);
		writel(CLK_PCIE0_PHYREF | BIT(7), c->crg + CRG_PERDIS1);
		val = readl(c->crg + CRG_PCIECTRL0);
		val &= ~(BIT(0) | BIT(1));	/* io/phyref HW bypass */
		val &= ~BIT(11);		/* oe HW bypass, as in 5.10 */
		writel(val, c->crg + CRG_PCIECTRL0);
	}

	/* phyio from CIO, disable SW io gate */
	val = clt_apb_read(c, APB_CTRL21);
	val &= ~(0x3 << 17);
	clt_apb_write(c, APB_CTRL21, val);

	if (c->rc_id) {
		clt_clk_pcie1_on(c, CLK_PCIE1_ACLK | CLK_PCIE1_TBU);
		clt_clk_pcie0_on(c, CLK_PCIE1_TCU);
		writel(BIT(8) | SCTRL_PCIE1_AUX_EN, c->sctrl + SCTRL_PCIE_AUX);
	} else {
		clt_clk_pcie0_on(c, CLK_PCIE0_ACLK | CLK_PCIE0_TCU |
				    CLK_PCIE0_TBU);
		writel(BIT(8) | SCTRL_PCIE0_AUX_EN, c->sctrl + SCTRL_PCIE_AUX);
	}

	/* 6. PHY init */
	clt_phy_init(c);

	/* 7. deassert controller power-on reset */
	writel(por_reset, c->crg + CRG_PERRSTDIS0);

	/* 8. release PERST: pulse the board GPIO, then the internal one */
	if (c->gpio) {
		usleep_range(42000, 46000);
		clt_perst_set(c, 0);
		mdelay(100);
		clt_perst_set(c, 1);
	}

	val = clt_apb_read(c, APB_CTRL12);
	val |= PERST_IN_N_CTRL_BIT2;
	val &= ~PERST_IN_N_CTRL_BIT3;
	clt_apb_write(c, APB_CTRL12, val);

	val = clt_apb_read(c, APB_CTRL23);
	val |= CTRL23_SP_MEM_TRA;
	clt_apb_write(c, APB_CTRL23, val);
	val = clt_apb_read(c, APB_CTRL20);
	val |= CTRL20_TP_MEM_TRA;
	clt_apb_write(c, APB_CTRL20, val);

	usleep_range(150000, 200000);

	/* 9. TCU/TBU out of reset */
	writel(c->rc_id ? (RST_PCIE1_TCU | RST_PCIE1_TBU)
			: (RST_PCIE0_TCU | RST_PCIE0_TBU),
	       c->crg + CRG_PERRSTDIS0);

	/* 10. wait for PIPE clock and PHY */
	for (val = 0; val < 100; val++) {
		if (!(clt_phy_read(c, PHY_STATE0) & PHY_PIPE_CLK_STABLE))
			break;
		mdelay(1);
	}
	if (val == 100)
		dev_warn(c->pci.dev, "PIPE clock not stable\n");

	clt_phy_ffe_cfg(c);

	if (clt_wait_phy_ready(c))
		return -ETIMEDOUT;

	val = clt_phy_read(c, PHY_CTRL39);
	val |= PHY_RX_TERMINATION;
	clt_phy_write(c, PHY_CTRL39, val);

	/* 11. leave NoC power idle */
	clt_noc_exit_idle(c);



	val = clt_apb_read(c, APB_CTRL30);
	if (c->rc_id)
		val |= CTRL30_DBI_EP_SPLT;
	else
		val &= ~CTRL30_DBI_EP_SPLT;
	clt_apb_write(c, APB_CTRL30, val);

	/*
	 * Route the CPU's DBI accesses through the ELBI slave.  The vendor
	 * driver does this around every DBI read/write; on this SoC the
	 * PCIe1 DBI window answers with a NoC external abort (SError) while
	 * the bit is clear.
	 */
	val = clt_apb_read(c, APB_CTRL1);
	val |= CTRL1_ELBI_DBI_EN;
	clt_apb_write(c, APB_CTRL1, val);
	udelay(10);

	/*
	 * Ask the secure world to release the RC's memory region and route
	 * its DMA around the TBU before the first DBI access.
	 */
	if (c->rc_id) {
		clt_ffa_call(c, FID_PCIE_GET_VREF_SEL);
		clt_ffa_call(c, FID_PCIE_SET_MEM_UNSEC);
		clt_ffa_call(c, FID_PCIE_SET_TBU_BYPASS);
	}

	dev_info(c->pci.dev,
		 "state: ctrl0=%#x ctrl1=%#x ctrl30=%#x pciectrl=%#x pm_idle=%#x\n",
		 clt_apb_read(c, APB_CTRL0), clt_apb_read(c, APB_CTRL1),
		 clt_apb_read(c, APB_CTRL30),
		 readl(c->crg + (c->rc_id ? CRG_PCIECTRL1 : CRG_PCIECTRL0)),
		 readl(c->pmctrl + PMC_NOC_IDLE));

	return 0;
}

/*
 * Device DMA needs inbound iATU windows: without one the RC drops every
 * device -> system-memory request, so rings and page tables that live in
 * GART memory can never be fetched.  Declaring dma-ranges in the DT is not
 * an option here -- the OF layer then hands the DMA API a window it cannot
 * express as a 4 GiB iATU region, and the resulting addresses are bogus --
 * so program the identity windows directly.  Each one stays inside a single
 * 4 GiB block, which is what the controller supports.
 */
static const struct {
	u64 start;
	u64 size;
} clt_dma_windows[] = {
	{ 0x0000000000ULL, 0x100000000ULL },	/* 0 - 4 GiB (low DRAM) */
	{ 0x1000000000ULL, 0x100000000ULL },	/* 4 - 8 GiB */
	{ 0x2000000000ULL, 0x100000000ULL },	/* 8 - 12 GiB */
	{ 0x3000000000ULL, 0x100000000ULL },	/* 12 - 16 GiB */
	{ 0x8000000000ULL, 0x020000000ULL },	/* 32 - 32.5 GiB */
};

static void clt_prog_dma_windows(struct dw_pcie *pci)
{
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(clt_dma_windows); i++) {
		ret = dw_pcie_prog_inbound_atu(pci, i, PCIE_ATU_TYPE_MEM,
					       clt_dma_windows[i].start,
					       clt_dma_windows[i].start,
					       clt_dma_windows[i].size);
		if (ret)
			dev_warn(pci->dev,
				 "inbound window %d (%#llx+%#llx) failed: %d\n",
				 i, (unsigned long long)clt_dma_windows[i].start,
				 (unsigned long long)clt_dma_windows[i].size, ret);
	}
}

static int clt_start_link(struct dw_pcie *pci)
{
	struct clt_pcie *c = container_of(pci, struct clt_pcie, pci);
	u32 val = clt_apb_read(c, APB_CTRL7);

	clt_prog_dma_windows(pci);

	val |= PCIE_LTSSM_ENABLE_BIT;
	clt_apb_write(c, APB_CTRL7, val);
	return 0;
}

static int clt_msi_init(struct dw_pcie_rp *pp)
{
	/* No wired MSI controller on this board: legacy INTx only for now. */
	return 0;
}

static const struct dw_pcie_ops clt_pcie_ops = {
	.start_link = clt_start_link,
};

static const struct dw_pcie_host_ops clt_host_ops = {
	.msi_init = clt_msi_init,
};

static int clt_pcie_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct clt_pcie *c;
	struct dw_pcie *pci;
	int ret;

	c = devm_kzalloc(dev, sizeof(*c), GFP_KERNEL);
	if (!c)
		return -ENOMEM;

	pci = &c->pci;
	pci->dev = dev;
	pci->ops = &clt_pcie_ops;

	platform_set_drvdata(pdev, c);

	/* Map DBI early so the power-on sequence can sanity check it. */
	pci->dbi_base = devm_platform_ioremap_resource_byname(pdev, "dbi");
	if (IS_ERR(pci->dbi_base))
		return PTR_ERR(pci->dbi_base);
	pci->dbi_phys_addr = 0;

	c->apb = devm_platform_ioremap_resource_byname(pdev, "apb");
	if (IS_ERR(c->apb))
		return PTR_ERR(c->apb);
	c->phy = devm_platform_ioremap_resource_byname(pdev, "phy");
	if (IS_ERR(c->phy))
		return PTR_ERR(c->phy);
	/* CRG/PLL/SCTRL/PMCTRL are shared between both controllers, so map
	 * them without claiming the resources (the second RC would otherwise
	 * fail with -EBUSY).
	 */
	{
		struct resource *r;

		r = platform_get_resource_byname(pdev, IORESOURCE_MEM, "crg");
		if (!r)
			return -ENODEV;
		c->crg = devm_ioremap(dev, r->start, resource_size(r));
		r = platform_get_resource_byname(pdev, IORESOURCE_MEM, "pll");
		if (!r)
			return -ENODEV;
		c->pll = devm_ioremap(dev, r->start, resource_size(r));
		r = platform_get_resource_byname(pdev, IORESOURCE_MEM, "sctrl");
		if (!r)
			return -ENODEV;
		c->sctrl = devm_ioremap(dev, r->start, resource_size(r));
		r = platform_get_resource_byname(pdev, IORESOURCE_MEM, "pmctrl");
		if (!r)
			return -ENODEV;
		c->pmctrl = devm_ioremap(dev, r->start, resource_size(r));
		if (!c->crg || !c->pll || !c->sctrl || !c->pmctrl)
			return -ENOMEM;
	}

	c->gpio = devm_platform_ioremap_resource_byname(pdev, "gpio");
	if (IS_ERR(c->gpio)) {
		dev_warn(dev, "no perst GPIO window, skipping PERST\n");
		c->gpio = NULL;
	}
	c->perst_gpio = -1;
	of_property_read_u32(dev->of_node, "perst-gpio", &c->perst_gpio);

	if (of_property_read_u32(dev->of_node, "rc-id", &c->rc_id))
		c->rc_id = 0;

	if (c->rc_id) {
		ret = clt_ffa_init(c);
		if (ret)
			dev_warn(dev, "platdrv FF-A unavailable: %d\n", ret);
	}

	dev_info(dev, "PCIe%u power-on\n", c->rc_id);
	ret = clt_power_on(c);
	if (ret) {
		dev_err(dev, "power-on failed: %d\n", ret);
		return ret;
	}

	pci->pp.ops = &clt_host_ops;

	dev_info(dev, "dbi_base=%px smccc=%#x\n", pci->dbi_base,
		 arm_smccc_get_version());

	dev_info(dev, "DBI id=%#x link=%d\n", dw_pcie_read_dbi(pci, 0, 4),
		 dw_pcie_link_up(pci));

	ret = dw_pcie_host_init(&pci->pp);
	if (ret) {
		dev_err(dev, "host init failed: %d\n", ret);
		return ret;
	}

	dev_info(dev, "host ready, link=%d\n", dw_pcie_link_up(pci));
	return 0;
}

static const struct of_device_id clt_pcie_of_match[] = {
	{ .compatible = "hisilicon,charlotte-pcie" },
	{ }
};
MODULE_DEVICE_TABLE(of, clt_pcie_of_match);

static struct platform_driver clt_pcie_driver = {
	.probe = clt_pcie_probe,
	.driver = {
		.name = "charlotte-pcie",
		.of_match_table = clt_pcie_of_match,
	},
};
builtin_platform_driver(clt_pcie_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("HiSilicon Kirin 9000C PCIe host glue");
